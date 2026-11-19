#include "textures.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace nwdview {

namespace {

constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

bool is_png(const uint8_t* data, size_t size) {
    return size >= 8 && std::memcmp(data, kPngSignature, 8) == 0;
}

// Medelvärdet över f × f pixlar.
void shrink(DecodedImage& img, int max_side) {
    int f = 1;
    while (std::max((img.width + f - 1) / f, (img.height + f - 1) / f) > max_side) ++f;
    if (f == 1) return;
    const int w = (img.width + f - 1) / f, h = (img.height + f - 1) / f;
    std::vector<uint8_t> out(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned sum[4] = {0, 0, 0, 0}, count = 0;
            for (int yy = y * f; yy < std::min(img.height, y * f + f); ++yy)
                for (int xx = x * f; xx < std::min(img.width, x * f + f); ++xx) {
                    const uint8_t* s = &img.rgba[(static_cast<size_t>(yy) *
                                                      static_cast<size_t>(img.width) +
                                                  static_cast<size_t>(xx)) *
                                                 4];
                    for (int k = 0; k < 4; ++k) sum[k] += s[k];
                    ++count;
                }
            uint8_t* t = &out[(static_cast<size_t>(y) * static_cast<size_t>(w) +
                               static_cast<size_t>(x)) *
                              4];
            for (int k = 0; k < 4; ++k) t[k] = static_cast<uint8_t>((sum[k] + count / 2) / count);
        }
    img.rgba = std::move(out);
    img.width = w;
    img.height = h;
}

// Ljuset i båda programmen: toLight (riktningen mot ljuset, 1 = belyst),
// light (ambient, diffuse, exponent, motljus), wrap (upprepa u, upprepa v).
//
// Normalen vänds mot betraktaren: den platta normalen ur derivatorna pekar
// alltid mot ögat (korsprodukten i språkets ordning), och en normal som pekar
// bort från den speglas i ytans plan. En yta som ses från baksidan — en
// enkelsidig vägg inifrån, marken underifrån, en triangel med omvänd
// vridning — belyses då som om den setts framifrån. Speglingen är kontinuerlig
// där normalen ligger i ytans plan, så en rundad yta får ingen kant i ljuset.
// Motljuset lyser upp det som vänder sig bort från ljuset (ett tak underifrån),
// så att ingen yta får bara omgivningsljuset.
//
// Snittet: clip[k] = (n, d) i meshens lokala lägen; ett fragment där
// n · läget + d < 0 för något plan kastas. Ett tomt plan (0, 0, 0, 0) klipper
// inget.

// Flyttalen i GlazeUser: tre float4 för ljuset, sedan snittets plan.
constexpr size_t kClipOffset = 12;
constexpr size_t kUniformFloats = kClipOffset + 4 * HNwd::kClipPlaneCount;

std::string hlsl_source(bool textured) {
    std::string s = R"hlsl(
struct LitUser
{
    float4 toLight;
    float4 light;
    float4 wrap;
    float4 clip[6];
};
cbuffer GlazeUser : register(b4)
{
    LitUser u;
};
)hlsl";
    if (textured)
        s += R"hlsl(
Texture2D    gTex  : register(t0);
SamplerState gTexS : register(s0);
)hlsl";
    s += R"hlsl(
struct VsIn
{
    float3 pos    : ATTR0;
    float4 normal : ATTR1;
    float4 color  : ATTR2;
)hlsl";
    if (textured) s += "    float2 uv     : ATTR3;\n";
    s += R"hlsl(};
struct VsOut
{
    float4 pos    : SV_Position;
    float3 world  : TEXCOORD0;
    float4 color  : TEXCOORD1;
    float3 normal : TEXCOORD2;
)hlsl";
    if (textured) s += "    float2 uv     : TEXCOORD3;\n";
    s += R"hlsl(};

