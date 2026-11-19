#include "material.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>
#include <system_error>
#include <utility>

namespace HNwd::detail {

namespace {

// Djupast tillåtna nästling av objekt och listor i materialets JSON.
constexpr int kMaxJsonDepth = 64;
// Felets text nedan säger "/ 16".
static_assert(kMaterialJsonBytesPerValue == 16);

// Ett tolkat JSON-värde. Ett objekt har nycklarna i `keys` och värdena i
// `items` i samma ordning, och `sorted` är nycklarnas index ordnade efter
// nyckeln; en lista har bara `items`.
struct Json {
    enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<std::string> keys;
    std::vector<uint32_t> sorted;
    std::vector<Json> items;
    size_t at = 0;   // tecknet där värdet börjar

    // Värdet för `key` i ett objekt, eller nullptr. Binärsökning i `sorted`.
    const Json* get(std::string_view key) const {
        if (kind != Kind::Object) return nullptr;
        const auto it = std::lower_bound(sorted.begin(), sorted.end(), key,
                                         [&](uint32_t i, std::string_view k) { return keys[i] < k; });
        if (it == sorted.end() || keys[*it] != key) return nullptr;
        return &items[*it];
    }
};

// Strikt JSON enligt RFC 8259: inga kommentarer, inga avslutande komman, inga
// dubbla nycklar i ett objekt. Texten ska vara giltig UTF-8.
class JsonParser {
public:
    // `max_bytes` är textens längsta längd; antalet värden får vara högst
    // max_bytes / kMaterialJsonBytesPerValue.
    JsonParser(const std::string& text, size_t max_bytes)
        : t_(text), max_bytes_(max_bytes), max_values_(max_bytes / kMaterialJsonBytesPerValue) {}

    bool parse(Json& out) {
        if (t_.size() > max_bytes_)
            return limit(0, "text is longer than the limit Options::max_material_json_bytes");
        if (!valid_utf8(reinterpret_cast<const uint8_t*>(t_.data()), t_.size()))
            return fail(0, "text is not valid UTF-8");
        space();
        if (!value(out, 0)) return false;
        space();
        if (pos_ != t_.size()) return fail(pos_, "text follows the value");
        return true;
    }

    size_t error_at() const { return at_; }
    const std::string& rule() const { return rule_; }
    // Är felet textens längd eller antalet värden över taket?
    bool limit_exceeded() const { return limit_; }

private:
    bool fail(size_t at, const char* rule) {
        at_ = at;
        rule_ = rule;
        return false;
    }
    bool limit(size_t at, const char* rule) {
        limit_ = true;
        return fail(at, rule);
    }

    bool end() const { return pos_ >= t_.size(); }
    char peek() const { return t_[pos_]; }

    void space() {
        while (!end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r'))
            ++pos_;
    }

    bool value(Json& out, int depth) {
        out = Json{};
        out.at = pos_;
        if (end()) return fail(pos_, "unexpected end");
        if (++values_ > max_values_)
            return limit(pos_, "more values than the limit Options::max_material_json_bytes / 16 allows");
        const char c = peek();
        if (c == '{' || c == '[') {
            if (depth >= kMaxJsonDepth) return fail(pos_, "nesting deeper than the limit");
            return c == '{' ? object(out, depth + 1) : array(out, depth + 1);
        }
        if (c == '"') {
            out.kind = Json::Kind::String;
            return string(out.s);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            out.kind = Json::Kind::Number;
            return number(out.n);
        }
        if (word("true")) {
            out.kind = Json::Kind::Bool;
            out.b = true;
            return true;
        }
        if (word("false")) {
            out.kind = Json::Kind::Bool;
            return true;
        }
        if (word("null")) return true;
        return fail(pos_, "unexpected character");
    }

    bool word(const char* w) {
        const size_t n = std::strlen(w);
        if (t_.compare(pos_, n, w) != 0) return false;
        pos_ += n;
        return true;
    }

    bool object(Json& out, int depth) {
        out.kind = Json::Kind::Object;
        ++pos_;   // {
        space();
        if (!end() && peek() == '}') {
            ++pos_;
            return true;
        }
        std::vector<size_t> key_at;   // där varje nyckel står, för en dubblett
        for (;;) {
            space();
            if (end()) return fail(pos_, "unexpected end");
            if (peek() != '"') return fail(pos_, "object key is not a string");
            key_at.push_back(pos_);
            std::string key;
            if (!string(key)) return false;
            space();
            if (end()) return fail(pos_, "unexpected end");
            if (peek() != ':') return fail(pos_, "expected ':' after an object key");
            ++pos_;
            space();
            Json v;
            if (!value(v, depth)) return false;
            out.keys.push_back(std::move(key));
            out.items.push_back(std::move(v));
            space();
            if (end()) return fail(pos_, "unexpected end");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return index_keys(out, key_at);
            }
            return fail(pos_, "expected ',' or '}' in an object");
        }
    }

