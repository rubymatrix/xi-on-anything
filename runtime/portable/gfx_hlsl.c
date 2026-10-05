/* HLSL (Shader Model 5.1) for a draw's keys (gfx.h): the Direct3D 12 back end's counterpart of
 * gfx_msl.c, with the same fixed-function pipeline - transform, lighting, fog, texture coordinate
 * generation and transforms, the texture stage cascade, alpha test - as one vertex and one pixel
 * function per key pair, and vs.1.x / ps.1.x shaders translated (gfx_hlsl_shaders.c). Pure C: the
 * D3D12 side (gfx_d3d12.c) compiles the text and caches the result by key.
 *
 * Conventions the functions follow (the root signature is gfx_d3d12.c's):
 *   - matrices are D3D's bytes in a column_major float4x4, so `mul(m, v)` is D3D's v * M;
 *   - D3D8 puts pixel centers on integer coordinates and D3D12 on half-integers: every clip-space
 *     position moves by half a pixel (the "position fixup"), as on Metal;
 *   - vertex fetch is by hand from the stream buffers (raw buffers t0..t3), stride and per-register
 *     offset in the uniforms, so one function serves any offsets and strides;
 *   - the uniforms are b0 in both stages; textures and samplers are indices into the descriptor
 *     heaps (b1), Texture2D in space1 and TextureCube in space2. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_hlsl.h"

_Static_assert(sizeof(GfxU) == 3536, "GfxU must match the HLSL struct U");

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
    "#define INFINITY asfloat(0x7F800000u)\n"
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
    "cbuffer CU : register(b0) { U u; };\n"
    "cbuffer Bind : register(b1) { uint4 ti[2]; uint4 si[2]; };\n"
    "ByteAddressBuffer s0 : register(t0);\n"
    "ByteAddressBuffer s1 : register(t1);\n"
    "ByteAddressBuffer s2 : register(t2);\n"
    "ByteAddressBuffer s3 : register(t3);\n"
    "Texture2D tx2[] : register(t0, space1);\n"
    "TextureCube txc[] : register(t0, space2);\n"
    "SamplerState smp[] : register(s0);\n"
    "cbuffer SM : register(b2) { float4x4 sm; };\n" /* drawn from the sun: the camera's clip space to the map */
    "float4 ld_color(ByteAddressBuffer b, int a) { uint c = b.Load(a); return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0; }\n"
    "float4 ld_ubyte4(ByteAddressBuffer b, int a) { uint c = b.Load(a); return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24); }\n"
    "float4 ld_short2(ByteAddressBuffer b, int a) { int c = asint(b.Load(a)); return float4((c << 16) >> 16, c >> 16, 0, 1); }\n"
    "float4 ld_short4(ByteAddressBuffer b, int a) { int2 c = asint(b.Load2(a)); return float4((c.x << 16) >> 16, c.x >> 16, (c.y << 16) >> 16, c.y >> 16); }\n";

/* lit per pixel (gfx_msl.c's, the same): the vertex function passes on what the lighting starts from
 * (the normal, the position and the material colors), and the pixel function lights. pixel 1: the
 * directional lights (the sun, the moon) per pixel, the point and spot lights per vertex as the game
 * made them; pixel 2: all of them per pixel. */
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

/* the vertex function's output: what the pixel function reads */
static void emit_vout(Sb* b, const GfxVsKey* k)
{
    const char* fl = k->flat ? "nointerpolation " : "";
    sb_printf(b, "struct VOut {\n  float4 pos : SV_Position;\n  %sfloat4 d : COLOR0;\n  %sfloat4 s : COLOR1;\n", fl, fl);
    if (pixel_lit(k))
        sb_printf(b, "  float3 n : LN;\n  float4 pe : LPE;\n  float4 md : LMD;\n  float4 ma : LMA;\n  float4 ms : LMS;\n  float4 me : LME;\n%s",
            point_per_vertex(k) ? "  float4 pa : LPA;\n  float4 pd : LPD;\n  float4 ps : LPS;\n" : "");
    for (int i = 0; i < k->ntex; ++i)
        sb_printf(b, "  float4 t%d : TEXCOORD%d;\n", i, i);
    sb_printf(b, "  float fog : FOG;\n  float ez : EZ;\n};\n");
}