VsOut vsMain(VsIn i)
{
    VsOut o;
    o.pos    = glaze_toClip(mul(glaze.viewProj, float4(i.pos, 1.0)));
    o.world  = i.pos;
    o.color  = i.color;
    o.normal = i.normal.xyz;
)hlsl";
    if (textured) s += "    o.uv     = i.uv;\n";
    s += R"hlsl(    return o;
}

float4 fsMain(VsOut i) : SV_Target
{
    float3 mot = cross(ddy(i.world), ddx(i.world));
    float  ml  = dot(mot, mot);
    mot = ml > 0.0 ? mot * rsqrt(ml) : float3(0.0, 0.0, 0.0);
    float4 c = i.color;
)hlsl";
    if (textured)
        s += R"hlsl(    // Bildens rad 0 är överst; v växer uppåt.
    float2 st = float2(i.uv.x, 1.0 - i.uv.y);
    float2 dx = ddx(st), dy = ddy(st);
    if (u.wrap.x > 0.5) st.x = frac(st.x);
    if (u.wrap.y > 0.5) st.y = frac(st.y);
    c *= gTex.SampleGrad(gTexS, st, dx, dy);
)hlsl";
    s += R"hlsl(    float  nl = dot(i.normal, i.normal);
    float3 N  = nl > 1e-12 ? i.normal * rsqrt(nl) : mot;
    float  nm = dot(N, mot);
    if (nm < 0.0) N -= 2.0 * nm * mot;
    if (u.toLight.w > 0.5) {
        float ndl  = dot(N, u.toLight.xyz);
        float diff = ndl > 0.0 ? pow(ndl, u.light.z) : 0.0;
        float back = max(-ndl, 0.0);
        c.rgb = saturate(c.rgb * (u.light.x + u.light.y * diff + u.light.w * back));
    }
    [unroll] for (int k = 0; k < 6; ++k)
        if (dot(u.clip[k].xyz, i.world) + u.clip[k].w < 0.0) discard;
    c.a *= glaze.scale.z;
    return c;
}
)hlsl";
    return s;
}

std::string glsl_block() {
    return R"glsl(
layout(std140) uniform GlazeUser
{
    vec4 toLight;
    vec4 light;
    vec4 wrap;
    vec4 clip[6];
} u;
)glsl";
}

std::string glsl_vertex(bool textured) {
    std::string s = glsl_block();
    s += R"glsl(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aNormal;
layout(location = 2) in vec4 aColor;
)glsl";
    if (textured) s += "layout(location = 3) in vec2 aUv;\n";
    s += R"glsl(out vec3 vWorld;
out vec4 vColor;
out vec3 vNormal;
)glsl";
    if (textured) s += "out vec2 vUv;\n";
    s += R"glsl(
void main()
{
    gl_Position = glaze_toClip(glaze.viewProj * vec4(aPos, 1.0));
    vWorld  = aPos;
    vColor  = aColor;
    vNormal = aNormal.xyz;
)glsl";
    if (textured) s += "    vUv     = aUv;\n";
    s += "}\n";
    return s;
}

std::string glsl_fragment(bool textured) {
    std::string s = glsl_block();
    if (textured) s += "\nuniform sampler2D GlazeTexture0;\n";
    s += R"glsl(in vec3 vWorld;
in vec4 vColor;
in vec3 vNormal;
)glsl";
    if (textured) s += "in vec2 vUv;\n";
    s += R"glsl(out vec4 frag;

