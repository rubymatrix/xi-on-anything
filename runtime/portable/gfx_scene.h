/* The scene effects' CPU side (gfx_scene.c), shared by gfx_metal.m and gfx_vulkan.c. Matrices are
 * D3D's: row-major, row vectors, v * M. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

/* the effects' uniforms: one buffer for every pass, mirrored in FX_MSL (gfx_metal.m) and FX_GLSL
 * (gfx_vulkan.c) */
typedef struct FxU
{
    float proj[4], zp[4], vp[4], size[4], ao[4], grade[4], hand[4], up[4], sun[4], suncol[4], sunuv[4], fogc[4], fogp[4],
        bloom[4], rays[4], shadow[4], lmat[16], smap[4], smap2[4], reproj[16], hist[4], lmatn[16], smapn[4], smapn2[4], aop[4];
} FxU;

enum { GFX_SUN_MAP = 4096 }; /* the far cascade's texels across */

/* One cascade of the sun's map: an orthographic view along the sun over a sphere around the slice
 * [t0, t1] of what the camera sees (gfx_sun_fit). */
typedef struct SunCascade
{
    float S[16];    /* the world to the map */
    float lmat[16]; /* view space to the map: x, y -1..1, z 0..1 */
    float texel, bias, soft, slope, range, across;
    int size; /* the map's texels across */
} SunCascade;

/* the inverse of a 4x4 matrix; 0 when it has none */
int gfx_mat_inverse(float* out, const float* m);
void gfx_mat_mul(float* o, const float* a, const float* b);
/* row vector p (x, y, z, w) through m */
void gfx_xform4(float* o, const float* p, const float* m);
void gfx_normalize3(float* v);
/* The clip-space position of the draw's first vertex (its vertex function run on the CPU: P * WVP,
 * or a vs.1.x interpreter); base[s] and have[s] are stream s's bytes where the draw binds it and
 * their count (NULL: none). 0 when it cannot be had. */
int gfx_clip0(const GfxDraw* d, const uint8_t* const* base, const size_t* have, float out[4]);
/* the cascade over [t0, t1] of the scene's view, toward the sun L (world), size texels across */
void gfx_sun_fit(const GfxScene* s, const float* invV, const float* L, float t0, float t1, int size, SunCascade* k);
/* the near cascade's texels across (sun_detail) */
int gfx_sun_near_size(void);
