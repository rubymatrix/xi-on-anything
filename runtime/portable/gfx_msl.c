/* Metal Shading Language for a draw's keys (gfx.h): D3D8's fixed-function pipeline - transform,
 * lighting, fog, texture coordinate generation and transforms, the texture stage cascade, alpha
 * test - as one vertex and one fragment function per key pair, and vs.1.x / ps.1.x shaders
 * translated (gfx_msl_shaders.c). Pure C: the Metal side (gfx_metal.m) compiles the text and caches
 * the result by key.
 *
 * The same text comes out as Vulkan GLSL (Sb.glsl, gfx_glsl_generate) for gfx_vulkan.c. Its prelude
 * defines MSL's type and function names (float4, saturate, ...), so the expressions are the same in
 * both; the dialects part only at signatures, the vertex function's outputs, vertex fetch, texture
 * sampling and vector compares. GLSL has no `in.` (a keyword): the fragment function's copy of its
 * inputs is `I`, and `in.` is rewritten to it at the end.
 *
 * And as WGSL (Sb.glsl 2, gfx_wgsl_generate) for gfx_webgpu.c. WGSL's functions are written the MSL
 * way (an entry point that returns its outputs), its prelude names MSL's types (float4 is vec4f), and
 * what C-like statements the emitters write - declarations, comma lists, one-line ifs - become WGSL in
 * one pass at the end (wgsl_fix). The few constructs WGSL lacks are written its own way where they
 * are emitted: ?: as select, writes to several components of a vector, vector clamps, vertex fetch.
 *
 * Conventions the functions follow:
 *   - matrices are D3D's bytes in a float4x4, so `m * v` is D3D's v * M;
 *   - D3D puts pixel centers on integer coordinates and Metal on half-integers: every clip-space
 *     position moves by half a pixel (the "position fixup");
 *   - vertex fetch is by hand from the stream buffers (buffers 0..3, stride and per-register offset
 *     in the uniforms), so one function serves any offsets and strides;
 *   - the uniforms are buffer 4 in both stages. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_msl.h"

_Static_assert(sizeof(GfxU) == 3536, "GfxU must match the MSL struct U");

void sb_printf(Sb* b, const char* fmt, ...)
{
    va_list ap;
    for (;;)
    {
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        int n = b->cap ? vsnprintf(b->s + b->len, room, fmt, ap) : -1;
        va_end(ap);
        if (n >= 0 && (size_t)n < room)
        {
            b->len += (size_t)n;
            return;
        }
        b->cap = b->cap ? b->cap * 2 : 8192;
        if (n >= 0 && b->cap < b->len + (size_t)n + 1)
            b->cap = b->len + (size_t)n + 1;
        b->s = (char*)realloc(b->s, b->cap);
    }
}

static const char PRELUDE[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Light { float4 diffuse, specular, ambient, pos, dir, att, spot; };\n"
    "struct U {\n"
    "  float4x4 wvp, wv, wvit;\n"
    "  float4x4 texm[8];\n"
    "  float4 mat_d, mat_a, mat_s, mat_e, params, ambient, tfactor, fogcolor, params2, vp;\n"
    "  int4 vofs, stride;\n"
    "  int4 offset[5];\n"
    "  Light light[8];\n"
    "  float4 vsc[96];\n"
    "  float4 psc[8];\n"
    "};\n"
    "static inline int reg_offset(constant U& u, int r) { return u.offset[r >> 2][r & 3]; }\n"
    "static inline float4 ld_color(device const uchar* p) { uchar4 c = *(device const uchar4*)p; return float4(c.z, c.y, c.x, c.w) / 255.0; }\n";

/* The GLSL prelude: MSL's names for GLSL's, the uniforms (binding 0), and how a vertex reads a word */
static const char GLSL_PRELUDE[] =
    "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n#define float4x4 mat4\n"
    "#define int4 ivec4\n#define uint2 uvec2\n"
    "#define saturate(x) clamp((x), 0.0, 1.0)\n#define rsqrt inversesqrt\n#define rint roundEven\n"
    "#define dfdx dFdx\n#define dfdy dFdy\n#define discard_fragment() discard\n"
    "#define INFINITY uintBitsToFloat(0x7f800000u)\n"
    "struct Light { vec4 diffuse, specular, ambient, pos, dir, att, spot; };\n"
    "layout(std140, set = 0, binding = 0) uniform UB {\n"
    "  mat4 wvp, wv, wvit;\n"
    "  mat4 texm[8];\n"
    "  vec4 mat_d, mat_a, mat_s, mat_e, params, ambient, tfactor, fogcolor, params2, vp;\n"
    "  ivec4 vofs, stride;\n"
    "  ivec4 offset[5];\n"
    "  Light light[8];\n"
    "  vec4 vsc[96];\n"
    "  vec4 psc[8];\n"
    "} u;\n"
    "#define reg_offset(uu, r) u.offset[(r) >> 2][(r) & 3]\n";

/* The WGSL prelude: MSL's names, the uniforms (binding 0), and the few functions WGSL names otherwise */
static const char WGSL_PRELUDE[] =
    "alias float2 = vec2f;\nalias float3 = vec3f;\nalias float4 = vec4f;\nalias float4x4 = mat4x4f;\n"
    "struct Light { diffuse: vec4f, specular: vec4f, ambient: vec4f, pos: vec4f, dir: vec4f, att: vec4f, spot: vec4f, };\n"
    "struct U {\n"
    "  wvp: mat4x4f, wv: mat4x4f, wvit: mat4x4f,\n"
    "  texm: array<mat4x4f, 8>,\n"
    "  mat_d: vec4f, mat_a: vec4f, mat_s: vec4f, mat_e: vec4f, params: vec4f, ambient: vec4f, tfactor: vec4f,\n"
    "  fogcolor: vec4f, params2: vec4f, vp: vec4f,\n"
    "  vofs: vec4i, stride: vec4i,\n"
    "  offset: array<vec4i, 5>,\n"
    "  light: array<Light, 8>,\n"
    "  vsc: array<vec4f, 96>,\n"
    "  psc: array<vec4f, 8>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "fn reg_offset(r: i32) -> i32 { return u.offset[r >> 2u][r & 3]; }\n"
    "fn rsqrt(x: f32) -> f32 { return inverseSqrt(x); }\n"
    "fn rint(x: f32) -> f32 { return round(x); }\n"
    "fn xinf() -> f32 { var b = 0x7f800000u; return bitcast<f32>(b); }\n";

#define WGSL(b) ((b)->glsl == 2)
#define GLSL(b) ((b)->glsl == 1)

/* The back end's water (GfxFsKey.water), after the game's own color and fog: the scene behind it
 * (wcol, a copy of the target made before the frame's first water draw) seen through the surface,
 * bent by ripples, turning to the game's color the deeper the water is (wdep, the depth copy); the
 * sky's color reflected at glancing angles; the sun's highlight; foam and a soft edge where it meets
 * the shore. 2 and 3 only soften the edge: of the alpha, and of the color too (additive blends). */
