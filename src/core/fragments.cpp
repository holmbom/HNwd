#include "fragments.h"

#include "byte_reader.h"
#include "object_reader.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace HNwd::detail {

static_assert(sizeof(FragmentStore::Item) == 24, "ett fragment är 24 byte i lagret");

uint32_t FragmentStore::pose_size(uint8_t pose_class) {
    switch (pose_class) {
    case 15: return 7;
    case 16: return 8;
    case 17: return 12;
    default: return 3;   // 14 och null
    }
}

void FragmentStore::pose(size_t i, PoseD& out) const {
    const Item& it = items[i];
    const double* p = pose_values(i);
    out = PoseD{};
    if (it.pose_class == 17) {
        out.affine = true;
        for (int k = 0; k < 9; ++k) out.m[k] = p[k];
        for (int k = 0; k < 3; ++k) out.t[k] = p[9 + k];
        return;
    }
    for (int k = 0; k < 3; ++k) out.t[k] = p[k];
    if (it.pose_class == 15 || it.pose_class == 16)
        for (int k = 0; k < 4; ++k) out.q[k] = p[3 + k];
    if (it.pose_class == 16) out.s = p[7];
}

size_t FragmentStore::memory_bytes() const {
    size_t n = sizeof(*this);
    n += items.capacity() * sizeof(Item);
    n += pose_blocks.capacity() * sizeof(std::vector<double>);
    for (const std::vector<double>& b : pose_blocks) n += b.capacity() * sizeof(double);
    n += geomrefs.capacity() * sizeof(GeometryRefData);
    n += looks.capacity() * sizeof(LookData);
    n += materials.capacity() * sizeof(MaterialData);
    for (const MaterialData& m : materials) {
        n += m.json.capacity() + m.texture.bitmap.capacity() + m.texture.real_units_x.capacity() +
             m.texture.real_units_y.capacity();
        n += m.files.capacity() * sizeof(MaterialFile);
        for (const MaterialFile& f : m.files)
            n += f.name.capacity() + f.kind.capacity() + f.type.capacity() + f.bytes.capacity();
    }
    return n;
}

namespace {

// Ett läst utseende (klass 55): vilket material det pekar ut och dess flaggor.
struct Appearance {
    uint32_t look;
    int32_t flags;
};

// Ett läst klass-93-objekt: mittpunkten, som bara tolkningen behöver, och
// dess plats i geomrefs.
struct GeometryObject {
    double c[3];
    uint32_t ref;
};
// Det ur en geometrireferens som bara verktygen behöver.
struct GeometryUnknown {
    float f;
    int32_t key;
};

// Största tillåtna avvikelse från 1 i en kvaternions längd. Kvaternionerna i
// filerna är normerade i dubbel precision (avvikelser kring 1e-15); taket
// rymmer också en kvaternion som normerats i enkel precision.
constexpr double kQuaternionTolerance = 1e-6;

// Plats för ett element till i `v`, som växer i takt med det som läses men
// aldrig förbi `limit` (antalet i strömmen): ett stort antal ur filen kostar
// inget minne förrän fragmenten faktiskt finns.
template <class T>
void grow_for_one(std::vector<T>& v, size_t limit) {
    if (v.size() < v.capacity()) return;
    size_t next = v.capacity() < 1024 ? 1024 : v.capacity() * 2;
    if (next > limit) next = limit;
    if (next <= v.size()) next = v.size() + 1;
    v.reserve(next);
}

class Parser {
public:
    Parser(ObjectReader& r, FragmentStore& out, std::vector<FragmentExtra>* extra,
           const std::function<bool(uint64_t, uint64_t)>* progress, size_t max_json_bytes)
        : r_(r), out_(out), extra_(extra), progress_(progress), max_json_bytes_(max_json_bytes) {}

    bool cancelled() const { return cancelled_; }
    uint64_t done() const { return out_.items.size(); }
    uint64_t total() const { return total_; }

    bool run() {
        int32_t n;
        // Ett fragment tar minst [id][13], D, fyra heltal, en ny klass-14-pose och
        // två bakåtpekare.
        if (!r_.count(52, n)) return false;
        total_ = static_cast<uint64_t>(n);
        if (!report(0)) return false;
        for (int32_t i = 0; i < n; ++i) {
            if (!fragment()) return false;
            const uint32_t read = static_cast<uint32_t>(i) + 1;
            if ((read % kProgressEvery == 0 || read == static_cast<uint32_t>(n)) && !report(read))
                return false;
        }
        if (!r_.end()) return false;
        // Det sista blocket i poolen och tabellerna som växt fritt behöver
        // inte mer plats.
        if (!out_.pose_blocks.empty()) out_.pose_blocks.back().shrink_to_fit();
        out_.geomrefs.shrink_to_fit();
        out_.looks.shrink_to_fit();
        out_.materials.shrink_to_fit();
        return true;
    }

private:
    bool report(uint64_t done) {
        if (!progress_ || !*progress_) return true;
        if ((*progress_)(done, total_)) return true;
        cancelled_ = true;
        return false;
    }

