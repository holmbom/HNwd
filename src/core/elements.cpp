#include "elements.h"
#include "object_reader.h"
#include <charconv>

namespace HNwd::detail {

namespace {

// En läsning av ett paket: ObjectReader för fälten, och vilken felkod ett
// fel ska ge. Ett fel fastnar som i ObjectReader; det första gäller.
struct Fields {
    Fields(const uint8_t* data, size_t size, const char* what)
        : r(data, size, what), data(data), what(what) {}

    ObjectReader r;
    const uint8_t* data;          // strömmen, för byte som lämnas ut som de står
    const char* what;
    size_t max_json_bytes = kDefaultMaxMaterialJsonBytes;   // tak för ett materials JSON
    bool unsupported = false;     // det första felet är en form som inte är avkodad
    bool own = false;             // det första felet står i own_code och own_detail
    ErrorCode own_code = ErrorCode::Ok;
    std::string own_detail;

    // Ett tal som väljer form har ett värde som inte är avkodat.
    bool form(const char* rule) {
        if (r.ok()) unsupported = true;
        return r.fail(rule);
    }

    // Ett fel på en egen position (i JSON-texten), med egen kod.
    bool fail_at(size_t byte, ErrorCode code, const std::string& rule) {
        if (r.ok() && !own) {
            own = true;
            own_code = code;
            own_detail = std::string(what) + " at byte " + std::to_string(byte) + ": " + rule;
        }
        return false;
    }

    bool marker(int32_t expected, const char* rule) {
        int32_t v;
        if (!r.i32(v)) return false;
        if (v != expected) return r.fail(rule);
        return true;
    }

    // Som marker(), men ett annat värde är en form som inte är avkodad.
    bool form_marker(int32_t expected, const char* rule) {
        int32_t v;
        if (!r.i32(v)) return false;
        if (v != expected) return form(rule);
        return true;
    }

    bool f64s(double* out, int n) {
        for (int i = 0; i < n; ++i)
            if (!r.f64(out[i])) return false;
        return true;
    }

    // 16 byte som fyra int32, i den ordning de ligger.
    bool guid(Guid16& out) {
        for (int k = 0; k < 4; ++k) {
            int32_t v;
            if (!r.i32(v)) return false;
            const uint32_t u = static_cast<uint32_t>(v);
            for (int b = 0; b < 4; ++b)
                out[static_cast<size_t>(4 * k + b)] = static_cast<uint8_t>(u >> (8 * b));
        }
        return true;
    }

    // `grammar` är koden för ett brott mot grammatiken: CorruptPackage för
    // ett paket, CorruptObject för en objektström.
    ErrorCode finish(ErrorCode grammar, std::string* detail) {
        if (own) {
            if (detail) *detail = own_detail;
            return own_code;
        }
        if (r.ok()) {
            if (detail) detail->clear();
            return ErrorCode::Ok;
        }
        if (detail) *detail = r.detail();
        if (unsupported) return ErrorCode::Unsupported;
        return r.code() == ErrorCode::CorruptObject ? grammar : r.code();
    }
};

// ---------------------------------------------------------------------------
// JSON i LcOpXRefTable. Bara det som behövs: ett objekt på toppnivån med
// kända nycklar, strängar med escape, tal, och en tom lista.

struct Json {
    explicit Json(const std::string& text) : s(text) {}

    const std::string& s;
    size_t i = 0;
    std::string rule;
    bool unsupported = false;

    bool fail(const char* why) {
        rule = why;
        return false;
    }
    bool unsupported_form(const std::string& why) {
        rule = why;
        unsupported = true;
        return false;
    }

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    bool hex4(uint32_t& v) {
        if (s.size() - i < 4) return fail("\\u escape is cut short");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            uint32_t d;
            if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
            else return fail("\\u escape has a character that is not hex");
            v = v * 16 + d;
        }
        return true;
    }

    static void utf8(uint32_t cp, std::string& out) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // En sträng, med markören på '"'.
    bool string(std::string& out) {
        out.clear();
        if (i >= s.size() || s[i] != '"') return fail("expected a string");
        ++i;
        while (true) {
            if (i >= s.size()) return fail("string is not closed");
            const unsigned char c = static_cast<unsigned char>(s[i++]);
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in a string");
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (i >= s.size()) return fail("escape is cut short");
            const char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone low surrogate");
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    if (s.size() - i < 2 || s[i] != '\\' || s[i + 1] != 'u')
                        return fail("high surrogate without a low one");
                    i += 2;
                    if (!hex4(lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF) return fail("high surrogate without a low one");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(cp, out);
                break;
            }
            default:
                return fail("unknown escape");
            }
        }
    }