void main()
{
    vec3  mot = cross(dFdx(vWorld), dFdy(vWorld));
    float ml  = dot(mot, mot);
    mot = ml > 0.0 ? mot * inversesqrt(ml) : vec3(0.0);
    vec4 c = vColor;
)glsl";
    if (textured)
        s += R"glsl(    // Bildens rad 0 är överst; v växer uppåt.
    vec2 st = vec2(vUv.x, 1.0 - vUv.y);
    vec2 dx = dFdx(st), dy = dFdy(st);
    if (u.wrap.x > 0.5) st.x = fract(st.x);
    if (u.wrap.y > 0.5) st.y = fract(st.y);
    c *= textureGrad(GlazeTexture0, st, dx, dy);
)glsl";
    s += R"glsl(    float nl = dot(vNormal, vNormal);
    vec3  N  = nl > 1e-12 ? vNormal * inversesqrt(nl) : mot;
    float nm = dot(N, mot);
    if (nm < 0.0) N -= 2.0 * nm * mot;
    if (u.toLight.w > 0.5) {
        float ndl  = dot(N, u.toLight.xyz);
        float diff = ndl > 0.0 ? pow(ndl, u.light.z) : 0.0;
        float back = max(-ndl, 0.0);
        c.rgb = clamp(c.rgb * (u.light.x + u.light.y * diff + u.light.w * back), 0.0, 1.0);
    }
    for (int k = 0; k < 6; ++k)
        if (dot(u.clip[k].xyz, vWorld) + u.clip[k].w < 0.0) discard;
    c.a *= glaze.scale.z;
    frag = c;
}
)glsl";
    return s;
}

std::string msl_source(bool textured) {
    std::string s = R"msl(
struct LitUser
{
    float4 toLight;
    float4 light;
    float4 wrap;
    float4 clip[6];
};
struct VIn
{
    float3 pos    [[attribute(0)]];
    float4 normal [[attribute(1)]];
    float4 color  [[attribute(2)]];
)msl";
    if (textured) s += "    float2 uv     [[attribute(3)]];\n";
    s += R"msl(};
struct VOut
{
    float4 pos [[position]];
    float3 world;
    float4 color;
    float3 normal;
)msl";
    if (textured) s += "    float2 uv;\n";
    s += R"msl(};

vertex VOut vsMain(VIn i [[stage_in]], constant GlazeFrame& glaze [[buffer(2)]],
                   constant LitUser& u [[buffer(3)]])
{
    VOut o;
    o.pos    = glaze_toClip(glaze.viewProj * float4(i.pos, 1.0), glaze);
    o.world  = i.pos;
    o.color  = i.color;
    o.normal = i.normal.xyz;
)msl";
    if (textured) s += "    o.uv     = i.uv;\n";
    s += R"msl(    return o;
}

fragment float4 fsMain(VOut i [[stage_in]], constant GlazeFrame& glaze [[buffer(2)]],
                       constant LitUser& u [[buffer(3)]])msl";
    if (textured)
        s += ",\n                       texture2d<float> tex [[texture(0)]], "
             "sampler texS [[sampler(0)]]";
    s += R"msl()
{
    float3 mot = cross(dfdy(i.world), dfdx(i.world));
    float  ml  = dot(mot, mot);
    mot = ml > 0.0 ? mot * rsqrt(ml) : float3(0.0);
    float4 c = i.color;
)msl";
    if (textured)
        s += R"msl(    float2 st = float2(i.uv.x, 1.0 - i.uv.y);
    float2 dx = dfdx(st), dy = dfdy(st);
    if (u.wrap.x > 0.5) st.x = fract(st.x);
    if (u.wrap.y > 0.5) st.y = fract(st.y);
    c *= tex.sample(texS, st, gradient2d(dx, dy));
)msl";
    s += R"msl(    float  nl = dot(i.normal, i.normal);
    float3 N  = nl > 1e-12 ? i.normal * rsqrt(nl) : mot;
    float  nm = dot(N, mot);
    if (nm < 0.0) N -= 2.0 * nm * mot;
    if (u.toLight.w > 0.5) {
        float ndl  = dot(N, u.toLight.xyz);
        float diff = ndl > 0.0 ? pow(ndl, u.light.z) : 0.0;
        float back = max(-ndl, 0.0);
        c.rgb = saturate(c.rgb * (u.light.x + u.light.y * diff + u.light.w * back));
    }
    for (int k = 0; k < 6; ++k)
        if (dot(u.clip[k].xyz, i.world) + u.clip[k].w < 0.0) discard_fragment();
    c.a *= glaze.scale.z;
    return c;
}
)msl";
    return s;
}

