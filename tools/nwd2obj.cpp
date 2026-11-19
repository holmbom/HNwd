// nwd2obj — skriver geometrin i en .nwd eller .nwc som Wavefront OBJ, för att
// kontrollera den i en visare.
//
//   nwd2obj <in.nwd|in.nwc> <out.obj> [--local] [--offset x y z]
//           [--max-fragments N] [--no-mtl]
//
// Förvalet är världskoordinater. --local skriver postens egna koordinater i
// stället. --offset dras från varje världspunkt innan den skrivs, för modeller
// vars koordinater är så stora att en visare tappar precision. --max-fragments
// stannar efter N fragment. Bredvid OBJ-filen skrivs en .mtl med ett material
// per utseende, och bilderna som ligger i filen som <mtl>_material_<i>.<typ>,
// utpekade med map_Kd; --no-mtl hoppar över båda.
//
// OBJ-filen har ett objekt (g) per Fragment::seq. Namnet är nodens namn följt av
// seq när trädet gick att läsa, annars seq_<n>. Eftersom linjer saknar normaler
// räknas v, vt och vn var för sig, och en triangel skrivs som v//vn, eller
// v/vt/vn när posten har UV. UV-värdena skrivs som de står, utan materialets
// skala och förskjutning.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "HNwd.h"
#include "path_utf8.h"
#include "tool_main.h"

namespace {

constexpr size_t kFlushBytes = size_t(4) << 20;   // bufferten töms till filen vid så här mycket

constexpr const char* kUsage =
    "usage: nwd2obj <in.nwd|in.nwc> <out.obj> [--local] [--offset x y z] "
    "[--max-fragments N] [--no-mtl]\n";

// ---------------------------------------------------------------------------
// Filer.

// Öppnar en fil för skrivning (binärt, så att radslut förblir \n) med en
// UTF-8-sökväg på alla plattformar. Sätter errno vid fel.
FILE* open_write_utf8(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = HNwd::detail::utf8_to_wide(path);
    if (wide.empty()) {
        errno = EINVAL;
        return nullptr;
    }
    return _wfopen(wide.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

// En utfil med en egen buffert. Texten läggs i buf och töms till filen när den
// nått några MB. Ett skrivfel kommer ihåg sig själv: allt därefter kastas, och
// failed() och finish() säger att filen inte är hel.
class Sink {
public:
    Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    ~Sink() {
        if (file_) std::fclose(file_);
    }

    bool open(const std::string& path) {
        path_ = path;
        file_ = open_write_utf8(path);
        if (!file_) {
            open_errno_ = errno;
            return false;
        }
        buf.reserve(kFlushBytes + (size_t(1) << 16));
        return true;
    }

    std::string buf;

    void flush_if_large() {
        if (buf.size() >= kFlushBytes) flush();
    }

    void flush() {
        if (file_ && !failed_ && !buf.empty()) {
            if (std::fwrite(buf.data(), 1, buf.size(), file_) != buf.size())
                failed_ = true;
            else
                written_ += buf.size();
        }
        buf.clear();
    }

    // Tömmer och stänger. Sant bara om varje skrivning och stängningen lyckades.
    bool finish() {
        flush();
        if (!file_) return false;
        if (std::ferror(file_)) failed_ = true;
        const int rc = std::fclose(file_);
        file_ = nullptr;
        if (rc != 0) failed_ = true;
        return !failed_;
    }

    // Stänger och tar bort filen, så att ingen avhuggen fil blir kvar.
    void discard() {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
        buf.clear();
        if (!path_.empty()) HNwd::detail::remove_utf8(path_);
    }

    bool failed() const { return failed_; }
    bool is_open() const { return file_ != nullptr; }
    uint64_t written() const { return written_; }
    int open_errno() const { return open_errno_; }
    const std::string& path() const { return path_; }

private:
    FILE* file_ = nullptr;
    std::string path_;
    bool failed_ = false;
    uint64_t written_ = 0;
    int open_errno_ = 0;
};

// ---------------------------------------------------------------------------
// Text.

void append_u64(std::string& out, uint64_t v) {
    char tmp[24];
    size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) out.push_back(tmp[--n]);
}

// printf-formatering av ett tal; formaten här ger aldrig mer än 40 tecken.
void append_num(std::string& out, const char* format, double v) {
    char tmp[48];
    const int n = std::snprintf(tmp, sizeof tmp, format, v);
    if (n > 0) out.append(tmp, static_cast<size_t>(std::min<int>(n, sizeof tmp - 1)));
}

// Ett namn som går att skriva efter g i en OBJ-fil: skrivbar ASCII och byten
// från 0x80 behålls, blanksteg och styrtecken blir _.
std::string clean_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        out.push_back((c > 0x20 && c != 0x7F) ? ch : '_');
    }
    return out;
}

