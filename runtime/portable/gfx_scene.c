/* The scene effects' CPU side, shared by the back ends that have them (gfx_metal.m, gfx_vulkan.c):
 * matrices, a draw's first vertex through its vertex function (where a copy of a zone mesh stands),
 * the sun's cascades, and the effects' uniforms (FxU, mirrored in each back end's effect shaders). */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gfx.h"
#include "gfx_fx.h"
#include "gfx_scene.h"

/* --- a draw's first vertex through its vertex function, on the CPU (gfx_clip0) ---
 * FFXI draws every copy of a zone mesh (all the trees of one kind) from the same buffers, each placed
 * by its vertex shader's constants: the buffers say nothing of where a copy stands. Its first
 * vertex's clip position, through the frame's camera back into the world, does. A small vs.1.x
 * interpreter over the draw's constants and vertex bytes, the same arithmetic gfx_msl_shaders.c
 * writes out as MSL; fixed function is P * WVP. */
static void vs_src(float out[4], uint32_t t, const float (*r)[4], const float (*v)[4], const float (*c)[4], const float* a0)
{
    uint32_t type = (t >> 28) & 7, num = t & 0x7FF, mod = (t >> 24) & 0xF, sw = (t >> 16) & 0xFF;
    static const float zero[4] = { 0, 0, 0, 0 };
    const float* reg = zero;
    if (type == 0 && num < 12)
        reg = r[num];
    else if (type == 1 && num < GFX_NREGS)
        reg = v[num];
    else if (type == 2)
    {
        int i = (int)num + ((t & 0x2000) ? (int)a0[0] : 0);
        reg = c[i < 0 ? 0 : i >= GFX_NVSC ? GFX_NVSC - 1 : i];
    }
    else if (type == 3)
        reg = a0;
    for (int i = 0; i < 4; ++i)
    {
        float x = reg[(sw >> (2 * i)) & 3];
        switch (mod)
        {
        case 1: x = -x; break;
        case 2: x = x - 0.5f; break;
        case 3: x = 0.5f - x; break;
        case 4: x = (x - 0.5f) * 2.0f; break;
        case 5: x = -(x - 0.5f) * 2.0f; break;
        case 6: x = 1.0f - x; break;
        case 7: x = x * 2.0f; break;
        case 8: x = -x * 2.0f; break;
        }
        out[i] = x;
    }
}

/* the input registers of vertex vi as the declaration maps them; base[s] and have[s]: stream s's bytes
 * where the draw binds it, and how many there are (NULL: none) */