// LINJEPROGRAMMET. Ett segment är en kvad med fyra hörn; varje hörn bär sitt
// eget läge, segmentets andra ände (user0.xyz, relativt meshens origo) och
// sidan av linjen (user0.w, −1 eller +1). Vertexsteget:
//
//  1. Projicerar båda ändarna. Förspelets pixelklipp har x/w och y/w i målets
//     fysiska px och djupet z i [0, w] (närplanet vid z = 0).
//  2. Klipper segmentet mot närplanet i klipprymden (z − 1e-5·w ≥ 0, linjärt i
//     ändarna, så klippet är exakt också för läget som fragmentsteget klipper
//     snittet mot). Ligger båda ändarna bakom ögat ritas inget: alla fyra hörn
//     får samma punkt och trianglarna blir tomma.
//  3. Räknar riktningen mellan de klippta ändarna i px och flyttar hörnet i px:
//     en halv linjebredd vinkelrätt mot riktningen åt sidans håll, och en halv
//     linjebredd längs riktningen bort från den andra änden (fyrkantig ände, så
//     att skarvarna mellan segmenten inte får springor).
//
// Bredden är linjens logiska px gånger ritytans skala (glaze.scale.y), alltså
// samma fysiska bredd vid varje avstånd. Fragmentsteget ger vertexfärgen utan
// belysning och klipper mot snittets plan i meshens lokala lägen (läget längs
// segmentet, interpolerat).
//
// Flyttalen i GlazeUser: bredden (float4, x), sedan snittets sex plan.
constexpr size_t kLineClipOffset = 4;
constexpr size_t kLineUniformFloats = kLineClipOffset + 4 * HNwd::kClipPlaneCount;

std::string line_hlsl() {
    return R"hlsl(
struct LineUser
{
    float4 params;
    float4 clip[6];
};
cbuffer GlazeUser : register(b4)
{
    LineUser u;
};
struct VsIn
{
    float3 pos   : ATTR0;
    float4 color : ATTR2;
    float4 other : ATTR5;
};
struct VsOut
{
    float4 pos   : SV_Position;
    float3 world : TEXCOORD0;
    float4 color : TEXCOORD1;
};

VsOut vsMain(VsIn i)
{
    VsOut o;
    o.world = i.pos;
    o.color = i.color;
    float4 ca = mul(glaze.viewProj, float4(i.pos, 1.0));
    float4 cb = mul(glaze.viewProj, float4(i.other.xyz, 1.0));
    float  fa = ca.z - 1e-5 * ca.w;
    float  fb = cb.z - 1e-5 * cb.w;
    if (fa < 0.0 && fb < 0.0) {
        o.pos = glaze_toClip(float4(0.0, 0.0, 0.0, 1.0));
        return o;
    }
    float  ta = fa < 0.0 ? fa / (fa - fb) : 0.0;
    float  tb = fb < 0.0 ? fb / (fb - fa) : 0.0;
    float4 a  = ca + ta * (cb - ca);
    float4 b  = cb + tb * (ca - cb);
    o.world   = i.pos + ta * (i.other.xyz - i.pos);
    float2 d   = b.xy / b.w - a.xy / a.w;
    float  len = length(d);
    d = len > 1e-4 ? d / len : float2(1.0, 0.0);
    float  hw  = 0.5 * u.params.x * glaze.scale.y;
    float2 off = i.other.w * hw * float2(-d.y, d.x) - hw * d;
    a.xy += off * a.w;
    o.pos = glaze_toClip(a);
    return o;
}

float4 fsMain(VsOut i) : SV_Target
{
    [unroll] for (int k = 0; k < 6; ++k)
        if (dot(u.clip[k].xyz, i.world) + u.clip[k].w < 0.0) discard;
    float4 c = i.color;
    c.a *= glaze.scale.z;
    return c;
}
)hlsl";
}

std::string line_glsl_block() {
    return R"glsl(
layout(std140) uniform GlazeUser
{
    vec4 params;
    vec4 clip[6];
} u;
)glsl";
}

