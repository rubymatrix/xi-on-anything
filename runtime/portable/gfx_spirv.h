/* gfx_hlsl.c's HLSL for the Android Vulkan back end (gfx_vulkan.cpp): rebound to fixed bindings in one
 * descriptor set and compiled to SPIR-V with glslang. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif
    enum
    {
        GFX_SPIRV_SET = 0,
        GFX_SPIRV_STREAM0 = 0,     /* four storage buffers, bindings 0..3 */
        GFX_SPIRV_U = 4,           /* GfxU or utility OU, uniform buffer */
        GFX_SPIRV_TEXTURE2D = 8,   /* eight sampled images */
        GFX_SPIRV_TEXTURECUBE = 9, /* eight sampled images */
        GFX_SPIRV_SAMPLER = 10,    /* eight samplers */
        GFX_SPIRV_TEXTURE_COUNT = 8,
    };
    /* Adapt this project's generated SM5.1 HLSL. malloc-owned result, or NULL. */
    char* gfx_spirv_source(const char* source);
    /* Compile one entry point. vertex != 0 for VS, else FS. malloc-owned words;
     * caller frees. Returns 1 on success, 0 with stderr diagnostic on failure.
     * No Y inversion: use a negative-height viewport. Thread-safe compilation. */
    int gfx_spirv_compile(const char* source, const char* entry, int vertex, uint32_t** words, size_t* word_count);
#ifdef __cplusplus
}
#endif