    // Ett tal enligt JSON: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
    bool number(double& v) {
        const size_t begin = i;
        auto digits = [&] {
            const size_t from = i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            return i > from;
        };
        if (i < s.size() && s[i] == '-') ++i;
        if (i < s.size() && s[i] == '0') ++i;
        else if (!digits()) return fail("expected a number");
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (!digits()) return fail("number has no digits after the point");
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (!digits()) return fail("number has no digits in the exponent");
        }
        const auto r = std::from_chars(s.data() + begin, s.data() + i, v);
        if (r.ec != std::errc() || r.ptr != s.data() + i) return fail("number is out of range");
        return true;
    }

    bool parse(XRefTable& out) {
        enum : unsigned { kType = 1, kVersion = 2, kSaved = 4, kTable = 8, kAll = 15 };
        unsigned seen = 0;
        ws();
        if (i >= s.size() || s[i] != '{') return fail("the text is not an object");
        ++i;
        ws();
        if (i < s.size() && s[i] == '}') return unsupported_form("the object is empty");
        while (true) {
            ws();
            std::string key;
            if (!string(key)) return false;
            ws();
            if (i >= s.size() || s[i] != ':') return fail("expected ':' after a key");
            ++i;
            ws();
            unsigned bit;
            if (key == "Type") bit = kType;
            else if (key == "Version") bit = kVersion;
            else if (key == "SavedFilename") bit = kSaved;
            else if (key == "Table") bit = kTable;
            else return unsupported_form("unknown key \"" + key + "\"");
            if (seen & bit) return fail("a key appears twice");
            seen |= bit;

            if (bit == kType) {
                std::string type;
                if (!string(type)) return fail("\"Type\" is not a string");
                if (type != "XRefTable") return unsupported_form("\"Type\" is not \"XRefTable\"");
            } else if (bit == kVersion) {
                double v;
                if (!number(v)) return fail("\"Version\" is not a number");
                if (v != 1) return unsupported_form("\"Version\" is not 1");
                out.version = 1;
            } else if (bit == kSaved) {
                if (!string(out.saved_filename)) return fail("\"SavedFilename\" is not a string");
            } else {
                if (i >= s.size() || s[i] != '[') return fail("\"Table\" is not a list");
                ++i;
                ws();
                if (i >= s.size()) return fail("\"Table\" is not closed");
                if (s[i] != ']') return unsupported_form("\"Table\" has entries; their form is not decoded");
                ++i;
            }

            ws();
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == '}') {
                ++i;
                break;
            }
            return fail("expected ',' or '}' in the object");
        }
        ws();
        if (i != s.size()) return fail("text follows the object");
        if (seen != kAll) return unsupported_form("a key is missing");
        return true;
    }
};

// Snittplanen sist i en vy, 680 byte. Det som står före slutar alltid på
// jämn 8 byte, så det första planets f64 kommer efter PAD8.
bool read_view_clip(Fields& f, ViewClip& out) {
    ObjectReader& r = f.r;
    for (int32_t& v : out.head)
        if (!r.i32(v)) return false;
    if (!f.f64s(out.head_values, 3)) return false;
    if (!f.form_marker(6, "view clip plane count is not 6")) return false;
    for (ClipPlane& p : out.planes) {
        if (!r.i32(p.enabled)) return false;
        if (p.enabled != 0 && p.enabled != 1) return f.form("view clip plane switch is neither 0 nor 1");
        if (!r.i32(p.mode) || !f.f64s(p.point, 3) || !f.f64s(p.normal, 3) || !f.f64s(p.tangent, 3))
            return false;
    }
    return r.i32(out.flags_a[0]) && r.i32(out.flags_a[1]) && f.f64s(out.values_a, 6) &&
           r.i32(out.flags_b[0]) && r.i32(out.flags_b[1]) && f.f64s(out.values_b, 6) &&
           f.f64s(out.rotation, 4);
}

