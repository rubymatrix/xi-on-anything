#include "gfx_spirv.h"
#include "gfx.h"
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(GfxU) == 3536, "GfxU must match HLSL U");
_Static_assert(offsetof(GfxU, texm) == 192, "U texm offset");
_Static_assert(offsetof(GfxU, mat_d) == 704, "U material offset");
_Static_assert(offsetof(GfxU, vofs) == 864, "U vertex offset");
_Static_assert(offsetof(GfxU, offset) == 896, "U input offsets");
_Static_assert(offsetof(GfxU, light) == 976, "U light offset");
_Static_assert(offsetof(GfxU, vsc) == 1872, "U VS constant offset");
_Static_assert(offsetof(GfxU, psc) == 3408, "U PS constant offset");

static pthread_once_t compiler_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t compiler_lock = PTHREAD_MUTEX_INITIALIZER;
static int compiler_ready;
static void compiler_initialize(void)
{
    compiler_ready = glslang_initialize_process();
}

static int replace(char** text, const char* before, const char* after)
{
    size_t n = strlen(before), m = strlen(after), len = strlen(*text), count = 0;
    const char* at = *text;
    while ((at = strstr(at, before)) != NULL)
    {
        ++count;
        at += n;
    }
    if (!count)
        return 1;
    if (m > n && count > (SIZE_MAX - len - 1) / (m - n))
        return 0;
    size_t result_len = m >= n ? len + count * (m - n) : len - count * (n - m);
    char* result = (char*)malloc(result_len + 1);
    if (!result)
        return 0;
    char* out = result;
    at = *text;
    const char* next;
    while ((next = strstr(at, before)) != NULL)
    {
        size_t part = (size_t)(next - at);
        memcpy(out, at, part);
        out += part;
        memcpy(out, after, m);
        out += m;
        at = next + n;
    }
    strcpy(out, at);
    free(*text);
    *text = result;
    return 1;
}

char* gfx_spirv_source(const char* source)
{
    if (!source)
        return NULL;
    size_t len = strlen(source);
    char* result = (char*)malloc(len + 1);
    if (!result)
        return NULL;
    memcpy(result, source, len + 1);
#define REPLACE(a, b)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!replace(&result, (a), (b)))                                                                               \
            goto failed;                                                                                               \
    } while (0)
    REPLACE("cbuffer CU : register(b0)", "[[vk::binding(4, 0)]] cbuffer CU");
    REPLACE("cbuffer OU : register(b0)", "[[vk::binding(4, 0)]] cbuffer OU");
    REPLACE("cbuffer Bind : register(b1) { uint4 ti[2]; uint4 si[2]; };", "");
    REPLACE("Texture2D tx2[] : register(t0, space1);", "[[vk::binding(8, 0)]] Texture2D tx2[8];");
    REPLACE("TextureCube txc[] : register(t0, space2);", "[[vk::binding(9, 0)]] TextureCube txc[8];");
    REPLACE("SamplerState smp[] : register(s0);", "[[vk::binding(10, 0)]] SamplerState smp[8];");
    /* HLSL column-major matrices reinterpret the D3D row-vector matrix bytes. */
    REPLACE("  float4x4 wvp, wv, wvit;", "  column_major float4x4 wvp, wv, wvit;");
    REPLACE("  float4x4 texm[8];", "  column_major float4x4 texm[8];");
    for (int i = 0; i < 4; ++i)
    {
        char before[80], after[80];
        snprintf(before, sizeof before, "ByteAddressBuffer s%d : register(t%d);", i, i);
        snprintf(after, sizeof after, "[[vk::binding(%d, 0)]] ByteAddressBuffer s%d;", i, i);
        REPLACE(before, after);
    }
    for (int i = 0; i < 8; ++i)
    {
        char before[96], after[96];
        snprintf(before, sizeof before, "ti[%d].%c", i >> 2, "xyzw"[i & 3]);
        snprintf(after, sizeof after, "%d", i);
        REPLACE(before, after);
        snprintf(before, sizeof before, "si[%d].%c", i >> 2, "xyzw"[i & 3]);
        REPLACE(before, after);
        /* Stable locations even if the other stage drops an unused field. */
        snprintf(before, sizeof before, "float4 t%d : TEXCOORD%d;", i, i);
        snprintf(after, sizeof after, "[[vk::location(%d)]] float4 t%d : TEXCOORD%d;", i + 2, i, i);
        REPLACE(before, after);
    }
    REPLACE("nointerpolation float4 d : COLOR0;", "[[vk::location(0)]] nointerpolation float4 d : COLOR0;");
    REPLACE("  float4 d : COLOR0;", "  [[vk::location(0)]] float4 d : COLOR0;");
    REPLACE("nointerpolation float4 s : COLOR1;", "[[vk::location(1)]] nointerpolation float4 s : COLOR1;");
    REPLACE("  float4 s : COLOR1;", "  [[vk::location(1)]] float4 s : COLOR1;");
    REPLACE("float fog : FOG;", "[[vk::location(10)]] float fog : FOG;");
    REPLACE("float ez : EZ;", "[[vk::location(11)]] float ez : EZ;");
    REPLACE("float2 uv : TEXCOORD0;", "[[vk::location(0)]] float2 uv : TEXCOORD0;");