    // Ordnar objektets nycklar i `sorted`. En nyckel som står två gånger är
    // ett fel vid dess andra förekomst; står flera dubbletter gäller den
    // första i texten.
    bool index_keys(Json& out, const std::vector<size_t>& key_at) {
        const std::vector<std::string>& keys = out.keys;
        out.sorted.resize(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) out.sorted[i] = static_cast<uint32_t>(i);
        std::sort(out.sorted.begin(), out.sorted.end(), [&](uint32_t a, uint32_t b) {
            return keys[a] < keys[b] || (keys[a] == keys[b] && a < b);
        });
        size_t first_duplicate = SIZE_MAX;
        for (size_t i = 1; i < out.sorted.size(); ++i)
            if (keys[out.sorted[i]] == keys[out.sorted[i - 1]])
                first_duplicate = std::min(first_duplicate, key_at[out.sorted[i]]);
        if (first_duplicate != SIZE_MAX) return fail(first_duplicate, "duplicate key in an object");
        return true;
    }

    bool array(Json& out, int depth) {
        out.kind = Json::Kind::Array;
        ++pos_;   // [
        space();
        if (!end() && peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            space();
            Json v;
            if (!value(v, depth)) return false;
            out.items.push_back(std::move(v));
            space();
            if (end()) return fail(pos_, "unexpected end");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail(pos_, "expected ',' or ']' in an array");
        }
    }

    bool hex4(uint32_t& v) {
        if (t_.size() - pos_ < 4) return fail(pos_, "unexpected end");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t_[pos_ + static_cast<size_t>(i)];
            uint32_t d;
            if (c >= '0' && c <= '9')
                d = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                d = static_cast<uint32_t>(c - 'A' + 10);
            else
                return fail(pos_ + static_cast<size_t>(i), "\\u escape has a non-hex digit");
            v = (v << 4) | d;
        }
        pos_ += 4;
        return true;
    }

    static void put_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // En sträng med citattecknen; flyktsekvenserna avkodas till UTF-8.
    bool string(std::string& out) {
        out.clear();
        ++pos_;   // "
        for (;;) {
            if (end()) return fail(pos_, "string is not closed");
            const unsigned char c = static_cast<unsigned char>(peek());
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return fail(pos_, "control character in a string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            const size_t esc_at = pos_;
            ++pos_;
            if (end()) return fail(pos_, "unexpected end");
            const char e = peek();
            ++pos_;
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xDC00 && cp <= 0xDFFF)
                    return fail(esc_at, "\\u escape is a lone low surrogate");
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (t_.compare(pos_, 2, "\\u") != 0)
                        return fail(esc_at, "\\u escape is a high surrogate without its pair");
                    pos_ += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF)
                        return fail(esc_at, "\\u escape is a high surrogate without its pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(out, cp);
                break;
            }
            default: return fail(esc_at, "unknown escape in a string");
            }
        }
    }

    static bool digit(char c) { return c >= '0' && c <= '9'; }

    // -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
    bool number(double& out) {
        const size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (end() || !digit(peek())) return fail(pos_, "number has no digits");
        if (peek() == '0') {
            ++pos_;
        } else {
            while (!end() && digit(peek())) ++pos_;
        }
        if (!end() && peek() == '.') {
            ++pos_;
            if (end() || !digit(peek())) return fail(pos_, "number has no digits after '.'");
            while (!end() && digit(peek())) ++pos_;
        }
        if (!end() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!end() && (peek() == '+' || peek() == '-')) ++pos_;
            if (end() || !digit(peek())) return fail(pos_, "number has no digits in the exponent");
            while (!end() && digit(peek())) ++pos_;
        }
        const char* first = t_.data() + start;
        const char* last = t_.data() + pos_;
        const std::from_chars_result r = std::from_chars(first, last, out);
        if (r.ec == std::errc::result_out_of_range) return fail(start, "number is out of range");
        if (r.ec != std::errc() || r.ptr != last) return fail(start, "number is not readable");
        return true;
    }

    const std::string& t_;
    size_t max_bytes_;
    size_t max_values_;
    size_t pos_ = 0;
    size_t at_ = 0;
    size_t values_ = 0;
    bool limit_ = false;
    std::string rule_;
};