std::string line_glsl_vertex() {
    return line_glsl_block() + R"glsl(
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec4 aColor;
layout(location = 5) in vec4 aOther;
out vec3 vWorld;
out vec4 vColor;

void main()
{
    vWorld = aPos;
    vColor = aColor;
    vec4  ca = glaze.viewProj * vec4(aPos, 1.0);
    vec4  cb = glaze.viewProj * vec4(aOther.xyz, 1.0);
    float fa = ca.z - 1e-5 * ca.w;
    float fb = cb.z - 1e-5 * cb.w;
    if (fa < 0.0 && fb < 0.0) {
        gl_Position = glaze_toClip(vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }
    float ta = fa < 0.0 ? fa / (fa - fb) : 0.0;
    float tb = fb < 0.0 ? fb / (fb - fa) : 0.0;
    vec4  a  = ca + ta * (cb - ca);
    vec4  b  = cb + tb * (ca - cb);
    vWorld   = aPos + ta * (aOther.xyz - aPos);
    vec2  d   = b.xy / b.w - a.xy / a.w;
    float len = length(d);
    d = len > 1e-4 ? d / len : vec2(1.0, 0.0);
    float hw  = 0.5 * u.params.x * glaze.scale.y;
    vec2  off = aOther.w * hw * vec2(-d.y, d.x) - hw * d;
    a.xy += off * a.w;
    gl_Position = glaze_toClip(a);
}
)glsl";
}

std::string line_glsl_fragment() {
    return line_glsl_block() + R"glsl(
in vec3 vWorld;
in vec4 vColor;
out vec4 frag;

void main()
{
    for (int k = 0; k < 6; ++k)
        if (dot(u.clip[k].xyz, vWorld) + u.clip[k].w < 0.0) discard;
    vec4 c = vColor;
    c.a *= glaze.scale.z;
    frag = c;
}
)glsl";
}

std::string line_msl() {
    return R"msl(
struct LineUser
{
    float4 params;
    float4 clip[6];
};
struct VIn
{
    float3 pos   [[attribute(0)]];
    float4 color [[attribute(2)]];
    float4 other [[attribute(5)]];
};
struct VOut
{
    float4 pos [[position]];
    float3 world;
    float4 color;
};

vertex VOut vsMain(VIn i [[stage_in]], constant GlazeFrame& glaze [[buffer(2)]],
                   constant LineUser& u [[buffer(3)]])
{
    VOut o;
    o.world = i.pos;
    o.color = i.color;
    float4 ca = glaze.viewProj * float4(i.pos, 1.0);
    float4 cb = glaze.viewProj * float4(i.other.xyz, 1.0);
    float  fa = ca.z - 1e-5 * ca.w;
    float  fb = cb.z - 1e-5 * cb.w;
    if (fa < 0.0 && fb < 0.0) {
        o.pos = glaze_toClip(float4(0.0, 0.0, 0.0, 1.0), glaze);
        return o;
    }
    float  ta = fa < 0.0 ? fa / (fa - fb) : 0.0;
    float  tb = fb < 0.0 ? fb / (fb - fa) : 0.0;
    float4 a  = ca + ta * (cb - ca);
    float4 b  = cb + tb * (ca - cb);
    o.world   = i.pos + ta * (i.other.xyz - i.pos);
    float2 d   = b.xy / b.w - a.xy / a.w;
    float  len = length(d);
    d = len > 1e-4 ? d / len : float2(1.0, 0.0);
    float  hw  = 0.5 * u.params.x * glaze.scale.y;
    float2 off = i.other.w * hw * float2(-d.y, d.x) - hw * d;
    a.xy += off * a.w;
    o.pos = glaze_toClip(a, glaze);
    return o;
}

fragment float4 fsMain(VOut i [[stage_in]], constant GlazeFrame& glaze [[buffer(2)]],
                       constant LineUser& u [[buffer(3)]])
{
    for (int k = 0; k < 6; ++k)
        if (dot(u.clip[k].xyz, i.world) + u.clip[k].w < 0.0) discard_fragment();
    float4 c = i.color;
    c.a *= glaze.scale.z;
    return c;
}
)msl";
}