static int vs_fetch(const GfxDraw* d, const uint8_t* const* base_s, const size_t* have_s, int vi, float v[GFX_NREGS][4])
{
    for (int r = 0; r < GFX_NREGS; ++r)
    {
        v[r][0] = v[r][1] = v[r][2] = 0, v[r][3] = 1;
        const GfxElem* e = &d->vs.el[r];
        if (!e->used)
            continue;
        int s = e->stream;
        const uint8_t* base = base_s[s];
        size_t have = have_s[s];
        if (!base)
            return 0;
        long at = (long)vi * d->u.stride[s] + d->u.offset[r];
        static const uint8_t SIZE[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
        if (at < 0 || (size_t)at + SIZE[e->type & 7] > have)
            return 0;
        const uint8_t* p = base + at;
        switch (e->type)
        {
        case GFX_FLOAT1: memcpy(v[r], p, 4); break;
        case GFX_FLOAT2: memcpy(v[r], p, 8); break;
        case GFX_FLOAT3: memcpy(v[r], p, 12); break;
        case GFX_FLOAT4: memcpy(v[r], p, 16); break;
        case GFX_D3DCOLOR: v[r][0] = p[2] / 255.0f, v[r][1] = p[1] / 255.0f, v[r][2] = p[0] / 255.0f, v[r][3] = p[3] / 255.0f; break;
        case GFX_UBYTE4: for (int i = 0; i < 4; ++i) v[r][i] = p[i]; break;
        case GFX_SHORT2: { int16_t h[2]; memcpy(h, p, 4); v[r][0] = h[0], v[r][1] = h[1]; break; }
        default: { int16_t h[4]; memcpy(h, p, 8); for (int i = 0; i < 4; ++i) v[r][i] = h[i]; break; }
        }
    }
    return 1;
}

int gfx_clip0(const GfxDraw* d, const uint8_t* const* base, const size_t* have, float out[4])
{
    if (d->vs.rhw)
        return 0;
    long idx = d->vertex_start;
    if (d->indices)
        idx = d->index_size == 2 ? ((const uint16_t*)d->indices)[0] : (long)((const uint32_t*)d->indices)[0];
    float v[GFX_NREGS][4];
    if (!vs_fetch(d, base, have, (int)(idx + d->u.vofs), v))
        return 0;
    if (!d->vs.prog)
    {
        const float* m = d->u.wvp;
        for (int j = 0; j < 4; ++j)
            out[j] = v[0][0] * m[j] + v[0][1] * m[4 + j] + v[0][2] * m[8 + j] + m[12 + j];
        return 1;
    }
    const uint32_t* t = d->vs_tokens;
    if (!t || (t[0] & 0xFFFF0000u) != 0xFFFE0000u)
        return 0;
    float r[12][4], a0[4] = { 0, 0, 0, 0 }, opos[4] = { 0, 0, 0, 1 };
    memset(r, 0, sizeof r);
    const float(*c)[4] = (const float(*)[4])d->u.vsc;
    for (uint32_t i = 1; i < 65536;)
    {
        uint32_t tok = t[i], op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu)
            break;
        if (op == 0xFFFE)
        {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const uint32_t* p = &t[i + 1];
        float s0[4], s1[4], s2[4], res[4] = { 0, 0, 0, 0 };
        int np;
        switch (op)
        {
        case 0: case 81: np = op ? 5 : 0; break;
        case 1: case 6: case 7: case 14: case 15: case 16: case 19: case 78: case 79: np = 2; break;
        case 2: case 3: case 5: case 8: case 9: case 10: case 11: case 12: case 13: case 17: np = 3; break;
        case 20: case 21: case 22: case 23: case 24: np = 3; break;
        case 4: case 18: np = 4; break;
        default: return 0; /* not vs.1.x */
        }
        if (op == 0 || op == 81)
        {
            i += 1 + (uint32_t)np;
            continue;
        }
        if (np >= 2)
            vs_src(s0, p[1], r, v, c, a0);
        if (np >= 3 && !(op >= 20 && op <= 24))
            vs_src(s1, p[2], r, v, c, a0);
        if (np >= 4)
            vs_src(s2, p[3], r, v, c, a0);
        switch (op)
        {
        case 1: memcpy(res, s0, 16); break;
        case 2: for (int k = 0; k < 4; ++k) res[k] = s0[k] + s1[k]; break;
        case 3: for (int k = 0; k < 4; ++k) res[k] = s0[k] - s1[k]; break;
        case 4: for (int k = 0; k < 4; ++k) res[k] = s0[k] * s1[k] + s2[k]; break;
        case 5: for (int k = 0; k < 4; ++k) res[k] = s0[k] * s1[k]; break;
        case 6: res[0] = res[1] = res[2] = res[3] = s0[3] == 0.0f ? INFINITY : 1.0f / s0[3]; break;
        case 7: res[0] = res[1] = res[2] = res[3] = s0[3] == 0.0f ? INFINITY : 1.0f / sqrtf(fabsf(s0[3])); break;
        case 8: res[0] = res[1] = res[2] = res[3] = s0[0] * s1[0] + s0[1] * s1[1] + s0[2] * s1[2]; break;
        case 9: res[0] = res[1] = res[2] = res[3] = s0[0] * s1[0] + s0[1] * s1[1] + s0[2] * s1[2] + s0[3] * s1[3]; break;
        case 10: for (int k = 0; k < 4; ++k) res[k] = fminf(s0[k], s1[k]); break;
        case 11: for (int k = 0; k < 4; ++k) res[k] = fmaxf(s0[k], s1[k]); break;
        case 12: for (int k = 0; k < 4; ++k) res[k] = s0[k] < s1[k] ? 1.0f : 0.0f; break;
        case 13: for (int k = 0; k < 4; ++k) res[k] = s0[k] >= s1[k] ? 1.0f : 0.0f; break;
        case 14: case 78: res[0] = res[1] = res[2] = res[3] = exp2f(s0[3]); break;
        case 15: case 79: res[0] = res[1] = res[2] = res[3] = s0[3] == 0.0f ? -INFINITY : log2f(fabsf(s0[3])); break;
        case 16:
            res[0] = 1.0f, res[1] = fmaxf(s0[0], 0.0f), res[3] = 1.0f;
            res[2] = s0[0] > 0.0f && s0[1] > 0.0f ? powf(s0[1], fminf(fmaxf(s0[3], -127.9961f), 127.9961f)) : 0.0f;
            break;
        case 17: res[0] = 1.0f, res[1] = s0[1] * s1[1], res[2] = s0[2], res[3] = s1[3]; break;
        case 18: for (int k = 0; k < 4; ++k) res[k] = s2[k] + (s1[k] - s2[k]) * s0[k]; break;
        case 19: for (int k = 0; k < 4; ++k) res[k] = s0[k] - floorf(s0[k]); break;
        default:
        {
            /* m4x4, m4x3, m3x4, m3x3, m3x2: dot products against consecutive constant rows */
            int rows = op == 20 ? 4 : op == 21 ? 3 : op == 22 ? 4 : op == 23 ? 3 : 2, three = op >= 22;
            for (int k = 0; k < rows; ++k)
            {
                float row[4];
                vs_src(row, p[2] + (uint32_t)k, r, v, c, a0);
                res[k] = s0[0] * row[0] + s0[1] * row[1] + s0[2] * row[2] + (three ? 0.0f : s0[3] * row[3]);
            }
            break;
        }
        }
        uint32_t dt = p[0], type = (dt >> 28) & 7, num = dt & 0x7FF, mask = (dt >> 16) & 0xF;
        if (op >= 20 && op <= 24)
            mask = op == 20 || op == 22 ? 0xF : op == 24 ? 0x3 : 0x7;
        if (mask == 0)
            mask = 0xF;
        if ((dt >> 20) & 1)
            for (int k = 0; k < 4; ++k)
                res[k] = fminf(fmaxf(res[k], 0.0f), 1.0f);
        float* dst = type == 0 && num < 12 ? r[num] : type == 3 ? a0 : type == 4 && num == 0 ? opos : NULL;
        if (dst)
            for (int k = 0; k < 4; ++k)
                if (mask & (1u << k))
                    dst[k] = res[k];
        i += 1 + (uint32_t)np;
    }
    memcpy(out, opos, 16);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]) && isfinite(out[3]);
}