// Filnamnet utan katalog.
std::string base_name(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

// Sökvägen till .mtl: utsökvägen med sista ändelsen utbytt mot .mtl.
std::string mtl_path_for(const std::string& obj_path) {
    const size_t slash = obj_path.find_last_of("/\\");
    const size_t dot = obj_path.find_last_of('.');
    const bool has_ext = dot != std::string::npos && (slash == std::string::npos || dot > slash) &&
                         dot > (slash == std::string::npos ? 0 : slash + 1);
    return (has_ext ? obj_path.substr(0, dot) : obj_path) + ".mtl";
}

bool parse_double(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    out = std::strtod(text.c_str(), &end);
    return end && *end == '\0' && errno != ERANGE && std::isfinite(out);
}

bool parse_count(const std::string& text, uint64_t& out) {
    if (text.empty() || text[0] < '0' || text[0] > '9') return false;
    char* end = nullptr;
    errno = 0;
    out = std::strtoull(text.c_str(), &end, 10);
    return end && *end == '\0' && errno != ERANGE;
}

// Skriver ett fel med status och bibliotekets text, och ger avslutningskoden.
int report_error(const char* what, HNwd::Status status, const HNwd::Document& doc) {
    std::fprintf(stderr, "error: %s: %s", what, HNwd::status_text(status));
    if (*doc.last_error()) std::fprintf(stderr, ": %s", doc.last_error());
    std::fputc('\n', stderr);
    return 1;
}

int report_write_error(const Sink& sink) {
    std::fprintf(stderr, "error: writing %s failed\n", sink.path().c_str());
    return 1;
}

// ---------------------------------------------------------------------------
// Materialen.

// En filändelse som går att skriva i ett filnamn: ASCII-bokstäver och siffror,
// högst åtta, med små bokstäver. Annars "bin".
std::string clean_extension(std::string_view type) {
    std::string out;
    for (const char ch : type) {
        const bool alnum = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (!alnum) return "bin";
        out.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return out.empty() || out.size() > 8 ? std::string("bin") : out;
}

// Bilderna som ligger i filen, skrivna bredvid .mtl. Namnet är .mtl-filens
// namn utan ändelse, "_material_", materialets index och bildens typ; inget av
// det kommer ur sökvägar i den lästa filen.
struct Textures {
    std::vector<std::string> names;   // per material: filnamnet bredvid .mtl, tomt om ingen bild skrevs
    std::vector<std::string> paths;   // de skrivna filerna, för att ta bort dem vid ett fel
    uint64_t bytes = 0;
};

bool write_textures(const HNwd::Document& doc, const std::string& mtl_path, Textures& out) {
    const size_t slash = mtl_path.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string() : mtl_path.substr(0, slash + 1);
    std::string stem = base_name(mtl_path);
    if (stem.size() > 4) stem.resize(stem.size() - 4);   // utan ".mtl"

    out.names.assign(doc.material_count(), std::string());
    for (size_t i = 0; i < doc.material_count(); ++i) {
        HNwd::Material m;
        if (!doc.material(i, m)) {
            std::fprintf(stderr, "error: material %zu could not be read\n", i);
            return false;
        }
        if (!m.texture_bytes || m.texture_size == 0) continue;

        const std::string name = stem + "_material_" + std::to_string(i) + "." +
                                 clean_extension(m.texture_type);
        const std::string path = dir + name;
        FILE* f = open_write_utf8(path);
        if (!f) {
            std::fprintf(stderr, "error: cannot open %s for writing: %s\n", path.c_str(),
                         std::strerror(errno));
            return false;
        }
        out.paths.push_back(path);
        const bool wrote = std::fwrite(m.texture_bytes, 1, m.texture_size, f) == m.texture_size;
        if (std::fclose(f) != 0 || !wrote) {
            std::fprintf(stderr, "error: writing %s failed\n", path.c_str());
            return false;
        }
        out.names[i] = name;
        out.bytes += m.texture_size;
    }
    return true;
}

bool write_mtl(Sink& mtl, const HNwd::Document& doc, const Textures& textures) {
    const size_t count = doc.look_count();
    std::string& b = mtl.buf;
    for (size_t i = 0; i < count; ++i) {
        HNwd::Look l;
        doc.look(i, l);
        const float all[] = {l.ambient[0],  l.ambient[1],  l.ambient[2],  l.diffuse[0],
                             l.diffuse[1],  l.diffuse[2],  l.specular[0], l.specular[1],
                             l.specular[2], l.shininess,   l.transparency};
        for (const float v : all) {
            if (!std::isfinite(v)) {
                std::fprintf(stderr, "error: look %zu has a value that is not finite\n", i);
                return false;
            }
        }
        b += "newmtl look_";
        append_u64(b, i);
        b += '\n';
        const struct {
            const char* key;
            const float* v;
        } colors[] = {{"Ka", l.ambient}, {"Kd", l.diffuse}, {"Ks", l.specular}};
        for (const auto& c : colors) {
            b += c.key;
            for (int k = 0; k < 3; ++k) {
                b += ' ';
                append_num(b, "%.6g", c.v[k]);
            }
            b += '\n';
        }
        b += "Ns ";
        append_num(b, "%.6g", static_cast<double>(l.shininess) * 1000.0);
        b += "\nd ";
        append_num(b, "%.6g", 1.0 - static_cast<double>(l.transparency));
        b += '\n';
        if (l.material != HNwd::kNone) {
            if (l.material >= textures.names.size()) {
                std::fprintf(stderr, "error: look %zu: material %u is outside the %zu materials\n",
                             i, l.material, textures.names.size());
                return false;
            }
            if (!textures.names[l.material].empty()) {
                b += "map_Kd ";
                b += textures.names[l.material];
                b += '\n';
            }
        }
        b += '\n';
        mtl.flush_if_large();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Verktygets val och mått.

struct Options {
    std::string in, out;
    bool local = false;
    bool mtl = true;
    bool has_offset = false;
    double offset[3] = {0, 0, 0};
    bool has_max = false;
    uint64_t max_fragments = 0;
};

struct Totals {
    uint64_t fragments = 0, triangles = 0, segments = 0;
    uint64_t undecoded = 0;         // fragment vars geometri inte avkodas
    uint64_t point_fragments = 0;   // fragment med punkter, som inte skrivs
    uint64_t text_fragments = 0;    // fragment med text, som inte skrivs
    uint64_t vertices = 0, normals = 0, uvs = 0, groups = 0;
    double lo[3] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity()};
    double hi[3] = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity()};
};

bool parse_args(const std::vector<std::string>& args, Options& o) {
    std::vector<std::string> positional;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--local") {
            o.local = true;
        } else if (a == "--no-mtl") {
            o.mtl = false;
        } else if (a == "--offset") {
            if (i + 3 >= args.size()) return false;
            for (int k = 0; k < 3; ++k)
                if (!parse_double(args[++i], o.offset[k])) return false;
            o.has_offset = true;
        } else if (a == "--max-fragments") {
            if (i + 1 >= args.size() || !parse_count(args[++i], o.max_fragments)) return false;
            o.has_max = true;
        } else if (a.size() > 1 && a[0] == '-' && a[1] == '-') {
            return false;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() != 2) return false;
    o.in = positional[0];
    o.out = positional[1];
    // Förskjutningen gäller världspunkter; mot postens egna koordinater betyder den inget.
    if (o.local && o.has_offset) return false;
    return !o.in.empty() && !o.out.empty();
}

} // namespace