static const char WATER_MSL[] =
    "struct WU {\n"
    "  float4x4 iv, view; // view to world, world to view\n"
    "  float4 zp;     // P22, P32, viewport MinZ, MaxZ\n"
    "  float4 hand;   // P23, time (s), refraction (fraction of the target's height), foam\n"
    "  float4 size;   // target width, height, 1 / width, 1 / height\n"
    "  float4 sun;    // world, toward the light; w = 1 when there is one\n"
    "  float4 suncol; // its color; a = highlight strength\n"
    "  float4 sky;    // the sky's color (the fog's); a = reflection strength\n"
    "  float4 p;      // depth where the game's color takes over, soft edge depth, ripple strength, waves per unit\n"
    "  float4 p2;     // world up; w = foam depth\n"
    "};\n"
    "constexpr sampler wsamp(filter::linear, address::clamp_to_edge);\n"
    "static float water_z(constant WU& w, float d) {\n"
    "  d = (d - w.zp.z) / max(w.zp.w - w.zp.z, 1e-6);\n"
    "  if (d >= 0.999999) return 0.0;\n"
    "  float z = w.zp.y / (d * w.hand.x - w.zp.x);\n"
    "  return z * w.hand.x > 0.0 ? z : 0.0;\n"
    "}\n"
    /* the scene's distance behind the surface at pixel px, along the view axis (far for the sky) */
    "static float water_thick(constant WU& w, depth2d<float> dt, float2 px, float zw) {\n"
    "  float zs = water_z(w, dt.read(uint2(clamp(px, float2(0.0), w.size.xy - 1.0))));\n"
    "  return zs == 0.0 ? 1e4 : (zs - zw) * w.hand.x;\n"
    "}\n"
    /* a few waves across the world's ground plane: height, and its slope along x and z */
    "static float3 water_waves(constant WU& w, float2 q) {\n"
    "  const float2 dir[4] = { float2(0.96, 0.29), float2(-0.37, 0.93), float2(0.75, -0.66), float2(-0.98, -0.2) };\n"
    "  const float4 f = float4(1.0, 1.7, 2.9, 4.3), a = float4(0.5, 0.3, 0.18, 0.1), v = float4(1.1, 1.5, 2.0, 2.6);\n"
    "  float3 r = float3(0.0);\n"
    "  for (int i = 0; i < 4; ++i) {\n"
    "    float k = f[i] * w.p.w, ph = dot(dir[i], q) * k + w.hand.y * v[i];\n"
    "    r += float3(a[i] * sin(ph), a[i] * k * cos(ph) * dir[i]);\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "static float water_noise(float2 p) {\n"
    "  float2 i = floor(p), f = fract(p);\n"
    "  f = f * f * (3.0 - 2.0 * f);\n"
    "  float4 h = fract(sin(float4(dot(i, float2(127.1, 311.7)), dot(i + float2(1, 0), float2(127.1, 311.7)),\n"
    "    dot(i + float2(0, 1), float2(127.1, 311.7)), dot(i + float2(1, 1), float2(127.1, 311.7)))) * 43758.5453);\n"
    "  return mix(mix(h.x, h.y, f.x), mix(h.z, h.w, f.x), f.y);\n"
    "}\n";

/* The same in GLSL: the uniforms and textures are globals (bindings 16-18), which the functions read
 * directly; the macros keep the calls emit_water makes the same in both dialects. */
static const char WATER_GLSL[] =
    "layout(std140, set = 0, binding = 16) uniform WUB {\n"
    "  mat4 iv, view;\n"
    "  vec4 zp, hand, size, sun, suncol, sky, p, p2;\n"
    "} wu;\n"
    "layout(set = 0, binding = 17) uniform sampler2D wcol;\n"
    "layout(set = 0, binding = 18) uniform sampler2D wdep;\n"
    "float water_z(float d) {\n"
    "  d = (d - wu.zp.z) / max(wu.zp.w - wu.zp.z, 1e-6);\n"
    "  if (d >= 0.999999) return 0.0;\n"
    "  float z = wu.zp.y / (d * wu.hand.x - wu.zp.x);\n"
    "  return z * wu.hand.x > 0.0 ? z : 0.0;\n"
    "}\n"
    "float water_thick_(vec2 px, float zw) {\n"
    "  float zs = water_z(texelFetch(wdep, ivec2(clamp(px, vec2(0.0), wu.size.xy - 1.0)), 0).r);\n"
    "  return zs == 0.0 ? 1e4 : (zs - zw) * wu.hand.x;\n"
    "}\n"
    "#define water_thick(w, dt, px, zw) water_thick_(px, zw)\n"
    "vec3 water_waves_(vec2 q) {\n"
    "  const vec2 dir[4] = vec2[4](vec2(0.96, 0.29), vec2(-0.37, 0.93), vec2(0.75, -0.66), vec2(-0.98, -0.2));\n"
    "  const vec4 f = vec4(1.0, 1.7, 2.9, 4.3), a = vec4(0.5, 0.3, 0.18, 0.1), v = vec4(1.1, 1.5, 2.0, 2.6);\n"
    "  vec3 r = vec3(0.0);\n"
    "  for (int i = 0; i < 4; ++i) {\n"
    "    float k = f[i] * wu.p.w, ph = dot(dir[i], q) * k + wu.hand.y * v[i];\n"
    "    r += vec3(a[i] * sin(ph), a[i] * k * cos(ph) * dir[i]);\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "#define water_waves(w, q) water_waves_(q)\n"
    "float water_noise(vec2 p) {\n"
    "  vec2 i = floor(p), f = fract(p);\n"
    "  f = f * f * (3.0 - 2.0 * f);\n"
    "  vec4 h = fract(sin(vec4(dot(i, vec2(127.1, 311.7)), dot(i + vec2(1, 0), vec2(127.1, 311.7)),\n"
    "    dot(i + vec2(0, 1), vec2(127.1, 311.7)), dot(i + vec2(1, 1), vec2(127.1, 311.7)))) * 43758.5453);\n"
    "  return mix(mix(h.x, h.y, f.x), mix(h.z, h.w, f.x), f.y);\n"
    "}\n";

/* the water over col (the game's color, fogged: f the fog factor when there is fog) */
static void emit_water(Sb* b, const GfxFsKey* k, const char* col)
{
    sb_printf(b, "  {\n  float wfog = %s;\n  float2 px = in.pos.xy;\n  float zw = in.ez;\n"
                 "  float thick0 = water_thick(wu, wdep, px, zw);\n  float soft = smoothstep(0.0, wu.p.y, thick0);\n",
        k->fog ? "f" : "1.0");
    if (k->water != 1)
    {
        sb_printf(b, "  %s%s *= soft;\n  }\n", col, k->water == 3 ? "" : ".a");
        return;
    }
    sb_printf(b,
        "  float3 pw = (wu.iv * float4(in.pv, 1.0)).xyz;\n"
        "  float3 V = normalize((wu.iv * float4(0.0, 0.0, 0.0, 1.0)).xyz - pw);\n"
        "  float3 Ng = cross(dfdx(pw), dfdy(pw));\n"
        "  Ng = dot(Ng, Ng) > 1e-12 ? normalize(Ng) : wu.p2.xyz;\n"
        "  if (dot(Ng, V) < 0.0) Ng = -Ng;\n"
        /* ripples fade out with distance, before they shimmer */
        "  float3 wv = water_waves(wu, pw.xz);\n"
        "  float3 G = float3(wv.y, 0.0, wv.z) * (wu.p.z * saturate(1.0 - abs(zw) / 150.0));\n"
        "  float3 N = normalize(Ng - (G - dot(G, Ng) * Ng));\n"
        /* the scene behind, bent by the ripples - not where something in front of the water would show */
        "  float2 bend = (wu.view * float4(N - Ng, 0.0)).xy * float2(1.0, -1.0);\n"
        "  float2 pr = px + bend * (wu.hand.z * wu.size.y * saturate(thick0 * 0.5));\n"
        "  float thick = water_thick(wu, wdep, pr, zw);\n"
        "  if (thick < 0.0) { pr = px; thick = thick0; }\n"
        "  float3 behind = %s.rgb;\n"
        /* shallow: clear; deep: the game's water, no clearer than the game made it */
        "  float deep = 1.0 - exp(-max(thick, 0.0) / max(wu.p.x, 1e-3));\n"
        "  float3 c = mix(behind, %s.rgb, saturate(max(%s.a, deep)));\n"
        "  float fres = 0.02 + 0.98 * pow(1.0 - saturate(dot(N, V)), 5.0);\n"
        "  float3 R = reflect(-V, N);\n"
        "  float hz = 1.0 - saturate(abs(dot(R, wu.p2.xyz)));\n"
        "  c = mix(c, mix(wu.sky.rgb, wu.sky.rgb * 1.15 + 0.04, hz * hz), fres * wu.sky.a);\n"
        "  float3 H = normalize(wu.sun.xyz + V);\n"
        "  c += wu.suncol.rgb * (pow(saturate(dot(N, H)), 300.0) * wu.suncol.a * wu.sun.w * saturate(dot(Ng, wu.sun.xyz) * 4.0) * wfog);\n"
        /* foam where it is shallow, broken up */
        "  float edge = 1.0 - smoothstep(0.0, wu.p2.w, thick0);\n"
        "  float n = water_noise(pw.xz * 1.7 + float2(wu.hand.y * 0.3, 0.0)) * 0.6 + water_noise(pw.xz * 4.1 - wu.hand.y * 0.5) * 0.4;\n"
        "  float foam = saturate(edge * edge * wu.hand.w * smoothstep(0.35, 0.75, n + edge * 0.4)) * wfog;\n"
        "  c = mix(c, mix(%s.rgb, float3(1.0), 0.75), foam);\n"
        "  %s = float4(c, soft);\n  }\n",
        b->glsl ? "texture(wcol, pr * wu.size.zw)" : "wcol.sample(wsamp, pr * wu.size.zw)", col, col, col, col);
}

/* lit per pixel: the vertex function passes on what the lighting starts from (the normal, the
 * position and the material colors), and the fragment function lights - the same equations, but
 * a highlight no longer depends on where the vertices fall. pixel 1: the directional lights (the
 * sun, the moon) per pixel, the point and spot lights per vertex as the game made them - its torches
 * flicker, and a pool of light per pixel pulses with them; pixel 2: all of them per pixel. */
static int pixel_lit(const GfxVsKey* k) { return k->pixel && k->lighting && !k->rhw && !k->flat && !k->prog; }
static int point_per_vertex(const GfxVsKey* k)
{
    if (k->pixel != 1)
        return 0;
    for (int i = 0; i < k->nlights; ++i)
        if (k->light_type[i] != 3)
            return 1;
    return 0;
}

/* GLSL: what the vertex function passes on, as members (the interface block's and the local
 * struct's); pos and psize are gl_Position and gl_PointSize, apart */
static void glsl_varyings(Sb* b, const GfxVsKey* k, int block)
{
    const char* fl = block && k->flat ? "flat " : "";
    sb_printf(b, "  %svec4 d;\n  %svec4 s;\n", fl, fl);
    if (pixel_lit(k))
        sb_printf(b, "  vec3 n;\n  vec4 pe, md, ma, ms, me;\n%s", point_per_vertex(k) ? "  vec4 pa, pd, ps;\n" : "");
    for (int i = 0; i < k->ntex; ++i)
        sb_printf(b, "  vec4 t%d;\n", i);
    if (k->water)
        sb_printf(b, "  vec3 pv;\n");
    sb_printf(b, "  float fog;\n  float ez;\n");
}

/* GLSL: the local VOut both functions work on, and the interface block between them (vo / vi) */
static void glsl_vout(Sb* b, const GfxVsKey* k)
{
    sb_printf(b, "struct VOut {\n  vec4 pos;\n  float psize;\n");
    glsl_varyings(b, k, 0);
    sb_printf(b, "};\n#ifdef GFX_VS\nlayout(location = 0) out VOutB {\n");
    glsl_varyings(b, k, 1);
    sb_printf(b, "} vo;\n#else\nlayout(location = 0) in VOutB {\n");
    glsl_varyings(b, k, 1);
    sb_printf(b, "} vi;\n#endif\n");
}

/* GLSL: each member of the block copied between it and the local VOut */
static void glsl_copy_varyings(Sb* b, const GfxVsKey* k, const char* dst, const char* src)
{
    char names[32][4] = { "d", "s" };
    int n = 2;
    if (pixel_lit(k))
    {
        static const char* const pl[] = { "n", "pe", "md", "ma", "ms", "me" };
        for (int i = 0; i < 6; ++i)
            snprintf(names[n++], sizeof names[0], "%s", pl[i]);
        if (point_per_vertex(k))
            snprintf(names[n++], 4, "pa"), snprintf(names[n++], 4, "pd"), snprintf(names[n++], 4, "ps");
    }
    for (int i = 0; i < k->ntex; ++i)
        snprintf(names[n++], 4, "t%d", i);
    if (k->water)
        snprintf(names[n++], 4, "pv");
    snprintf(names[n++], 4, "fog");
    snprintf(names[n++], 4, "ez");
    for (int i = 0; i < n; ++i)
        sb_printf(b, "  %s.%s = %s.%s;\n", dst, names[i], src, names[i]);
}

/* WGSL: the vertex function's output, one location each (flat colors when the key says so) */
static void wgsl_vout(Sb* b, const GfxVsKey* k)
{
    const char* fl = k->flat ? " @interpolate(flat)" : "";
    int loc = 0;
    sb_printf(b, "struct VOut {\n  @builtin(position) pos: vec4f,\n");
    sb_printf(b, "  @location(%d)%s d: vec4f,\n  @location(%d)%s s: vec4f,\n", loc, fl, loc + 1, fl);
    loc += 2;
    if (pixel_lit(k))
    {
        static const char* const pl[] = { "n: vec3f", "pe: vec4f", "md: vec4f", "ma: vec4f", "ms: vec4f", "me: vec4f",
            "pa: vec4f", "pd: vec4f", "ps: vec4f" };
        for (int i = 0; i < (point_per_vertex(k) ? 9 : 6); ++i)
            sb_printf(b, "  @location(%d) %s,\n", loc++, pl[i]);
    }
    for (int i = 0; i < k->ntex; ++i)
        sb_printf(b, "  @location(%d) t%d: vec4f,\n", loc++, i);
    if (k->water)
        sb_printf(b, "  @location(%d) pv: vec3f,\n", loc++);
    sb_printf(b, "  @location(%d) fog: f32,\n  @location(%d) ez: f32,\n};\n", loc, loc + 1);
}

/* the vertex function's output: what the fragment function reads */
static void emit_vout(Sb* b, const GfxVsKey* k)
{
    if (WGSL(b))
    {
        wgsl_vout(b, k);
        return;
    }
    if (b->glsl)
    {
        glsl_vout(b, k);
        return;
    }
    int ntex = k->ntex;
    const char* fl = k->flat ? " [[flat]]" : "";
    sb_printf(b, "struct VOut {\n  float4 pos [[position]];\n  float4 d [[user(d)]]%s;\n  float4 s [[user(s)]]%s;\n", fl, fl);
    if (pixel_lit(k))
        sb_printf(b, "  float3 n [[user(n)]];\n  float4 pe [[user(pe)]];\n  float4 md [[user(md)]];\n  float4 ma [[user(ma)]];\n"
                     "  float4 ms [[user(ms)]];\n  float4 me [[user(me)]];\n%s",
            point_per_vertex(k) ? "  float4 pa [[user(pa)]];\n  float4 pd [[user(pd)]];\n  float4 ps [[user(ps)]];\n" : "");
    for (int i = 0; i < ntex; ++i)
        sb_printf(b, "  float4 t%d [[user(t%d)]];\n", i, i);
    if (k->water)
        sb_printf(b, "  float3 pv [[user(pv)]];\n");
    sb_printf(b, "  float fog [[user(fog)]];\n  float ez [[user(ez)]];\n  float psize [[point_size]];\n};\n");
}

/* GLSL: v# for an element, from the words of its stream (D3D8's elements and strides are whole words) */
static void glsl_fetch(Sb* b, const GfxElem* e, int reg)
{
    sb_printf(b, "  int p%d = (vi * u.stride[%d] + reg_offset(u, %d)) >> 2;\n", reg, e->stream, reg);
    int s = e->stream;
    switch (e->type)
    {
    case GFX_FLOAT1: sb_printf(b, "  float4 v%d = float4(uintBitsToFloat(s%d.w[p%d]), 0, 0, 1);\n", reg, s, reg); break;
    case GFX_FLOAT2:
        sb_printf(b, "  float4 v%d = float4(uintBitsToFloat(uvec2(s%d.w[p%d], s%d.w[p%d + 1])), 0, 1);\n", reg, s, reg, s, reg);
        break;
    case GFX_FLOAT3:
        sb_printf(b, "  float4 v%d = float4(uintBitsToFloat(uvec3(s%d.w[p%d], s%d.w[p%d + 1], s%d.w[p%d + 2])), 1);\n", reg, s, reg,
            s, reg, s, reg);
        break;
    case GFX_FLOAT4:
        sb_printf(b, "  float4 v%d = uintBitsToFloat(uvec4(s%d.w[p%d], s%d.w[p%d + 1], s%d.w[p%d + 2], s%d.w[p%d + 3]));\n", reg, s,
            reg, s, reg, s, reg, s, reg);
        break;
    case GFX_D3DCOLOR: sb_printf(b, "  float4 v%d = unpackUnorm4x8(s%d.w[p%d]).zyxw;\n", reg, s, reg); break;
    case GFX_UBYTE4:
        sb_printf(b, "  uint w%d = s%d.w[p%d];\n  float4 v%d = float4(uvec4(w%d, w%d >> 8, w%d >> 16, w%d >> 24) & 0xFFu);\n", reg, s,
            reg, reg, reg, reg, reg, reg);
        break;
    case GFX_SHORT2:
        sb_printf(b, "  int w%d = int(s%d.w[p%d]);\n  float4 v%d = float4(float((w%d << 16) >> 16), float(w%d >> 16), 0, 1);\n", reg,
            s, reg, reg, reg, reg);
        break;
    case GFX_SHORT4:
        sb_printf(b,
            "  int w%d = int(s%d.w[p%d]), x%d = int(s%d.w[p%d + 1]);\n"
            "  float4 v%d = float4(float((w%d << 16) >> 16), float(w%d >> 16), float((x%d << 16) >> 16), float(x%d >> 16));\n",
            reg, s, reg, reg, s, reg, reg, reg, reg, reg, reg);
        break;
    default: sb_printf(b, "  float4 v%d = float4(0, 0, 0, 1);\n", reg); break;
    }
}

/* WGSL: the same from words, with WGSL's explicit integer types (shifts by u32, bitcasts) */
static void wgsl_fetch(Sb* b, const GfxElem* e, int reg)
{
    int s = e->stream;
    sb_printf(b, "  var p%d: i32 = (vi * u.stride[%d] + reg_offset(%d)) >> 2u;\n", reg, s, reg);
    switch (e->type)
    {
    case GFX_FLOAT1: sb_printf(b, "  var v%d: vec4f = vec4f(bitcast<f32>(s%d.w[p%d]), 0.0, 0.0, 1.0);\n", reg, s, reg); break;
    case GFX_FLOAT2:
        sb_printf(b, "  var v%d: vec4f = vec4f(bitcast<vec2f>(vec2u(s%d.w[p%d], s%d.w[p%d + 1])), 0.0, 1.0);\n", reg, s, reg, s, reg);
        break;
    case GFX_FLOAT3:
        sb_printf(b, "  var v%d: vec4f = vec4f(bitcast<vec3f>(vec3u(s%d.w[p%d], s%d.w[p%d + 1], s%d.w[p%d + 2])), 1.0);\n", reg, s,
            reg, s, reg, s, reg);
        break;
    case GFX_FLOAT4:
        sb_printf(b, "  var v%d: vec4f = bitcast<vec4f>(vec4u(s%d.w[p%d], s%d.w[p%d + 1], s%d.w[p%d + 2], s%d.w[p%d + 3]));\n", reg,
            s, reg, s, reg, s, reg, s, reg);
        break;
    case GFX_D3DCOLOR: sb_printf(b, "  var v%d: vec4f = unpack4x8unorm(s%d.w[p%d]).zyxw;\n", reg, s, reg); break;
    case GFX_UBYTE4:
        sb_printf(b, "  var w%d: u32 = s%d.w[p%d];\n  var v%d: vec4f = vec4f(vec4u(w%d, w%d >> 8u, w%d >> 16u, w%d >> 24u) & vec4u(0xFFu));\n",
            reg, s, reg, reg, reg, reg, reg, reg);
        break;
    case GFX_SHORT2:
        sb_printf(b, "  var w%d: i32 = bitcast<i32>(s%d.w[p%d]);\n  var v%d: vec4f = vec4f(f32((w%d << 16u) >> 16u), f32(w%d >> 16u), 0.0, 1.0);\n",
            reg, s, reg, reg, reg, reg);
        break;
    case GFX_SHORT4:
        sb_printf(b,
            "  var w%d: i32 = bitcast<i32>(s%d.w[p%d]);\n  var x%d: i32 = bitcast<i32>(s%d.w[p%d + 1]);\n"
            "  var v%d: vec4f = vec4f(f32((w%d << 16u) >> 16u), f32(w%d >> 16u), f32((x%d << 16u) >> 16u), f32(x%d >> 16u));\n",
            reg, s, reg, reg, s, reg, reg, reg, reg, reg, reg);
        break;
    default: sb_printf(b, "  var v%d: vec4f = vec4f(0.0, 0.0, 0.0, 1.0);\n", reg); break;
    }
}

/* v# for an element: a float4 read from its stream */
static void emit_fetch(Sb* b, const GfxVsKey* k, int reg)
{
    const GfxElem* e = &k->el[reg];
    if (!e->used)
    {
        sb_printf(b, "  float4 v%d = float4(0, 0, 0, 1);\n", reg);
        return;
    }
    if (WGSL(b))
    {
        wgsl_fetch(b, e, reg);
        return;
    }
    if (b->glsl)
    {
        glsl_fetch(b, e, reg);
        return;
    }
    sb_printf(b, "  device const uchar* p%d = s%d + vi * u.stride[%d] + reg_offset(u, %d);\n", reg, e->stream, e->stream, reg);
    switch (e->type)
    {
    case GFX_FLOAT1: sb_printf(b, "  float4 v%d = float4(((device const float*)p%d)[0], 0, 0, 1);\n", reg, reg); break;
    case GFX_FLOAT2:
        sb_printf(b, "  float4 v%d = float4(((device const float*)p%d)[0], ((device const float*)p%d)[1], 0, 1);\n", reg, reg, reg);
        break;
    case GFX_FLOAT3:
        sb_printf(b, "  float4 v%d = float4(((device const float*)p%d)[0], ((device const float*)p%d)[1], ((device const float*)p%d)[2], 1);\n",
            reg, reg, reg, reg);
        break;
    case GFX_FLOAT4:
        sb_printf(b, "  float4 v%d = float4(((device const float*)p%d)[0], ((device const float*)p%d)[1], ((device const float*)p%d)[2], ((device const float*)p%d)[3]);\n",
            reg, reg, reg, reg, reg);
        break;
    case GFX_D3DCOLOR: sb_printf(b, "  float4 v%d = ld_color(p%d);\n", reg, reg); break;
    case GFX_UBYTE4: sb_printf(b, "  float4 v%d = float4(*(device const uchar4*)p%d);\n", reg, reg); break;
    case GFX_SHORT2: sb_printf(b, "  float4 v%d = float4(float2(*(device const short2*)p%d), 0, 1);\n", reg, reg); break;
    case GFX_SHORT4: sb_printf(b, "  float4 v%d = float4(*(device const short4*)p%d);\n", reg, reg); break;
    default: sb_printf(b, "  float4 v%d = float4(0, 0, 0, 1);\n", reg); break;
    }
}

static int elem_components(uint8_t type)
{
    switch (type)
    {
    case GFX_FLOAT1: return 1;
    case GFX_FLOAT2:
    case GFX_SHORT2: return 2;
    case GFX_FLOAT3: return 3;
    default: return 4;
    }
}

/* drawn from the sun: whatever clip-space position the function makes (the camera's, from the
 * transforms or a vertex shader's constants) goes on through the camera's inverse into the sun's
 * view - one matrix, so any draw of the scene can be drawn again into the shadow map */
void gfx_msl_vs_params(Sb* b, const GfxVsKey* k)
{
    if (k->shadow && !b->glsl) /* GLSL: a global (binding 5) */
        sb_printf(b, ", constant float4x4& sm [[buffer(5)]]");
}

void gfx_msl_vs_return(Sb* b, const GfxVsKey* k)
{
    if (k->shadow) /* D3D's pixel-centre fixup undone (the map has pixels of its own), then on into the map */
        sb_printf(b, "  o.pos.x -= o.pos.w / u.vp.z;\n  o.pos.y += o.pos.w / u.vp.w;\n  o.pos = sm * o.pos;\n");
    if (GLSL(b))
    {
        sb_printf(b, "  gl_Position = o.pos;\n  gl_PointSize = o.psize;\n");
        glsl_copy_varyings(b, k, "vo", "o");
        sb_printf(b, "}\n");
        return;
    }
    sb_printf(b, "  return o;\n}\n");
}

static void emit_vs_signature(Sb* b, const GfxVsKey* k)
{
    if (WGSL(b)) /* the streams and the shadow matrix are module bindings (gfx_wgsl_generate) */
    {
        sb_printf(b, "@vertex fn vs_main(@builtin(vertex_index) vid: u32) -> VOut {\n  var o: VOut;\n"
                     "  var vi: i32 = i32(vid) + u.vofs.x;\n");
        return;
    }
    if (b->glsl)
    {
        sb_printf(b, "void main() {\n  VOut o;\n  int vi = gl_VertexIndex + u.vofs.x;\n  o.psize = 1.0;\n");
        return;
    }
    sb_printf(b, "vertex VOut vs_main(uint vid [[vertex_id]], constant U& u [[buffer(4)]]");
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        sb_printf(b, ", device const uchar* s%d [[buffer(%d)]]", s, s);
    gfx_msl_vs_params(b, k);
    sb_printf(b, ") {\n  VOut o;\n  int vi = int(vid) + u.vofs.x;\n  o.psize = 1.0;\n");
}

/* the vertex function's start and every v# it reads (the fixed-function one and vs.1.x's) */
void gfx_msl_vs_begin(Sb* b, const GfxVsKey* k)
{
    emit_vs_signature(b, k);
    for (int r = 0; r < GFX_NREGS; ++r) /* unused registers are constants the compiler drops */
        emit_fetch(b, k, r);
}

/* clip-space position fixup: D3D's pixel centers onto Metal's */
static void emit_fixup(Sb* b)
{
    sb_printf(b, "  o.pos.x += o.pos.w / u.vp.z;\n  o.pos.y -= o.pos.w / u.vp.w;\n");
}

/* a D3DMATERIALCOLORSOURCE: the vertex color when the vertex has it, else the material's */
static const char* mcs(const GfxVsKey* k, int src, const char* mat)
{
    return src == 1 && k->el[GFX_R_DIFFUSE].used ? "v5" : src == 2 && k->el[GFX_R_SPECULAR].used ? "v6" : mat;
}

static void emit_fog_factor(Sb* b, const char* dst, int mode, const char* dist)
{
    switch (mode)
    {
    case 1: sb_printf(b, "  %s = saturate(exp(-u.params2.x * %s));\n", dst, dist); break;
    case 2: sb_printf(b, "  %s = saturate(exp(-(u.params2.x * %s) * (u.params2.x * %s)));\n", dst, dist, dist); break;
    case 3: sb_printf(b, "  %s = saturate((u.params.w - %s) / (u.params.w - u.params.z));\n", dst, dist); break;
    default: sb_printf(b, "  %s = 1.0;\n", dst); break;
    }
}

/* D3D's lighting from N and pe (view space) and the material colors cd, ca, cs, ce in scope, into
 * lit_d and lit_s */
/* which: 0 every light; 1 the directional ones, with the rest's terms from the vertex function
 * (in.pa, pd, ps); 2 the rest alone, leaving amb, dif and spc for it to pass on */
static void emit_lighting(Sb* b, const GfxVsKey* k, int pixel, int which)
{
    if (which == 2)
        sb_printf(b, "  float3 amb = float3(0), dif = float3(0), spc = float3(0);\n");
    else if (which == 1)
        sb_printf(b, "  float3 amb = u.ambient.rgb + in.pa.rgb, dif = in.pd.rgb, spc = in.ps.rgb;\n");
    else
        sb_printf(b, "  float3 amb = u.ambient.rgb, dif = float3(0), spc = float3(0);\n");
    if (k->specular)
        sb_printf(b, "  float3 V = %s;\n", k->localviewer ? "normalize(-pe)" : "float3(0, 0, -1)");
    for (int i = 0; i < k->nlights; ++i)
    {
        int t = k->light_type[i];
        if ((which == 1 && t != 3) || (which == 2 && t == 3))
            continue;
        sb_printf(b, WGSL(b) ? "  {\n    let L = u.light[%d];\n" : b->glsl ? "  {\n    Light L = u.light[%d];\n" : "  {\n    constant Light& L = u.light[%d];\n", i);
        if (t == 3)
            sb_printf(b, "    float3 l = L.dir.xyz;\n    float a = 1.0;\n");
        else
        {
            sb_printf(b, "    float3 lv = L.pos.xyz - pe;\n    float d = length(lv);\n    float3 l = lv / max(d, 1e-20);\n");
            if (WGSL(b))
                sb_printf(b, "    float a = select(1.0 / max(L.att.x + L.att.y * d + L.att.z * d * d, 1e-20), 0.0, d > L.pos.w);\n");
            else
                sb_printf(b, "    float a = d > L.pos.w ? 0.0 : 1.0 / max(L.att.x + L.att.y * d + L.att.z * d * d, 1e-20);\n");
            /* per pixel, D3D's hard edge at the range is a ring, and its attenuation near the light (far
             * over 1, where no vertex ever was) a white spot - both flash as the game's lights flicker
             * and move: the last quarter of the range fades, and the light is at most its colour */
            if (pixel)
                sb_printf(b, "    a = min(a, 1.0) * saturate((L.pos.w - d) / max(0.25 * L.pos.w, 1e-6));\n");
            if (t == 2 && WGSL(b))
                sb_printf(b,
                    "    float rho = dot(-l, L.dir.xyz);\n"
                    "    a *= select(select(pow(saturate((rho - L.spot.y) / (L.spot.x - L.spot.y)), L.dir.w), 0.0, rho <= L.spot.y), 1.0, rho > L.spot.x);\n");
            else if (t == 2)
                sb_printf(b,
                    "    float rho = dot(-l, L.dir.xyz);\n"
                    "    a *= rho > L.spot.x ? 1.0 : rho <= L.spot.y ? 0.0 : pow(saturate((rho - L.spot.y) / (L.spot.x - L.spot.y)), L.dir.w);\n");
        }
        sb_printf(b, "    amb += L.ambient.rgb * a;\n    float ndl = max(dot(N, l), 0.0);\n    dif += L.diffuse.rgb * (ndl * a);\n");
        if (k->specular)
            sb_printf(b, "    if (ndl > 0.0) spc += L.specular.rgb * (pow(max(dot(N, normalize(V + l)), 0.0), u.params.x) * a);\n");
        sb_printf(b, "  }\n");
    }
    if (which != 2)
        sb_printf(b,
            "  float4 lit_d = saturate(float4(ce.rgb + ca.rgb * amb + cd.rgb * dif, cd.a));\n"
            "  float4 lit_s = saturate(float4(cs.rgb * spc, cs.a));\n");
}

static void emit_ff_vs(Sb* b, const GfxVsKey* k)
{
    gfx_msl_vs_begin(b, k);
    if (k->rhw)
    {
        sb_printf(b, WGSL(b) ? "  float w = select(1.0, 1.0 / v0.w, v0.w != 0.0);\n" : "  float w = v0.w != 0.0 ? 1.0 / v0.w : 1.0;\n");
        sb_printf(b,
            "  float2 ndc = float2((v0.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (v0.y - u.vp.y) / u.vp.w * 2.0);\n"
            "  o.pos = float4(ndc * w, v0.z * w, w);\n"
            "  float3 pe = float3(0, 0, w);\n"
            "  o.ez = w;\n");
    }
    else
    {
        sb_printf(b,
            "  float4 P = float4(v0.xyz, 1.0);\n"
            "  o.pos = u.wvp * P;\n"
            "  float3 pe = (u.wv * P).xyz;\n"
            "  o.ez = pe.z;\n");
    }
    emit_fixup(b);
    if (k->water)
        sb_printf(b, "  o.pv = pe;\n");
    int need_normal = k->lighting || 0;
    for (int i = 0; i < k->ntex; ++i)
        need_normal |= (k->tci[i] >> 4) == 1 || (k->tci[i] >> 4) == 3;
    if (need_normal && !k->rhw)
    {
        sb_printf(b, "  float3 N = (u.wvit * float4(v3.xyz, 0.0)).xyz;\n");
        if (k->normalize)
            sb_printf(b, "  N = normalize(N);\n");
    }
    else
        sb_printf(b, "  float3 N = float3(0, 0, 1);\n");

    const char* dif_in = k->el[GFX_R_DIFFUSE].used ? "v5" : "float4(1.0)";
    const char* spe_in = k->el[GFX_R_SPECULAR].used ? "v6" : "float4(0.0)";
    if (k->lighting && !k->rhw)
    {
        sb_printf(b, "  float4 cd = %s, ca = %s, cs = %s, ce = %s;\n", mcs(k, k->src_diffuse, "u.mat_d"),
            mcs(k, k->src_ambient, "u.mat_a"), mcs(k, k->src_specular, "u.mat_s"), mcs(k, k->src_emissive, "u.mat_e"));
        if (pixel_lit(k)) /* the length of N too: without NORMALIZENORMALS a scaled one lights more */
        {
            sb_printf(b, "  o.n = N;\n  o.pe = float4(pe, length(N));\n  o.md = cd, o.ma = ca, o.ms = cs, o.me = ce;\n"
                         "  o.d = cd;\n  o.s = cs;\n");
            if (point_per_vertex(k))
            {
                sb_printf(b, "  {\n");
                emit_lighting(b, k, 0, 2);
                sb_printf(b, "  o.pa = float4(amb, 0), o.pd = float4(dif, 0), o.ps = float4(spc, 0);\n  }\n");
            }
        }
        else
        {
            emit_lighting(b, k, 0, 0);
            sb_printf(b, "  o.d = lit_d;\n  o.s = lit_s;\n");
        }
    }
    else
        sb_printf(b, "  o.d = %s;\n  o.s = %s;\n", dif_in, spe_in);

    if (k->fog_vertex && !k->rhw)
    {
        sb_printf(b, "  float fd = %s;\n", k->range_fog ? "length(pe)" : "abs(pe.z)");
        emit_fog_factor(b, "o.fog", k->fog_vertex, "fd");
    }
    else
        sb_printf(b, "  o.fog = %s.a;\n", k->el[GFX_R_SPECULAR].used ? "v6" : "float4(1.0)");

    for (int i = 0; i < k->ntex; ++i)
    {
        int idx = k->tci[i] & 7, gen = k->tci[i] >> 4, count = k->ttf[i] & 7;
        if (k->rhw)
            gen = 0;
        switch (gen)
        {
        case 1: sb_printf(b, "  float4 c%d = float4(N, 1.0);\n", i); break;
        case 2: sb_printf(b, "  float4 c%d = float4(pe, 1.0);\n", i); break;
        case 3: sb_printf(b, "  float4 c%d = float4(reflect(%s, N), 1.0);\n", i, k->localviewer ? "normalize(pe)" : "float3(0, 0, 1)"); break;
        default:
        {
            int reg = GFX_R_TEXCOORD0 + idx;
            if (!k->el[reg].used)
                sb_printf(b, "  float4 c%d = float4(0, 0, 0, 1);\n", i);
            else if (count)
            {
                /* D3D fills the component after the last one given with 1, which is how a 2D
                 * texture matrix carries its translation in the third row */
                int n = elem_components(k->el[reg].type);
                char y[16] = "1.0", z[16] = "1.0", w[16] = "1.0";
                if (n > 1)
                    snprintf(y, sizeof y, "v%d.y", reg);
                if (n > 2)
                    snprintf(z, sizeof z, "v%d.z", reg);
                if (n > 3)
                    snprintf(w, sizeof w, "v%d.w", reg);
                sb_printf(b, "  float4 c%d = float4(v%d.x, %s, %s, %s);\n", i, reg, y, z, w);
            }
            else
                sb_printf(b, "  float4 c%d = v%d;\n", i, reg);
            break;
        }
        }
        if (count && !k->rhw)
            sb_printf(b, "  c%d = u.texm[%d] * c%d;\n", i, i, i);
        sb_printf(b, "  o.t%d = c%d;\n", i, i);
    }
    gfx_msl_vs_return(b, k);
}

/* --- fragment: the texture stage cascade ------------------------------------------------------------ */
static void arg_expr(char* out, size_t n, int a)
{
    const char* base;
    switch (a & 0xF)
    {
    case 0: base = "in.d"; break;
    case 1: base = "cur"; break;
    case 2: base = "tex"; break;
    case 3: base = "u.tfactor"; break;
    case 4: base = "in.s"; break;
    case 5: base = "tmp"; break;
    default: base = "cur"; break;
    }
    char v[64];
    if (a & 0x20)
        snprintf(v, sizeof v, "%s.aaaa", base);
    else
        snprintf(v, sizeof v, "%s", base);
    if (a & 0x10)
        snprintf(out, n, "(1.0 - %s)", v);
    else
        snprintf(out, n, "%s", v);
}

/* D3DTEXTUREOP on float4 arguments; the caller keeps .rgb or .a */
static void op_expr(char* out, size_t n, int op, const char* a1, const char* a2, const char* a0)
{
    switch (op)
    {
    case 2: snprintf(out, n, "%s", a1); break;
    case 3: snprintf(out, n, "%s", a2); break;
    case 4: snprintf(out, n, "(%s * %s)", a1, a2); break;
    case 5: snprintf(out, n, "(%s * %s * 2.0)", a1, a2); break;
    case 6: snprintf(out, n, "(%s * %s * 4.0)", a1, a2); break;
    case 7: snprintf(out, n, "(%s + %s)", a1, a2); break;
    case 8: snprintf(out, n, "(%s + %s - 0.5)", a1, a2); break;
    case 9: snprintf(out, n, "((%s + %s - 0.5) * 2.0)", a1, a2); break;
    case 10: snprintf(out, n, "(%s - %s)", a1, a2); break;
    case 11: snprintf(out, n, "(%s + %s - %s * %s)", a1, a2, a1, a2); break;
    case 12: snprintf(out, n, "mix(%s, %s, in.d.a)", a2, a1); break;
    case 13: snprintf(out, n, "mix(%s, %s, tex.a)", a2, a1); break;
    case 14: snprintf(out, n, "mix(%s, %s, u.tfactor.a)", a2, a1); break;
    case 15: snprintf(out, n, "(%s + %s * (1.0 - tex.a))", a1, a2); break;
    case 16: snprintf(out, n, "mix(%s, %s, cur.a)", a2, a1); break;
    case 17: snprintf(out, n, "%s", a1); break; /* PREMODULATE: the next stage's texture is not known here */
    case 18: snprintf(out, n, "(%s + %s.a * %s)", a1, a1, a2); break;
    case 19: snprintf(out, n, "(%s * %s + %s.a)", a1, a2, a1); break;
    case 20: snprintf(out, n, "((1.0 - %s.a) * %s + %s)", a1, a2, a1); break;
    case 21: snprintf(out, n, "((1.0 - %s) * %s + %s.a)", a1, a2, a1); break;
    case 24: snprintf(out, n, "float4(saturate(dot((%s.rgb - 0.5) * 2.0, (%s.rgb - 0.5) * 2.0)))", a1, a2); break;
    case 25: snprintf(out, n, "(%s + %s * %s)", a0, a1, a2); break;
    case 26: snprintf(out, n, "(%s * %s + (1.0 - %s) * %s)", a0, a1, a0, a2); break;
    default: snprintf(out, n, "%s", a1); break; /* the bump ops: the environment map is not emulated */
    }
}

/* a texture stage sampled at coord (the stage's own texture and sampler) */
void gfx_msl_sample(char* out, size_t n, const Sb* b, unsigned stage, const char* coord)
{
    if (WGSL(b))
        snprintf(out, n, "textureSample(tx%u, sp%u, %s)", stage, stage, coord);
    else if (b->glsl)
        snprintf(out, n, "texture(tx%u, %s)", stage, coord);
    else
        snprintf(out, n, "tx%u.sample(sp%u, %s)", stage, stage, coord);
}

/* per component: x op y ? bv : a (op one of < <= > >= == !=) */
void gfx_msl_select(char* out, size_t n, const Sb* b, const char* a, const char* bv, const char* x, const char* op, const char* y)
{
    if (!GLSL(b)) /* MSL's select and WGSL's take the same arguments */
    {
        snprintf(out, n, "select(%s, %s, %s %s %s)", a, bv, x, op, y);
        return;
    }
    const char* fn = !strcmp(op, "<") ? "lessThan" : !strcmp(op, "<=") ? "lessThanEqual" : !strcmp(op, ">") ? "greaterThan"
        : !strcmp(op, ">=") ? "greaterThanEqual" : !strcmp(op, "==") ? "equal" : "notEqual";
    snprintf(out, n, "mix(%s, %s, %s(%s, %s))", a, bv, fn, x, y);
}

static void emit_fs_signature(Sb* b, const GfxFsKey* k, const GfxVsKey* vk)
{
    int pix = pixel_lit(vk);
    if (WGSL(b)) /* the textures are module bindings; the inputs are copied into I, which `in.` becomes */
        sb_printf(b, "@fragment fn fs_main(vin: VOut) -> @location(0) vec4f {\n  var I = vin;\n");
    else if (b->glsl) /* the textures are globals (generate); the inputs are copied into I, which `in.` becomes */
    {
        sb_printf(b, "void main() {\n  VOut I;\n  I.pos = gl_FragCoord;\n  I.psize = 1.0;\n");
        glsl_copy_varyings(b, vk, "I", "vi");
    }
    else
    {
        sb_printf(b, "fragment float4 fs_main(VOut %s [[stage_in]], constant U& u [[buffer(4)]]", pix ? "vin" : "in");
        for (int i = 0; i < 8; ++i)
        {
            int t = k->prog || i < k->nstages ? k->st[i].tex : 0;
            if (t == 1)
                sb_printf(b, ", texture2d<float> tx%d [[texture(%d)]], sampler sp%d [[sampler(%d)]]", i, i, i, i);
            else if (t == 2)
                sb_printf(b, ", texturecube<float> tx%d [[texture(%d)]], sampler sp%d [[sampler(%d)]]", i, i, i, i);
        }
        if (k->water)
            sb_printf(b, ", texture2d<float> wcol [[texture(8)]], depth2d<float> wdep [[texture(9)]], constant WU& wu [[buffer(5)]]");
        sb_printf(b, ") {\n%s", pix ? "  VOut in = vin;\n" : "");
    }
    if (pix) /* the colors the vertex function would have given, lit here */
    {
        sb_printf(b, "  {\n  float3 N = in.n * (rsqrt(max(dot(in.n, in.n), 1e-20)) * %s);\n"
                     "  float3 pe = in.pe.xyz;\n  float4 cd = in.md, ca = in.ma, cs = in.ms, ce = in.me;\n",
            vk->normalize ? "1.0" : "in.pe.w");
        emit_lighting(b, vk, 1, point_per_vertex(vk) ? 1 : 0);
        sb_printf(b, "  in.d = lit_d, in.s = lit_s;\n  }\n");
    }
}

static void emit_fs_tail(Sb* b, const GfxFsKey* k, const char* col)
{
    if (k->alpha_func && k->alpha_func != 8)
    {
        static const char* const cmp[] = { "", "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        if (k->alpha_func == 1)
            sb_printf(b, "  discard_fragment();\n");
        else if (k->alpha_func < 8)
            sb_printf(b, "  if (!(rint(saturate(%s.a) * 255.0) %s u.params.y)) discard_fragment();\n", col, cmp[k->alpha_func]);
    }
    if (k->fog)
    {
        if (k->fog == 4)
            sb_printf(b, "  float f = saturate(in.fog);\n");
        else
        {
            sb_printf(b, "  float f;\n  float fd = abs(in.ez);\n");
            emit_fog_factor(b, "f", k->fog, "fd");
        }
        if (WGSL(b))
            sb_printf(b, "  %s = float4(mix(u.fogcolor.rgb, %s.rgb, f), %s.a);\n", col, col, col);
        else
            sb_printf(b, "  %s.rgb = mix(u.fogcolor.rgb, %s.rgb, f);\n", col, col);
    }
    if (k->water && !WGSL(b))
        emit_water(b, k, col);
    sb_printf(b, GLSL(b) ? "  oc = %s;\n}\n" : "  return %s;\n}\n", col);
}

static void emit_ff_fs(Sb* b, const GfxFsKey* k, const GfxVsKey* vk)
{
    emit_fs_signature(b, k, vk);
    sb_printf(b, "  float4 cur = in.d, tmp = float4(0), tex = float4(1);\n");
    for (int i = 0; i < k->nstages; ++i)
    {
        const GfxStage* s = &k->st[i];
        char coord[64], smp[128];
        if (s->tex == 1)
        {
            if (s->projected)
                snprintf(coord, sizeof coord, "in.t%d.xy / in.t%d.%s", i, i, s->ncoord == 3 ? "z" : s->ncoord == 4 ? "w" : "y");
            else
                snprintf(coord, sizeof coord, "in.t%d.xy", i);
        }
        else
            snprintf(coord, sizeof coord, "in.t%d.xyz", i);
        gfx_msl_sample(smp, sizeof smp, b, (unsigned)i, coord);
        if (s->tex)
            sb_printf(b, "  tex = %s;\n", smp);
        else
            sb_printf(b, "  tex = float4(1);\n");
        char a1[64], a2[64], a0[64], ce[512], ae[512];
        arg_expr(a1, sizeof a1, s->ca1);
        arg_expr(a2, sizeof a2, s->ca2);
        arg_expr(a0, sizeof a0, s->ca0);
        op_expr(ce, sizeof ce, s->cop, a1, a2, a0);
        const char* dst = s->result == 5 ? "tmp" : "cur";
        if (s->cop == 24) /* DOTPRODUCT3 goes to every channel, alpha too */
        {
            sb_printf(b, "  %s = saturate(%s);\n", dst, ce);
            continue;
        }
        if (s->aop > 1)
        {
            arg_expr(a1, sizeof a1, s->aa1);
            arg_expr(a2, sizeof a2, s->aa2);
            arg_expr(a0, sizeof a0, s->aa0);
            op_expr(ae, sizeof ae, s->aop, a1, a2, a0);
            sb_printf(b, "  { float3 c = saturate(%s.rgb); float a = saturate(%s.a); %s = float4(c, a); }\n", ce, ae, dst);
        }
        else /* alpha disabled: the alpha carries on unchanged */
            sb_printf(b, "  { float3 c = saturate(%s.rgb); %s = float4(c, %s.a); }\n", ce, dst, i ? "cur" : "in.d");
    }
    if (k->specular_add)
        sb_printf(b, WGSL(b) ? "  cur = float4(saturate(cur.rgb + in.s.rgb), cur.a);\n" : "  cur.rgb = saturate(cur.rgb + in.s.rgb);\n");
    emit_fs_tail(b, k, "cur");
}

/* GLSL: `in.` (MSL's fragment input) is I, outside identifiers */
static void glsl_rename_in(char* s)
{
    for (char* p = s; (p = strstr(p, "in.")) != NULL; p += 3)
    {
        char c = p == s ? ' ' : p[-1];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            continue;
        memcpy(p, "I. ", 3); /* "I. d" reads as I.d */
    }
}

/* --- WGSL: C-like statements into WGSL's ---------------------------------------------------------------
 * The emitters write declarations as C does (`float4 a = x, b = y;`, `float f;`), lists of assignments
 * joined by commas, and ifs whose body is one statement. Split into statements at the top level
 * (outside parentheses), each becomes WGSL's: a `var` per declarator, one statement per assignment,
 * the if's body in braces; discard_fragment() is discard and psize (WGSL has no point size) goes. */
static const char* const WGSL_TYPES[][2] = { { "float4x4", "mat4x4f" }, { "float4", "vec4f" }, { "float3", "vec3f" },
    { "float2", "vec2f" }, { "float", "f32" }, { "int", "i32" }, { "uint", "u32" } };

static const char* wgsl_decl_type(const char* st, size_t* len)
{
    for (size_t i = 0; i < sizeof WGSL_TYPES / sizeof *WGSL_TYPES; ++i)
    {
        size_t n = strlen(WGSL_TYPES[i][0]);
        if (!strncmp(st, WGSL_TYPES[i][0], n) && st[n] == ' ')
        {
            *len = n;
            return WGSL_TYPES[i][1];
        }
    }
    return NULL;
}

/* the top-level comma-separated parts of s (n chars) into parts; returns how many */
static int wgsl_split_commas(const char* s, size_t n, const char** at, size_t* len, int max)
{
    int k = 0, depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n && k < max; ++i)
    {
        char c = i < n ? s[i] : ',';
        if (c == '(' || c == '[')
            depth++;
        else if (c == ')' || c == ']')
            depth--;
        else if (c == ',' && depth == 0)
        {
            at[k] = s + start, len[k] = i - start, k++;
            start = i + 1;
        }
    }
    return k;
}

static void wgsl_statement(Sb* o, const char* s, size_t n)
{
    while (n && (*s == ' ' || *s == '\n'))
        s++, n--;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n'))
        n--;
    if (!n)
        return;
    if (strstr(s, "psize") && (size_t)(strstr(s, "psize") - s) < n)
        return;
    if (n >= 18 && !strncmp(s, "discard_fragment()", 18))
    {
        sb_printf(o, "  discard;\n");
        return;
    }
    if (n > 3 && !strncmp(s, "if ", 3) && s[3] == '(') /* if (cond) statement */
    {
        int depth = 0;
        size_t i = 3;
        for (; i < n; ++i)
        {
            if (s[i] == '(')
                depth++;
            else if (s[i] == ')' && --depth == 0)
                break;
        }
        sb_printf(o, "  if %.*s {\n", (int)(i + 1 - 3), s + 3);
        wgsl_statement(o, s + i + 1, n - i - 1);
        sb_printf(o, "  }\n");
        return;
    }
    const char* at[24];
    size_t len[24];
    size_t tl = 0;
    const char* type = wgsl_decl_type(s, &tl);
    if (type)
    {
        int k = wgsl_split_commas(s + tl + 1, n - tl - 1, at, len, 24);
        for (int i = 0; i < k; ++i)
        {
            const char* d = at[i];
            size_t dn = len[i];
            while (dn && *d == ' ')
                d++, dn--;
            const char* eq = memchr(d, '=', dn);
            if (eq)
            {
                size_t nl = (size_t)(eq - d);
                while (nl && d[nl - 1] == ' ')
                    nl--;
                sb_printf(o, "  var %.*s: %s = %.*s;\n", (int)nl, d, type, (int)(dn - (size_t)(eq + 1 - d)), eq + 1);
            }
            else
                sb_printf(o, "  var %.*s: %s;\n", (int)dn, d, type);
        }
        return;
    }
    int k = wgsl_split_commas(s, n, at, len, 24);
    for (int i = 0; i < k; ++i)
    {
        const char* d = at[i];
        size_t dn = len[i];
        while (dn && *d == ' ')
            d++, dn--;
        sb_printf(o, "  %.*s;\n", (int)dn, d);
    }
}

/* the body (everything after the prelude) as WGSL statements */
static char* wgsl_fix(const char* s)
{
    Sb o = { 0 };
    int depth = 0;
    size_t start = 0, n = strlen(s);
    for (size_t i = 0; i < n; ++i)
    {
        char c = s[i];
        if (c == '(' || c == '[')
            depth++;
        else if (c == ')' || c == ']')
            depth--;
        else if (depth == 0 && (c == ';' || c == '{' || c == '}'))
        {
            if (c == ';')
                wgsl_statement(&o, s + start, i - start);
            else
            {
                /* a block's head (fn ..., if (...), struct, or a bare block) or its end: as written */
                const char* h = s + start;
                size_t hn = i - start;
                while (hn && (*h == ' ' || *h == '\n'))
                    h++, hn--;
                while (hn && (h[hn - 1] == ' ' || h[hn - 1] == '\n'))
                    hn--;
                if (hn > 3 && !strncmp(h, "if ", 3) && h[3] == '(')
                    sb_printf(&o, "  if %.*s ", (int)(hn - 3), h + 3);
                else if (hn)
                    sb_printf(&o, "%.*s ", (int)hn, h);
                sb_printf(&o, "%c\n", c);
            }
            start = i + 1;
        }
    }
    return o.s;
}

char* gfx_wgsl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    Sb b = { 0 };
    b.glsl = 2;
    emit_vout(&b, vk);
    sb_printf(&b, "struct SB { w: array<u32>, };\n");
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        sb_printf(&b, "@group(0) @binding(%d) var<storage, read> s%d: SB;\n", 1 + s, s);
    if (vk->shadow)
        sb_printf(&b, "@group(0) @binding(5) var<uniform> sm: mat4x4f;\n");
    for (int i = 0; i < 8; ++i)
    {
        int t = fk->prog || i < fk->nstages ? fk->st[i].tex : 0;
        if (t)
            sb_printf(&b, "@group(0) @binding(%d) var tx%d: texture_%s<f32>;\n@group(0) @binding(%d) var sp%d: sampler;\n", 8 + i, i,
                t == 2 ? "cube" : "2d", 24 + i, i);
    }
    size_t decls = b.len; /* the module's declarations, as written; the functions go through wgsl_fix */
    if (vk->prog)
    {
        if (!gfx_msl_vs1(&b, vk, vs_tokens))
            goto fail;
    }
    else
        emit_ff_vs(&b, vk);
    if (fk->prog)
    {
        emit_fs_signature(&b, fk, vk);
        if (!gfx_msl_ps1(&b, fk, ps_tokens))
            goto fail;
        emit_fs_tail(&b, fk, "r0");
    }
    else
        emit_ff_fs(&b, fk, vk);
    glsl_rename_in(b.s + decls);
    char* body = wgsl_fix(b.s + decls);
    Sb out = { 0 };
    sb_printf(&out, "%s%.*s%s", WGSL_PRELUDE, (int)decls, b.s, body ? body : "");
    free(body);
    free(b.s);
    return out.s;
fail:
    free(b.s);
    return NULL;
}