glaze::Program make_program(const char* name, bool textured) {
    glaze::Program p;
    p.name = name;
    p.hlsl = hlsl_source(textured);
    p.glslVertex = glsl_vertex(textured);
    p.glslFragment = glsl_fragment(textured);
    p.msl = msl_source(textured);
    p.textures = textured ? 1 : 0;
    p.uniformFloats = static_cast<int>(kUniformFloats);
    return p;
}

// Snittets plan i ett GlazeUser-block: planen börjar vid `offset` och blocket
// är `total` flyttal. Varje plan blir (n, d + n · origin), eftersom lägena är
// relativa meshens origo.
void write_clip(std::vector<float>& uniforms, size_t offset, size_t total, const ClipState& clip,
                const double origin[3]) {
    uniforms.resize(total, 0.0f);
    for (size_t k = 0; k < HNwd::kClipPlaneCount; ++k) {
        float* out = &uniforms[offset + 4 * k];
        if (static_cast<int>(k) >= clip.count) {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            continue;
        }
        const double* p = clip.plane[k];
        for (int a = 0; a < 3; ++a) out[a] = static_cast<float>(p[a]);
        out[3] = static_cast<float>(p[3] + p[0] * origin[0] + p[1] * origin[1] + p[2] * origin[2]);
    }
}

} // namespace

bool texture_format_known(const uint8_t* data, size_t size) {
    return is_png(data, size) || looks_like_jpeg(data, size);
}

bool decode_texture(const uint8_t* data, size_t size, int max_side, DecodedImage& out,
                    std::string& error) {
    out = DecodedImage{};
    if (looks_like_jpeg(data, size)) return decode_jpeg(data, size, max_side, out, error);
    if (!is_png(data, size)) {
        error = "bilden är varken PNG eller JPEG";
        return false;
    }
    int w = 0, h = 0;
    if (!gfx::decodeImage(data, size, out.rgba, w, h)) {
        error = "PNG-bilden gick inte att avkoda";
        out = DecodedImage{};
        return false;
    }
    out.width = out.source_width = w;
    out.height = out.source_height = h;
    if (max_side > 0) shrink(out, max_side);
    return true;
}

glaze::Program repeat_program() { return make_program(kRepeatProgram, true); }

glaze::Program lit_program() { return make_program(kLitProgram, false); }

std::vector<float> lit_uniforms(const glaze::Lighting& light, bool repeat_u, bool repeat_v) {
    double d[3] = {-light.direction.x, -light.direction.y, -light.direction.z};
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len > 0)
        for (double& v : d) v /= len;
    std::vector<float> u = {static_cast<float>(d[0]), static_cast<float>(d[1]),
                            static_cast<float>(d[2]), 1.0f,
                            light.ambient,           light.diffuse,           light.exponent,
                            light.rim,
                            repeat_u ? 1.0f : 0.0f,  repeat_v ? 1.0f : 0.0f,  0.0f, 0.0f};
    u.resize(kUniformFloats, 0.0f);
    return u;
}

void set_clip_uniforms(std::vector<float>& uniforms, const ClipState& clip, const double origin[3]) {
    write_clip(uniforms, kClipOffset, kUniformFloats, clip, origin);
}

glaze::Program line_program() {
    glaze::Program p;
    p.name = kLineProgram;
    p.hlsl = line_hlsl();
    p.glslVertex = line_glsl_vertex();
    p.glslFragment = line_glsl_fragment();
    p.msl = line_msl();
    p.textures = 0;
    p.uniformFloats = static_cast<int>(kLineUniformFloats);
    return p;
}

std::vector<float> line_uniforms(float width_px) {
    std::vector<float> u(kLineUniformFloats, 0.0f);
    u[0] = width_px;
    return u;
}

void set_line_clip_uniforms(std::vector<float>& uniforms, const ClipState& clip,
                            const double origin[3]) {
    write_clip(uniforms, kLineClipOffset, kLineUniformFloats, clip, origin);
}

} // namespace nwdview