// ---------------------------------------------------------------------------
// Det materialet säger.

class Extractor {
public:
    Extractor(const Json& root, MaterialTexture& out) : root_(root), out_(out) {}

    bool run() {
        if (root_.kind != Json::Kind::Object) return fail(root_, "material json is not an object");
        const Json* materials = root_.get("materials");
        if (!materials) return true;
        if (materials->kind != Json::Kind::Object)
            return fail(*materials, "\"materials\" is not an object");
        materials_ = materials;

        const Json* users = root_.get("userassets");
        if (!users) return true;
        if (users->kind != Json::Kind::Array) return fail(*users, "\"userassets\" is not an array");

        const Json* generic = nullptr;
        for (const Json& id : users->items) {
            const Json* asset = nullptr;
            if (!lookup(id, asset)) return false;
            const Json* definition = nullptr;
            if (!definition_of(*asset, definition)) return false;
            if (definition && definition->s == "Generic") {
                generic = asset;
                break;
            }
        }
        if (!generic) return true;
        return generic_asset(*generic);
    }

    size_t at() const { return at_; }
    const std::string& rule() const { return rule_; }

private:
    bool fail(const Json& where, const char* rule) {
        at_ = where.at;
        rule_ = rule;
        return false;
    }

    // Ett id i "userassets" eller "connections": en sträng som är en nyckel i "materials".
    bool lookup(const Json& id, const Json*& asset) {
        if (id.kind != Json::Kind::String) return fail(id, "asset reference is not a string");
        asset = materials_->get(id.s);
        if (!asset) return fail(id, "asset reference is not in \"materials\"");
        if (asset->kind != Json::Kind::Object) return fail(*asset, "asset is not an object");
        return true;
    }

    bool definition_of(const Json& asset, const Json*& definition) {
        definition = asset.get("definition");
        if (definition && definition->kind != Json::Kind::String)
            return fail(*definition, "\"definition\" is not a string");
        return true;
    }

    // En sektion i en tillgång, eller nullptr om nyckeln inte står. Sektionerna
    // ("colors", "uris", "scalars", "booleans") ligger i tillgångens
    // "properties".
    bool section(const Json& asset, const char* key, const Json*& out) {
        out = nullptr;
        const Json* properties = asset.get("properties");
        if (!properties) return true;
        if (properties->kind != Json::Kind::Object)
            return fail(*properties, "\"properties\" is not an object");
        const Json* found = properties->get(key);
        if (!found) return true;
        if (found->kind != Json::Kind::Object) return fail(*found, "asset section is not an object");
        out = found;
        return true;
    }

    // "values" i ett värde: listan, eller nullptr om den saknas eller är tom.
    bool first_value(const Json& holder, const Json*& out) {
        out = nullptr;
        const Json* values = holder.get("values");
        if (!values) return true;
        if (values->kind != Json::Kind::Array) return fail(*values, "\"values\" is not an array");
        if (!values->items.empty()) out = &values->items[0];
        return true;
    }

    bool generic_asset(const Json& generic) {
        const Json* colors = nullptr;
        if (!section(generic, "colors", colors)) return false;
        if (!colors) return true;
        const Json* diffuse = colors->get("generic_diffuse");
        if (!diffuse) return true;
        if (diffuse->kind != Json::Kind::Object)
            return fail(*diffuse, "\"generic_diffuse\" is not an object");

        const Json* rgba = nullptr;
        if (!first_value(*diffuse, rgba)) return false;
        if (rgba) {
            if (rgba->kind != Json::Kind::Object)
                return fail(*rgba, "generic_diffuse value is not an object");
            const char* names[4] = {"r", "g", "b", "a"};
            for (int k = 0; k < 4; ++k) {
                const Json* c = rgba->get(names[k]);
                if (!c) {
                    if (k == 3) continue;   // utan "a" är färgen ogenomskinlig
                    return fail(*rgba, "generic_diffuse value lacks r, g or b");
                }
                if (c->kind != Json::Kind::Number)
                    return fail(*c, "generic_diffuse component is not a number");
                out_.color[k] = static_cast<float>(c->n);
            }
            out_.has_color = true;
        }

        // En koppling som är avstängd följs inte: bilden räknas som inte inkopplad.
        if (const Json* enabled = diffuse->get("IsConnectionEnabled")) {
            if (enabled->kind != Json::Kind::Bool)
                return fail(*enabled, "\"IsConnectionEnabled\" is not true or false");
            if (!enabled->b) return true;
        }
        const Json* connections = diffuse->get("connections");
        if (!connections) return true;
        if (connections->kind != Json::Kind::Array)
            return fail(*connections, "\"connections\" is not an array");
        for (const Json& id : connections->items) {
            const Json* asset = nullptr;
            if (!lookup(id, asset)) return false;
            const Json* definition = nullptr;
            if (!definition_of(*asset, definition)) return false;
            if (definition && definition->s == "UnifiedBitmap") return bitmap_asset(*asset);
        }
        return true;
    }