// Vyn från form till slutet av snittplanen; samma i alla paket som har en.
// Med `keep_rest` falsk lämnas `rest` tom.
bool read_view(Fields& f, ViewElement& out, bool keep_rest = true) {
    ObjectReader& r = f.r;
    out = ViewElement{};
    if (!r.i32(out.form)) return false;
    const int32_t form = out.form;
    if (form != 9 && form != 843 && form != 895 && form != 1919)
        return f.form("view form is not 9, 843, 895 or 1919");
    if (!r.i32(out.projection)) return false;
    if (out.projection != 0 && out.projection != 1)
        return f.form("view projection is neither 0 nor 1");
    if (!f.f64s(out.position, 3) || !f.f64s(out.rotation, 4) || !r.f64(out.u) ||
        !r.f64(out.angle) || !r.f64(out.near_depth) || !r.f64(out.far_depth) ||
        !f.f64s(out.zero, 2))
        return false;
    if (form != 9) {
        if (!f.f64s(out.avatar_values, 4) || !r.str(out.avatar) ||
            !f.f64s(out.after_avatar, 4) || !r.i32(out.avatar_flag) || !f.f64s(out.more, 4))
            return false;
    }
    if (form == 9 || form == 843) {
        if (!r.f64(out.distance)) return false;
    } else if (!f.f64s(out.extra, 6)) {
        return false;
    }
    out.tail_ints.resize(form == 9 ? 2 : 6);
    for (size_t k = 0; k < out.tail_ints.size(); ++k) {
        if (form == 1919 && k == 4 && !r.f64(out.ortho_extra)) return false;
        if (!r.i32(out.tail_ints[k])) return false;
    }
    const size_t clip_at = r.pos();
    if (!read_view_clip(f, out.clip)) return false;
    if (keep_rest) out.rest.assign(f.data + clip_at, f.data + r.pos());
    return true;
}

// [str namn] [i32 0] [GUID] [i32 0]: postens huvud efter slaget.
bool read_item_head(Fields& f, std::string& name, Guid16& guid) {
    return f.r.str(name) && f.marker(0, "marker before the item GUID is not 0") && f.guid(guid) &&
           f.marker(0, "marker after the item GUID is not 0");
}

// En pekare till ett material av klass 185, ny, bakåt eller null. `index`
// blir platsen i `materials`, kNoMaterial för null.
bool read_material_ptr(Fields& f, std::vector<MaterialData>& materials, uint32_t& index) {
    ObjectReader& r = f.r;
    ObjectReader::Ptr m;
    if (!r.ptr(m, {185}, true, true)) return false;
    if (m.kind == ObjectReader::Ptr::New) {
        MaterialData mat;
        if (!read_class185(r, mat, f.max_json_bytes)) return false;
        m.value = static_cast<uint32_t>(materials.size());
        if (!r.finish(m.id, m.value)) return false;
        materials.push_back(std::move(mat));
    }
    index = m.kind == ObjectReader::Ptr::Null ? kNoMaterial : m.value;
    return true;
}

// En markering i en sparad vy.
bool read_redline(Fields& f, Redline& out) {
    ObjectReader& r = f.r;
    if (!r.i32(out.type)) return false;
    if (out.type == 1) {
        if (!f.f64s(out.points, 4)) return false;
    } else if (out.type == 4) {
        if (!r.str(out.text) || !f.f64s(out.points, 2)) return false;
    } else {
        return f.form("redline type is neither 1 nor 4");
    }
    return r.i32(out.width) && f.f64s(out.colour, 3) && r.i32(out.pattern);
}

// Ett objekt av klass 89 i en sparad vy, kroppen efter pekaren.
bool read_view_state(Fields& f, ViewState& out) {
    ObjectReader& r = f.r;
    if (!r.str(out.element)) return false;
    if (out.element == "LcOpShadOverridesElement") {
        while (true) {
            int32_t more;
            if (!r.i32(more)) return false;
            if (more == 0) return true;
            if (more != 1) return r.fail("override list marker is neither 0 nor 1");
            // Varje post tar 8 byte; listan kan inte vara längre än strömmen.
            if (out.indices.size() >= r.size() / 8) return r.fail("override list is longer than the stream");
            int32_t index;
            if (!r.i32(index)) return false;
            out.indices.push_back(index);
        }
    }
    if (out.element == "LcOpShadFragMaterialElement") {
        return f.form_marker(0, "first FragMaterial number is not 0; a non-empty form is not decoded") &&
               f.form_marker(0, "second FragMaterial number is not 0; a non-empty form is not decoded");
    }
    return f.form("class-89 element is not decoded");
}