    bool finite(double v) { return std::isfinite(v) || r_.fail("pose value is not finite"); }

    // Plats för `count` tal i poolen. Ger posens index och en pekare till talen.
    bool pose_slot(uint32_t count, uint32_t& index, double*& values) {
        std::vector<std::vector<double>>& blocks = out_.pose_blocks;
        if (blocks.empty() ||
            blocks.back().size() + count > FragmentStore::kPoseBlock) {
            if (blocks.size() >= UINT32_MAX / FragmentStore::kPoseBlock)
                return r_.fail("the pose values do not fit in a 32-bit index");
            blocks.emplace_back();
            blocks.back().reserve(FragmentStore::kPoseBlock);
        }
        std::vector<double>& b = blocks.back();
        index = static_cast<uint32_t>((blocks.size() - 1) * FragmentStore::kPoseBlock + b.size());
        b.resize(b.size() + count);   // inom det reserverade: blocket flyttas inte
        values = b.data() + (b.size() - count);
        return true;
    }

    bool fragment() {
        ObjectReader::Ptr p;
        if (!r_.ptr(p, {13}, false, false)) return false;

        FragmentStore::Item it{};
        FragmentExtra ex{};
        if (!r_.f64(ex.d) || !r_.i32(it.primitives) || !r_.i32(ex.b) || !r_.i32(ex.c) ||
            !r_.i32(it.seq))
            return false;

        double* t = nullptr;   // flytten i poolen; fylls ur geometrin för en null-pose
        if (!pose(it, t) || !appearance(it, ex) || !geometry(it, ex, t)) return false;
        if (!r_.finish(p.id, 0)) return false;

        grow_for_one(out_.items, static_cast<size_t>(total_));
        out_.items.push_back(it);
        if (extra_) {
            grow_for_one(*extra_, static_cast<size_t>(total_));
            extra_->push_back(ex);
        }
        return true;
    }

    // Klass 17: tre kolumner om tre tal och en nolla, sedan en etta, sedan
    // flytten. Den linjära delen lagras radvis, följd av flytten.
    bool matrix_pose(double* v) {
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 4; ++row) {
                float x;
                if (!r_.f32(x)) return false;
                if (row == 3) {
                    if (x != 0) return r_.fail("class-17 column does not end with 0");
                    continue;
                }
                if (!finite(x)) return false;
                v[3 * row + column] = x;
            }
        }
        float one;
        if (!r_.f32(one)) return false;
        if (one != 1) return r_.fail("class-17 matrix does not end with 1");
        for (int k = 0; k < 3; ++k)
            if (!r_.f64(v[9 + k]) || !finite(v[9 + k])) return false;