    // Ett tal direkt, eller {"values":[tal], "units":"…"}.
    bool scalar(const Json& v, double& out, std::string* units) {
        if (v.kind == Json::Kind::Number) {
            out = v.n;
            return true;
        }
        if (v.kind != Json::Kind::Object)
            return fail(v, "scalar is neither a number nor an object with \"values\"");
        const Json* first = nullptr;
        if (!first_value(v, first)) return false;
        if (first) {
            if (first->kind != Json::Kind::Number) return fail(*first, "scalar value is not a number");
            out = first->n;
        }
        const Json* u = v.get("units");
        if (u) {
            if (u->kind != Json::Kind::String) return fail(*u, "\"units\" is not a string");
            if (units) *units = u->s;
        }
        return true;
    }

    // Sant eller falskt direkt, eller {"values":[sant eller falskt]}.
    bool boolean(const Json& v, bool& out) {
        if (v.kind == Json::Kind::Bool) {
            out = v.b;
            return true;
        }
        if (v.kind != Json::Kind::Object)
            return fail(v, "boolean is neither true, false nor an object with \"values\"");
        const Json* first = nullptr;
        if (!first_value(v, first)) return false;
        if (first) {
            if (first->kind != Json::Kind::Bool) return fail(*first, "boolean value is not true or false");
            out = first->b;
        }
        return true;
    }

    bool bitmap_asset(const Json& bitmap) {
        const Json* uris = nullptr;
        if (!section(bitmap, "uris", uris)) return false;
        if (uris) {
            if (const Json* path = uris->get("unifiedbitmap_Bitmap")) {
                if (path->kind != Json::Kind::Object)
                    return fail(*path, "\"unifiedbitmap_Bitmap\" is not an object");
                const Json* first = nullptr;
                if (!first_value(*path, first)) return false;
                if (first) {
                    if (first->kind != Json::Kind::String)
                        return fail(*first, "bitmap file name is not a string");
                    out_.bitmap = first->s;
                }
            }
        }

        const Json* scalars = nullptr;
        if (!section(bitmap, "scalars", scalars)) return false;
        if (scalars) {
            const struct {
                const char* key;
                double* value;
                std::string* units;
            } wanted[] = {
                {"texture_UScale", &out_.u_scale, nullptr},
                {"texture_VScale", &out_.v_scale, nullptr},
                {"texture_UOffset", &out_.u_offset, nullptr},
                {"texture_VOffset", &out_.v_offset, nullptr},
                {"texture_WAngle", &out_.w_angle, nullptr},
                {"texture_RealWorldScaleX", &out_.real_scale_x, &out_.real_units_x},
                {"texture_RealWorldScaleY", &out_.real_scale_y, &out_.real_units_y},
            };
            for (const auto& w : wanted)
                if (const Json* v = scalars->get(w.key))
                    if (!scalar(*v, *w.value, w.units)) return false;
        }

        const Json* booleans = nullptr;
        if (!section(bitmap, "booleans", booleans)) return false;
        if (booleans) {
            if (const Json* v = booleans->get("texture_URepeat"))
                if (!boolean(*v, out_.u_repeat)) return false;
            if (const Json* v = booleans->get("texture_VRepeat"))
                if (!boolean(*v, out_.v_repeat)) return false;
        }
        return true;
    }

    const Json& root_;
    MaterialTexture& out_;
    const Json* materials_ = nullptr;
    size_t at_ = 0;
    std::string rule_;
};