// En post i LcOpSavedViewsElement, med sina poster om den är en mapp.
bool read_saved_item(Fields& f, SavedViews& out, uint32_t parent, int depth) {
    ObjectReader& r = f.r;
    if (depth > kMaxNesting) {
        f.fail_at(r.pos(), ErrorCode::LimitExceeded,
                  "folders nest deeper than " + std::to_string(kMaxNesting));
        return false;
    }
    SavedViews::Item head;
    head.parent = parent;
    if (!r.i32(head.kind)) return false;
    if (head.kind != 0 && head.kind != 4) return f.form("saved item kind is neither 0 nor 4");
    if (!read_item_head(f, head.name, head.guid)) return false;
    const uint32_t self = static_cast<uint32_t>(out.items.size());
    if (head.kind == 4) {
        int32_t c;
        // En post tar minst sitt huvud: 4 + 4 + 4 + 16 + 4 byte.
        if (!r.count(32, c)) return false;
        head.child_count = static_cast<uint32_t>(c);
        out.items.push_back(std::move(head));
        for (int32_t k = 0; k < c; ++k)
            if (!read_saved_item(f, out, self, depth + 1)) return false;
        return true;
    }
    head.view = static_cast<uint32_t>(out.views.size());
    out.items.push_back(std::move(head));
    SavedViews::View v;
    if (!read_view(f, v.view, false)) return false;
    int32_t n;
    // En markering tar minst typ, två f64, bredd, färg och mönster. Antalet
    // prövas mot strömmen; listan växer med markeringarna som läses.
    if (!r.count(48, n)) return false;
    v.redline_begin = static_cast<uint32_t>(out.redlines.size());
    v.redline_count = static_cast<uint32_t>(n);
    for (int32_t k = 0; k < n; ++k) {
        Redline rl;
        if (!read_redline(f, rl)) return false;
        out.redlines.push_back(std::move(rl));
    }
    // En plats tar minst en bakåtpekare: 8 byte.
    if (!r.count(8, n)) return false;
    v.state_begin = static_cast<uint32_t>(out.state_refs.size());
    v.state_count = static_cast<uint32_t>(n);
    for (int32_t k = 0; k < n; ++k) {
        ObjectReader::Ptr p;
        if (!r.ptr(p, {89}, false, true)) return false;
        if (p.kind == ObjectReader::Ptr::New) {
            ViewState s;
            if (!read_view_state(f, s)) return false;
            p.value = static_cast<uint32_t>(out.states.size());
            if (!r.finish(p.id, p.value)) return false;
            out.states.push_back(std::move(s));
        }
        out.state_refs.push_back(p.value);
    }
    out.views.push_back(std::move(v));
    return true;
}

// Klass 27, kroppen efter pekaren.
bool read_light_body(Fields& f, LightsElement& out, LightBody& body) {
    ObjectReader& r = f.r;
    if (!r.i32(body.version)) return false;
    if (body.version != 1) return r.fail("class-27 version is not 1");
    if (!read_material_ptr(f, out.materials, body.material)) return false;
    if (!f.marker(0, "class-27 marker after the material is not 0")) return false;
    return f.f64s(body.values, 12);
}

} // namespace