        const double* m = v;
        const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                           m[2] * (m[3] * m[7] - m[4] * m[6]);
        if (!(det != 0) || !std::isfinite(det)) return r_.fail("class-17 matrix has determinant 0");
        return true;
    }

    // Posen är ett nytt objekt eller null, aldrig en bakåtpekare. Klass 14 saknar
    // rotation, klass 15 skala, klass 17 är en full matris, och null är ingen
    // pose alls: pose_class 0, utan rotation och skala, med flytten ur
    // geometrireferensen (se geometry()). `t` pekar på flytten i poolen.
    bool pose(FragmentStore::Item& it, double*& t) {
        ObjectReader::Ptr p;
        if (!r_.ptr(p, {14, 15, 16, 17}, true, false)) return false;

        it.pose_class = static_cast<uint8_t>(p.kind == ObjectReader::Ptr::Null ? 0 : p.cls);
        double* v = nullptr;
        if (!pose_slot(FragmentStore::pose_size(it.pose_class), it.pose, v)) return false;
        if (p.kind == ObjectReader::Ptr::Null) {
            t = v;
            return true;
        }
        if (p.cls == 17) {
            t = v + 9;
            return matrix_pose(v) && r_.finish(p.id, 0);
        }
        t = v;
        for (int k = 0; k < 3; ++k)
            if (!r_.f64(v[k]) || !finite(v[k])) return false;

        if (p.cls != 14) {
            for (int k = 3; k < 7; ++k)
                if (!r_.f64(v[k]) || !finite(v[k])) return false;
            const double length = std::sqrt(v[3] * v[3] + v[4] * v[4] + v[5] * v[5] + v[6] * v[6]);
            if (!(std::fabs(length - 1) <= kQuaternionTolerance))
                return r_.fail("pose quaternion does not have length 1");
        }
        if (p.cls == 16) {
            if (!r_.f64(v[7]) || !finite(v[7])) return false;
            if (v[7] == 0) return r_.fail("pose scale is 0");
        }
        return r_.finish(p.id, 0);
    }

    // Utseendet (klass 55) delas av flera fragment genom bakåtpekare.
    bool appearance(FragmentStore::Item& it, FragmentExtra& ex) {
        ObjectReader::Ptr p;
        if (!r_.ptr(p, {55}, false, true)) return false;

        if (p.kind == ObjectReader::Ptr::Back) {
            it.look = appearances_[p.value].look;
            ex.appearance_flags = appearances_[p.value].flags;
            return true;
        }

        int32_t flags;
        uint32_t colors;
        if (!r_.i32(flags) || !material(colors)) return false;
        uint32_t data = kNoMaterial;
        if ((flags & 32) != 0) {
            ObjectReader::Ptr extra;
            if (!r_.ptr(extra, {185}, true, true)) return false;
            if (extra.kind == ObjectReader::Ptr::New) {
                // Läses rakt in i tabellen, så att bildernas byte kopieras en gång.
                data = static_cast<uint32_t>(out_.materials.size());
                out_.materials.emplace_back();
                if (!read_class185(r_, out_.materials.back(), max_json_bytes_) ||
                    !r_.finish(extra.id, data))
                    return false;
            } else if (extra.kind == ObjectReader::Ptr::Back) {
                data = extra.value;
            }
        }
        const uint32_t look = intern(colors, data);
        // Flaggan 64 ger ett heltal till, 0 eller 1. Dess betydelse är inte känd.
        if ((flags & 64) != 0) {
            int32_t value;
            if (!r_.i32(value)) return false;
            if (value != 0 && value != 1)
                return r_.fail("class-55 value after flag 64 is not 0 or 1");
        }
        // Andra flaggor än de kända kan betyda fler fält.
        if ((flags & ~(1 | 16 | 32 | 64)) != 0)
            return r_.fail("class-55 flags have an unknown bit");

        const uint32_t index = static_cast<uint32_t>(appearances_.size());
        appearances_.push_back({look, flags});
        if (!r_.finish(p.id, index)) return false;
        it.look = look;
        ex.appearance_flags = flags;
        return true;
    }

    // Materialet (klass 54): 14 flyttal. Också det kan delas. `index` är dess
    // plats i colors_.
    bool material(uint32_t& index) {
        ObjectReader::Ptr p;
        if (!r_.ptr(p, {54}, false, true)) return false;
        if (p.kind == ObjectReader::Ptr::Back) {
            index = p.value;
            return true;
        }

        LookData data;
        for (float& v : data.v)
            if (!r_.f32(v) || (!std::isfinite(v) && !r_.fail("class-54 value is not finite")))
                return false;

        index = static_cast<uint32_t>(colors_.size());
        colors_.push_back(data);
        return r_.finish(p.id, index);
    }

    // Utseendet för paret (klass 54, klass 185): ett nytt index första gången
    // paret förekommer, annars det det fick då.
    uint32_t intern(uint32_t colors, uint32_t data) {
        const uint64_t key = (static_cast<uint64_t>(colors) << 32) | data;
        const auto found = looks_by_pair_.find(key);
        if (found != looks_by_pair_.end()) return found->second;
        LookData look = colors_[colors];
        look.material = data;
        const uint32_t index = static_cast<uint32_t>(out_.looks.size());
        out_.looks.push_back(look);
        looks_by_pair_.emplace(key, index);
        return index;
    }

    // Geometrireferensen (klass 93) delas av alla fragment som visar samma post.
    // Två objekt med samma post och samma låda lagras en gång i geomrefs.
    bool geometry(FragmentStore::Item& it, FragmentExtra& ex, double* t) {
        ObjectReader::Ptr p;
        if (!r_.ptr(p, {93}, false, true)) return false;

        if (p.kind == ObjectReader::Ptr::New) {
            GeometryRefData g{};
            GeometryObject o{};
            GeometryUnknown u{};
            for (float& v : g.bbox)
                if (!r_.f32(v) ||
                    (!std::isfinite(v) && !r_.fail("geometry box value is not finite")))
                    return false;
            for (double& v : o.c)
                if (!r_.f64(v)) return false;
            if (!r_.f32(u.f) || !r_.i32(u.key) || !r_.i32(g.record)) return false;

            o.ref = intern_ref(g);
            const uint32_t index = static_cast<uint32_t>(objects_.size());
            objects_.push_back(o);
            if (extra_) unknowns_.push_back(u);
            if (!r_.finish(p.id, index)) return false;
            p.value = index;
        }

        const GeometryObject& o = objects_[p.value];
        it.geomref = o.ref;
        // Postens positioner är lagrade kring sin mittpunkt. En pose bär den
        // redan i sin flytt; ett fragment utan pose står i mittpunkten.
        if (it.pose_class == 0)
            for (int i = 0; i < 3; ++i) {
                if (!std::isfinite(o.c[i]))
                    return r_.fail("geometry center is not finite in a fragment without a pose");
                t[i] = o.c[i];
            }
        if (extra_) {
            const GeometryUnknown& u = unknowns_[p.value];
            ex.key = u.key;
            for (int i = 0; i < 3; ++i) ex.center[i] = o.c[i];
            ex.f = u.f;
        }
        return true;
    }

    // Indexet i geomrefs för (post, låda): ett nytt första gången paret
    // förekommer. Lådorna jämförs bit för bit.
    uint32_t intern_ref(const GeometryRefData& g) {
        const auto found = ref_by_record_.find(g.record);
        if (found != ref_by_record_.end()) {
            for (uint32_t k = found->second; k != kNoMaterial; k = same_record_[k])
                if (std::memcmp(out_.geomrefs[k].bbox, g.bbox, sizeof g.bbox) == 0) return k;
        }
        const uint32_t index = static_cast<uint32_t>(out_.geomrefs.size());
        out_.geomrefs.push_back(g);
        // Den nya läggs först i postens kedja.
        same_record_.push_back(found != ref_by_record_.end() ? found->second : kNoMaterial);
        ref_by_record_[g.record] = index;
        return index;
    }

    ObjectReader& r_;
    FragmentStore& out_;
    std::vector<FragmentExtra>* extra_;
    const std::function<bool(uint64_t, uint64_t)>* progress_;
    size_t max_json_bytes_;   // tak för ett materials JSON
    uint64_t total_ = 0;
    bool cancelled_ = false;
    std::vector<Appearance> appearances_;
    std::vector<GeometryObject> objects_;     // per klass-93-objekt
    std::vector<GeometryUnknown> unknowns_;   // per klass-93-objekt, bara med `extra`
    std::unordered_map<int32_t, uint32_t> ref_by_record_;   // post → senaste index i geomrefs
    std::vector<uint32_t> same_record_;       // per index i geomrefs: förra med samma post, eller kNoMaterial
    std::vector<LookData> colors_;            // ett per klass-54-objekt, utan material
    std::unordered_map<uint64_t, uint32_t> looks_by_pair_;
};