/* v# for an element: a float4 read from its stream (shared with gfx_hlsl_shaders.c's vs.1.x) */
void gfx_hlsl_fetch(Sb* b, const GfxVsKey* k, int reg);
void gfx_hlsl_fetch(Sb* b, const GfxVsKey* k, int reg)
{
    const GfxElem* e = &k->el[reg];
    if (!e->used)
    {
        sb_printf(b, "  float4 v%d = float4(0, 0, 0, 1);\n", reg);
        return;
    }
    sb_printf(b, "  int ad%d = vi * u.stride[%d] + u.offset[%d][%d];\n", reg, e->stream, reg >> 2, reg & 3);
    int s = e->stream;
    switch (e->type)
    {
    case GFX_FLOAT1: sb_printf(b, "  float4 v%d = float4(asfloat(s%d.Load(ad%d)), 0, 0, 1);\n", reg, s, reg); break;
    case GFX_FLOAT2: sb_printf(b, "  float4 v%d = float4(asfloat(s%d.Load2(ad%d)), 0, 1);\n", reg, s, reg); break;
    case GFX_FLOAT3: sb_printf(b, "  float4 v%d = float4(asfloat(s%d.Load3(ad%d)), 1);\n", reg, s, reg); break;
    case GFX_FLOAT4: sb_printf(b, "  float4 v%d = asfloat(s%d.Load4(ad%d));\n", reg, s, reg); break;
    case GFX_D3DCOLOR: sb_printf(b, "  float4 v%d = ld_color(s%d, ad%d);\n", reg, s, reg); break;
    case GFX_UBYTE4: sb_printf(b, "  float4 v%d = ld_ubyte4(s%d, ad%d);\n", reg, s, reg); break;
    case GFX_SHORT2: sb_printf(b, "  float4 v%d = ld_short2(s%d, ad%d);\n", reg, s, reg); break;
    case GFX_SHORT4: sb_printf(b, "  float4 v%d = ld_short4(s%d, ad%d);\n", reg, s, reg); break;
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

static void emit_vs_signature(Sb* b)
{
    sb_printf(b, "VOut vs_main(uint vid : SV_VertexID) {\n  VOut o = (VOut)0;\n  int vi = int(vid) + u.vofs.x;\n");
}

/* clip-space position fixup: D3D8's pixel centers onto D3D12's */
static void emit_fixup(Sb* b)
{
    sb_printf(b, "  o.pos.x += o.pos.w / u.vp.z;\n  o.pos.y -= o.pos.w / u.vp.w;\n");
}

/* the vertex function's end (shared with gfx_hlsl_shaders.c's vs.1.x). Drawn from the sun (the shadow
 * key): whatever clip-space position the function made goes on through the camera's inverse into the
 * sun's map - sm, b2 - so any draw of the scene can be drawn again into the shadow map. */
void gfx_hlsl_vs_return(Sb* b, const GfxVsKey* k);
void gfx_hlsl_vs_return(Sb* b, const GfxVsKey* k)
{
    if (k->shadow) /* the pixel-centre fixup undone (the map has pixels of its own), then on into the map */
        sb_printf(b, "  o.pos.x -= o.pos.w / u.vp.z;\n  o.pos.y += o.pos.w / u.vp.w;\n  o.pos = mul(sm, o.pos);\n");
    sb_printf(b, "  return o;\n}\n");
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
 * lit_d and lit_s. which: 0 every light; 1 the directional ones, with the rest's terms from the vertex
 * function (vin.pa, pd, ps); 2 the rest alone, leaving amb, dif and spc for it to pass on */
static void emit_lighting(Sb* b, const GfxVsKey* k, int pixel, int which)
{
    if (which == 2)
        sb_printf(b, "  float3 amb = float3(0, 0, 0), dif = float3(0, 0, 0), spc = float3(0, 0, 0);\n");
    else if (which == 1)
        sb_printf(b, "  float3 amb = u.ambient.rgb + vin.pa.rgb, dif = vin.pd.rgb, spc = vin.ps.rgb;\n");
    else
        sb_printf(b, "  float3 amb = u.ambient.rgb, dif = float3(0, 0, 0), spc = float3(0, 0, 0);\n");
    if (k->specular)
        sb_printf(b, "  float3 V = %s;\n", k->localviewer ? "normalize(-pe)" : "float3(0, 0, -1)");
    for (int i = 0; i < k->nlights; ++i)
    {
        int t = k->light_type[i];
        if ((which == 1 && t != 3) || (which == 2 && t == 3))
            continue;
        sb_printf(b, "  {\n    Light L = u.light[%d];\n", i);
        if (t == 3)
            sb_printf(b, "    float3 l = L.dir.xyz;\n    float a = 1.0;\n");
        else
        {
            sb_printf(b,
                "    float3 lv = L.pos.xyz - pe;\n    float d = length(lv);\n    float3 l = lv / max(d, 1e-20);\n"
                "    float a = d > L.pos.w ? 0.0 : 1.0 / max(L.att.x + L.att.y * d + L.att.z * d * d, 1e-20);\n");
            /* per pixel, D3D's hard edge at the range is a ring and its attenuation near the light a
             * white spot: the last quarter of the range fades, and the light is at most its colour */
            if (pixel)
                sb_printf(b, "    a = min(a, 1.0) * saturate((L.pos.w - d) / max(0.25 * L.pos.w, 1e-6));\n");
            if (t == 2)
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
    emit_vs_signature(b);
    for (int r = 0; r < GFX_NREGS; ++r) /* unused registers are constants the compiler drops */
        gfx_hlsl_fetch(b, k, r);
    if (k->rhw)
    {
        sb_printf(b,
            "  float w = v0.w != 0.0 ? 1.0 / v0.w : 1.0;\n"
            "  float2 ndc = float2((v0.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (v0.y - u.vp.y) / u.vp.w * 2.0);\n"
            "  o.pos = float4(ndc * w, v0.z * w, w);\n"
            "  float3 pe = float3(0, 0, w);\n"
            "  o.ez = w;\n");
    }
    else
    {
        sb_printf(b,
            "  float4 P = float4(v0.xyz, 1.0);\n"
            "  o.pos = mul(u.wvp, P);\n"
            "  float3 pe = mul(u.wv, P).xyz;\n"
            "  o.ez = pe.z;\n");
    }
    emit_fixup(b);
    int need_normal = k->lighting || 0;
    for (int i = 0; i < k->ntex; ++i)
        need_normal |= (k->tci[i] >> 4) == 1 || (k->tci[i] >> 4) == 3;
    if (need_normal && !k->rhw)
    {
        sb_printf(b, "  float3 N = mul(u.wvit, float4(v3.xyz, 0.0)).xyz;\n");
        if (k->normalize)
            sb_printf(b, "  N = normalize(N);\n");
    }
    else
        sb_printf(b, "  float3 N = float3(0, 0, 1);\n");

    const char* dif_in = k->el[GFX_R_DIFFUSE].used ? "v5" : "float4(1, 1, 1, 1)";
    const char* spe_in = k->el[GFX_R_SPECULAR].used ? "v6" : "float4(0, 0, 0, 0)";
    if (k->lighting && !k->rhw)
    {
        sb_printf(b, "  float4 cd = %s, ca = %s, cs = %s, ce = %s;\n", mcs(k, k->src_diffuse, "u.mat_d"),
            mcs(k, k->src_ambient, "u.mat_a"), mcs(k, k->src_specular, "u.mat_s"), mcs(k, k->src_emissive, "u.mat_e"));
        if (pixel_lit(k)) /* the length of N too: without NORMALIZENORMALS a scaled one lights more */
        {
            sb_printf(b, "  o.n = N;\n  o.pe = float4(pe, length(N));\n  o.md = cd; o.ma = ca; o.ms = cs; o.me = ce;\n"
                         "  o.d = cd;\n  o.s = cs;\n");
            if (point_per_vertex(k))
            {
                sb_printf(b, "  {\n");
                emit_lighting(b, k, 0, 2);
                sb_printf(b, "  o.pa = float4(amb, 0); o.pd = float4(dif, 0); o.ps = float4(spc, 0);\n  }\n");
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
        sb_printf(b, "  o.fog = %s.a;\n", k->el[GFX_R_SPECULAR].used ? "v6" : "float4(1, 1, 1, 1)");

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
            sb_printf(b, "  c%d = mul(u.texm[%d], c%d);\n", i, i, i);
        sb_printf(b, "  o.t%d = c%d;\n", i, i);
    }
    gfx_hlsl_vs_return(b, k);
}

/* --- pixel: the texture stage cascade --------------------------------------------------------------- */
static void arg_expr(char* out, size_t n, int a)
{
    const char* base;
    switch (a & 0xF)
    {
    case 0: base = "vin.d"; break;
    case 1: base = "cur"; break;
    case 2: base = "tex"; break;
    case 3: base = "u.tfactor"; break;
    case 4: base = "vin.s"; break;
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
    case 12: snprintf(out, n, "lerp(%s, %s, vin.d.a)", a2, a1); break;
    case 13: snprintf(out, n, "lerp(%s, %s, tex.a)", a2, a1); break;
    case 14: snprintf(out, n, "lerp(%s, %s, u.tfactor.a)", a2, a1); break;
    case 15: snprintf(out, n, "(%s + %s * (1.0 - tex.a))", a1, a2); break;
    case 16: snprintf(out, n, "lerp(%s, %s, cur.a)", a2, a1); break;
    case 17: snprintf(out, n, "%s", a1); break; /* PREMODULATE: the next stage's texture is not known here */
    case 18: snprintf(out, n, "(%s + %s.a * %s)", a1, a1, a2); break;
    case 19: snprintf(out, n, "(%s * %s + %s.a)", a1, a2, a1); break;
    case 20: snprintf(out, n, "((1.0 - %s.a) * %s + %s)", a1, a2, a1); break;
    case 21: snprintf(out, n, "((1.0 - %s) * %s + %s.a)", a1, a2, a1); break;
    case 24: snprintf(out, n, "(float4)(saturate(dot((%s.rgb - 0.5) * 2.0, (%s.rgb - 0.5) * 2.0)))", a1, a2); break;
    case 25: snprintf(out, n, "(%s + %s * %s)", a0, a1, a2); break;
    case 26: snprintf(out, n, "(%s * %s + (1.0 - %s) * %s)", a0, a1, a0, a2); break;
    default: snprintf(out, n, "%s", a1); break; /* the bump ops: the environment map is not emulated */
    }
}

/* texture slot i sampled at coord: the slot's heap indices are root constants */
void gfx_hlsl_sample(Sb* b, const char* dst, int i, int cube, const char* coord);
void gfx_hlsl_sample(Sb* b, const char* dst, int i, int cube, const char* coord)
{
    char c = "xyzw"[i & 3];
    sb_printf(b, "  %s = %s[ti[%d].%c].Sample(smp[si[%d].%c], %s);\n", dst, cube ? "txc" : "tx2", i >> 2, c, i >> 2, c, coord);
}

static void emit_fs_signature(Sb* b, const GfxVsKey* vk)
{
    sb_printf(b, "float4 fs_main(VOut vin) : SV_Target {\n");
    if (pixel_lit(vk)) /* the colors the vertex function would have given, lit here */
    {
        sb_printf(b, "  {\n  float3 N = vin.n * (rsqrt(max(dot(vin.n, vin.n), 1e-20)) * %s);\n"
                     "  float3 pe = vin.pe.xyz;\n  float4 cd = vin.md, ca = vin.ma, cs = vin.ms, ce = vin.me;\n",
            vk->normalize ? "1.0" : "vin.pe.w");
        emit_lighting(b, vk, 1, point_per_vertex(vk) ? 1 : 0);
        sb_printf(b, "  vin.d = lit_d; vin.s = lit_s;\n  }\n");
    }
}

static void emit_fs_tail(Sb* b, const GfxFsKey* k, const char* col)
{
    if (k->alpha_func && k->alpha_func != 8)
    {
        static const char* const cmp[] = { "", "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        if (k->alpha_func == 1)
            sb_printf(b, "  clip(-1.0);\n");
        else if (k->alpha_func < 8)
            sb_printf(b, "  if (!(round(saturate(%s.a) * 255.0) %s u.params.y)) discard;\n", col, cmp[k->alpha_func]);
    }
    if (k->fog)
    {
        if (k->fog == 4)
            sb_printf(b, "  float f = saturate(vin.fog);\n");
        else
        {
            sb_printf(b, "  float f;\n  float fd = abs(vin.ez);\n");
            emit_fog_factor(b, "f", k->fog, "fd");
        }
        sb_printf(b, "  %s.rgb = lerp(u.fogcolor.rgb, %s.rgb, f);\n", col, col);
    }
    sb_printf(b, "  return %s;\n}\n", col);
}

static void emit_ff_fs(Sb* b, const GfxFsKey* k, const GfxVsKey* vk)
{
    emit_fs_signature(b, vk);
    sb_printf(b, "  float4 cur = vin.d, tmp = float4(0, 0, 0, 0), tex = float4(1, 1, 1, 1);\n");
    for (int i = 0; i < k->nstages; ++i)
    {
        const GfxStage* s = &k->st[i];
        char coord[64];
        if (s->tex == 1)
        {
            if (s->projected)
            {
                const char* w = s->ncoord == 3 ? "z" : s->ncoord == 4 ? "w" : "y";
                snprintf(coord, sizeof coord, "vin.t%d.xy / vin.t%d.%s", i, i, w);
            }
            else
                snprintf(coord, sizeof coord, "vin.t%d.xy", i);
            gfx_hlsl_sample(b, "tex", i, 0, coord);
        }
        else if (s->tex == 2)
        {
            snprintf(coord, sizeof coord, "vin.t%d.xyz", i);
            gfx_hlsl_sample(b, "tex", i, 1, coord);
        }
        else
            sb_printf(b, "  tex = float4(1, 1, 1, 1);\n");
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
            sb_printf(b, "  { float3 c = saturate(%s.rgb); %s = float4(c, %s.a); }\n", ce, dst, i ? "cur" : "vin.d");
    }
    if (k->specular_add)
        sb_printf(b, "  cur.rgb = saturate(cur.rgb + vin.s.rgb);\n");
    emit_fs_tail(b, k, "cur");
}

char* gfx_hlsl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    Sb b = { 0 };
    sb_printf(&b, "%s", PRELUDE);
    emit_vout(&b, vk);
    if (vk->prog)
    {
        if (!gfx_hlsl_vs1(&b, vk, vs_tokens))
            goto fail;
    }
    else
        emit_ff_vs(&b, vk);
    if (fk->prog)
    {
        emit_fs_signature(&b, vk);
        if (!gfx_hlsl_ps1(&b, fk, ps_tokens))
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

/* --- the device's own functions: the back buffer to the window, and the frame-rate overlay -------------- */
const char gfx_hlsl_util[] =
    "cbuffer Bind : register(b1) { uint4 ti[2]; uint4 si[2]; };\n"
    "Texture2D tx2[] : register(t0, space1);\n"
    "SamplerState smp[] : register(s0);\n"
    "struct PO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "PO present_vs(uint vid : SV_VertexID) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  PO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "float4 present_fs(PO i) : SV_Target {\n"
    "  return float4(tx2[ti[0].x].Sample(smp[si[0].x], i.uv).rgb, 1.0);\n"
    "}\n"
    /* the same, sharpened by k (si[1].w, 0..1): contrast-adaptive, a negative lobe over the four
     * neighbors that shrinks where the neighborhood is already near black or white (gfx_metal.m's) */
    "float3 pt(float2 uv) { return tx2[ti[0].x].SampleLevel(smp[si[0].x], uv, 0).rgb; }\n"
    "float4 present_cas_fs(PO i) : SV_Target {\n"
    "  uint w, h; tx2[ti[0].x].GetDimensions(w, h);\n"
    "  float2 tx = 1.0 / float2(w, h);\n"
    "  float k = asfloat(si[1].w);\n"
    "  float3 c = pt(i.uv);\n"
    "  float3 n = pt(i.uv - float2(0, tx.y)), so = pt(i.uv + float2(0, tx.y));\n"
    "  float3 we = pt(i.uv - float2(tx.x, 0)), e = pt(i.uv + float2(tx.x, 0));\n"
    "  float3 mn = min(c, min(min(n, so), min(we, e))), mx = max(c, max(max(n, so), max(we, e)));\n"
    "  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));\n"
    "  float3 lobe = -amp * lerp(0.125, 0.2, saturate(k));\n"
    "  return float4(saturate((c + (n + so + we + e) * lobe) / (1.0 + 4.0 * lobe)), 1.0);\n"
    "}\n"
    /* the frame-rate overlay: a 5x7 bitmap font drawn per pixel, no texture */
    "cbuffer OU : register(b0) { float4 rect; float scale; uint n; float2 size; uint4 text[8]; };\n"
    "static const uint FONT[17 * 7] = {\n"
    "  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,\n"
    "  0x1F,0x02,0x04,0x02,0x01,0x11,0x0E, 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,\n"
    "  0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, 0x1F,0x01,0x02,0x04,0x08,0x08,0x08, 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,\n"
    "  0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C, 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,\n"
    "  0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, 0x00,0x00,0x1A,0x15,0x15,0x11,0x11, 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E,\n"
    "  0x00,0x00,0x00,0x00,0x00,0x0C,0x0C, 0x00,0x00,0x00,0x00,0x00,0x00,0x00 };\n"
    "float4 overlay_vs(uint vid : SV_VertexID) : SV_Position {\n"
    "  float2 c = rect.xy + float2((vid & 1) ? rect.z : 0.0, (vid & 2) ? rect.w : 0.0);\n"
    "  return float4(c.x / size.x * 2.0 - 1.0, 1.0 - c.y / size.y * 2.0, 0, 1);\n"
    "}\n"
    "float4 overlay_fs(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 p = (pos.xy - rect.xy) / scale - 2.0;\n"
    "  int cell = int(floor(p.x / 6.0)), gx = int(floor(p.x)) - cell * 6, gy = int(floor(p.y));\n"
    "  if (p.x >= 0.0 && cell < int(n) && gx < 5 && gy >= 0 && gy < 7) {\n"
    "    uint ch = text[cell >> 2][cell & 3];\n"
    "    if ((FONT[ch * 7 + uint(gy)] >> (4 - gx)) & 1) return float4(1.0, 0.85, 0.2, 1.0);\n"
    "  }\n"
    "  return float4(0, 0, 0, 0.55);\n"
    "}\n";

/* --- the scene effects (gfx_d3d12.c's scene_fx): gfx_metal.m's FX_MSL, function for function -----------
 * Its comments say what each pass does; here only what differs. The uniforms are FxU at b0; b1 holds
 * the pass's texture heap indices (ft: up to 8), the linear-clamp and comparison sampler slots
 * (fsm[0].xy) and a blur's direction (fsm[1].xy). Every texture is sampled at level 0 (SampleLevel):
 * the passes' targets have one level, and loops and branches leave no derivatives. */
const char gfx_hlsl_fx[] =
    "struct FxU {\n"
    "  float4 proj, zp, vp, size, ao, grade, hand, up, sun, suncol, sunuv, fogc, fogp, bloom, rays, shadow;\n"
    "  float4x4 lmat; float4 smap, smap2; float4x4 reproj; float4 hist; float4x4 lmatn; float4 smapn, smapn2, aop;\n"
    "  float4x4 gimat, giinv; float4 gi, gip;\n"
    "};\n"
    "cbuffer CU : register(b0) { FxU u; };\n"
    "cbuffer FB : register(b1) { uint4 ft[2]; uint4 fsm[2]; };\n"
    "Texture2D tx2[] : register(t0, space1);\n"
    "SamplerState smp[] : register(s0);\n"
    "SamplerComparisonState cmps[] : register(s0, space1);\n"
    "#define TX(i) tx2[ft[(i) >> 2][(i) & 3]]\n"
    "#define LIN smp[fsm[0].x]\n"
    "#define CMP cmps[fsm[0].y]\n"
    "#define DIR int2(asint(fsm[1].x), asint(fsm[1].y))\n"
    "struct FO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "FO fx_vs(uint vid : SV_VertexID) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  FO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);\n"
    "float view_z(float d) {\n"
    "  d = (d - u.zp.z) / max(u.zp.w - u.zp.z, 1e-6);\n"
    "  float z = u.zp.y / (d * u.hand.x - u.zp.x);\n"
    "  return d >= 0.999999 || !(z * u.hand.x > 0.0) ? 0.0 : z;\n"
    "}\n"
    /* px a pixel's centre in the target; every draw went through the position fixup (D3D8's pixel
     * centres onto these: half a pixel right and down), so the point it shows is the one half a pixel
     * up and left of it. Without that, a floor seen at a slant came back below itself - by a few
     * centimetres a few units out, more farther - and fell into its own shadow. */
    "float3 view_pos(float2 px, float z) {\n"
    "  px -= 0.5;\n"
    "  float2 ndc = float2((px.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (px.y - u.vp.y) / u.vp.w * 2.0);\n"
    "  float w = z * u.hand.x;\n"
    "  return float3((ndc.x * w - u.proj.z * z) / u.proj.x, (ndc.y * w - u.proj.w * z) / u.proj.y, z);\n"
    "}\n"
    "float depth_at(Texture2D dt, float2 px) { return dt.Load(int3(int2(px), 0)).r; }\n"
    "float3 pos_at(Texture2D dt, float2 px) {\n"
    "  px = clamp(px, u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n"
    "  px = floor(px) + 0.5;\n"
    "  return view_pos(px, view_z(depth_at(dt, px)));\n"
    "}\n"
    "float4 fx_linz(FO fi) : SV_Target {\n"
    "  float2 px = floor(u.vp.xy + fi.uv * u.vp.zw) + 0.5;\n"
    "  return float4(view_z(depth_at(TX(0), px)), 0, 0, 0);\n"
    "}\n"
    "float4 fx_zmip(FO fi) : SV_Target {\n"
    "  uint w, h; TX(0).GetDimensions(w, h);\n"
    "  int2 p = int2(fi.pos.xy), hi = int2(w, h) - 1;\n"
    "  return TX(0).Load(int3(min(p * 2 + int2(p.y & 1, p.x & 1), hi), 0));\n"
    "}\n"
    /* the level's size from the occlusion's (size.zw), not GetDimensions: the same, and cheaper in the loop */
    "float3 pos_lz(Texture2D lz, float2 q, float r) {\n"
    "  float2 t = (q - u.vp.xy) * u.size.zw / u.vp.zw;\n"
    "  uint lv = uint(clamp(int(floor(log2(max(r * u.size.z / u.vp.z, 1.0)))) - 3, 0, 3));\n"
    "  uint2 dim = max(uint2(u.size.zw) >> lv, uint2(1, 1));\n"
    "  uint2 p = min(uint2(max(t, 0.0)) >> lv, dim - 1);\n"
    "  return view_pos(q, lz.Load(int3(p, lv)).r);\n"
    "}\n"
    "float sun_shadow(Texture2D dt, float3 P, float3 N, float dist, float k) {\n"
    "  float nl = dot(N, u.sun.xyz);\n"
    "  float fade = smoothstep(0.0, 0.15, nl) * (1.0 - smoothstep(0.6 * u.shadow.w, u.shadow.w, dist));\n"
    "  if (fade <= 0.0) return 1.0;\n"
    "  const int NS = 16;\n"
    "  float3 O = P + N * (0.01 * dist);\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float a = (float(i) + k) / float(NS);\n"
    "    float3 R = O + u.sun.xyz * (u.shadow.y * a * a);\n"
    "    float rd = R.z * u.hand.x;\n"
    "    if (rd <= 0.05) break;\n"
    "    float2 ndc = float2(R.x * u.proj.x + R.z * u.proj.z, R.y * u.proj.y + R.z * u.proj.w) / rd;\n"
    "    float2 q = u.vp.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;\n"
    "    if (any(q < u.vp.xy) || any(q + 0.5 >= u.vp.xy + u.vp.zw)) break;\n" /* the pixel read within the viewport, not just q */
    "    float sd = view_z(depth_at(dt, q + 0.5)) * u.hand.x;\n" /* where the fixup drew it */
    "    float in_front = rd - sd;\n"
    "    if (sd > 0.0 && in_front > 0.005 * rd + 0.02 && in_front < u.shadow.z + 0.01 * rd) return 1.0 - fade * (1.0 - a * a);\n"
    "  }\n"
    "  return 1.0;\n"
    "}\n"
    "static const float2 DISK[16] = { float2(-0.94, -0.40), float2(0.95, -0.77), float2(-0.09, -0.93), float2(0.34, 0.29),\n"
    "  float2(-0.92, 0.46), float2(-0.82, -0.88), float2(-0.38, 0.28), float2(0.97, 0.76), float2(0.44, -0.98),\n"
    "  float2(0.54, -0.47), float2(-0.26, -0.42), float2(-0.42, 0.87), float2(0.31, 0.92), float2(0.79, 0.19),\n"
    "  float2(-0.03, 0.04), float2(0.15, -0.33) };\n"
    "float sun_look(Texture2D sm, float4x4 lm, float4 p, float du, float minw, float3 P, float3 N, float dist, float k,\n"
    "               bool hard, out float edge) {\n"
    "  float3 Q = P + N * (1.5 * p.x + 0.002 * dist);\n"
    "  float4 lc = mul(lm, float4(Q, 1.0));\n"
    "  float2 uv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);\n"
    "  float2 e = abs(lc.xy);\n"
    "  edge = lc.z >= 1.0 ? 0.0 : 1.0 - smoothstep(0.8, 0.95, max(e.x, e.y));\n"
    "  if (edge <= 0.0) return 1.0;\n"
    "  uint sw, sh; sm.GetDimensions(sw, sh);\n"
    "  float z = lc.z - p.y, sz = float(sw), tx = 1.0 / sz;\n"
    "  float a = k * 6.2831853, ca = cos(a), sa = sin(a);\n"
    "  float bs = 0.0, bn = 0.0;\n"
    "  for (int i = 0; i < 16; ++i) {\n"
    "    float2 q = clamp((uv + DISK[i] * (32.0 * tx)) * sz, 0.0, sz - 1.0);\n"
    "    float d = sm.Load(int3(int2(q), 0)).r;\n"
    "    if (d < z) { bs += d; bn += 1.0; }\n"
    "  }\n"
    "  if (bn == 0.0) return 1.0;\n"
    "  float pen = clamp((z - bs / bn) * p.z, hard ? 0.5 * tx : max(1.5 * tx, 0.02 / (p.x * sz)), 32.0 * tx);\n"
    "  float nr = minw > 0.0 ? smoothstep(0.5 * minw, 1.5 * minw, (z - bs / bn) * du) : 1.0;\n" /* sun_min 0: keep them all */
    "  float zc = z - pen * p.w, s = 0.0;\n"
    "  for (int j = 0; j < 16; ++j) {\n"
    "    float2 dk = DISK[j], r = float2(ca * dk.x - sa * dk.y, sa * dk.x + ca * dk.y);\n"
    "    s += sm.SampleCmpLevelZero(CMP, uv + r * pen, zc);\n"
    "  }\n"
    "  return lerp(1.0, s / 16.0, nr);\n"
    "}\n"
    "float sun_map(Texture2D sm, Texture2D smn, float3 P, float3 N, float dist, float k) {\n"
    "  float nl = dot(N, u.sun.xyz);\n"
    "  float face = lerp(1.0 - 0.6 * u.smap2.y, 1.0, smoothstep(-0.3, 0.25, nl)), use = smoothstep(-0.05, 0.15, nl);\n"
    /* turned from the sun: looked up as well, but shaded only by casters a unit or more away (a wall, a
     * roof) - not by the body's own front, whose shade the game's lighting has already. Skipped, a
     * character in a building's shadow was lit where it faced away and dark where it faced the sun. */
    "  float mw = lerp(max(u.smap2.z, 1.0), u.smap2.z, use);\n"
    "  float en = 0.0, ef = 0.0, s = 1.0;\n"
    "  if (u.smapn2.y > 0.0)\n"
    "    s = sun_look(smn, u.lmatn, u.smapn, u.smapn2.x, mw, P, N, dist, k, u.smapn2.z > 0.0, en);\n"
    "  if (en < 1.0) {\n"
    "    float sf = sun_look(sm, u.lmat, float4(u.smap.yzw, u.smap2.x), u.smap2.w, mw, P, N, dist, k, u.smapn2.z > 0.0, ef);\n"
    "    s = lerp(sf, s, en);\n"
    "  }\n"
    "  return min(lerp(1.0, s, max(en, ef)), face);\n" /* the face's shade with a map or without */
    "}\n"
    "static const uint BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n"
    /* the depth at t0, the sun's maps at t1 and t2, the occlusion's depth levels at t3 */
    /* the surface's normal at px (P its point) from the depth: each way the neighbour nearer in depth */
    "float3 normal_at(Texture2D dt, float2 px, float3 P) {\n"
    "  float3 r = pos_at(dt, px + float2(1, 0)) - P, l = P - pos_at(dt, px - float2(1, 0));\n"
    "  float3 d = pos_at(dt, px + float2(0, 1)) - P, t = P - pos_at(dt, px - float2(0, 1));\n"
    /* at the viewport's edge one side is the pixel itself (pos_at clamps to it): zero, its cross a NaN */
    "  float3 dx = (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0 ? r : l;\n"
    "  float3 dy = (abs(d.z) < abs(t.z) && dot(d, d) > 0.0) || dot(t, t) == 0.0 ? d : t;\n"
    "  float3 nc = cross(dx, dy);\n"
    "  float3 N = dot(nc, nc) > 1e-24 ? normalize(nc) : -normalize(P);\n"
    "  if (dot(N, P) > 0.0) N = -N;\n"
    "  return N;\n"
    "}\n"
    "float4 fx_ao(FO fi) : SV_Target {\n"
    "  float2 px = floor(u.vp.xy + fi.uv * u.vp.zw) + 0.5;\n"
    "  float3 P = pos_at(TX(0), px);\n"
    "  float dist = P.z * u.hand.x;\n"
    "  if (dist <= 0.0) return float4(1.0, 0.0, 1.0, 1.0);\n"
    "  float3 N = normal_at(TX(0), px, P);\n"
    "  int2 cell = int2(fi.pos.xy) & 3;\n"
    "  float k = frac((float(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0 + u.hist.y);\n"
    /* ifs, not ?: (HLSL evaluates both sides of one) */
    "  float sh = 1.0, mp = 1.0;\n"
    "  if (u.shadow.x > 0.0 && u.sun.w > 0.0) sh = sun_shadow(TX(0), P, N, dist, k);\n"
    "  if (u.smap.x > 0.0 && u.sun.w > 0.0) mp = sun_map(TX(1), TX(2), P, N, dist, k);\n"
    "  float rad = u.ao.x, rpx = min(rad * u.proj.y * 0.5 * u.vp.w / dist, u.ao.w);\n"
    "  if (u.ao.y <= 0.0 || rpx < 2.0) return float4(1.0, dist, mp, sh);\n"
    "  int NS = max(int(u.aop.x), 1);\n"
    "  float sum = 0.0;\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float a = (float(i) + k) / float(NS);\n"
    "    float ang = float(i) * 2.3999632 + k * 6.2831853;\n"
    "    float2 q = px + float2(cos(ang), sin(ang)) * (a * rpx);\n"
    "    if (any(q < u.vp.xy) || any(q >= u.vp.xy + u.vp.zw)) continue;\n"
    "    float3 Q = pos_lz(TX(3), q, a * rpx);\n"
    "    if (Q.z == 0.0 || dist - Q.z * u.hand.x > 0.5 * rad) continue;\n"
    "    float3 v = Q - P;\n"
    "    float vv = dot(v, v), vn = dot(v, N);\n"
    "    float q2 = vv / (rad * rad), fall = saturate(1.0 - q2 * q2);\n"
    "    sum += fall * max(vn * rsqrt(vv + 1e-6) - u.ao.z, 0.0);\n"
    "  }\n"
    "  float facing = smoothstep(0.1, 0.4, dot(N, -P) / dist);\n"
    "  return float4(saturate(1.0 - 3.0 * facing * sum / float(NS)), dist, mp, sh);\n"
    "}\n"
    "float3 ao_at(Texture2D ao, float2 uv, float dist) {\n"
    "  if (dist <= 0.0) return float3(1, 1, 1);\n"
    "  float2 g = uv * u.size.zw - 0.5, f = frac(g);\n"
    "  int2 i0 = int2(floor(g)), hi = int2(u.size.zw) - 1;\n"
    "  float3 s = float3(0, 0, 0);\n"
    "  float w = 0.0;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    int2 o = int2(k & 1, k >> 1);\n"
    "    float4 t = ao.Load(int3(clamp(i0 + o, int2(0, 0), hi), 0));\n"
    "    float bw = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);\n"
    "    float dw = t.y > 0.0 ? 1.0 / (1e-3 + abs(t.y - dist) / dist) : 1e-3;\n"
    "    s += t.xzw * bw * dw; w += bw * dw;\n"
    "  }\n"
    "  return w > 0.0 ? s / w : float3(1, 1, 1);\n"
    "}\n"
    "float4 fx_blur(FO fi) : SV_Target {\n"
    "  int2 p = int2(fi.pos.xy), hi = int2(u.size.zw) - 1, dir = DIR;\n"
    "  float4 c = TX(0).Load(int3(p, 0));\n"
    "  if (c.y <= 0.0) return c;\n"
    "  float sx = c.x, w = 1.0, sw = 1.0;\n"
    "  float2 ss = c.zw;\n"
    "  for (int i = -2; i <= 2; ++i) {\n"
    "    if (i == 0) continue;\n"
    "    float4 t = TX(0).Load(int3(clamp(p + dir * i, int2(0, 0), hi), 0));\n"
    "    float k = (abs(i) == 2 ? 0.5 : 1.0) * saturate(1.0 - abs(t.y - c.y) / (0.03 * c.y));\n"
    "    sx += t.x * k; w += k;\n"
    "    if (abs(i) == 1 && u.smapn2.z == 0.0) { ss += t.zw * (0.5 * k); sw += 0.5 * k; }\n"
    "  }\n"
    "  return float4(sx / w, c.y, ss / sw);\n"
    "}\n"
    "float4 fx_temporal(FO fi) : SV_Target {\n"
    "  float4 c = TX(0).Load(int3(int2(fi.pos.xy), 0));\n"
    "  if (c.y <= 0.0 || u.hist.x == 0.0) return c;\n"
    "  float2 px = u.vp.xy + fi.uv * u.vp.zw;\n"
    "  float3 P = view_pos(px, c.y * u.hand.x);\n"
    "  float4 pc = mul(u.reproj, float4(P, 1.0));\n"
    "  if (pc.w <= 1e-4) return c;\n"
    "  float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
    "  if (any(puv < 0.0) || any(puv > 1.0)) return c;\n"
    "  float4 h = TX(1).SampleLevel(LIN, puv, 0);\n"
    "  if (!(h.y > 0.0) || abs(h.y - pc.w) > 0.04 * pc.w) return c;\n"
    "  int2 p = int2(fi.pos.xy), hi = int2(u.size.zw) - 1;\n"
    "  float3 lo = c.xzw, hi3 = c.xzw;\n"
    "  for (int dy = -1; dy <= 1; ++dy)\n"
    "    for (int dx = -1; dx <= 1; ++dx) {\n"
    "      float4 t = TX(0).Load(int3(clamp(p + int2(dx, dy), int2(0, 0), hi), 0));\n"
    "      if (t.y > 0.0 && abs(t.y - c.y) < 0.05 * c.y) { lo = min(lo, t.xzw); hi3 = max(hi3, t.xzw); }\n"
    "    }\n"
    "  const float3 give = float3(0.06, 0.02, 0.02);\n"
    "  float3 m = lerp(c.xzw, clamp(h.xzw, lo - give, hi3 + give), u.hist.z);\n"
    "  return float4(m.x, c.y, m.y, m.z);\n"
    "}\n"
    /* The bounce light (gi): what the sunlit surfaces near P throw onto it. The map holds the casters
     * near the camera as the sun sees them - each texel a surface the sun lights, its depth and its
     * colour - so the light thrown is gathered from the texels round P's own place on it: a spiral of
     * gip.z over the reach (gi.y of the map), each a surface X at its depth, its colour read from a level
     * that averages its neighbours (gi.w). Each is weighed by how P faces it, how X (turned toward the
     * sun, as what the sun lights is) faces P, and how far it is. Nothing stands between them here: a
     * floor under a roof sees the roof's top turned away from it, and takes nothing from it. Depth at
     * t0, the map's depth at t1, its colour at t2; rgb the light, a the distance (for the passes after). */
    "float4 fx_gi(FO fi) : SV_Target {\n"
    "  float2 px = floor(u.vp.xy + fi.uv * u.vp.zw) + 0.5;\n"
    "  float3 P = pos_at(TX(0), px);\n"
    "  float dist = P.z * u.hand.x;\n"
    "  if (dist <= 0.0) return float4(0, 0, 0, 0);\n"
    "  float4 q = mul(u.gimat, float4(P, 1.0));\n"
    "  float2 e = abs(q.xy);\n"
    "  float edge = (1.0 - smoothstep(0.8, 0.95, max(e.x, e.y))) * (1.0 - smoothstep(0.7 * u.gip.w, u.gip.w, dist));\n"
    "  if (edge <= 0.0 || q.z >= 1.0) return float4(0, 0, 0, dist);\n"
    "  float3 N = normal_at(TX(0), px, P);\n"
    "  float2 uv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);\n"
    "  uint sw, sh; TX(1).GetDimensions(sw, sh);\n"
    "  int2 cell = int2(fi.pos.xy) & 3;\n"
    "  float k = frac((float(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0 + u.hist.y);\n"
    "  int NS = max(int(u.gip.z), 1);\n"
    "  float r2 = u.gi.z * u.gi.z;\n"
    "  float3 sum = float3(0, 0, 0);\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float t = (float(i) + k) / float(NS), a = float(i) * 2.3999632 + k * 6.2831853;\n"
    "    float2 us = uv + float2(cos(a), sin(a)) * (sqrt(t) * u.gi.y);\n"
    "    if (any(us <= 0.0) || any(us >= 1.0)) continue;\n"
    "    float zs = TX(1).Load(int3(int2(us * float2(sw, sh)), 0)).r;\n"
    "    if (zs >= 1.0) continue;\n"
    "    float4 x = mul(u.giinv, float4(us.x * 2.0 - 1.0, 1.0 - us.y * 2.0, zs, 1.0));\n"
    "    float3 v = x.xyz / x.w - P;\n"
    "    float dd = dot(v, v) + 1e-4;\n"
    "    float3 vn = v * rsqrt(dd);\n"
    "    float w = saturate(dot(N, vn)) * saturate(0.25 - 0.75 * dot(u.sun.xyz, vn)) * r2 / (dd + 0.25 * r2) * saturate(2.0 - dd / r2);\n"
    "    sum += TX(2).SampleLevel(LIN, us, u.gi.w).rgb * w;\n"
    "  }\n"
    "  return float4(sum * (u.gi.x * edge / float(NS)), dist);\n"
    "}\n"
    /* the bounce light over frames: last frame's where this point was then, kept within what its
     * neighbours have now (gip.y of it: the temporal setting) */
    "float4 fx_gitemp(FO fi) : SV_Target {\n"
    "  float4 c = TX(0).Load(int3(int2(fi.pos.xy), 0));\n"
    "  if (c.a <= 0.0 || u.gip.x == 0.0) return c;\n"
    "  float2 px = u.vp.xy + fi.uv * u.vp.zw;\n"
    "  float3 P = view_pos(px, c.a * u.hand.x);\n"
    "  float4 pc = mul(u.reproj, float4(P, 1.0));\n"
    "  if (pc.w <= 1e-4) return c;\n"
    "  float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
    "  if (any(puv < 0.0) || any(puv > 1.0)) return c;\n"
    "  float4 h = TX(1).SampleLevel(LIN, puv, 0);\n"
    "  if (!(h.a > 0.0) || abs(h.a - pc.w) > 0.04 * pc.w) return c;\n"
    "  uint w, hh; TX(0).GetDimensions(w, hh);\n"
    "  int2 p = int2(fi.pos.xy), hi = int2(w, hh) - 1;\n"
    "  float3 lo = c.rgb, up3 = c.rgb;\n"
    "  for (int dy = -1; dy <= 1; ++dy)\n"
    "    for (int dx = -1; dx <= 1; ++dx) {\n"
    "      float4 t = TX(0).Load(int3(clamp(p + int2(dx, dy), int2(0, 0), hi), 0));\n"
    "      if (t.a > 0.0 && abs(t.a - c.a) < 0.05 * c.a) { lo = min(lo, t.rgb); up3 = max(up3, t.rgb); }\n"
    "    }\n"
    "  float3 give = 0.1 * (up3 - lo) + 0.01;\n"
    "  return float4(lerp(c.rgb, clamp(h.rgb, lo - give, up3 + give), u.gip.y), c.a);\n"
    "}\n"
    /* the bounce light at uv from its half size: the four round it, each as near in distance as it is */
    "float3 gi_at(Texture2D g, float2 uv, float dist) {\n"
    "  if (dist <= 0.0) return float3(0, 0, 0);\n"
    "  uint w, h; g.GetDimensions(w, h);\n"
    "  float2 gg = uv * float2(w, h) - 0.5, f = frac(gg);\n"
    "  int2 i0 = int2(floor(gg)), hi = int2(w, h) - 1;\n"
    "  float3 s = float3(0, 0, 0);\n"
    "  float sw = 0.0;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    int2 o = int2(k & 1, k >> 1);\n"
    "    float4 t = g.Load(int3(clamp(i0 + o, int2(0, 0), hi), 0));\n"
    "    float bw = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);\n"
    "    float dw = t.a > 0.0 ? 1.0 / (1e-3 + abs(t.a - dist) / dist) : 1e-3;\n"
    "    s += t.rgb * bw * dw; sw += bw * dw;\n"
    "  }\n"
    "  return sw > 0.0 ? s / sw : float3(0, 0, 0);\n"
    "}\n"
    "float3 s0(float2 uv) { return TX(0).SampleLevel(LIN, uv, 0).rgb; }\n"
    "float4 fx_bright(FO fi) : SV_Target {\n"
    "  float2 uv = (u.vp.xy + fi.uv * u.vp.zw) / u.size.xy, t = 1.0 / u.size.xy;\n"
    "  float3 c = 0.25 * (s0(uv + t * float2(-1, -1)) + s0(uv + t * float2(1, -1)) + s0(uv + t * float2(-1, 1)) + s0(uv + t * float2(1, 1)));\n"
    /* as fx_comp will shade it: the scene is copied before the occlusion and the sun's shadows, and a
     * character in a cliff's shadow, drawn dark, still glowed as bright as in the open */
    "  if (u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0) {\n"
    "    float4 os = TX(1).SampleLevel(LIN, fi.uv, 0);\n"
    "    c *= lerp(1.0, os.x, u.ao.y) * lerp(1.0, os.z, u.smap.x) * lerp(1.0, os.w, u.shadow.x);\n"
    "  }\n"
    "  float l = max(c.r, max(c.g, c.b)), k = u.bloom.z;\n"
    "  float soft = clamp(l - u.bloom.x + k, 0.0, 2.0 * k);\n"
    "  soft = soft * soft / (4.0 * k + 1e-5);\n"
    "  return float4(c * (max(soft, l - u.bloom.x) / max(l, 1e-5)), 1.0);\n"
    "}\n"
    "static const float FXAA_Q[10] = { 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
    "float l0(float2 uv) { return dot(s0(uv), LUMA); }\n"
    "float4 fx_fxaa(FO fi) : SV_Target {\n"
    "  float2 rc = 1.0 / u.size.xy, uv = fi.pos.xy * rc;\n"
    "  float3 c = s0(uv);\n"
    "  float lm = dot(c, LUMA);\n"
    "  float ln = l0(uv + float2(0, -rc.y)), ls = l0(uv + float2(0, rc.y));\n"
    "  float lw = l0(uv + float2(-rc.x, 0)), le = l0(uv + float2(rc.x, 0));\n"
    "  float mx = max(lm, max(max(ln, ls), max(lw, le))), mn = min(lm, min(min(ln, ls), min(lw, le))), range = mx - mn;\n"
    "  if (range < max(0.0312, mx * 0.125)) return float4(c, 1.0);\n"
    "  float lnw = l0(uv + float2(-rc.x, -rc.y)), lne = l0(uv + float2(rc.x, -rc.y));\n"
    "  float lsw = l0(uv + float2(-rc.x, rc.y)), lse = l0(uv + float2(rc.x, rc.y));\n"
    "  float eh = abs(lnw + lne - 2.0 * ln) + 2.0 * abs(lw + le - 2.0 * lm) + abs(lsw + lse - 2.0 * ls);\n"
    "  float ev = abs(lnw + lsw - 2.0 * lw) + 2.0 * abs(ln + ls - 2.0 * lm) + abs(lne + lse - 2.0 * le);\n"
    "  bool horz = eh >= ev;\n"
    "  float l1 = horz ? ln : lw, l2 = horz ? ls : le, g1 = abs(l1 - lm), g2 = abs(l2 - lm);\n"
    "  float stp = horz ? rc.y : rc.x, lavg, grad;\n"
    "  if (g1 >= g2) { stp = -stp; lavg = 0.5 * (l1 + lm); grad = g1; } else { lavg = 0.5 * (l2 + lm); grad = g2; }\n"
    "  float2 e = uv, along = horz ? float2(rc.x, 0) : float2(0, rc.y);\n"
    "  if (horz) e.y += stp * 0.5; else e.x += stp * 0.5;\n"
    "  float2 p1 = e - along, p2 = e + along;\n"
    "  float d1 = l0(p1) - lavg, d2 = l0(p2) - lavg;\n"
    "  bool r1 = abs(d1) >= grad * 0.25, r2 = abs(d2) >= grad * 0.25;\n"
    "  for (int i = 0; i < 10 && !(r1 && r2); ++i) {\n"
    "    if (!r1) { p1 -= along * FXAA_Q[i]; d1 = l0(p1) - lavg; r1 = abs(d1) >= grad * 0.25; }\n"
    "    if (!r2) { p2 += along * FXAA_Q[i]; d2 = l0(p2) - lavg; r2 = abs(d2) >= grad * 0.25; }\n"
    "  }\n"
    "  float dist1 = horz ? uv.x - p1.x : uv.y - p1.y, dist2 = horz ? p2.x - uv.x : p2.y - uv.y;\n"
    "  bool near1 = dist1 < dist2;\n"
    "  float dmin = min(dist1, dist2), len = dist1 + dist2;\n"
    "  bool mid_lower = lm < lavg, good = ((near1 ? d1 : d2) < 0.0) != mid_lower;\n"
    "  float off = good ? -dmin / len + 0.5 : 0.0;\n"
    "  float avg = (2.0 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 12.0;\n"
    "  float sub = saturate(abs(avg - lm) / range);\n"
    "  sub = (-2.0 * sub + 3.0) * sub * sub;\n"
    "  off = max(off, sub * sub * 0.75);\n"
    "  float2 f = uv;\n"
    "  if (horz) f.y += off * stp; else f.x += off * stp;\n"
    "  return float4(s0(f), 1.0);\n"
    "}\n"
    "float4 fx_down(FO fi) : SV_Target {\n"
    "  uint w, h; TX(0).GetDimensions(w, h);\n"
    "  float2 tx = 1.0 / float2(w, h);\n"
    "  return 0.25 * (TX(0).SampleLevel(LIN, fi.uv + tx * float2(-1, -1), 0) + TX(0).SampleLevel(LIN, fi.uv + tx * float2(1, -1), 0) +\n"
    "                 TX(0).SampleLevel(LIN, fi.uv + tx * float2(-1, 1), 0) + TX(0).SampleLevel(LIN, fi.uv + tx * float2(1, 1), 0));\n"
    "}\n"
    "float4 fx_gauss(FO fi) : SV_Target {\n"
    "  uint w, h; TX(0).GetDimensions(w, h);\n"
    "  float2 tx = float2(DIR) / float2(w, h);\n"
    "  float4 c = TX(0).SampleLevel(LIN, fi.uv, 0) * 0.2270270;\n"
    "  c += (TX(0).SampleLevel(LIN, fi.uv + tx * 1.3846154, 0) + TX(0).SampleLevel(LIN, fi.uv - tx * 1.3846154, 0)) * 0.3162162;\n"
    "  c += (TX(0).SampleLevel(LIN, fi.uv + tx * 3.2307692, 0) + TX(0).SampleLevel(LIN, fi.uv - tx * 3.2307692, 0)) * 0.0702703;\n"
    "  return c;\n"
    "}\n"
    "float4 fx_raymask(FO fi) : SV_Target {\n"
    "  float2 px = floor(u.vp.xy + fi.uv * u.vp.zw) + 0.5;\n"
    "  if (view_z(depth_at(TX(1), px)) != 0.0) return float4(0, 0, 0, 0);\n"
    "  float3 c = s0(px / u.size.xy);\n"
    "  float2 d = (fi.uv - u.sunuv.xy) * float2(u.proj.y / u.proj.x, 1.0);\n"
    "  float glow = saturate(1.0 - length(d) / 0.6);\n"
    "  return float4(c * smoothstep(0.35, 0.9, dot(c, LUMA)) * glow * glow, 1.0);\n"
    "}\n"
    "float4 fx_rays(FO fi) : SV_Target {\n"
    "  const int NS = 64;\n"
    "  float2 uv = fi.uv, st = (fi.uv - u.sunuv.xy) * (u.rays.z / float(NS));\n"
    "  float3 acc = float3(0, 0, 0);\n"
    "  float w = 1.0;\n"
    "  for (int i = 0; i < NS; ++i) { acc += s0(uv) * w; w *= u.rays.y; uv -= st; }\n"
    "  return float4(acc * (4.0 / float(NS)), 1.0);\n"
    "}\n"
    "float3 screen(float3 a, float3 b) { return 1.0 - (1.0 - saturate(a)) * (1.0 - saturate(b)); }\n"
    /* the scene's copy at t0, the occlusion at t1, the depth at t2, bloom at t3 and t4, the rays at t5,
     * the bounce light at t6 */
    "float4 fx_comp(FO fi) : SV_Target {\n"
    "  float2 px = fi.pos.xy;\n"
    "  float4 c = TX(0).Load(int3(int2(px), 0));\n"
    "  int dbg = int(u.grade.w);\n"
    "  float3 os = float3(1, 1, 1);\n"
    "  if (u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0) os = ao_at(TX(1), fi.uv, view_z(depth_at(TX(2), px)) * u.hand.x);\n"
    "  float o = os.x, sun = lerp(1.0, os.y, u.smap.x) * lerp(1.0, os.z, u.shadow.x);\n"
    "  if (dbg == 1) return float4(o, o, o, c.a);\n"
    "  if (dbg == 5) return float4(sun, sun, sun, c.a);\n"
    /* the bounce light (t6): on the surface's own colour, mostly where the sun does not reach (in full
     * sun it is little beside it), shaded by the occlusion like any light that is not the sun's */
    "  float3 gl = float3(0, 0, 0);\n"
    "  if (u.gi.x > 0.0) gl = gi_at(TX(6), fi.uv, view_z(depth_at(TX(2), px)) * u.hand.x);\n"
    "  if (dbg == 6) return float4(gl * 3.0, c.a);\n"
    "  if (dbg == 7 && px.x < u.vp.x + u.vp.z * 0.5) gl = float3(0, 0, 0);\n"
    "  float3 base = c.rgb;\n"
    "  c.rgb *= lerp(1.0, o, u.ao.y) * sun;\n"
    "  c.rgb += base * gl * lerp(1.0, o, u.ao.y) * (1.0 - 0.75 * lerp(1.0, os.y, u.smap.x));\n"
    "  float f = 0.0;\n"
    "  if (u.fogc.a > 0.0) {\n"
    "    float z = view_z(depth_at(TX(2), px));\n"
    "    if (z != 0.0) {\n"
    "      float3 P = view_pos(px, z);\n"
    "      float d = length(P), bd = u.fogp.x * dot(P, u.up.xyz);\n"
    "      float k = abs(bd) > 1e-4 ? (1.0 - exp(-bd)) / bd : 1.0;\n"
    "      f = min(1.0 - exp(-u.fogc.a * d * k), u.fogp.y);\n"
    "      float g = u.fogp.w, cs = dot(P / max(d, 1e-5), u.sun.xyz);\n"
    "      float sunk = u.sun.w * u.fogp.z * pow((1.0 - g) * (1.0 - g) / max(1.0 + g * g - 2.0 * g * cs, 1e-5), 1.5);\n"
    "      c.rgb = lerp(c.rgb, u.fogc.rgb + u.suncol.rgb * sunk, f);\n"
    "    }\n"
    "  }\n"
    "  if (dbg == 2) return float4(f, f, f, c.a);\n"
    "  float3 add = float3(0, 0, 0);\n"
    "  if (u.bloom.y > 0.0) {\n"
    "    float3 bl = TX(3).SampleLevel(LIN, fi.uv, 0).rgb * 0.6 + TX(4).SampleLevel(LIN, fi.uv, 0).rgb * 0.8;\n"
    "    if (dbg == 3) return float4(bl, c.a);\n"
    "    add += bl * u.bloom.y;\n"
    "  }\n"
    "  if (u.rays.x > 0.0 && u.sunuv.z > 0.0) {\n"
    "    float3 r = TX(5).SampleLevel(LIN, fi.uv, 0).rgb * u.suncol.rgb * u.sunuv.z;\n"
    "    if (dbg == 4) return float4(r, c.a);\n"
    "    add += r * u.rays.x;\n"
    "  }\n"
    "  c.rgb = screen(c.rgb, add);\n"
    "  float3 x = lerp((float3)dot(c.rgb, LUMA), c.rgb, u.grade.y);\n"
    "  x = saturate(x);\n"
    "  x = lerp(x, x * x * (3.0 - 2.0 * x), u.grade.z);\n"
    "  c.rgb = lerp(c.rgb, x, u.grade.x);\n"
    "  return c;\n"
    "}\n"
    /* one level of a mip chain from the level above (the scene filter): bilinear at the texel's
     * center, the average of the 2x2 above it */
    "float4 fx_mip(FO fi) : SV_Target { return TX(0).SampleLevel(LIN, fi.uv, 0); }\n";
