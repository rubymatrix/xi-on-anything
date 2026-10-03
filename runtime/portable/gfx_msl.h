/* Shader generation for the back ends (gfx_msl.c, gfx_msl_shaders.c): Metal Shading Language for
 * Metal (gfx_metal.m), and the same functions as Vulkan GLSL for Vulkan (gfx_vulkan.c). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

typedef struct Sb
{
    char* s;
    size_t len, cap;
    int glsl; /* the dialect being written: 0 MSL, 1 Vulkan GLSL */
} Sb;

void sb_printf(Sb* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/* The source of one library holding vs_main and fs_main for a key pair, or NULL when a shader
 * uses something the translator does not know (malloc'd; the caller frees). */
char* gfx_msl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens);

/* The same as GLSL 4.50 for Vulkan: one text with both stages, the vertex function under
 * #ifdef GFX_VS and the fragment function under #ifdef GFX_FS, with no #version line (the back end
 * adds it and the define). The bindings, all in set 0 (gfx_vulkan.c pushes them per draw):
 *   0 the uniforms (U), 1-4 the vertex streams (storage buffers of words), 5 the shadow matrix,
 *   8-15 texture stages 0-7 (combined image samplers), 16 the water's uniforms, 17 and 18 its
 *   copies of the color and depth targets. */
char* gfx_glsl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens);

/* The pieces both translators share (gfx_msl.c): the vertex function's start (signature, v#
 * fetched from the streams), a texture stage sampled at coord, and a per-component compare that
 * picks b where cond holds else a (MSL select, GLSL mix over a bvec4). */
void gfx_msl_vs_begin(Sb* b, const GfxVsKey* k);
void gfx_msl_sample(char* out, size_t n, const Sb* b, unsigned stage, const char* coord);
void gfx_msl_select(char* out, size_t n, const Sb* b, const char* a, const char* bv, const char* x, const char* op, const char* y);

/* vs.1.0/1.1 and ps.1.0-1.4 token streams: the function bodies (after the signature, before the
 * fragment tail, which reads r0). 0 when the shader cannot be translated. */
int gfx_msl_vs1(Sb* b, const GfxVsKey* k, const uint32_t* tokens);
/* The vertex function's parameters after the streams, and its end (the shadow pass's matrix). */
void gfx_msl_vs_params(Sb* b, const GfxVsKey* k);
void gfx_msl_vs_return(Sb* b, const GfxVsKey* k);
int gfx_msl_ps1(Sb* b, const GfxFsKey* k, const uint32_t* tokens);