ErrorCode parse_empty_pair(const uint8_t* data, size_t size, const char* what,
                           std::string* detail) {
    Fields f(data, size, what);
    (void)(f.form_marker(0, "first number is not 0; a non-empty form is not decoded") &&
           f.form_marker(0, "second number is not 0; a non-empty form is not decoded") &&
           f.r.end());
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_stats(const uint8_t* data, size_t size, std::string& text, bool& non_utf8,
                      std::string* detail) {
    text.clear();
    Fields f(data, size, "LcOpNwdStats");
    (void)(f.r.str(text) && f.r.end());
    non_utf8 = f.r.non_utf8_strings() != 0;
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_scene_props(const uint8_t* data, size_t size, SceneProps& out,
                            std::string* detail) {
    out = SceneProps{};
    Fields f(data, size, "LcOpOdyScenePropsChunk");
    (void)(f.r.str(out.path, &out.path_null) && f.guid(out.guid) &&
           f.form_marker(0, "first number after the GUID is not 0") &&
           f.form_marker(0, "second number after the GUID is not 0") && f.r.end());
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_file_info(const uint8_t* data, size_t size, FileInfo& out,
                          std::string* detail) {
    out = FileInfo{};
    Fields f(data, size, "LcOpOdyFileInfoChunk");
    ObjectReader& r = f.r;
    bool ok = [&] {
        std::string first;
        bool null = false;
        if (!r.str(first, &null)) return false;
        if (!null) return f.form("first string is not null");
        if (!f.form_marker(0, "marker before the file GUID is not 0") || !f.guid(out.file_guid) ||
            !f.form_marker(0, "marker after the file GUID is not 0") || !r.str(out.tag) ||
            !r.str(out.name) || !f.form_marker(0, "marker before the scene GUID is not 0") ||
            !f.guid(out.scene_guid) || !f.form_marker(0, "marker after the scene GUID is not 0") ||
            !r.str(out.prefix))
            return false;
        const int32_t expected[4] = {0, 0, -1, -1};
        for (const int32_t e : expected)
            if (!f.form_marker(e, "markers after the prefix are not 0, 0, -1, -1")) return false;
        return r.str(out.prefix_third, &out.prefix_third_null) && f.guid(out.scene_guid_again) && r.str(out.prefix_again) &&
               f.form_marker(0, "marker before the last two GUIDs is not 0") && f.guid(out.guid_c) &&
               f.guid(out.guid_d) && r.end();
    }();
    (void)ok;
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_xref_table(const uint8_t* data, size_t size, XRefTable& out,
                           std::string* detail) {
    out = XRefTable{};
    Fields f(data, size, "LcOpXRefTable");
    if (f.r.str(out.json) && f.r.end()) {
        Json j(out.json);
        if (!j.parse(out)) {
            // Texten börjar efter längden, 4 byte in.
            f.fail_at(4 + j.i, j.unsupported ? ErrorCode::Unsupported : ErrorCode::CorruptPackage,
                      "json at character " + std::to_string(j.i) + ": " + j.rule);
        }
    }
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_tool(const uint8_t* data, size_t size, int32_t& out, std::string* detail) {
    out = 0;
    Fields f(data, size, "LcOpToolElement");
    (void)(f.r.i32(out) && f.r.end());
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_next_comment_id(const uint8_t* data, size_t size, NextCommentId& out,
                                std::string* detail) {
    out = NextCommentId{};
    Fields f(data, size, "LcOpNextCommentIdElement");
    (void)(f.r.i32(out.values[0]) && f.form_marker(0, "number after the first value is not 0") &&
           f.r.i32(out.values[1]) && f.form_marker(0, "number after the second value is not 0") &&
           f.r.end());
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_view_element(const uint8_t* data, size_t size, ViewElement& out,
                             std::string* detail, const char* what) {
    out = ViewElement{};
    Fields f(data, size, what);
    ObjectReader& r = f.r;
    if (read_view(f, out)) {
        // Mer efter snittplanen än utfyllnaden är en form som inte är avkodad.
        if (r.size() - r.pos() >= 8) f.form("the view goes on after the clip planes");
        else r.end();
    }
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_saved_views(const uint8_t* data, size_t size, SavedViews& out,
                            std::string* detail) {
    out = SavedViews{};
    Fields f(data, size, "LcOpSavedViewsElement");
    ObjectReader& r = f.r;
    int32_t n;
    bool ok = r.count(32, n);
    for (int32_t k = 0; ok && k < n; ++k) ok = read_saved_item(f, out, UINT32_MAX, 0);
    (void)(ok && r.end());
    return f.finish(ErrorCode::CorruptObject, detail);
}

static_assert(sizeof(ShadOverride) == 8 && sizeof(TextureSpace) == 136);

ErrorCode parse_shad_overrides(const uint8_t* data, size_t size,
                               std::vector<ShadOverride>& out, std::string* detail) {
    out.clear();
    Fields f(data, size, "LcOpShadOverridesElement");
    int32_t n;
    // En post är 8 byte i strömmen och i minnet, så listan tar inte mer än
    // strömmen har kvar.
    bool ok = f.r.count(8, n);
    if (ok) out.resize(static_cast<size_t>(n));
    for (size_t k = 0; ok && k < out.size(); ++k)
        ok = f.r.i32(out[k].index) && f.r.i32(out[k].flags);
    (void)(ok && f.r.end());
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_texture_space(const uint8_t* data, size_t size,
                              std::vector<TextureSpace>& out, std::string* detail) {
    out.clear();
    Fields f(data, size, "LcOpTextureSpaceElement");
    int32_t n = 0;
    // En post: två heltal och 16 f64, 136 byte i strömmen och i minnet, så
    // listan tar inte mer än strömmen har kvar.
    bool ok = f.form_marker(0, "first number is not 0") && f.r.count(136, n);
    if (ok) out.resize(static_cast<size_t>(n));
    for (size_t k = 0; ok && k < out.size(); ++k)
        ok = f.r.i32(out[k].index) && f.r.i32(out[k].kind) && f.f64s(out[k].values, 16);
    (void)(ok && f.r.end());
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_shared_nodes(const uint8_t* data, size_t size, SharedNodes& out,
                             std::string* detail) {
    out = SharedNodes{};
    Fields f(data, size, "LcOpNwdSharedNodes");
    ObjectReader& r = f.r;
    bool ok = [&] {
        int32_t n;
        // En post tar minst en bakåtpekare: 8 byte.
        if (!r.count(8, n)) return false;
        for (int32_t k = 0; k < n; ++k) {
            ObjectReader::Ptr p;
            if (!r.ptr(p, {102}, false, true)) return false;
            if (p.kind == ObjectReader::Ptr::New) {
                FontEntry e;
                if (!r.str(e.name) || !r.f64(e.size)) return false;
                for (int32_t& v : e.values)
                    if (!r.i32(v)) return false;
                p.value = static_cast<uint32_t>(out.fonts.size());
                if (!r.finish(p.id, p.value)) return false;
                out.fonts.push_back(std::move(e));
            }
            out.entries.push_back(p.value);
        }
        return r.end();
    }();
    (void)ok;
    return f.finish(ErrorCode::CorruptObject, detail);
}

ErrorCode parse_grid(const uint8_t* data, size_t size, GridElement& out, std::string* detail) {
    out = GridElement{};
    Fields f(data, size, "LcOpGridElement");
    ObjectReader& r = f.r;
    bool ok = [&] {
        int32_t n;
        // Ett system tar minst namnet, tolv f64 och tre heltal.
        // Antalen prövas mot strömmen; listorna växer med det som läses.
        if (!r.count(112, n)) return false;
        for (int32_t k = 0; k < n; ++k) {
            out.systems.emplace_back();
            GridSystem& s = out.systems.back();
            if (!r.str(s.name) || !f.f64s(s.origin, 3)) return false;
            for (auto& a : s.axes)
                if (!f.f64s(a, 3)) return false;
            int32_t nl;
            // En linje: etiketten, sex f64 och tre heltal.
            if (!r.count(64, nl)) return false;
            for (int32_t i = 0; i < nl; ++i) {
                GridLine l;
                if (!r.str(l.label) || !f.f64s(l.values, 2)) return false;
                for (int32_t& v : l.ints)
                    if (!r.i32(v)) return false;
                if (!f.f64s(l.ends, 4)) return false;
                s.lines.push_back(std::move(l));
            }
            int32_t nv;
            // En nivå: namnet och en f64.
            if (!r.count(12, nv)) return false;
            for (int32_t i = 0; i < nv; ++i) {
                GridLevel v;
                if (!r.str(v.name) || !r.f64(v.elevation)) return false;
                s.levels.push_back(std::move(v));
            }
            if (!r.i32(s.level)) return false;
            if (s.level < -1 || s.level >= nv)
                return f.form("level index is outside the list of levels");
        }
        return r.i32(out.tail[0]) && r.i32(out.tail[1]) && r.end();
    }();
    (void)ok;
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_re_materials(const uint8_t* data, size_t size, ReMaterials& out,
                             std::string* detail, size_t max_json_bytes) {
    out = ReMaterials{};
    Fields f(data, size, "LcReMaterialElement");
    f.max_json_bytes = max_json_bytes;
    ObjectReader& r = f.r;
    bool ok = [&] {
        int32_t n;
        // En post tar minst huvudet och en null-pekare: 36 byte.
        // Antalet prövas mot strömmen; listan växer med posterna som läses.
        if (!r.count(36, n)) return false;
        for (int32_t k = 0; k < n; ++k) {
            ReMaterials::Entry e;
            int32_t kind;
            if (!r.i32(kind)) return false;
            if (kind != 80) return f.form("material kind is not 80");
            if (!read_item_head(f, e.name, e.guid)) return false;
            if (!read_material_ptr(f, out.materials, e.material)) return false;
            out.entries.push_back(std::move(e));
        }
        return r.end();
    }();
    (void)ok;
    return f.finish(ErrorCode::CorruptObject, detail);
}

namespace {

// Fasta tal i följd: varje ska ha sitt värde, annars en form som inte är avkodad.
bool form_markers(Fields& f, std::initializer_list<int32_t> expected, const char* rule) {
    for (const int32_t e : expected)
        if (!f.form_marker(e, rule)) return false;
    return true;
}

// Ett test (slag 50), efter slaget.
bool read_clash_test(Fields& f, ClashTest& t) {
    return read_item_head(f, t.name, t.guid) &&
           form_markers(f, {0, -1, 0, 0, -1, 0, 0, 1, 1},
                        "clash test numbers after the GUID are not 0, -1, 0, 0, -1, 0, 0, 1, 1") &&
           f.f64s(t.values, 2) && f.f64s(t.zero, 3) &&
           form_markers(f, {1, 0}, "clash test does not end with 1, 0");
}

// En modell, i sin helhet.
bool read_clash_model(Fields& f, ClashModel& m) {
    ObjectReader& r = f.r;
    return r.str(m.file) && r.str(m.path) && r.str(m.prefix) && f.guid(m.source_guid) &&
           r.str(m.name) && r.str(m.loader) &&
           f.form_marker(0, "clash model number after the loader is not 0") && r.i32(m.mode) &&
           f.f64s(m.matrix, 9) && f.f64s(m.offset, 3) && r.i32(m.unit_code) &&
           f.form_marker(1, "clash model number after the unit code is not 1") && [&] {
               double zero;
               if (!r.f64(zero)) return false;
               if (zero != 0) return f.form("clash model value before the up vector is not 0");
               return true;
           }() &&
           f.f64s(m.up, 3) && f.f64s(m.north, 3) &&
           form_markers(f, {0, -1, -1, -1, 0, 0, 1},
                        "clash model numbers after the north vector are not 0, -1, -1, -1, 0, 0, 1") &&
           r.str(m.file_again) && r.str(m.path_again) && r.i64(m.time) && r.i64(m.size) && [&] {
               double zero;
               if (!r.f64(zero)) return false;
               if (zero != 0) return f.form("clash model value after the size is not 0");
               return true;
           }() &&
           f.f64s(m.c, 3) && f.guid(m.guid);
}

} // namespace

ErrorCode parse_clash(const uint8_t* data, size_t size, ClashElement& out, std::string* detail) {
    out = ClashElement{};
    Fields f(data, size, "LcOpClashElement");
    ObjectReader& r = f.r;
    bool ok = [&] {
        int32_t n;
        // En rot tar minst huvudet och ett antal: 36 byte.
        // Antalen prövas mot strömmen; listorna växer med det som läses.
        if (!r.count(36, n)) return false;
        for (int32_t k = 0; k < n; ++k) {
            out.roots.emplace_back();
            ClashRoot& root = out.roots.back();
            if (!r.i32(root.kind)) return false;
            if (root.kind != 53 && root.kind != 54 && root.kind != 55)
                return f.form("clash root kind is not 53, 54 or 55");
            if (!read_item_head(f, root.name, root.guid)) return false;
            if (root.kind != 53) {
                if (!f.form_marker(0, "clash root of kind 54 or 55 has entries; their form is not decoded"))
                    return false;
                continue;
            }
            int32_t c;
            // Ett test tar minst huvudet, elva heltal och fem f64: 116 byte.
            if (!r.count(116, c)) return false;
            root.test_begin = static_cast<uint32_t>(out.tests.size());
            root.test_count = static_cast<uint32_t>(c);
            for (int32_t j = 0; j < c; ++j) {
                if (!f.form_marker(50, "clash test kind is not 50")) return false;
                ClashTest t;
                if (!read_clash_test(f, t)) return false;
                out.tests.push_back(std::move(t));
            }
        }
        // En regel: en tom sträng och ett tal.
        if (!r.count(8, n)) return false;
        for (int32_t k = 0; k < n; ++k) {
            std::string rule;
            if (!r.str(rule) || !f.form_marker(1, "number after a clash rule is not 1")) return false;
            out.rules.push_back(std::move(rule));
        }
        // En modell tar minst sju tomma strängar, två GUID, 23 f64, två i64
        // och elva heltal: 304 byte.
        if (!r.count(304, n)) return false;
        for (int32_t k = 0; k < n; ++k) {
            ClashModel m;
            if (!read_clash_model(f, m)) return false;
            out.models.push_back(std::move(m));
        }
        return r.end();
    }();
    (void)ok;
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_headlight(const uint8_t* data, size_t size, HeadlightElement& out,
                          std::string* detail) {
    out = HeadlightElement{};
    Fields f(data, size, "LcOpHeadlightElement");
    (void)(f.f64s(out.values, 2) && f.r.end());
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_speed(const uint8_t* data, size_t size, double& out, std::string* detail) {
    out = 0;
    Fields f(data, size, "LcOpSpeedElement");
    (void)(f.r.f64(out) && f.r.end());
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_culling(const uint8_t* data, size_t size, CullingElement& out,
                        std::string* detail) {
    out = CullingElement{};
    Fields f(data, size, "LcOpCullingElement");
    (void)(f.r.f64(out.first) && f.f64s(out.values, 4) && f.r.i32(out.flag) && f.r.end());
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_background(const uint8_t* data, size_t size, BackgroundElement& out,
                           std::string* detail, size_t max_json_bytes) {
    out = BackgroundElement{};
    Fields f(data, size, "LcOpBackgroundElement");
    f.max_json_bytes = max_json_bytes;
    bool ok = f.r.i32(out.mode);
    for (int k = 0; ok && k < 7; ++k) ok = f.f64s(out.colours[k], 3);
    ok = ok && f.form_marker(0, "number before the sky pointer is not 0");
    if (ok) {
        std::vector<MaterialData> sky;
        uint32_t index = kNoMaterial;
        ok = read_material_ptr(f, sky, index);
        if (ok && index != kNoMaterial) {
            out.has_sky = true;
            out.sky = std::move(sky[index]);
        }
    }
    (void)(ok && f.r.end());
    return f.finish(ErrorCode::CorruptPackage, detail);
}

ErrorCode parse_lights(const uint8_t* data, size_t size, LightsElement& out,
                       std::string* detail, size_t max_json_bytes) {
    out = LightsElement{};
    Fields f(data, size, "LcOpLightsElement");
    f.max_json_bytes = max_json_bytes;
    ObjectReader& r = f.r;
    bool ok = [&] {
        int32_t n;
        // Ett ljus tar minst kind, ett tomt namn, två markörer, GUID, en
        // bakåtpekare och sex f64: 4 + 4 + 4 + 16 + 4 + 8 + 48 byte.
        // Antalet prövas mot strömmen; listan växer med ljusen som läses.
        if (!r.count(88, n)) return false;
        for (int32_t k = 0; k < n; ++k) {
            LightsElement::Entry e;
            if (!r.i32(e.kind)) return false;
            if (e.kind != 83) return f.form("light kind is not 83");
            if (!r.str(e.name) || !f.marker(0, "marker after the light name is not 0") ||
                !f.guid(e.guid) || !f.marker(0, "marker after the light GUID is not 0"))
                return false;
            ObjectReader::Ptr p;
            if (!r.ptr(p, {27}, false, true)) return false;
            if (p.kind == ObjectReader::Ptr::New) {
                LightBody body;
                if (!read_light_body(f, out, body)) return false;
                p.value = static_cast<uint32_t>(out.lights.size());
                if (!r.finish(p.id, p.value)) return false;
                out.lights.push_back(body);
            }
            e.light = p.value;
            if (!f.f64s(e.origin, 3) || !f.f64s(e.direction, 3)) return false;
            out.entries.push_back(std::move(e));
        }
        return r.end();
    }();
    (void)ok;
    return f.finish(ErrorCode::CorruptObject, detail);
}

} // namespace HNwd::detail