/* the inverse of a 4x4 matrix (row-major); 0 when it has none */
int gfx_mat_inverse(float* out, const float* m)
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f)
        return 0;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] / det;
    return 1;
}

void gfx_mat_mul(float* o, const float* a, const float* b)
{
    float t[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, t, sizeof t);
}

/* row vector p (x, y, z, w) through a row-major matrix */
void gfx_xform4(float* o, const float* p, const float* m)
{
    float t[4];
    for (int j = 0; j < 4; ++j)
        t[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + p[3] * m[12 + j];
    memcpy(o, t, 16);
}

void gfx_normalize3(float* v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f)
        v[0] /= l, v[1] /= l, v[2] /= l;
}

/* One cascade of the sun's map: an orthographic view along the sun over a sphere around the slice
 * [t0, t1] of what the camera sees, its center snapped to whole texels (the shadows' edges hold still
 * as the camera moves). */
void gfx_sun_fit(const GfxScene* s, const float* invV, const float* L, float t0, float t1, int size, SunCascade* k)
{
    float hand = s->proj[11] < 0.0f ? -1.0f : 1.0f, p[8][3], c[3] = { 0, 0, 0 };
    for (int i = 0; i < 8; ++i)
    {
        float t = i < 4 ? t0 : t1, z = t * hand, nx = (i & 1) ? 1.0f : -1.0f, ny = (i & 2) ? 1.0f : -1.0f;
        float v[4] = { (nx * t - s->proj[8] * z) / s->proj[0], (ny * t - s->proj[9] * z) / s->proj[5], z, 1.0f };
        for (int j = 0; j < 3; ++j)
            p[i][j] = v[0] * invV[j] + v[1] * invV[4 + j] + v[2] * invV[8 + j] + invV[12 + j], c[j] += p[i][j] / 8.0f;
    }
    float R = 0.0f;
    for (int i = 0; i < 8; ++i)
    {
        float dx = p[i][0] - c[0], dy = p[i][1] - c[1], dz = p[i][2] - c[2];
        R = fmaxf(R, sqrtf(dx * dx + dy * dy + dz * dz));
    }
    R = ceilf(R); /* whole units: the texel's size holds still */
    /* the sun's axes: f the way its light goes, r and u across */
    float f[3] = { -L[0], -L[1], -L[2] }, up[3] = { 0, 1, 0 };
    if (fabsf(f[1]) > 0.9f)
        up[0] = 1, up[1] = 0;
    float r[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
    gfx_normalize3(r);
    float u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    float tx = 2.0f * R / (float)size;
    k->size = size;
    float cx = floorf((c[0] * r[0] + c[1] * r[1] + c[2] * r[2]) / tx) * tx;
    float cy = floorf((c[0] * u[0] + c[1] * u[1] + c[2] * u[2]) / tx) * tx;
    float cz = c[0] * f[0] + c[1] * f[1] + c[2] * f[2];
    /* casters stand up to 200 units sunward of the sphere (a cliff over the camera) */
    float back = 200.0f, range = 2.0f * R + back, z0 = cz - R - back;
    float S[16] = {
        r[0] / R, u[0] / R, f[0] / range, 0,
        r[1] / R, u[1] / R, f[1] / range, 0,
        r[2] / R, u[2] / R, f[2] / range, 0,
        -cx / R, -cy / R, -z0 / range, 1,
    };
    memcpy(k->S, S, sizeof S);
    gfx_mat_mul(k->lmat, invV, S);
    k->texel = tx, k->bias = 0.03f / range, k->range = range, k->across = 2.0f * R;
    /* the penumbra's radius grows by sun_soft for each unit from the caster: in the map's width
     * (2R across) per unit of its depth (range deep). Never less than the sun's own half-degree disc
     * (0.0047 a unit), even for hard edges: a character's shadow stays hard, a cliff's 30 units off
     * no longer ends in its low-polygon outline */
    k->soft = fmaxf(g_fxs.sun_soft, 0.0047f) * range / (2.0f * R);
    k->slope = 2.0f * R / range;
}

/* the near map's texels across: sun_detail, a power of two from 512 to 8192; 0 and 1 are the old
 * switch (4096, 8192) */
int gfx_sun_near_size(void)
{
    float d = g_fxs.sun_detail;
    if (d < 2.0f)
        return d >= 0.5f ? 2 * GFX_SUN_MAP : GFX_SUN_MAP;
    int n = 512;
    while (n < 8192 && (float)n * 1.5f < d)
        n *= 2;
    return n;
}