char* gfx_glsl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    Sb b = { 0 };
    b.glsl = 1;
    sb_printf(&b, "%s", GLSL_PRELUDE);
    emit_vout(&b, vk);
    sb_printf(&b, "#ifdef GFX_VS\n");
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        sb_printf(&b, "layout(std430, set = 0, binding = %d) readonly buffer SB%d { uint w[]; } s%d;\n", 1 + s, s, s);
    if (vk->shadow)
        sb_printf(&b, "layout(std140, set = 0, binding = 5) uniform SMB { mat4 sm; };\n");
    if (vk->prog)
    {
        if (!gfx_msl_vs1(&b, vk, vs_tokens))
            goto fail;
    }
    else
        emit_ff_vs(&b, vk);
    sb_printf(&b, "#else\n");
    if (fk->water)
        sb_printf(&b, "%s", WATER_GLSL);
    for (int i = 0; i < 8; ++i)
    {
        int t = fk->prog || i < fk->nstages ? fk->st[i].tex : 0;
        if (t)
            sb_printf(&b, "layout(set = 0, binding = %d) uniform sampler%s tx%d;\n", 8 + i, t == 2 ? "Cube" : "2D", i);
    }
    sb_printf(&b, "layout(location = 0) out vec4 oc;\n");
    if (fk->prog)
    {
        emit_fs_signature(&b, fk, vk);
        if (!gfx_msl_ps1(&b, fk, ps_tokens))
            goto fail;
        emit_fs_tail(&b, fk, "r0");
    }
    else
        emit_ff_fs(&b, fk, vk);
    sb_printf(&b, "#endif\n");
    glsl_rename_in(b.s);
    return b.s;
fail:
    free(b.s);
    return NULL;
}

char* gfx_msl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    Sb b = { 0 };
    sb_printf(&b, "%s", PRELUDE);
    if (fk->water)
        sb_printf(&b, "%s", WATER_MSL);
    emit_vout(&b, vk);
    if (vk->prog)
    {
        if (!gfx_msl_vs1(&b, vk, vs_tokens))
            goto fail;
    }
    else
        emit_ff_vs(&b, vk);
    if (fk->prog)
    {
        emit_fs_signature(&b, fk, vk);
        if (!gfx_msl_ps1(&b, fk, ps_tokens))
            goto fail;
        emit_fs_tail(&b, fk, "r0");
    }
    else
        emit_ff_fs(&b, fk, vk);
    return b.s;
fail:
    free(b.s);
    return NULL;
}