// Så många byte i taget växer en inbäddad fil när den läses.
constexpr size_t kBytesStep = size_t(1) << 20;

// `len` byte ur strömmen, med nollorna upp till 4 efter dem. Läses fyra byte
// i taget genom läsarens int32, som är little-endian: byten kommer ut i
// filens ordning.
//
// Genom ett fönster är läsarens size() bara en övre gräns (se
// object_reader.h), så `out` växer i steg om kBytesStep medan byten läses:
// minnet följer det som faktiskt finns i strömmen, inte längden ur filen.
bool read_bytes(ObjectReader& r, int64_t len, std::vector<uint8_t>& out) {
    out.clear();
    if (!r.ok()) return false;
    if (len < 0) return r.fail("byte length is negative");
    const uint64_t n = static_cast<uint64_t>(len);
    const uint64_t padded = (n + 3) & ~uint64_t(3);
    const uint64_t left = r.size() - r.pos();
    if (n > left || padded > left) return r.fail("byte length passes the end of the data");

    const size_t total = static_cast<size_t>(n);
    size_t k = 0;
    for (; k + 4 <= total; k += 4) {
        if (k + 4 > out.size()) out.resize(std::min(total, out.size() + kBytesStep));
        int32_t v;
        if (!r.i32(v)) return false;
        const uint32_t u = static_cast<uint32_t>(v);
        for (size_t j = 0; j < 4; ++j) out[k + j] = static_cast<uint8_t>(u >> (8 * j));
    }
    out.resize(total);
    if (k < out.size()) {
        int32_t v;
        if (!r.i32(v)) return false;
        const uint32_t u = static_cast<uint32_t>(v);
        for (size_t j = 0; j < 4; ++j) {
            const uint8_t byte = static_cast<uint8_t>(u >> (8 * j));
            if (k + j < out.size())
                out[k + j] = byte;
            else if (byte != 0)
                return r.fail("pad after the bytes is not zero");
        }
    }
    // Vektorn kan ha vuxit förbi längden; den behåller bara byten.
    if (out.capacity() != out.size()) out.shrink_to_fit();
    return true;
}

} // namespace

bool parse_material_json(const std::string& json, const std::vector<MaterialFile>& files,
                         MaterialTexture& out, size_t& at, std::string& rule,
                         size_t max_json_bytes, bool* limit) {
    out = MaterialTexture{};
    at = 0;
    rule.clear();
    if (limit) *limit = false;

    Json root;
    JsonParser parser(json, max_json_bytes);
    if (!parser.parse(root)) {
        at = parser.error_at();
        rule = parser.rule();
        if (limit) *limit = parser.limit_exceeded();
        return false;
    }
    Extractor extractor(root, out);
    if (!extractor.run()) {
        at = extractor.at();
        rule = extractor.rule();
        out = MaterialTexture{};
        return false;
    }
    if (!out.bitmap.empty()) {
        for (size_t i = 0; i < files.size(); ++i) {
            if (files[i].name == out.bitmap) {
                out.file = static_cast<uint32_t>(i);
                break;
            }
        }
    }
    return true;
}

bool read_class185(ObjectReader& r, MaterialData& out, size_t max_json_bytes) {
    out = MaterialData{};
    std::string second;
    int32_t n;
    if (!r.str(out.json) || !r.str(second) || !r.count(12, n)) return false;
    // Genom ett fönster är antalet bara prövat mot strömmens tak, så
    // tabellen växer med filerna som läses i stället för efter antalet.
    for (int32_t i = 0; i < n; ++i) {
        MaterialFile f;
        int32_t embedded;
        if (!r.str(f.name) || !r.i32(embedded)) return false;
        if (embedded != 0 && embedded != 1)
            return r.fail("class-185 item flag is neither 0 nor 1");
        if (!r.str(f.kind)) return false;
        f.embedded = embedded == 1;
        if (f.embedded) {
            int64_t len;
            if (!r.str(f.type) || !r.i64(len) || !read_bytes(r, len, f.bytes)) return false;
        }
        out.files.push_back(std::move(f));
    }

    size_t at = 0;
    std::string rule;
    bool limit = false;
    if (!parse_material_json(out.json, out.files, out.texture, at, rule, max_json_bytes, &limit)) {
        const std::string text = "class-185 json at character " + std::to_string(at) + ": " + rule;
        return limit ? r.fail_limit(text.c_str()) : r.fail(text.c_str());
    }
    return true;
}

} // namespace HNwd::detail