#undef REPLACE
    return result;
failed:
    free(result);
    return NULL;
}

int gfx_spirv_compile(const char* source, const char* entry, int vertex, uint32_t** words, size_t* word_count)
{
    if (!words || !word_count)
        return 0;
    *words = NULL;
    *word_count = 0;
    if (!source || !entry)
        return 0;
    char* adapted = gfx_spirv_source(source);
    if (!adapted)
        return 0;
    pthread_once(&compiler_once, compiler_initialize);
    if (!compiler_ready)
    {
        free(adapted);
        return 0;
    }
    pthread_mutex_lock(&compiler_lock);
    glslang_input_t input = {0};
    input.language = GLSLANG_SOURCE_HLSL;
    input.stage = vertex ? GLSLANG_STAGE_VERTEX : GLSLANG_STAGE_FRAGMENT;
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_1;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_3;
    input.code = adapted;
    input.default_version = 100;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.messages = (glslang_messages_t)(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT |
                                          GLSLANG_MSG_READ_HLSL_BIT | GLSLANG_MSG_HLSL_OFFSETS_BIT);
    input.resource = glslang_default_resource();
    glslang_shader_t* shader = glslang_shader_create(&input);
    glslang_program_t* program = NULL;
    int result = 0;
    if (!shader)
        goto done;
    glslang_shader_set_entry_point(shader, entry);
    glslang_shader_set_options(shader, GLSLANG_SHADER_AUTO_MAP_LOCATIONS);
    if (!glslang_shader_preprocess(shader, &input) || !glslang_shader_parse(shader, &input))
    {
        fprintf(stderr, "[recomp] Vulkan HLSL %s: %s\n%s\n", entry, glslang_shader_get_info_log(shader),
                glslang_shader_get_info_debug_log(shader));
        goto done;
    }
    program = glslang_program_create();
    if (!program)
        goto done;
    glslang_program_add_shader(program, shader);
    if (!glslang_program_link(program, input.messages))
    {
        fprintf(stderr, "[recomp] Vulkan HLSL link %s: %s\n", entry, glslang_program_get_info_log(program));
        goto done;
    }
    /* SPIRV-Tools legalizes HLSL resource parameters. Keep its pinned optimizer. */
    glslang_spv_options_t options = {0};
    options.optimize_performance = true;
    options.validate = true;
    glslang_program_SPIRV_generate_with_options(program, input.stage, &options);
    size_t count = glslang_program_SPIRV_get_size(program);
    const char* messages = glslang_program_SPIRV_get_messages(program);
    if (!count || (messages && (strstr(messages, "error") || strstr(messages, "ERROR") || strstr(messages, "Error"))))
    {
        fprintf(stderr, "[recomp] Vulkan SPIR-V %s: %s\n", entry, messages ? messages : "empty output");
        goto done;
    }
    if (count > SIZE_MAX / sizeof(uint32_t))
        goto done;
    uint32_t* data = (uint32_t*)malloc(count * sizeof *data);
    if (!data)
        goto done;
    glslang_program_SPIRV_get(program, data);
    *words = data;
    *word_count = count;
    result = 1;
done:
    if (program)
        glslang_program_delete(program);
    if (shader)
        glslang_shader_delete(shader);
    pthread_mutex_unlock(&compiler_lock);
    free(adapted);
    return result;
}