// Kör tolkningen och översätter utfallet.
ErrorCode run(ObjectReader& r, FragmentStore& out, std::vector<FragmentExtra>* extra,
              const std::function<bool(uint64_t, uint64_t)>* progress, std::string* detail,
              size_t max_json_bytes) {
    out = FragmentStore{};
    if (extra) extra->clear();
    if (detail) detail->clear();

    ErrorCode code = ErrorCode::Ok;
    {
        Parser parser(r, out, extra, progress, max_json_bytes);
        if (parser.run() && r.ok()) return ErrorCode::Ok;
        if (parser.cancelled()) {
            code = ErrorCode::Cancelled;
            if (detail)
                *detail = "LcOpNwdFragments: cancelled after " + std::to_string(parser.done()) +
                          " of " + std::to_string(parser.total()) + " fragments";
        } else {
            code = r.ok() ? ErrorCode::CorruptObject : r.code();
            if (detail) *detail = r.detail();
        }
    }
    out = FragmentStore{};
    if (extra) {
        extra->clear();
        extra->shrink_to_fit();
    }
    return code;
}

} // namespace

ErrorCode parse_fragments(const uint8_t* data, size_t size, FragmentStore& out,
                          std::vector<FragmentExtra>* extra, std::string* detail,
                          size_t max_json_bytes) {
    ObjectReader r(data, size, "LcOpNwdFragments");
    return run(r, out, extra, nullptr, detail, max_json_bytes);
}

ErrorCode parse_fragments(InflateStream& stream, FragmentStore& out,
                          const std::function<bool(uint64_t, uint64_t)>& progress,
                          std::vector<FragmentExtra>* extra, std::string* detail,
                          size_t window_bytes, size_t max_json_bytes) {
    StreamWindow window(stream, window_bytes);
    ObjectReader r(window, "LcOpNwdFragments");
    return run(r, out, extra, &progress, detail, max_json_bytes);
}

} // namespace HNwd::detail