int tool_run(const std::vector<std::string>& args) {
    const auto started = std::chrono::steady_clock::now();

    Options opt;
    if (!parse_args(args, opt)) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const std::string mtl_path = mtl_path_for(opt.out);
    if (opt.mtl && mtl_path == opt.out) {
        std::fprintf(stderr, "error: the output file would be overwritten by its own .mtl: %s\n",
                     opt.out.c_str());
        return 2;
    }

    // Läsningen.
    HNwd::Document doc;
    if (const HNwd::Status st = doc.open(opt.in.c_str()); st != HNwd::Status::Ok)
        return report_error("open", st, doc);
    if (const HNwd::Status st = doc.load_fragments(); st != HNwd::Status::Ok)
        return report_error("load_fragments", st, doc);

    // Trädet ger namn åt grupperna. Utan det heter de seq_<n>.
    if (const HNwd::Status st = doc.load_tree(); st != HNwd::Status::Ok) {
        std::fprintf(stderr, "warning: the tree was not read, groups are named seq_<n>: %s",
                     HNwd::status_text(st));
        if (*doc.last_error()) std::fprintf(stderr, ": %s", doc.last_error());
        std::fputc('\n', stderr);
    }
    const bool named = doc.has_tree();

    // Utfilerna.
    Sink obj, mtl;
    if (!obj.open(opt.out)) {
        std::fprintf(stderr, "error: cannot open %s for writing: %s\n", opt.out.c_str(),
                     std::strerror(obj.open_errno()));
        return 1;
    }
    if (opt.mtl) {
        if (!mtl.open(mtl_path)) {
            std::fprintf(stderr, "error: cannot open %s for writing: %s\n", mtl_path.c_str(),
                         std::strerror(mtl.open_errno()));
            obj.discard();
            return 1;
        }
    }
    // Ett fel efter att filerna öppnats tar bort dem, så att ingen avhuggen fil blir kvar.
    Textures textures;
    const auto fail = [&]() {
        obj.discard();
        mtl.discard();
        for (const std::string& path : textures.paths) HNwd::detail::remove_utf8(path);
        return 1;
    };

    if (opt.mtl) {
        if (!write_textures(doc, mtl_path, textures)) return fail();
        if (!write_mtl(mtl, doc, textures)) return fail();
        if (!mtl.finish()) {
            report_write_error(mtl);
            return fail();
        }
        obj.buf += "mtllib ";
        obj.buf += base_name(mtl_path);
        obj.buf += '\n';
    }

    // Geometrin, fragment för fragment.
    HNwd::Fragment f;
    const size_t total = doc.fragment_count();
    const size_t limit =
        opt.has_max ? static_cast<size_t>(std::min<uint64_t>(opt.max_fragments, total)) : total;
    const size_t look_count = doc.look_count();

    Totals t;
    HNwd::Mesh mesh;
    bool in_group = false;
    uint32_t group_seq = 0;
    uint32_t current_look = HNwd::kNone;
    std::string& b = obj.buf;

    for (size_t i = 0; i < limit; ++i) {
        doc.fragment(i, f);
        const std::string where = "fragment " + std::to_string(i);

        const HNwd::Status st = doc.fragment_mesh(i, mesh);
        // Geometri av ett slag som inte avkodas hoppas över och räknas;
        // punkter har ingen form i en OBJ-fil med ytor och räknas likaså.
        if (st == HNwd::Status::Unsupported) {
            ++t.undecoded;
            continue;
        }
        if (st != HNwd::Status::Ok) {
            report_error(where.c_str(), st, doc);
            return fail();
        }
        if (mesh.kind == HNwd::MeshKind::Text) {
            ++t.text_fragments;
            continue;
        }
        if (mesh.kind == HNwd::MeshKind::Points) {
            ++t.point_fragments;
            continue;
        }
        if (mesh.positions.size() % 3 != 0) {
            std::fprintf(stderr, "error: %s: the positions are not a multiple of 3 floats\n",
                         where.c_str());
            return fail();
        }
        const size_t nv = mesh.positions.size() / 3;
        const bool lines = mesh.kind == HNwd::MeshKind::Lines;
        const size_t per_primitive = lines ? 2 : 3;
        if (mesh.indices.size() % per_primitive != 0) {
            std::fprintf(stderr, "error: %s: the index count %zu does not divide by %zu\n",
                         where.c_str(), mesh.indices.size(), per_primitive);
            return fail();
        }
        const bool has_normals = !lines && !mesh.normals.empty();
        if (has_normals && mesh.normals.size() != mesh.positions.size()) {
            std::fprintf(stderr, "error: %s: %zu normal floats for %zu position floats\n",
                         where.c_str(), mesh.normals.size(), mesh.positions.size());
            return fail();
        }
        const bool has_uvs = !lines && !mesh.uvs.empty();
        if (has_uvs && mesh.uvs.size() != 2 * nv) {
            std::fprintf(stderr, "error: %s: %zu UV floats for %zu vertices\n", where.c_str(),
                         mesh.uvs.size(), nv);
            return fail();
        }
        for (const uint32_t index : mesh.indices) {
            if (index >= nv) {
                std::fprintf(stderr, "error: %s: index %u is outside the %zu vertices\n",
                             where.c_str(), index, nv);
                return fail();
            }
        }

        // Ett objekt per seq.
        if (!in_group || f.seq != group_seq) {
            std::string name = "seq_" + std::to_string(f.seq);
            if (named) {
                HNwd::Node node;
                if (doc.node(f.seq, node)) {
                    const std::string cleaned = clean_name(node.name);
                    if (!cleaned.empty()) name = cleaned + "_" + std::to_string(f.seq);
                }
            }
            b += "g ";
            b += name;
            b += '\n';
            in_group = true;
            group_seq = f.seq;
            ++t.groups;
        }

        // Utseendet.
        if (opt.mtl && f.look != current_look) {
            if (f.look >= look_count) {
                std::fprintf(stderr, "error: %s: look %u is outside the %zu looks\n", where.c_str(),
                             f.look, look_count);
                return fail();
            }
            b += "usemtl look_";
            append_u64(b, f.look);
            b += '\n';
            current_look = f.look;
        }

        // Vertexarna.
        for (size_t v = 0; v < nv; ++v) {
            double w[3];
            if (opt.local) {
                for (int k = 0; k < 3; ++k) w[k] = mesh.positions[3 * v + static_cast<size_t>(k)];
            } else {
                HNwd::world_point(f.pose, &mesh.positions[3 * v], w);
                for (int k = 0; k < 3; ++k) w[k] -= opt.offset[k];
            }
            if (!std::isfinite(w[0]) || !std::isfinite(w[1]) || !std::isfinite(w[2])) {
                std::fprintf(stderr, "error: %s: vertex %zu is not finite\n", where.c_str(), v);
                return fail();
            }
            b += "v";
            for (int k = 0; k < 3; ++k) {
                b += ' ';
                append_num(b, "%.15g", w[k]);
                t.lo[k] = std::min(t.lo[k], w[k]);
                t.hi[k] = std::max(t.hi[k], w[k]);
            }
            b += '\n';
            obj.flush_if_large();
        }
        if (has_normals) {
            for (size_t v = 0; v < nv; ++v) {
                float n[3];
                if (opt.local)
                    for (int k = 0; k < 3; ++k) n[k] = mesh.normals[3 * v + static_cast<size_t>(k)];
                else
                    HNwd::world_normal(f.pose, &mesh.normals[3 * v], n);
                if (!std::isfinite(n[0]) || !std::isfinite(n[1]) || !std::isfinite(n[2])) {
                    std::fprintf(stderr, "error: %s: normal %zu is not finite\n", where.c_str(), v);
                    return fail();
                }
                b += "vn";
                for (int k = 0; k < 3; ++k) {
                    b += ' ';
                    append_num(b, "%.6g", n[k]);
                }
                b += '\n';
                obj.flush_if_large();
            }
        }
        if (has_uvs) {
            for (size_t v = 0; v < nv; ++v) {
                const float u = mesh.uvs[2 * v], w = mesh.uvs[2 * v + 1];
                if (!std::isfinite(u) || !std::isfinite(w)) {
                    std::fprintf(stderr, "error: %s: UV %zu is not finite\n", where.c_str(), v);
                    return fail();
                }
                b += "vt ";
                append_num(b, "%.7g", u);
                b += ' ';
                append_num(b, "%.7g", w);
                b += '\n';
                obj.flush_if_large();
            }
        }

        // Trianglar och segment; OBJ räknar från 1 genom hela filen.
        const uint64_t v_base = t.vertices + 1;
        const uint64_t n_base = t.normals + 1;
        const uint64_t uv_base = t.uvs + 1;
        for (size_t k = 0; k < mesh.indices.size(); k += per_primitive) {
            if (lines) {
                b += "l ";
                append_u64(b, v_base + mesh.indices[k]);
                b += ' ';
                append_u64(b, v_base + mesh.indices[k + 1]);
            } else {
                b += 'f';
                for (size_t c = 0; c < 3; ++c) {
                    b += ' ';
                    append_u64(b, v_base + mesh.indices[k + c]);
                    if (has_uvs || has_normals) b += '/';
                    if (has_uvs) append_u64(b, uv_base + mesh.indices[k + c]);
                    if (has_normals) {
                        b += '/';
                        append_u64(b, n_base + mesh.indices[k + c]);
                    }
                }
            }
            b += '\n';
            obj.flush_if_large();
        }

        t.vertices += nv;
        if (has_normals) t.normals += nv;
        if (has_uvs) t.uvs += nv;
        if (lines)
            t.segments += mesh.indices.size() / 2;
        else
            t.triangles += mesh.indices.size() / 3;
        ++t.fragments;

        if (obj.failed()) {
            report_write_error(obj);
            return fail();
        }
    }

    if (!obj.finish()) {
        report_write_error(obj);
        return fail();
    }

    // Sammanfattningen.
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("fragments:  %" PRIu64 " of %zu%s\n", t.fragments, total,
                t.fragments < total ? " (stopped by --max-fragments)" : "");
    std::printf("triangles:  %" PRIu64 "\n", t.triangles);
    if (t.undecoded)
        std::printf("skipped:    %" PRIu64 " fragments with geometry that is not decoded\n",
                    t.undecoded);
    if (t.text_fragments)
        std::printf("skipped:    %" PRIu64 " fragments with text\n", t.text_fragments);
    if (t.point_fragments)
        std::printf("skipped:    %" PRIu64 " fragments with points\n", t.point_fragments);
    std::printf("segments:   %" PRIu64 "\n", t.segments);
    std::printf("vertices:   %" PRIu64 "\n", t.vertices);
    if (t.uvs) std::printf("uvs:        %" PRIu64 "\n", t.uvs);
    std::printf("groups:     %" PRIu64 " (%s)\n", t.groups,
                named ? "named from the tree" : "seq_<n>");
    std::printf("space:      %s\n", opt.local ? "local (the record's own coordinates)"
                                              : (opt.has_offset ? "world, minus --offset" : "world"));
    if (t.vertices > 0) {
        std::printf("extent:     min %.15g %.15g %.15g\n", t.lo[0], t.lo[1], t.lo[2]);
        std::printf("            max %.15g %.15g %.15g\n", t.hi[0], t.hi[1], t.hi[2]);
    } else {
        std::printf("extent:     none, no vertices were written\n");
    }
    std::printf("obj:        %" PRIu64 " byte  %s\n", obj.written(), opt.out.c_str());
    if (opt.mtl) {
        std::printf("mtl:        %" PRIu64 " byte  %s (%zu materials)\n", mtl.written(),
                    mtl_path.c_str(), look_count);
        size_t images = 0;
        for (const std::string& name : textures.names) images += name.empty() ? 0 : 1;
        if (images)
            std::printf("images:     %zu, %" PRIu64 " byte, beside the .mtl\n", images,
                        textures.bytes);
    }
    std::printf("time:       %.2f s\n", seconds);
    return 0;
}
