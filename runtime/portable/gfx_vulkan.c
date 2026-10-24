/* The graphics back end on Vulkan 1.3 (Linux): what gfx.h asks for, on one VkDevice. The same model
 * as the Metal one (gfx_metal.m), mapped onto Vulkan:
 *
 *   - Frames: one command buffer at a time, submitted at Present (or when the CPU needs a result, or
 *     in chunks of draws); up to three frames in flight, each with its own upload ring - vertex,
 *     index and uniform bytes are copied into the ring per draw, so the game may rewrite a buffer the
 *     moment a draw returns, as D3D lets it. One timeline semaphore counts the submissions; a frame is
 *     done when it reaches the value of the frame's last one.
 *   - Rendering is dynamic (no render pass objects): a pass opens on the first draw or clear after the
 *     targets change, and closes when they change again, at Present, or for a transfer. A clear of the
 *     whole target becomes the next pass's load op; a partial one is vkCmdClearAttachments.
 *   - Every image stays in VK_IMAGE_LAYOUT_GENERAL, and one full memory barrier separates whatever
 *     wrote (a pass, a copy) from what comes next: D3D8's few passes a frame need nothing finer.
 *   - Pipelines come from the generated GLSL (gfx_msl.c, gfx_glsl_generate) compiled to SPIR-V with
 *     glslang, built on worker threads and cached by key. Depth, stencil, culling, depth bias and the
 *     topology are dynamic state, so a key is the functions, blending, and the target formats.
 *   - Bindings are pushed per draw (VK_KHR_push_descriptor), as Metal sets them: the uniforms, the
 *     four vertex streams as storage buffers (the functions fetch their own vertices), the textures.
 *   - Textures: device-local images filled through the ring with a copy - into a command buffer
 *     submitted ahead of the frame's when the frame has not drawn with the texture yet, so an upload
 *     does not break the pass. Formats Vulkan samples differently are views with a swizzle (X8, L8,
 *     A8, A8L8); the 16-bit color ones are widened to BGRA8 on upload, as on Metal.
 *
 * The scene effects (gfx_scene_done), the sun's shadow maps and the water are gfx_metal.m's, ported:
 * the same passes in GLSL (FX_GLSL), the same CPU side (gfx_scene.c). The GPU's time per pass
 * (gfx_pass_timing) is timestamp queries around each pass, read when the frame's slot comes round.
 * The scene renderer's zone and characters (gfx_world_draw, gfx_chars_draw; host/scene.c) are
 * gfx_metal.m's WORLD_MSL and CHARS_MSL as GLSL (WORLD_GLSL, CHARS_GLSL), drawn into the game's
 * targets in its pass, and its sun and moon shadows (gfx_shadow_begin) Metal's too: the casters'
 * passes between the game's and at the frame's end (CAST_GLSL), the receivers' SHADOW_GLSL. Its
 * occlusion (SAO_GLSL, made when the scene is done; AO_GLSL), highlights (SPEC_GLSL) and lamps
 * (LAMP_GLSL: the tiles, the cubes at the frame's end beside the characters' shadow layers, the
 * glows) are Metal's as well, and so are the modern fog (FOG_GLSL, or with the sun's cascades a volume in the shafts'
 * march) and the post effects on the finished scene - bloom (the scene's additive draws recorded and drawn again:
 * glow_new), depth of field and sun shafts (POST_GLSL, SHAFT_GLSL) - and the HDR scene
 * (scene_hdr: the world target made float in place, one tonemap). Its bounce light (gfx_gi_zone, gfx_gi_frame) is
 * Metal's probes as compute passes with ray queries (GI_KERNELS_GLSL), read by the zone and characters (GI_GLSL); the
 * occlusion's rays trace the same structure. */
#include "volk.h" /* Vulkan's functions, loaded at run time: nothing links against the system's loader */

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "vk_mem_alloc.h"
#pragma clang diagnostic pop

#include "cachedir.h"
#include "gfx.h"
#include "gfx_fx.h"
#include "gfx_msl.h"
#include "gfx_scene.h"

#define FRAMES 3
#define GFX_PROBES 8 /* reads of one surface per frame that keep their own history */
#define GFX_READBACKS ((FRAMES + 1) * GFX_PROBES)
#define RING_CHUNK (8u << 20)
#define RING_ALIGN 256 /* >= every min*OffsetAlignment a device may ask for */

/* D3DFORMAT values the back end maps */
enum
{
    F_A8R8G8B8 = 21,
    F_X8R8G8B8 = 22,
    F_R5G6B5 = 23,
    F_X1R5G5B5 = 24,
    F_A1R5G5B5 = 25,
    F_A4R4G4B4 = 26,
    F_A8 = 28,
    F_L8 = 50,
    F_A8L8 = 51,
    F_V8U8 = 60,
    F_D24S8 = 75,
    F_D24X8 = 77,
    F_D16 = 80,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

enum
{
    CONV_NONE,
    CONV_565,
    CONV_X555,
    CONV_1555,
    CONV_4444,
    CONV_F16, /* an 8-bit color target made RGBA16F (scene_hdr): D3D's BGRA8 to and from half floats */
};

/* the bindings of the draw functions' set (gfx_msl.h) */
enum
{
    B_U = 0,
    B_STREAM0 = 1,
    B_SHADOW = 5,
    B_RT_OUT = 6, /* captured for ray tracing (GfxVsKey.shadow 2): the corners written, where and how, the indices */
    B_RT_C = 7,
    B_TEX0 = 8,
    B_WATER_U = 16,
    B_WATER_COL = 17,
    B_WATER_DEPTH = 18,
    B_RT_IDX = 19,
    B_COUNT = 20,
};

#define VK_CHECK(x)                                                                                                   \
    do                                                                                                                \
    {                                                                                                                 \
        VkResult r_ = (x);                                                                                            \
        if (r_ != VK_SUCCESS)                                                                                         \
            fprintf(stderr, "[recomp] gfx: %s failed: %d (%s:%d)\n", #x, (int)r_, __FILE__, __LINE__);               \
    } while (0)

struct GfxTex
{
    VkImage img;
    VmaAllocation mem;
    VkImageView view;    /* what is sampled: every level, swizzled for the formats that need it */
    VkImageView mipview; /* the whole chain of a large render target, once the scene filter filled it */
    VkImageView* rtv;    /* attachment views, one per face and level, made on first use */
    VkFormat vf;
    VkImageAspectFlags aspect;
    int type, use, conv;
    uint32_t fmt, w, h, levels;
    uint32_t block; /* bytes per 4x4 block for the compressed formats, else 0 */
    uint32_t texel; /* bytes per texel in Vulkan's layout */
    uint64_t used;  /* the last frame serial that referenced it */
    uint64_t rec;   /* the command buffer (g_cb_index) that last referenced it */
    int has_stencil, x8;
    uint32_t mips;   /* levels of the image: t->levels, or a whole chain for a large render target */
    uint32_t filled; /* the levels uploaded so far, a bit each */
    uint64_t scene;  /* its mips hold its first level as it is now (the scene filter): 0 once drawn to again */
    VkImageView* levelv; /* sampled views of one level each, made on first use (the occlusion's depth) */
    /* a render target: the depth its first level was last drawn with, and the depth the scene's
     * casters (its world) were last drawn with - what the scene effects read */
    struct GfxTex *depth_seen, *depth_world;
    struct GfxTex *rt_prev, *rt_next; /* the render targets, for a depth surface's destruction to reach */
    /* asynchronous readbacks (gfx_tex_read_async): staging buffers and the frame each was recorded in */
    VkBuffer rb[GFX_READBACKS];
    VmaAllocation rbm[GFX_READBACKS];
    void* rbp[GFX_READBACKS];
    uint32_t rbsize[GFX_READBACKS];
    uint64_t rb_serial[GFX_READBACKS];
    uint32_t rb_face[GFX_READBACKS], rb_level[GFX_READBACKS], rb_index[GFX_READBACKS];
    uint64_t rb_frame;
    uint32_t rb_count;
    uint32_t nlayers; /* a 2D array's layers (the scene renderer's shadow maps; tex_make's array), else 0 */
};

struct GfxBuf
{
    VkBuffer b;
    VmaAllocation mem;
    void* p;
    uint32_t size;
    uint64_t used;
    uint64_t up_last, up_prev; /* the frames of its last two uploads (buf_volatile) */
    /* the bytes it was last filled from, which the caller keeps (gfx.h; d3d8.c: the buffer's own memory),
     * as gfx_d3d12.c keeps them: draw_clip0 reads them instead of p, which is write-combined. The sun
     * cache's copies (cache_copy) still read p: they are made when the scene is done, and by then the
     * game may have written newer bytes here than the draw had */
    const uint8_t* cpu;
    uint32_t cpu_size;
};

typedef struct Chunk
{
    VkBuffer buf;
    VmaAllocation mem;
    uint8_t* p;
    uint32_t used, size;
} Chunk;

/* what a frame let go of, destroyed once the GPU is past it */
typedef struct Trash
{
    int kind; /* 0 image, 1 view, 2 buffer, 3 acceleration structure */
    uint64_t h;
    VmaAllocation mem;
} Trash;

typedef struct Frame
{
    Chunk* chunks;
    uint32_t nchunks, cur;
    VkCommandPool pool;
    VkCommandBuffer* cbs;
    uint32_t ncbs, cbs_used;
    uint64_t serial; /* the frame recorded in this slot */
    uint64_t value;  /* the timeline value of its last submission: done when the semaphore is there */
    Trash* trash;
    uint32_t ntrash, cap;
    VkSemaphore acquired; /* the swapchain image's, waited on by the present's submission */
} Frame;

static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;
static VmaAllocator g_vma;
static VkPhysicalDeviceProperties g_props;
static int g_has_bc, g_has_aniso, g_has_lines, g_has_mirror_once, g_has_large_points;
static int g_has_clamp; /* depth clamped rather than clipped (the scene renderer's shadow casters) */
static int g_has_rt; /* ray queries and acceleration structures, and what rt_capture needs besides (gfx_init) */
static float g_max_aniso;
static VkFormat g_depth_format; /* D24S8 and D24X8: D32_SFLOAT_S8_UINT where the device has it */
static PFN_vkCmdPushDescriptorSetKHR p_push;

static VkSemaphore g_tl; /* the timeline: one step per submission */
static uint64_t g_submits;
static Frame g_frames[FRAMES];
static uint32_t g_frame;
static uint64_t g_serial = 1;  /* the frame being recorded */
static uint64_t g_completed;   /* the last frame the GPU finished */
static int g_frame_open;
static VkCommandBuffer g_cb;   /* the frame's commands being recorded */
/* GPU time per pass (gfx_pass_timing): a timestamp before and after each timed pass into the frame's
 * query pool, two queries a pass, and two for the frame (its first command buffer's start, the
 * present's end); read when its slot comes round again, the frame done (FRAMES - 1 frames late). */
enum { TS_MAX = 128, TS_FRAME = 2 * TS_MAX, TS_QUERIES = TS_FRAME + 2 };
static VkQueryPool g_ts_pool[FRAMES];
static uint8_t g_ts_pass[FRAMES][TS_MAX]; /* each frame's timed passes' GFX_PASS_* in order */
static uint32_t g_ts_n[FRAMES];           /* how many */
static uint8_t g_ts_state[FRAMES];        /* 0 untimed, 1 the pool reset and the start written, 2 the end too */
static uint64_t g_ts_mask;                /* the queue's valid timestamp bits; 0: no timestamps */
static double g_ts_period;                /* ns a tick */
static int g_pass = GFX_PASS_COMP;        /* what the next fx_pass or rt_pass times as */
static int g_pass_next = -1;              /* the next render pass of the game's targets is timed as this (the scene renderer's) */
static int g_pass_ts = -1;                /* the open pass's ts_begin, for its ts_end */
static VkCommandBuffer g_upcb; /* uploads to submit ahead of g_cb */
static uint64_t g_cb_index = 1;
static int g_in_pass, g_dirty;
static int g_up_dirty; /* the uploads' command buffer cleared a new texture since its last barrier */
static uint32_t g_cmd_draws;

static VkDescriptorSetLayout g_dsl, g_util_dsl;
static VkPipelineLayout g_layout, g_util_layout;
static VkPipelineCache g_vkcache;
static VkBuffer g_dummy;
static VmaAllocation g_dummy_mem;
static GfxTex *g_dummy2d, *g_dummycube;
static VkSampler g_lin_samp, g_near_samp, g_cmp_samp; /* the fixed samplers (gfx_init) */

static GfxTex* g_rt;
static uint32_t g_rt_face, g_rt_level;
static GfxTex* g_ds;
static GfxTex* g_scratch_depth;
static uint32_t g_pending_clear; /* D3DCLEAR flags for the next pass's load ops */
static float g_clear_color[4], g_clear_z;
static uint32_t g_clear_stencil;
static VkPipeline g_bound;

/* the window */
static SDL_Window* g_window;
static VkSurfaceKHR g_surface;
static VkSwapchainKHR g_swap;
static VkFormat g_swap_format;
static VkExtent2D g_swap_extent;
static uint32_t g_swap_count;
static VkImage* g_swap_images;
static VkImageView* g_swap_views;
static VkSemaphore* g_swap_done; /* per image: its rendering finished, for the present */
static int g_vsync, g_swap_stale;
static VkPipeline g_present_pipe, g_present_cas_pipe, g_overlay_pipe;
static VkFormat g_present_pipe_format;

/* the frame-rate overlay (g_fxs.fps): presents counted over half-second windows */
static uint64_t g_fps_since;
static uint32_t g_fps_frames;
static char g_fps_text[32] = "-- FPS";

/* --- small hash maps (key bytes -> object) ------------------------------------------------------------- */
typedef struct MapEnt
{
    uint64_t hash;
    void* key;
    size_t klen;
    void* obj;
} MapEnt;

typedef struct Map
{
    MapEnt* e;
    uint32_t cap, n;
} Map;

/* Eight bytes a step: a pipeline's key is a few hundred bytes, looked up one to three times per draw,
 * and FNV's byte at a time was a measurable share of the frame. Only these maps use it (never saved). */
static uint64_t key_hash(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n, w;
    for (; n >= 8; b += 8, n -= 8)
    {
        memcpy(&w, b, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    w = 0;
    memcpy(&w, b, n);
    h = (h ^ w) * 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 29;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 32;
    return h ? h : 1;
}

static void* map_get(Map* m, const void* key, size_t klen)
{
    if (!m->cap)
        return NULL;
    uint64_t h = key_hash(key, klen);
    for (uint32_t i = (uint32_t)h & (m->cap - 1);; i = (i + 1) & (m->cap - 1))
    {
        MapEnt* e = &m->e[i];
        if (!e->hash)
            return NULL;
        if (e->hash == h && e->klen == klen && !memcmp(e->key, key, klen))
            return e->obj;
    }
}

static void map_put(Map* m, const void* key, size_t klen, void* obj)
{
    if ((m->n + 1) * 2 > m->cap)
    {
        Map old = *m;
        m->cap = m->cap ? m->cap * 2 : 256;
        m->e = (MapEnt*)calloc(m->cap, sizeof *m->e);
        m->n = 0;
        for (uint32_t i = 0; i < old.cap; ++i)
            if (old.e[i].hash)
            {
                uint32_t j = (uint32_t)old.e[i].hash & (m->cap - 1);
                while (m->e[j].hash)
                    j = (j + 1) & (m->cap - 1);
                m->e[j] = old.e[i];
                m->n++;
            }
        free(old.e);
    }
    uint64_t h = key_hash(key, klen);
    uint32_t j = (uint32_t)h & (m->cap - 1);
    while (m->e[j].hash)
        j = (j + 1) & (m->cap - 1);
    m->e[j].hash = h;
    m->e[j].key = malloc(klen);
    memcpy(m->e[j].key, key, klen);
    m->e[j].klen = klen;
    m->e[j].obj = obj;
    m->n++;
}

static Map g_pipes, g_samplers;

/* --- the frame profile ------------------------------------------------------------------------------------ */
int gfx_profiling;

typedef struct Prof
{
    uint64_t frames, draws, bytes, pipelines, front_ns, draw_ns, sem_ns, acquire_ns, present_ns, since_ns;
    uint64_t skips[GFX_NSKIPS];
    uint64_t shim_ns, probe_ns;
} Prof;

static Prof g_prof;
static pthread_t g_present_thread;

uint64_t gfx_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason) { if (reason >= 0 && reason < GFX_NSKIPS) g_prof.skips[reason]++; }

void gfx_prof_shim(uint64_t ns)
{
    if (pthread_equal(pthread_self(), g_present_thread))
        g_prof.shim_ns += ns;
}

/* at each Present: every two seconds, the average frame and where it went */
static void prof_frame(uint64_t present_start)
{
    uint64_t now = gfx_now_ns();
    g_prof.present_ns += now - present_start;
    g_prof.frames++;
    if (!g_prof.since_ns)
        g_prof.since_ns = now;
    if (now - g_prof.since_ns < 2000000000ull)
        return;
    double f = (double)g_prof.frames, ms = 1e-6 / f;
    double frame = (double)(now - g_prof.since_ns) * ms;
    double front = (double)g_prof.front_ns * ms, enc = (double)g_prof.draw_ns * ms;
    double sem = (double)g_prof.sem_ns * ms, acq = (double)g_prof.acquire_ns * ms, pres = (double)g_prof.present_ns * ms;
    fprintf(stderr,
        "[gfx] %.1f fps: frame %.2f ms = game %.2f + d3d %.2f (encode %.2f) + present %.2f (gpu wait %.2f, acquire %.2f) | "
        "%.0f draws, %.0f KB up, %llu new pipelines\n",
        f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - front - pres, front, enc, pres, sem, acq,
        (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0, (unsigned long long)g_prof.pipelines);
    static const char* const WHY[GFX_NSKIPS] = { "no shader", "stream>=4", "no position", "no buffer data", "range past buffer",
        "no indices", "pipeline not ready", "no target" };
    int any = 0;
    for (int i = 0; i < GFX_NSKIPS; ++i)
        if (g_prof.skips[i])
            fprintf(stderr, "%s%s %llu", any++ ? ", " : "[gfx]   skipped draws (2 s): ", WHY[i], (unsigned long long)g_prof.skips[i]);
    if (any)
        fprintf(stderr, "\n");
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since_ns = now;
}

static _Atomic uint32_t g_failures;

uint32_t gfx_failures(void) { return atomic_load(&g_failures); }

/* --- buffers in host memory the GPU reads (the ring, static buffers, readbacks) ------------------------- */
static int host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, int readback, VkBuffer* b, VmaAllocation* m, void** p)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai = { 0 };
    ai.usage = readback ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
        (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vma, &bi, &ai, b, m, &info) != VK_SUCCESS)
    {
        fprintf(stderr, "[recomp] gfx: buffer of %llu bytes failed\n", (unsigned long long)size);
        return 0;
    }
    *p = info.pMappedData;
    return 1;
}

#define RING_USAGE                                                                                                    \
    (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |      \
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT)

/* --- frames, command buffers and the upload ring ------------------------------------------------------- */
/* destroyed once the frame that may have used it is done: this one, or between frames (g_frame the next,
 * whose slot is emptied as it opens, while the two before may still be drawing) the last one */
static void trash(int kind, uint64_t h, VmaAllocation mem)
{
    if (!h)
        return;
    Frame* f = &g_frames[g_frame_open ? g_frame : (g_frame + FRAMES - 1) % FRAMES];
    if (f->ntrash == f->cap)
    {
        f->cap = f->cap ? f->cap * 2 : 64;
        f->trash = (Trash*)realloc(f->trash, f->cap * sizeof(Trash));
    }
    f->trash[f->ntrash++] = (Trash){ kind, h, mem };
}

/* the pipelines built for a target's format (fmt_build), each on first use; two formats kept (the world target 8-bit
 * and float, scene_hdr, so turning it on and off builds nothing again): a third replaces the older, its pipelines
 * destroyed once the frame is done */
enum
{
    FP_FX_COMP, /* the effects' composite and anti-aliasing */
    FP_FX_AA,
    FP_BLOOM_COMP, /* the post effects': screened, added (an HDR scene) */
    FP_BLOOM_ADD,
    FP_DOF_COMP,
    FP_SHAFT_COMP,
    FP_SHAFT_ADD,
    FP_SHAFT_DBG,
    FP_SHAFT_VOL, /* the shafts as the fog volume: premultiplied over (built on first use, as those after it) */
    FP_FOG,       /* the modern fog: over by its alpha */
    FP_GLOW,      /* the lamps' glows */
    FP_HDR_COPY, /* the world target into its other format */
    FP_COUNT,
};
static struct
{
    VkFormat fmt[2];
    VkPipeline p[2][FP_COUNT];
    uint32_t tried[2], next;
} g_fmtp;
static VkPipeline fmt_build(VkFormat f, int which);

/* the pipeline which (FP_*) into format f; VK_NULL_HANDLE when it failed (not tried again) */
static VkPipeline fmt_pipe(VkFormat f, int which)
{
    uint32_t i = 0;
    while (i < 2 && !(g_fmtp.fmt[i] == f && f))
        ++i;
    if (i == 2)
    {
        i = g_fmtp.next, g_fmtp.next ^= 1;
        for (int k = 0; k < FP_COUNT; ++k)
            trash(4, (uint64_t)(uintptr_t)g_fmtp.p[i][k], NULL), g_fmtp.p[i][k] = VK_NULL_HANDLE;
        g_fmtp.fmt[i] = f, g_fmtp.tried[i] = 0;
    }
    if (!(g_fmtp.tried[i] >> which & 1))
        g_fmtp.tried[i] |= 1u << which, g_fmtp.p[i][which] = fmt_build(f, which);
    return g_fmtp.p[i][which];
}

static void empty_trash(Frame* f)
{
    for (uint32_t i = 0; i < f->ntrash; ++i)
    {
        Trash* t = &f->trash[i];
        if (t->kind == 0)
            vmaDestroyImage(g_vma, (VkImage)(uintptr_t)t->h, t->mem);
        else if (t->kind == 1)
            vkDestroyImageView(g_dev, (VkImageView)(uintptr_t)t->h, NULL);
        else if (t->kind == 3)
            vkDestroyAccelerationStructureKHR(g_dev, (VkAccelerationStructureKHR)(uintptr_t)t->h, NULL);
        else if (t->kind == 4)
            vkDestroyPipeline(g_dev, (VkPipeline)(uintptr_t)t->h, NULL);
        else
            vmaDestroyBuffer(g_vma, (VkBuffer)(uintptr_t)t->h, t->mem);
    }
    f->ntrash = 0;
}

static void wait_value(uint64_t v)
{
    if (!v)
        return;
    VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g_tl;
    wi.pValues = &v;
    VK_CHECK(vkWaitSemaphores(g_dev, &wi, UINT64_MAX));
}

/* the newest frame the GPU has finished */
static uint64_t completed(void)
{
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_dev, g_tl, &v);
    for (int i = 0; i < FRAMES; ++i)
        if (g_frames[i].serial && g_frames[i].value && g_frames[i].value <= v && g_frames[i].serial > g_completed &&
            g_frames[i].serial < g_serial)
            g_completed = g_frames[i].serial;
    return g_completed;
}

/* a slot's timestamps, its frame done, into the profile's sums */
static void ts_read(uint32_t slot)
{
    uint32_t n = g_ts_n[slot];
    int state = g_ts_state[slot];
    g_ts_n[slot] = 0, g_ts_state[slot] = 0;
    if (!state)
        return;
    uint64_t t[TS_QUERIES];
    if (n && vkGetQueryPoolResults(g_dev, g_ts_pool[slot], 0, 2 * n, 2 * n * sizeof(uint64_t), t, sizeof(uint64_t),
                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
    {
        uint64_t last = 0; /* as gfx_metal.m's: a pass from its start, or the last one's end if later */
        for (uint32_t i = 0; i < n; ++i)
        {
            uint64_t a = t[2 * i] & g_ts_mask, z = t[2 * i + 1] & g_ts_mask;
            if (a < last)
                a = last;
            if (z > a)
                gfx_pass_add(g_ts_pass[slot][i], (uint64_t)((double)(z - a) * g_ts_period));
            if (z > last)
                last = z;
        }
    }
    if (state == 2 && vkGetQueryPoolResults(g_dev, g_ts_pool[slot], TS_FRAME, 2, 2 * sizeof(uint64_t), t, sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
    {
        uint64_t a = t[0] & g_ts_mask, z = t[1] & g_ts_mask;
        if (z > a)
            gfx_pass_gpu((uint64_t)((double)(z - a) * g_ts_period));
    }
    gfx_pass_frame();
}

/* the frame's first command buffer: its pool reset (outside a pass), and the frame's start */
static void ts_frame_start(VkCommandBuffer cb)
{
    if (g_ts_state[g_frame] || !gfx_pass_timing_on())
        return;
    if (!g_ts_mask)
    {
        static int said;
        if (!said++)
            fprintf(stderr, "[recomp] gfx: no GPU timestamps on this queue: no time per pass in the profile\n");
        return;
    }
    vkCmdResetQueryPool(cb, g_ts_pool[g_frame], 0, TS_QUERIES);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_ts_pool[g_frame], TS_FRAME);
    g_ts_state[g_frame] = 1;
}

/* a timed pass's start, before it begins rendering (after gpu_sync's barrier: what came before is
 * done): its index for ts_end, or -1 untimed */
static int ts_begin(VkCommandBuffer cb, int pass)
{
    uint32_t i = g_ts_n[g_frame];
    if (g_ts_state[g_frame] != 1 || i >= TS_MAX)
        return -1;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_ts_pool[g_frame], 2 * i);
    g_ts_pass[g_frame][i] = (uint8_t)pass;
    g_ts_n[g_frame] = i + 1;
    return (int)i;
}

/* its end, after its rendering ended */
static void ts_end(VkCommandBuffer cb, int i)
{
    if (i >= 0)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ts_pool[g_frame], 2 * (uint32_t)i + 1);
}

static void frame_begin(void)
{
    if (g_frame_open)
        return;
    Frame* f = &g_frames[g_frame];
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    wait_value(f->value);
    if (gfx_profiling)
        g_prof.sem_ns += gfx_now_ns() - t0;
    ts_read(g_frame);
    completed();
    empty_trash(f);
    if (f->cbs_used)
        VK_CHECK(vkResetCommandPool(g_dev, f->pool, 0));
    f->cbs_used = 0;
    for (uint32_t i = 0; i < f->nchunks; ++i)
        f->chunks[i].used = 0;
    f->cur = 0;
    f->serial = g_serial;
    f->value = 0;
    g_frame_open = 1;
}

static void full_barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier m = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m, 0, NULL, 0, NULL);
}

static VkCommandBuffer new_cb(void)
{
    frame_begin();
    Frame* f = &g_frames[g_frame];
    if (f->cbs_used == f->ncbs)
    {
        f->cbs = (VkCommandBuffer*)realloc(f->cbs, (f->ncbs + 4) * sizeof(VkCommandBuffer));
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 4;
        VK_CHECK(vkAllocateCommandBuffers(g_dev, &ai, f->cbs + f->ncbs));
        f->ncbs += 4;
    }
    VkCommandBuffer cb = f->cbs[f->cbs_used++];
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

/* the frame's command buffer, begun */
static VkCommandBuffer cmd(void)
{
    if (!g_cb)
    {
        g_cb = new_cb();
        g_dirty = 1; /* the work submitted before it may still be writing */
        g_bound = VK_NULL_HANDLE;
        ts_frame_start(g_cb);
    }
    return g_cb;
}

/* the uploads' command buffer: submitted just before the frame's, so it may only touch what the
 * frame's commands recorded so far have not */
static VkCommandBuffer upcmd(void)
{
    if (!g_upcb)
    {
        g_upcb = new_cb();
        full_barrier(g_upcb);
        g_up_dirty = 0;
    }
    return g_upcb;
}

/* before a transfer or a pass in the frame's command buffer: what wrote before is done */
static void gpu_sync(void)
{
    VkCommandBuffer cb = cmd();
    if (g_dirty)
        full_barrier(cb), g_dirty = 0;
}

/* n bytes of this frame's ring; the buffer and offset for binding */
static const uint8_t* g_ring_base; /* the chunk the last ring() came from: its bytes, how many */
static uint32_t g_ring_size;

static void* ring(size_t n, size_t align, VkBuffer* buf, VkDeviceSize* off)
{
    frame_begin();
    Frame* f = &g_frames[g_frame];
    if (align < 16)
        align = 16;
    for (;; f->cur++)
    {
        if (f->cur == f->nchunks)
        {
            uint32_t size = n + align > RING_CHUNK ? (uint32_t)(n + align) : RING_CHUNK;
            f->chunks = (Chunk*)realloc(f->chunks, (f->nchunks + 1) * sizeof(Chunk));
            Chunk* c = &f->chunks[f->nchunks];
            memset(c, 0, sizeof *c);
            void* p = NULL;
            host_buffer(size, RING_USAGE, 0, &c->buf, &c->mem, &p);
            c->p = (uint8_t*)p;
            c->size = size;
            f->nchunks++;
        }
        Chunk* c = &f->chunks[f->cur];
        uint32_t at = (uint32_t)((c->used + align - 1) & ~(align - 1));
        if (at + n <= c->size)
        {
            c->used = at + (uint32_t)n;
            g_prof.bytes += n;
            *buf = c->buf;
            *off = at;
            g_ring_base = c->p, g_ring_size = c->size;
            return c->p + at;
        }
    }
}

/* n bytes of p into the ring, as a uniform or storage binding takes them */
static VkDescriptorBufferInfo ring_put(const void* p, size_t n)
{
    VkDescriptorBufferInfo bi = { VK_NULL_HANDLE, 0, n };
    memcpy(ring(n, RING_ALIGN, &bi.buffer, &bi.offset), p, n);
    return bi;
}

/* the same, put once a frame while the bytes stay the same (the scene renderer's uniforms, which its zone and character
 * batches share): k the last put and its bytes */
typedef struct RingSame
{
    uint64_t serial;
    VkDescriptorBufferInfo bi;
    uint8_t last[1536];
} RingSame;

static VkDescriptorBufferInfo ring_same(RingSame* k, const void* p, size_t n)
{
    if (n > sizeof k->last)
        return ring_put(p, n);
    if (k->serial != g_serial || k->bi.range != n || memcmp(k->last, p, n))
        k->bi = ring_put(p, n), memcpy(k->last, p, n), k->serial = g_serial;
    return k->bi;
}

static void end_pass(void)
{
    if (g_in_pass)
    {
        vkCmdEndRendering(g_cb);
        g_in_pass = 0;
        g_dirty = 1;
        if (g_pass_ts >= 0)
            ts_end(g_cb, g_pass_ts), g_pass_ts = -1;
    }
}

static int begin_pass(void);

/* closes the pass, first opening one if a clear is still pending, so the clear happens before whatever
 * comes next (a copy, a readback, other targets) */
static void flush_pass(void)
{
    if (g_pending_clear)
        begin_pass();
    end_pass();
}

/* what is recorded to the GPU: the uploads first, then the frame's commands. wait_sem / signal_sem:
 * the present's binary semaphores */
static void submit_ex(int wait, VkSemaphore wait_sem, VkSemaphore signal_sem)
{
    flush_pass();
    VkCommandBuffer cbs[2];
    uint32_t n = 0;
    if (g_upcb)
    {
        full_barrier(g_upcb);
        VK_CHECK(vkEndCommandBuffer(g_upcb));
        cbs[n++] = g_upcb;
    }
    if (g_cb)
    {
        VK_CHECK(vkEndCommandBuffer(g_cb));
        cbs[n++] = g_cb;
    }
    if (!n && !wait_sem && !signal_sem)
        return;
    uint64_t value = ++g_submits;
    uint64_t values[2] = { value, 0 };
    VkSemaphore sig[2] = { g_tl, signal_sem };
    VkTimelineSemaphoreSubmitInfo ti = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    ti.signalSemaphoreValueCount = signal_sem ? 2 : 1;
    ti.pSignalSemaphoreValues = values;
    uint64_t wait_values[1] = { 0 };
    ti.waitSemaphoreValueCount = wait_sem ? 1 : 0;
    ti.pWaitSemaphoreValues = wait_values;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &ti;
    si.commandBufferCount = n;
    si.pCommandBuffers = cbs;
    si.signalSemaphoreCount = signal_sem ? 2 : 1;
    si.pSignalSemaphores = sig;
    si.waitSemaphoreCount = wait_sem ? 1 : 0;
    si.pWaitSemaphores = &wait_sem;
    si.pWaitDstStageMask = &stage;
    VK_CHECK(vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE));
    g_frames[g_frame].value = value;
    g_cb = g_upcb = VK_NULL_HANDLE;
    g_cb_index++;
    g_cmd_draws = 0;
    if (wait)
        wait_value(value);
}

static void submit(int wait) { submit_ex(wait, VK_NULL_HANDLE, VK_NULL_HANDLE); }

/* --- formats ---------------------------------------------------------------------------------------------- */
static VkFormat vk_format(uint32_t fmt, int use, int* conv, uint32_t* block, uint32_t* texel, VkComponentMapping* sw)
{
    *conv = CONV_NONE, *block = 0, *texel = 4;
    *sw = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY };
    if (use == GFX_USE_DEPTH)
        return fmt == F_D16 ? VK_FORMAT_D16_UNORM : g_depth_format;
    switch (fmt)
    {
    case F_A8R8G8B8: return VK_FORMAT_B8G8R8A8_UNORM;
    case F_X8R8G8B8: sw->a = VK_COMPONENT_SWIZZLE_ONE; return VK_FORMAT_B8G8R8A8_UNORM;
    case F_R5G6B5: *conv = CONV_565; return VK_FORMAT_B8G8R8A8_UNORM;
    case F_X1R5G5B5: *conv = CONV_X555; return VK_FORMAT_B8G8R8A8_UNORM;
    case F_A1R5G5B5: *conv = CONV_1555; return VK_FORMAT_B8G8R8A8_UNORM;
    case F_A4R4G4B4: *conv = CONV_4444; return VK_FORMAT_B8G8R8A8_UNORM;
    case F_A8:
        *texel = 1;
        *sw = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
        return VK_FORMAT_R8_UNORM;
    case F_L8:
        *texel = 1;
        *sw = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
        return VK_FORMAT_R8_UNORM;
    case F_A8L8:
        *texel = 2;
        *sw = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G };
        return VK_FORMAT_R8G8_UNORM;
    case F_V8U8: *texel = 2; return VK_FORMAT_R8G8_SNORM;
    }
    if (fmt == FOURCC('D', 'X', 'T', '1'))
        return *block = 8, VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    if (fmt == FOURCC('D', 'X', 'T', '2') || fmt == FOURCC('D', 'X', 'T', '3'))
        return *block = 16, VK_FORMAT_BC2_UNORM_BLOCK;
    if (fmt == FOURCC('D', 'X', 'T', '4') || fmt == FOURCC('D', 'X', 'T', '5'))
        return *block = 16, VK_FORMAT_BC3_UNORM_BLOCK;
    return VK_FORMAT_B8G8R8A8_UNORM;
}

static uint32_t d3d_bpp(uint32_t fmt)
{
    switch (fmt)
    {
    case F_A8:
    case F_L8: return 1;
    case F_R5G6B5:
    case F_X1R5G5B5:
    case F_A1R5G5B5:
    case F_A4R4G4B4:
    case F_A8L8:
    case F_V8U8: return 2;
    default: return 4;
    }
}

static uint32_t expand(uint32_t v, int bits)
{
    return bits == 1 ? (v ? 255u : 0u) : (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0));
}

static uint16_t f16_from_unit(uint32_t v) /* 0..255 / 255 as a half float */
{
    if (!v)
        return 0;
    float f = (float)v / 255.0f;
    int e;
    float m = frexpf(f, &e); /* f = m * 2^e, m in [0.5, 1) */
    if (e < -13) /* (a subnormal half; 1/255 isn't one) */
        return (uint16_t)lrintf(f * 16777216.0f);
    return (uint16_t)(((uint32_t)(e + 14) << 10) + ((uint32_t)lrintf(m * 2048.0f) - 1024u)); /* (a carry: the next exponent) */
}

static uint8_t unit_from_f16(uint16_t h) /* a half float saturated to 0..1, as a byte */
{
    uint32_t e = h >> 10 & 31, m = h & 1023;
    if (h & 0x8000)
        return 0;
    if (e == 31)
        return m ? 0 : 255;
    float f = e ? ldexpf((float)(m | 1024), (int)e - 25) : ldexpf((float)m, -24);
    return f >= 1.0f ? 255 : (uint8_t)lrintf(f * 255.0f);
}

/* one row of a 16-bit D3D format into BGRA8 (CONV_F16: BGRA8 into RGBA16F) */
static void convert_row(int conv, const uint8_t* src, uint8_t* dst, uint32_t w)
{
    if (conv == CONV_F16)
    {
        for (uint32_t x = 0; x < w; ++x)
        {
            const uint8_t* p = src + 4 * x;
            uint16_t* d = (uint16_t*)(dst + 8 * x);
            d[0] = f16_from_unit(p[2]), d[1] = f16_from_unit(p[1]), d[2] = f16_from_unit(p[0]), d[3] = f16_from_unit(p[3]);
        }
        return;
    }
    for (uint32_t x = 0; x < w; ++x)
    {
        uint32_t p = (uint32_t)src[2 * x] | ((uint32_t)src[2 * x + 1] << 8), r, g, b, a;
        switch (conv)
        {
        case CONV_565: r = expand(p >> 11, 5), g = expand((p >> 5) & 63, 6), b = expand(p & 31, 5), a = 255; break;
        case CONV_X555: r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = 255; break;
        case CONV_1555:
            r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = (p & 0x8000) ? 255 : 0;
            break;
        default: r = (p >> 8 & 15) * 17, g = (p >> 4 & 15) * 17, b = (p & 15) * 17, a = (p >> 12) * 17; break;
        }
        dst[4 * x] = (uint8_t)b, dst[4 * x + 1] = (uint8_t)g, dst[4 * x + 2] = (uint8_t)r, dst[4 * x + 3] = (uint8_t)a;
    }
}

/* --- static buffers --------------------------------------------------------------------------------------------- */
#define STATIC_USAGE (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT)

static void sun_cache_forget(VkBuffer buf);

/* a static buffer the game keeps rewriting (uploaded in two frames within the last 300): what is drawn
 * from it cannot be drawn again later from it, so the sun's cache keeps a copy instead */
static int buf_volatile(const GfxBuf* b)
{
    return b->up_prev && b->up_last - b->up_prev <= 300 && g_serial - b->up_last <= 300;
}

GfxBuf* gfx_buf_create(uint32_t size)
{
    if (!g_dev || !size)
        return NULL;
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    b->size = size;
    if (!host_buffer((size + 3) & ~3u, STATIC_USAGE, 0, &b->b, &b->mem, &b->p))
    {
        free(b);
        return NULL;
    }
    return b;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    sun_cache_forget(b->b);
    trash(2, (uint64_t)(uintptr_t)b->b, b->mem);
    free(b);
}

void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b)
        return;
    if (size > b->size)
        size = b->size;
    sun_cache_forget(b->b); /* new contents: the cache's copy of what it drew is no longer it */
    b->cpu = (const uint8_t*)data, b->cpu_size = size;
    if (b->up_last != g_serial)
        b->up_prev = b->up_last, b->up_last = g_serial;
    if (b->used > completed())
    {
        /* recorded or running work still reads the old contents: rename */
        trash(2, (uint64_t)(uintptr_t)b->b, b->mem);
        host_buffer((b->size + 3) & ~3u, STATIC_USAGE, 0, &b->b, &b->mem, &b->p);
    }
    memcpy(b->p, data, size);
}

/* --- textures ------------------------------------------------------------------------------------------------ */
static void level_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = t->w >> level ? t->w >> level : 1;
    *h = (t->type == GFX_TEX_CUBE ? t->w : t->h) >> level;
    if (!*h)
        *h = 1;
}

static uint32_t layers(const GfxTex* t) { return t->nlayers ? t->nlayers : t->type == GFX_TEX_CUBE ? 6 : 1; }

static VkImageView make_view(const GfxTex* t, VkImageViewType type, uint32_t level0, uint32_t nlevels, uint32_t layer0,
    uint32_t nlayers, const VkComponentMapping* sw, VkImageAspectFlags aspect)
{
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->img;
    vi.viewType = type;
    vi.format = t->vf;
    if (sw)
        vi.components = *sw;
    vi.subresourceRange = (VkImageSubresourceRange){ aspect, level0, nlevels, layer0, nlayers };
    VkImageView v = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(g_dev, &vi, NULL, &v));
    return v;
}

static GfxTex* g_rts; /* the render targets (GfxTex.rt_next) */

/* force: the format (the effects' own targets, the float world target), else the D3D format's; plain: no mip chain
 * for a large render target (the effects' own); array: a 2D array of that many layers (its view one), else 0; usage:
 * more than use's (a storage image) */
static GfxTex* tex_make(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use, VkFormat force, int plain,
    uint32_t array, VkImageUsageFlags usage)
{
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    VkComponentMapping sw;
    t->vf = vk_format(fmt, use, &t->conv, &t->block, &t->texel, &sw);
    if (force && force != t->vf)
    {
        t->vf = force;
        /* a target made float keeps the game's reads and writes BGRA8, converted (its readbacks sized by texel) */
        if (force == VK_FORMAT_R16G16B16A16_SFLOAT && use == GFX_USE_RT && t->texel == 4)
            t->texel = 8, t->conv = CONV_F16;
    }
    t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->levels = levels ? levels : 1;
    t->nlayers = array;
    t->has_stencil = use == GFX_USE_DEPTH && (t->vf == VK_FORMAT_D32_SFLOAT_S8_UINT || t->vf == VK_FORMAT_D24_UNORM_S8_UINT);
    t->x8 = fmt == F_X8R8G8B8;
    t->aspect = use == GFX_USE_DEPTH ? VK_IMAGE_ASPECT_DEPTH_BIT | (t->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                                     : VK_IMAGE_ASPECT_COLOR_BIT;
    if (t->block && !g_has_bc)
    {
        fprintf(stderr, "[recomp] gfx: the device has no BC texture compression: DXT textures stay empty\n");
        t->vf = VK_FORMAT_B8G8R8A8_UNORM; /* TODO: decode on the CPU */
    }
    t->mips = t->levels;
    /* a large color target (FFXI's background) gets a whole mip chain: the finished scene is made
     * smaller through it rather than one bilinear sample per screen pixel (scene_mips) */
    if (use == GFX_USE_RT && !plain && type == GFX_TEX_2D && t->levels == 1 && t->w >= 1024 && t->h >= 1024)
        while ((t->w >> t->mips) || (t->h >> t->mips))
            t->mips++;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.flags = type == GFX_TEX_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = t->vf;
    ii.extent = (VkExtent3D){ t->w, type == GFX_TEX_CUBE ? t->w : t->h, 1 };
    ii.mipLevels = t->mips;
    ii.arrayLayers = layers(t);
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (use == GFX_USE_RT)
        ii.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    else if (use == GFX_USE_DEPTH)
        ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.usage |= usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = { 0 };
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(g_vma, &ii, &ai, &t->img, &t->mem, NULL) != VK_SUCCESS)
    {
        fprintf(stderr, "[recomp] gfx: texture %ux%u format %u failed\n", w, h, fmt);
        free(t);
        return NULL;
    }
    VkImageViewType vt = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : type == GFX_TEX_CUBE ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    if (use == GFX_USE_DEPTH) /* sampled (the scene effects): depth alone */
        t->view = make_view(t, vt, 0, t->mips, 0, layers(t), NULL, VK_IMAGE_ASPECT_DEPTH_BIT);
    else
    {
        /* the game's draws see the levels it made: the rest of a large target's chain holds nothing
         * until the scene filter builds it */
        t->view = make_view(t, vt, 0, t->levels, 0, layers(t), &sw, VK_IMAGE_ASPECT_COLOR_BIT);
        if (t->mips > t->levels)
            t->mipview = make_view(t, vt, 0, t->mips, 0, layers(t), &sw, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (use != GFX_USE_SAMPLE)
        t->rtv = (VkImageView*)calloc((size_t)layers(t) * t->mips, sizeof(VkImageView));
    if (use == GFX_USE_RT)
    {
        t->rt_next = g_rts;
        if (g_rts)
            g_rts->rt_prev = t;
        g_rts = t;
    }
    /* into GENERAL, where it stays; cleared, so a level read before it is written reads zeros */
    VkCommandBuffer cb = upcmd();
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->img;
    b.subresourceRange = (VkImageSubresourceRange){ t->aspect, 0, t->mips, 0, layers(t) };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    if (use == GFX_USE_DEPTH)
    {
        VkClearDepthStencilValue z = { 1.0f, 0 };
        vkCmdClearDepthStencilImage(cb, t->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    }
    else if (!t->block || !g_has_bc)
    {
        VkClearColorValue c = { { 0, 0, 0, 0 } };
        vkCmdClearColorImage(cb, t->img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &b.subresourceRange);
    }
    g_up_dirty = 1;
    return t;
}

static GfxTex* tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use, VkFormat force, int plain)
{
    return tex_make(type, fmt, w, h, levels, use, force, plain, 0, 0);
}

GfxTex* gfx_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    return tex_create(type, fmt, w, h, levels, use, VK_FORMAT_UNDEFINED, 0);
}

static void sun_cache_forget_tex(const GfxTex* t);
static void water_forget(const GfxTex* t);
static void sao_forget(const GfxTex* t);
static void glows_forget(const GfxTex* t);
static void hdr_forget(const GfxTex* t);

/* what the effects keep of t (their caches and history) let go: t goes, or its image changes */
static void tex_forget(const GfxTex* t)
{
    sun_cache_forget_tex(t);
    glows_forget(t);
    hdr_forget(t);
    water_forget(t);
    sao_forget(t);
}

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (g_rt == t)
        end_pass(), g_rt = NULL;
    if (g_ds == t)
        end_pass(), g_ds = NULL;
    if (t->use == GFX_USE_RT)
    {
        if (t->rt_prev)
            t->rt_prev->rt_next = t->rt_next;
        else
            g_rts = t->rt_next;
        if (t->rt_next)
            t->rt_next->rt_prev = t->rt_prev;
    }
    else if (t->use == GFX_USE_DEPTH) /* no render target remembers it */
        for (GfxTex* r = g_rts; r; r = r->rt_next)
        {
            if (r->depth_seen == t)
                r->depth_seen = NULL;
            if (r->depth_world == t)
                r->depth_world = NULL;
        }
    tex_forget(t);
    if (t->levelv)
        for (uint32_t i = 0; i < t->mips; ++i)
            trash(1, (uint64_t)(uintptr_t)t->levelv[i], NULL);
    free(t->levelv);
    trash(1, (uint64_t)(uintptr_t)t->view, NULL);
    trash(1, (uint64_t)(uintptr_t)t->mipview, NULL);
    if (t->rtv)
        for (uint32_t i = 0; i < layers(t) * t->mips; ++i)
            trash(1, (uint64_t)(uintptr_t)t->rtv[i], NULL);
    free(t->rtv);
    trash(0, (uint64_t)(uintptr_t)t->img, t->mem);
    for (int i = 0; i < GFX_READBACKS; ++i)
        trash(2, (uint64_t)(uintptr_t)t->rb[i], t->rbm[i]);
    free(t);
}

/* the command buffer a transfer touching t goes into: the uploads' when the frame has not used t yet,
 * else the frame's own, outside a pass */
static VkCommandBuffer transfer_cb(GfxTex* t)
{
    if (t->rec != g_cb_index || !g_cb)
    {
        VkCommandBuffer cb = upcmd();
        if (g_up_dirty) /* a texture's clear at creation before its first upload */
            full_barrier(cb), g_up_dirty = 0;
        return cb;
    }
    flush_pass();
    gpu_sync();
    g_dirty = 1;
    return g_cb;
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels || !w || !h)
        return;
    if (level < 32)
        t->filled |= 1u << level;
    uint32_t lw, lh;
    level_size(t, level, &lw, &lh);
    if (x >= lw || y >= lh)
        return;
    if (x + w > lw)
        w = lw - x;
    if (y + h > lh)
        h = lh - y;
    if (t->block && !g_has_bc)
        return;
    uint32_t rows = t->block ? (h + 3) / 4 : h;
    uint32_t row_bytes = t->block ? ((w + 3) / 4) * t->block : w * t->texel;
    VkBuffer buf;
    VkDeviceSize off;
    uint8_t* dst = (uint8_t*)ring((size_t)row_bytes * rows, 16, &buf, &off);
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t r = 0; r < rows; ++r)
    {
        if (t->conv)
            convert_row(t->conv, s + (size_t)r * pitch, dst + (size_t)r * row_bytes, w);
        else
            memcpy(dst + (size_t)r * row_bytes, s + (size_t)r * pitch, row_bytes);
    }
    VkBufferImageCopy c = { 0 };
    c.bufferOffset = off;
    c.imageSubresource = (VkImageSubresourceLayers){ t->aspect, level, face, 1 };
    c.imageOffset = (VkOffset3D){ (int32_t)x, (int32_t)y, 0 };
    c.imageExtent = (VkExtent3D){ w, h, 1 };
    vkCmdCopyBufferToImage(transfer_cb(t), buf, t->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    t->used = g_serial;
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    gfx_tex_upload_rect(t, face, level, 0, 0, w, h, src, pitch);
}

/* rows of a level read back in Vulkan's layout -> D3D's */
static void copy_out(const GfxTex* t, const uint8_t* s, uint32_t w, uint32_t h, uint32_t row, void* dst, uint32_t pitch)
{
    uint32_t bpp = d3d_bpp(t->fmt);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t* d = (uint8_t*)dst + (size_t)y * pitch;
        if (!t->conv)
            memcpy(d, s + (size_t)y * row, (size_t)w * bpp);
        else if (t->conv == CONV_F16) /* the float world target: its colors saturated, as the tonemap's input */
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint16_t* p = (const uint16_t*)(s + (size_t)y * row + 8 * x);
                d[4 * x] = unit_from_f16(p[2]), d[4 * x + 1] = unit_from_f16(p[1]), d[4 * x + 2] = unit_from_f16(p[0]);
                d[4 * x + 3] = unit_from_f16(p[3]);
            }
        else /* 16-bit color targets: back from BGRA8 */
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* p = s + (size_t)y * row + 4 * x;
                uint32_t b = p[0], g = p[1], r = p[2], a = p[3], v;
                switch (t->conv)
                {
                case CONV_565: v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
                case CONV_4444: v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; break;
                default: v = (a >= 128 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
                }
                d[2 * x] = (uint8_t)v, d[2 * x + 1] = (uint8_t)(v >> 8);
            }
    }
}

/* a copy of the level into buf, recorded with the frame's other work */
static void queue_readback(GfxTex* t, uint32_t face, uint32_t level, uint32_t w, uint32_t h, VkBuffer buf)
{
    flush_pass();
    gpu_sync();
    VkBufferImageCopy c = { 0 };
    c.imageSubresource = (VkImageSubresourceLayers){ t->aspect, level, face, 1 };
    c.imageExtent = (VkExtent3D){ w, h, 1 };
    vkCmdCopyImageToBuffer(g_cb, t->img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    /* the host reads it after the submission's wait */
    VkMemoryBarrier m = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &m, 0, NULL, 0, NULL);
    g_dirty = 1;
    t->used = g_serial, t->rec = g_cb_index;
}

void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || t->use == GFX_USE_DEPTH || t->block)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    uint32_t row = w * t->texel;
    VkBuffer buf;
    VmaAllocation mem;
    void* p;
    if (!host_buffer((VkDeviceSize)row * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &buf, &mem, &p))
        return;
    queue_readback(t, face, level, w, h, buf);
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    submit(1);
    if (gfx_profiling)
        g_prof.probe_ns += gfx_now_ns() - t0;
    vmaInvalidateAllocation(g_vma, mem, 0, VK_WHOLE_SIZE);
    copy_out(t, (const uint8_t*)p, w, h, row, dst, pitch);
    vmaDestroyBuffer(g_vma, buf, mem);
}

/* A surface read several times a frame keeps each read's history apart (gfx_metal.m's note): the k-th
 * read this frame gets the k-th read of the newest frame the GPU has finished. */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || t->use == GFX_USE_DEPTH || t->block)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    uint32_t row = w * t->texel;
    if (t->rb_frame != g_serial)
        t->rb_frame = g_serial, t->rb_count = 0;
    uint32_t index = t->rb_count++;
    if (index >= GFX_PROBES)
    {
        gfx_tex_read(t, face, level, dst, pitch);
        return;
    }
    uint64_t done = completed();
    int best = -1;
    for (int i = 0; i < GFX_READBACKS; ++i)
        if (t->rb[i] && t->rb_serial[i] && t->rb_serial[i] <= done && t->rb_index[i] == index && t->rb_face[i] == face &&
            t->rb_level[i] == level && (best < 0 || t->rb_serial[i] > t->rb_serial[best]))
            best = i;
    if (best < 0)
        gfx_tex_read(t, face, level, dst, pitch); /* nothing finished for this read yet: wait, once */
    else
    {
        vmaInvalidateAllocation(g_vma, t->rbm[best], 0, VK_WHOLE_SIZE);
        copy_out(t, (const uint8_t*)t->rbp[best], w, h, row, dst, pitch);
    }
    int slot = -1;
    for (int i = 0; i < GFX_READBACKS && slot < 0; ++i)
        if (i != best && (!t->rb_serial[i] || t->rb_serial[i] <= done))
            slot = i;
    if (slot < 0)
        return;
    if (!t->rb[slot] || t->rbsize[slot] < row * h)
    {
        if (t->rb[slot])
            trash(2, (uint64_t)(uintptr_t)t->rb[slot], t->rbm[slot]);
        t->rb[slot] = VK_NULL_HANDLE;
        if (!host_buffer((VkDeviceSize)row * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &t->rb[slot], &t->rbm[slot], &t->rbp[slot]))
            return;
        t->rbsize[slot] = row * h;
    }
    queue_readback(t, face, level, w, h, t->rb[slot]);
    t->rb_serial[slot] = g_serial, t->rb_face[slot] = face, t->rb_level[slot] = level, t->rb_index[slot] = index;
}

void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || src->vf != dst->vf || src->aspect != dst->aspect || !w || !h)
        return;
    flush_pass();
    gpu_sync();
    VkImageCopy c = { 0 };
    c.srcSubresource = (VkImageSubresourceLayers){ src->aspect, slevel, sface, 1 };
    c.srcOffset = (VkOffset3D){ (int32_t)sx, (int32_t)sy, 0 };
    c.dstSubresource = (VkImageSubresourceLayers){ dst->aspect, dlevel, dface, 1 };
    c.dstOffset = (VkOffset3D){ (int32_t)dx, (int32_t)dy, 0 };
    c.extent = (VkExtent3D){ w, h, 1 };
    vkCmdCopyImage(g_cb, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    g_dirty = 1;
    src->used = dst->used = g_serial;
    src->rec = dst->rec = g_cb_index;
    if (!dlevel)
        dst->scene = 0;
}

/* --- render passes --------------------------------------------------------------------------------------------- */
/* Draws recorded since the last submission. A frame goes to the GPU in chunks - submitted when a pass
 * ends with this many behind it - so the GPU works while the game builds the rest, and a mid-frame
 * readback (the game's per-frame probe) waits only for the tail. */
#define CHUNK_DRAWS 96
#define SPLIT_DRAWS 320

static void color_size(uint32_t* w, uint32_t* h) { level_size(g_rt, g_rt_level, w, h); }

static VkImageView attachment_view(GfxTex* t, uint32_t face, uint32_t level)
{
    VkImageView* v = &t->rtv[face * t->mips + level];
    if (!*v)
        *v = make_view(t, VK_IMAGE_VIEW_TYPE_2D, level, 1, face, 1, NULL, t->aspect);
    return *v;
}

/* the depth attachment for the current color target: the bound one when it is at least as large, or
 * a scratch one of the color target's size */
static GfxTex* depth_attachment(void)
{
    if (!g_ds || !g_rt)
        return NULL;
    uint32_t w, h;
    color_size(&w, &h);
    if (g_ds->w >= w && g_ds->h >= h)
        return g_ds;
    if (!g_scratch_depth || g_scratch_depth->w != w || g_scratch_depth->h != h || g_scratch_depth->vf != g_ds->vf)
    {
        gfx_tex_destroy(g_scratch_depth);
        g_scratch_depth = tex_create(GFX_TEX_2D, g_ds->fmt, w, h, 1, GFX_USE_DEPTH, g_ds->vf, 1);
    }
    return g_scratch_depth;
}

static int begin_pass(void)
{
    if (g_in_pass)
        return 1;
    if (!g_rt)
        return 0;
    gpu_sync();
    uint32_t w, h;
    color_size(&w, &h);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = attachment_view(g_rt, g_rt_face, g_rt_level);
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = (g_pending_clear & 1) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memcpy(ca.clearValue.color.float32, g_clear_color, sizeof g_clear_color);
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO }, sa = da;
    GfxTex* depth = depth_attachment();
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ w, h };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (depth)
    {
        da.imageView = attachment_view(depth, 0, 0);
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = (g_pending_clear & 2) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = g_clear_z;
        ri.pDepthAttachment = &da;
        if (depth->has_stencil)
        {
            sa = da;
            sa.loadOp = (g_pending_clear & 4) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            sa.clearValue.depthStencil.stencil = g_clear_stencil;
            ri.pStencilAttachment = &sa;
        }
        depth->used = g_serial, depth->rec = g_cb_index;
        if (!g_rt_face && !g_rt_level)
            g_rt->depth_seen = depth;
    }
    g_pending_clear = 0;
    if (g_pass_next >= 0)
        g_pass_ts = ts_begin(g_cb, g_pass_next), g_pass_next = -1;
    vkCmdBeginRendering(g_cb, &ri);
    g_in_pass = 1;
    g_bound = VK_NULL_HANDLE;
    g_rt->used = g_serial, g_rt->rec = g_cb_index;
    if (!g_rt_face && !g_rt_level)
        g_rt->scene = 0; /* drawn to: its mips are behind */
    return 1;
}

void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (color == g_rt && face == g_rt_face && level == g_rt_level && depth == g_ds)
        return;
    if (g_dev)
        flush_pass(); /* a clear nothing drew after still happens */
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
    if (g_in_pass)
        end_pass();
    if (g_cmd_draws >= CHUNK_DRAWS && g_cb)
        submit(0);
}

/* --- shaders and pipelines, built off the game's thread ---------------------------------------------------
 * A pipeline for a key seen for the first time is built on a worker thread; the draws that need it are
 * skipped until it is ready (the game never waits for the compiler). Every key built is recorded in
 * the pipeline cache file and built again at start-up, as gfx_metal.m does; the driver's own cache
 * (VkPipelineCache, saved beside it) makes those builds quick. */
typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

static int alpha_tested(const GfxFsKey* k) { return k->alpha_func && k->alpha_func != 8; }

typedef struct PipeKey
{
    LibKey lib;
    GfxPipeKey pipe;
    uint32_t color, depth, stencil; /* VkFormat */
    uint8_t x8, topo, fill, pad;    /* topo: 0 points, 1 lines, 2 triangles; fill: 1 lines */
} PipeKey;

#define PIPE_FAILED ((void*)1)
#define PIPE_MAGIC 0x314B5056u /* "VPK1" */

typedef struct PipeEntry
{
    void* _Atomic state; /* NULL while building, PIPE_FAILED, or the VkPipeline */
} PipeEntry;

typedef struct PipeJob
{
    PipeKey k;
    uint32_t *vs, *ps;
    uint32_t nvs, nps;
    PipeEntry* e;
    int record;
    struct PipeJob* next;
} PipeJob;

static int g_sync_pipelines;
static char g_pipe_cache[1024], g_vk_cache_file[1024];
static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER, g_file_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;
static PipeJob *g_jobs, *g_jobs_tail;
static int g_workers;

void gfx_set_sync_pipelines(int on) { g_sync_pipelines = on; }

static uint32_t* copy_tok(const uint32_t* t, uint32_t* n)
{
    *n = 0;
    if (!t)
        return NULL;
    uint32_t k = 0;
    while (k < 65536 && t[k] != 0x0000FFFFu)
        k++;
    k++;
    uint32_t* c = (uint32_t*)malloc(4u * k);
    memcpy(c, t, 4u * k);
    *n = k;
    return c;
}

/* GLSL to a shader module: one stage of a text with both (GFX_VS / GFX_FS) */
/* text as SPIR-V: a vertex (0), fragment (1) or compute (2) function, GFX_VS, GFX_FS or GFX_CS defined */
static VkShaderModule compile_glsl(const char* text, int fragment)
{
    size_t n = strlen(text);
    char* src = (char*)malloc(n + 64);
    snprintf(src, n + 64, "#version 460\n#define %s\n%s", fragment == 2 ? "GFX_CS" : fragment ? "GFX_FS" : "GFX_VS", text);
    glslang_stage_t stage = fragment == 2 ? GLSLANG_STAGE_COMPUTE : fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    glslang_input_t in = { 0 };
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = stage;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_3;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_6;
    in.code = src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT;
    in.resource = glslang_default_resource();
    VkShaderModule m = VK_NULL_HANDLE;
    glslang_shader_t* sh = glslang_shader_create(&in);
    glslang_program_t* prog = NULL;
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in))
    {
        fprintf(stderr, "[recomp] gfx: GLSL compile failed: %s\n%s\n", glslang_shader_get_info_log(sh), src);
        goto done;
    }
    prog = glslang_program_create();
    glslang_program_add_shader(prog, sh);
    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))
    {
        fprintf(stderr, "[recomp] gfx: GLSL link failed: %s\n", glslang_program_get_info_log(prog));
        goto done;
    }
    glslang_program_SPIRV_generate(prog, stage);
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = glslang_program_SPIRV_get_size(prog) * 4;
    mi.pCode = glslang_program_SPIRV_get_ptr(prog);
    VK_CHECK(vkCreateShaderModule(g_dev, &mi, NULL, &m));
done:
    if (prog)
        glslang_program_delete(prog);
    glslang_shader_delete(sh);
    free(src);
    if (!m)
        atomic_fetch_add(&g_failures, 1);
    return m;
}

static VkBlendFactor blend_factor(uint32_t f, int x8)
{
    switch (f)
    {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return x8 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return x8 ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

/* a pipeline from two modules (fs may be null: depth alone) and the fixed state the key gives */
/* make_pipeline into ncolor colour targets (blend one each) */
static VkPipeline make_pipeline_n(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, const VkPipelineColorBlendAttachmentState* blend,
    const VkFormat* color, uint32_t ncolor, VkFormat depth, VkFormat stencil, VkPrimitiveTopology topo, VkPolygonMode fill,
    const VkDynamicState* dyn, uint32_t ndyn, int discard)
{
    /* discard: 1 the rasterizer discards (ray tracing's capture: the vertex function's writes alone), 2 the depth
     * clamped rather than clipped where the device can (the scene renderer's shadow casters) */
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = vs, st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = fs, st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = topo;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = fill;
    rs.rasterizerDiscardEnable = (discard & 1) ? VK_TRUE : VK_FALSE;
    rs.depthClampEnable = (discard & 2) && g_has_clamp ? VK_TRUE : VK_FALSE;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.front.compareOp = ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = ncolor; /* none: depth alone (the sun's maps) */
    cb.pAttachments = blend;
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = ndyn;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = ncolor;
    ri.pColorAttachmentFormats = color;
    ri.depthAttachmentFormat = depth;
    ri.stencilAttachmentFormat = stencil;
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.pNext = &ri;
    pi.stageCount = fs ? 2 : 1;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_dev, g_vkcache, 1, &pi, NULL, &p) != VK_SUCCESS)
    {
        atomic_fetch_add(&g_failures, 1);
        fprintf(stderr, "[recomp] gfx: pipeline failed\n");
        return VK_NULL_HANDLE;
    }
    return p;
}

static VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, const VkPipelineColorBlendAttachmentState* blend,
    VkFormat color, VkFormat depth, VkFormat stencil, VkPrimitiveTopology topo, VkPolygonMode fill, const VkDynamicState* dyn,
    uint32_t ndyn, int discard)
{
    return make_pipeline_n(vs, fs, layout, blend, &color, color ? 1 : 0, depth, stencil, topo, fill, dyn, ndyn, discard);
}

static const VkDynamicState DRAW_DYNAMIC[] = {
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_DEPTH_BIAS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE,
    VK_DYNAMIC_STATE_FRONT_FACE,
    VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
    VK_DYNAMIC_STATE_STENCIL_OP,
    VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
};

/* The modules for a pair of shader keys, compiled once: pipelines that differ only in blending, their
 * attachments or topology share them (the source is the keys' alone). Kept for the session, g_libs
 * under g_lib_lock: the pipelines are built on several workers. */
typedef struct Lib
{
    VkShaderModule vm, fm; /* fm VK_NULL_HANDLE for depth alone */
    int ok;
} Lib;

static Map g_libs;
static pthread_mutex_t g_lib_lock = PTHREAD_MUTEX_INITIALIZER;

static const Lib* library(const LibKey* k, const uint32_t* vs, const uint32_t* ps)
{
    pthread_mutex_lock(&g_lib_lock);
    Lib* l = (Lib*)map_get(&g_libs, k, sizeof *k);
    pthread_mutex_unlock(&g_lib_lock);
    if (l)
        return l;
    l = (Lib*)calloc(1, sizeof *l);
    char* src = gfx_glsl_generate(&k->vs, &k->fs, vs, ps);
    if (src)
    {
        /* captured for ray tracing: no fragments at all. (The sun's maps draw depth alone unless alpha-tested,
         * but the bounce light's draws its casters in colour: build_pipeline leaves the function out.) */
        int none = k->vs.shadow == 2;
        l->vm = compile_glsl(src, 0);
        l->fm = l->vm && !none ? compile_glsl(src, 1) : VK_NULL_HANDLE;
        l->ok = l->vm && (l->fm || none);
        free(src);
    }
    else
        atomic_fetch_add(&g_failures, 1);
    pthread_mutex_lock(&g_lib_lock);
    Lib* had = (Lib*)map_get(&g_libs, k, sizeof *k);
    if (had) /* another worker compiled it meanwhile */
    {
        if (l->vm)
            vkDestroyShaderModule(g_dev, l->vm, NULL);
        if (l->fm)
            vkDestroyShaderModule(g_dev, l->fm, NULL);
        free(l);
        l = had;
    }
    else
        map_put(&g_libs, k, sizeof *k, l);
    pthread_mutex_unlock(&g_lib_lock);
    return l;
}

static VkPipeline build_pipeline(const PipeKey* k, const uint32_t* vs, const uint32_t* ps)
{
    const Lib* l = library(&k->lib, vs, ps);
    VkShaderModule vm = l->vm, fm = l->fm;
    if (k->lib.vs.shadow && !alpha_tested(&k->lib.fs) && !k->pipe.write_mask) /* depth alone: nothing for the fragments to do */
        fm = VK_NULL_HANDLE;
    VkPipeline p = VK_NULL_HANDLE;
    if (l->ok)
    {
        VkPipelineColorBlendAttachmentState b = { 0 };
        uint32_t wm = k->pipe.write_mask;
        b.colorWriteMask = ((wm & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((wm & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
            ((wm & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((wm & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
        if (k->pipe.blend)
        {
            uint32_t sf = k->pipe.src, df = k->pipe.dst;
            if (sf == 12) /* BOTHSRCALPHA */
                sf = 5, df = 6;
            else if (sf == 13) /* BOTHINVSRCALPHA */
                sf = 6, df = 5;
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = b.srcAlphaBlendFactor = blend_factor(sf, k->x8);
            b.dstColorBlendFactor = b.dstAlphaBlendFactor = blend_factor(df, k->x8);
            /* SRC_ALPHA_SATURATE is not allowed for alpha: there it is ONE */
            if (b.srcAlphaBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE)
                b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            if (b.dstAlphaBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE)
                b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.colorBlendOp = b.alphaBlendOp = blend_op(k->pipe.op);
        }
        static const VkPrimitiveTopology TOPO[3] = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        p = make_pipeline(vm, fm, g_layout, &b, (VkFormat)k->color, (VkFormat)k->depth, (VkFormat)k->stencil, TOPO[k->topo],
            k->fill ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL, DRAW_DYNAMIC, sizeof DRAW_DYNAMIC / sizeof DRAW_DYNAMIC[0],
            k->lib.vs.shadow == 2);
    }
    return p;
}

/* one record of the cache file: magic, the key, then each shader's tokens (count first) */
static void record_job(const PipeJob* j)
{
    if (!g_pipe_cache[0])
        return;
    pthread_mutex_lock(&g_file_lock);
    FILE* f = fopen(g_pipe_cache, "ab");
    if (f)
    {
        uint32_t v[2] = { PIPE_MAGIC, (uint32_t)sizeof(PipeKey) };
        fwrite(v, 4, 2, f);
        fwrite(&j->k, sizeof(PipeKey), 1, f);
        fwrite(&j->nvs, 4, 1, f);
        if (j->nvs)
            fwrite(j->vs, 4, j->nvs, f);
        fwrite(&j->nps, 4, 1, f);
        if (j->nps)
            fwrite(j->ps, 4, j->nps, f);
        fclose(f);
    }
    pthread_mutex_unlock(&g_file_lock);
}

static void run_job(PipeJob* j)
{
    VkPipeline p = build_pipeline(&j->k, j->vs, j->ps);
    atomic_store(&j->e->state, p ? (void*)p : PIPE_FAILED);
    if (p && j->record)
        record_job(j);
    free(j->vs);
    free(j->ps);
    free(j);
}

static void* worker(void* arg)
{
    (void)arg;
    for (;;)
    {
        pthread_mutex_lock(&g_job_lock);
        while (!g_jobs)
            pthread_cond_wait(&g_job_cond, &g_job_lock);
        PipeJob* j = g_jobs;
        g_jobs = j->next;
        if (!g_jobs)
            g_jobs_tail = NULL;
        pthread_mutex_unlock(&g_job_lock);
        run_job(j);
    }
    return NULL;
}

static void queue_job(const PipeKey* k, const uint32_t* vs, uint32_t nvs, const uint32_t* ps, uint32_t nps, int record)
{
    if (map_get(&g_pipes, k, sizeof *k))
        return;
    PipeEntry* e = (PipeEntry*)calloc(1, sizeof *e);
    map_put(&g_pipes, k, sizeof *k, e);
    PipeJob* j = (PipeJob*)calloc(1, sizeof *j);
    j->k = *k, j->e = e, j->record = record;
    if (nvs)
        j->vs = (uint32_t*)malloc(4u * nvs), memcpy(j->vs, vs, 4u * nvs), j->nvs = nvs;
    if (nps)
        j->ps = (uint32_t*)malloc(4u * nps), memcpy(j->ps, ps, 4u * nps), j->nps = nps;
    g_prof.pipelines++;
    if (g_sync_pipelines || !g_workers)
    {
        run_job(j);
        return;
    }
    pthread_mutex_lock(&g_job_lock);
    if (g_jobs_tail)
        g_jobs_tail->next = j;
    else
        g_jobs = j;
    g_jobs_tail = j;
    pthread_cond_signal(&g_job_cond);
    pthread_mutex_unlock(&g_job_lock);
}

/* at start-up: the keys earlier sessions built, built again in the background */
static void prewarm_pipelines(void)
{
    char dir[900];
    if (!cache_dir(dir, sizeof dir))
        return;
    snprintf(g_pipe_cache, sizeof g_pipe_cache, "%s/pipelines-vk.v1", dir);
    FILE* f = fopen(g_pipe_cache, "rb");
    if (!f)
        return;
    uint32_t n = 0, hdr[2];
    while (fread(hdr, 4, 2, f) == 2 && hdr[0] == PIPE_MAGIC && hdr[1] == sizeof(PipeKey))
    {
        PipeKey k;
        uint32_t nvs = 0, nps = 0, *vs = NULL, *ps = NULL;
        int ok = fread(&k, sizeof k, 1, f) == 1 && fread(&nvs, 4, 1, f) == 1 && nvs <= 65536;
        if (ok && nvs)
            vs = (uint32_t*)malloc(4u * nvs), ok = fread(vs, 4, nvs, f) == nvs;
        ok = ok && fread(&nps, 4, 1, f) == 1 && nps <= 65536;
        if (ok && nps)
            ps = (uint32_t*)malloc(4u * nps), ok = fread(ps, 4, nps, f) == nps;
        if (ok)
            queue_job(&k, vs, nvs, ps, nps, 0), n++;
        free(vs);
        free(ps);
        if (!ok)
            break;
    }
    fclose(f);
    fprintf(stderr, "[recomp] gfx: building %u pipelines from %s in the background\n", n, g_pipe_cache);
}

/* the driver's pipeline cache, beside the keys */
static void vk_cache_load(void)
{
    char dir[900];
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    void* data = NULL;
    if (!g_sync_pipelines && cache_dir(dir, sizeof dir))
    {
        snprintf(g_vk_cache_file, sizeof g_vk_cache_file, "%s/pipelines-vk.cache", dir);
        FILE* f = fopen(g_vk_cache_file, "rb");
        if (f)
        {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, f) == (size_t)n)
                ci.initialDataSize = (size_t)n, ci.pInitialData = data;
            fclose(f);
        }
    }
    if (vkCreatePipelineCache(g_dev, &ci, NULL, &g_vkcache) != VK_SUCCESS)
    {
        ci.initialDataSize = 0, ci.pInitialData = NULL;
        vkCreatePipelineCache(g_dev, &ci, NULL, &g_vkcache);
    }
    free(data);
}

/* every minute or so while new pipelines come: the driver's cache to disk */
static void vk_cache_save(void)
{
    static uint64_t last, last_count;
    if (!g_vk_cache_file[0] || g_pipes.n == last_count)
        return;
    uint64_t now = gfx_now_ns();
    if (last && now - last < 60000000000ull)
        return;
    last = now, last_count = g_pipes.n;
    size_t n = 0;
    if (vkGetPipelineCacheData(g_dev, g_vkcache, &n, NULL) != VK_SUCCESS || !n)
        return;
    void* data = malloc(n);
    if (vkGetPipelineCacheData(g_dev, g_vkcache, &n, data) == VK_SUCCESS)
    {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.tmp", g_vk_cache_file);
        FILE* f = fopen(tmp, "wb");
        if (f)
        {
            int ok = fwrite(data, 1, n, f) == n;
            fclose(f);
            if (ok)
                rename(tmp, g_vk_cache_file);
        }
    }
    free(data);
}

/* the pipeline for a key, queued for building the first time (null until built) */
static VkPipeline pipeline_for(const PipeKey* k, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    PipeEntry* e = (PipeEntry*)map_get(&g_pipes, k, sizeof *k);
    if (!e)
    {
        uint32_t nvs = 0, nps = 0;
        uint32_t* vs = k->lib.vs.prog ? copy_tok(vs_tokens, &nvs) : NULL;
        uint32_t* ps = k->lib.fs.prog ? copy_tok(ps_tokens, &nps) : NULL;
        queue_job(k, vs, nvs, ps, nps, !g_sync_pipelines);
        free(vs);
        free(ps);
        e = (PipeEntry*)map_get(&g_pipes, k, sizeof *k);
    }
    void* st = atomic_load(&e->state);
    return st && st != PIPE_FAILED ? (VkPipeline)st : VK_NULL_HANDLE;
}

static uint8_t topo_class(uint32_t prim)
{
    return prim == GFX_POINTLIST ? 0 : prim == GFX_LINELIST || prim == GFX_LINESTRIP ? 1 : 2;
}

static VkPipeline pipeline(const GfxDraw* d, GfxTex* depth, int* water)
{
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.lib.vs = d->vs, k.lib.fs = d->fs, k.pipe = d->pipe;
    k.lib.vs.water = *water == 1, k.lib.fs.water = (uint8_t)*water; /* water_mode */
    /* the world's lit draws lit per pixel (gfx_msl.c pixel_lit) */
    if (g_fxs.fx != 0.0f && g_fxs.light != 0.0f && d->vs.lighting && !d->vs.rhw && !d->vs.prog && !d->vs.flat)
        k.lib.vs.pixel = g_fxs.light >= 2.0f ? 2 : 1;
    k.color = (uint32_t)g_rt->vf;
    k.depth = depth ? (uint32_t)depth->vf : 0;
    k.stencil = depth && depth->has_stencil ? k.depth : 0;
    k.x8 = (uint8_t)g_rt->x8;
    k.topo = topo_class(d->prim);
    k.fill = d->fill == 2 && k.topo == 2 && g_has_lines;
    VkPipeline p = pipeline_for(&k, d->vs_tokens, d->ps_tokens);
    if (!p && k.lib.vs.pixel)
    {
        /* lit per pixel, still building: lit per vertex meanwhile */
        k.lib.vs.pixel = 0;
        p = pipeline_for(&k, d->vs_tokens, d->ps_tokens);
    }
    if (!p && *water)
    {
        /* water still building: as the game drew it meanwhile */
        k.lib.vs.water = k.lib.fs.water = 0, *water = 0;
        p = pipeline_for(&k, d->vs_tokens, d->ps_tokens);
    }
    return p;
}

/* --- samplers ------------------------------------------------------------------------------------------------- */
static VkSamplerAddressMode address(uint32_t a)
{
    switch (a)
    {
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return g_has_mirror_once ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkSampler sampler(const GfxSampler* k)
{
    VkSampler s = (VkSampler)map_get(&g_samplers, k, sizeof *k);
    if (s)
        return s;
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.addressModeU = address(k->addr_u);
    si.addressModeV = address(k->addr_v);
    si.addressModeW = address(k->addr_w);
    si.magFilter = k->mag >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = k->min >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = k->mip >= 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if ((k->min == 3 || k->mag == 3) && k->max_aniso > 1 && g_has_aniso)
    {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = k->max_aniso > g_max_aniso ? g_max_aniso : (float)k->max_aniso;
    }
    if (k->mip == 0) /* not mipmapped: the first level alone */
        si.minLod = 0.0f, si.maxLod = 0.25f;
    else
    {
        si.minLod = (float)k->max_level;
        si.maxLod = k->lod_cap ? (float)(k->lod_cap - 1) : VK_LOD_CLAMP_NONE;
        if (si.maxLod < si.minLod)
            si.maxLod = si.minLod;
    }
    uint32_t a = k->border >> 24, rgb = k->border & 0xFFFFFF;
    si.borderColor = a < 128 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
        : rgb >= 0x808080 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VK_CHECK(vkCreateSampler(g_dev, &si, NULL, &s));
    map_put(&g_samplers, k, sizeof *k, s);
    return s;
}

/* --- drawing --------------------------------------------------------------------------------------------------- */
static VkCompareOp compare(uint32_t f)
{
    switch (f)
    {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

/* D3D's viewport, clamped to the target, as Vulkan's with y flipped (a negative height: D3D's clip
 * space has y up, Vulkan's down). 0 when nothing of it is left. */
static VkViewport g_set_vp; /* the last viewport and scissor the draws set (a glow keeps them: glow_new) */
static VkRect2D g_set_sc;

static int set_viewport(const uint32_t vp[6])
{
    float zmin, zmax;
    memcpy(&zmin, &vp[4], 4);
    memcpy(&zmax, &vp[5], 4);
    uint32_t w, h;
    color_size(&w, &h);
    double x = vp[0], y = vp[1], vw = vp[2], vh = vp[3];
    if (x > w)
        x = w;
    if (y > h)
        y = h;
    if (x + vw > w)
        vw = w - x;
    if (y + vh > h)
        vh = h - y;
    if (vw <= 0 || vh <= 0)
        return 0;
    VkViewport v = { (float)x, (float)(y + vh), (float)vw, (float)-vh, zmin, zmax };
    vkCmdSetViewport(g_cb, 0, 1, &v);
    g_set_vp = v;
    return 1;
}

/* GfxDraw.scissor, clamped to the target; none: all of it */
static void set_scissor(const int32_t sc[4])
{
    uint32_t w, h;
    color_size(&w, &h);
    int64_t x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (sc[2] > 0)
    {
        x0 = sc[0] < 0 ? 0 : sc[0];
        y0 = sc[1] < 0 ? 0 : sc[1];
        x1 = (int64_t)sc[0] + sc[2];
        y1 = (int64_t)sc[1] + sc[3];
        if (x1 > w)
            x1 = w;
        if (y1 > h)
            y1 = h;
        if (x0 > x1)
            x0 = x1;
        if (y0 > y1)
            y0 = y1;
    }
    VkRect2D r = { { (int32_t)x0, (int32_t)y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
    vkCmdSetScissor(g_cb, 0, 1, &r);
    g_set_sc = r;
}

/* index count for a D3D primitive count */
static uint32_t vertex_count(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case GFX_POINTLIST: return n;
    case GFX_LINELIST: return n * 2;
    case GFX_LINESTRIP: return n + 1;
    case GFX_TRIANGLELIST: return n * 3;
    case GFX_TRIANGLESTRIP: return n + 2;
    case GFX_TRIANGLEFAN: return n * 3; /* as a list */
    default: return 0;
    }
}

static VkPrimitiveTopology vk_prim(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case GFX_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case GFX_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case GFX_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

/* The scene filter (gfx_metal.m scene_mips): a large render target drawn smaller onto a large target
 * is sampled through its mips, made here from it as it is now. */
static void scene_mips(const GfxDraw* d)
{
    if (g_fxs.fx == 0.0f || g_fxs.filter == 0.0f || !d->vs.rhw || !g_rt)
        return;
    uint32_t tw, th;
    color_size(&tw, &th);
    if (tw < 512 || th < 512)
        return; /* onto a large target only: not the flare's probe, nor the game's 256x256 targets (gfx_metal.m) */
    for (int i = 0; i < 8; ++i)
    {
        GfxTex* t = d->tex[i];
        int wanted = d->fs.prog || i < d->fs.nstages ? d->fs.st[i].tex : 0;
        if (!wanted || !t || t == g_rt || !t->mipview || t->mips < 2 || t->scene)
            continue;
        if (t->w <= tw && t->h <= th)
            continue; /* not made smaller */
        flush_pass();
        gpu_sync();
        for (uint32_t l = 1; l < t->mips; ++l)
        {
            uint32_t sw, sh, dw, dh;
            level_size(t, l - 1, &sw, &sh);
            level_size(t, l, &dw, &dh);
            VkImageBlit b = { 0 };
            b.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1 };
            b.srcOffsets[1] = (VkOffset3D){ (int32_t)sw, (int32_t)sh, 1 };
            b.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 };
            b.dstOffsets[1] = (VkOffset3D){ (int32_t)dw, (int32_t)dh, 1 };
            vkCmdBlitImage(g_cb, t->img, VK_IMAGE_LAYOUT_GENERAL, t->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
            full_barrier(g_cb); /* the next level reads this one */
        }
        g_dirty = 1;
        t->scene = g_serial, t->used = g_serial, t->rec = g_cb_index;
    }
}

/* --- the sun's shadow map: the scene's casters, drawn again from the sun (gfx_scene_done) --------------
 * Each opaque draw of the scene (GfxDraw.caster) is recorded as it is encoded - its pipeline key and the
 * buffers, uniforms and textures it was bound with, all alive until the frame ends - and when the scene
 * is done they are drawn again into a depth map from the sun (gfx_metal.m's note). A record keeps where
 * its bytes are on the CPU too (the cache copies what the game rewrites). */
typedef struct Caster
{
    LibKey lib;
    const uint32_t *vs, *ps;
    VkBuffer vb[GFX_NSTREAMS], ub, ib;
    VkDeviceSize voff[GFX_NSTREAMS], uoff, ioff;
    const uint8_t* vbp[GFX_NSTREAMS]; /* the whole buffer each is bound from, on the CPU, and its size */
    size_t vblen[GFX_NSTREAMS];
    const uint8_t* ibp;
    size_t iblen;
    const GfxU* up; /* the uniforms, as bound: its g_caster_u, not the ring's copy */
    VkImageView tex[8]; /* for an alpha test, and the bounce light's map (in colour); the cache's an alpha test's alone */
    GfxSampler samp[8];
    VkPrimitiveTopology prim;
    uint32_t n, vstart;
    uint8_t itype; /* 0 no indices, 2 or 4 bytes each */
    uint8_t fixed; /* every vertex and index from buffers the game keeps (the zone's): cached (sun_cache) */
    uint8_t keep;  /* not fixed, but a placed object (not a character): kept as a copy (sun_cache_update) */
    uint8_t has_pos;
    uint8_t lost_tex; /* its alpha test's texture was destroyed this frame: drawn, but not cached (sun_cache_forget_tex) */
    int32_t zbias;
    float clip0[4]; /* its first vertex in the camera's clip space (gfx_clip0): where this copy stands */
} Caster;

static Caster* g_casters;
static uint32_t g_ncasters, g_casters_cap;
/* each caster's uniforms in host memory (g_casters[i].up is &g_caster_u[i]), as gfx_d3d12.c keeps them: the
 * ring is write-combined, and reading it back is uncached - sun_cache_update's copy from it took 40 ms of
 * a 50 ms frame in Bastok Markets */
static GfxU* g_caster_u;

static void casters_clear(void) { g_ncasters = 0; }

/* --- the glow: the scene's additive draws, drawn again for the post bloom (post_bloom; gfx_metal.m's glow_new) ---
 * The world's lamps, spell effects and the sun's halo are additive draws (the destination blended by ONE). Each one into
 * a 3D scene's target is recorded as it is encoded - its pipeline, dynamic state and bindings, all alive until the
 * frame ends - and when the scene is done they are drawn again onto black over the scene's depth (post_glow): what
 * glows, as bright as it was added. */
typedef struct Glow
{
    const GfxTex* rt;   /* the target (compared only: gfx_tex_destroy drops its glows) */
    VkFormat fmt;       /* its format, which the pipeline was built for */
    VkImageView depth;  /* the depth attachment it was tested against (VK_NULL_HANDLE: none) */
    int stencil;        /* that attachment has stencil */
    VkPipeline pipe;
    VkViewport vp;
    VkRect2D sc;
    VkCompareOp zfunc;
    VkCullModeFlags cull;
    VkPrimitiveTopology topo;
    uint8_t ztest, nw, indexed;
    int32_t zbias;
    uint32_t bind; /* its nw bindings, from here in g_glow_binds */
    VkBuffer ib;
    VkDeviceSize ioff;
    VkIndexType itype;
    uint32_t n, vstart;
} Glow;

static Glow* g_glows;
static uint32_t g_nglows, g_glows_cap;
/* a glow's binding: a write of one buffer or one image */
typedef struct GlowBind
{
    uint32_t binding;
    VkDescriptorType type;
    union
    {
        VkDescriptorBufferInfo b;
        VkDescriptorImageInfo i;
    };
} GlowBind;
static GlowBind* g_glow_binds;
static uint32_t g_nglow_binds, g_glow_binds_cap;

static void glows_clear(void) { g_nglows = g_nglow_binds = 0; }

/* the draw's writes kept for its glow */
static void glow_binds(Glow* g, const VkWriteDescriptorSet* w, uint32_t nw)
{
    if (g_nglow_binds + nw > g_glow_binds_cap)
    {
        while (g_nglow_binds + nw > g_glow_binds_cap)
            g_glow_binds_cap = g_glow_binds_cap ? g_glow_binds_cap * 2 : 4096;
        g_glow_binds = (GlowBind*)realloc(g_glow_binds, g_glow_binds_cap * sizeof *g_glow_binds);
    }
    g->bind = g_nglow_binds, g->nw = (uint8_t)nw;
    for (uint32_t k = 0; k < nw; ++k)
    {
        GlowBind* b = &g_glow_binds[g_nglow_binds++];
        b->binding = w[k].dstBinding, b->type = w[k].descriptorType;
        if (w[k].pBufferInfo)
            b->b = *w[k].pBufferInfo;
        else
            b->i = *w[k].pImageInfo;
    }
}

/* a target going: its glows go too */
static void glows_forget(const GfxTex* t)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < g_nglows; ++i)
        if (g_glows[i].rt != t)
            g_glows[k++] = g_glows[i];
    g_nglows = k;
}

/* a record for this draw when it glows: post bloom on, an additive 3D draw into the target's first level */
static Glow* glow_new(const GfxDraw* d, int water)
{
    if (g_fxs.post_bloom <= 0.0f || g_fxs.bloom_emissive_fx <= 0.0f || water || d->vs.rhw || !d->pipe.blend || d->pipe.dst != 2 ||
        d->depth.stencil || g_rt_face || g_rt_level || !g_rt || g_nglows >= 4096)
        return NULL;
    /* not the sky's own layers (drawn round the camera: no translation in world x view), which would haze all of it;
     * the sun and a bright sky glow by the threshold instead */
    if (!d->vs.prog && fabsf(d->u.wv[12]) + fabsf(d->u.wv[13]) + fabsf(d->u.wv[14]) < 1e-3f)
        return NULL;
    if (g_nglows == g_glows_cap)
        g_glows_cap = g_glows_cap ? g_glows_cap * 2 : 256, g_glows = (Glow*)realloc(g_glows, g_glows_cap * sizeof(Glow));
    Glow* g = &g_glows[g_nglows++];
    memset(g, 0, sizeof *g);
    g->rt = g_rt, g->fmt = g_rt->vf, g->zbias = d->zbias;
    return g;
}

/* clip-space position of the draw's first vertex in out (gfx_clip0); 0 when it cannot be had */
static int draw_clip0(const GfxDraw* d, float out[4])
{
    const uint8_t* base[GFX_NSTREAMS];
    size_t have[GFX_NSTREAMS];
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        base[s] = NULL, have[s] = 0;
        if (d->buf[s])
        {
            const GfxBuf* b = d->buf[s];
            const uint8_t* p = b->cpu ? b->cpu : (const uint8_t*)b->p;
            uint32_t n = b->cpu ? b->cpu_size : b->size;
            base[s] = p + d->buf_off[s], have[s] = n > d->buf_off[s] ? n - d->buf_off[s] : 0;
        }
        else if (d->data[s])
            base[s] = (const uint8_t*)d->data[s], have[s] = d->size[s];
    }
    return gfx_clip0(d, base, have, out);
}

/* a shader's tokens as casters hold them: the game's own are freed by DeleteVertexShader, but an entry
 * of the sun's cache may build its shadow pipeline from them long after. One copy per program (its
 * hash, as the pipelines' keys have it), kept for the session. */
static const uint32_t* tokens_kept(uint32_t prog, int pixel, const uint32_t* t)
{
    static Map kept;
    static uint32_t last_prog[2];
    static const uint32_t* last[2];
    if (!prog || !t)
        return t;
    if (last[pixel] && last_prog[pixel] == prog)
        return last[pixel];
    uint64_t key = (uint64_t)prog << 1 | (uint64_t)pixel;
    uint32_t* c = (uint32_t*)map_get(&kept, &key, sizeof key);
    if (!c)
    {
        uint32_t n;
        c = copy_tok(t, &n);
        map_put(&kept, &key, sizeof key, c);
    }
    last_prog[pixel] = prog, last[pixel] = c;
    return c;
}

static Caster* caster_new(const GfxDraw* d)
{
    if (g_ncasters == g_casters_cap)
    {
        g_casters_cap = g_casters_cap ? g_casters_cap * 2 : 1024;
        g_casters = (Caster*)realloc(g_casters, g_casters_cap * sizeof(Caster));
        g_caster_u = (GfxU*)realloc(g_caster_u, g_casters_cap * sizeof(GfxU));
        for (uint32_t i = 0; i < g_ncasters; ++i)
            g_casters[i].up = &g_caster_u[i];
    }
    Caster* c = &g_casters[g_ncasters];
    memset(c, 0, sizeof *c);
    c->up = &g_caster_u[g_ncasters++];
    c->lib.vs = d->vs, c->lib.fs = d->fs;
    c->vs = tokens_kept(d->vs.prog, 0, d->vs_tokens), c->ps = tokens_kept(d->fs.prog, 1, d->ps_tokens);
    c->zbias = d->zbias;
    c->fixed = d->prim != GFX_TRIANGLEFAN && (!d->indices || d->ibuf);
    c->has_pos = (uint8_t)draw_clip0(d, c->clip0);
    return c;
}

static int water_mode(const GfxDraw* d);
static int water_capture(void);
static uint32_t water_bind(VkWriteDescriptorSet* w, VkDescriptorBufferInfo* bi, VkDescriptorImageInfo* ii);

static void draw_encode(const GfxDraw* d)
{
    int water = g_rt ? water_mode(d) : 0;
    if (water && !water_capture())
        water = 0;
    if (!begin_pass())
    {
        gfx_prof_skip(GFX_SKIP_NO_TARGET);
        return;
    }
    GfxTex* depth = depth_attachment();
    VkPipeline p = pipeline(d, depth, &water);
    if (!p)
    {
        gfx_prof_skip(GFX_SKIP_PIPELINE); /* still building (or failed) */
        return;
    }
    if (!set_viewport(d->vp))
        return;
    VkCommandBuffer cb = g_cb;
    if (p != g_bound)
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p), g_bound = p;
    set_scissor(d->scissor);
    GfxDepthKey dk = d->depth;
    if (!depth)
        memset(&dk, 0, sizeof dk);
    vkCmdSetDepthTestEnable(cb, dk.zenable ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, dk.zenable && dk.zwrite ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cb, dk.zenable ? compare(dk.zfunc) : VK_COMPARE_OP_ALWAYS);
    int stencil = dk.stencil && depth && depth->has_stencil;
    vkCmdSetStencilTestEnable(cb, stencil ? VK_TRUE : VK_FALSE);
    if (stencil)
    {
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_op(dk.sfail), stencil_op(dk.spass), stencil_op(dk.szfail),
            compare(dk.sfunc));
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, dk.sread);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, dk.swrite);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, d->stencil_ref);
    }
    else
    {
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
            VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    }
    /* D3D's front faces are clockwise on screen; CULL_CCW (the default) culls the back ones */
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetCullMode(cb, d->cull == 3 ? VK_CULL_MODE_BACK_BIT : d->cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
    vkCmdSetDepthBiasEnable(cb, d->zbias ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthBias(cb, -(float)d->zbias, 0.0f, -(float)d->zbias * 0.5f);
    vkCmdSetPrimitiveTopology(cb, vk_prim(d->prim));

    VkWriteDescriptorSet w[B_COUNT];
    VkDescriptorBufferInfo bi[GFX_NSTREAMS + 2];
    VkDescriptorImageInfo ii[10];
    uint32_t nw = 0;
    /* the uniforms the draw's functions read (as gfx_metal.m: what the key uses, room for the whole
     * struct), with each stream's bytes past its binding's aligned offset added to its registers' */
    size_t need = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
    if (d->vs.prog)
        need = offsetof(GfxU, psc);
    if (d->fs.prog)
        need = sizeof(GfxU);
    VkBuffer ub;
    VkDeviceSize uoff;
    GfxU* u = (GfxU*)ring(sizeof(GfxU), RING_ALIGN, &ub, &uoff);
    if (d->caster && !g_rt_face && !g_rt_level && depth)
        g_rt->depth_world = depth;
    Caster* rec = d->caster && g_fxs.fx != 0.0f && g_fxs.sun > 0.0f ? caster_new(d) : NULL;
    /* put together in host memory (a caster's in its g_caster_u) and copied to the write-combined ring once */
    GfxU own;
    GfxU* hu = rec ? &g_caster_u[g_ncasters - 1] : &own;
    memcpy(hu, &d->u, need);
    if (rec)
        rec->ub = ub, rec->uoff = uoff;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        VkDescriptorBufferInfo* b = &bi[1 + s];
        uint32_t rem = 0;
        if (d->buf[s])
        {
            VkDeviceSize align = g_props.limits.minStorageBufferOffsetAlignment;
            VkDeviceSize at = d->buf_off[s] / align * align;
            rem = (uint32_t)(d->buf_off[s] - at);
            *b = (VkDescriptorBufferInfo){ d->buf[s]->b, at, VK_WHOLE_SIZE };
            d->buf[s]->used = g_serial;
            if (rec)
            {
                rec->vb[s] = d->buf[s]->b, rec->voff[s] = at;
                rec->vbp[s] = (const uint8_t*)d->buf[s]->p, rec->vblen[s] = d->buf[s]->size;
                if (buf_volatile(d->buf[s]))
                    rec->fixed = 0;
            }
        }
        else if (d->data[s] && d->size[s])
        {
            VkBuffer vb;
            VkDeviceSize voff;
            void* v = ring(d->size[s], RING_ALIGN, &vb, &voff);
            memcpy(v, d->data[s], d->size[s]);
            *b = (VkDescriptorBufferInfo){ vb, voff, (d->size[s] + 3) & ~3u };
            if (rec)
                rec->vb[s] = vb, rec->voff[s] = voff, rec->vbp[s] = g_ring_base, rec->vblen[s] = g_ring_size, rec->fixed = 0;
        }
        else
        {
            *b = (VkDescriptorBufferInfo){ g_dummy, 0, VK_WHOLE_SIZE };
            if (rec)
                rec->vb[s] = g_dummy;
        }
        if (rem)
            for (int r = 0; r < GFX_NREGS; ++r)
                if (d->vs.el[r].used && d->vs.el[r].stream == s)
                    hu->offset[r] += (int32_t)rem;
        w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[nw].dstBinding = B_STREAM0 + (uint32_t)s;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw++].pBufferInfo = b;
    }
    memcpy(u, hu, need);
    bi[0] = (VkDescriptorBufferInfo){ ub, uoff, sizeof(GfxU) };
    w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w[nw].dstBinding = B_U;
    w[nw].descriptorCount = 1;
    w[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[nw++].pBufferInfo = &bi[0];
    for (int i = 0; i < 8; ++i)
    {
        int wanted = d->fs.prog || i < d->fs.nstages ? d->fs.st[i].tex : 0;
        if (!wanted)
            continue;
        GfxTex* t = d->tex[i];
        VkImageView view;
        GfxSampler sk = d->samp[i];
        if (!t || t->use == GFX_USE_DEPTH || (wanted == 2) != (t->type == GFX_TEX_CUBE))
            view = wanted == 2 ? g_dummycube->view : g_dummy2d->view;
        else
        {
            view = t->view;
            if (g_fxs.fx != 0.0f)
            {
                /* the world's solid textures, where every mip is there: trilinear and anisotropic
                 * (gfx_metal.m draw_encode) */
                if (!d->vs.rhw && d->depth.zwrite && !d->fs.st[i].projected && g_fxs.aniso > 1.0f && t->levels > 1 &&
                    t->levels < 32 && t->filled == (1u << t->levels) - 1 && sk.min >= 2)
                    sk.min = 3, sk.mip = 2, sk.max_aniso = (uint8_t)(g_fxs.aniso > 16.0f ? 16.0f : g_fxs.aniso);
                /* the finished scene made smaller: through its mips */
                else if (t->scene && t->mipview)
                    sk.min = 3, sk.mag = 2, sk.mip = 2, sk.max_aniso = 16, sk.max_level = 0, view = t->mipview;
            }
            t->used = g_serial, t->rec = g_cb_index;
        }
        ii[i] = (VkDescriptorImageInfo){ sampler(&sk), view, VK_IMAGE_LAYOUT_GENERAL };
        w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[nw].dstBinding = B_TEX0 + (uint32_t)i;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[nw++].pImageInfo = &ii[i];
        if (rec)
            rec->tex[i] = view, rec->samp[i] = sk;
    }
    if (water)
        nw += water_bind(&w[nw], &bi[GFX_NSTREAMS + 1], &ii[8]);
    p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, nw, w);

    uint32_t n = vertex_count(d->prim, d->count);
    VkBuffer buf;
    VkDeviceSize off;
    if (rec)
        rec->prim = vk_prim(d->prim), rec->n = n, rec->vstart = d->vertex_start;
    Glow* gl = glow_new(d, water);
    if (gl)
    {
        gl->depth = depth ? attachment_view(depth, 0, 0) : VK_NULL_HANDLE, gl->stencil = depth && depth->has_stencil;
        gl->pipe = p, gl->vp = g_set_vp, gl->sc = g_set_sc;
        gl->ztest = dk.zenable != 0, gl->zfunc = dk.zenable ? compare(dk.zfunc) : VK_COMPARE_OP_ALWAYS;
        gl->cull = d->cull == 3 ? VK_CULL_MODE_BACK_BIT : d->cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
        gl->topo = vk_prim(d->prim), gl->n = n, gl->vstart = d->vertex_start;
        glow_binds(gl, w, nw);
    }
    if (d->prim == GFX_TRIANGLEFAN)
    {
        /* a list with the same vertices, as on Metal */
        uint32_t* idx = (uint32_t*)ring((size_t)n * 4, 16, &buf, &off);
        for (uint32_t i = 0; i < d->count; ++i)
        {
            uint32_t k[3] = { 0, i + 1, i + 2 };
            for (int j = 0; j < 3; ++j)
            {
                if (!d->indices)
                    idx[3 * i + j] = d->vertex_start + k[j];
                else if (d->index_size == 2)
                    idx[3 * i + j] = ((const uint16_t*)d->indices)[k[j]];
                else
                    idx[3 * i + j] = ((const uint32_t*)d->indices)[k[j]];
            }
        }
        vkCmdBindIndexBuffer(cb, buf, off, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, n, 1, 0, 0, 0);
        if (rec)
            rec->prim = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, rec->ib = buf, rec->ioff = off, rec->ibp = g_ring_base,
            rec->iblen = g_ring_size, rec->itype = 4;
        if (gl)
            gl->indexed = 1, gl->ib = buf, gl->ioff = off, gl->itype = VK_INDEX_TYPE_UINT32;
    }
    else if (d->ibuf)
    {
        d->ibuf->used = g_serial;
        if (rec)
        {
            rec->ib = d->ibuf->b, rec->ioff = d->ibuf_off, rec->itype = (uint8_t)(d->index_size == 2 ? 2 : 4);
            rec->ibp = (const uint8_t*)d->ibuf->p, rec->iblen = d->ibuf->size;
            if (buf_volatile(d->ibuf))
                rec->fixed = 0;
        }
        vkCmdBindIndexBuffer(cb, d->ibuf->b, d->ibuf_off, d->index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, n, 1, 0, 0, 0);
        if (gl)
            gl->indexed = 1, gl->ib = d->ibuf->b, gl->ioff = d->ibuf_off,
            gl->itype = d->index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    }
    else if (d->indices)
    {
        void* idx = ring((size_t)n * d->index_size, 16, &buf, &off);
        memcpy(idx, d->indices, (size_t)n * d->index_size);
        vkCmdBindIndexBuffer(cb, buf, off, d->index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, n, 1, 0, 0, 0);
        if (rec)
            rec->ib = buf, rec->ioff = off, rec->ibp = g_ring_base, rec->iblen = g_ring_size,
            rec->itype = (uint8_t)(d->index_size == 2 ? 2 : 4);
        if (gl)
            gl->indexed = 1, gl->ib = buf, gl->ioff = off, gl->itype = d->index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    }
    else
        vkCmdDraw(cb, n, 1, d->vertex_start, 0);
}

void gfx_draw(const GfxDraw* d)
{
    if (!g_dev || !d->count)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    scene_mips(d);
    draw_encode(d);
    g_cmd_draws++;
    /* a long pass (the 3D scene) goes to the GPU in pieces too: end it here, submit, and the next draw
     * resumes it with its contents loaded */
    if (g_cmd_draws >= SPLIT_DRAWS && g_in_pass)
    {
        end_pass();
        submit(0);
    }
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws++;
}

/* --- clears ---------------------------------------------------------------------------------------------------- */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    if (!g_dev || !g_rt)
        return;
    if (!g_ds)
        flags &= 1;
    if (!flags)
        return;
    float c[4] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f };
    uint32_t w, h;
    color_size(&w, &h);
    int whole = !nrects && vp[0] == 0 && vp[1] == 0 && vp[2] >= w && vp[3] >= h;
    if (whole)
    {
        /* the next pass starts cleared: close this one */
        end_pass();
        g_pending_clear |= flags;
        if (flags & 1)
            memcpy(g_clear_color, c, sizeof c);
        if (flags & 2)
            g_clear_z = z;
        if (flags & 4)
            g_clear_stencil = stencil;
        return;
    }
    if (!begin_pass())
        return;
    GfxTex* depth = depth_attachment();
    VkClearAttachment ca[2];
    uint32_t nca = 0;
    if (flags & 1)
    {
        ca[nca].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[nca].colorAttachment = 0;
        memcpy(ca[nca++].clearValue.color.float32, c, sizeof c);
    }
    VkImageAspectFlags za = ((flags & 2) && depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
        ((flags & 4) && depth && depth->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (za)
    {
        ca[nca].aspectMask = za;
        ca[nca].colorAttachment = 0;
        ca[nca].clearValue.depthStencil.depth = z;
        ca[nca++].clearValue.depthStencil.stencil = stencil;
    }
    if (!nca)
        return;
    /* the viewport, intersected with each rectangle */
    int32_t vx0 = (int32_t)vp[0], vy0 = (int32_t)vp[1], vx1 = vx0 + (int32_t)vp[2], vy1 = vy0 + (int32_t)vp[3];
    if (vx1 > (int32_t)w)
        vx1 = (int32_t)w;
    if (vy1 > (int32_t)h)
        vy1 = (int32_t)h;
    int32_t whole_rect[4] = { vx0, vy0, vx1, vy1 };
    if (!nrects)
        rects = whole_rect, nrects = 1;
    for (uint32_t i = 0; i < nrects; ++i)
    {
        int32_t x0 = rects[4 * i] > vx0 ? rects[4 * i] : vx0, y0 = rects[4 * i + 1] > vy0 ? rects[4 * i + 1] : vy0;
        int32_t x1 = rects[4 * i + 2] < vx1 ? rects[4 * i + 2] : vx1, y1 = rects[4 * i + 3] < vy1 ? rects[4 * i + 3] : vy1;
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 <= x0 || y1 <= y0)
            continue;
        VkClearRect r = { { { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } }, 0, 1 };
        vkCmdClearAttachments(g_cb, nca, ca, 1, &r);
    }
}

/* --- scene effects (gfx_scene_done) -----------------------------------------------------------------------------
 * gfx_metal.m's, on Vulkan: on the finished 3D scene, in place, when fx is on - ambient occlusion and the
 * sun's shadows (its maps and contact shadows), height fog, bloom, god rays and a color grade; FXAA on
 * its own (aa). The passes are FX_MSL's in GLSL, one text with each pass under FX_<NAME>; their
 * uniforms are FxU (gfx_scene.h), at binding 0, their textures t0..t5 at 1-6 and the sun's maps again
 * through a compare sampler at 7 and 8. Every pass is a full-screen triangle into its own target. */
static const char FX_GLSL[] =
    "layout(std140, set = 0, binding = 0) uniform FxUB {\n"
    "  vec4 proj;   // P00, P11, P20, P21\n"
    "  vec4 zp;     // P22, P32, viewport MinZ, MaxZ\n"
    "  vec4 vp;     // the scene viewport in target pixels: x, y, width, height\n"
    "  vec4 size;   // target width, height; occlusion width, height\n"
    "  vec4 ao;     // radius, strength, bias, largest radius in pixels\n"
    "  vec4 grade;  // strength, saturation, contrast, debug\n"
    "  vec4 hand;   // P23: 1 for a left-handed projection, -1 for a right-handed one\n"
    "  vec4 up;     // world up in view space\n"
    "  vec4 sun;    // view space, toward the light; w = 1 when the scene has one\n"
    "  vec4 suncol; // its color\n"
    "  vec4 sunuv;  // its place in the viewport (0..1), z = how much of it shows\n"
    "  vec4 fogc;   // fog color, a = density at the camera's height\n"
    "  vec4 fogp;   // falloff with height, most fog, sun glow, its forward scattering (g)\n"
    "  vec4 bloom;  // threshold, strength, knee\n"
    "  vec4 rays;   // strength, decay, length\n"
    "  vec4 shadow; // contact shadows: strength, ray length, thickness, how far out\n"
    "  mat4 lmat;   // view space to the sun's map\n"
    "  vec4 smap;   // strength, a texel in world units, depth bias, penumbra\n"
    "  vec4 smap2;  // depth units per map width, sun_face, sun_min, world units per depth unit\n"
    "  mat4 reproj; // view space to the frame before's clip space\n"
    "  vec4 hist;   // 1 when the frame before is there, the pattern's turn this frame, its weight\n"
    "  mat4 lmatn;  // the near cascade: view space to its map\n"
    "  vec4 smapn;  // its texel in world units, depth bias, penumbra, slope\n"
    "  vec4 smapn2; // its depth units, 1 when it is there, 1 for hard edges\n"
    "  vec4 aop;    // the occlusion's taps this frame\n"
    "  mat4 gimat;  // the bounce light: view space to its map\n"
    "  mat4 giinv;  // its map back to view space\n"
    "  vec4 gi;     // its strength (0: none), its reach in the map's uv and in world units, the level read\n"
    "  vec4 gip;    // 1 when its frame before is there, that one's weight, its samples, how far it reaches\n"
    "} u;\n"
    "#ifdef FX_RT\n" /* (ray tracing's passes: RT_GLSL's counts too) */
    "layout(push_constant) uniform FxPC { ivec2 dir; ivec2 pad; uvec4 rtp; } pc;\n"
    "#else\n"
    "layout(push_constant) uniform FxPC { ivec2 dir; } pc;\n"
    "#endif\n"
    "#ifdef GFX_VS\n"
    "layout(location = 0) out vec2 vuv;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "  vuv = p;\n"
    "}\n"
    "#else\n"
    "layout(location = 0) in vec2 vuv;\n"
    "layout(location = 0) out vec4 oc;\n"
    "layout(set = 0, binding = 1) uniform sampler2D t0;\n"
    "layout(set = 0, binding = 2) uniform sampler2D t1;\n"
    "layout(set = 0, binding = 3) uniform sampler2D t2;\n"
    "layout(set = 0, binding = 4) uniform sampler2D t3;\n"
    "layout(set = 0, binding = 5) uniform sampler2D t4;\n"
    "layout(set = 0, binding = 6) uniform sampler2D t5;\n"
    "layout(set = 0, binding = 7) uniform sampler2DShadow smc;  /* t1 with a compare sampler */\n"
    "layout(set = 0, binding = 8) uniform sampler2DShadow smnc; /* t2 with a compare sampler */\n"
    "layout(set = 0, binding = 9) uniform sampler2D t6;\n"
    "const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);\n"
    "float view_z(float d) {\n"
    "  d = (d - u.zp.z) / max(u.zp.w - u.zp.z, 1e-6);\n"
    "  float z = u.zp.y / (d * u.hand.x - u.zp.x);\n"
    "  return d >= 0.999999 || !(z * u.hand.x > 0.0) ? 0.0 : z;\n"
    "}\n"
    "// px a pixel's centre in the target; every draw went through the position fixup (D3D's pixel\n"
    "// centres onto Vulkan's: half a pixel right and down), so the point it shows is the one half a\n"
    "// pixel up and left of it (gfx_metal.m's view_pos)\n"
    "vec3 view_pos(vec2 px, float z) {\n"
    "  px -= 0.5;\n"
    "  vec2 ndc = vec2((px.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (px.y - u.vp.y) / u.vp.w * 2.0);\n"
    "  float w = z * u.hand.x;\n"
    "  return vec3((ndc.x * w - u.proj.z * z) / u.proj.x, (ndc.y * w - u.proj.w * z) / u.proj.y, z);\n"
    "}\n"
    "float depth_at(sampler2D dt, vec2 px) { return texelFetch(dt, ivec2(px), 0).r; }\n"
    "vec3 pos_at(sampler2D dt, vec2 px) {\n"
    "  px = clamp(px, u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n"
    "  px = floor(px) + 0.5;\n"
    "  return view_pos(px, view_z(depth_at(dt, px)));\n"
    "}\n"
    "bool outside(vec2 q) { return any(lessThan(q, u.vp.xy)) || any(greaterThanEqual(q, u.vp.xy + u.vp.zw)); }\n"
    "\n"
    "#ifdef FX_LINZ\n"
    "void main() {\n"
    "  vec2 px = floor(u.vp.xy + vuv * u.vp.zw) + 0.5;\n"
    "  oc = vec4(view_z(depth_at(t0, px)), 0.0, 0.0, 0.0);\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_ZMIP\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy), hi = textureSize(t0, 0) - 1;\n"
    "  oc = texelFetch(t0, min(p * 2 + ivec2(p.y & 1, p.x & 1), hi), 0);\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_AO\n"
    "vec3 pos_lz(vec2 q, float r) {\n"
    "  vec2 t = (q - u.vp.xy) * u.size.zw / u.vp.zw;\n"
    "  int lv = clamp(int(floor(log2(max(r * u.size.z / u.vp.z, 1.0)))) - 3, 0, 3);\n"
    "  ivec2 p = min(ivec2(max(t, 0.0)) >> lv, textureSize(t3, lv) - 1);\n"
    "  return view_pos(q, texelFetch(t3, p, lv).r);\n"
    "}\n"
    "float sun_shadow(vec3 P, vec3 N, float dist, float k) {\n"
    "  float nl = dot(N, u.sun.xyz);\n"
    "  float fade = smoothstep(0.0, 0.15, nl) * (1.0 - smoothstep(0.6 * u.shadow.w, u.shadow.w, dist));\n"
    "  if (fade <= 0.0) return 1.0;\n"
    "  int NS = max(int(u.aop.y), 1);\n" /* contact_steps, jittered by k and gathered over frames */
    "  vec3 O = P + N * (0.01 * dist);\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float a = (float(i) + k) / float(NS);\n"
    "    vec3 R = O + u.sun.xyz * (u.shadow.y * a * a);\n"
    "    float rd = R.z * u.hand.x;\n"
    "    if (rd <= 0.05) break;\n"
    "    vec2 ndc = vec2(R.x * u.proj.x + R.z * u.proj.z, R.y * u.proj.y + R.z * u.proj.w) / rd;\n"
    "    vec2 q = u.vp.xy + vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;\n"
    "    if (outside(q) || outside(q + 0.5)) break;\n" /* the pixel read within the viewport, not just q */
    "    float sd = view_z(depth_at(t0, q + 0.5)) * u.hand.x;\n" /* where the fixup drew it */
    "    float in_front = rd - sd;\n"
    "    if (sd > 0.0 && in_front > 0.005 * rd + 0.02 && in_front < u.shadow.z + 0.01 * rd) return 1.0 - fade * (1.0 - a * a);\n"
    "  }\n"
    "  return 1.0;\n"
    "}\n"
    "const vec2 DISK[16] = vec2[16](vec2(-0.94, -0.40), vec2(0.95, -0.77), vec2(-0.09, -0.93), vec2(0.34, 0.29),\n"
    "  vec2(-0.92, 0.46), vec2(-0.82, -0.88), vec2(-0.38, 0.28), vec2(0.97, 0.76), vec2(0.44, -0.98),\n"
    "  vec2(0.54, -0.47), vec2(-0.26, -0.42), vec2(-0.42, 0.87), vec2(0.31, 0.92), vec2(0.79, 0.19),\n"
    "  vec2(-0.03, 0.04), vec2(0.15, -0.33));\n"
    "float sun_look(sampler2D sm, sampler2DShadow cmp, mat4 lm, vec4 p, float du, float minw, vec3 P,\n"
    "               vec3 N, float dist, float k, bool hard, out float edge) {\n"
    "  vec3 Q = P + N * (1.5 * p.x + 0.002 * dist);\n"
    "  vec4 lc = lm * vec4(Q, 1.0);\n"
    "  vec2 uv = vec2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);\n"
    "  vec2 e = abs(lc.xy);\n"
    "  edge = lc.z >= 1.0 ? 0.0 : 1.0 - smoothstep(0.8, 0.95, max(e.x, e.y));\n"
    "  if (edge <= 0.0) return 1.0;\n"
    "  float z = lc.z - p.y, sz = float(textureSize(sm, 0).x), tx = 1.0 / sz;\n"
    "  float a = k * 6.2831853, ca = cos(a), sa = sin(a);\n"
    "  mat2 rot = mat2(vec2(ca, sa), vec2(-sa, ca));\n"
    "  float bs = 0.0, bn = 0.0;\n"
    "  for (int i = 0; i < 16; ++i) {\n"
    "    vec2 q = clamp((uv + DISK[i] * (32.0 * tx)) * sz, 0.0, sz - 1.0);\n"
    "    float d = texelFetch(sm, ivec2(q), 0).r;\n"
    "    if (d < z) bs += d, bn += 1.0;\n"
    "  }\n"
    "  if (bn == 0.0) return 1.0;\n"
    "  float pen = clamp((z - bs / bn) * p.z, hard ? 0.5 * tx : max(1.5 * tx, 0.02 / (p.x * sz)), 32.0 * tx);\n"
    "  float near = minw > 0.0 ? smoothstep(0.5 * minw, 1.5 * minw, (z - bs / bn) * du) : 1.0;\n"
    "  float zc = z - pen * p.w, s = 0.0;\n"
    "  for (int i = 0; i < 16; ++i) s += texture(cmp, vec3(uv + rot * DISK[i] * pen, zc));\n"
    "  return mix(1.0, s / 16.0, near);\n"
    "}\n"
    "float sun_map(vec3 P, vec3 N, float dist, float k) {\n"
    "  float nl = dot(N, u.sun.xyz);\n"
    "  float face = mix(1.0 - 0.6 * u.smap2.y, 1.0, smoothstep(-0.3, 0.25, nl)), use = smoothstep(-0.05, 0.15, nl);\n"
    /* turned from the sun: looked up too, shaded only by casters a unit or more away (gfx_hlsl.c) */
    "  float mw = mix(max(u.smap2.z, 1.0), u.smap2.z, use);\n"
    "  float en = 0.0, ef = 0.0, s = 1.0;\n"
    "  if (u.smapn2.y > 0.0)\n"
    "    s = sun_look(t2, smnc, u.lmatn, u.smapn, u.smapn2.x, mw, P, N, dist, k, u.smapn2.z > 0.0, en);\n"
    "  if (en < 1.0) {\n"
    "    float sf = sun_look(t1, smc, u.lmat, vec4(u.smap.yzw, u.smap2.x), u.smap2.w, mw, P, N, dist, k, u.smapn2.z > 0.0, ef);\n"
    "    s = mix(sf, s, en);\n"
    "  }\n"
    "  return min(mix(1.0, s, max(en, ef)), face);\n" /* the face's shade with a map or without */
    "}\n"
    "void main() {\n"
    "  vec2 px = floor(u.vp.xy + vuv * u.vp.zw) + 0.5;\n"
    "  vec3 P = pos_at(t0, px);\n"
    "  float dist = P.z * u.hand.x;\n"
    "  if (dist <= 0.0) { oc = vec4(1.0, 0.0, 1.0, 1.0); return; }\n"
    "  vec3 r = pos_at(t0, px + vec2(1, 0)) - P, l = P - pos_at(t0, px - vec2(1, 0));\n"
    "  vec3 d = pos_at(t0, px + vec2(0, 1)) - P, t = P - pos_at(t0, px - vec2(0, 1));\n"
    /* at the viewport's edge one side is the pixel itself (pos_at clamps to it): zero, its cross a NaN */
    "  vec3 dx = (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0 ? r : l;\n"
    "  vec3 dy = (abs(d.z) < abs(t.z) && dot(d, d) > 0.0) || dot(t, t) == 0.0 ? d : t;\n"
    "  vec3 nc = cross(dx, dy);\n"
    "  vec3 N = dot(nc, nc) > 1e-24 ? normalize(nc) : -normalize(P);\n"
    "  if (dot(N, P) > 0.0) N = -N;\n"
    "  const int BAYER[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);\n"
    "  ivec2 cell = ivec2(gl_FragCoord.xy) & 3;\n"
    "  float k = fract((float(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0 + u.hist.y);\n"
    "  float sh = u.shadow.x > 0.0 && u.sun.w > 0.0 ? sun_shadow(P, N, dist, k) : 1.0;\n"
    "  float mp = u.smap.x > 0.0 && u.sun.w > 0.0 ? sun_map(P, N, dist, k) : 1.0;\n"
    "  float rad = u.ao.x, rpx = min(rad * u.proj.y * 0.5 * u.vp.w / dist, u.ao.w);\n"
    "  if (u.ao.y <= 0.0 || rpx < 2.0) { oc = vec4(1.0, dist, mp, sh); return; }\n"
    "  int NS = max(int(u.aop.x), 1);\n"
    "  float sum = 0.0;\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float a = (float(i) + k) / float(NS);\n"
    "    float ang = float(i) * 2.3999632 + k * 6.2831853;\n"
    "    vec2 q = px + vec2(cos(ang), sin(ang)) * (a * rpx);\n"
    "    if (outside(q)) continue;\n"
    "    vec3 Q = pos_lz(q, a * rpx);\n"
    "    if (Q.z == 0.0 || dist - Q.z * u.hand.x > 0.5 * rad) continue;\n"
    "    vec3 v = Q - P;\n"
    "    float vv = dot(v, v), vn = dot(v, N);\n"
    "    float q2 = vv / (rad * rad), fall = clamp(1.0 - q2 * q2, 0.0, 1.0);\n"
    "    sum += fall * max(vn * inversesqrt(vv + 1e-6) - u.ao.z, 0.0);\n"
    "  }\n"
    "  float facing = smoothstep(0.1, 0.4, dot(N, -P) / dist);\n"
    "  oc = vec4(clamp(1.0 - 3.0 * facing * sum / float(NS), 0.0, 1.0), dist, mp, sh);\n"
    "}\n"
    "#endif\n"
    "\n"
    /* the bounce light (gfx_hlsl.c's fx_gi, which says what each step does): depth at t0, the map's depth
     * at t1, its colour at t2; rgb the light, a the distance */
    "#ifdef FX_GI\n"
    "vec3 normal_at(vec2 px, vec3 P) {\n"
    "  vec3 r = pos_at(t0, px + vec2(1, 0)) - P, l = P - pos_at(t0, px - vec2(1, 0));\n"
    "  vec3 d = pos_at(t0, px + vec2(0, 1)) - P, t = P - pos_at(t0, px - vec2(0, 1));\n"
    "  vec3 dx = (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0 ? r : l;\n"
    "  vec3 dy = (abs(d.z) < abs(t.z) && dot(d, d) > 0.0) || dot(t, t) == 0.0 ? d : t;\n"
    "  vec3 nc = cross(dx, dy);\n"
    "  vec3 N = dot(nc, nc) > 1e-24 ? normalize(nc) : -normalize(P);\n"
    "  return dot(N, P) > 0.0 ? -N : N;\n"
    "}\n"
    "void main() {\n"
    "  vec2 px = floor(u.vp.xy + vuv * u.vp.zw) + 0.5;\n"
    "  vec3 P = pos_at(t0, px);\n"
    "  float dist = P.z * u.hand.x;\n"
    "  oc = vec4(0.0);\n"
    "  if (dist <= 0.0) return;\n"
    "  oc.a = dist;\n"
    "  vec4 q = u.gimat * vec4(P, 1.0);\n"
    "  vec2 e = abs(q.xy);\n"
    "  float edge = (1.0 - smoothstep(0.8, 0.95, max(e.x, e.y))) * (1.0 - smoothstep(0.7 * u.gip.w, u.gip.w, dist));\n"
    "  if (edge <= 0.0 || q.z >= 1.0) return;\n"
    "  vec3 N = normal_at(px, P);\n"
    "  vec2 uv = vec2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5), sz = vec2(textureSize(t1, 0));\n"
    "  const int BAYER[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);\n"
    "  ivec2 cell = ivec2(gl_FragCoord.xy) & 3;\n"
    "  float k = fract((float(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0 + u.hist.y);\n"
    "  int NS = max(int(u.gip.z), 1);\n"
    "  float r2 = u.gi.z * u.gi.z;\n"
    "  vec3 sum = vec3(0.0);\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float t = (float(i) + k) / float(NS), a = float(i) * 2.3999632 + k * 6.2831853;\n"
    "    vec2 us = uv + vec2(cos(a), sin(a)) * (sqrt(t) * u.gi.y);\n"
    "    if (any(lessThanEqual(us, vec2(0.0))) || any(greaterThanEqual(us, vec2(1.0)))) continue;\n"
    "    float zs = texelFetch(t1, ivec2(us * sz), 0).r;\n"
    "    if (zs >= 1.0) continue;\n"
    "    vec4 x = u.giinv * vec4(us.x * 2.0 - 1.0, 1.0 - us.y * 2.0, zs, 1.0);\n"
    "    vec3 v = x.xyz / x.w - P;\n"
    "    float dd = dot(v, v) + 1e-4;\n"
    "    vec3 vn = v * inversesqrt(dd);\n"
    "    float w = clamp(dot(N, vn), 0.0, 1.0) * clamp(0.25 - 0.75 * dot(u.sun.xyz, vn), 0.0, 1.0) * r2 / (dd + 0.25 * r2) *\n"
    "      clamp(2.0 - dd / r2, 0.0, 1.0);\n"
    "    sum += textureLod(t2, us, u.gi.w).rgb * w;\n"
    "  }\n"
    "  oc.rgb = sum * (u.gi.x * edge / float(NS));\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_GITEMP\n"
    "void main() {\n"
    "  vec4 c = texelFetch(t0, ivec2(gl_FragCoord.xy), 0);\n"
    "  oc = c;\n"
    "  if (c.a <= 0.0 || u.gip.x == 0.0) return;\n"
    "  vec2 px = u.vp.xy + vuv * u.vp.zw;\n"
    "  vec3 P = view_pos(px, c.a * u.hand.x);\n"
    "  vec4 pcl = u.reproj * vec4(P, 1.0);\n"
    "  if (pcl.w <= 1e-4) return;\n"
    "  vec2 puv = vec2(pcl.x / pcl.w * 0.5 + 0.5, 0.5 - pcl.y / pcl.w * 0.5);\n"
    "  if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) return;\n"
    "  vec4 h = texture(t1, puv);\n"
    "  if (!(h.a > 0.0) || abs(h.a - pcl.w) > 0.04 * pcl.w) return;\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy), hi = textureSize(t0, 0) - 1;\n"
    "  vec3 lo = c.rgb, up = c.rgb;\n"
    "  for (int dy = -1; dy <= 1; ++dy)\n"
    "    for (int dx = -1; dx <= 1; ++dx) {\n"
    "      vec4 t = texelFetch(t0, clamp(p + ivec2(dx, dy), ivec2(0), hi), 0);\n"
    "      if (t.a > 0.0 && abs(t.a - c.a) < 0.05 * c.a) lo = min(lo, t.rgb), up = max(up, t.rgb);\n"
    "    }\n"
    "  vec3 give = 0.1 * (up - lo) + 0.01;\n"
    "  oc = vec4(mix(c.rgb, clamp(h.rgb, lo - give, up + give), u.gip.y), c.a);\n"
    "}\n"
    "#endif\n"
    "\n"
    /* one level of the bounce light's map from the level above: the average of the 2x2 over it */
    "#ifdef FX_MIP\n"
    "void main() { oc = texture(t0, vuv); }\n"
    "#endif\n"
    "\n"
    "#ifdef FX_BLUR\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy), hi = ivec2(u.size.zw) - 1;\n"
    "  vec4 c = texelFetch(t0, p, 0);\n"
    "  if (c.y <= 0.0) { oc = c; return; }\n"
    "  vec3 s = c.xzw;\n"
    "  float w = 1.0, sw = 1.0;\n"
    "  vec2 ss = c.zw;\n"
    "  for (int i = -2; i <= 2; ++i) {\n"
    "    if (i == 0) continue;\n"
    "    vec4 t = texelFetch(t0, clamp(p + pc.dir * i, ivec2(0), hi), 0);\n"
    "    float k = (abs(i) == 2 ? 0.5 : 1.0) * clamp(1.0 - abs(t.y - c.y) / (0.03 * c.y), 0.0, 1.0);\n"
    "    s.x += t.x * k, w += k;\n"
    "    if (abs(i) == 1 && u.smapn2.z == 0.0) ss += t.zw * (0.5 * k), sw += 0.5 * k;\n"
    "  }\n"
    "  oc = vec4(s.x / w, c.y, ss / sw);\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_TEMPORAL\n"
    "void main() {\n"
    "  vec4 c = texelFetch(t0, ivec2(gl_FragCoord.xy), 0);\n"
    "  oc = c;\n"
    "  if (c.y <= 0.0 || u.hist.x == 0.0) return;\n"
    "  vec2 px = u.vp.xy + vuv * u.vp.zw;\n"
    "  vec3 P = view_pos(px, c.y * u.hand.x);\n"
    "  vec4 pcl = u.reproj * vec4(P, 1.0);\n"
    "  if (pcl.w <= 1e-4) return;\n"
    "  vec2 puv = vec2(pcl.x / pcl.w * 0.5 + 0.5, 0.5 - pcl.y / pcl.w * 0.5);\n"
    "  if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) return;\n"
    "  vec4 h = texture(t1, puv);\n"
    "  if (!(h.y > 0.0) || abs(h.y - pcl.w) > 0.04 * pcl.w) return;\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy), hi = ivec2(u.size.zw) - 1;\n"
    "  vec3 lo = c.xzw, up = c.xzw;\n"
    "  for (int dy = -1; dy <= 1; ++dy)\n"
    "    for (int dx = -1; dx <= 1; ++dx) {\n"
    "      vec4 t = texelFetch(t0, clamp(p + ivec2(dx, dy), ivec2(0), hi), 0);\n"
    "      if (t.y > 0.0 && abs(t.y - c.y) < 0.05 * c.y) lo = min(lo, t.xzw), up = max(up, t.xzw);\n"
    "    }\n"
    "  const vec3 give = vec3(0.06, 0.02, 0.02);\n"
    "  vec3 m = mix(c.xzw, clamp(h.xzw, lo - give, up + give), u.hist.z);\n"
    "  oc = vec4(m.x, c.y, m.y, m.z);\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_BRIGHT\n"
    "void main() {\n"
    "  vec2 uv = (u.vp.xy + vuv * u.vp.zw) / u.size.xy, t = 1.0 / u.size.xy;\n"
    "  vec3 c = 0.25 * (texture(t0, uv + t * vec2(-1, -1)).rgb + texture(t0, uv + t * vec2(1, -1)).rgb +\n"
    "                   texture(t0, uv + t * vec2(-1, 1)).rgb + texture(t0, uv + t * vec2(1, 1)).rgb);\n"
    /* as the composite will shade it: the scene is copied before the occlusion and the sun's shadows,
     * and a character in a cliff's shadow, drawn dark, still glowed as bright as in the open */
    "  if (u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0) {\n"
    "    vec4 os = textureLod(t1, vuv, 0.0);\n"
    "    c *= mix(1.0, os.x, u.ao.y) * mix(1.0, os.z, u.smap.x) * mix(1.0, os.w, u.shadow.x);\n"
    "  }\n"
    "  float l = max(c.r, max(c.g, c.b)), k = u.bloom.z;\n"
    "  float soft = clamp(l - u.bloom.x + k, 0.0, 2.0 * k);\n"
    "  soft = soft * soft / (4.0 * k + 1e-5);\n"
    "  oc = vec4(c * (max(soft, l - u.bloom.x) / max(l, 1e-5)), 1.0);\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_FXAA\n"
    "const float FXAA_Q[10] = float[10](1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0);\n"
    "float luma(vec2 p) { return dot(texture(t0, p).rgb, LUMA); }\n"
    "void main() {\n"
    "  vec2 rcp = 1.0 / u.size.xy, uv = gl_FragCoord.xy * rcp;\n"
    "  vec3 c = texture(t0, uv).rgb;\n"
    "  float lm = dot(c, LUMA);\n"
    "  float ln = luma(uv + vec2(0, -rcp.y)), ls = luma(uv + vec2(0, rcp.y));\n"
    "  float lw = luma(uv + vec2(-rcp.x, 0)), le = luma(uv + vec2(rcp.x, 0));\n"
    "  float mx = max(lm, max(max(ln, ls), max(lw, le))), mn = min(lm, min(min(ln, ls), min(lw, le))), range = mx - mn;\n"
    "  if (range < max(0.0312, mx * 0.125)) { oc = vec4(c, 1.0); return; }\n"
    "  float lnw = luma(uv + vec2(-rcp.x, -rcp.y)), lne = luma(uv + vec2(rcp.x, -rcp.y));\n"
    "  float lsw = luma(uv + vec2(-rcp.x, rcp.y)), lse = luma(uv + vec2(rcp.x, rcp.y));\n"
    "  float eh = abs(lnw + lne - 2.0 * ln) + 2.0 * abs(lw + le - 2.0 * lm) + abs(lsw + lse - 2.0 * ls);\n"
    "  float ev = abs(lnw + lsw - 2.0 * lw) + 2.0 * abs(ln + ls - 2.0 * lm) + abs(lne + lse - 2.0 * le);\n"
    "  bool horz = eh >= ev;\n"
    "  float l1 = horz ? ln : lw, l2 = horz ? ls : le, g1 = abs(l1 - lm), g2 = abs(l2 - lm);\n"
    "  float stp = horz ? rcp.y : rcp.x, lavg, grad;\n"
    "  if (g1 >= g2) { stp = -stp; lavg = 0.5 * (l1 + lm); grad = g1; } else { lavg = 0.5 * (l2 + lm); grad = g2; }\n"
    "  vec2 e = uv, along = horz ? vec2(rcp.x, 0) : vec2(0, rcp.y);\n"
    "  if (horz) e.y += stp * 0.5; else e.x += stp * 0.5;\n"
    "  vec2 p1 = e - along, p2 = e + along;\n"
    "  float d1 = luma(p1) - lavg, d2 = luma(p2) - lavg;\n"
    "  bool r1 = abs(d1) >= grad * 0.25, r2 = abs(d2) >= grad * 0.25;\n"
    "  for (int i = 0; i < 10 && !(r1 && r2); ++i) {\n"
    "    if (!r1) { p1 -= along * FXAA_Q[i]; d1 = luma(p1) - lavg; r1 = abs(d1) >= grad * 0.25; }\n"
    "    if (!r2) { p2 += along * FXAA_Q[i]; d2 = luma(p2) - lavg; r2 = abs(d2) >= grad * 0.25; }\n"
    "  }\n"
    "  float dist1 = horz ? uv.x - p1.x : uv.y - p1.y, dist2 = horz ? p2.x - uv.x : p2.y - uv.y;\n"
    "  bool near1 = dist1 < dist2;\n"
    "  float dmin = min(dist1, dist2), len = dist1 + dist2;\n"
    "  bool mid_lower = lm < lavg, good = ((near1 ? d1 : d2) < 0.0) != mid_lower;\n"
    "  float off = good ? -dmin / len + 0.5 : 0.0;\n"
    "  float avg = (2.0 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 12.0;\n"
    "  float sub = clamp(abs(avg - lm) / range, 0.0, 1.0);\n"
    "  sub = (-2.0 * sub + 3.0) * sub * sub;\n"
    "  off = max(off, sub * sub * 0.75);\n"
    "  vec2 f = uv;\n"
    "  if (horz) f.y += off * stp; else f.x += off * stp;\n"
    "  oc = vec4(texture(t0, f).rgb, 1.0);\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_DOWN\n"
    "void main() {\n"
    "  vec2 tx = 1.0 / vec2(textureSize(t0, 0));\n"
    "  oc = 0.25 * (texture(t0, vuv + tx * vec2(-1, -1)) + texture(t0, vuv + tx * vec2(1, -1)) +\n"
    "               texture(t0, vuv + tx * vec2(-1, 1)) + texture(t0, vuv + tx * vec2(1, 1)));\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_GAUSS\n"
    "void main() {\n"
    "  vec2 tx = vec2(pc.dir) / vec2(textureSize(t0, 0));\n"
    "  vec4 c = texture(t0, vuv) * 0.2270270;\n"
    "  c += (texture(t0, vuv + tx * 1.3846154) + texture(t0, vuv - tx * 1.3846154)) * 0.3162162;\n"
    "  c += (texture(t0, vuv + tx * 3.2307692) + texture(t0, vuv - tx * 3.2307692)) * 0.0702703;\n"
    "  oc = c;\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_RAYMASK\n"
    "void main() {\n"
    "  vec2 px = floor(u.vp.xy + vuv * u.vp.zw) + 0.5;\n"
    "  if (view_z(depth_at(t1, px)) != 0.0) { oc = vec4(0.0); return; }\n"
    "  vec3 c = texture(t0, px / u.size.xy).rgb;\n"
    "  vec2 d = (vuv - u.sunuv.xy) * vec2(u.proj.y / u.proj.x, 1.0);\n"
    "  float glow = clamp(1.0 - length(d) / 0.6, 0.0, 1.0);\n"
    "  oc = vec4(c * smoothstep(0.35, 0.9, dot(c, LUMA)) * glow * glow, 1.0);\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_RAYS\n"
    "void main() {\n"
    "  const int NS = 64;\n"
    "  vec2 uv = vuv, stp = (vuv - u.sunuv.xy) * (u.rays.z / float(NS));\n"
    "  vec3 acc = vec3(0.0);\n"
    "  float w = 1.0;\n"
    "  for (int i = 0; i < NS; ++i) { acc += texture(t0, uv).rgb * w; w *= u.rays.y; uv -= stp; }\n"
    "  oc = vec4(acc * (4.0 / float(NS)), 1.0);\n"
    "}\n"
    "#endif\n"
    "\n"
    "#ifdef FX_COMP\n"
    "vec3 ao_at(vec2 uv, float dist) {\n"
    "  if (dist <= 0.0) return vec3(1.0);\n"
    "  vec2 g = uv * u.size.zw - 0.5, f = fract(g);\n"
    "  ivec2 i0 = ivec2(floor(g)), hi = ivec2(u.size.zw) - 1;\n"
    "  vec3 s = vec3(0.0);\n"
    "  float w = 0.0;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    ivec2 o = ivec2(k & 1, k >> 1);\n"
    "    vec4 t = texelFetch(t1, clamp(i0 + o, ivec2(0), hi), 0);\n"
    "    float bw = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);\n"
    "    float dw = t.y > 0.0 ? 1.0 / (1e-3 + abs(t.y - dist) / dist) : 1e-3;\n"
    "    s += t.xzw * bw * dw, w += bw * dw;\n"
    "  }\n"
    "  return w > 0.0 ? s / w : vec3(1.0);\n"
    "}\n"
    "vec3 screen(vec3 a, vec3 b) { return 1.0 - (1.0 - clamp(a, 0.0, 1.0)) * (1.0 - clamp(b, 0.0, 1.0)); }\n"
    /* the bounce light at uv from its half size: the four round it, each as near in distance as it is */
    "vec3 gi_at(vec2 uv, float dist) {\n"
    "  if (dist <= 0.0) return vec3(0.0);\n"
    "  ivec2 sz = textureSize(t6, 0), hi = sz - 1;\n"
    "  vec2 g = uv * vec2(sz) - 0.5, f = fract(g);\n"
    "  ivec2 i0 = ivec2(floor(g));\n"
    "  vec3 s = vec3(0.0);\n"
    "  float w = 0.0;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    ivec2 o = ivec2(k & 1, k >> 1);\n"
    "    vec4 t = texelFetch(t6, clamp(i0 + o, ivec2(0), hi), 0);\n"
    "    float bw = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);\n"
    "    float dw = t.a > 0.0 ? 1.0 / (1e-3 + abs(t.a - dist) / dist) : 1e-3;\n"
    "    s += t.rgb * bw * dw, w += bw * dw;\n"
    "  }\n"
    "  return w > 0.0 ? s / w : vec3(0.0);\n"
    "}\n"
    "void main() {\n"
    "  vec2 px = gl_FragCoord.xy;\n"
    "  vec4 c = texelFetch(t0, ivec2(px), 0);\n"
    "  int dbg = int(u.grade.w);\n"
    "  vec3 os = u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0 ? ao_at(vuv, view_z(depth_at(t2, px)) * u.hand.x) : vec3(1.0);\n"
    /* what glows (a lamp's glass, a lit doorway: near white in a colour) is light, not a surface: the occlusion
     * leaves it, as it greyed the lamps it stood next to */
    "  float o = mix(os.x, 1.0, smoothstep(0.6, 0.95, max(c.r, max(c.g, c.b)))), sun = mix(1.0, os.y, u.smap.x) * mix(1.0, os.z, u.shadow.x);\n"
    "  if (dbg == 1) { oc = vec4(o, o, o, c.a); return; }\n"
    "  if (dbg == 5) { oc = vec4(vec3(sun), c.a); return; }\n"
    /* the bounce light (t6) on the surface's own colour, mostly where the sun does not reach (gfx_hlsl.c) */
    "  vec3 gl = u.gi.x > 0.0 ? gi_at(vuv, view_z(depth_at(t2, px)) * u.hand.x) : vec3(0.0);\n"
    "  if (dbg == 6) { oc = vec4(gl * 3.0, c.a); return; }\n"
    "  if (dbg == 7 && px.x < u.vp.x + u.vp.z * 0.5) gl = vec3(0.0);\n"
    "  vec3 base = c.rgb;\n"
    "  c.rgb *= mix(1.0, o, u.ao.y) * sun;\n"
    "  c.rgb += base * gl * mix(1.0, o, u.ao.y) * (1.0 - 0.75 * mix(1.0, os.y, u.smap.x));\n"
    "  float f = 0.0;\n"
    "  if (u.fogc.a > 0.0) {\n"
    "    float z = view_z(depth_at(t2, px));\n"
    "    if (z != 0.0) {\n"
    "      vec3 P = view_pos(px, z);\n"
    "      float d = length(P), bd = u.fogp.x * dot(P, u.up.xyz);\n"
    "      float k = abs(bd) > 1e-4 ? (1.0 - exp(-bd)) / bd : 1.0;\n"
    "      f = min(1.0 - exp(-u.fogc.a * d * k), u.fogp.y);\n"
    "      float g = u.fogp.w, cs = dot(P / max(d, 1e-5), u.sun.xyz);\n"
    "      float sunk = u.sun.w * u.fogp.z * pow((1.0 - g) * (1.0 - g) / max(1.0 + g * g - 2.0 * g * cs, 1e-5), 1.5);\n"
    "      c.rgb = mix(c.rgb, u.fogc.rgb + u.suncol.rgb * sunk, f);\n"
    "    }\n"
    "  }\n"
    "  if (dbg == 2) { oc = vec4(f, f, f, c.a); return; }\n"
    "  vec3 add = vec3(0.0);\n"
    "  if (u.bloom.y > 0.0) {\n"
    "    vec3 bl = texture(t3, vuv).rgb * 0.6 + texture(t4, vuv).rgb * 0.8;\n"
    "    if (dbg == 3) { oc = vec4(bl, c.a); return; }\n"
    "    add += bl * u.bloom.y;\n"
    "  }\n"
    "  if (u.rays.x > 0.0 && u.sunuv.z > 0.0) {\n"
    "    vec3 r = texture(t5, vuv).rgb * u.suncol.rgb * u.sunuv.z;\n"
    "    if (dbg == 4) { oc = vec4(r, c.a); return; }\n"
    "    add += r * u.rays.x;\n"
    "  }\n"
    "  c.rgb = screen(c.rgb, add);\n"
    "  vec3 x = mix(vec3(dot(c.rgb, LUMA)), c.rgb, u.grade.y);\n"
    "  x = clamp(x, 0.0, 1.0);\n"
    /* the contrast curve leaves what nothing was drawn on (no depth). (The game's sky dome has depth: it
     * stands round the camera, no farther than the walls, so the curve still darkens it.) */
    "  x = mix(x, x * x * (3.0 - 2.0 * x), u.grade.z * (view_z(depth_at(t2, px)) != 0.0 ? 1.0 : 0.0));\n"
    "  c.rgb = mix(c.rgb, x, u.grade.x);\n"
    "  oc = c;\n"
    "}\n"
    "#endif\n"
    "#endif\n";

#define GI_MAP 1024 /* the bounce light's map's texels across */
#define GI_FORMAT VK_FORMAT_R8G8B8A8_UNORM

static struct
{
    int tried;
    VkShaderModule vs;
    VkPipeline ao_pipe, blur_pipe, bright_pipe, down_pipe, gauss_pipe, raymask_pipe, rays_pipe, comp_pipe, temporal_pipe, aa_pipe,
        linz_pipe, zmip_pipe, gi_pipe, gitemp_pipe, mip_pipe;
    GfxTex* lz;     /* the occlusion's depth: view z at its size and three levels below (FX_LINZ, FX_ZMIP) */
    GfxTex* aa_src; /* the scene as it was, for the anti-aliasing pass to read (scene_aa) */
    GfxTex *src, *ao0, *ao1, *b1a, *b1b, *b2a, *b2b, *ra, *rb;
    GfxTex* hist[2];      /* the occlusion and shadows after the temporal pass: this frame's and the one before */
    int hist_at;          /* which of hist[] the frame before wrote */
    uint64_t hist_serial; /* the frame it was written (0: none) */
    float prev_view[16], prev_proj[16], prev_cam[3];
    float last_cam[3]; /* where the camera was at the last scene (a jump is a new place) */
    GfxTex *smap, *smapn, *sdummy; /* the sun's shadow maps (far, near), a stand-in */
    /* the bounce light: the casters near the camera as the sun sees them, depth and colour (with three
     * levels below); the gather at half the occlusion's size; after the temporal pass, this frame's and
     * the one before; the sampler that reads the colour's levels (samp reads level 0 alone) */
    GfxTex *gimap, *gicol, *gi0, *gi1, *gih[2]; /* (gi1: the traced bounce light's smoothing, rt_giblur) */
    int gih_at;
    uint64_t gih_serial, prev_serial; /* the frames the bounce's history and the camera (prev_view) were kept */
    VkSampler mipsamp;
    uint32_t mip_in; /* fx_pass: the inputs read through mipsamp (a bit each) */
    float focus[3];                /* the player's place in the world (gfx_set_focus) */
    int has_focus;
    /* what the fog and rays follow, eased from frame to frame (fx_ease) */
    int eased;
    uint64_t eased_serial;
    float fog_on, fogc[3], up[3], sun[3], suncol[3];
    float sunw[3]; /* toward the sun in the world, as the last lit draw gave it, and the frame it was seen */
    float sun0[3], sunt[3], sunp; /* the sun's glide from its last step (sun0) to the game's (sunt), sunp of the way */
    float direct;  /* how much of the game's light is the sun's, eased */
    uint64_t sunw_seen;
    /* the shadows' profile (FFXI_PROFILE) */
    uint32_t st_frames, st_own, st_map, st_cmin, st_cmax, st_drawn_this, st_cached;
    float st_across;
    uint64_t st_last;
    /* the trace (gfx_trace_dump): each scene done */
    uint32_t tr_live, tr_cached, tr_skipped, tr_depth;
    float tr_strength, tr_day, tr_dl, tr_al, tr_fog[3];
    int fogc_set;
    struct { uint64_t serial; uint32_t live, cached, skipped, own, depth; float fog[3], dl, al, strength, day, sun[3], cam[3], across; } trace[1200];
    uint32_t ntrace;
    /* the water (water_mode): the scene's camera and light as its draws need them (set by gfx_scene_done
     * the frame it was), and the copies of the target it draws over */
    struct WaterU
    {
        float iv[16], view[16], zp[4], hand[4], size[4], sun[4], suncol[4], sky[4], p[4], p2[4];
    } wu;
    uint64_t wu_serial;
    GfxTex *wcol, *wdep;
    const GfxTex* w_from; /* the target they were copied from, and the frame */
    uint64_t w_serial;
    VkBuffer ub;          /* the uniforms of the passes being recorded (fx_uniforms) */
    VkDeviceSize uoff;
} g_fx;

static VkDescriptorSetLayout g_fx_dsl;
static VkPipelineLayout g_fx_layout;

/* a toward b by k; the first time, b */
static void fx_ease(float* a, const float* b, int n, float k)
{
    for (int i = 0; i < n; ++i)
        a[i] = g_fx.eased ? a[i] + (b[i] - a[i]) * k : b[i];
}

/* In a Mog House (host64.c, from the zone-in packet): the room's ceiling stands between the sun and
 * everything in it, so the sun's shadows would black the room out; they are scaled by the moghouse
 * setting there (0, the default: none, the room as the game lights it). */
static volatile int g_moghouse;
void gfx_set_moghouse(int in) { g_moghouse = in; }

/* the last frame the sun's shadows were drawn a quarter or more of a day's strength (scene_fx) */
static uint64_t g_sun_shown;
int gfx_sun_shadows_shown(void) { return g_sun_shown && g_serial - g_sun_shown <= 30; }
float gfx_sun_prime(float* center) { (void)center; return 0.0f; } /* the zone out of view: Metal's only */

void gfx_set_focus(const float* pos)
{
    g_fx.has_focus = pos != NULL;
    if (pos)
        memcpy(g_fx.focus, pos, sizeof g_fx.focus);
}

/* defines and then text, compiled as one module (stage as compile_glsl's) */
static VkShaderModule compile_with(const char* defines, const char* text, int stage)
{
    size_t n = strlen(defines) + strlen(text) + 1;
    char* src = (char*)malloc(n);
    snprintf(src, n, "%s%s", defines, text);
    VkShaderModule m = compile_glsl(src, stage);
    free(src);
    return m;
}

/* the blend of a full-screen pass: 0 none, 1 added, 2 screen (src (1 - dst) + dst), 3 over by the source's alpha, 4 over
 * premultiplied; the target's alpha kept where blended */
static VkPipelineColorBlendAttachmentState fs_blend(int blend)
{
    VkPipelineColorBlendAttachmentState b = { 0 };
    b.colorWriteMask = 0xF;
    if (blend)
    {
        b.blendEnable = VK_TRUE;
        b.colorBlendOp = b.alphaBlendOp = VK_BLEND_OP_ADD;
        b.srcColorBlendFactor = blend == 1 || blend == 4 ? VK_BLEND_FACTOR_ONE
            : blend == 2                                ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
                                                        : VK_BLEND_FACTOR_SRC_ALPHA;
        b.dstColorBlendFactor = blend >= 3 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    }
    return b;
}

/* a pass's pipeline, only its viewport and scissor dynamic, into one colour target of format fmt: vs, and the fragment
 * functions defines + text (compiled here); blended as fs_blend says */
static VkPipeline fs_pipeline(VkShaderModule vs, const char* defines, const char* text, VkPipelineLayout layout, VkFormat fmt,
    int blend, VkPrimitiveTopology topo)
{
    VkShaderModule fs = compile_with(defines, text, 1);
    VkPipeline p = VK_NULL_HANDLE;
    if (fs && vs)
    {
        static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineColorBlendAttachmentState b = fs_blend(blend);
        p = make_pipeline(vs, fs, layout, &b, fmt, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, topo, VK_POLYGON_MODE_FILL, dyn, 2, 0);
    }
    if (fs)
        vkDestroyShaderModule(g_dev, fs, NULL);
    return p;
}

/* a compute pipeline: defines + text on layout */
static VkPipeline compute_pipeline(const char* defines, const char* text, VkPipelineLayout layout)
{
    VkShaderModule cs = compile_with(defines, text, 2);
    VkPipeline p = VK_NULL_HANDLE;
    if (cs)
    {
        VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, ci.stage.module = cs, ci.stage.pName = "main";
        ci.layout = layout;
        if (vkCreateComputePipelines(g_dev, g_vkcache, 1, &ci, NULL, &p) != VK_SUCCESS)
            p = VK_NULL_HANDLE;
        vkDestroyShaderModule(g_dev, cs, NULL);
    }
    return p;
}

/* a push-descriptor set layout of n bindings, and a pipeline layout of it with pcr (NULL: no push constants) */
static void push_layout(const VkDescriptorSetLayoutBinding* b, uint32_t n, const VkPushConstantRange* pcr, VkDescriptorSetLayout* dsl,
    VkPipelineLayout* layout)
{
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = n;
    dl.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, dsl));
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = dsl;
    pl.pushConstantRangeCount = pcr ? 1 : 0;
    pl.pPushConstantRanges = pcr;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &pl, NULL, layout));
}

/* the full-screen passes' layout - their uniforms, six textures, the two sun maps with a compare sampler, a seventh
 * texture, the lamps' list and tiles (the modern fog's halos), a push constant - and vertex function: the effects', the
 * scene renderer's occlusion's and the post effects' */
static int fs_init(void)
{
    static int tried;
    if (!tried)
    {
        tried = 1;
        VkDescriptorSetLayoutBinding b[12];
        b[0] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        for (uint32_t i = 1; i < 12; ++i)
            b[i] = (VkDescriptorSetLayoutBinding){ i, i < 10 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 };
        push_layout(b, 12, &pcr, &g_fx_dsl, &g_fx_layout);
        g_fx.vs = compile_glsl(FX_GLSL, 0);
    }
    return g_fx_layout && g_fx.vs;
}

/* one pass's pipeline: the shared vertex function, FX_<name>, into a target of format fmt */
static VkPipeline fx_pipeline(const char* name, VkFormat fmt)
{
    char d[64];
    snprintf(d, sizeof d, "#define FX_%s\n", name);
    VkPipeline p = fs_pipeline(g_fx.vs, d, FX_GLSL, g_fx_layout, fmt, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    if (!p)
        fprintf(stderr, "[recomp] gfx: scene effect %s failed\n", name);
    return p;
}

static int fx_init(void)
{
    if (g_fx.tried)
        return g_fx.ao_pipe != VK_NULL_HANDLE;
    g_fx.tried = 1;
    if (!fs_init())
        return 0;
    const VkFormat F16 = VK_FORMAT_R16G16B16A16_SFLOAT, F32 = VK_FORMAT_R32_SFLOAT;
    g_fx.ao_pipe = fx_pipeline("AO", F16);
    g_fx.blur_pipe = fx_pipeline("BLUR", F16);
    g_fx.bright_pipe = fx_pipeline("BRIGHT", F16);
    g_fx.down_pipe = fx_pipeline("DOWN", F16);
    g_fx.gauss_pipe = fx_pipeline("GAUSS", F16);
    g_fx.raymask_pipe = fx_pipeline("RAYMASK", F16);
    g_fx.rays_pipe = fx_pipeline("RAYS", F16);
    g_fx.temporal_pipe = fx_pipeline("TEMPORAL", F16);
    g_fx.linz_pipe = fx_pipeline("LINZ", F32);
    g_fx.zmip_pipe = fx_pipeline("ZMIP", F32);
    g_fx.gi_pipe = fx_pipeline("GI", F16);
    g_fx.gitemp_pipe = fx_pipeline("GITEMP", F16);
    g_fx.mip_pipe = fx_pipeline("MIP", GI_FORMAT);
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(g_dev, &si, NULL, &g_fx.mipsamp));
    g_fx.sdummy = tex_create(GFX_TEX_2D, F_D16, 1, 1, 1, GFX_USE_DEPTH, VK_FORMAT_D32_SFLOAT, 1);
    if (!g_fx.ao_pipe || !g_fx.blur_pipe || !g_fx.bright_pipe || !g_fx.down_pipe || !g_fx.gauss_pipe || !g_fx.raymask_pipe ||
        !g_fx.rays_pipe || !g_fx.temporal_pipe || !g_fx.linz_pipe || !g_fx.zmip_pipe || !g_fx.gi_pipe || !g_fx.gitemp_pipe ||
        !g_fx.mip_pipe || !g_fx.sdummy)
    {
        g_fx.ao_pipe = VK_NULL_HANDLE;
        return 0;
    }
    fprintf(stderr, "[recomp] gfx: scene effects ready\n");
    return 1;
}

/* a texture of this size and format in *t, made again when either changes: a render target, or a
 * depth surface (use) */
static GfxTex* fx_tex(GfxTex** t, VkFormat fmt, uint32_t w, uint32_t h, int use, uint32_t levels)
{
    if (*t && (*t)->w == w && (*t)->h == h && (*t)->vf == fmt && (*t)->levels == levels)
        return *t;
    gfx_tex_destroy(*t);
    *t = tex_create(GFX_TEX_2D, use == GFX_USE_DEPTH ? F_D16 : F_A8R8G8B8, w, h, levels, use, fmt, 1);
    return *t;
}

/* a view of one level of t, to read it while another level is drawn */
static VkImageView level_view(GfxTex* t, uint32_t level)
{
    if (!t->levelv)
        t->levelv = (VkImageView*)calloc(t->mips, sizeof(VkImageView));
    if (!t->levelv[level])
        t->levelv[level] = make_view(t, VK_IMAGE_VIEW_TYPE_2D, level, 1, 0, 1, NULL, VK_IMAGE_ASPECT_COLOR_BIT);
    return t->levelv[level];
}

/* the uniforms the passes recorded next read */
static void fx_uniforms(const FxU* u)
{
    void* p = ring(sizeof *u, RING_ALIGN, &g_fx.ub, &g_fx.uoff);
    memcpy(p, u, sizeof *u);
}

static void image_copy(GfxTex* src, GfxTex* dst, VkImageAspectFlags aspect)
{
    VkImageCopy c = { 0 };
    c.srcSubresource = (VkImageSubresourceLayers){ aspect, 0, 0, 1 };
    c.dstSubresource = c.srcSubresource;
    c.extent = (VkExtent3D){ src->w, src->h, 1 };
    vkCmdCopyImage(g_cb, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    src->used = dst->used = g_serial;
    src->rec = dst->rec = g_cb_index;
}

/* a pass of the effects' own, after what came before it, into one layer and level of t (and t2, a second colour target
 * the same size; NULL none): its colour, or its depth for a depth texture; load LOAD, CLEAR (depth 1, colour 0) or
 * DONT_CARE; within v (NULL: all of it); timed as pass. Its timestamp's index, for pass_close */
static int pass_open2(GfxTex* t, GfxTex* t2, uint32_t layer, uint32_t level, VkAttachmentLoadOp load, const VkViewport* v, int pass)
{
    end_pass();
    gpu_sync();
    uint32_t tw, th;
    level_size(t, level, &tw, &th);
    VkRenderingAttachmentInfo a[2] = { { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO } };
    a[0].imageView = attachment_view(t, layer, level);
    a[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    a[0].loadOp = load;
    a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[1] = a[0];
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ tw, th };
    ri.layerCount = 1;
    if (t2)
        a[1].imageView = attachment_view(t2, layer, level), t2->used = g_serial, t2->rec = g_cb_index;
    if (t->use == GFX_USE_DEPTH)
        a[0].clearValue.depthStencil.depth = 1.0f, ri.pDepthAttachment = &a[0];
    else
        ri.colorAttachmentCount = t2 ? 2 : 1, ri.pColorAttachments = a;
    int ts = ts_begin(g_cb, pass);
    vkCmdBeginRendering(g_cb, &ri);
    VkViewport all = { 0, 0, (float)tw, (float)th, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { tw, th } };
    vkCmdSetViewport(g_cb, 0, 1, v ? v : &all);
    vkCmdSetScissor(g_cb, 0, 1, &sc);
    t->used = g_serial, t->rec = g_cb_index;
    return ts;
}

static int pass_open(GfxTex* t, uint32_t layer, uint32_t level, VkAttachmentLoadOp load, const VkViewport* v, int pass)
{
    return pass_open2(t, NULL, layer, level, load, v, pass);
}

static void pass_close(int ts)
{
    vkCmdEndRendering(g_cb);
    ts_end(g_cb, ts);
    g_dirty = 1;
}

/* one full-screen triangle into a level of target (within vp, x y w h; NULL: all of it), reading
 * in[0..n) as t0.. (up to seven), and the sun maps sh[0..2) through the compare sampler; load keeps
 * what is there */
static void fx_pass(GfxTex* target, uint32_t level, int load, VkPipeline p, const float* vp, const VkImageView* in, int n,
    const VkImageView* sh, const int32_t* dir)
{
    VkViewport v = vp ? (VkViewport){ vp[0], vp[1], vp[2], vp[3], 0, 1 } : (VkViewport){ 0 };
    int ts = pass_open(target, 0, level, load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE, vp ? &v : NULL, g_pass);
    VkCommandBuffer cb = g_cb;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkWriteDescriptorSet w[10];
    VkDescriptorBufferInfo bi = { g_fx.ub, g_fx.uoff, sizeof(FxU) };
    VkDescriptorImageInfo ii[9];
    w[0] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &bi;
    for (int i = 0; i < 9; ++i)
    {
        int t = i < 6 ? i : i == 8 ? 6 : -1; /* bindings 1..6 are t0..t5, 9 is t6; 7 and 8 the sun maps */
        if (t >= 0)
            ii[i] = (VkDescriptorImageInfo){ (g_fx.mip_in >> t) & 1 ? g_fx.mipsamp : g_lin_samp, t < n && in[t] ? in[t] : g_dummy2d->view,
                VK_IMAGE_LAYOUT_GENERAL };
        else
            ii[i] = (VkDescriptorImageInfo){ g_cmp_samp, sh && sh[i - 6] ? sh[i - 6] : g_fx.sdummy->view, VK_IMAGE_LAYOUT_GENERAL };
        w[1 + i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[1 + i].dstBinding = 1 + (uint32_t)i;
        w[1 + i].descriptorCount = 1;
        w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1 + i].pImageInfo = &ii[i];
    }
    p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fx_layout, 0, 10, w);
    int32_t d2[2] = { dir ? dir[0] : 0, dir ? dir[1] : 0 };
    vkCmdPushConstants(cb, g_fx_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, d2);
    vkCmdDraw(cb, 3, 1, 0, 0);
    pass_close(ts);
}

/* the occlusion's depth texture at w x h, four levels; made again when the size changes */
static GfxTex* fx_lz(uint32_t w, uint32_t h)
{
    if (w < 8 || h < 8)
        return NULL;
    return fx_tex(&g_fx.lz, VK_FORMAT_R32_SFLOAT, w, h, GFX_USE_RT, 4);
}

enum { SUN_CACHE_FRAMES = 60 * 30, SUN_CACHE_MAX = 8192 };
#define SUN_CACHE_NEAR 40.0f               /* what was seen within this of the camera stays past SUN_CACHE_FRAMES */
#define SUN_CACHE_FAR 4.0f                 /* kept within this many times the far map's reach (sun_distance, 100 at least), and 200 more */
#define SUN_COPY_BYTES (48u * 1024 * 1024) /* the copies' vertices and indices, all told */

/* The zone's casters, kept after they leave the view (gfx_metal.m's note): each caster drawn from
 * buffers the game keeps is kept with a copy of its uniforms and that frame's camera, and drawn into
 * the map from there while it is out of view; what the game draws from buffers it rewrites is kept as
 * a copy of its vertices and indices as last drawn. */
typedef struct CacheKey
{
    uint64_t vb[GFX_NSTREAMS];
    VkDeviceSize voff[GFX_NSTREAMS];
    uint64_t ib;
    VkDeviceSize ioff;
    uint32_t n, vstart, copy;
    LibKey lib;
} CacheKey;

typedef struct Cached
{
    Caster c; /* c.ub is the entry's own copy of the uniforms (ubuf); a copy's c.vb[] and c.ib are own */
    float clip_world[16];
    float pos[3], cam[3]; /* where it stood and where the camera was, when last seen */
    uint64_t seen, replayed;
    int32_t next;        /* the next entry with its key, -1 none */
    uint32_t copy_bytes; /* a copy's vertices and indices */
    int dead;            /* a buffer or texture it draws from went (the game let go of it: a zone left behind) */
    VkBuffer own, ubuf;
    VmaAllocation own_mem, ubuf_mem;
    void *own_p, *ubuf_p;
} Cached;

static Cached* g_cache;
static uint32_t g_ncache, g_cache_cap, g_copy_bytes;
static uint32_t g_ncache_over; /* where a trim left the cache past SUN_CACHE_MAX (else 0): the next waits for a quarter more */
static Map g_cache_map; /* CacheKey -> the first entry's index + 1 */
static float g_cache_cam[3];

static void cache_key(CacheKey* k, const Caster* c, int copy)
{
    memset(k, 0, sizeof *k);
    if (!copy)
    {
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            k->vb[s] = (uint64_t)(uintptr_t)c->vb[s], k->voff[s] = c->voff[s];
        k->ib = (uint64_t)(uintptr_t)c->ib, k->ioff = c->ioff;
    }
    k->n = c->n, k->vstart = copy ? 0 : c->vstart, k->copy = (uint32_t)copy | (uint32_t)c->itype << 8 | (uint32_t)c->prim << 16;
    k->lib = c->lib;
}

static void cache_map_free(void)
{
    for (uint32_t i = 0; i < g_cache_map.cap; ++i)
        if (g_cache_map.e[i].hash)
            free(g_cache_map.e[i].key);
    free(g_cache_map.e);
    memset(&g_cache_map, 0, sizeof g_cache_map);
}

static void cached_release(Cached* ce)
{
    trash(2, (uint64_t)(uintptr_t)ce->own, ce->own_mem);
    trash(2, (uint64_t)(uintptr_t)ce->ubuf, ce->ubuf_mem);
    ce->own = ce->ubuf = VK_NULL_HANDLE, ce->own_p = ce->ubuf_p = NULL;
    Caster* c = &ce->c;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        c->vb[s] = VK_NULL_HANDLE, c->vbp[s] = NULL;
    c->ub = c->ib = VK_NULL_HANDLE, c->ibp = NULL, c->up = NULL;
    memset(c->tex, 0, sizeof c->tex);
    g_copy_bytes -= ce->copy_bytes, ce->copy_bytes = 0;
}

/* a buffer the game destroyed or rewrote: what the cache draws from it goes */
static void sun_cache_forget(VkBuffer buf)
{
    if (!buf)
        return;
    for (uint32_t i = 0; i < g_ncache; ++i)
    {
        Caster* c = &g_cache[i].c;
        if (g_cache[i].copy_bytes)
            continue; /* its own buffers */
        int hit = c->ib == buf;
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            hit |= c->vb[s] == buf;
        if (hit)
            g_cache[i].dead = 1;
    }
    /* this frame's casters from it still draw (the buffer they read goes to the trash, kept until the
     * frame is done, and gfx_buf_upload renames it), but none may go into the cache as drawn from the
     * game's buffer, whose handle would outlive it: kept, they are kept as copies */
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* c = &g_casters[i];
        int hit = c->ib == buf;
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            hit |= c->vb[s] == buf;
        if (hit)
            c->fixed = 0;
    }
}

/* caster c draws with texture t (either of its views) */
static int reads_tex(const Caster* c, const GfxTex* t)
{
    for (int k = 0; k < 8; ++k)
        if (c->tex[k] && (c->tex[k] == t->view || c->tex[k] == t->mipview))
            return 1;
    return 0;
}

/* a texture the game destroyed: what the cache draws with it (an alpha test's) goes. This frame's alpha
 * tests that read it still draw (its views go to the trash, kept until the frame is done), but none goes
 * into the cache, whose handle would outlive the view (gfx_d3d12.c's casters hold the GfxTex, freed now,
 * so it drops their draw instead) */
static void sun_cache_forget_tex(const GfxTex* t)
{
    for (uint32_t i = 0; i < g_ncache; ++i)
        if (reads_tex(&g_cache[i].c, t))
            g_cache[i].dead = 1;
    for (uint32_t i = 0; i < g_ncasters; ++i)
        if (alpha_tested(&g_casters[i].lib.fs) && reads_tex(&g_casters[i], t))
            g_casters[i].lost_tex = 1;
}

/* gone from the cache's view: not seen for SUN_CACHE_FRAMES, and the camera has moved on from where it was */
static int cached_expired(const Cached* ce)
{
    if (ce->seen + SUN_CACHE_FRAMES >= g_serial)
        return 0;
    float dx = g_cache_cam[0] - ce->cam[0], dy = g_cache_cam[1] - ce->cam[1], dz = g_cache_cam[2] - ce->cam[2];
    return dx * dx + dy * dy + dz * dz > SUN_CACHE_NEAR * SUN_CACHE_NEAR;
}

/* qsort's order for frames seen: the latest first */
static int seen_later(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* drops what is dead or expired (all of it when all is true) and rebuilds the map and its chains;
 * past SUN_CACHE_MAX, down to three quarters of it, what was seen longest ago going first (what the
 * latest frame drew all stays, so it can end above that) */
static void sun_cache_trim(int all)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_ncache; ++i)
    {
        if (all || g_cache[i].dead || cached_expired(&g_cache[i]))
            cached_release(&g_cache[i]);
        else
            g_cache[n++] = g_cache[i];
    }
    uint64_t* seen = n > SUN_CACHE_MAX ? (uint64_t*)malloc(n * sizeof *seen) : NULL;
    if (seen)
    {
        for (uint32_t i = 0; i < n; ++i)
            seen[i] = g_cache[i].seen;
        qsort(seen, n, sizeof *seen, seen_later);
        uint64_t oldest = seen[SUN_CACHE_MAX * 3 / 4 - 1];
        free(seen);
        uint32_t kept = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (g_cache[i].seen < oldest)
                cached_release(&g_cache[i]);
            else
                g_cache[kept++] = g_cache[i];
        }
        n = kept;
    }
    g_ncache = n, g_ncache_over = n > SUN_CACHE_MAX ? n : 0;
    cache_map_free();
    for (uint32_t i = 0; i < n; ++i)
    {
        CacheKey k;
        cache_key(&k, &g_cache[i].c, g_cache[i].copy_bytes != 0);
        uintptr_t at = (uintptr_t)map_get(&g_cache_map, &k, sizeof k);
        g_cache[i].next = -1;
        if (!at)
            map_put(&g_cache_map, &k, sizeof k, (void*)(uintptr_t)(i + 1));
        else /* after the first */
            g_cache[i].next = g_cache[at - 1].next, g_cache[at - 1].next = (int32_t)i;
    }
}

/* the entry for caster c (its key k) this frame: the nearest of its key within a unit not yet updated
 * this frame, else one at the very same place (the same thing drawn twice), else a new one */
static Cached* cache_slot(const CacheKey* k, const Caster* c, const float* pos, int* fresh)
{
    uintptr_t head = (uintptr_t)map_get(&g_cache_map, k, sizeof *k);
    int32_t best = -1, same = -1;
    float bd = 1.0f;
    for (int32_t i = head ? (int32_t)head - 1 : -1; i >= 0; i = g_cache[i].next)
    {
        Cached* ce = &g_cache[i];
        if (ce->dead)
            continue;
        if (!c->has_pos || !ce->c.has_pos) /* nowhere to tell copies apart: one entry for the key */
        {
            best = i;
            break;
        }
        float dx = pos[0] - ce->pos[0], dy = pos[1] - ce->pos[1], dz = pos[2] - ce->pos[2], d2 = dx * dx + dy * dy + dz * dz;
        if (ce->seen != g_serial && d2 < bd)
            bd = d2, best = i;
        if (d2 < 1e-6f)
            same = i;
    }
    *fresh = 0;
    if (best >= 0 || same >= 0)
        return &g_cache[best >= 0 ? best : same];
    if (g_ncache == g_cache_cap)
    {
        g_cache_cap = g_cache_cap ? g_cache_cap * 2 : 1024;
        g_cache = (Cached*)realloc(g_cache, g_cache_cap * sizeof(Cached));
    }
    int32_t at = (int32_t)g_ncache++;
    Cached* ce = &g_cache[at];
    memset(ce, 0, sizeof *ce);
    ce->next = -1;
    if (!head)
        map_put(&g_cache_map, k, sizeof *k, (void*)(uintptr_t)(at + 1));
    else
        ce->next = g_cache[head - 1].next, g_cache[head - 1].next = at;
    *fresh = 1;
    return ce;
}

/* makes room for a copy of `need` bytes: the copies seen longest ago go (not `keep`'s) */
static int copy_room(uint32_t need, const Cached* keep)
{
    if (need > SUN_COPY_BYTES / 4)
        return 0;
    while (g_copy_bytes + need > SUN_COPY_BYTES)
    {
        Cached* old = NULL;
        for (uint32_t i = 0; i < g_ncache; ++i)
            if (&g_cache[i] != keep && g_cache[i].copy_bytes && !g_cache[i].dead && g_cache[i].seen != g_serial &&
                (!old || g_cache[i].seen < old->seen))
                old = &g_cache[i];
        if (!old)
            return 0;
        old->dead = 1;
        cached_release(old);
    }
    return 1;
}

/* the vertices and indices caster c draws, copied into ce's own buffer: only the vertices its indices
 * reach, with the uniforms' vertex offset (ub, the cache's copy) moved to match. 0 if it cannot be. */
static int cache_copy(Cached* ce, const Caster* c, GfxU* ub)
{
    long lo = c->vstart, hi = (long)c->vstart + (long)c->n - 1;
    if (c->ib)
    {
        if (!c->ibp || (size_t)c->ioff + (size_t)c->n * c->itype > c->iblen)
            return 0;
        const uint8_t* ip = c->ibp + c->ioff;
        lo = LONG_MAX, hi = -1;
        for (uint32_t i = 0; i < c->n; ++i)
        {
            long x = c->itype == 2 ? ((const uint16_t*)ip)[i] : (long)((const uint32_t*)ip)[i];
            lo = x < lo ? x : lo, hi = x > hi ? x : hi;
        }
    }
    lo += ub->vofs, hi += ub->vofs;
    if (lo < 0 || hi < lo)
        return 0;
    /* each stream at a storage-buffer offset; past its last vertex, the bytes its binding's aligned
     * offset moved its registers by (draw_encode) */
    uint32_t off[GFX_NSTREAMS], len[GFX_NSTREAMS], total = 0;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        off[s] = total, len[s] = 0;
        if (!c->vb[s] || c->vb[s] == g_dummy || !c->vbp[s])
            continue;
        long stride = ub->stride[s], from = lo * stride, n = (hi - lo + 1) * stride + RING_ALIGN;
        if (!stride)
            from = 0, n = 64 + RING_ALIGN;
        long have = (long)c->vblen[s] - (long)c->voff[s] - from;
        if (have <= 0)
            return 0;
        len[s] = (uint32_t)(n < have ? n : have);
        total += (len[s] + RING_ALIGN - 1) & ~(uint32_t)(RING_ALIGN - 1);
    }
    uint32_t ioff = total, ilen = c->ib ? c->n * c->itype : 0;
    total += (ilen + 15) & ~15u;
    if (!total)
        return 0;
    /* the copy's buffer again when it fits and no frame in flight reads it */
    if (!(ce->own && ce->copy_bytes == total && ce->replayed + FRAMES < g_serial))
    {
        if (!copy_room(total, ce))
            return 0;
        VkBuffer b;
        VmaAllocation m;
        void* p;
        if (!host_buffer(total, STATIC_USAGE, 0, &b, &m, &p))
            return 0;
        trash(2, (uint64_t)(uintptr_t)ce->own, ce->own_mem);
        ce->own = b, ce->own_mem = m, ce->own_p = p;
    }
    uint8_t* dst = (uint8_t*)ce->own_p;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (len[s])
            memcpy(dst + off[s], c->vbp[s] + c->voff[s] + (ub->stride[s] ? lo * ub->stride[s] : 0), len[s]);
    if (ilen)
        memcpy(dst + ioff, c->ibp + c->ioff, ilen);
    /* the copy's own buffer in place of the caster's */
    g_copy_bytes -= ce->copy_bytes;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        ce->c.vb[s] = len[s] ? ce->own : g_dummy;
        ce->c.voff[s] = len[s] ? off[s] : 0;
        ce->c.vbp[s] = len[s] ? dst : NULL, ce->c.vblen[s] = len[s] ? total : 0;
    }
    ce->c.ib = ilen ? ce->own : VK_NULL_HANDLE, ce->c.ioff = ioff, ce->c.ibp = ilen ? dst : NULL, ce->c.iblen = total;
    ce->copy_bytes = total, g_copy_bytes += total;
    ub->vofs -= (int32_t)lo;
    return 1;
}

/* is a world point in plain view of this frame's camera (w >= 1, within 90% of the frustum)? */
static int in_plain_view(const float* p, const float* vp)
{
    float q[4] = { p[0], p[1], p[2], 1.0f }, c[4];
    gfx_xform4(c, q, vp);
    return c[3] >= 1.0f && fabsf(c[0]) <= 0.9f * c[3] && fabsf(c[1]) <= 0.9f * c[3];
}

/* this frame's casters into the cache (new ones added, seen ones brought up to date), then those in
 * plain view the game did not draw dropped (gfx_metal.m's sun_cache_update) */
static void sun_cache_update(const float* clip_world, const float* view, const float* vp, const float* cam)
{
    float dx = cam[0] - g_cache_cam[0], dy = cam[1] - g_cache_cam[1], dz = cam[2] - g_cache_cam[2];
    if (dx * dx + dy * dy + dz * dz > 50.0f * 50.0f)
    {
        if (gfx_profiling)
            fprintf(stderr, "[recomp] gfx: shadows: the camera jumped %.0f units (frame %llu): cache cleared\n",
                sqrtf(dx * dx + dy * dy + dz * dz), (unsigned long long)g_serial);
        sun_cache_trim(1);
        g_fx.fogc_set = 0;
    }
    memcpy(g_cache_cam, cam, 12);
    if (!(g_serial & 255) || (g_ncache > SUN_CACHE_MAX && g_ncache > g_ncache_over + SUN_CACHE_MAX / 4))
        sun_cache_trim(0);
    /* what is not drawn from the zone's own buffers: a placed object (keep) or a character (not kept) */
    float invView[16];
    int have_iv = gfx_mat_inverse(invView, view);
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* c = &g_casters[i];
        if (c->fixed)
            continue;
        c->keep = (uint8_t)(c->lib.vs.prog != 0);
        if (!c->lib.vs.prog && have_iv)
        {
            float w[16], off = 0.0f;
            gfx_mat_mul(w, c->up->wv, invView);
            for (int j = 0; j < 16; ++j)
                off = fmaxf(off, fabsf(w[j] - ((j % 5) == 0 ? 1.0f : 0.0f)));
            c->keep = off > 1e-3f;
        }
    }
    if (g_fxs.sun_casters == 1.0f) /* characters alone cast: the cache is not drawn */
        return;
    /* none of what stands further out casts into the map (whose casters stand up to 200 units sunward
     * of it: gfx_sun_fit), and what moves out there (an airship's view at the longest draw distance)
     * would leave a new entry each frame, never expiring while the camera stays aboard */
    float keep_within = SUN_CACHE_FAR * fmaxf(g_fxs.sun_distance, 100.0f) + 200.0f;
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* c = &g_casters[i];
        const GfxU* src = c->up;
        if (c->lost_tex || (!c->fixed && (!c->keep || !c->has_pos)))
            continue;
        float pos[3] = { 0, 0, 0 };
        if (c->has_pos)
        {
            float h[4];
            gfx_xform4(h, c->clip0, clip_world);
            if (fabsf(h[3]) < 1e-6f)
                continue;
            pos[0] = h[0] / h[3], pos[1] = h[1] / h[3], pos[2] = h[2] / h[3];
            float ox = pos[0] - cam[0], oy = pos[1] - cam[1], oz = pos[2] - cam[2];
            if (ox * ox + oy * oy + oz * oz > keep_within * keep_within)
                continue;
        }
        CacheKey k;
        int copy = !c->fixed, fresh;
        cache_key(&k, c, copy);
        Cached* ce = cache_slot(&k, c, pos, &fresh);
        if (fresh)
        {
            ce->c = *c;
            if (copy)
            {
                for (int s = 0; s < GFX_NSTREAMS; ++s)
                    ce->c.vb[s] = VK_NULL_HANDLE, ce->c.vbp[s] = NULL;
                ce->c.ib = VK_NULL_HANDLE, ce->c.ibp = NULL;
            }
            ce->c.ub = VK_NULL_HANDLE, ce->c.up = NULL;
        }
        else if (copy)
        {
            /* the textures as of this frame (an alpha test's) */
            memcpy(ce->c.tex, c->tex, sizeof c->tex);
            memcpy(ce->c.samp, c->samp, sizeof c->samp);
            ce->c.n = c->n, ce->c.vstart = c->vstart, ce->c.prim = c->prim, ce->c.itype = c->itype;
        }
        if (!alpha_tested(&c->lib.fs)) /* its textures are the bounce light's, which draws this frame's alone */
            memset(ce->c.tex, 0, sizeof ce->c.tex);
        /* the uniforms as of this frame: in place, unless a frame the GPU may still be drawing read them */
        if (!(ce->ubuf && ce->replayed + FRAMES < g_serial))
        {
            trash(2, (uint64_t)(uintptr_t)ce->ubuf, ce->ubuf_mem);
            ce->ubuf = VK_NULL_HANDLE;
            if (!host_buffer(sizeof(GfxU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0, &ce->ubuf, &ce->ubuf_mem, &ce->ubuf_p))
            {
                ce->dead = 1;
                continue;
            }
        }
        /* a copy's vertex offset moves to its own vertices (cache_copy) before ubuf is written: like the
         * ring, ubuf is write-combined and not read back */
        GfxU moved;
        if (copy)
        {
            memcpy(&moved, src, sizeof moved);
            if (!cache_copy(ce, c, &moved))
            {
                ce->dead = 1;
                continue;
            }
            src = &moved;
        }
        memcpy(ce->ubuf_p, src, sizeof(GfxU));
        ce->c.ub = ce->ubuf, ce->c.uoff = 0, ce->c.up = (const GfxU*)ce->ubuf_p;
        ce->c.has_pos = c->has_pos;
        memcpy(ce->c.clip0, c->clip0, 16);
        memcpy(ce->pos, pos, 12);
        memcpy(ce->cam, cam, 12);
        memcpy(ce->clip_world, clip_world, 64);
        ce->seen = g_serial;
    }
    /* in plain view, and the game did not draw it: it has gone */
    for (uint32_t i = 0; i < g_ncache; ++i)
    {
        Cached* ce = &g_cache[i];
        if (!ce->dead && ce->seen != g_serial && ce->c.has_pos && in_plain_view(ce->pos, vp))
        {
            ce->dead = 1;
            if (ce->copy_bytes)
                cached_release(ce);
        }
    }
}

static GfxTex* sun_target(GfxTex** t, int size)
{
    return fx_tex(t, VK_FORMAT_D32_SFLOAT, (uint32_t)size, (uint32_t)size, GFX_USE_DEPTH, 1);
}

/* the casters into one cascade's map: this frame's, and the zone's kept from before (cache). With col,
 * the bounce light's: every caster (whoever casts) in its own colour as well, through its own pixel
 * function, unfogged (gfx_d3d12.c's) */
static uint32_t sun_draw(GfxTex* target, GfxTex* col, const float* invP, const float* invV, const SunCascade* k, int cache)
{
    float clip_world[16], M[16];
    gfx_mat_mul(clip_world, invP, invV);
    gfx_mat_mul(M, clip_world, k->S); /* the camera's clip space -> the map */
    end_pass();
    gpu_sync();
    VkCommandBuffer cb = g_cb;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = attachment_view(target, 0, 0);
    da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ (uint32_t)k->size, (uint32_t)k->size };
    ri.layerCount = 1;
    ri.pDepthAttachment = &da;
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    if (col)
    {
        ca.imageView = attachment_view(col, 0, 0);
        ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    int ts = ts_begin(cb, GFX_PASS_SUN);
    vkCmdBeginRendering(cb, &ri);
    /* y up, as the draws' (set_viewport): the map's rows as sun_look reads them */
    VkViewport v = { 0, (float)k->size, (float)k->size, -(float)k->size, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { (uint32_t)k->size, (uint32_t)k->size } };
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &sc);
    VkBuffer mb = VK_NULL_HANDLE;
    VkDeviceSize moff = 0;
    uint32_t drawn = 0, total = g_ncasters + (cache ? g_ncache : 0);
    VkPipeline bound = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < total; ++i)
    {
        const Caster* cs;
        if (i < g_ncasters)
        {
            cs = &g_casters[i];
            if (!mb)
                memcpy(ring(64, RING_ALIGN, &mb, &moff), M, 64);
            /* sun_casters 1: characters alone cast; 2: the zone alone (the bounce light's map has them all) */
            if (!col && ((g_fxs.sun_casters == 1.0f && (cs->fixed || cs->keep)) || (g_fxs.sun_casters == 2.0f && !cs->fixed && !cs->keep)))
                continue;
            /* more than 96 units outside the map's sides (the bounce light's: 16): no shadow of it falls in it */
            if (cs->has_pos)
            {
                float h[4];
                gfx_xform4(h, cs->clip0, clip_world);
                if (fabsf(h[3]) > 1e-6f)
                {
                    float q[3] = { h[0] / h[3], h[1] / h[3], h[2] / h[3] };
                    float mx = q[0] * k->S[0] + q[1] * k->S[4] + q[2] * k->S[8] + k->S[12];
                    float my = q[0] * k->S[1] + q[1] * k->S[5] + q[2] * k->S[9] + k->S[13];
                    float edge = 1.0f + (col ? 16.0f : 96.0f) * 2.0f / k->across;
                    if (fabsf(mx) > edge || fabsf(my) > edge)
                        continue;
                }
            }
        }
        else
        {
            if (g_fxs.sun_casters == 1.0f)
                break;
            /* the zone out of view: as it was drawn when last seen, through that frame's camera */
            Cached* ce = &g_cache[i - g_ncasters];
            if (ce->dead || ce->seen == g_serial || cached_expired(ce) || !ce->c.ub)
                continue;
            /* standing more than 96 units outside the map's sides: no shadow of it falls in the map */
            if (ce->c.has_pos)
            {
                const float* q = ce->pos;
                float mx = q[0] * k->S[0] + q[1] * k->S[4] + q[2] * k->S[8] + k->S[12];
                float my = q[0] * k->S[1] + q[1] * k->S[5] + q[2] * k->S[9] + k->S[13];
                float edge = 1.0f + 96.0f * 2.0f / k->across;
                if (fabsf(mx) > edge || fabsf(my) > edge)
                    continue;
            }
            float m[16];
            gfx_mat_mul(m, ce->clip_world, k->S);
            memcpy(ring(64, RING_ALIGN, &mb, &moff), m, 64);
            ce->replayed = g_serial;
            cs = &ce->c;
        }
        PipeKey pk;
        memset(&pk, 0, sizeof pk);
        pk.lib = cs->lib;
        pk.lib.vs.shadow = 1, pk.lib.vs.pixel = 0, pk.lib.vs.water = 0, pk.lib.fs.water = 0;
        int at = alpha_tested(&cs->lib.fs), tex = at || col;
        if (col)
            pk.lib.fs.fog = 0, pk.pipe.write_mask = 15; /* its colour as the sun sees it: no fog of the camera's */
        else if (!at)
        {
            /* the position alone: one pipeline serves every draw with the same vertex layout */
            GfxVsKey* vk = &pk.lib.vs;
            vk->lighting = vk->normalize = vk->localviewer = vk->specular = 0;
            vk->src_diffuse = vk->src_specular = vk->src_ambient = vk->src_emissive = 0;
            vk->nlights = 0, memset(vk->light_type, 0, sizeof vk->light_type);
            vk->fog_vertex = vk->range_fog = 0, vk->ntex = 0, vk->flat = 0;
            memset(vk->tci, 0, sizeof vk->tci), memset(vk->ttf, 0, sizeof vk->ttf);
            memset(&pk.lib.fs, 0, sizeof pk.lib.fs);
        }
        pk.color = col ? (uint32_t)col->vf : VK_FORMAT_UNDEFINED, pk.depth = VK_FORMAT_D32_SFLOAT;
        pk.topo = cs->prim == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? 0
            : cs->prim == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || cs->prim == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ? 1 : 2;
        VkPipeline ps = pipeline_for(&pk, cs->vs, cs->ps);
        if (!ps)
        {
            g_fx.tr_skipped++;
            continue;
        }
        if (i < g_ncasters)
            g_fx.tr_live++;
        else
            g_fx.tr_cached++;
        if (ps != bound)
        {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ps), bound = ps;
            vkCmdSetDepthTestEnable(cb, VK_TRUE);
            vkCmdSetDepthWriteEnable(cb, VK_TRUE);
            vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_LESS);
            vkCmdSetStencilTestEnable(cb, VK_FALSE);
            vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                VK_COMPARE_OP_ALWAYS);
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
            vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
            vkCmdSetDepthBiasEnable(cb, VK_TRUE);
            vkCmdSetDepthBias(cb, 0.0f, 0.0f, 1.5f);
        }
        vkCmdSetPrimitiveTopology(cb, cs->prim);
        VkWriteDescriptorSet w[B_COUNT];
        VkDescriptorBufferInfo bi[GFX_NSTREAMS + 2];
        VkDescriptorImageInfo ii[8];
        uint32_t nw = 0;
        bi[0] = (VkDescriptorBufferInfo){ cs->ub, cs->uoff, sizeof(GfxU) };
        bi[1] = (VkDescriptorBufferInfo){ mb, moff, 64 };
        uint32_t ubind[2] = { B_U, B_SHADOW };
        for (int j = 0; j < 2; ++j)
        {
            w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[nw].dstBinding = ubind[j];
            w[nw].descriptorCount = 1;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[nw++].pBufferInfo = &bi[j];
        }
        for (int st = 0; st < GFX_NSTREAMS; ++st)
        {
            bi[2 + st] = cs->vb[st] ? (VkDescriptorBufferInfo){ cs->vb[st], cs->voff[st], VK_WHOLE_SIZE }
                                    : (VkDescriptorBufferInfo){ g_dummy, 0, VK_WHOLE_SIZE };
            w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[nw].dstBinding = B_STREAM0 + (uint32_t)st;
            w[nw].descriptorCount = 1;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[nw++].pBufferInfo = &bi[2 + st];
        }
        if (tex)
            for (int t = 0; t < 8; ++t)
            {
                int wanted = cs->lib.fs.prog || t < cs->lib.fs.nstages ? cs->lib.fs.st[t].tex : 0;
                if (!wanted)
                    continue;
                GfxSampler sk = cs->samp[t];
                VkImageView view = cs->tex[t] ? cs->tex[t] : wanted == 2 ? g_dummycube->view : g_dummy2d->view;
                ii[t] = (VkDescriptorImageInfo){ sampler(&sk), view, VK_IMAGE_LAYOUT_GENERAL };
                w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w[nw].dstBinding = B_TEX0 + (uint32_t)t;
                w[nw].descriptorCount = 1;
                w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[nw++].pImageInfo = &ii[t];
            }
        p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, nw, w);
        if (cs->itype)
        {
            vkCmdBindIndexBuffer(cb, cs->ib, cs->ioff, cs->itype == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, cs->n, 1, 0, 0, 0);
        }
        else
            vkCmdDraw(cb, cs->n, 1, cs->vstart, 0);
        drawn++;
    }
    vkCmdEndRendering(cb);
    ts_end(cb, ts);
    g_dirty = 1;
    g_bound = VK_NULL_HANDLE;
    target->used = g_serial, target->rec = g_cb_index;
    if (col)
        col->used = g_serial, col->rec = g_cb_index;
    return drawn;
}

/* The sun's maps for the scene: a near cascade out to sun_near units past the player and a far one out
 * to sun_distance (gfx_metal.m's sun_map). Fills the effects' uniforms for both; 0 when nothing was drawn. */
static int sun_map(const GfxScene* s, const float* L, FxU* u)
{
    if (!g_ncasters && !g_ncache)
        return 0;
    float invP[16], invV[16];
    if (!gfx_mat_inverse(invP, s->proj) || !gfx_mat_inverse(invV, s->view))
        return 0;
    float clip_world[16], vp[16];
    gfx_mat_mul(clip_world, invP, invV);
    gfx_mat_mul(vp, s->view, s->proj);
    sun_cache_update(clip_world, s->view, vp, invV + 12);
    /* sun_near 3 to 80 (0 or less: no near map); past the far map's reach it stops at it */
    float dfar = fmaxf(g_fxs.sun_distance, 4.0f), dnear = g_fxs.sun_near > 0.0f ? fminf(fmaxf(g_fxs.sun_near, 3.0f), 80.0f) : 0.0f;
    SunCascade far, near;
    gfx_sun_fit(s, invV, L, 0.5f, dfar, GFX_SUN_MAP, &far);
    if (!sun_target(&g_fx.smap, GFX_SUN_MAP))
        return 0;
    uint32_t drawn = sun_draw(g_fx.smap, NULL, invP, invV, &far, 1);
    memcpy(u->lmat, far.lmat, 64);
    u->smap[1] = far.texel, u->smap[2] = far.bias, u->smap[3] = far.soft;
    u->smap2[0] = far.slope, u->smap2[3] = far.range;
    u->smapn2[1] = 0.0f;
    /* the near map reaches sun_near past the player, not the camera */
    float lead = 0.0f;
    if (g_fx.has_focus)
    {
        float dx = g_fx.focus[0] - invV[12], dy = g_fx.focus[1] - invV[13], dz = g_fx.focus[2] - invV[14];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        lead = d < 40.0f ? d : 0.0f;
    }
    float tnear = fminf(dnear + lead, dfar);
    int nsize = gfx_sun_near_size();
    if (dnear > 0.0f && sun_target(&g_fx.smapn, nsize))
    {
        gfx_sun_fit(s, invV, L, 0.5f, tnear, nsize, &near);
        sun_draw(g_fx.smapn, NULL, invP, invV, &near, 1);
        memcpy(u->lmatn, near.lmat, 64);
        u->smapn[0] = near.texel, u->smapn[1] = near.bias, u->smapn[2] = near.soft, u->smapn[3] = near.slope;
        u->smapn2[0] = near.range, u->smapn2[1] = 1.0f;
    }
    /* the bounce light's map: this frame's casters over the first gi_distance units the camera sees, in
     * colour, 1024 across, and its levels below (gfx_d3d12.c's) */
    u->gi[0] = 0.0f;
    float gd = fminf(fmaxf(g_fxs.gi_distance, 8.0f), dfar);
    if (g_fxs.gi > 0.0f && sun_target(&g_fx.gimap, GI_MAP) && fx_tex(&g_fx.gicol, GI_FORMAT, GI_MAP, GI_MAP, GFX_USE_RT, 4))
    {
        SunCascade gk;
        float inv[16];
        gfx_sun_fit(s, invV, L, 0.5f, gd, GI_MAP, &gk);
        if (sun_draw(g_fx.gimap, g_fx.gicol, invP, invV, &gk, 0) && gfx_mat_inverse(inv, gk.lmat))
        {
            fx_uniforms(u); /* (the levels read none of them, but the pass binds them) */
            for (uint32_t lv = 1; lv < 4; ++lv)
            {
                VkImageView prev = level_view(g_fx.gicol, lv - 1);
                g_pass = GFX_PASS_SUN;
                fx_pass(g_fx.gicol, lv, 0, g_fx.mip_pipe, NULL, &prev, 1, NULL, NULL);
            }
            float r = fmaxf(g_fxs.gi_radius, 0.5f);
            memcpy(u->gimat, gk.lmat, 64), memcpy(u->giinv, inv, 64);
            u->gi[0] = 1.0f, u->gi[1] = r / gk.across, u->gi[2] = r;
            u->gi[3] = fminf(fmaxf(log2f(r / (4.0f * gk.texel)), 0.0f), 3.0f);
            u->gip[3] = gd;
        }
    }
    g_fx.st_across = far.across;
    g_fx.st_cached = g_ncache;
    return drawn != 0;
}

/* --- ray tracing (rt) -------------------------------------------------------------------------------------------
 * The world for rays (gfx_d3d12.c's, which says more; gfx_metal.m's the same way), made from the frame's
 * casters as the sun's maps are: each drawn once more through its own vertex function (the capture key,
 * GfxVsKey.shadow 2: rasterization off, the function run once a corner of its triangles, writing each
 * corner's point in this frame's view space into one buffer - gfx_msl.c emit_rt_index), a structure built
 * over the solid triangles and one over the alpha-tested, and one over both on top, every frame. The casters
 * out of view come from the sun's cache, through the camera they were drawn with. Rays are traced inline
 * (ray queries) from the scene effects' passes (RT_GLSL). Where the device cannot (g_has_rt), none are. */
#define RT_REACH 150.0f /* the cache's casters further than this from the camera are left out */
#define RT_TEXS 1024    /* the alpha tests' textures a frame (set 1 of the passes): those past it are traced solid */
#define RT_TEXS_S "1024"

/* Ray tracing's passes (gfx_rt.hlsl's, gfx_metal.m RT_MSL's, as GLSL ray queries): compiled with FX_GLSL
 * around them (rt_fx_pipeline puts this in its fragment part, FX_RT and FX_RT_<NAME> defined). Set 0 as
 * the passes' (FxU at 0, t0..t6) and the world: the structure at 10 (two instances, custom index 0 the
 * solid triangles, mask 1, and 1 the alpha-tested, mask 2), the corners at 11 (two vec4s each: the point,
 * then the first texture coordinates and the diffuse alpha), their smooth normals at 12, the alpha tests'
 * table at 13 (each its first triangle, its texture in set 1, stage 0's alpha op << 2 and its arguments
 * << 8 and << 16, its test: D3DCMPFUNC << 8 | reference | the texture factor's alpha << 16); set 1 the
 * alpha tests' textures, each with its own sampler. pc.rtp: the solid triangles' count, the alpha tests'. */
static const char RT_GLSL[] =
    "#ifdef FX_RT\n"
    "layout(set = 0, binding = 10) uniform accelerationStructureEXT world;\n"
    "layout(std430, set = 0, binding = 11) readonly buffer RtTri { vec4 tri[]; };\n"
    "layout(std430, set = 0, binding = 12) readonly buffer RtNrm { vec4 nrm[]; };\n"
    "layout(std430, set = 0, binding = 13) readonly buffer RtTab { uvec4 tab[]; };\n"
    "layout(set = 1, binding = 0) uniform sampler2D texs[" RT_TEXS_S "];\n"
    "vec3 rt_view_dir(vec2 px) { return normalize(view_pos(px, u.hand.x)); }\n"
    "vec3 rt_corner(uint prim, uint k) { return tri[(prim * 3u + k) * 2u].xyz; }\n"
    "vec3 rt_tri_normal(uint prim, vec3 dir) {\n"
    "  vec3 a = rt_corner(prim, 0u), b = rt_corner(prim, 1u), c = rt_corner(prim, 2u);\n"
    "  vec3 n = cross(b - a, c - a);\n"
    "  n = dot(n, n) > 1e-20 ? normalize(n) : -dir;\n"
    "  return dot(n, dir) > 0.0 ? -n : n;\n"
    "}\n"
    "vec3 rt_smooth_normal(uint prim, vec2 bary, vec3 geo) {\n"
    "  vec3 n = nrm[prim * 3u].xyz * (1.0 - bary.x - bary.y) + nrm[prim * 3u + 1u].xyz * bary.x + nrm[prim * 3u + 2u].xyz * bary.y;\n"
    "  if (dot(n, n) < 1e-8) return geo;\n"
    "  n = normalize(n);\n"
    "  return dot(n, geo) < 0.0 ? -n : n;\n"
    "}\n"
    /* does an alpha-tested triangle (prim, of all of them) let a ray through where it meets it (bary)? */
    "bool rt_alpha_holds(uint prim, vec2 bary) {\n"
    "  uint lo = 0u, hi = pc.rtp.y;\n"
    "  while (hi - lo > 1u) { uint mid = (lo + hi) / 2u; if (tab[mid].x <= prim) lo = mid; else hi = mid; }\n"
    "  uvec4 e = tab[lo];\n"
    "  vec3 uv = tri[prim * 6u + 1u].xyz * (1.0 - bary.x - bary.y) + tri[prim * 6u + 3u].xyz * bary.x + tri[prim * 6u + 5u].xyz * bary.y;\n"
    "  float ta = textureLod(texs[nonuniformEXT(e.y)], uv.xy, 0.0).a;\n"
    "  float tf = float((e.w >> 16) & 255u) / 255.0, ar[2];\n"
    "  for (int i = 0; i < 2; ++i) {\n"
    "    uint x = (e.z >> (8 + 8 * i)) & 255u, w = x & 15u;\n"
    "    float v = w == 2u ? ta : w == 3u ? tf : w <= 1u ? uv.z : 1.0;\n"
    "    ar[i] = (x & 16u) != 0u ? 1.0 - v : v;\n"
    "  }\n"
    "  uint op = (e.z >> 2) & 63u;\n"
    "  float a = op == 1u ? uv.z : op == 2u ? ar[0] : op == 3u ? ar[1] : op == 4u ? ar[0] * ar[1] : op == 5u ? ar[0] * ar[1] * 2.0 :\n"
    "    op == 6u ? ar[0] * ar[1] * 4.0 : op == 7u ? ar[0] + ar[1] : ta;\n"
    "  a = clamp(a, 0.0, 1.0) * 255.0;\n"
    "  float ref = float(e.w & 255u);\n"
    "  switch ((e.w >> 8) & 255u) {\n"
    "  case 1u: return false;\n"
    "  case 2u: return a < ref;\n"
    "  case 3u: return abs(a - ref) < 0.5;\n"
    "  case 4u: return a <= ref;\n"
    "  case 5u: return a > ref;\n"
    "  case 6u: return abs(a - ref) >= 0.5;\n"
    "  case 7u: return a >= ref;\n"
    "  default: return true;\n"
    "  }\n"
    "}\n"
    /* the nearest hit along a ray, t in [tmin, tmax]; prim the triangle (of all of them; -1 none), bary where */
    "float rt_trace(vec3 o, vec3 d, float tmin, float tmax, out int prim, out vec2 bary) {\n"
    "  rayQueryEXT q;\n"
    "  rayQueryInitializeEXT(q, world, gl_RayFlagsNoneEXT, 0xFFu, o, tmin, d, tmax);\n"
    "  while (rayQueryProceedEXT(q))\n"
    "    if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&\n"
    "        rt_alpha_holds(pc.rtp.x + uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)), rayQueryGetIntersectionBarycentricsEXT(q, false)))\n"
    "      rayQueryConfirmIntersectionEXT(q);\n"
    "  prim = -1, bary = vec2(0.0);\n"
    "  if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return tmax;\n"
    "  prim = rayQueryGetIntersectionPrimitiveIndexEXT(q, true) + (rayQueryGetIntersectionInstanceCustomIndexEXT(q, true) != 0 ? int(pc.rtp.x) : 0);\n"
    "  bary = rayQueryGetIntersectionBarycentricsEXT(q, true);\n"
    "  return rayQueryGetIntersectionTEXT(q, true);\n"
    "}\n"
    /* the nearest hit among the solid triangles alone (the bounce light's and occlusion's rays pass the leaves) */
    "float rt_trace_solid(vec3 o, vec3 d, float tmin, float tmax, out int prim) {\n"
    "  rayQueryEXT q;\n"
    "  rayQueryInitializeEXT(q, world, gl_RayFlagsOpaqueEXT, 0x01u, o, tmin, d, tmax);\n"
    "  while (rayQueryProceedEXT(q)) {}\n"
    "  prim = -1;\n"
    "  if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return tmax;\n"
    "  prim = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);\n"
    "  return rayQueryGetIntersectionTEXT(q, true);\n"
    "}\n"
    "bool rt_blocked(vec3 o, vec3 d, float tmin, float tmax) {\n"
    "  rayQueryEXT q;\n"
    "  rayQueryInitializeEXT(q, world, gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, o, tmin, d, tmax);\n"
    "  while (rayQueryProceedEXT(q))\n"
    "    if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&\n"
    "        rt_alpha_holds(pc.rtp.x + uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)), rayQueryGetIntersectionBarycentricsEXT(q, false)))\n"
    "      rayQueryConfirmIntersectionEXT(q);\n"
    "  return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT;\n"
    "}\n"
    "const uint RT_BAYER[16] = uint[16](0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u);\n"
    "vec2 rt_pattern(vec2 pos, int i) {\n"
    "  ivec2 cell = ivec2(pos) & 3;\n"
    "  float k = fract((float(RT_BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0 + u.hist.y + float(i) * 0.6180340);\n"
    "  uint h = (uint(pos.x) * 73856093u) ^ (uint(pos.y) * 19349663u);\n"
    "  float j = fract(float(h & 1023u) / 1024.0 + u.hist.y * 1.3247180 + float(i) * 0.7548777);\n"
    "  return vec2(k, j);\n"
    "}\n"
    "void rt_basis(vec3 N, out vec3 t, out vec3 s) {\n"
    "  t = abs(N.y) < 0.99 ? normalize(cross(N, vec3(0, 1, 0))) : normalize(cross(N, vec3(1, 0, 0)));\n"
    "  s = cross(N, t);\n"
    "}\n"
    "vec3 rt_hemi(vec3 N, float a, float b) {\n"
    "  vec3 t, s;\n"
    "  rt_basis(N, t, s);\n"
    "  float r = sqrt(a), phi = 6.2831853 * b;\n"
    "  return normalize(t * (r * cos(phi)) + s * (r * sin(phi)) + N * sqrt(max(1.0 - a, 0.0)));\n"
    "}\n"
    "vec3 rt_normal_at(vec2 px, vec3 P) {\n"
    "  vec3 r = pos_at(t0, px + vec2(1, 0)) - P, l = P - pos_at(t0, px - vec2(1, 0));\n"
    "  vec3 d = pos_at(t0, px + vec2(0, 1)) - P, t = P - pos_at(t0, px - vec2(0, 1));\n"
    "  vec3 dx = (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0 ? r : l;\n"
    "  vec3 dy = (abs(d.z) < abs(t.z) && dot(d, d) > 0.0) || dot(t, t) == 0.0 ? d : t;\n"
    "  vec3 nc = cross(dx, dy);\n"
    "  vec3 N = dot(nc, nc) > 1e-24 ? normalize(nc) : -normalize(P);\n"
    "  return dot(N, P) > 0.0 ? -N : N;\n"
    "}\n"
    "#ifdef FX_RT_CLAY\n"
    /* debug=clay (8): the world as the rays see it. The depth at t0. */
    "float rt_sun_seen(vec3 p, vec3 n, vec3 g, vec2 pos, int rays) {\n"
    "  float nl = dot(n, u.sun.xyz);\n"
    "  if (nl <= 0.0) return 0.0;\n"
    "  vec3 t, s;\n"
    "  rt_basis(u.sun.xyz, t, s);\n"
    "  vec3 o = p + g * 0.02 + n * 0.03;\n"
    "  float spread = 0.035, tmin = max(u.smap2.z, 0.05), lit = 0.0;\n"
    "  for (int i = 0; i < rays; ++i) {\n"
    "    vec2 rn = rt_pattern(pos, i + 3);\n"
    "    float rr = sqrt(rn.x) * spread, phi = 6.2831853 * rn.y;\n"
    "    vec3 d = normalize(u.sun.xyz + t * (rr * cos(phi)) + s * (rr * sin(phi)));\n"
    "    lit += rt_blocked(o, d, tmin, 500.0) ? 0.0 : 1.0;\n"
    "  }\n"
    "  return smoothstep(0.0, 0.2, nl) * lit / float(rays);\n"
    "}\n"
    "void main() {\n"
    "  vec2 px = floor(u.vp.xy + vuv * u.vp.zw) + 0.5;\n"
    "  float z = view_z(depth_at(t0, px));\n"
    "  float drawn = z != 0.0 ? length(view_pos(px, z)) : 0.0;\n"
    "  vec3 d = rt_view_dir(px);\n"
    "  int prim;\n"
    "  vec2 bary;\n"
    "  float t = rt_trace(vec3(0.0), d, 0.05, 2000.0, prim, bary);\n"
    "  if (prim < 0) { oc = drawn > 0.0 ? vec4(0.1, 0.2, 0.9, 1) : vec4(0.35, 0.45, 0.6, 1); return; }\n"
    "  vec3 g = rt_tri_normal(uint(prim), d), n = rt_smooth_normal(uint(prim), bary, g), p = d * t;\n"
    "  float sky = 0.25 + 0.15 * dot(n, u.up.xyz);\n"
    "  float lit = sky + (u.sun.w > 0.0 ? 0.7 * clamp(dot(n, u.sun.xyz), 0.0, 1.0) * rt_sun_seen(p, n, g, gl_FragCoord.xy, 8) : 0.0);\n"
    "  vec3 c = vec3(lit);\n"
    "  if (drawn <= 0.0) c *= vec3(1.0, 0.85, 0.2);\n"
    "  else if (abs(t - drawn) > max(0.05 * drawn, 0.3)) c *= t < drawn ? vec3(1.0, 0.3, 0.3) : vec3(0.3, 0.4, 1.0);\n"
    "  oc = vec4(c, 1);\n"
    "}\n"
    "#endif\n"
    "#ifdef FX_RT_GI\n"
    /* the bounce light traced. t0 the depth, t1 the scene as drawn, t2 the occlusion and sun (z), t3 and t4
     * the bounce map's depth and colour */
    "vec3 rt_radiance(vec3 Q) {\n"
    "  float qd = Q.z * u.hand.x;\n"
    "  if (qd > 0.05) {\n"
    "    vec2 ndc = vec2(Q.x * u.proj.x + Q.z * u.proj.z, Q.y * u.proj.y + Q.z * u.proj.w) / qd;\n"
    "    vec2 q = u.vp.xy + vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;\n"
    "    if (all(greaterThanEqual(q, u.vp.xy)) && all(lessThan(q, u.vp.xy + u.vp.zw - 1.0))) {\n"
    "      float sd = view_z(depth_at(t0, q + 0.5)) * u.hand.x;\n"
    "      if (sd > 0.0 && abs(sd - qd) < 0.03 * qd + 0.1) {\n"
    "        vec3 c = texelFetch(t1, ivec2(q + 0.5), 0).rgb;\n"
    "        float sun = textureLod(t2, (q - u.vp.xy) / u.vp.zw, 0.0).z;\n"
    "        return c * mix(1.0, sun, u.smap.x);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  if (u.gi.x > 0.0) {\n"
    "    vec4 lc = u.gimat * vec4(Q, 1.0);\n"
    "    if (all(lessThan(abs(lc.xy), vec2(0.98))) && lc.z < 1.0) {\n"
    "      vec2 uv = vec2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);\n"
    "      float z = texelFetch(t3, ivec2(uv * vec2(textureSize(t3, 0))), 0).r;\n"
    "      if (lc.z - z < 0.002) return textureLod(t4, uv, 0.0).rgb;\n"
    "    }\n"
    "  }\n"
    "  return vec3(0.0);\n"
    "}\n"
    "void main() {\n"
    "  vec2 px = floor(u.vp.xy + vuv * u.vp.zw) + 0.5;\n"
    "  vec3 P = pos_at(t0, px);\n"
    "  float dist = P.z * u.hand.x;\n"
    "  if (dist <= 0.0) { oc = vec4(0.0); return; }\n"
    "  float fade = 1.0 - smoothstep(0.7 * u.gip.w, u.gip.w, dist);\n"
    "  if (fade <= 0.0) { oc = vec4(0.0, 0.0, 0.0, dist); return; }\n"
    "  vec3 N = rt_normal_at(px, P);\n"
    "  vec3 o = P + N * (0.02 + 0.002 * dist);\n"
    "  int NS = max(int(u.gip.z), 1);\n"
    "  vec3 sum = vec3(0.0);\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    vec2 rn = rt_pattern(gl_FragCoord.xy, i);\n"
    "    vec3 d = rt_hemi(N, rn.x, rn.y);\n"
    "    int prim;\n"
    "    float t = rt_trace_solid(o, d, 0.0, u.gi.z, prim);\n"
    "    if (prim < 0) continue;\n"
    "    vec3 Q = o + d * t, nq = rt_tri_normal(uint(prim), d);\n"
    "    sum += rt_radiance(Q + nq * 0.02);\n"
    "  }\n"
    "  oc = vec4(sum * (u.gi.x * fade / float(NS)), dist);\n"
    "}\n"
    "#endif\n"
    "#ifdef FX_RT_AO\n"
    /* the occlusion traced: fx_ao's result (t0) with x traced; the depth at t1 */
    "void main() {\n"
    "  vec4 c = texelFetch(t0, ivec2(gl_FragCoord.xy), 0);\n"
    "  if (c.y <= 0.0) { oc = c; return; }\n"
    "  vec2 px = floor(u.vp.xy + vuv * u.vp.zw) + 0.5;\n"
    "  vec3 P = pos_at(t1, px);\n"
    "  vec3 r = pos_at(t1, px + vec2(1, 0)) - P, l = P - pos_at(t1, px - vec2(1, 0));\n"
    "  vec3 d = pos_at(t1, px + vec2(0, 1)) - P, t = P - pos_at(t1, px - vec2(0, 1));\n"
    "  vec3 dx = (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0 ? r : l;\n"
    "  vec3 dy = (abs(d.z) < abs(t.z) && dot(d, d) > 0.0) || dot(t, t) == 0.0 ? d : t;\n"
    "  vec3 nc = cross(dx, dy);\n"
    "  vec3 N = dot(nc, nc) > 1e-24 ? normalize(nc) : -normalize(P);\n"
    "  if (dot(N, P) > 0.0) N = -N;\n"
    "  vec3 o = P + N * (0.01 + 0.002 * c.y);\n"
    "  float R = u.ao.x, occl = 0.0;\n"
    "  const int NS = 6;\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    vec2 rn = rt_pattern(gl_FragCoord.xy, i + 7);\n"
    "    vec3 dd = rt_hemi(N, rn.x, rn.y);\n"
    "    int prim;\n"
    "    float th = rt_trace_solid(o, dd, 0.0, R, prim);\n"
    "    if (prim >= 0) { float f = 1.0 - th / R; occl += f * f; }\n"
    "  }\n"
    "  c.x = clamp(1.0 - occl / float(NS), 0.0, 1.0);\n"
    "  oc = c;\n"
    "}\n"
    "#endif\n"
    "#endif\n"
    "#ifdef FX_RT_GIBLUR\n"
    /* the bounce light smoothed: a 5x5 at the step pc.dir, by how near each distance is (needs no world) */
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy), hi = textureSize(t0, 0) - 1;\n"
    "  vec4 c = texelFetch(t0, p, 0);\n"
    "  if (c.a <= 0.0) { oc = c; return; }\n"
    "  const float K[3] = float[3](0.375, 0.25, 0.0625);\n"
    "  vec3 s = vec3(0.0);\n"
    "  float sw = 0.0;\n"
    "  for (int y = -2; y <= 2; ++y)\n"
    "    for (int x = -2; x <= 2; ++x) {\n"
    "      vec4 t = texelFetch(t0, clamp(p + ivec2(x, y) * pc.dir, ivec2(0), hi), 0);\n"
    "      float k = K[abs(x)] * K[abs(y)] * (t.a > 0.0 ? clamp(1.0 - abs(t.a - c.a) / (0.04 * c.a), 0.0, 1.0) : 0.0);\n"
    "      s += t.rgb * k, sw += k;\n"
    "    }\n"
    "  oc = vec4(sw > 0.0 ? s / sw : c.rgb, c.a);\n"
    "}\n"
    "#endif\n";

/* The world's smooth normals (gfx_rt.hlsl rt_nclear, rt_nsum, rt_nresolve), compute: the corners that share
 * a place (to 1/256 of a unit) found through a hash table, each place summing its faces' normals (in
 * 1/4096ths), each corner taking its place's average unless it turns more than 60 degrees from its own face.
 * Bindings (pushed): the table 0 (four words a slot: its tag, the sum), the corners 1, the normals 2;
 * NC (push constants): the triangles' count, the table's mask (its slots less one), the corners' count.
 * RT_NCLEAR, RT_NSUM or RT_NRESOLVE picks the pass. */
static const char RT_NORMALS_GLSL[] =
    "layout(local_size_x = 256) in;\n"
    "layout(push_constant) uniform NC { uint tris, mask, verts, pad; } n;\n"
    "layout(std430, set = 0, binding = 0) buffer Table { uint table[]; };\n"
    "layout(std430, set = 0, binding = 1) readonly buffer Tri { vec4 tri[]; };\n"
    "layout(std430, set = 0, binding = 2) writeonly buffer Nrm { vec4 nrm[]; };\n"
    "uvec3 rt_key(vec3 p) { return uvec3(ivec3(floor(p * 256.0 + 0.5))); }\n"
    "uint rt_hash(uvec3 k) { return (k.x * 73856093u) ^ (k.y * 19349663u) ^ (k.z * 83492791u); }\n"
    "uint rt_tag(uvec3 k) { return ((k.x * 2654435761u) ^ (k.y * 2246822519u) ^ (k.z * 3266489917u)) | 1u; }\n"
    "uint rt_slot(vec3 p, bool make) {\n"
    "  uvec3 k = rt_key(p);\n"
    "  uint h = rt_hash(k) & n.mask, tag = rt_tag(k);\n"
    "  for (uint i = 0u; i < 64u; ++i) {\n"
    "    uint slot = (h + i) & n.mask;\n"
    "    uint was = make ? atomicCompSwap(table[slot * 4u], 0u, tag) : table[slot * 4u];\n"
    "    if (was == tag || (make && was == 0u)) return slot;\n"
    "    if (!make && was == 0u) break;\n"
    "  }\n"
    "  return n.mask + 1u;\n"
    "}\n"
    "vec3 rt_corner(uint t, uint k) { return tri[(t * 3u + k) * 2u].xyz; }\n"
    "void main() {\n"
    "  uint id = gl_GlobalInvocationID.x;\n"
    "#if defined(RT_NCLEAR)\n"
    "  if (id <= n.mask) table[id * 4u] = table[id * 4u + 1u] = table[id * 4u + 2u] = table[id * 4u + 3u] = 0u;\n"
    "#elif defined(RT_NSUM)\n"
    "  if (id >= n.tris) return;\n"
    "  vec3 v[3] = vec3[3](rt_corner(id, 0u), rt_corner(id, 1u), rt_corner(id, 2u));\n"
    "  vec3 f = cross(v[1] - v[0], v[2] - v[0]);\n"
    "  if (!(dot(f, f) > 1e-14)) return;\n"
    "  ivec3 q = ivec3(normalize(f) * 4096.0);\n"
    "  for (int k = 0; k < 3; ++k) {\n"
    "    uint slot = rt_slot(v[k], true);\n"
    "    if (slot > n.mask) continue;\n"
    "    atomicAdd(table[slot * 4u + 1u], uint(q.x));\n"
    "    atomicAdd(table[slot * 4u + 2u], uint(q.y));\n"
    "    atomicAdd(table[slot * 4u + 3u], uint(q.z));\n"
    "  }\n"
    "#else\n"
    "  if (id >= n.verts) return;\n"
    "  uint t0 = id - id % 3u;\n"
    "  vec3 a = tri[t0 * 2u].xyz, b = tri[(t0 + 1u) * 2u].xyz, c = tri[(t0 + 2u) * 2u].xyz;\n"
    "  vec3 f = cross(b - a, c - a);\n"
    "  if (!(dot(f, f) > 1e-14)) { nrm[id] = vec4(0.0); return; }\n"
    "  f = normalize(f);\n"
    "  vec3 nn = f;\n"
    "  uint slot = rt_slot(tri[id * 2u].xyz, false);\n"
    "  if (slot <= n.mask) {\n"
    "    vec3 sum = vec3(ivec3(int(table[slot * 4u + 1u]), int(table[slot * 4u + 2u]), int(table[slot * 4u + 3u])));\n"
    "    if (dot(sum, sum) > 1.0) { sum = normalize(sum); nn = dot(sum, f) > 0.5 ? sum : f; }\n"
    "  }\n"
    "  nrm[id] = vec4(nn, 0.0);\n"
    "#endif\n"
    "}\n";


typedef struct RtBuf
{
    VkBuffer b;
    VmaAllocation m;
    VkDeviceSize size;
    VkDeviceAddress addr;
    void* p; /* host-visible ones' */
} RtBuf;

static struct
{
    int tried, ok;
    VkPipeline gi, ao, giblur, clay;
    VkFormat clay_fmt;
    VkPipeline nclear, nsum, nresolve;
    VkDescriptorSetLayout dsl, tex_dsl, cdsl;
    VkPipelineLayout layout, clayout;
    VkDescriptorPool pool[FRAMES]; /* the alpha tests' textures' sets, a frame's */
    uint64_t pool_serial[FRAMES];
    VkDescriptorSet texset; /* this frame's */
    RtBuf tri, nrm, table, scratch, tlas_buf, blas_buf[2], inst[FRAMES], tab[FRAMES];
    VkAccelerationStructureKHR tlas, blas[2]; /* (blas[1]: the alpha-tested) */
    uint32_t rtp[4];                         /* RT_GLSL pc.rtp: the solid triangles, the alpha tests */
    uint32_t verts;
    VkDeviceSize scratch_align;
    /* the profile: casters captured, from the cache, skipped (no pipeline yet), triangles */
    uint32_t st_live, st_cached, st_skipped, st_frames;
    uint64_t st_tris;
} g_ray;

/* one of RT_GLSL's passes (FX_RT_<name>) into fmt */
static VkPipeline rt_fx_pipeline(const char* name, VkFormat fmt)
{
    size_t fl = strlen(FX_GLSL), rl = strlen(RT_GLSL), n = fl + rl + 256;
    char* src = (char*)malloc(n);
    /* RT_GLSL goes inside FX_GLSL's fragment part: before its last #endif */
    int at = (int)(fl - strlen("#endif\n"));
    snprintf(src, n, "#extension GL_EXT_ray_query : require\n#extension GL_EXT_nonuniform_qualifier : require\n"
                     "#define FX_RT\n#define FX_RT_%s\n%.*s%s#endif\n", name, at, FX_GLSL, RT_GLSL);
    VkShaderModule fs = compile_glsl(src, 1);
    free(src);
    VkPipeline p = VK_NULL_HANDLE;
    if (fs && g_fx.vs)
    {
        VkPipelineColorBlendAttachmentState b = { 0 };
        b.colorWriteMask = 0xF;
        static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        p = make_pipeline(g_fx.vs, fs, g_ray.layout, &b, fmt, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_POLYGON_MODE_FILL, dyn, 2, 0);
    }
    if (fs)
        vkDestroyShaderModule(g_dev, fs, NULL);
    if (!p)
        fprintf(stderr, "[recomp] gfx: ray tracing pass %s failed\n", name);
    return p;
}

static VkPipeline rt_compute(const char* pass)
{
    char d[64];
    snprintf(d, sizeof d, "#define %s\n", pass);
    return compute_pipeline(d, RT_NORMALS_GLSL, g_ray.clayout);
}

/* whether rays can be traced here (asking once; after fx_init) */
static int rt_init(void)
{
    if (g_ray.tried)
        return g_ray.ok;
    g_ray.tried = 1;
    if (!g_has_rt || !fx_init())
    {
        fprintf(stderr, "[recomp] gfx: ray tracing: not on this device\n");
        return 0;
    }
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &asp;
    vkGetPhysicalDeviceProperties2(g_phys, &p2);
    g_ray.scratch_align = asp.minAccelerationStructureScratchOffsetAlignment ? asp.minAccelerationStructureScratchOffsetAlignment : 256;
    /* the passes': FX_GLSL's ten (fx_init), and the world (10-13), pushed; the textures (set 1) */
    VkDescriptorSetLayoutBinding b[14];
    b[0] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (uint32_t i = 1; i < 10; ++i)
        b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[10] = (VkDescriptorSetLayoutBinding){ 10, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (uint32_t i = 11; i < 14; ++i)
        b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 14;
    dl.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, &g_ray.dsl));
    VkDescriptorSetLayoutBinding tb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, RT_TEXS, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorBindingFlags tf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo tfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    tfi.bindingCount = 1, tfi.pBindingFlags = &tf;
    VkDescriptorSetLayoutCreateInfo tl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    tl.pNext = &tfi;
    tl.bindingCount = 1, tl.pBindings = &tb;
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &tl, NULL, &g_ray.tex_dsl));
    VkDescriptorSetLayout sets[2] = { g_ray.dsl, g_ray.tex_dsl };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 2, pl.pSetLayouts = sets;
    pl.pushConstantRangeCount = 1, pl.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &pl, NULL, &g_ray.layout));
    for (int i = 0; i < FRAMES; ++i)
    {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, RT_TEXS * 4 };
        VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets = 4, pi.poolSizeCount = 1, pi.pPoolSizes = &ps;
        VK_CHECK(vkCreateDescriptorPool(g_dev, &pi, NULL, &g_ray.pool[i]));
    }
    /* the normals' compute passes: three storage buffers, pushed; NC as push constants */
    VkDescriptorSetLayoutBinding cb[3];
    for (uint32_t i = 0; i < 3; ++i)
        cb[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    dl.bindingCount = 3, dl.pBindings = cb;
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, &g_ray.cdsl));
    VkPushConstantRange cpr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
    pl.setLayoutCount = 1, pl.pSetLayouts = &g_ray.cdsl;
    pl.pPushConstantRanges = &cpr;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &pl, NULL, &g_ray.clayout));
    const VkFormat F16 = VK_FORMAT_R16G16B16A16_SFLOAT;
    g_ray.gi = rt_fx_pipeline("GI", F16);
    g_ray.ao = rt_fx_pipeline("AO", F16);
    g_ray.giblur = rt_fx_pipeline("GIBLUR", F16);
    g_ray.nclear = rt_compute("RT_NCLEAR");
    g_ray.nsum = rt_compute("RT_NSUM");
    g_ray.nresolve = rt_compute("RT_NRESOLVE");
    g_ray.ok = g_ray.gi && g_ray.ao && g_ray.giblur && g_ray.nclear && g_ray.nsum && g_ray.nresolve;
    fprintf(stderr, g_ray.ok ? "[recomp] gfx: ray tracing ready\n" : "[recomp] gfx: ray tracing: its pipelines failed\n");
    return g_ray.ok;
}

int gfx_rt_supported(void) { return rt_init(); }

/* the soft shadows' disc (the sun's and moon's, SHADOW_GLSL, and the lamps', LAMP_GLSL): sixteen points on a Vogel
 * spiral, n of them turned by r; sh_turn, a turn from a hash of the place, so neighbouring pixels' taps differ */
#define SHDISC_GLSL                                                                                                   \
    "#ifdef GFX_FS\n"                                                                                                 \
    "vec2 sh_turn(vec3 p) {\n"                                                                                        \
    "  float a = 6.2831853 * fract(52.9829189 * fract(dot(floor(p * 40.0), vec3(0.06711056, 0.00583715, 0.1234567))));\n" \
    "  return vec2(cos(a), sin(a));\n"                                                                                \
    "}\n"                                                                                                             \
    "const vec2 sh_disc[16] = vec2[16](vec2(0.1768, 0.0000), vec2(-0.2258, 0.2068), vec2(0.0346, -0.3938), vec2(0.2846, 0.3712),\n" \
    "  vec2(-0.5222, -0.0924), vec2(0.4947, -0.3147), vec2(-0.1655, 0.6155), vec2(-0.3156, -0.6076),\n"               \
    "  vec2(0.6846, 0.2500), vec2(-0.7123, 0.2940), vec2(0.3434, -0.7337), vec2(0.2537, 0.8089),\n"                   \
    "  vec2(-0.7647, -0.4432), vec2(0.8971, -0.1972), vec2(-0.5475, 0.7788), vec2(-0.1265, -0.9761));\n"              \
    "vec2 sh_vogel(int i, int n, vec2 r) {\n"                                                                         \
    "  vec2 q = sh_disc[i * (16 / n) + (16 / n - 1)];\n"                                                              \
    "  return vec2(q.x * r.x - q.y * r.y, q.x * r.y + q.y * r.x);\n"                                                  \
    "}\n"                                                                                                             \
    "#endif\n"

/* --- the scene renderer's sun and moon shadows, the receivers' side (gfx_metal.m's SHADOW_MSL) ----------------------
 * SH is ShU: the cascades' matrices and ranges, the characters' layers (last frame's), the light, the camera, the
 * filters. The maps are depth arrays a cascade a layer, compared through cs (LESS_OR_EQUAL, linear) and gathered
 * through ps (nearest); their rows run down from the map's +y, as the casters' viewport draws them. */
#define SHADOW_GLSL                                                                                                   \
    "layout(std140, set = 0, binding = 6) uniform SHB { mat4 m[4]; vec4 cas[4]; vec4 cbox[4]; vec4 dir; vec4 cam; vec4 info;\n" \
    "  mat4 cm[4]; vec4 ctx; vec4 pk; } S;\n"                                                                        \
    "#ifdef GFX_FS\n"                                                                                                 \
    "layout(set = 0, binding = 7) uniform texture2DArray zm;\n"                                                       \
    "layout(set = 0, binding = 8) uniform texture2DArray cm;\n"                                                       \
    "layout(set = 0, binding = 9) uniform texture2DArray ct;\n"                                                       \
    "layout(set = 0, binding = 10) uniform samplerShadow cs;\n"                                                       \
    "layout(set = 0, binding = 11) uniform sampler ps;\n"                                                             \
    "float sh_cmp(texture2DArray t, vec2 uv, uint l, float z) { return texture(sampler2DArrayShadow(t, cs), vec4(uv, float(l), z)); }\n" \
    "float sh_pcf(texture2DArray t, vec2 uv, uint c, float z, float tx) {\n"                                         \
    "  if (c >= 2u) return sh_cmp(t, uv, c, z);\n" /* far: the hardware's 2x2 alone */                                \
    "  return 0.25 * (sh_cmp(t, uv + vec2(-0.5, -0.5) * tx, c, z) + sh_cmp(t, uv + vec2(0.5, -0.5) * tx, c, z) +\n"  \
    "                 sh_cmp(t, uv + vec2(-0.5, 0.5) * tx, c, z) + sh_cmp(t, uv + vec2(0.5, 0.5) * tx, c, z));\n"     \
    "}\n"                                                                                                             \
    /* the characters' layers: nine of the hardware's 2x2 compares a texel apart (a 4x4 footprint, smooth edges) */   \
    "float sh_cpcf(texture2DArray t, vec2 uv, uint l, float z, float tx) {\n"                                        \
    "  float v = 0.0;\n"                                                                                              \
    "  for (int y = -1; y <= 1; ++y)\n"                                                                               \
    "    for (int x = -1; x <= 1; ++x) v += sh_cmp(t, uv + vec2(x, y) * tx, l, z);\n"                                 \
    "  return v * (1.0 / 9.0);\n"                                                                                     \
    "}\n"                                                                                                             \
    /* contact-hardening soft shadows (shadow_softness), as gfx_metal.m's sh_pcss: the blockers' mean depth round the \
     * point (gathers on a disc turned by SHDISC_GLSL's hash of the place), then that far from them times the light's \
     * size wide */                                                                                                   \
    "float sh_pcss(texture2DArray t, mat4 M, uint l, vec2 uv, float z, float tx, float tl, int nb, int nf, vec2 rot, float nl) {\n" \
    "  float wpt = 2.0 * tx / length(vec3(M[0][0], M[1][0], M[2][0]));\n"                                            \
    "  float wpd = 1.0 / length(vec3(M[0][2], M[1][2], M[2][2]));\n"                                                 \
    "  float rs = clamp(tl * 6.0 / wpt, 1.0, 12.0);\n"                                                                \
    "  float sl = sqrt(1.0 - nl * nl) / max(nl, 0.2), zs = z - min(rs * wpt * sl, 1.0) / wpd;\n"                      \
    "  float sum = 0.0, n = 0.0;\n"                                                                                   \
    "  for (int i = 0; i < nb; ++i) {\n"                                                                              \
    "    vec4 d = textureGather(sampler2DArray(t, ps), vec3(uv + sh_vogel(i, 4, rot) * (rs * tx), float(l)));\n"      \
    "    vec4 b = vec4(lessThan(d, vec4(zs)));\n"                                                                     \
    "    sum += dot(d, b), n += b.x + b.y + b.z + b.w;\n"                                                             \
    "  }\n"                                                                                                           \
    "  if (n == 0.0) return 1.0;\n"                                                                                   \
    "  float pen = clamp((z - sum / n) * wpd * tl / wpt, 0.5, 12.0);\n"                                               \
    "  float zb = z - min(pen * wpt * sl, 0.5) / wpd;\n"                                                              \
    "  if (n == 4.0 * float(nb) && pen < 1.5) return sh_cmp(t, uv, l, zb);\n"                                         \
    "  float v = 0.0;\n"                                                                                              \
    "  for (int i = 0; i < nf; ++i) v += sh_cmp(t, uv + sh_vogel(i, nf, rot) * (pen * tx), l, zb);\n"                 \
    "  return v / float(nf);\n"                                                                                       \
    "}\n"                                                                                                             \
    "float sh_char(uint c, vec3 wp, vec3 n, float nl, bool soft) {\n"                                                 \
    "  float off = 1.0 + 4.0 * sqrt(1.0 - nl * nl);\n"                                                                \
    "  float t = S.ctx.x;\n" /* the fitted layer, when there is one and the point is in it */                         \
    "  if (t > 0.0) {\n"                                                                                              \
    "    vec4 h = S.cm[0] * vec4(wp + n * (t * off) + S.dir.xyz * t, 1.0);\n"                                         \
    "    vec2 u0 = vec2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5);\n"                                                         \
    "    if (all(lessThan(abs(h.xy), vec2(0.995))))\n"                                                                \
    "      return soft && S.pk.x > 0.0 ? sh_pcss(ct, S.cm[0], 0u, u0, h.z, S.ctx.y, S.pk.x, max(int(S.pk.y) / 2, 1), int(S.pk.z) / 2, sh_turn(wp), nl)\n" \
    "                                  : sh_cpcf(ct, u0, 0u, h.z, S.ctx.y * (soft ? max(S.ctx.z, 1.0) : 1.0));\n"    \
    "  }\n"                                                                                                           \
    "  if (c >= uint(S.info.y)) return 1.0;\n" /* the cascade's characters' layer, half size, only round them */       \
    "  t = 2.0 * S.cas[c].y;\n"                                                                                       \
    "  vec4 h = S.cm[1u + c] * vec4(wp + n * (t * off) + S.dir.xyz * t, 1.0);\n"                                      \
    "  vec4 b = S.cbox[c];\n"                                                                                         \
    "  if (any(greaterThan(abs(h.xy), vec2(0.995))) || any(lessThan(h.xy, b.xy)) || any(greaterThan(h.xy, b.zw))) return 1.0;\n" \
    "  vec2 u1 = vec2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5);\n"                                                           \
    "  if (soft && S.pk.x > 0.0) return sh_pcss(cm, S.cm[1u + c], c, u1, h.z, 2.0 * S.info.w, S.pk.x, max(int(S.pk.y) / 2, 1), int(S.pk.z) / 2, sh_turn(wp), nl);\n" \
    "  return sh_cpcf(cm, u1, c, h.z, 2.0 * S.info.w * (soft ? max(S.ctx.z, 1.0) : 1.0));\n"                         \
    "}\n"                                                                                                             \
    "float sh_at(uint c, vec3 wp, vec3 n, bool soft) {\n"                                                             \
    "  float nl = sat(dot(n, S.dir.xyz)), t = S.cas[c].y;\n" /* more the more the surface turns from it */            \
    "  vec4 h = S.m[c] * vec4(wp + n * (t * (1.0 + 4.0 * sqrt(1.0 - nl * nl))) + S.dir.xyz * t, 1.0);\n"              \
    "  if (abs(h.x) > 0.995 || abs(h.y) > 0.995) return -1.0;\n"                                                      \
    "  vec2 uv = vec2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5);\n"                                                           \
    "  float v;\n"                                                                                                    \
    "  if (soft && S.pk.x > 0.0) {\n" /* soft: all the taps in the nearest cascade, half in the next, a quarter beyond */ \
    "    int k = c == 0u ? 1 : c == 1u ? 2 : 4;\n"                                                                    \
    "    v = sh_pcss(zm, S.m[c], c, uv, h.z, S.info.w, S.pk.x, max(int(S.pk.y) / k, 1), max(int(S.pk.z) / k, 4), sh_turn(wp), nl);\n" \
    "  } else\n"                                                                                                      \
    "    v = soft && S.ctx.z > 1.01 && c < 2u ? sh_cpcf(zm, uv, c, h.z, S.info.w * S.ctx.z) : sh_pcf(zm, uv, c, h.z, S.info.w);\n" /* the moon's wider */ \
    "  if (v > 0.0 && S.info.y > 0.0) v = min(v, sh_char(c, wp, n, nl, soft));\n"                                     \
    "  return v;\n"                                                                                                   \
    "}\n"                                                                                                             \
    "float shadow(vec3 wp, vec3 n, bool soft) {\n" /* soft: PCSS (the zone); a character's own the fixed filter */    \
    "  float d = length(wp - S.cam.xyz);\n"                                                                           \
    "  uint nc = uint(S.info.x);\n"                                                                                   \
    "  for (uint c = 0u; c < nc; ++c) {\n"                                                                            \
    "    if (S.cas[c].w == 0.0 || d > S.cas[c].x) continue;\n"                                                        \
    "    float v = sh_at(c, wp, n, soft);\n"                                                                          \
    "    if (v < 0.0) continue;\n"                                                                                    \
    "    float r = S.cas[c].x, f = sat((d - 0.9 * r) / (0.1 * r));\n"                                                 \
    "    if (f > 0.0) {\n"                                                                                            \
    "      float w = c + 1u < nc && S.cas[c + 1u].w != 0.0 ? sh_at(c + 1u, wp, n, soft) : 1.0;\n"                     \
    "      v = mix(v, w < 0.0 ? 1.0 : w, f);\n"                                                                       \
    "    }\n"                                                                                                         \
    "    return mix(1.0, v, S.info.z);\n"                                                                             \
    "  }\n"                                                                                                           \
    "  return 1.0;\n"                                                                                                 \
    "}\n"                                                                                                             \
    "#endif\n"

/* a character's vertex in the game's skinned space (CHARS_GLSL, CAST_GLSL): two bones, or their mirror bones mirrored;
 * GfxCharVertex is 18 words */
#define SKIN_GLSL                                                                                                     \
    "layout(std430, set = 0, binding = 3) readonly buffer VB { uint vb[]; };\n"                                       \
    "layout(std430, set = 0, binding = 5) readonly buffer UB { vec2 uvs[]; };\n"                                      \
    "const vec3 SIGN[4] = vec3[4](vec3(1.0), vec3(-1.0, 1.0, 1.0), vec3(1.0, -1.0, 1.0), vec3(1.0, 1.0, -1.0));\n"    \
    "vec4 word4(uint b) { return uintBitsToFloat(uvec4(vb[b], vb[b + 1u], vb[b + 2u], vb[b + 3u])); }\n"              \
    "vec3 skin(out vec3 n) {\n"                                                                                       \
    "  uint b = uint(gl_VertexIndex) * 18u;\n"                                                                        \
    "  vec4 va = word4(b), vb4 = word4(b + 4u);\n"                                                                    \
    "  vec3 na = word4(b + 8u).xyz, nb = word4(b + 12u).xyz;\n"                                                       \
    "  uint bones = vb[b + 16u], mir = vb[b + 17u];\n"                                                                \
    "  uint ba = bones & 0xffu, bb = (bones >> 8) & 0xffu;\n"                                                         \
    "  if (D.mirror != 0u) {\n"                                                                                       \
    "    ba = (bones >> 16) & 0xffu, bb = bones >> 24;\n"                                                             \
    "    vec3 sa = SIGN[mir & 3u], sb = SIGN[(mir >> 2) & 3u];\n"                                                     \
    "    va.xyz *= sa; vb4.xyz *= sb; na *= sa; nb *= sb;\n"                                                          \
    "  }\n"                                                                                                           \
    "  mat4 MA = P[D.palette + ba], MB = P[D.palette + bb];\n"                                                        \
    "  n = (MA * vec4(na, 0.0)).xyz + (MB * vec4(nb, 0.0)).xyz;\n"                                                    \
    "  return (MA * va + MB * vb4).xyz;\n"                                                                            \
    "}\n"

/* the scene renderer's occlusion, the shading's side (gfx_ao_msl.h's AO_MSL; how it is made: SAO_GLSL): AU the last
 * result's view and projection (pvp), its viewport's size and the share of the texture it fills (vp), the zone's
 * strength, the characters', debug and the depth test's tolerance (k). ao_at: a bilinear gather of the half-size result
 * (x the occlusion, y the distance it was made at), each tap weighted by how near its distance is to the point's; 1
 * where nothing matches */
#define AO_GLSL                                                                                                       \
    "layout(std140, set = 0, binding = 12) uniform AUB { mat4 pvp; vec4 vp; vec4 k; } A;\n"                           \
    "#ifdef GFX_FS\n"                                                                                                 \
    "layout(set = 0, binding = 13) uniform sampler2D aot;\n"                                                          \
    "float ao_at(vec4 pc) {\n"                                                                                        \
    "  if (pc.w <= 1e-4) return 1.0;\n"                                                                               \
    "  vec2 uv = vec2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"                                          \
    "  if (any(lessThanEqual(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return 1.0;\n"                  \
    "  vec2 tc = uv * A.vp.zw;\n"                                                                                     \
    "  vec2 sz = vec2(textureSize(aot, 0)), t = tc * sz - 0.5, f = fract(t);\n"                                       \
    "  vec2 g = (floor(t) + 1.0) / sz;\n"                                                                             \
    "  vec4 o = textureGather(aot, g, 0), d = textureGather(aot, g, 1);\n"                                            \
    /* gather's order: (0,1) (1,1) (1,0) (0,0) */                                                                     \
    "  vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));\n"                 \
    "  w *= sat(1.0 - abs(d - pc.w) / (A.k.w * pc.w));\n"                                                             \
    "  float ws = w.x + w.y + w.z + w.w;\n"                                                                           \
    "  return ws > 1e-3 ? dot(o, w) / ws : 1.0;\n"                                                                    \
    "}\n"                                                                                                             \
    "#endif\n"

/* the highlights (scene_spec, gfx_metal.m's SPEC_MSL and its notes): GGX with Schlick's Fresnel and Smith-Schlick
 * visibility; the roughness widened by how fast the normal turns across the pixel. SP: k (strength, the lamps' share,
 * wetness, anti-aliasing), pad.x 1 on an HDR target */
#define SPEC_GLSL                                                                                                     \
    "layout(std140, set = 0, binding = 14) uniform SPB { vec4 k; vec4 pad; } SPc;\n"                                  \
    "#ifdef GFX_FS\n"                                                                                                 \
    "float spec_ggx(vec3 N, vec3 V, vec3 L, float a2, float f0) {\n"                                                  \
    "  vec3 H = normalize(V + L);\n"                                                                                  \
    "  float nl = sat(dot(N, L)), nv = max(dot(N, V), 1e-3), nh = sat(dot(N, H)), vh = sat(dot(V, H));\n"             \
    "  float d = nh * nh * (a2 - 1.0) + 1.0;\n"                                                                       \
    "  float D = a2 / (3.14159265 * d * d);\n"                                                                        \
    "  float k = 0.5 * sqrt(a2);\n"                                                                                   \
    "  float vis = 0.25 / ((nl * (1.0 - k) + k) * (nv * (1.0 - k) + k));\n"                                           \
    "  float F = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);\n"                                                             \
    "  return min(D * vis * F * nl, 16.0);\n"                                                                         \
    "}\n"                                                                                                             \
    "float spec_a2(float rough, vec3 N, float aa) {\n"                                                                \
    "  vec3 dx = dFdx(N), dy = dFdy(N);\n"                                                                            \
    "  float var = 0.5 * (dot(dx, dx) + dot(dy, dy)) * aa;\n"                                                         \
    "  float a = rough * rough;\n"                                                                                    \
    "  return clamp(a * a + min(2.0 * var, 0.18), 0.002, 1.0);\n"                                                     \
    "}\n"                                                                                                             \
    "#endif\n"

/* the lamps (scene_lamps, gfx_metal.m's LAMP_MSL and its notes): every lit point light of the pool, binned into 64-pixel
 * tiles once a frame (lamps_bin); a pixel takes its tile's, each faded to nothing at its range. LS the lamps, TL the
 * tiles (32 16-bit words each: the count, the lamps), LU the viewport (vp), tiles across and down, shadows on and debug
 * (t), the zone's cubes, the lamp's size and the taps (k). The characters' shadows from the nearest lamps are cubes
 * (lsm, a lamp six layers, LM their faces' matrices), the zone's in zlsm, compared through lcs */
#define LAMP_GLSL                                                                                                     \
    "struct LL { vec4 pos, col, att, amb; };\n"                                                                       \
    "#ifdef GFX_FS\n"                                                                                                 \
    "layout(std430, set = 0, binding = 15) readonly buffer LLB { LL LS[]; };\n"                                       \
    "layout(std430, set = 0, binding = 16) readonly buffer TLB { uint TL[]; };\n"                                     \
    "layout(std140, set = 0, binding = 17) uniform LUB { vec4 vp; vec4 t; vec4 k; } LU;\n"                            \
    "layout(set = 0, binding = 18) uniform texture2DArray lsm;\n"                                                     \
    "layout(set = 0, binding = 19) uniform texture2DArray zlsm;\n"                                                    \
    "layout(std430, set = 0, binding = 20) readonly buffer LMB { mat4 LM[]; };\n"                                     \
    "layout(set = 0, binding = 21) uniform samplerShadow lcs;\n"                                                      \
    "uint tl_at(uint i) { uint w = TL[i >> 1]; return (i & 1u) != 0u ? w >> 16 : w & 0xffffu; }\n"                   \
    "float lamp_cmp(texture2DArray t, vec2 uv, uint l, float z) { return texture(sampler2DArrayShadow(t, lcs), vec4(uv, float(l), z)); }\n" \
    /* the lamp's shadow at P: the cube face its direction falls in, the point pushed toward the lamp a little */      \
    "float lamp_shadow(uint s, vec3 P, vec3 Lp, vec3 N, bool zone, float ls, int nt) {\n"                             \
    "  vec3 d = P - Lp, a = abs(d);\n"                                                                                \
    "  uint f = a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0u : 1u) : a.y >= a.z ? (d.y > 0.0 ? 2u : 3u) : (d.z > 0.0 ? 4u : 5u);\n" \
    "  float dl = length(d);\n"                                                                                       \
    "  uint l = s * 6u + f;\n"                                                                                        \
    "  vec4 h = LM[l] * vec4(P - d * (0.08 / max(dl, 0.5)) * min(dl, 2.0) + N * 0.06, 1.0);\n"                        \
    "  if (h.w <= 1e-4) return 1.0;\n"                                                                                \
    "  vec2 uv = vec2(h.x / h.w * 0.5 + 0.5, 0.5 - h.y / h.w * 0.5);\n"                                               \
    "  float z = h.z / h.w;\n"                                                                                        \
    "  if (ls <= 0.0) {\n"                                                                                            \
    "    float v = lamp_cmp(lsm, uv, l, z);\n"                                                                        \
    "    return zone ? min(v, lamp_cmp(zlsm, uv, l, z)) : v;\n"                                                       \
    "  }\n"                                                                                                           \
    /* soft (lamp_size): a disc as wide as the lamp seen from here, a face spanning twice the distance, one to six    \
     * texels */                                                                                                      \
    "  float tx = 1.0 / float(textureSize(sampler2DArrayShadow(lsm, lcs), 0).x), r = clamp(ls / (4.0 * max(dl, 0.3)), tx, 6.0 * tx);\n" \
    "  vec2 a0 = sh_turn(P);\n"                                                                                       \
    "  float v = 0.0;\n"                                                                                              \
    "  for (int i = 0; i < nt; ++i) {\n"                                                                              \
    "    vec2 q = uv + sh_vogel(i, nt, a0) * r;\n"                                                                    \
    "    float w = lamp_cmp(lsm, q, l, z);\n"                                                                         \
    "    v += zone ? min(w, lamp_cmp(zlsm, q, l, z)) : w;\n"                                                          \
    "  }\n"                                                                                                           \
    "  return v / float(nt);\n"                                                                                       \
    "}\n"                                                                                                             \
    "void lamp_light(vec2 fp, vec3 P, vec3 N, inout vec3 dif, inout vec3 amb, vec3 Ps, bool soft) {\n"                \
    "  ivec2 t = ivec2((fp - LU.vp.xy) / 64.0);\n"                                                                    \
    "  if (any(lessThan(t, ivec2(0))) || t.x >= int(LU.t.x) || t.y >= int(LU.t.y)) return;\n"                        \
    "  uint b = (uint(t.y) * uint(LU.t.x) + uint(t.x)) * 32u;\n"                                                      \
    "  uint n = tl_at(b);\n"                                                                                          \
    "  for (uint k = 0u; k < n; ++k) {\n"                                                                             \
    "    LL l = LS[tl_at(b + 1u + k)];\n"                                                                             \
    "    vec3 v = l.pos.xyz - P;\n"                                                                                   \
    "    float d = length(v), q = d / l.pos.w;\n"                                                                     \
    "    if (q >= 1.0) continue;\n"                                                                                   \
    "    float w = sat(1.0 - q * q * q * q);\n"                                                                       \
    "    float at = w * w / max(l.att.x + l.att.y * d + l.att.z * d * d, 1e-3);\n"                                    \
    "    amb += l.amb.rgb * at;\n"                                                                                    \
    "    float sh = l.att.w > 0.5 && LU.t.z > 0.5 ? lamp_shadow(uint(l.att.w) - 1u, Ps, l.pos.xyz, N, LU.k.x > 0.5, soft ? LU.k.y : 0.0, int(LU.k.z)) : 1.0;\n" \
    "    dif += l.col.rgb * at * sh * max(dot(N, v / max(d, 1e-4)), 0.0);\n"                                         \
    "    if (LU.t.w > 0.5 && l.att.w > 0.5) amb += vec3(1.0 - sh, sh, 0.0) * 4.0 * w;\n" /* scene_lamp_debug: red shadowed, green lit */ \
    "  }\n"                                                                                                           \
    "}\n"                                                                                                             \
    "#endif\n"

/* the bounce light, the shading's side (gfx_gi_msl.h's GI_MSL and its notes; what the probes are: GI_KERNELS_GLSL),
 * in the zone's and characters' functions and the probes' own kernels, which say where it is bound (GI_B_*) and
 * whether PS is written (GI_PS_Q). gi_at: the probes at P (normal N): rgb the bounce light (not scaled by strength), a
 * what the ambient is multiplied by (1 outside the grid). GLSL's % is undefined for negative operands: gi_slot wraps
 * with floor */
#define GI_OCT_GLSL                                                                                                   \
    "float gi_sgn(float v) { return v >= 0.0 ? 1.0 : -1.0; }\n"                                                       \
    "vec2 gi_oct(vec3 n) {\n"                                                                                         \
    "  n /= abs(n.x) + abs(n.y) + abs(n.z);\n"                                                                        \
    "  return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(gi_sgn(n.x), gi_sgn(n.y));\n"                              \
    "}\n"                                                                                                             \
    "vec3 gi_unoct(vec2 e) {\n"                                                                                       \
    "  vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));\n"                                                                \
    "  if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * vec2(gi_sgn(v.x), gi_sgn(v.y));\n"                                   \
    "  return normalize(v);\n"                                                                                        \
    "}\n"
#define GI_GLSL                                                                                                       \
    "layout(std140, set = 0, binding = GI_B_GU) uniform GUB { vec4 org; ivec4 base; ivec4 dims; vec4 k; vec4 inv; vec4 tr; uvec4 n;\n" \
    "  vec4 sun_dir, sun_col, sun2_dir, sun2_col, amb; mat4 rot; } G;\n"                                             \
    "layout(std430, set = 0, binding = GI_B_PS) GI_PS_Q buffer GPSB { uint PS[]; };\n"                               \
    "layout(set = 0, binding = GI_B_IR) uniform sampler2D gir;\n"                                                    \
    "layout(set = 0, binding = GI_B_DS) uniform sampler2D gid;\n"                                                    \
    GI_OCT_GLSL                                                                                                       \
    "ivec3 gi_slot(ivec3 c) { return c - G.dims.xyz * ivec3(floor(vec3(c) / vec3(G.dims.xyz))); }\n"                  \
    "uint gi_index(ivec3 s) { return uint(s.x + G.dims.x * (s.y + G.dims.y * s.z)); }\n"                              \
    /* a probe's map (n texels inside a border of one) at direction d, in the atlas's uv */                           \
    "vec2 gi_uv(ivec3 s, vec3 d, float n, vec2 inv) {\n"                                                              \
    "  vec2 t = vec2(float(s.x + G.dims.x * s.y), float(s.z)) * (n + 2.0) + 1.0 + (gi_oct(d) * 0.5 + 0.5) * n;\n"     \
    "  return t * inv;\n"                                                                                             \
    "}\n"                                                                                                             \
    "vec4 gi_at(vec3 P, vec3 N) {\n"                                                                                  \
    "  float sp = G.org.w;\n"                                                                                         \
    "  vec3 Pb = P + N * G.k.w;\n" /* off the surface, so its own probes' near hits don't shade it */                 \
    "  vec3 g = Pb / sp, fl = floor(g), f = g - fl;\n"                                                                \
    "  ivec3 i0 = ivec3(fl);\n"                                                                                       \
    "  vec3 sum = vec3(0.0);\n"                                                                                       \
    "  float sky = 0.0, ws = 0.0;\n"                                                                                  \
    "  for (int c = 0; c < 8; ++c) {\n"                                                                               \
    "    ivec3 o = ivec3(c & 1, (c >> 1) & 1, c >> 2), pc = i0 + o, rel = pc - G.base.xyz;\n"                         \
    "    if (any(lessThan(rel, ivec3(0))) || any(greaterThanEqual(rel, G.dims.xyz))) continue;\n"                     \
    "    ivec3 s = gi_slot(pc);\n"                                                                                    \
    "    if ((PS[gi_index(s)] & 3u) != 1u) continue;\n" /* traced, and not inside something */                       \
    "    vec3 tw = mix(1.0 - f, f, vec3(o));\n"                                                                       \
    "    float w = tw.x * tw.y * tw.z;\n"                                                                             \
    "    vec3 dv = vec3(pc) * sp - Pb;\n"                                                                             \
    "    float dl = length(dv);\n"                                                                                    \
    "    vec3 dn = dl > 1e-4 ? dv / dl : N;\n"                                                                        \
    "    float wb = (dot(dn, N) + 1.0) * 0.5;\n" /* probes behind the surface count less */                          \
    "    w *= wb * wb + 0.2;\n"                                                                                       \
    "    vec2 m = textureLod(gid, gi_uv(s, -dn, 14.0, G.inv.zw), 0.0).rg;\n"                                         \
    "    if (dl > m.x) {\n" /* the probe sees something nearer than this point that way: it is likely hidden */     \
    "      float v = abs(m.y - m.x * m.x), t = dl - m.x, ch = v / (v + t * t);\n"                                     \
    "      w *= max(ch * ch * ch, 0.05);\n"                                                                           \
    "    }\n"                                                                                                         \
    "    if (w < 0.2) w *= w * w * 25.0;\n"                                                                           \
    "    vec4 e = textureLod(gir, gi_uv(s, N, 6.0, G.inv.xy), 0.0);\n"                                               \
    "    sum += e.rgb * w;\n"                                                                                         \
    "    sky += e.a * w;\n"                                                                                           \
    "    ws += w;\n"                                                                                                  \
    "  }\n"                                                                                                           \
    "  if (ws <= 1e-5) return vec4(0.0, 0.0, 0.0, 1.0);\n"                                                            \
    "  vec3 r = g - vec3(G.base.xyz), e3 = min(r, vec3(G.dims.xyz - 1) - r);\n"                                       \
    "  float fade = clamp(min(e3.x, min(e3.y, e3.z)) * 0.5, 0.0, 1.0);\n" /* the grid's last two cells fade out */   \
    "  return vec4(sum / ws * fade, mix(1.0, mix(1.0, sky / ws, G.k.y), fade));\n"                                    \
    "}\n"

/* where the zone's and characters' functions take the bounce light (W_GU..W_GID) */
#define GI_SHADE_GLSL                                                                                                 \
    "#define GI_B_GU 22\n#define GI_B_PS 23\n#define GI_B_IR 24\n#define GI_B_DS 25\n#define GI_PS_Q readonly\n" GI_GLSL

/* the effects' functions the zone's and the characters' draws take, as their W_* defines say */
#define WFX_GLSL                                                                                                      \
    "#if W_SHADOW || W_LAMPS\n" SHDISC_GLSL "#endif\n"                                                                 \
    "#if W_SHADOW\n" SHADOW_GLSL "#endif\n"                                                                            \
    "#if W_AO\n" AO_GLSL "#endif\n"                                                                                    \
    "#if W_SPEC\n" SPEC_GLSL "#endif\n"                                                                                \
    "#if W_LAMPS\n" LAMP_GLSL "#endif\n"                                                                               \
    "#if W_GI\n" GI_SHADE_GLSL "#endif\n"

/* --- the scene renderer's zone (gfx_world_draw, gfx.h) --------------------------------------------------------------
 * gfx_metal.m's WORLD_MSL: D3D's fixed function as the game lights its zone, COLORVERTEX with the vertex colour as
 * diffuse and ambient reflectance, the sun (and moon) and the instance's four point lights, then MODULATE2X and
 * MODULATE4X and table fog. W_PIXEL lights per pixel (scene 2), W_ATEST alpha tests GREATER the reference, W_SHADOW
 * shades the directional light along the shadows' (the sun's or the moon's) with them. The vertices are the game's own
 * buffers, fetched as the generated functions fetch theirs (36 bytes: position, normal, D3DCOLOR, uv); the instance
 * (inst) and its flags are push constants. W_AO takes the occlusion from the ambient, W_SPEC adds the highlights (with
 * W_SHADOW), W_LAMPS lights every lamp per pixel in place of the instance's four point lights (with W_PIXEL), W_GI adds
 * the bounce light to the ambient (per vertex, or per pixel at its quality 3 in scene 2). */
static const char WORLD_GLSL[] =
    "#define sat(x) clamp(x, 0.0, 1.0)\n"
    "layout(std140, set = 0, binding = 0) uniform WF { mat4 vp; vec4 params; } F;\n" /* params: alpha reference 0..255, the pixel-centre fixup */
    "struct WL { vec4 pos, diffuse, ambient, att; };\n"
    "struct WI { mat4 wvp, world, world_inv; vec4 sun_dir, sun_col, sun2_dir, sun2_col, ambient, fog, fog_col; ivec4 light; };\n"
    "layout(std430, set = 0, binding = 1) readonly buffer WIB { WI I[]; };\n"
    "layout(std430, set = 0, binding = 2) readonly buffer WLB { WL L[]; };\n"
    "layout(push_constant) uniform WD { uint inst, flags; } D;\n"
    WFX_GLSL
    /* saturate(vc * (ambient + each light's ambient) + vc * sum of N.L x diffuse) without the saturate, and without the
     * directional light along sd, which goes into sun: saturate(the two's sum) is D3D's. lmp: the point lights' share,
     * which fills the sun's (the moon's) shadow (shadow_fill); amb: the ambient's, which the occlusion scales. With
     * W_LAMPS the point lights are the lamps', every one, per pixel (lamp_light). g: the bounce light, added to the
     * ambient, and what the ambient is multiplied by ((0, 0, 0, 1) without it) */
    "vec3 lit(vec3 P, vec3 N, vec3 vc, vec3 sd, vec4 g, out vec3 sun, out vec3 amb, out vec3 lmp) {\n"
    "  vec3 a = I[D.inst].ambient.rgb * g.a + g.rgb, d = vec3(0.0), s = vec3(0.0), pl = vec3(0.0);\n"
    "  if (I[D.inst].sun_dir.w > 0.0) {\n"
    "    vec3 t = I[D.inst].sun_col.rgb * max(dot(N, I[D.inst].sun_dir.xyz), 0.0);\n"
    "    if (dot(I[D.inst].sun_dir.xyz, sd) > 0.98) s += t; else d += t;\n"
    "  }\n"
    "  if (I[D.inst].sun2_dir.w > 0.0) {\n"
    "    vec3 t = I[D.inst].sun2_col.rgb * max(dot(N, I[D.inst].sun2_dir.xyz), 0.0);\n"
    "    if (dot(I[D.inst].sun2_dir.xyz, sd) > 0.98) s += t; else d += t;\n"
    "  }\n"
    "#if !W_LAMPS\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    int li = I[D.inst].light[k];\n"
    "    if (li < 0) continue;\n"
    "    vec3 v = L[li].pos.xyz - P;\n"
    "    float dist = length(v);\n"
    "    if (dist > L[li].pos.w) continue;\n"
    "    float at = 1.0 / max(L[li].att.x + L[li].att.y * dist + L[li].att.z * dist * dist, 1e-6);\n"
    "    vec3 t = L[li].ambient.rgb * at + L[li].diffuse.rgb * at * max(dot(N, v / max(dist, 1e-6)), 0.0);\n"
    "    d += t, pl += t;\n"
    "  }\n"
    "#endif\n"
    "  lmp = vc * pl;\n"
    "  sun = vc * s;\n"
    "  amb = vc * a;\n"
    "  return vc * a + vc * d;\n"
    "}\n"
    "#if W_SHADOW\n"
    "#define SD S.dir.xyz\n"
    "#else\n"
    "#define SD vec3(0.0)\n"
    "#endif\n"
    "#ifdef GFX_VS\n"
    "layout(std430, set = 0, binding = 3) readonly buffer VB { uint vb[]; };\n"
    "layout(location = 0) out vec2 o_uv;\n"
    "layout(location = 1) out vec4 o_col;\n"
    "layout(location = 2) out vec3 o_wpos;\n"
    "layout(location = 3) out vec3 o_nrm;\n"
    "layout(location = 4) out float o_z;\n"
    "layout(location = 5) out vec3 o_sun;\n"
    "layout(location = 6) out vec3 o_lmp;\n"
    "layout(location = 7) out vec3 o_amb;\n"
    "layout(location = 8) out vec4 o_aop;\n" /* where the occlusion was made: this point through last frame's camera */
    "layout(location = 9) out vec4 o_gi;\n"
    "void main() {\n"
    "  uint b = uint(gl_VertexIndex) * 9u;\n"
    "  vec3 pos = uintBitsToFloat(uvec3(vb[b], vb[b + 1u], vb[b + 2u]));\n"
    "  vec3 nrm = uintBitsToFloat(uvec3(vb[b + 3u], vb[b + 4u], vb[b + 5u]));\n"
    "  vec4 col = unpackUnorm4x8(vb[b + 6u]).zyxw;\n" /* D3DCOLOR's bytes are B, G, R, A */
    "  o_uv = uintBitsToFloat(uvec2(vb[b + 7u], vb[b + 8u]));\n"
    "  vec4 wp = I[D.inst].world * vec4(pos, 1.0);\n"
    "  vec3 n = (vec4(nrm, 0.0) * I[D.inst].world_inv).xyz;\n"
    "  if ((D.flags & 32u) != 0u) n = normalize(n);\n"
    "  gl_Position = I[D.inst].wvp * vec4(pos, 1.0);\n"
    "  o_z = gl_Position.w;\n"
    "  gl_Position.x += gl_Position.w * F.params.y;\n" /* D3D's pixel centres, as the generated functions' fixup */
    "  gl_Position.y -= gl_Position.w * F.params.z;\n"
    "  o_wpos = wp.xyz;\n"
    "  o_nrm = n;\n"
    "  o_col = col;\n"
    "  o_sun = vec3(0.0), o_lmp = vec3(0.0), o_amb = vec3(0.0);\n"
    "#if W_AO\n"
    "  o_aop = A.pvp * wp;\n"
    "#else\n"
    "  o_aop = vec4(0.0);\n"
    "#endif\n"
    "  o_gi = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "#if W_GI\n"
    "  if (!(W_PIXEL != 0 && G.org.x > 0.5)) { o_gi = gi_at(wp.xyz, normalize(n)); o_gi.rgb *= G.k.x; }\n"
    "#endif\n"
    "#if !W_PIXEL\n"
    "  vec3 rest = lit(wp.xyz, n, col.rgb, SD, o_gi, o_sun, o_amb, o_lmp);\n"
    "  o_col.rgb = W_SHADOW != 0 || W_AO != 0 ? rest : sat(rest + o_sun);\n"
    "#endif\n"
    "}\n"
    "#endif\n"
    "#ifdef GFX_FS\n"
    "layout(set = 0, binding = 4) uniform sampler2D t;\n"
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 1) in vec4 i_col;\n"
    "layout(location = 2) in vec3 i_wpos;\n"
    "layout(location = 3) in vec3 i_nrm;\n"
    "layout(location = 4) in float i_z;\n"
    "layout(location = 5) in vec3 i_sun;\n"
    "layout(location = 6) in vec3 i_lmp;\n"
    "layout(location = 7) in vec3 i_amb;\n"
    "layout(location = 8) in vec4 i_aop;\n"
    "layout(location = 9) in vec4 i_gi;\n"
    "layout(location = 0) out vec4 oc;\n"
    "void main() {\n"
    "  vec3 c = i_col.rgb, sun = i_sun, lmp = i_lmp, amb = i_amb;\n"
    "  float shv = 1.0;\n"
    "  vec3 N = (D.flags & 32u) != 0u ? normalize(i_nrm) : i_nrm;\n"
    "  vec4 g = i_gi;\n"
    "#if W_GI && W_PIXEL\n"
    "  if (G.org.x > 0.5) { g = gi_at(i_wpos, normalize(N)); g.rgb *= G.k.x; }\n"
    "#endif\n"
    "#if W_PIXEL\n"
    "  c = lit(i_wpos, N, i_col.rgb, SD, g, sun, amb, lmp);\n"
    "#endif\n"
    "#if W_LAMPS && W_PIXEL\n"
    "  {\n"
    "    vec3 ld = vec3(0.0), la = vec3(0.0);\n"
    "    lamp_light(gl_FragCoord.xy, i_wpos, normalize(N), ld, la, i_wpos, true);\n"
    "    c += i_col.rgb * (ld + la);\n" /* (on the vertex colour, as lit() lights) */
    "    lmp += i_col.rgb * (ld + la);\n"
    "    amb += i_col.rgb * la;\n"
    "  }\n"
    "#endif\n"
    "#if W_AO\n" /* the occlusion takes from the ambient alone; scene_ao_debug 1 shows it */
    "  {\n"
    "    float ao = pow(ao_at(i_aop), A.k.x);\n"
    "    if (A.k.z > 0.5) { oc = vec4(vec3(ao), 1.0); return; }\n"
    "    c -= amb * (1.0 - ao);\n"
    "  }\n"
    "#endif\n"
    "#if W_GI\n" /* scene_gi_debug: 1 the bounce light on the vertex colour, 2 the ambient's factor, 3 the bounce x 8 */
    "  if (G.k.z > 0.5) { oc = vec4(sat(G.k.z > 2.5 ? 8.0 * g.rgb : G.k.z > 1.5 ? vec3(g.a) : 2.0 * i_col.rgb * g.rgb), 1.0); return; }\n"
    "#endif\n"
    "#if W_SHADOW\n"
    "  if (S.cam.w > 1.5) {\n" /* scene_shadow_debug: the shadow, tinted by its cascade; dark facing away */
    "    float d = length(i_wpos - S.cam.xyz);\n"
    "    uint k = 0u;\n"
    "    while (k < 3u && d > S.cas[k].x) ++k;\n"
    "    vec3 tint = k == 0u ? vec3(1.0, 0.55, 0.55) : k == 1u ? vec3(0.55, 1.0, 0.55) : k == 2u ? vec3(0.55, 0.55, 1.0) : vec3(1.0, 1.0, 0.55);\n"
    "    float nl = dot(normalize(N), S.dir.xyz);\n"
    "    oc = vec4(nl > 0.0 ? tint * (0.2 + 0.8 * shadow(i_wpos, normalize(N), true)) : vec3(0.05), 1.0);\n"
    "    return;\n"
    "  }\n"
    "  if (any(greaterThan(sun, vec3(0.0)))) {\n"
    "    shv = shadow(i_wpos, normalize(N), true);\n"
    "    vec3 Y = vec3(0.2126, 0.7152, 0.0722);\n" /* where the lamps outshine the sun or moon, its shadow fades out */
    "    shv = mix(shv, 1.0, sat(S.ctx.w * dot(lmp, Y) / max(dot(sun, Y), 1e-4)));\n"
    "    sun *= shv;\n"
    "  }\n"
    "  c = sat(c + sun);\n"
    "#elif W_PIXEL || W_AO\n"
    "  c = sat(c + sun);\n"
    "#endif\n"
    "  vec4 tx = texture(t, i_uv);\n"
    "  vec4 o = vec4(sat(2.0 * tx.rgb * c), sat(4.0 * tx.a * i_col.a));\n" /* MODULATE2X, MODULATE4X */
    "#if W_ATEST\n"
    "  if (!(o.a * 255.0 > F.params.x)) discard;\n"
    "#endif\n"
    "#if W_SPEC && W_SHADOW\n" /* the sun's (or moon's) highlight, where it reaches, and the lamps' */
    "  {\n"
    "    vec3 Nn = normalize(N), V = normalize(S.cam.xyz - i_wpos);\n"
    "    float wet = SPc.k.z, lum = dot(tx.rgb, vec3(0.299, 0.587, 0.114));\n"
    "    float a2 = spec_a2(mix(0.8, 0.5, wet), Nn, SPc.k.w);\n"
    "    float f0 = mix(0.03, 0.05, wet) * (0.5 + lum);\n"
    "    float occ = sat(2.0 * dot(i_col.rgb, vec3(0.333)));\n" /* the vertex colour's darkness: corners and crevices */
    "    vec3 sp = vec3(0.0);\n"
    "    vec4 s1 = I[D.inst].sun_dir, s2 = I[D.inst].sun2_dir;\n"
    "    vec3 scol = s2.w > 0.0 && dot(s2.xyz, S.dir.xyz) > dot(s1.xyz, S.dir.xyz) ? I[D.inst].sun2_col.rgb : I[D.inst].sun_col.rgb;\n"
    "    if (s1.w > 0.0 || s2.w > 0.0) sp += scol * spec_ggx(Nn, V, S.dir.xyz, a2, f0) * shv;\n" /* (the light the shadows are drawn from) */
    "    for (int k = 0; k < 4 && SPc.k.y > 0.0; ++k) {\n"
    "      int li = I[D.inst].light[k];\n"
    "      if (li < 0) continue;\n"
    "      vec3 v = L[li].pos.xyz - i_wpos;\n"
    "      float dist = length(v);\n"
    "      if (dist > L[li].pos.w) continue;\n"
    "      float at = 1.0 / max(L[li].att.x + L[li].att.y * dist + L[li].att.z * dist * dist, 1e-6);\n"
    "      sp += L[li].diffuse.rgb * at * SPc.k.y * spec_ggx(Nn, V, v / max(dist, 1e-6), a2, f0);\n"
    "    }\n"
    "    if (SPc.pad.x > 0.0) o.rgb += sp * (SPc.k.x * occ);\n" /* an HDR scene: past white, the tonemap's to roll off */
    "    else o.rgb = 1.0 - (1.0 - o.rgb) * exp(-sp * (SPc.k.x * occ));\n" /* (a soft shoulder: toward white, never flat white) */
    "  }\n"
    "#endif\n"
    "  vec4 fog = I[D.inst].fog;\n"
    "  if (fog.z > 0.0) o.rgb = mix(I[D.inst].fog_col.rgb, o.rgb, sat((fog.y - i_z) / max(fog.y - fog.x, 1e-6)));\n"
    "  oc = o;\n"
    "}\n"
    "#endif\n";

/* --- the scene renderer's characters (gfx_chars_draw, gfx.h) -------------------------------------------------------
 * gfx_metal.m's CHARS_MSL: skinned from the frame's bone palettes (each vertex its bone-space position and normal for
 * two bones, weights applied, and their mirror bones), lit per vertex with D3D's fixed function as the game sets it
 * for a character, then its two texture stages; alpha tested, no culling. W_SHADOW: the sun's (the moon's) light
 * shaded with the shadows, looked up where the character stood when last frame's layers were drawn (lag); W_AO, W_SPEC,
 * W_LAMPS and W_GI as the zone's (the lamps per pixel, in place of the state's point lights; the bounce light per
 * vertex). The vertices (GfxCharVertex)
 * and uvs are storage buffers, the indices the game's own; the palette, state, mirror, lag and the frame's parameters
 * are push constants. */
static const char CHARS_GLSL[] =
    "#define sat(x) clamp(x, 0.0, 1.0)\n"
    "struct CL { vec4 pos, dir, diffuse, ambient, att; };\n"
    "struct CS { mat4 wvp, world; CL light[4]; vec4 ambient, md, ma, me, tf, fog, fog_col; };\n"
    "layout(std430, set = 0, binding = 1) readonly buffer PB { mat4 P[]; };\n"
    "layout(std430, set = 0, binding = 2) readonly buffer SB { CS St[]; };\n"
    "layout(push_constant) uniform CD { vec4 params; uint palette, state, mirror, pad; vec4 lag; } D;\n" /* params as WF's */
    WFX_GLSL
    "#ifdef GFX_VS\n"
    SKIN_GLSL
    "layout(location = 0) out vec2 o_uv;\n"
    "layout(location = 1) out vec4 o_col;\n"
    "layout(location = 2) out float o_z;\n"
    "layout(location = 3) out vec3 o_sun;\n"
    "layout(location = 4) out vec3 o_wp;\n"
    "layout(location = 5) out vec3 o_wn;\n"
    "layout(location = 6) out vec3 o_camb;\n" /* the ambient's share, which the occlusion scales */
    "layout(location = 7) out vec4 o_aop;\n"
    "layout(location = 8) flat out vec3 o_lmd;\n" /* a lamp's light as the game's would be: the material's scale and colours */
    "layout(location = 9) flat out vec3 o_lma;\n"
    "void main() {\n"
    "  vec3 n, p = skin(n);\n"
    "  vec3 wp = (St[D.state].world * vec4(p, 1.0)).xyz;\n"
    "  vec3 wn = normalize((St[D.state].world * vec4(n, 0.0)).xyz);\n" /* NORMALIZENORMALS */
    "  gl_Position = St[D.state].wvp * vec4(p, 1.0);\n"
    "  o_z = gl_Position.w;\n"
    "  gl_Position.x += gl_Position.w * D.params.y;\n"
    "  gl_Position.y -= gl_Position.w * D.params.z;\n"
    "  o_uv = uvs[gl_VertexIndex];\n"
    "  vec3 amb = St[D.state].ambient.rgb, dif = vec3(0.0), sun = vec3(0.0);\n"
    "#if W_GI\n" /* the bounce light */
    "  { vec4 g = gi_at(wp, wn); amb = amb * g.a + g.rgb * G.k.x; }\n"
    "#endif\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    CL l = St[D.state].light[k];\n"
    "    if (l.dir.w == 0.0) continue;\n"
    "#if W_LAMPS\n"
    "    if (l.dir.w > 1.5) continue;\n" /* (point lights: the lamps, per pixel) */
    "#endif\n"
    "    float at = 1.0;\n"
    "    vec3 Ld = l.dir.xyz;\n"
    "    if (l.dir.w > 1.5) {\n"
    "      vec3 d = l.pos.xyz - wp;\n"
    "      float dist = length(d);\n"
    "      if (dist > l.pos.w) continue;\n"
    "      Ld = d / max(dist, 1e-6);\n"
    "      at = 1.0 / max(l.att.x + l.att.y * dist + l.att.z * dist * dist, 1e-6);\n"
    "    }\n"
    "    amb += l.ambient.rgb * at;\n"
    "    vec3 t = l.diffuse.rgb * at * max(dot(wn, Ld), 0.0);\n"
    "#if W_SHADOW\n"
    "    if (l.dir.w < 1.5 && dot(Ld, S.dir.xyz) > 0.98) { sun += t; continue; }\n" /* the sun or moon */
    "#endif\n"
    "    dif += t;\n"
    "  }\n"
    "  vec3 rest = St[D.state].ma.rgb * amb + St[D.state].md.rgb * dif + St[D.state].me.rgb;\n"
    "  o_col = vec4(W_SHADOW != 0 || W_AO != 0 ? rest : sat(rest), St[D.state].md.a);\n"
    "  o_camb = St[D.state].ma.rgb * amb;\n"
    "#if W_AO\n"
    "  o_aop = A.pvp * vec4(wp, 1.0);\n"
    "#else\n"
    "  o_aop = vec4(0.0);\n"
    "#endif\n"
    "  o_sun = St[D.state].md.rgb * sun;\n"
    "  o_wp = wp, o_wn = wn;\n"
    "  o_lmd = St[D.state].md.rgb * St[D.state].fog_col.a, o_lma = St[D.state].ma.rgb;\n"
    "}\n"
    "#endif\n"
    "#ifdef GFX_FS\n"
    "layout(set = 0, binding = 4) uniform sampler2D t;\n"
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 1) in vec4 i_col;\n"
    "layout(location = 2) in float i_z;\n"
    "layout(location = 3) in vec3 i_sun;\n"
    "layout(location = 4) in vec3 i_wp;\n"
    "layout(location = 5) in vec3 i_wn;\n"
    "layout(location = 6) in vec3 i_camb;\n"
    "layout(location = 7) in vec4 i_aop;\n"
    "layout(location = 8) flat in vec3 i_lmd;\n"
    "layout(location = 9) flat in vec3 i_lma;\n"
    "layout(location = 0) out vec4 oc;\n"
    "void main() {\n"
    "  vec4 tx = texture(t, i_uv), tf = St[D.state].tf;\n"
    "  vec3 col = i_col.rgb, lmp = vec3(0.0);\n"
    "  float shv = 1.0;\n"
    "#if W_LAMPS\n"
    "  {\n"
    "    vec3 ld = vec3(0.0), la = vec3(0.0);\n"
    "    lamp_light(gl_FragCoord.xy, i_wp, normalize(i_wn), ld, la, W_SHADOW != 0 ? i_wp + D.lag.xyz : i_wp, false);\n"
    "    lmp = i_lmd * ld + i_lma * la;\n"
    "    col += lmp;\n"
    "  }\n"
    "#endif\n"
    "#if W_AO\n"
    "  {\n"
    "    float ao = pow(ao_at(i_aop), A.k.y);\n"
    "    if (A.k.z > 0.5) { oc = vec4(vec3(ao), 1.0); return; }\n"
    "    col -= i_camb * (1.0 - ao);\n"
    "    if (W_SHADOW == 0) col = sat(col);\n"
    "  }\n"
    "#endif\n"
    "#if W_SHADOW\n"
    "  vec3 sun = i_sun;\n"
    "  if (any(greaterThan(sun, vec3(0.0)))) {\n"
    "    shv = shadow(i_wp + D.lag.xyz, normalize(i_wn), false);\n" /* (PCSS speckles curved, close self-shadow) */
    "    vec3 Y = vec3(0.2126, 0.7152, 0.0722);\n" /* the lamps fill it, as the zone's (shadow_fill) */
    "    shv = mix(shv, 1.0, sat(S.ctx.w * dot(lmp, Y) / max(dot(sun, Y), 1e-4)));\n"
    "    sun *= shv;\n"
    "  }\n"
    "  col = sat(col + sun);\n"
    "#endif\n"
    "  vec3 c = sat(2.0 * sat(2.0 * tx.rgb * col) * tf.rgb);\n" /* MODULATE2X, then MODULATE2X by the factor */
    "  float a = sat(4.0 * sat(2.0 * tx.a * tf.a) * tf.a);\n"    /* MODULATE2X, then MODULATE4X */
    "  if (!(a * 255.0 > D.params.x)) discard;\n"
    "#if W_SPEC && W_SHADOW\n" /* the sun's highlight: its colour from what it lit here (its diffuse over N.L) */
    "  {\n"
    "    vec3 Nn = normalize(i_wn), V = normalize(S.cam.xyz - i_wp), Ls = S.dir.xyz;\n"
    "    vec3 tc = sat(2.0 * tx.rgb * tf.rgb);\n"
    "    float lum = dot(tc, vec3(0.299, 0.587, 0.114));\n"
    "    float sa = max(tc.r, max(tc.g, tc.b)) - min(tc.r, min(tc.g, tc.b));\n"
    "    float m = smoothstep(0.25, 0.6, lum) * (1.0 - smoothstep(0.08, 0.3, sa));\n" /* bright and grey: metal */
    "    float a2 = spec_a2(mix(0.7, 0.3, m), Nn, SPc.k.w);\n"
    "    vec3 sc = i_sun / max(sat(dot(Nn, Ls)), 0.2);\n"
    "    vec3 f = mix(vec3(0.03), tc * 0.6, m);\n"
    "    float g = spec_ggx(Nn, V, Ls, a2, 1.0);\n" /* (Fresnel's own, tinted below) */
    "    vec3 Hh = normalize(V + Ls);\n"
    "    float fr = pow(1.0 - sat(dot(V, Hh)), 5.0);\n"
    "    vec3 Fr = f + (1.0 - f) * fr;\n"
    "    if (SPc.pad.x > 0.0) c += sc * g * Fr * (SPc.k.x * shv);\n"
    "    else c = 1.0 - (1.0 - c) * exp(-sc * g * Fr * (SPc.k.x * shv));\n"
    "  }\n"
    "#endif\n"
    "  vec4 fog = St[D.state].fog;\n"
    "  if (fog.z > 0.0) c = mix(St[D.state].fog_col.rgb, c, sat((fog.y - i_z) / max(fog.y - fog.x, 1e-6)));\n"
    "  oc = vec4(c, a);\n"
    "}\n"
    "#endif\n";

/* --- the shadows' casters: the zone into a cascade of its layer (C_CHARS 0: depth alone, or alpha tested as the walk
 * tests it, MODULATE4X GREATER 0x60), the characters into a layer of theirs (C_CHARS 1, alpha tested as they are drawn).
 * F.M is the layer's matrix; the zone's world matrices are a storage buffer (inst a push constant) */
static const char CAST_GLSL[] =
    "#define sat(x) clamp(x, 0.0, 1.0)\n"
    "layout(std140, set = 0, binding = 0) uniform CF { mat4 M; vec4 params; } F;\n" /* params.x: the alpha reference */
    "#if C_CHARS\n"
    "struct CL { vec4 pos, dir, diffuse, ambient, att; };\n"
    "struct CS { mat4 wvp, world; CL light[4]; vec4 ambient, md, ma, me, tf, fog, fog_col; };\n"
    "layout(std430, set = 0, binding = 1) readonly buffer PB { mat4 P[]; };\n"
    "layout(std430, set = 0, binding = 2) readonly buffer SB { CS St[]; };\n"
    "layout(push_constant) uniform CD { vec4 params; uint palette, state, mirror, pad; } D;\n"
    "#else\n"
    "layout(std430, set = 0, binding = 1) readonly buffer WB { mat4 W[]; };\n"
    "layout(push_constant) uniform ZD { uint inst; } D;\n"
    "#endif\n"
    "#ifdef GFX_VS\n"
    "layout(location = 0) out vec2 o_uv;\n"
    "layout(location = 1) out float o_a;\n"
    "#if C_CHARS\n"
    SKIN_GLSL
    "void main() {\n"
    "  vec3 n, p = skin(n);\n"
    "  gl_Position = F.M * (St[D.state].world * vec4(p, 1.0));\n"
    "  CLAMP_Z;\n"
    "  o_uv = uvs[gl_VertexIndex];\n"
    "  o_a = St[D.state].tf.a;\n"
    "}\n"
    "#else\n"
    "layout(std430, set = 0, binding = 3) readonly buffer VB { uint vb[]; };\n"
    "void main() {\n"
    "  uint b = uint(gl_VertexIndex) * 9u;\n"
    "  vec3 pos = uintBitsToFloat(uvec3(vb[b], vb[b + 1u], vb[b + 2u]));\n"
    "  gl_Position = F.M * (W[D.inst] * vec4(pos, 1.0));\n"
    "  CLAMP_Z;\n"
    "  o_uv = uintBitsToFloat(uvec2(vb[b + 7u], vb[b + 8u]));\n"
    "  o_a = unpackUnorm4x8(vb[b + 6u]).w;\n"
    "}\n"
    "#endif\n"
    "#endif\n"
    "#ifdef GFX_FS\n"
    "layout(set = 0, binding = 4) uniform sampler2D t;\n"
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 1) in float i_a;\n"
    "void main() {\n"
    "#if C_CHARS\n"
    "  float a = sat(4.0 * sat(2.0 * texture(t, i_uv).a * i_a) * i_a);\n"
    "  if (!(a * 255.0 > F.params.x)) discard;\n"
    "#else\n"
    "  if (!(sat(4.0 * texture(t, i_uv).a * i_a) * 255.0 > 96.0)) discard;\n"
    "#endif\n"
    "}\n"
    "#endif\n";

/* the zone's and characters' bindings (push descriptors): 0 the frame's uniforms (WF; a caster's CF), 1 the instances or
 * the bone palettes (a zone caster's world matrices), 2 the lights or the characters' states, 3 the vertices, 4 the
 * texture, 5 the characters' uvs; with the shadows 6 their uniforms (SH), 7-9 the zone's, the characters' and the
 * fitted maps, 10 the comparing sampler, 11 the gathering one; with the occlusion 12 its uniforms (AU), 13 the result;
 * with the highlights 14 theirs (SP); with the lamps 15 the lamps, 16 the tiles, 17 their uniforms (LU), 18-19 the
 * characters' and the zone's cubes, 20 the cubes' faces, 21 their comparing sampler */
enum
{
    W_FRAME = 0,
    W_INST = 1,
    W_LIGHTS = 2,
    W_VB = 3,
    W_TEX = 4,
    W_UV = 5,
    W_SH = 6,
    W_ZM = 7,
    W_CM = 8,
    W_CT = 9,
    W_CS = 10,
    W_PS = 11,
    W_AU = 12,
    W_AOT = 13,
    W_SP = 14,
    W_LL = 15,
    W_TL = 16,
    W_LU = 17,
    W_LSM = 18,
    W_ZLSM = 19,
    W_LSMM = 20,
    W_LCS = 21,
    W_GU = 22, /* the bounce light (GI_SHADE_GLSL): its uniforms, the probes' states, the atlases */
    W_GPS = 23,
    W_GIR = 24,
    W_GID = 25,
    W_COUNT = 26,
};

static struct
{
    int tried, ok;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    /* the zone's (by W_PIXEL, W_ATEST, W_SHADOW, W_AO, W_SPEC, W_LAMPS, W_GI) and the characters' (by W_SHADOW, W_AO,
     * W_SPEC, W_LAMPS, W_GI) functions, made on first use: [stage][key] */
    VkShaderModule mod[2][256];
    VkShaderModule cast[2][2]; /* the casters': [stage][C_CHARS] */
    struct
    {
        uint32_t key[2];
        VkPipeline p;
    } pipe[256];
    uint32_t npipe;
} g_world;

static const VkDynamicState WORLD_DYNAMIC[] = {
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_DEPTH_BIAS,
    VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_CULL_MODE,
    VK_DYNAMIC_STATE_FRONT_FACE,
    VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
};

static int world_init(void)
{
    if (g_world.tried)
        return g_world.ok;
    g_world.tried = 1;
    VkDescriptorSetLayoutBinding b[W_COUNT];
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[W_FRAME] = (VkDescriptorSetLayoutBinding){ W_FRAME, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[W_INST] = (VkDescriptorSetLayoutBinding){ W_INST, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, NULL };
    b[W_LIGHTS] = (VkDescriptorSetLayoutBinding){ W_LIGHTS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, NULL };
    b[W_VB] = (VkDescriptorSetLayoutBinding){ W_VB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    b[W_TEX] = (VkDescriptorSetLayoutBinding){ W_TEX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_UV] = (VkDescriptorSetLayoutBinding){ W_UV, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    b[W_SH] = (VkDescriptorSetLayoutBinding){ W_SH, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    for (uint32_t i = W_ZM; i <= W_CT; ++i)
        b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_CS] = (VkDescriptorSetLayoutBinding){ W_CS, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_PS] = (VkDescriptorSetLayoutBinding){ W_PS, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_AU] = (VkDescriptorSetLayoutBinding){ W_AU, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[W_AOT] = (VkDescriptorSetLayoutBinding){ W_AOT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_SP] = (VkDescriptorSetLayoutBinding){ W_SP, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_LL] = (VkDescriptorSetLayoutBinding){ W_LL, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, NULL }; /* (the glows' too) */
    b[W_TL] = (VkDescriptorSetLayoutBinding){ W_TL, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_LU] = (VkDescriptorSetLayoutBinding){ W_LU, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_LSM] = (VkDescriptorSetLayoutBinding){ W_LSM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_ZLSM] = (VkDescriptorSetLayoutBinding){ W_ZLSM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_LSMM] = (VkDescriptorSetLayoutBinding){ W_LSMM, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_LCS] = (VkDescriptorSetLayoutBinding){ W_LCS, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[W_GU] = (VkDescriptorSetLayoutBinding){ W_GU, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[W_GPS] = (VkDescriptorSetLayoutBinding){ W_GPS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, NULL };
    b[W_GIR] = (VkDescriptorSetLayoutBinding){ W_GIR, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, both, NULL };
    b[W_GID] = (VkDescriptorSetLayoutBinding){ W_GID, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, both, NULL };
    VkPushConstantRange pcr = { both, 0, 48 };
    push_layout(b, W_COUNT, &pcr, &g_world.dsl, &g_world.layout);
    g_world.ok = g_world.dsl && g_world.layout;
    return g_world.ok;
}

/* the effects a zone's or characters' pipeline has (the GLSL's defines): WK_* bits */
enum
{
    WK_PIXEL = 1,
    WK_ATEST = 2,
    WK_SHADOW = 4,
    WK_AO = 8,
    WK_SPEC = 16,
    WK_LAMPS = 32,
    WK_GI = 64,
};

/* one stage of the zone's (chars 0) or the characters' (1) functions, compiled on first use (k: WK_*; the characters'
 * are neither per pixel nor alpha tested by a define) */
static VkShaderModule world_module(int chars, uint32_t k, int fragment)
{
    if (chars)
        k &= ~(uint32_t)(WK_PIXEL | WK_ATEST);
    VkShaderModule* m = &g_world.mod[fragment][(chars ? 128u : 0u) | k];
    if (*m)
        return *m;
    char d[224];
    snprintf(d, sizeof d,
        "#define W_PIXEL %d\n#define W_ATEST %d\n#define W_SHADOW %d\n#define W_AO %d\n#define W_SPEC %d\n#define W_LAMPS %d\n"
        "#define W_GI %d\n",
        !!(k & WK_PIXEL), !!(k & WK_ATEST), !!(k & WK_SHADOW), !!(k & WK_AO), !!(k & WK_SPEC), !!(k & WK_LAMPS), !!(k & WK_GI));
    *m = compile_with(d, chars ? CHARS_GLSL : WORLD_GLSL, fragment);
    if (!*m)
        fprintf(stderr, "[recomp] gfx: scene renderer %s functions failed\n", chars ? "characters'" : "zone's");
    return *m;
}

/* a pipeline into the current targets: the zone's or the characters', with the effects k (WK_*), blended or not */
static VkPipeline world_pipeline(int chars, uint32_t k, int blend, GfxTex* depth)
{
    uint32_t df = depth ? (uint32_t)depth->vf : 0, sf = depth && depth->has_stencil ? df : 0;
    uint32_t key[2] = { (uint32_t)chars | (uint32_t)blend << 1 | (uint32_t)(sf != 0) << 2 | k << 3 | (uint32_t)g_rt->vf << 12, df };
    for (uint32_t i = 0; i < g_world.npipe; ++i)
        if (g_world.pipe[i].key[0] == key[0] && g_world.pipe[i].key[1] == key[1])
            return g_world.pipe[i].p;
    if (g_world.npipe == sizeof g_world.pipe / sizeof g_world.pipe[0])
        return VK_NULL_HANDLE;
    VkShaderModule vs = world_module(chars, k, 0), fs = world_module(chars, k, 1);
    VkPipeline p = VK_NULL_HANDLE;
    if (vs && fs)
    {
        VkPipelineColorBlendAttachmentState bs = { 0 };
        bs.colorWriteMask = 0xF;
        if (blend)
        {
            bs.blendEnable = VK_TRUE;
            bs.srcColorBlendFactor = bs.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            bs.dstColorBlendFactor = bs.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
        p = make_pipeline(vs, fs, g_world.layout, &bs, g_rt->vf, (VkFormat)df, (VkFormat)sf, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_POLYGON_MODE_FILL, WORLD_DYNAMIC, sizeof WORLD_DYNAMIC / sizeof WORLD_DYNAMIC[0], 0);
    }
    /* (a failure is kept too: not tried again every draw) */
    g_world.pipe[g_world.npipe].key[0] = key[0], g_world.pipe[g_world.npipe].key[1] = key[1];
    g_world.pipe[g_world.npipe++].p = p;
    return p;
}

static void desc_write(VkWriteDescriptorSet* w, uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo* bi,
    const VkDescriptorImageInfo* ii)
{
    *w = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w->dstBinding = binding;
    w->descriptorCount = 1;
    w->descriptorType = type;
    w->pBufferInfo = bi;
    w->pImageInfo = ii;
}

/* an acceleration structure's write: wa holds it, until the push */
static void as_write(VkWriteDescriptorSet* w, uint32_t binding, VkWriteDescriptorSetAccelerationStructureKHR* wa,
    const VkAccelerationStructureKHR* as)
{
    *wa = (VkWriteDescriptorSetAccelerationStructureKHR){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    wa->accelerationStructureCount = 1, wa->pAccelerationStructures = as;
    desc_write(w, binding, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, NULL, NULL);
    w->pNext = wa;
}

/* the state the zone's and characters' draws share: no stencil, front faces clockwise (D3D's), the depth tested as
 * the game tests its zone (LESSEQUAL) when there is one, no scissor */
static void world_state(GfxTex* depth)
{
    VkCommandBuffer cb = g_cb;
    int32_t none[4] = { 0, 0, 0, 0 };
    set_scissor(none);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetDepthTestEnable(cb, depth ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_LESS_OR_EQUAL);
    vkCmdSetDepthBiasEnable(cb, VK_TRUE);
}

int gfx_world_supported(void) { return g_dev != VK_NULL_HANDLE; }

void gfx_world_time(int pass)
{
    if (!g_dev || !gfx_pass_timing_on())
        return;
    end_pass();
    g_pass_next = pass == 1 ? GFX_PASS_WORLD : pass == 2 ? GFX_PASS_CHARS : -1;
}

/* --- the lamps (scene_lamps): gfx_lamps hands the frame's; binned into 64-pixel tiles of the scene's viewport at the
 * frame's first zone batch (its camera), the list and the tiles in the frame's ring (gfx_metal.m's, its notes there) */

/* a zone item kept for the lamps' zone cubes at the frame's end: its buffer, texture and sampler as handles (the game
 * may destroy the GfxBuf meanwhile; the handles live until the frame is done) */
typedef struct LampZoneItem
{
    VkBuffer vb;
    VkImageView tex; /* alpha tested with it; VK_NULL_HANDLE: depth alone */
    VkSampler samp;
    uint32_t first, count, prim, inst;
} LampZoneItem;

static struct
{
    GfxWorldLight l[256];
    uint32_t n;
    uint64_t set, binned;            /* the frames they were set, binned */
    VkBuffer lb, tb;                 /* the lamps (LL), the tiles (32 16-bit words each: count, indices) */
    VkDeviceSize lo, to, lsz, tsz;
    float lu[12];                    /* LU: viewport, tiles across, down; shadows; debug; the zone's cubes, size, taps */
    float vp[16];                    /* the camera's view x projection, for the glows */
    float cam[3];
    /* the characters' shadows from the nearest lamps (scene_lamp_shadows): drawn at a frame's end into a cube a lamp
     * (six layers each of lsm), read by the next frame; each its lamp's place, to find it again */
    GfxTex *lsm, *dummy;
    uint32_t lsize, nsh;
    float shpos[4][4];               /* the shadowed lamps' places (w: range) */
    float shm[24][16];               /* their faces' matrices */
    uint64_t sh_serial;              /* the frame they were drawn */
    /* the zone into the same lamps' cubes (zlsm, Depth32F), kept: drawn again when the shadowed lamps change or every
     * 600 frames (a door, a lift) */
    GfxTex* zlsm;
    float zpos[4][4];
    uint32_t zn;
    uint64_t z_serial;
    /* the frame's opaque zone items, every batch's, and their world matrices, for the cubes at the frame's end */
    LampZoneItem* zi;
    float (*zw)[16];
    uint32_t nzi, czi, nzw, czw;
    uint64_t zc_serial;
} g_lamps;

void gfx_lamps(const GfxWorldLight* l, uint32_t n)
{
    g_lamps.n = n > 256 ? 256 : n;
    if (g_lamps.n)
        memcpy(g_lamps.l, l, g_lamps.n * sizeof *l);
    g_lamps.set = g_serial;
}

static int g_fixed_light; /* the zone's light is fixed (scene.c, the weather record): the modern fog a light dust */
void gfx_set_fixed_light(int fixed) { g_fixed_light = fixed != 0; }
static int g_weather = -1; /* the zone's weather (host64, the server's packets): the ground mist and its drift */
void gfx_set_weather(int weather) { g_weather = weather; }

static void lamp_shadows_draw(const GfxCharFrame* cf, VkDescriptorBufferInfo pal, VkDescriptorBufferInfo st);
static void lamp_zone_cubes(void);

/* --- the scene renderer's sun and moon shadows (gfx_shadow_begin, gfx.h): gfx_metal.m's, on Vulkan ----------------
 * Two depth arrays a cascade a layer: the zone's, 32-bit and kept (a layer drawn again only when host/scene.c says),
 * and the characters', 16-bit, drawn at each frame's end into the first cascades and a fitted layer round those near
 * the camera, read by the next frame. Casters are drawn with no culling, the depth clamped rather than clipped where
 * the device can (what stands nearer the light than the cascade's box still casts) and a slope bias; the receivers'
 * side (SHADOW_GLSL) adds a normal offset. The passes are the frame's command buffer's, between the game's. */

/* the shadows' uniforms for the draws, as SHADOW_GLSL's SH */
typedef struct ShU
{
    float m[GFX_SHADOW_CASCADES][16];
    float cas[GFX_SHADOW_CASCADES][4];  /* reach from the camera, texel, depth range (world units), valid */
    float cbox[GFX_SHADOW_CASCADES][4]; /* the characters in the map (x0, y0, x1, y1), with a margin */
    float dir[4], cam[4];
    float info[4]; /* cascades, of them the characters', strength, 1 / size */
    float cm[GFX_SHADOW_CASCADES][16]; /* the characters' layers (last frame's): 0 fitted round those near the camera, 1
                                        * and on the first cascades' */
    float ctx[4];  /* world units a texel in the fitted layer (0: none), 1 / its size, the penumbra's width (1 the sun's), how
                    * the lamps fill the shadow */
    float pk[4];   /* soft shadows: the light's tan(angular radius) (0: off), the blocker search's taps, the filter's */
} ShU;

/* a character's draw as the shadow passes take it at the frame's end: its buffers' and texture's handles as they were
 * (the game may let them go or fill them anew first: both wait for the frame) */
typedef struct ShItem
{
    VkBuffer vb, uv, ib;
    VkImageView tex;
    VkSampler samp;
    uint32_t index_size, first, count, prim, palette, state, mirror;
    float at[3]; /* its place, world: its first bone through its state's WORLD */
    uintptr_t key;
} ShItem;

static struct
{
    int on; /* this batch's draws are shadowed */
    ShU u;
    GfxTex *zmap, *cmap, *cmt; /* the zone's layers (kept); the characters' in the first cascades, half size; the fitted one */
    uint32_t size, fsize;
    VkPipeline zone[2], chars; /* depth alone, alpha tested; the characters' */
    int tried;
    float freach;        /* the fitted layer's reach from the camera */
    uint64_t cframe;     /* the frame of the last batch */
    uint32_t cdone;      /* of the frame's characters (the host's list grows batch by batch), those taken so far */
    float near_lo[2], near_hi[2];       /* the frame's near characters in cascade 0's map */
    float cbox[GFX_SHADOW_CASCADES][4]; /* the frame's characters in each cascade's map */
    /* the frame's characters for the layers drawn at its end: its last batch (the host's palette and states hold until
     * the next frame) and every batch's items */
    GfxShadowFrame lf;
    GfxCharFrame lc;
    ShItem* items;
    uint32_t nitems, capitems;
    uint32_t lnch;
    uint64_t lserial;
    /* the layers drawn at a frame's end, read by the next frame */
    struct
    {
        uint64_t serial;  /* the frame they hold */
        uint32_t n;       /* cascade layers */
        float cm[2][16];  /* their matrices */
        float cbox[2][4]; /* their characters' boxes, with a margin */
        float tm[16], ttx, chs; /* the fitted layer's matrix, texel (0 none), half size */
        /* where each character's part (its vertex buffer and mirror) was: open addressing, cap a power of two */
        uintptr_t* key;
        float (*at)[3];
        uint32_t cap;
    } lag;
} g_shd;
static uint32_t g_shd_drawn[2]; /* character draws into the cascades and the fitted layer since the last log line */

static void char_at(const GfxCharFrame* cf, const GfxCharItem* it, float at[3])
{
    const float* r = cf->palette + 16 * (size_t)it->palette + 12;
    const float* W = cf->state[it->state].world;
    for (int a = 0; a < 3; ++a)
        at[a] = r[0] * W[a] + r[1] * W[4 + a] + r[2] * W[8 + a] + W[12 + a];
}

static uint32_t char_hash(uintptr_t k) { return (uint32_t)((k >> 4) * 0x9E3779B97F4A7C15ull >> 32); }
static uintptr_t char_key(const GfxCharItem* it) { return (uintptr_t)it->vb ^ (uintptr_t)(it->mirror != 0); }

/* how far a character's part moved since last frame's layers were drawn (where it was, less where it is): its shadow
 * lookups go back by it, so it is shadowed as it stood then; 0 when it wasn't there (or jumped) */
static void char_lag(const GfxCharFrame* cf, const GfxCharItem* it, float d[4])
{
    d[0] = d[1] = d[2] = d[3] = 0.0f;
    if (!g_shd.lag.cap || g_shd.lag.serial + 1 != g_serial)
        return;
    uintptr_t k = char_key(it);
    for (uint32_t h = char_hash(k) & (g_shd.lag.cap - 1);; h = (h + 1) & (g_shd.lag.cap - 1))
    {
        if (!g_shd.lag.key[h])
            return;
        if (g_shd.lag.key[h] == k)
        {
            float now[3];
            char_at(cf, it, now);
            float dx = g_shd.lag.at[h][0] - now[0], dy = g_shd.lag.at[h][1] - now[1], dz = g_shd.lag.at[h][2] - now[2];
            if (dx * dx + dy * dy + dz * dz < 1.0f)
                d[0] = dx, d[1] = dy, d[2] = dz;
            return;
        }
    }
}

/* the host's items [from, nitem) taken (marked used this frame, so a later update renames rather than overwrites) */
static void shadow_items_add(const GfxCharFrame* cf, uint32_t from)
{
    if (g_shd.nitems + (cf->nitem - from) > g_shd.capitems)
        g_shd.capitems = (g_shd.nitems + (cf->nitem - from)) * 2,
        g_shd.items = (ShItem*)realloc(g_shd.items, g_shd.capitems * sizeof *g_shd.items);
    for (uint32_t i = from; i < cf->nitem; ++i)
    {
        const GfxCharItem* it = &cf->item[i];
        if (!it->vb || !it->uv || !it->ib || !it->tex || !it->count || it->tex->type != GFX_TEX_2D)
            continue;
        it->vb->used = it->uv->used = it->ib->used = it->tex->used = g_serial;
        it->tex->rec = g_cb_index;
        ShItem* o = &g_shd.items[g_shd.nitems++];
        o->vb = it->vb->b, o->uv = it->uv->b, o->ib = it->ib->b, o->tex = it->tex->view;
        o->samp = sampler(&it->samp);
        o->index_size = it->index_size, o->first = it->first, o->count = it->count, o->prim = it->prim;
        o->palette = it->palette, o->state = it->state, o->mirror = it->mirror;
        char_at(cf, it, o->at);
        o->key = char_key(it);
    }
}

/* a depth array of n layers, size across (an attachment view a layer, attachment_view's face), cleared to 1 */
static GfxTex* tex_array(VkFormat vf, uint32_t size, uint32_t n)
{
    return tex_make(GFX_TEX_2D, F_D16, size, size, 1, GFX_USE_DEPTH, vf, 1, n, 0);
}

/* a caster pipeline: the zone's (chars 0; atest: with its fragment function) into Depth32F, the characters' into Depth16 */
static VkPipeline cast_pipeline(int chars, int atest)
{
    VkShaderModule m[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    for (int s = 0; s < 2; ++s)
    {
        if (s && !atest)
            break;
        VkShaderModule* p = &g_world.cast[s][chars];
        /* without depth clamp, a caster nearer the light than the cascade is flattened onto its near plane instead of
         * clipped (the cascades are orthographic: z is the depth) */
        char d[96];
        snprintf(d, sizeof d, "#define C_CHARS %d\n#define CLAMP_Z %s\n", chars, g_has_clamp ? "" : "gl_Position.z = max(gl_Position.z, 0.0)");
        if (!*p)
            *p = compile_with(d, CAST_GLSL, s);
        m[s] = *p;
    }
    if (!m[0] || (atest && !m[1]))
        return VK_NULL_HANDLE;
    return make_pipeline(m[0], m[1], g_world.layout, NULL, VK_FORMAT_UNDEFINED, chars ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_UNDEFINED, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, WORLD_DYNAMIC,
        sizeof WORLD_DYNAMIC / sizeof WORLD_DYNAMIC[0], 2);
}

static int shadow_init(uint32_t size)
{
    if (!world_init())
        return 0;
    if (!g_shd.tried)
    {
        g_shd.tried = 1;
        g_shd.zone[0] = cast_pipeline(0, 0);
        g_shd.zone[1] = cast_pipeline(0, 1);
        g_shd.chars = cast_pipeline(1, 1);
        if (!g_has_clamp)
            fprintf(stderr, "[recomp] gfx: no depth clamp: the scene renderer's casters nearer the light than a cascade are flattened onto it\n");
    }
    if (!g_shd.zone[0] || !g_shd.zone[1] || !g_shd.chars)
        return 0;
    uint32_t fsz = (uint32_t)g_fxs.scene_shadow_fit_size;
    fsz = fsz < 256 ? 256 : fsz > 4096 ? 4096 : fsz;
    if (g_shd.size != size || g_shd.fsize != fsz || !g_shd.zmap || !g_shd.cmap || !g_shd.cmt)
    {
        end_pass(); /* (a pass of the game's may hold them) */
        if (g_shd.size != size || !g_shd.zmap) /* the zone's layers are kept: only a new size draws them anew */
            gfx_tex_destroy(g_shd.zmap), g_shd.zmap = tex_array(VK_FORMAT_D32_SFLOAT, size, GFX_SHADOW_CASCADES);
        gfx_tex_destroy(g_shd.cmap), gfx_tex_destroy(g_shd.cmt);
        g_shd.cmap = tex_array(VK_FORMAT_D16_UNORM, size / 2, GFX_SHADOW_CASCADES);
        g_shd.cmt = tex_array(VK_FORMAT_D16_UNORM, fsz, 1);
        g_shd.size = size, g_shd.fsize = fsz;
        g_shd.cframe = 0, g_shd.lag.serial = 0, g_shd.lserial = 0;
    }
    return g_shd.zmap && g_shd.cmap && g_shd.cmt;
}

/* a depth pass into one layer of a shadow map, cleared, y up as the draws' (set_viewport): the map's rows as
 * SHADOW_GLSL reads them; the casters' state set; its timestamp's index (ts_end's). next: a layer after another of the
 * same group, which wrote apart from it and read nothing since - no barrier between them */
static int shadow_pass_begin(GfxTex* t, uint32_t layer, int pass, int next)
{
    if (next)
        g_dirty = 0;
    VkViewport v = { 0, (float)t->h, (float)t->w, -(float)t->h, 0, 1 };
    int ts = pass_open(t, layer, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, &v, pass);
    VkCommandBuffer cb = g_cb;
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetDepthTestEnable(cb, VK_TRUE);
    vkCmdSetDepthWriteEnable(cb, VK_TRUE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_LESS_OR_EQUAL);
    vkCmdSetDepthBiasEnable(cb, VK_TRUE);
    vkCmdSetDepthBias(cb, 0.0f, 0.0f, 2.0f);
    return ts;
}

/* a caster layer's draws (the cascades', the lamps' cubes'): its uniforms (W_FRAME: the layer's matrix, the alpha
 * reference), the zone's instances' matrices or the characters' palette (W_INST) and states (W_LIGHTS), and what the
 * last draw left bound */
typedef struct Cast
{
    VkDescriptorBufferInfo bi[3];
    float alpha_ref;
    int pipe, topo;
    uint32_t drawn;
} Cast;

/* the layer's uniforms; p bound for every draw (the characters'), or VK_NULL_HANDLE (the zone's: chosen per draw) */
static void cast_begin(Cast* c, const float* m, float alpha_ref, VkDescriptorBufferInfo inst, VkDescriptorBufferInfo states,
    VkPipeline p)
{
    float cu[20];
    memcpy(cu, m, 64);
    cu[16] = alpha_ref, cu[17] = cu[18] = cu[19] = 0.0f;
    c->bi[0] = ring_put(cu, sizeof cu), c->bi[1] = inst, c->bi[2] = states;
    c->alpha_ref = alpha_ref, c->pipe = c->topo = -1, c->drawn = 0;
    if (p)
        vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
}

static void cast_topo(Cast* c, uint32_t prim)
{
    int strip = prim == GFX_TRIANGLESTRIP;
    if (strip != c->topo)
        vkCmdSetPrimitiveTopology(g_cb, strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST), c->topo = strip;
}

/* a zone caster: alpha tested with tex (and samp) where there is one, else depth alone */
static void cast_zone(Cast* c, VkBuffer vb, VkImageView tex, VkSampler samp, uint32_t inst, uint32_t first, uint32_t count,
    uint32_t prim)
{
    VkCommandBuffer cb = g_cb;
    int at = tex != VK_NULL_HANDLE;
    if (at != c->pipe)
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_shd.zone[at]), c->pipe = at;
    cast_topo(c, prim);
    VkDescriptorBufferInfo vbi = { vb, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo ii = { samp, tex, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[4];
    desc_write(&w[0], W_FRAME, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &c->bi[0], NULL);
    desc_write(&w[1], W_INST, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &c->bi[1], NULL);
    desc_write(&w[2], W_VB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &vbi, NULL);
    if (at)
        desc_write(&w[3], W_TEX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &ii);
    p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_world.layout, 0, at ? 4 : 3, w);
    vkCmdPushConstants(cb, g_world.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &inst);
    vkCmdDraw(cb, vertex_count(prim, count), 1, first, 0);
    ++c->drawn;
}

/* a character caster (g_shd.chars bound by cast_begin) */
static void cast_char(Cast* c, const ShItem* it)
{
    VkCommandBuffer cb = g_cb;
    cast_topo(c, it->prim);
    VkDescriptorBufferInfo vbi = { it->vb, 0, VK_WHOLE_SIZE }, ubi = { it->uv, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo ii = { it->samp, it->tex, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[6];
    desc_write(&w[0], W_FRAME, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &c->bi[0], NULL);
    desc_write(&w[1], W_INST, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &c->bi[1], NULL);
    desc_write(&w[2], W_LIGHTS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &c->bi[2], NULL);
    desc_write(&w[3], W_VB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &vbi, NULL);
    desc_write(&w[4], W_UV, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &ubi, NULL);
    desc_write(&w[5], W_TEX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &ii);
    p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_world.layout, 0, 6, w);
    struct
    {
        float params[4];
        uint32_t palette, state, mirror, pad;
    } pc = { { c->alpha_ref, 0.0f, 0.0f, 0.0f }, it->palette, it->state, it->mirror, 0 };
    vkCmdPushConstants(cb, g_world.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdBindIndexBuffer(cb, it->ib, 0, it->index_size == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cb, vertex_count(it->prim, it->count), 1, it->first, 0, 0);
    ++c->drawn;
}

/* one cascade of the zone's layer drawn again */
static uint32_t shadow_zone(const GfxShadowFrame* f)
{
    int ts = shadow_pass_begin(g_shd.zmap, (uint32_t)f->redraw, GFX_PASS_SHADOW_ZONE, 0);
    Cast c = { { { 0 } } };
    if (f->nitem && f->nworld)
    {
        VkDescriptorBufferInfo none = { 0 };
        cast_begin(&c, f->c[f->redraw].lmat, 0.0f, ring_put(f->world, (size_t)f->nworld * 64), none, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < f->nitem; ++i)
        {
            const GfxWorldItem* it = &f->item[i];
            if (!it->vb || !it->count || it->inst >= f->nworld)
                continue;
            int at = (it->flags & GFX_WORLD_ATEST) && it->tex && it->tex->type == GFX_TEX_2D;
            if (at)
                it->tex->used = g_serial, it->tex->rec = g_cb_index;
            it->vb->used = g_serial;
            cast_zone(&c, it->vb->b, at ? it->tex->view : VK_NULL_HANDLE, at ? sampler(&it->samp) : VK_NULL_HANDLE, it->inst,
                it->first, it->count, it->prim);
        }
    }
    pass_close(ts);
    g_cmd_draws += c.drawn;
    return c.drawn;
}

/* characters into a characters' map (tex): the frame's taken items, each into the layers of mats. reach NULL: the one
 * layer, the characters whose place is within two units of its square; else a layer each cascade, a character into
 * the one its place (its first bone) from the camera falls in and the next (the cascades blend over their last tenth),
 * none beyond scene_shadow_char_far. A pass a layer (Metal's one layered pass, without a layer from the vertex
 * function). pal, st: the frame's palette and states in the ring */
static void shadow_chars_pass(const GfxCharFrame* cf, VkDescriptorBufferInfo pal, VkDescriptorBufferInfo st, const float* cam,
    GfxTex* tex, const float (*mats)[16], uint32_t nmat, const float* reach)
{
    const float* M = mats[0];
    float lim = 1.0f + 2.0f * sqrtf(M[0] * M[0] + M[4] * M[4] + M[8] * M[8]); /* the square and two units, in the layer */
    float far2 = g_fxs.scene_shadow_char_far * g_fxs.scene_shadow_char_far;
    for (uint32_t layer = 0; layer < nmat; ++layer)
    {
        int ts = shadow_pass_begin(tex, layer, GFX_PASS_SHADOW_CHARS, layer > 0);
        Cast c;
        cast_begin(&c, mats[layer], cf->alpha_ref, pal, st, g_shd.chars);
        for (uint32_t i = 0; i < g_shd.nitems; ++i)
        {
            const ShItem* it = &g_shd.items[i];
            const float* r = it->at;
            if (!reach)
            {
                if (fabsf(r[0] * M[0] + r[1] * M[4] + r[2] * M[8] + M[12]) > lim || fabsf(r[0] * M[1] + r[1] * M[5] + r[2] * M[9] + M[13]) > lim)
                    continue;
            }
            else
            {
                float dx = r[0] - cam[0], dy = r[1] - cam[1], dz = r[2] - cam[2], d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > far2)
                    continue;
                uint32_t base = 0;
                while (base < nmat && d2 > (reach[base] + 3.0f) * (reach[base] + 3.0f))
                    ++base;
                if (base >= nmat || layer < base || layer > base + 1)
                    continue;
            }
            cast_char(&c, it);
        }
        pass_close(ts);
        g_shd_drawn[!reach] += c.drawn;
        g_cmd_draws += c.drawn;
    }
}

/* a fitted layer round the near characters (their extent lo..hi in cascade 0's map, M): its square rounded up to a power
 * of two (kept in *hs until they outgrow it or fit in a third of it), its centre on whole texels, so it holds still while
 * the characters do; 0 when there are none */
static int shadow_fit(const float* M, const float lo[2], const float hi[2], float* hs_kept, float tm[16], float* ttx)
{
    if (!(lo[0] <= hi[0] && lo[1] <= hi[1]))
        return 0;
    float half0 = 1.0f / sqrtf(M[0] * M[0] + M[4] * M[4] + M[8] * M[8]); /* cascade 0's half size, world */
    float m = 1.5f / half0, minhs = 2.0f / half0; /* a unit and a half's margin (the places are hips and feet), two at least */
    float e = 0.5f * fmaxf(hi[0] - lo[0], hi[1] - lo[1]) + m;
    float hs = *hs_kept > 0.0f ? *hs_kept : 1.0f;
    if (e > hs || e < hs * (1.0f / 3.0f))
        hs = exp2f(ceilf(log2f(fmaxf(e, minhs))));
    hs = hs > 1.0f ? 1.0f : hs < minhs ? minhs : hs;
    *hs_kept = hs;
    float tq = 2.0f * hs / (float)g_shd.fsize; /* a texel, in cascade 0's map */
    float cx = roundf(0.5f * (lo[0] + hi[0]) / tq) * tq, cy = roundf(0.5f * (lo[1] + hi[1]) / tq) * tq;
    memcpy(tm, M, 64);
    for (int r = 0; r < 4; ++r)
        tm[4 * r] = M[4 * r] / hs, tm[4 * r + 1] = M[4 * r + 1] / hs;
    tm[12] = (M[12] - cx) / hs, tm[13] = (M[13] - cy) / hs;
    *ttx = 2.0f * hs * half0 / (float)g_shd.fsize;
    return 1;
}

/* The characters' layers drawn at the frame's end, after all its draws, from all of the frame's characters: the first
 * cascades' (half size) and a fitted one round those near the camera. The next frame's zone reads them (its draws come
 * before any character is known), and so do its characters beyond the near ones, a frame late. */
static void shadow_frame_end(void)
{
    if (g_shd.lserial != g_serial || !g_shd.cmap || !g_shd.lnch)
        return;
    const GfxShadowFrame* f = &g_shd.lf;
    uint32_t n = g_shd.lnch < 2 ? g_shd.lnch : 2;
    float reach[2];
    for (uint32_t i = 0; i < n; ++i)
    {
        const float* C = f->c[i].lmat;
        memcpy(g_shd.lag.cm[i], C, 64);
        reach[i] = f->c[i].radius;
        float mi = 2.5f * sqrtf(C[0] * C[0] + C[4] * C[4] + C[8] * C[8]); /* two and a half units */
        g_shd.lag.cbox[i][0] = g_shd.cbox[i][0] - mi, g_shd.lag.cbox[i][1] = g_shd.cbox[i][1] - mi;
        g_shd.lag.cbox[i][2] = g_shd.cbox[i][2] + mi, g_shd.lag.cbox[i][3] = g_shd.cbox[i][3] + mi;
    }
    g_shd.lag.n = n, g_shd.lag.ttx = 0.0f;
    if (g_fxs.scene_shadow_fit > 0.0f)
        shadow_fit(f->c[0].lmat, g_shd.near_lo, g_shd.near_hi, &g_shd.lag.chs, g_shd.lag.tm, &g_shd.lag.ttx);
    /* where each part is, for the next frame's lookups */
    uint32_t cap = 64;
    while (cap < 2 * g_shd.nitems)
        cap *= 2;
    if (cap > g_shd.lag.cap)
    {
        free(g_shd.lag.key), free(g_shd.lag.at);
        g_shd.lag.key = (uintptr_t*)malloc(cap * sizeof *g_shd.lag.key), g_shd.lag.at = (float (*)[3])malloc(cap * sizeof *g_shd.lag.at);
        g_shd.lag.cap = cap;
    }
    memset(g_shd.lag.key, 0, g_shd.lag.cap * sizeof *g_shd.lag.key);
    for (uint32_t i = 0; i < g_shd.nitems; ++i)
    {
        uintptr_t k = g_shd.items[i].key;
        uint32_t h = char_hash(k) & (g_shd.lag.cap - 1);
        while (g_shd.lag.key[h] && g_shd.lag.key[h] != k)
            h = (h + 1) & (g_shd.lag.cap - 1);
        g_shd.lag.key[h] = k, memcpy(g_shd.lag.at[h], g_shd.items[i].at, 12);
    }
    cmd();
    const GfxCharFrame* cf = &g_shd.lc;
    VkDescriptorBufferInfo pal = ring_put(cf->palette, (size_t)cf->nbones * 64);
    VkDescriptorBufferInfo st = ring_put(cf->state, (size_t)cf->nstate * sizeof(GfxCharState));
    shadow_chars_pass(cf, pal, st, f->cam, g_shd.cmap, (const float (*)[16])g_shd.lag.cm, n, reach);
    if (g_shd.lag.ttx > 0.0f)
        shadow_chars_pass(cf, pal, st, f->cam, g_shd.cmt, (const float (*)[16])g_shd.lag.tm, 1, NULL);
    lamp_shadows_draw(cf, pal, st);
    lamp_zone_cubes();
    g_shd.lag.serial = g_serial;
    g_shd.lserial = 0;
    g_shd.nitems = 0; /* (the handles were the frame's: the next takes its own) */
}

void gfx_shadow_begin(const GfxShadowFrame* f)
{
    g_shd.on = 0;
    if (!g_dev || !f || f->dir[3] <= 0.0f || !f->size || !f->ncascades)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    if (!shadow_init(f->size))
        return;
    ShU* u = &g_shd.u;
    memset(u, 0, sizeof *u);
    uint32_t nc = f->ncascades < GFX_SHADOW_CASCADES ? f->ncascades : GFX_SHADOW_CASCADES;
    for (uint32_t i = 0; i < nc; ++i)
    {
        memcpy(u->m[i], f->c[i].lmat, 64);
        u->cas[i][0] = f->c[i].radius, u->cas[i][1] = f->c[i].texel, u->cas[i][2] = f->c[i].depth;
        u->cas[i][3] = f->c[i].valid ? 1.0f : 0.0f;
    }
    memcpy(u->dir, f->dir, 16), memcpy(u->cam, f->cam, 16);
    uint32_t nch = f->chars && f->chars->nitem && f->chars->nbones && f->c[0].valid ? (f->nchar < nc ? f->nchar : nc) : 0;
    /* The characters' shadows. A frame has a batch for each run of the game's draws between the scene renderer's (a
     * crowd: several), and the zone's come before any character is known: the characters' layers are drawn at the
     * frame's end from all of them (shadow_frame_end) and read by the next frame - the zone a frame late, the
     * characters each as it stood then (char_lag). Here each batch's new characters are taken. */
    int first = g_shd.cframe != g_serial;
    if (first || (f->chars && f->chars->nitem < g_shd.cdone))
    {
        g_shd.near_lo[0] = g_shd.near_lo[1] = 1e30f, g_shd.near_hi[0] = g_shd.near_hi[1] = -1e30f;
        for (int i = 0; i < GFX_SHADOW_CASCADES; ++i)
            g_shd.cbox[i][0] = g_shd.cbox[i][1] = 1e30f, g_shd.cbox[i][2] = g_shd.cbox[i][3] = -1e30f;
        g_shd.cdone = 0;
        g_shd.nitems = 0;
    }
    g_shd.cframe = g_serial;
    g_shd.freach = fminf(f->c[0].radius, fmaxf(g_fxs.scene_shadow_fit_reach, 0.0f));
    uint32_t from = g_shd.nitems;
    if (nch)
    {
        float reach2 = g_shd.freach * g_shd.freach;
        /* this batch's characters into the boxes: the near ones' extent, each cascade's box (by their places; the
         * margins take the rest of them) */
        shadow_items_add(f->chars, g_shd.cdone);
        for (uint32_t k = from; k < g_shd.nitems; ++k)
        {
            const float* p = g_shd.items[k].at;
            float dx = p[0] - f->cam[0], dy = p[1] - f->cam[1], dz = p[2] - f->cam[2];
            int near = dx * dx + dy * dy + dz * dz <= reach2;
            for (uint32_t i = 0; i < nch; ++i)
            {
                const float* C = f->c[i].lmat;
                for (int a = 0; a < 2; ++a)
                {
                    float q = p[0] * C[a] + p[1] * C[4 + a] + p[2] * C[8 + a] + C[12 + a];
                    g_shd.cbox[i][a] = q < g_shd.cbox[i][a] ? q : g_shd.cbox[i][a];
                    g_shd.cbox[i][2 + a] = q > g_shd.cbox[i][2 + a] ? q : g_shd.cbox[i][2 + a];
                    if (i == 0 && near)
                        g_shd.near_lo[a] = q < g_shd.near_lo[a] ? q : g_shd.near_lo[a],
                        g_shd.near_hi[a] = q > g_shd.near_hi[a] ? q : g_shd.near_hi[a];
                }
            }
        }
        g_shd.cdone = f->chars->nitem;
        g_shd.lf = *f, g_shd.lc = *f->chars, g_shd.lf.chars = &g_shd.lc, g_shd.lnch = nch, g_shd.lserial = g_serial;
    }
    if (f->redraw >= 0 && f->redraw < (int32_t)nc)
        shadow_zone(f); /* (between the game's draws: its pass is taken up again after) */
    /* the characters' layers they read: last frame's, when it drew them */
    if (g_shd.lag.serial && g_shd.lag.serial + 1 == g_serial)
    {
        memcpy(u->cm[0], g_shd.lag.tm, 64);
        for (uint32_t i = 0; i < g_shd.lag.n; ++i)
            memcpy(u->cm[1 + i], g_shd.lag.cm[i], 64), memcpy(u->cbox[i], g_shd.lag.cbox[i], 16);
        u->info[1] = (float)g_shd.lag.n, u->ctx[0] = g_shd.lag.ttx;
    }
    u->ctx[1] = 1.0f / (float)g_shd.fsize, u->ctx[2] = f->soft > 1.0f ? f->soft : 1.0f;
    u->ctx[3] = fmaxf(g_fxs.shadow_fill, 0.0f);
    int hq = g_fxs.shadow_soft_q >= 1.5f;
    u->pk[0] = f->soft_tan, u->pk[1] = hq ? 4.0f : 2.0f, u->pk[2] = hq ? 16.0f : 8.0f, u->pk[3] = 0.0f; /* gathers, compares */
    u->info[0] = (float)nc, u->info[2] = f->strength, u->info[3] = 1.0f / (float)f->size;
    g_shd.on = 1;
    if (gfx_profiling)
    {
        g_prof.draw_ns += gfx_now_ns() - t0;
        static uint64_t last;
        static uint32_t calls;
        ++calls;
        uint64_t now = gfx_now_ns();
        if (!last)
            last = now;
        if (now - last > 2000000000ull)
        {
            fprintf(stderr, "[recomp] gfx: scene shadows: %u batches, %.1f/%.1f character draws a batch into the cascades/fitted layer\n",
                calls, calls ? (double)g_shd_drawn[0] / calls : 0.0, calls ? (double)g_shd_drawn[1] / calls : 0.0);
            last = now, calls = 0, g_shd_drawn[0] = g_shd_drawn[1] = 0;
        }
    }
}

/* the shadows' bindings for a draw (6-11), their uniforms in bi: how many writes */
static uint32_t shadow_writes(VkWriteDescriptorSet* w, const VkDescriptorBufferInfo* bi, VkDescriptorImageInfo ii[5])
{
    ii[0] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, g_shd.zmap->view, VK_IMAGE_LAYOUT_GENERAL };
    ii[1] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, g_shd.cmap->view, VK_IMAGE_LAYOUT_GENERAL };
    ii[2] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, g_shd.cmt->view, VK_IMAGE_LAYOUT_GENERAL };
    ii[3] = (VkDescriptorImageInfo){ g_cmp_samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL };
    ii[4] = (VkDescriptorImageInfo){ g_near_samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL };
    desc_write(&w[0], W_SH, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bi, NULL);
    desc_write(&w[1], W_ZM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NULL, &ii[0]);
    desc_write(&w[2], W_CM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NULL, &ii[1]);
    desc_write(&w[3], W_CT, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NULL, &ii[2]);
    desc_write(&w[4], W_CS, VK_DESCRIPTOR_TYPE_SAMPLER, NULL, &ii[3]);
    desc_write(&w[5], W_PS, VK_DESCRIPTOR_TYPE_SAMPLER, NULL, &ii[4]);
    g_shd.zmap->used = g_shd.cmap->used = g_shd.cmt->used = g_serial;
    return 6;
}

/* the shadows' uniforms into the ring, when the batch is shadowed */
static int shadow_uniforms(VkDescriptorBufferInfo* bi)
{
    if (!g_shd.on)
        return 0;
    static RingSame k;
    *bi = ring_same(&k, &g_shd.u, sizeof g_shd.u);
    return 1;
}

/* A camera cut (gfx_metal.m's cam_cut_note, its note there): the view jumped (first person and back, a cutscene, a
 * warp) - the camera moved more than two units or turned more than 25 degrees since the last frame that had a view.
 * The scene renderer's occlusion starts afresh rather than show the last view's for a frame. */
static uint64_t g_cam_cut;  /* the last frame found to be a cut */
static uint64_t g_cam_seen; /* the last frame a view was noted */
static float g_cam_last[16];
static void cam_cut_note(const float* view)
{
    if (g_cam_seen == g_serial)
        return;
    int cut = !g_cam_seen || g_cam_seen + 1 < g_serial;
    if (!cut)
    {
        float a[16], b[16];
        if (gfx_mat_inverse(a, view) && gfx_mat_inverse(b, g_cam_last))
        {
            float dx = a[12] - b[12], dy = a[13] - b[13], dz = a[14] - b[14];
            float f = a[8] * b[8] + a[9] * b[9] + a[10] * b[10]; /* the view's z axes' cosine */
            cut = dx * dx + dy * dy + dz * dz > 4.0f || f < 0.906f;
        }
    }
    if (cut)
        g_cam_cut = g_serial;
    memcpy(g_cam_last, view, 64);
    g_cam_seen = g_serial;
}
static int cam_cut(void) { return g_cam_cut == g_serial; }

/* --- the scene renderer's bounce light: its state (gfx_gi_zone, gfx_gi_frame and the kernels further on, by the ray
 * tracing's helpers), and what the zone's and characters' draws bind of it ---------------------------------------- */
#define GI_IRR 6 /* texels across a probe's irradiance map, inside its border */
#define GI_DIS 14 /* its distance map's */

/* GU of GI_GLSL */
typedef struct GiU
{
    float org[4];     /* x 1: looked up per pixel (scene 2), y 1: the shadows' zone layer is there, w: world units between
                       * probes */
    int32_t base[4];  /* the grid's lowest cell (world cells); w 1 when the grid is there */
    int32_t dims[4];  /* cells each way; w all of them */
    float k[4];       /* strength, the sky's share's hold on the ambient, debug, the shading's normal offset */
    float inv[4];     /* 1 / the atlases' sizes: irradiance w, h, distance w, h */
    float tr[4];      /* the rays' reach, hysteresis, the earlier bounces' weight, the ambient's weight at a hit */
    uint32_t n[4];    /* rays a probe, 1 probe in this many traced an update, this update's phase, lamps */
    float sun_dir[4], sun_col[4], sun2_dir[4], sun2_col[4], amb[4];
    float rot[16];    /* this update's rays' rotation (columns) */
} GiU;

static struct
{
    int tried, ok;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline flatten, scroll, trace, birr, bdis;
    VkQueryPool csize; /* the zone's structure's compacted size */
    VkSampler rep;      /* the zone's textures' as they are flattened (repeated, mips) */
    GfxTex* nozm;       /* bound for the shadows' zone layer when there is none */
    /* the zone: its triangles' colours and normals, one structure over them, one instance of it at the top */
    RtBuf attr, blas_buf, tlas_buf, inst;
    VkAccelerationStructureKHR blas, tlas;
    uint32_t ntri;
    /* the probes: the irradiance's and the distance's atlases, their states, cells, and an update's rays */
    GfxTex *irr, *dis;
    RtBuf ps, pc, rb;
    int clear;        /* ps and pc to be zeroed before the next update (every probe afresh) */
    uint32_t quality;
    uint32_t failed;  /* the qualities whose atlases or buffers couldn't be made (a bit each): not tried again */
    uint64_t updated; /* the frame they were last traced */
    uint32_t phase;
    uint32_t seed;
    int on;           /* this batch's zone and characters take the bounce light */
    GiU u;
} g_gi;

/* the bounce light for a batch's draws: its uniforms into the ring (*bi); 0 off */
static int gi_uniforms(VkDescriptorBufferInfo* bi)
{
    if (!g_gi.on)
        return 0;
    static RingSame k;
    *bi = ring_same(&k, &g_gi.u, sizeof g_gi.u);
    return 1;
}

/* its bindings (W_GU..W_GID): bi[0] its uniforms, bi[1] set here (the probes' states); how many writes */
static uint32_t gi_writes(VkWriteDescriptorSet* w, VkDescriptorBufferInfo bi[2], VkDescriptorImageInfo ii[2])
{
    bi[1] = (VkDescriptorBufferInfo){ g_gi.ps.b, 0, VK_WHOLE_SIZE };
    ii[0] = (VkDescriptorImageInfo){ g_lin_samp, g_gi.irr->view, VK_IMAGE_LAYOUT_GENERAL };
    ii[1] = (VkDescriptorImageInfo){ g_lin_samp, g_gi.dis->view, VK_IMAGE_LAYOUT_GENERAL };
    desc_write(&w[0], W_GU, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bi[0], NULL);
    desc_write(&w[1], W_GPS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[1], NULL);
    desc_write(&w[2], W_GIR, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &ii[0]);
    desc_write(&w[3], W_GID, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &ii[1]);
    g_gi.irr->used = g_gi.dis->used = g_serial;
    return 4;
}

/* --- the scene renderer's ambient occlusion (scene_ao): gfx_ao_metal.inc's, on Vulkan (its notes there) -------------
 * Made once a frame at half size when the scene is done (gfx_scene_done, ahead of the effects), from the depth the
 * zone and characters were drawn with: SAO_PREP the nearest of each 2x2 as distance along the view (0 the sky),
 * SAO_GTAO ground-truth occlusion, SAO_RESOLVE a 3x3 blur that keeps to its own depth blended over frames with the last
 * result reprojected (the result keeps its distance and count beside it). Full-screen passes into float targets as
 * the effects' are (fx_pass), where Metal has compute kernels. The zone and characters read the last frame's result
 * (AO_GLSL). The rays (scene_ao_q 3, 4: SAO_RAYS) trace the bounce light's structure (built while scene_gi or they are
 * on); without it, or without ray queries, GTAO high instead, as on Metal. */
static const char SAO_GLSL[] =
    /* vp: the viewport in the target (x, y, w, h); hs: the half size and its inverse; pr: P00, P11, P20, P21 (D3D's
     * projection, row-major); pz: P22, P32, P23 (+-1), the depth's far end; prm: the reach, steps, slices, the reach's
     * largest size in half pixels; tp: the blend's least weight, whether there is a last result, the frame, the frames
     * it counts at most */
    "layout(std140, set = 0, binding = 0) uniform AoUB { vec4 vp, hs, pr, pz, prm, tp; mat4 iv, pvp; } U;\n"
    "layout(set = 0, binding = 1) uniform sampler2D t0;\n"
    "layout(set = 0, binding = 2) uniform sampler2D t1;\n"
    "layout(set = 0, binding = 3) uniform sampler2D t2;\n"
    "#ifdef SAO_RAYS\n" /* the bounce light's structure and its triangles' colours and normals (GI_KERNELS_GLSL) */
    "layout(set = 0, binding = 4) uniform accelerationStructureEXT gas;\n"
    "layout(std430, set = 0, binding = 5) readonly buffer ATB { uvec2 gattr[]; };\n"
    GI_OCT_GLSL
    "#endif\n"
    "layout(location = 0) out vec4 oc;\n"
    "float ao_ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }\n"
    "float ao_acos(float x) {\n" /* XeGTAO's, within some 0.001 */
    "  float r = (-0.156583 * abs(x) + 1.5707963) * sqrt(clamp(1.0 - abs(x), 0.0, 1.0));\n"
    "  return x >= 0.0 ? r : 3.14159265 - r;\n"
    "}\n"
    /* half pixel h (its corner) at distance z, in the view: x right, y up, z the distance */
    "vec3 ao_vpos(vec2 h, float z) {\n"
    "  vec2 ndc = vec2((h.x + 0.5) * 2.0 * U.hs.z - 1.0, 1.0 - (h.y + 0.5) * 2.0 * U.hs.w);\n"
    "  float vz = z * U.pz.z;\n"
    "  return vec3((ndc.x * z - vz * U.pr.z) / U.pr.x, (ndc.y * z - vz * U.pr.w) / U.pr.y, z);\n"
    "}\n"
    "vec3 ao_world(vec3 P) { return (U.iv * vec4(P.x, P.y, P.z * U.pz.z, 1.0)).xyz; }\n"
    "float hz(ivec2 q) { return texelFetch(t0, q, 0).x; }\n"
    /* the normal from the neighbours, each way the side whose depth is nearer this one's (edges stay sharp); toward the
     * eye */
    "vec3 ao_normal(ivec2 p, vec3 P) {\n"
    "  ivec2 hi = ivec2(U.hs.xy) - 1;\n"
    "  float zl = hz(clamp(p - ivec2(1, 0), ivec2(0), hi)), zr = hz(clamp(p + ivec2(1, 0), ivec2(0), hi));\n"
    "  float zu = hz(clamp(p - ivec2(0, 1), ivec2(0), hi)), zd = hz(clamp(p + ivec2(0, 1), ivec2(0), hi));\n"
    "  bool r = zr > 0.0 && (zl <= 0.0 || abs(zr - P.z) < abs(zl - P.z));\n"
    "  bool d = zd > 0.0 && (zu <= 0.0 || abs(zd - P.z) < abs(zu - P.z));\n"
    "  vec2 h = vec2(p);\n"
    "  vec3 dx = r ? ao_vpos(h + vec2(1, 0), zr) - P : (zl > 0.0 ? P - ao_vpos(h - vec2(1, 0), zl) : vec3(1, 0, 0));\n"
    "  vec3 dy = d ? ao_vpos(h + vec2(0, 1), zd) - P : (zu > 0.0 ? P - ao_vpos(h - vec2(0, 1), zu) : vec3(0, 1, 0));\n"
    "  vec3 n = normalize(cross(dx, dy));\n"
    "  return dot(n, P) > 0.0 ? -n : n;\n"
    "}\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "#if defined(SAO_PREP)\n" /* t0: the depth */
    "  ivec2 b = ivec2(U.vp.xy + floor(vec2(p) * U.vp.zw * U.hs.zw)), hi = textureSize(t0, 0) - 1;\n"
    "  float z = 1e30;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    float d = texelFetch(t0, min(b + ivec2(k & 1, k >> 1), hi), 0).x;\n"
    "    if (d >= U.pz.w) continue;\n" /* the sky, or nothing drawn */
    "    float vz = U.pz.y / (d * U.pz.z - U.pz.x);\n"
    "    z = min(z, vz * U.pz.z);\n"
    "  }\n"
    "  oc = vec4(z < 1e29 && z > 0.0 ? z : 0.0);\n"
    "#elif defined(SAO_GTAO)\n" /* t0: the half-size distances */
    "  float z = hz(p);\n"
    /* the reach in texels each way (the texels need not be square on screen), held to prm.w texels near the eye */
    "  float R = U.prm.x;\n"
    "  vec2 rpx = R / max(z, 1e-3) * vec2(U.pr.x, U.pr.y) * 0.5 * U.hs.xy;\n"
    "  float big = max(rpx.x, rpx.y);\n"
    "  if (z <= 0.0 || big < 1.0) { oc = vec4(1.0); return; }\n"
    "  if (big > U.prm.w) { float k = U.prm.w / big; rpx *= k, R *= k; }\n"
    "  vec2 px1 = rpx / max(rpx.x, rpx.y);\n" /* a texel's step, the long way */
    "  vec3 P = ao_vpos(vec2(p), z), N = ao_normal(p, P), V = normalize(-P);\n"
    "  int steps = int(U.prm.y), slices = int(U.prm.z);\n"
    "  float fr = U.tp.z, n0 = ao_ign(vec2(p) + 5.588238 * fr), n1 = ao_ign(vec2(p) + vec2(37.0, 17.0) + 5.588238 * fr);\n"
    "  vec2 hi = U.hs.xy - 1.0;\n"
    "  float vis = 0.0;\n"
    "  for (int s = 0; s < slices; ++s) {\n"
    "    float phi = (float(s) + n0) * (3.14159265 / float(slices));\n"
    "    vec2 om = vec2(cos(phi), -sin(phi));\n"
    "    vec3 dv = vec3(cos(phi), sin(phi), 0.0), od = dv - dot(dv, V) * V, ax = normalize(cross(od, V));\n"
    "    vec3 pn = N - ax * dot(N, ax);\n"
    "    float pl = length(pn);\n"
    "    float cn = clamp(dot(pn, V) / max(pl, 1e-4), 0.0, 1.0), n = (dot(od, pn) >= 0.0 ? 1.0 : -1.0) * ao_acos(cn);\n"
    "    float lo0 = cos(n + 1.5707963), lo1 = cos(n - 1.5707963), h0c = lo0, h1c = lo1;\n"
    "    for (int j = 0; j < steps; ++j) {\n"
    "      float t = (float(j) + fract(n1 + float(j) * 0.618034)) / float(steps);\n"
    "      vec2 off = om * (t * t * rpx + px1);\n"
    "      for (int side = 0; side < 2; ++side) {\n"
    "        vec2 q = vec2(p) + (side == 0 ? off : -off);\n"
    "        if (any(lessThan(q, vec2(0.0))) || any(greaterThan(q, hi))) continue;\n"
    "        float zq = hz(ivec2(q + 0.5));\n"
    "        if (zq <= 0.0) continue;\n"
    "        vec3 dd = ao_vpos(floor(q + 0.5), zq) - P;\n"
    "        float l = length(dd);\n"
    "        if (l < 1e-4) continue;\n"
    "        float w = clamp((R - l) / (0.4 * R), 0.0, 1.0);\n"
    "        float c = dot(dd, V) / l;\n"
    "        if (side == 0) h0c = max(h0c, mix(lo0, c, w)); else h1c = max(h1c, mix(lo1, c, w));\n"
    "      }\n"
    "    }\n"
    "    float h0 = -ao_acos(clamp(h1c, -1.0, 1.0)), h1 = ao_acos(clamp(h0c, -1.0, 1.0));\n"
    "    h0 = n + clamp(h0 - n, -1.5707963, 1.5707963), h1 = n + clamp(h1 - n, -1.5707963, 1.5707963);\n"
    "    float sn = sin(n);\n"
    "    vis += pl * ((cn + 2.0 * h0 * sn - cos(2.0 * h0 - n)) + (cn + 2.0 * h1 * sn - cos(2.0 * h1 - n))) * 0.25;\n"
    "  }\n"
    "  oc = vec4(clamp(vis / float(slices), 0.0, 1.0));\n"
    "#elif defined(SAO_RAYS)\n" /* t0: the half-size distances; rays against the bounce light's structure */
    "  float z = hz(p);\n"
    "  if (z <= 0.0) { oc = vec4(1.0); return; }\n"
    "  vec3 P = ao_vpos(vec2(p), z), N = ao_normal(p, P);\n"
    "  vec3 W = ao_world(P), Nw = normalize((U.iv * vec4(N.x, N.y, N.z * U.pz.z, 0.0)).xyz);\n"
    "  vec3 T = normalize(abs(Nw.y) < 0.99 ? cross(Nw, vec3(0.0, 1.0, 0.0)) : cross(Nw, vec3(1.0, 0.0, 0.0))), B = cross(Nw, T);\n"
    "  vec3 o = W + Nw * (0.03 + 0.002 * z);\n"
    "  float R = U.prm.x, fr = U.tp.z;\n"
    "  float n0 = ao_ign(vec2(p) + 5.588238 * fr), n1 = ao_ign(vec2(p) + vec2(37.0, 17.0) + 5.588238 * fr);\n"
    "  int nr = int(U.prm.z);\n"
    "  float occ = 0.0;\n"
    "  for (int r = 0; r < nr; ++r) {\n"
    "    float u1 = fract(n0 + float(r) / float(nr)), u2 = fract(n1 + float(r) * 0.618034);\n" /* cosine-weighted */
    "    float sr = sqrt(u1), ph = 6.2831853 * u2;\n"
    "    vec3 d = normalize(T * (sr * cos(ph)) + B * (sr * sin(ph)) + Nw * sqrt(max(1.0 - u1, 0.0)));\n"
    "    rayQueryEXT q;\n"
    "    rayQueryInitializeEXT(q, gas, gl_RayFlagsOpaqueEXT, 0xFFu, o, 0.0, d, R);\n"
    "    while (rayQueryProceedEXT(q)) {}\n"
    "    if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionTriangleEXT) continue;\n"
    "    uvec2 at = gattr[rayQueryGetIntersectionPrimitiveIndexEXT(q, true)];\n"
    "    vec3 hn = gi_unoct(unpackUnorm2x16(at.y) * 2.0 - 1.0);\n"
    "    if (dot(hn, d) > 0.0 && (at.x >> 24) == 0u) continue;\n" /* a backface: the coarse model under the drawn one */
    "    occ += 1.0 - rayQueryGetIntersectionTEXT(q, true) / R;\n"
    "  }\n"
    "  oc = vec4(clamp(1.0 - occ / float(nr), 0.0, 1.0));\n"
    "#else\n" /* SAO_RESOLVE; t0 the distances, t1 this frame's occlusion, t2 the last result */
    "  float z = hz(p);\n"
    "  if (z <= 0.0) { oc = vec4(1.0, 0.0, 0.0, 0.0); return; }\n"
    "  ivec2 hi = ivec2(U.hs.xy) - 1;\n"
    "  float s = 0.0, ws = 0.0;\n"
    "  for (int y = -1; y <= 1; ++y)\n"
    "    for (int x = -1; x <= 1; ++x) {\n"
    "      ivec2 q = clamp(p + ivec2(x, y), ivec2(0), hi);\n"
    "      float zq = hz(q);\n"
    "      float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0) * clamp(1.0 - abs(zq - z) / (0.03 * z), 0.0, 1.0);\n"
    "      s += texelFetch(t1, q, 0).x * w, ws += w;\n"
    "    }\n"
    "  s = ws > 0.0 ? s / ws : texelFetch(t1, p, 0).x;\n"
    "  float a = s, cnt = 1.0;\n"
    "  if (U.tp.y > 0.5) {\n"
    "    vec4 pc = U.pvp * vec4(ao_world(ao_vpos(vec2(p), z)), 1.0);\n"
    "    if (pc.w > 1e-4) {\n"
    "      vec2 uv = vec2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
    "      vec2 t = uv * U.hs.xy - 0.5, f = fract(t);\n"
    "      ivec2 b = ivec2(floor(t));\n"
    "      vec4 hv = vec4(0.0);\n"
    "      float hw = 0.0;\n"
    "      for (int k = 0; k < 4; ++k) {\n"
    "        ivec2 o = b + ivec2(k & 1, k >> 1);\n"
    "        if (any(lessThan(o, ivec2(0))) || any(greaterThan(o, hi))) continue;\n"
    "        vec4 h = texelFetch(t2, o, 0);\n"
    "        float w = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y) * clamp(1.0 - abs(h.y - pc.w) / (0.05 * pc.w), 0.0, 1.0);\n"
    "        hv += h * w, hw += w;\n"
    "      }\n"
    "      if (hw > 0.05) {\n"
    "        hv /= hw;\n"
    "        cnt = min(hv.z + 1.0, U.tp.w);\n"
    "        a = mix(hv.x, s, max(1.0 / cnt, U.tp.x));\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  oc = vec4(a, z, cnt, 0.0);\n"
    "#endif\n"
    "}\n";

/* AoUB of SAO_GLSL */
typedef struct AoU
{
    float vp[4], hs[4], pr[4], pz[4], prm[4], tp[4];
    float iv[16], pvp[16];
} AoU;

/* AUB of AO_GLSL */
typedef struct AoShadeU
{
    float pvp[16];
    float vp[4];
    float k[4];
} AoShadeU;

static struct
{
    int tried, ok;
    VkPipeline prep, gtao, resolve;
    /* the rays (SAO_RAYS): their layout (the effects' four, the structure and the triangles), made on first use */
    int rtried;
    VkDescriptorSetLayout rdsl;
    VkPipelineLayout rlayout;
    VkPipeline rays;
    GfxTex *hz, *raw, *hist[2];
    int cur; /* hist[cur] is the last result */
    uint32_t hw, hh;
    uint32_t frame;
    /* the zone's draw this frame: its depth, target, camera and viewport (ao_note) */
    GfxTex *depth, *rt;
    uint64_t noted;
    float view[16], proj[16];
    uint32_t vp[4];
    /* the last result's camera (row-major view x projection) and viewport; made: its frame */
    float made_vp[16];
    uint32_t made_view[4];
    uint64_t made;
    AoShadeU su;
} g_sao;

static void sao_forget(const GfxTex* t)
{
    if (g_sao.depth == t || g_sao.rt == t)
        g_sao.depth = g_sao.rt = NULL, g_sao.noted = 0, g_sao.made = 0;
}

/* one of SAO_GLSL's passes, on the effects' layout (its uniforms, then samplers; the rays' own) and full-screen
 * triangle */
static VkPipeline sao_pipeline(const char* name, VkFormat fmt)
{
    int rays = !strcmp(name, "SAO_RAYS");
    char d[96];
    snprintf(d, sizeof d, "%s#define %s\n", rays ? "#extension GL_EXT_ray_query : require\n" : "", name);
    return fs_pipeline(g_fx.vs, d, SAO_GLSL, rays ? g_sao.rlayout : g_fx_layout, fmt, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
}

static int sao_init(void)
{
    if (g_sao.tried)
        return g_sao.ok;
    g_sao.tried = 1;
    if (!fx_init())
        return 0;
    g_sao.prep = sao_pipeline("SAO_PREP", VK_FORMAT_R32_SFLOAT);
    g_sao.gtao = sao_pipeline("SAO_GTAO", VK_FORMAT_R16_SFLOAT);
    g_sao.resolve = sao_pipeline("SAO_RESOLVE", VK_FORMAT_R16G16B16A16_SFLOAT);
    g_sao.ok = g_sao.prep && g_sao.gtao && g_sao.resolve;
    if (!g_sao.ok)
    {
        atomic_fetch_add(&g_failures, 1);
        fprintf(stderr, "[recomp] gfx: scene occlusion: its passes failed\n");
    }
    return g_sao.ok;
}

/* the rays' pass (SAO_RAYS), made on first use where there are ray queries: 1 when it is there */
static int sao_rays_init(void)
{
    if (g_sao.rtried)
        return g_sao.rays != VK_NULL_HANDLE;
    g_sao.rtried = 1;
    if (!g_has_rt)
        return 0;
    VkDescriptorSetLayoutBinding b[6];
    b[0] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (uint32_t i = 1; i < 4; ++i)
        b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[4] = (VkDescriptorSetLayoutBinding){ 4, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[5] = (VkDescriptorSetLayoutBinding){ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    push_layout(b, 6, NULL, &g_sao.rdsl, &g_sao.rlayout);
    g_sao.rays = g_sao.rlayout ? sao_pipeline("SAO_RAYS", VK_FORMAT_R16_SFLOAT) : VK_NULL_HANDLE;
    if (!g_sao.rays)
    {
        atomic_fetch_add(&g_failures, 1);
        fprintf(stderr, "[recomp] gfx: scene occlusion: its rays' pass failed\n");
    }
    return g_sao.rays != VK_NULL_HANDLE;
}

/* one full-screen triangle into all of target, reading in[0..n) as t0.. through the nearest sampler; timed as pass. The
 * rays' (p g_sao.rays) take the bounce light's structure and triangles too */
static void sao_pass(GfxTex* target, VkPipeline p, const VkDescriptorBufferInfo* ub, GfxTex* const* in, int n, int pass)
{
    int ts = pass_open(target, 0, 0, VK_ATTACHMENT_LOAD_OP_DONT_CARE, NULL, pass);
    VkCommandBuffer cb = g_cb;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkWriteDescriptorSet w[6];
    VkDescriptorImageInfo ii[3];
    desc_write(&w[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, ub, NULL);
    for (int i = 0; i < n; ++i)
    {
        ii[i] = (VkDescriptorImageInfo){ g_near_samp, in[i]->view, VK_IMAGE_LAYOUT_GENERAL };
        desc_write(&w[1 + i], 1 + (uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &ii[i]);
        in[i]->used = g_serial, in[i]->rec = g_cb_index;
    }
    uint32_t nw = 1 + (uint32_t)n;
    int rays = p == g_sao.rays;
    VkWriteDescriptorSetAccelerationStructureKHR wa;
    VkDescriptorBufferInfo abi = { g_gi.attr.b, 0, VK_WHOLE_SIZE };
    if (rays)
    {
        as_write(&w[nw++], 4, &wa, &g_gi.tlas);
        desc_write(&w[nw++], 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &abi, NULL);
    }
    p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rays ? g_sao.rlayout : g_fx_layout, 0, nw, w);
    vkCmdDraw(cb, 3, 1, 0, 0);
    pass_close(ts);
}

/* the zone's draw (gfx_world_draw): its depth and camera, the first this frame into the main target */
static void ao_note(const GfxWorldFrame* f, GfxTex* depth)
{
    if (g_fxs.scene_ao <= 0.0f || !depth || !g_rt || g_rt_face || g_rt_level || g_sao.noted == g_serial)
        return;
    g_sao.depth = depth, g_sao.rt = g_rt;
    memcpy(g_sao.view, f->view, 64), memcpy(g_sao.proj, f->proj, 64);
    uint32_t w = g_rt->w, h = g_rt->h;
    uint32_t x = f->vp[0] < w ? f->vp[0] : 0, y = f->vp[1] < h ? f->vp[1] : 0;
    uint32_t vw = f->vp[2] && x + f->vp[2] <= w ? f->vp[2] : w - x, vh = f->vp[3] && y + f->vp[3] <= h ? f->vp[3] : h - y;
    g_sao.vp[0] = x, g_sao.vp[1] = y, g_sao.vp[2] = vw, g_sao.vp[3] = vh;
    g_sao.noted = g_serial;
}

/* the scene done (gfx_scene_done): this frame's occlusion from the zone's depth, ahead of the effects. The quality's
 * method: 1 GTAO low (2 slices of 4 steps each way), 2 GTAO high (3 of 6), 3 rays low (1 a pixel), 4 rays high (2); the
 * rays GTAO high while the bounce light's structure is not there */
static void ao_make(GfxTex* color)
{
    if (g_fxs.scene_ao <= 0.0f || g_sao.noted != g_serial || !color || color != g_sao.rt || !g_sao.depth || !sao_init())
        return;
    flush_pass();
    /* half the viewport across, and down as many as keep the texels square on screen (the scene's target can be a
     * square stretched to the window: its projection says the window's shape) */
    const float* P = g_sao.proj;
    uint32_t hw = (g_sao.vp[2] + 1) / 2;
    float sq = P[5] != 0.0f ? fabsf(P[0] / P[5]) : 1.0f;
    uint32_t hh = (uint32_t)((float)hw * sq + 0.5f);
    if (hh > g_sao.vp[3])
        hh = g_sao.vp[3];
    if (hw < 8 || hh < 8)
        return;
    int fresh = !g_sao.hist[0] || g_sao.hw != hw || g_sao.hh != hh;
    if (!fx_tex(&g_sao.hz, VK_FORMAT_R32_SFLOAT, hw, hh, GFX_USE_RT, 1) || !fx_tex(&g_sao.raw, VK_FORMAT_R16_SFLOAT, hw, hh, GFX_USE_RT, 1) ||
        !fx_tex(&g_sao.hist[0], VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, GFX_USE_RT, 1) ||
        !fx_tex(&g_sao.hist[1], VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, GFX_USE_RT, 1))
        return;
    g_sao.hw = hw, g_sao.hh = hh;
    float vpm[16], iv[16];
    gfx_mat_mul(vpm, g_sao.view, g_sao.proj);
    if (!gfx_mat_inverse(iv, g_sao.view))
        return;
    AoU u;
    memset(&u, 0, sizeof u);
    u.vp[0] = (float)g_sao.vp[0], u.vp[1] = (float)g_sao.vp[1], u.vp[2] = (float)g_sao.vp[2], u.vp[3] = (float)g_sao.vp[3];
    u.hs[0] = (float)hw, u.hs[1] = (float)hh, u.hs[2] = 1.0f / (float)hw, u.hs[3] = 1.0f / (float)hh;
    u.pr[0] = P[0], u.pr[1] = P[5], u.pr[2] = P[8], u.pr[3] = P[9];
    u.pz[0] = P[10], u.pz[1] = P[14], u.pz[2] = P[11] < 0.0f ? -1.0f : 1.0f, u.pz[3] = 0.99999f;
    int q = g_fxs.scene_ao_q >= 3.5f ? 4 : g_fxs.scene_ao_q >= 2.5f ? 3 : g_fxs.scene_ao_q >= 1.5f ? 2 : 1;
    if (q >= 3 && !(g_gi.tlas && g_gi.attr.b && sao_rays_init()))
        q = 2;
    u.prm[0] = g_fxs.scene_ao_radius > 0.05f ? g_fxs.scene_ao_radius : 0.05f;
    u.prm[1] = q == 1 ? 4.0f : 6.0f;
    u.prm[2] = q == 1 ? 2.0f : q == 2 ? 3.0f : q == 3 ? 1.0f : 2.0f;
    u.prm[3] = 0.06f * (float)hw;
    cam_cut_note(g_sao.view);
    int same = !fresh && g_sao.made + 1 >= g_serial && !memcmp(g_sao.made_view, g_sao.vp, sizeof g_sao.vp) && !cam_cut();
    u.tp[0] = 0.08f, u.tp[1] = same ? 1.0f : 0.0f, u.tp[2] = (float)(g_sao.frame++ & 63), u.tp[3] = 16.0f;
    memcpy(u.iv, iv, 64);
    memcpy(u.pvp, g_sao.made_vp, 64);
    VkDescriptorBufferInfo ub = ring_put(&u, sizeof u);
    GfxTex* in[3] = { g_sao.depth, NULL, NULL };
    sao_pass(g_sao.hz, g_sao.prep, &ub, in, 1, GFX_PASS_SCENE_AO);
    in[0] = g_sao.hz;
    sao_pass(g_sao.raw, q >= 3 ? g_sao.rays : g_sao.gtao, &ub, in, 1, GFX_PASS_SCENE_AO_TRACE);
    int nx = g_sao.cur ^ 1;
    in[1] = g_sao.raw, in[2] = g_sao.hist[g_sao.cur];
    sao_pass(g_sao.hist[nx], g_sao.resolve, &ub, in, 3, GFX_PASS_SCENE_AO);
    g_sao.cur = nx;
    memcpy(g_sao.made_vp, vpm, 64);
    memcpy(g_sao.made_view, g_sao.vp, sizeof g_sao.vp);
    g_sao.made = g_serial;
    /* what the next frame's shading reads */
    AoShadeU* s = &g_sao.su;
    memcpy(s->pvp, vpm, 64);
    s->vp[0] = (float)g_sao.vp[2], s->vp[1] = (float)g_sao.vp[3];
    s->vp[2] = s->vp[3] = 1.0f; /* the texture is the viewport */
    static uint32_t told;
    if (++told % 3000 == 1)
        fprintf(stderr, "[recomp] gfx: scene occlusion: %s, %ux%u of a %ux%u viewport (projection %.3f %.3f), reach %.2f\n",
            q >= 3 ? (q == 4 ? "rays high" : "rays low") : (q == 2 ? "GTAO high" : "GTAO low"), hw, hh, g_sao.vp[2], g_sao.vp[3],
            (double)P[0], (double)P[5], (double)u.prm[0]);
}

/* the occlusion for a batch's draws: its uniforms into the ring (*bi); 0 off */
static int ao_uniforms(VkDescriptorBufferInfo* bi)
{
    if (g_fxs.scene_ao <= 0.0f || !g_sao.made || g_sao.made + 2 < g_serial || !g_sao.hist[g_sao.cur] || !g_rt || g_rt != g_sao.rt ||
        g_rt_face || g_rt_level)
        return 0;
    AoShadeU* s = &g_sao.su;
    s->k[0] = g_fxs.scene_ao, s->k[1] = g_fxs.scene_ao * g_fxs.scene_ao_chars, s->k[2] = g_fxs.scene_ao_debug, s->k[3] = 0.08f;
    static RingSame k;
    *bi = ring_same(&k, s, sizeof *s);
    return 1;
}

static uint32_t ao_writes(VkWriteDescriptorSet* w, const VkDescriptorBufferInfo* bi, VkDescriptorImageInfo* ii)
{
    GfxTex* t = g_sao.hist[g_sao.cur];
    *ii = (VkDescriptorImageInfo){ g_near_samp, t->view, VK_IMAGE_LAYOUT_GENERAL };
    desc_write(&w[0], W_AU, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bi, NULL);
    desc_write(&w[1], W_AOT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, ii);
    t->used = g_serial, t->rec = g_cb_index;
    return 2;
}

/* the highlights' settings for a batch's draws (*bi); 0 off (scene_spec 0, or no sun shadows to place them by) */
static int spec_uniforms(int can, VkDescriptorBufferInfo* bi)
{
    if (!can || g_fxs.scene_spec <= 0.0f)
        return 0;
    float k[8] = { g_fxs.scene_spec, fmaxf(g_fxs.scene_spec_lamps, 0.0f), fminf(fmaxf(g_fxs.scene_spec_wet, 0.0f), 1.0f),
        fmaxf(g_fxs.scene_spec_aa, 0.0f), g_rt && g_rt->vf == VK_FORMAT_R16G16B16A16_SFLOAT ? 1.0f : 0.0f, 0, 0, 0 };
    static RingSame keep;
    *bi = ring_same(&keep, k, sizeof k);
    return 1;
}

static void lamps_bin(const float* view, const float* proj, const uint32_t vp[6])
{
    float VP[16], iv[16];
    gfx_mat_mul(VP, view, proj);
    memcpy(g_lamps.vp, VP, 64);
    if (gfx_mat_inverse(iv, view))
        memcpy(g_lamps.cam, iv + 12, 12);
    float x0 = (float)vp[0], y0 = (float)vp[1], w = (float)vp[2], h = (float)vp[3];
    uint32_t tx = ((uint32_t)w + 63) / 64, ty = ((uint32_t)h + 63) / 64;
    if (!tx || !ty || tx * ty > 4096)
        return;
    float k = fmaxf(g_fxs.scene_lamps, 0.0f), far2 = g_fxs.scene_lamp_far * g_fxs.scene_lamp_far;
    g_lamps.lsz = (VkDeviceSize)g_lamps.n * 64 + 64, g_lamps.tsz = (VkDeviceSize)tx * ty * 64;
    float* L = (float*)ring(g_lamps.lsz, RING_ALIGN, &g_lamps.lb, &g_lamps.lo);
    static uint16_t T[4096 * 32]; /* binned here, then into the ring at once (its memory is for writing, not reading) */
    memset(T, 0, g_lamps.tsz);
    for (uint32_t i = 0; i < g_lamps.n; ++i)
    {
        const GfxWorldLight* l = &g_lamps.l[i];
        float* o = L + 16 * i;
        memcpy(o, l->pos, 16);
        for (int c = 0; c < 3; ++c)
            o[4 + c] = l->diffuse[c] * k, o[12 + c] = l->ambient[c] * k;
        o[7] = o[15] = 0.0f;
        memcpy(o + 8, l->att, 16);
        o[11] = 0.0f;
        if (g_lamps.sh_serial && g_lamps.sh_serial + 1 == g_serial)
            for (uint32_t s = 0; s < g_lamps.nsh; ++s)
            {
                float ex = l->pos[0] - g_lamps.shpos[s][0], ey = l->pos[1] - g_lamps.shpos[s][1], ez = l->pos[2] - g_lamps.shpos[s][2];
                if (ex * ex + ey * ey + ez * ez < 0.01f)
                    o[11] = (float)(s + 1);
            }
        float r = l->pos[3];
        float dx = l->pos[0] - g_lamps.cam[0], dy = l->pos[1] - g_lamps.cam[1], dz = l->pos[2] - g_lamps.cam[2];
        if (dx * dx + dy * dy + dz * dz > far2 + r * r + 2.0f * r * sqrtf(far2))
            continue;
        /* the lamp's sphere wholly behind the camera: none */
        {
            float vz = l->pos[0] * view[2] + l->pos[1] * view[6] + l->pos[2] * view[10] + view[14];
            float depth = proj[11] < 0.0f ? -vz : vz; /* along the view, toward what is seen */
            if (depth < -r)
                continue;
        }
        /* its box's corners on screen: the rectangle it can light (all of it when a corner is behind the camera) */
        float sx0 = 1e30f, sy0 = 1e30f, sx1 = -1e30f, sy1 = -1e30f;
        int behind = 0;
        for (int c = 0; c < 8; ++c)
        {
            float p[3] = { l->pos[0] + ((c & 1) ? r : -r), l->pos[1] + ((c & 2) ? r : -r), l->pos[2] + ((c & 4) ? r : -r) };
            float q[4];
            for (int j = 0; j < 4; ++j)
                q[j] = p[0] * VP[j] + p[1] * VP[4 + j] + p[2] * VP[8 + j] + VP[12 + j];
            if (q[3] <= 0.05f)
            {
                behind = 1;
                continue;
            }
            float sx = x0 + (q[0] / q[3] * 0.5f + 0.5f) * w, sy = y0 + (0.5f - q[1] / q[3] * 0.5f) * h;
            sx0 = fminf(sx0, sx), sy0 = fminf(sy0, sy), sx1 = fmaxf(sx1, sx), sy1 = fmaxf(sy1, sy);
        }
        int ax = 0, ay = 0, bx = (int)tx - 1, by = (int)ty - 1;
        if (!behind)
        {
            if (sx1 < x0 || sy1 < y0 || sx0 > x0 + w || sy0 > y0 + h)
                continue;
            ax = (int)fmaxf((sx0 - x0) / 64.0f, 0.0f), ay = (int)fmaxf((sy0 - y0) / 64.0f, 0.0f);
            bx = (int)fminf((sx1 - x0) / 64.0f, (float)(tx - 1)), by = (int)fminf((sy1 - y0) / 64.0f, (float)(ty - 1));
        }
        for (int y = ay; y <= by; ++y)
            for (int x = ax; x <= bx; ++x)
            {
                uint16_t* t = T + 32 * ((size_t)y * tx + x);
                if (t[0] < 31)
                    t[1 + t[0]++] = (uint16_t)i;
            }
    }
    memcpy(ring(g_lamps.tsz, RING_ALIGN, &g_lamps.tb, &g_lamps.to), T, g_lamps.tsz);
    float lu[12] = { x0, y0, w, h, (float)tx, (float)ty, 0, 0, 0, 0, 0, 0 };
    memcpy(g_lamps.lu, lu, sizeof lu);
    g_lamps.binned = g_serial;
}

/* a batch's lamps: what its draws bind (lamps_writes) */
typedef struct LampBind
{
    VkDescriptorBufferInfo bi[4]; /* the lamps, the tiles, LU, the cubes' faces */
    VkDescriptorImageInfo ii[3];  /* the characters' cubes, the zone's, the comparing sampler */
} LampBind;

/* the lamps for a batch's draws: binned at the frame's first zone batch (view etc. given), then bound; 0 none */
static int lamps_bind(LampBind* b, const float* view, const float* proj, const uint32_t* vp)
{
    if (g_fxs.scene_lamps <= 0.0f || !g_lamps.n || g_lamps.set != g_serial)
        return 0;
    if (g_lamps.binned != g_serial)
    {
        if (!view)
            return 0;
        lamps_bin(view, proj, vp);
        if (g_lamps.binned != g_serial)
            return 0;
    }
    if (!g_lamps.dummy && !(g_lamps.dummy = tex_array(VK_FORMAT_D16_UNORM, 1, 1)))
        return 0;
    int sh = g_lamps.lsm && g_lamps.nsh && g_lamps.sh_serial + 1 == g_serial;
    g_lamps.lu[6] = sh ? 1.0f : 0.0f, g_lamps.lu[7] = g_fxs.scene_lamp_debug;
    int zs = sh && g_lamps.zlsm && g_lamps.zn == g_lamps.nsh && !memcmp(g_lamps.zpos, g_lamps.shpos, sizeof g_lamps.zpos[0] * g_lamps.nsh);
    g_lamps.lu[8] = zs ? 1.0f : 0.0f;
    g_lamps.lu[9] = fmaxf(g_fxs.lamp_size, 0.0f) * fmaxf(g_fxs.shadow_softness, 0.0f);
    g_lamps.lu[10] = g_fxs.shadow_soft_q >= 1.5f ? 8.0f : 4.0f;
    b->bi[0] = (VkDescriptorBufferInfo){ g_lamps.lb, g_lamps.lo, g_lamps.lsz };
    b->bi[1] = (VkDescriptorBufferInfo){ g_lamps.tb, g_lamps.to, g_lamps.tsz };
    static RingSame klu, kshm;
    b->bi[2] = ring_same(&klu, g_lamps.lu, sizeof g_lamps.lu);
    size_t msz = sh ? (size_t)64 * 6 * g_lamps.nsh : 64;
    b->bi[3] = ring_same(&kshm, g_lamps.shm, msz);
    GfxTex *cm = sh ? g_lamps.lsm : g_lamps.dummy, *zm = zs ? g_lamps.zlsm : g_lamps.dummy;
    b->ii[0] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, cm->view, VK_IMAGE_LAYOUT_GENERAL };
    b->ii[1] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, zm->view, VK_IMAGE_LAYOUT_GENERAL };
    b->ii[2] = (VkDescriptorImageInfo){ g_cmp_samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL };
    cm->used = zm->used = g_serial;
    cm->rec = zm->rec = g_cb_index;
    return 1;
}

static uint32_t lamps_writes(VkWriteDescriptorSet* w, const LampBind* b)
{
    desc_write(&w[0], W_LL, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &b->bi[0], NULL);
    desc_write(&w[1], W_TL, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &b->bi[1], NULL);
    desc_write(&w[2], W_LU, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &b->bi[2], NULL);
    desc_write(&w[3], W_LSMM, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &b->bi[3], NULL);
    desc_write(&w[4], W_LSM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NULL, &b->ii[0]);
    desc_write(&w[5], W_ZLSM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NULL, &b->ii[1]);
    desc_write(&w[6], W_LCS, VK_DESCRIPTOR_TYPE_SAMPLER, NULL, &b->ii[2]);
    return 7;
}

/* a zone batch's opaque items and their world matrices, kept for lamp_zone_cubes at the frame's end (a frame draws the
 * zone in several batches) */
static void lamp_zone_collect(const GfxWorldFrame* f)
{
    if (g_fxs.scene_lamp_zone <= 0.0f || g_fxs.scene_lamp_shadows <= 0.0f || !f->nitem || !f->ninst)
        return;
    if (g_lamps.zc_serial != g_serial)
        g_lamps.nzi = g_lamps.nzw = 0, g_lamps.zc_serial = g_serial;
    if (g_lamps.nzw + f->ninst > g_lamps.czw)
    {
        uint32_t c = g_lamps.czw ? g_lamps.czw : 256;
        while (c < g_lamps.nzw + f->ninst)
            c *= 2;
        float (*w)[16] = (float (*)[16])realloc(g_lamps.zw, (size_t)c * sizeof *w);
        if (!w)
            return;
        g_lamps.zw = w, g_lamps.czw = c;
    }
    if (g_lamps.nzi + f->nitem > g_lamps.czi)
    {
        uint32_t c = g_lamps.czi ? g_lamps.czi : 1024;
        while (c < g_lamps.nzi + f->nitem)
            c *= 2;
        LampZoneItem* v = (LampZoneItem*)realloc(g_lamps.zi, (size_t)c * sizeof *v);
        if (!v)
            return;
        g_lamps.zi = v, g_lamps.czi = c;
    }
    uint32_t base = g_lamps.nzw;
    for (uint32_t i = 0; i < f->ninst; ++i)
        memcpy(g_lamps.zw[base + i], f->inst[i].world, 64);
    g_lamps.nzw += f->ninst;
    for (uint32_t i = 0; i < f->nitem; ++i)
    {
        const GfxWorldItem* it = &f->item[i];
        if (!it->vb || !it->count || it->inst >= f->ninst || (it->flags & GFX_WORLD_BLEND))
            continue;
        LampZoneItem* o = &g_lamps.zi[g_lamps.nzi++];
        int at = (it->flags & GFX_WORLD_ATEST) && it->tex && it->tex->type == GFX_TEX_2D;
        o->vb = it->vb->b, o->tex = at ? it->tex->view : VK_NULL_HANDLE, o->samp = at ? sampler(&it->samp) : VK_NULL_HANDLE;
        o->first = it->first, o->count = it->count, o->prim = it->prim, o->inst = base + it->inst;
        it->vb->used = g_serial;
        if (at)
            it->tex->used = g_serial;
    }
}

/* the characters' shadows from the nearest lamps (scene_lamp_shadows): a cube each (six layers of lsm) at the frame's
 * end, of the frame's characters within its reach (gfx_metal.m's, a pass a layer as the cascades' are here, each face
 * drawing only the characters toward it); pal, st the frame's palette and states in the ring */
static void lamp_shadows_draw(const GfxCharFrame* cf, VkDescriptorBufferInfo pal, VkDescriptorBufferInfo st)
{
    g_lamps.nsh = 0;
    int want = (int)g_fxs.scene_lamp_shadows;
    want = want < 0 ? 0 : want > 4 ? 4 : want;
    if (g_fxs.scene_lamps <= 0.0f || !want || g_lamps.set != g_serial || !g_lamps.n || !g_shd.nitems || !g_shd.chars || !cf->nbones)
        return;
    uint32_t size = (uint32_t)g_fxs.scene_lamp_shadow_size;
    size = size < 64 ? 64 : size > 1024 ? 1024 : size;
    if (!g_lamps.lsm || g_lamps.lsize != size)
    {
        end_pass();
        gfx_tex_destroy(g_lamps.lsm);
        g_lamps.lsm = tex_array(VK_FORMAT_D16_UNORM, size, 24);
        g_lamps.lsize = size;
        if (!g_lamps.lsm)
            return;
    }
    /* the nearest lamps to the camera with a character within their reach: the lamps in order of their distance (the
     * first of equals first), each tried against the characters' places (a character's parts, one after another, share
     * theirs) until enough have one */
    float dist[256];
    uint32_t order[256];
    for (uint32_t i = 0; i < g_lamps.n; ++i)
    {
        const float* p = g_lamps.l[i].pos;
        float cx = p[0] - g_lamps.cam[0], cy = p[1] - g_lamps.cam[1], cz = p[2] - g_lamps.cam[2];
        dist[i] = cx * cx + cy * cy + cz * cz;
        uint32_t j = i;
        for (; j > 0 && dist[order[j - 1]] > dist[i]; --j)
            order[j] = order[j - 1];
        order[j] = i;
    }
    int pick[4] = { -1, -1, -1, -1 };
    for (uint32_t o = 0, np = 0; o < g_lamps.n && np < (uint32_t)want; ++o)
    {
        const GfxWorldLight* l = &g_lamps.l[order[o]];
        float r = l->pos[3] + 1.5f;
        int near = 0;
        for (uint32_t k = 0; k < g_shd.nitems && !near; ++k)
        {
            const float* a = g_shd.items[k].at;
            if (k && !memcmp(a, g_shd.items[k - 1].at, 12))
                continue;
            float dx = a[0] - l->pos[0], dy = a[1] - l->pos[1], dz = a[2] - l->pos[2];
            near = dx * dx + dy * dy + dz * dz < r * r;
        }
        if (near)
            pick[np++] = (int)order[o];
    }
    static const float AX[6][3][3] = { /* each face: right, up, forward */
        { { 0, 0, -1 }, { 0, -1, 0 }, { 1, 0, 0 } },  { { 0, 0, 1 }, { 0, -1, 0 }, { -1, 0, 0 } },
        { { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },    { { 1, 0, 0 }, { 0, 0, -1 }, { 0, -1, 0 } },
        { { 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, 1 } },   { { -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 } } };
    uint32_t ns = 0;
    for (int j = 0; j < want && pick[j] >= 0; ++j, ++ns)
    {
        const GfxWorldLight* l = &g_lamps.l[pick[j]];
        memcpy(g_lamps.shpos[ns], l->pos, 16);
        float n = 0.05f, f = fmaxf(l->pos[3], 0.5f), A = f / (f - n), B = -n * f / (f - n);
        for (int fc = 0; fc < 6; ++fc)
        {
            const float* R = AX[fc][0];
            const float* U = AX[fc][1];
            const float* F = AX[fc][2];
            float* M = g_lamps.shm[ns * 6 + fc];
            memset(M, 0, 64);
            float lr = R[0] * l->pos[0] + R[1] * l->pos[1] + R[2] * l->pos[2];
            float lu = U[0] * l->pos[0] + U[1] * l->pos[1] + U[2] * l->pos[2];
            float lf = F[0] * l->pos[0] + F[1] * l->pos[1] + F[2] * l->pos[2];
            for (int a = 0; a < 3; ++a)
                M[4 * a + 0] = R[a], M[4 * a + 1] = U[a], M[4 * a + 2] = F[a] * A, M[4 * a + 3] = F[a];
            M[12] = -lr, M[13] = -lu, M[14] = -lf * A + B, M[15] = -lf;
        }
    }
    if (!ns)
        return;
    uint32_t drawn = 0;
    for (uint32_t layer = 0; layer < 6 * ns; ++layer)
    {
        const float* Lp = g_lamps.shpos[layer / 6];
        const float *R = AX[layer % 6][0], *U = AX[layer % 6][1], *F = AX[layer % 6][2];
        float r = Lp[3] + 1.5f;
        int ts = shadow_pass_begin(g_lamps.lsm, layer, GFX_PASS_LAMPS, layer > 0);
        vkCmdSetDepthBias(g_cb, 2.0f, 0.0f, 2.0f);
        Cast c;
        cast_begin(&c, g_lamps.shm[layer], cf->alpha_ref, pal, st, g_shd.chars);
        for (uint32_t i = 0; i < g_shd.nitems; ++i)
        {
            const ShItem* it = &g_shd.items[i];
            float d[3] = { it->at[0] - Lp[0], it->at[1] - Lp[1], it->at[2] - Lp[2] };
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > r * r)
                continue;
            /* within the face's quarter of the cube: three units round the part's place, a large character's reach,
             * inside each of its four sides */
            float f = F[0] * d[0] + F[1] * d[1] + F[2] * d[2] + 3.0f * 1.41421356f;
            if (fabsf(R[0] * d[0] + R[1] * d[1] + R[2] * d[2]) > f || fabsf(U[0] * d[0] + U[1] * d[1] + U[2] * d[2]) > f)
                continue;
            cast_char(&c, it);
        }
        pass_close(ts);
        drawn += c.drawn;
    }
    g_cmd_draws += drawn;
    g_lamps.nsh = ns, g_lamps.sh_serial = g_serial;
}

/* the frame's zone into the cubes of the lamps lamp_shadows_draw just took, when they changed or every 600 frames; read
 * from the next frame on. The zone is what the game drew this frame: none of what lies out of its view */
static void lamp_zone_cubes(void)
{
    if (!g_lamps.nsh || g_lamps.sh_serial != g_serial || g_lamps.zc_serial != g_serial || !g_lamps.nzi || !g_shd.zone[0] ||
        !g_shd.zone[1])
        return;
    int same = g_lamps.zn == g_lamps.nsh && g_lamps.z_serial && g_serial - g_lamps.z_serial < 600 &&
        !memcmp(g_lamps.zpos, g_lamps.shpos, sizeof g_lamps.zpos[0] * g_lamps.nsh);
    if (same)
        return;
    uint32_t size = g_lamps.lsize;
    if (!g_lamps.zlsm || g_lamps.zlsm->w != size)
    {
        end_pass();
        gfx_tex_destroy(g_lamps.zlsm);
        g_lamps.zlsm = tex_array(VK_FORMAT_D32_SFLOAT, size, 24);
        if (!g_lamps.zlsm)
            return;
    }
    VkDescriptorBufferInfo wbi = ring_put(g_lamps.zw, (size_t)g_lamps.nzw * 64), none = { 0 };
    uint32_t drawn = 0;
    for (uint32_t layer = 0; layer < 6 * g_lamps.nsh; ++layer)
    {
        const float* L = g_lamps.shpos[layer / 6];
        float reach = L[3] + 40.0f;
        int ts = shadow_pass_begin(g_lamps.zlsm, layer, GFX_PASS_LAMPS, layer > 0);
        Cast c;
        cast_begin(&c, g_lamps.shm[layer], 0.0f, wbi, none, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < g_lamps.nzi; ++i)
        {
            const LampZoneItem* it = &g_lamps.zi[i];
            const float* w = g_lamps.zw[it->inst];
            float dx = w[12] - L[0], dy = w[13] - L[1], dz = w[14] - L[2];
            if (dx * dx + dy * dy + dz * dz <= reach * reach)
                cast_zone(&c, it->vb, it->tex, it->samp, it->inst, it->first, it->count, it->prim);
        }
        pass_close(ts);
        drawn += c.drawn;
    }
    g_cmd_draws += drawn;
    memcpy(g_lamps.zpos, g_lamps.shpos, sizeof g_lamps.zpos);
    g_lamps.zn = g_lamps.nsh, g_lamps.z_serial = g_serial;
}

/* the lamps' glows (scene_lamp_glow): a soft sprite at each lamp, its own colour, hidden by what stands in front of it
 * (the scene's depth, softly), faded with distance; added to the scene (gfx_metal.m's lamp_glow_vs/fs). GLU in W_FRAME,
 * the lamps in W_LL, the depth in W_TEX */
static const char GLOW_GLSL[] =
    "struct GLL { vec4 pos, col, att, amb; };\n"
    "layout(std140, set = 0, binding = 0) uniform GLU { mat4 vp; vec4 p; vec4 cam; vec4 zp; vec4 vpr; vec4 pr; } G;\n"
    "#ifdef GFX_VS\n"
    "layout(std430, set = 0, binding = 15) readonly buffer LLB { GLL LS[]; };\n"
    "layout(location = 0) out vec2 o_c;\n"
    "layout(location = 1) flat out vec3 o_col;\n"
    "layout(location = 2) flat out float o_lz;\n"
    "void main() {\n"
    "  GLL l = LS[gl_InstanceIndex];\n"
    "  vec2 c = vec2((gl_VertexIndex & 1) != 0 ? 1.0 : -1.0, (gl_VertexIndex & 2) != 0 ? 1.0 : -1.0);\n"
    "  vec4 q = G.vp * vec4(l.pos.xyz, 1.0);\n"
    "  float dist = length(l.pos.xyz - G.cam.xyz);\n"
    "  float fade = clamp(1.0 - dist / G.p.z, 0.0, 1.0) * step(0.3, q.w);\n"
    "  float size = G.p.x * clamp(l.pos.w / 8.0, 0.5, 2.0);\n"
    "  gl_Position = q + vec4(c * size * G.pr.xy, 0.0, 0.0);\n"
    "  if (fade <= 0.0) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n" /* (off screen) */
    "  o_c = c;\n"
    "  o_col = l.col.rgb * (G.p.y * fade);\n"
    "  o_lz = q.w;\n"
    "}\n"
    "#else\n"
    "layout(set = 0, binding = 4) uniform sampler2D dt;\n"
    "layout(location = 0) in vec2 i_c;\n"
    "layout(location = 1) flat in vec3 i_col;\n"
    "layout(location = 2) flat in float i_lz;\n"
    "layout(location = 0) out vec4 oc;\n"
    "void main() {\n"
    "  float r2 = dot(i_c, i_c);\n"
    "  if (r2 >= 1.0) discard;\n"
    "  float d = texelFetch(dt, ivec2(gl_FragCoord.xy), 0).x;\n"
    "  float dn = (d - G.zp.z) / max(G.zp.w - G.zp.z, 1e-6);\n"
    "  float sz = dn >= 0.999999 ? 1e6 : abs(G.zp.y / (dn * G.pr.z - G.zp.x));\n" /* the scene's view depth here */
    "  float vis = clamp((sz - i_lz) / max(G.p.x, 0.1) + 1.0, 0.0, 1.0);\n"
    "  float f = exp(-r2 * 5.0) - 0.0067;\n"
    "  oc = vec4(i_col * (f * vis), 1.0);\n"
    "}\n"
    "#endif\n";

/* the lamps' glows onto the finished scene (color, its depth), before what takes the scene from here (the effects'
 * bloom, as Metal's post effects do); the viewport in the target */
static void post_lamp_glows(GfxTex* color, const GfxScene* s, GfxTex* depth, float vx, float vy, float vw, float vh)
{
    if (g_fxs.scene_lamps <= 0.0f || g_fxs.scene_lamp_glow <= 0.0f || g_lamps.binned != g_serial || !g_lamps.n || !g_lamps.lb ||
        !world_init())
        return;
    VkPipeline glow = fmt_pipe(color->vf, FP_GLOW);
    if (!glow)
        return;
    float minz, maxz;
    memcpy(&minz, &s->vp[4], 4);
    memcpy(&maxz, &s->vp[5], 4);
    if (maxz <= minz)
        minz = 0, maxz = 1;
    struct
    {
        float vp[16], p[4], cam[4], zp[4], vpr[4], pr[4];
    } u = { { 0 } };
    memcpy(u.vp, g_lamps.vp, 64);
    u.p[0] = fmaxf(g_fxs.scene_lamp_glow_size, 0.05f), u.p[1] = g_fxs.scene_lamp_glow, u.p[2] = fmaxf(g_fxs.scene_lamp_far, 1.0f);
    memcpy(u.cam, g_lamps.cam, 12);
    u.zp[0] = s->proj[10], u.zp[1] = s->proj[14], u.zp[2] = minz, u.zp[3] = maxz;
    u.vpr[0] = vx, u.vpr[1] = vy, u.vpr[2] = vw, u.vpr[3] = vh;
    u.pr[0] = s->proj[0], u.pr[1] = s->proj[5], u.pr[2] = s->proj[11] < 0.0f ? -1.0f : 1.0f;
    VkViewport v = { vx, vy + vh, vw, -vh, 0, 1 }; /* y up, as the game's draws (set_viewport) */
    int ts = pass_open(color, 0, 0, VK_ATTACHMENT_LOAD_OP_LOAD, &v, GFX_PASS_LAMPS);
    VkCommandBuffer cb = g_cb;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, glow);
    VkDescriptorBufferInfo bi[2] = { ring_put(&u, sizeof u), { g_lamps.lb, g_lamps.lo, g_lamps.lsz } };
    VkDescriptorImageInfo ii = { g_near_samp, depth->view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[3];
    desc_write(&w[0], W_FRAME, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bi[0], NULL);
    desc_write(&w[1], W_LL, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[1], NULL);
    desc_write(&w[2], W_TEX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &ii);
    p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_world.layout, 0, 3, w);
    vkCmdDraw(cb, 4, g_lamps.n, 0, 0);
    pass_close(ts);
    depth->used = g_serial, depth->rec = g_cb_index;
}

/* the effects a batch's draws take (shadows, occlusion, highlights, lamps, bounce light): their uniforms and writes,
 * made once a batch, every draw pushing them after its own */
typedef struct WorldFx
{
    uint32_t wk; /* WK_SHADOW, WK_AO, WK_SPEC, WK_LAMPS, WK_GI */
    VkDescriptorBufferInfo sbi, abi, pbi, gbi[2];
    VkDescriptorImageInfo si[5], aii, gii[2];
    LampBind lb;
} WorldFx;

/* f the zone's batch (its occlusion noted, its lamps binned and its items kept for the cubes, per pixel only), NULL the
 * characters'; the writes into w (x holds what they point at); how many */
static uint32_t world_fx(WorldFx* x, const GfxWorldFrame* f, GfxTex* depth, VkWriteDescriptorSet* w)
{
    int pixel = !f || f->mode == 2;
    int shadow = shadow_uniforms(&x->sbi);
    if (f)
        ao_note(f, depth);
    int ao = ao_uniforms(&x->abi);
    int spec = spec_uniforms(shadow && pixel, &x->pbi);
    int lamps = pixel && lamps_bind(&x->lb, f ? f->view : NULL, f ? f->proj : NULL, f ? f->vp : NULL);
    if (lamps && f)
        lamp_zone_collect(f);
    int gi = gi_uniforms(&x->gbi[0]);
    x->wk = (shadow ? WK_SHADOW : 0) | (ao ? WK_AO : 0) | (spec ? WK_SPEC : 0) | (lamps ? WK_LAMPS : 0) | (gi ? WK_GI : 0);
    uint32_t nw = shadow ? shadow_writes(w, &x->sbi, x->si) : 0;
    if (ao)
        nw += ao_writes(&w[nw], &x->abi, &x->aii);
    if (spec)
        desc_write(&w[nw++], W_SP, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &x->pbi, NULL);
    if (lamps)
        nw += lamps_writes(&w[nw], &x->lb);
    if (gi)
        nw += gi_writes(&w[nw], x->gbi, x->gii);
    return nw;
}

int gfx_world_draw(const GfxWorldFrame* f)
{
    if (!g_dev || !f->nitem || !world_init())
        return 0;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    /* timed: a render pass of its own (an extra load and store of the target, only while timing) */
    int timed = gfx_pass_timing_on();
    if (timed)
        end_pass(), g_pass_next = GFX_PASS_WORLD;
    if (!begin_pass())
    {
        g_pass_next = -1;
        return 0;
    }
    GfxTex* depth = depth_attachment();
    int drew = 0;
    if (!set_viewport(f->vp))
        goto done;
    VkCommandBuffer cb = g_cb;
    struct
    {
        float vp[16];
        float params[4];
    } fu;
    gfx_mat_mul(fu.vp, f->view, f->proj);
    fu.params[0] = f->alpha_ref, fu.params[1] = f->pixel[0], fu.params[2] = f->pixel[1], fu.params[3] = 0.0f;
    VkDescriptorBufferInfo bi[4];
    bi[0] = ring_put(&fu, sizeof fu);
    size_t isz = (size_t)f->ninst * sizeof(GfxWorldInst);
    bi[1] = ring_put(f->inst, isz);
    GfxWorldLight none;
    memset(&none, 0, sizeof none);
    uint32_t nl = f->nlight ? f->nlight : 1;
    bi[2] = ring_put(f->nlight ? f->light : &none, (size_t)nl * sizeof none);
    world_state(depth);
    /* the batch's bindings made once: the draws' own five (bi[3] and ii set per item), then the effects' */
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w[W_COUNT];
    desc_write(&w[0], W_FRAME, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bi[0], NULL);
    desc_write(&w[1], W_INST, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[1], NULL);
    desc_write(&w[2], W_LIGHTS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[2], NULL);
    desc_write(&w[3], W_VB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[3], NULL);
    desc_write(&w[4], W_TEX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &ii);
    WorldFx x;
    uint32_t nw = 5 + world_fx(&x, f, depth, &w[5]);
    uint32_t wk = (f->mode == 2 ? WK_PIXEL : 0) | x.wk;
    VkPipeline pipes[4], cur = VK_NULL_HANDLE; /* by blend and alpha test, each found once */
    uint32_t found = 0;
    int cull = -1, bias = INT_MIN, write = -1, topo = -1;
    for (uint32_t i = 0; i < f->nitem; ++i)
    {
        const GfxWorldItem* it = &f->item[i];
        if (!it->vb || !it->tex || !it->count || it->tex->type != GFX_TEX_2D)
            continue;
        int blend = (it->flags & GFX_WORLD_BLEND) != 0, at = (it->flags & GFX_WORLD_ATEST) != 0, pk = blend << 1 | at;
        if (!(found >> pk & 1))
            pipes[pk] = world_pipeline(0, wk | (at ? WK_ATEST : 0), blend, depth), found |= 1u << pk;
        VkPipeline p = pipes[pk];
        if (!p)
            continue;
        if (p != cur)
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p), cur = p;
        if ((!blend) != write)
            vkCmdSetDepthWriteEnable(cb, depth && !blend ? VK_TRUE : VK_FALSE), write = !blend;
        int c = (it->flags & GFX_WORLD_TWOSIDED) ? 0 : (it->flags & GFX_WORLD_MIRRORED) ? 1 : 2;
        if (c != cull)
            vkCmdSetCullMode(cb, c == 2 ? VK_CULL_MODE_BACK_BIT : c == 1 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE), cull = c;
        int zb = blend ? f->zbias_blend : f->zbias_opaque;
        if (zb != bias)
            vkCmdSetDepthBias(cb, -(float)zb, 0.0f, -(float)zb * 0.5f), bias = zb;
        int strip = it->prim == GFX_TRIANGLESTRIP;
        if (strip != topo)
            vkCmdSetPrimitiveTopology(cb, strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST), topo = strip;
        GfxSampler sk = it->samp;
        sk.addr_u = sk.addr_v = sk.addr_w = (it->flags & GFX_WORLD_CLAMP) ? 3 : 1;
        if (g_fxs.fx != 0.0f && g_fxs.aniso > 1.0f && it->tex->levels > 1 && it->tex->levels < 32 && sk.min >= 2 &&
            it->tex->filled == (1u << it->tex->levels) - 1)
            sk.min = 3, sk.mip = 2, sk.max_aniso = (uint8_t)(g_fxs.aniso > 16.0f ? 16.0f : g_fxs.aniso);
        ii = (VkDescriptorImageInfo){ sampler(&sk), it->tex->view, VK_IMAGE_LAYOUT_GENERAL };
        bi[3] = (VkDescriptorBufferInfo){ it->vb->b, 0, VK_WHOLE_SIZE };
        p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_world.layout, 0, nw, w);
        uint32_t dc[2] = { it->inst, it->flags };
        vkCmdPushConstants(cb, g_world.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof dc, dc);
        it->vb->used = g_serial;
        it->tex->used = g_serial, it->tex->rec = g_cb_index;
        vkCmdDraw(cb, vertex_count(it->prim, it->count), 1, it->first, 0);
        ++drew;
    }
    g_bound = VK_NULL_HANDLE; /* the game's next draw binds its own */
    g_cmd_draws += drew;
done:
    if (timed)
        end_pass();
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws += drew;
    return drew > 0;
}

int gfx_chars_draw(const GfxCharFrame* f)
{
    if (!g_dev || !f->nitem || !f->nbones || !world_init())
        return 0;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    int timed = gfx_pass_timing_on();
    if (timed)
        end_pass(), g_pass_next = GFX_PASS_CHARS;
    if (!begin_pass())
    {
        g_pass_next = -1;
        return 0;
    }
    GfxTex* depth = depth_attachment();
    int drew = 0;
    /* the batch's bindings made once: the draws' own five (bi[2], bi[3] and ii set per item), then the effects' */
    VkDescriptorBufferInfo bi[4];
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w[W_COUNT];
    desc_write(&w[0], W_INST, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[0], NULL);
    desc_write(&w[1], W_LIGHTS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[1], NULL);
    desc_write(&w[2], W_VB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[2], NULL);
    desc_write(&w[3], W_UV, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[3], NULL);
    desc_write(&w[4], W_TEX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &ii);
    WorldFx x;
    uint32_t nw = 5 + world_fx(&x, NULL, depth, &w[5]);
    int shadow = (x.wk & WK_SHADOW) != 0;
    VkPipeline p = world_pipeline(1, x.wk, 0, depth);
    if (!p || !set_viewport(f->vp))
        goto done;
    VkCommandBuffer cb = g_cb;
    size_t psz = (size_t)f->nbones * 64, ssz = (size_t)f->nstate * sizeof(GfxCharState);
    bi[0] = ring_put(f->palette, psz);
    bi[1] = ring_put(f->state, ssz);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    world_state(depth);
    vkCmdSetDepthWriteEnable(cb, depth ? VK_TRUE : VK_FALSE);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
    int topo = -1;
    struct
    {
        float params[4];
        uint32_t palette, state, mirror, pad;
        float lag[4];
    } pc = { { f->alpha_ref, f->pixel[0], f->pixel[1], 0.0f }, 0, 0, 0, 0, { 0.0f, 0.0f, 0.0f, 0.0f } };
    for (uint32_t i = 0; i < f->nitem; ++i)
    {
        const GfxCharItem* it = &f->item[i];
        if (!it->vb || !it->uv || !it->ib || !it->tex || !it->count || it->tex->type != GFX_TEX_2D)
            continue;
        int strip = it->prim == GFX_TRIANGLESTRIP;
        if (strip != topo)
            vkCmdSetPrimitiveTopology(cb, strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST), topo = strip;
        ii = (VkDescriptorImageInfo){ sampler(&it->samp), it->tex->view, VK_IMAGE_LAYOUT_GENERAL };
        bi[2] = (VkDescriptorBufferInfo){ it->vb->b, 0, VK_WHOLE_SIZE };
        bi[3] = (VkDescriptorBufferInfo){ it->uv->b, 0, VK_WHOLE_SIZE };
        p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_world.layout, 0, nw, w);
        pc.palette = it->palette, pc.state = it->state, pc.mirror = it->mirror;
        if (shadow)
            char_lag(f, it, pc.lag);
        vkCmdPushConstants(cb, g_world.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdBindIndexBuffer(cb, it->ib->b, 0, it->index_size == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        it->vb->used = it->uv->used = it->ib->used = g_serial;
        it->tex->used = g_serial, it->tex->rec = g_cb_index;
        vkCmdDrawIndexed(cb, vertex_count(it->prim, it->count), 1, it->first, 0, 0);
        ++drew;
    }
    g_bound = VK_NULL_HANDLE;
    g_cmd_draws += drew;
done:
    if (timed)
        end_pass();
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws += drew;
    return drew > 0;
}

/* a buffer of at least need bytes in *b (made again, half as large again - or as large, exact: one made once, as the
 * bounce light's zone - when smaller; the old one let go once the GPU is past it): the GPU's own, or one the CPU
 * writes (host) */
static int rt_buffer_ex(RtBuf* b, VkDeviceSize need, VkBufferUsageFlags usage, int host, int exact)
{
    if (b->b && b->size >= need)
        return 0;
    if (b->b)
        trash(2, (uint64_t)(uintptr_t)b->b, b->m), b->b = VK_NULL_HANDLE;
    need = (need + (exact ? 0 : need / 2) + 65535) & ~(VkDeviceSize)65535;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = need;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo ai = { 0 };
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (host)
        ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vma, &bi, &ai, &b->b, &b->m, &info) != VK_SUCCESS)
    {
        fprintf(stderr, "[recomp] gfx: ray tracing: a buffer of %llu bytes failed\n", (unsigned long long)need);
        b->b = VK_NULL_HANDLE, b->size = 0;
        return -1;
    }
    b->size = need, b->p = info.pMappedData;
    VkBufferDeviceAddressInfo ad = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    ad.buffer = b->b;
    b->addr = vkGetBufferDeviceAddress(g_dev, &ad);
    return 1;
}

static int rt_buffer(RtBuf* b, VkDeviceSize need, VkBufferUsageFlags usage, int host) { return rt_buffer_ex(b, need, usage, host, 0); }

/* an acceleration structure of at least need bytes in *s, over its own buffer (exact: rt_buffer_ex's) */
static int rt_structure_ex(VkAccelerationStructureKHR* s, RtBuf* buf, VkDeviceSize need, VkAccelerationStructureTypeKHR type, int exact)
{
    int grown = rt_buffer_ex(buf, need, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0, exact);
    if (grown < 0)
        return 0;
    if (*s && !grown)
        return 1;
    if (*s)
        trash(3, (uint64_t)(uintptr_t)*s, VK_NULL_HANDLE), *s = VK_NULL_HANDLE;
    VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    ci.buffer = buf->b, ci.size = buf->size, ci.type = type;
    return vkCreateAccelerationStructureKHR(g_dev, &ci, NULL, s) == VK_SUCCESS;
}

static int rt_structure(VkAccelerationStructureKHR* s, RtBuf* buf, VkDeviceSize need, VkAccelerationStructureTypeKHR type)
{
    return rt_structure_ex(s, buf, need, type, 0);
}

static void rt_free(RtBuf* b)
{
    if (b->b)
        trash(2, (uint64_t)(uintptr_t)b->b, b->m);
    memset(b, 0, sizeof *b);
}

/* --- the scene renderer's bounce light (gfx_gi_zone, gfx_gi_frame, gfx.h): gfx_gi_metal.inc's, on Vulkan (its notes
 * there; the shading's side is GI_GLSL, the state g_gi) -------------------------------------------------------------
 * The same kernels as compute passes with ray queries: the zone flattened into world-space triangles (GI_FLATTEN) and
 * built into one bottom-level structure, one instance of it at the top; each frame's share of the probes traced
 * (GI_SCROLL, GI_TRACE) and blended into the two atlases (GI_BLEND, the distances' first), storage images the zone's
 * and characters' functions sample (W_GI). Recorded into the frame's command buffer outside a pass, where Metal commits
 * a command buffer of its own ahead of the frame's; the zone's structure is built and waited for once, as it loads.
 * Without ray queries (g_has_rt) both calls do nothing. Both atlases are RGBA16F (the distances' RG16F on Metal: an
 * extended storage format here). */
static const char GI_KERNELS_GLSL[] =
    "#if defined(GI_TRACE)\n"
    "#extension GL_EXT_ray_query : require\n"
    "#endif\n"
    "#define sat(x) clamp(x, 0.0, 1.0)\n"
    "#if defined(GI_FLATTEN)\n"
    GI_OCT_GLSL
    /* the zone flattened: triangle t of an item into world space, with its albedo and its normal (faced as its vertex
     * normals face, its corners turned to match); flags 1 textured, 2 two-sided. I: the item's world matrix and its
     * inverse, a (first vertex, triangles, primitive, its first triangle in the list), b (flags, the texture's level) */
    "layout(local_size_x = 64) in;\n"
    "layout(std430, set = 0, binding = 10) readonly buffer VBB { uint vb[]; };\n"
    "layout(set = 0, binding = 11) uniform sampler2D tx;\n"
    "layout(std430, set = 0, binding = 12) writeonly buffer POSB { float pos[]; };\n"
    "layout(std430, set = 0, binding = 3) writeonly buffer ATB { uvec2 attr[]; };\n"
    "layout(std140, set = 0, binding = 13) uniform FIB { mat4 world, inv; uvec4 a; uvec4 b; } I;\n"
    "void main() {\n"
    "  uint t = gl_GlobalInvocationID.x;\n"
    "  if (t >= I.a.y) return;\n"
    "  uint v0 = I.a.x + (I.a.z == 5u ? t : 3u * t);\n"
    "  uint vi[3] = uint[3](v0, v0 + 1u, v0 + 2u);\n"
    "  if (I.a.z == 5u && (t & 1u) != 0u) { vi[0] = v0 + 1u; vi[1] = v0; }\n" /* a strip's odd triangles wind the other way */
    "  vec3 p[3];\n"
    "  vec3 n = vec3(0.0);\n"
    "  vec4 vc = vec4(0.0), tc = vec4(0.0);\n"
    "  vec2 uvc = vec2(0.0);\n"
    "  bool tex = (I.b.x & 1u) != 0u;\n"
    "  float lv = float(I.b.y);\n"
    "  for (int k = 0; k < 3; ++k) {\n"
    "    uint b = vi[k] * 9u;\n" /* 36-byte vertices: position, normal, D3DCOLOR, uv */
    "    p[k] = (I.world * vec4(uintBitsToFloat(uvec3(vb[b], vb[b + 1u], vb[b + 2u])), 1.0)).xyz;\n"
    "    n += (vec4(uintBitsToFloat(uvec3(vb[b + 3u], vb[b + 4u], vb[b + 5u])), 0.0) * I.inv).xyz;\n"
    "    vc += unpackUnorm4x8(vb[b + 6u]).zyxw;\n"
    "    vec2 uv = uintBitsToFloat(uvec2(vb[b + 7u], vb[b + 8u]));\n"
    "    uvc += uv;\n"
    "    if (tex) tc += textureLod(tx, uv, lv);\n"
    "  }\n"
    "  uvc /= 3.0;\n"
    "  tc = tex ? (tc + textureLod(tx, uvc, lv)) * 0.25 : vec4(0.5);\n"
    "  vc /= 3.0;\n"
    "  vec3 fn = cross(p[1] - p[0], p[2] - p[0]);\n"
    "  if (dot(fn, n) < 0.0) { vec3 x = p[1]; p[1] = p[2]; p[2] = x; fn = -fn; }\n"
    "  float fl = length(fn);\n"
    "  if (!(fl > 1e-12)) { p[1] = p[0]; p[2] = p[0]; }\n" /* nothing: a degenerate triangle no ray hits */
    "  fn = fl > 1e-12 ? fn / fl : vec3(0.0, -1.0, 0.0);\n"
    "  uint o = I.a.w + t;\n"
    "  for (int k = 0; k < 3; ++k) {\n"
    "    uint q = 9u * o + 3u * uint(k);\n"
    "    pos[q] = p[k].x; pos[q + 1u] = p[k].y; pos[q + 2u] = p[k].z;\n"
    "  }\n"
    "  vec3 alb = min(2.0 * tc.rgb * vc.rgb, vec3(0.9));\n"
    "  attr[o] = uvec2(packUnorm4x8(vec4(alb, float(I.b.x & 2u) / 255.0)), packUnorm2x16(gi_oct(fn) * 0.5 + 0.5));\n"
    "}\n"
    "#else\n"
    "#define GI_B_GU 0\n#define GI_B_PS 1\n#define GI_B_IR 8\n#define GI_B_DS 9\n#define GI_PS_Q\n"
    GI_GLSL
    /* probe l's world cell */
    "ivec3 gi_cell(uint l) {\n"
    "  ivec3 s = ivec3(int(l) % G.dims.x, (int(l) / G.dims.x) % G.dims.y, int(l) / (G.dims.x * G.dims.y));\n"
    "  return G.base.xyz + gi_slot(s - G.base.xyz);\n"
    "}\n"
    /* ray r of n: a spherical Fibonacci direction, turned by this update's rotation */
    "vec3 gi_ray(uint r) {\n"
    "  float n = float(G.n.x), z = 1.0 - (2.0 * float(r) + 1.0) / n, s = sqrt(sat(1.0 - z * z));\n"
    "  float ph = 6.2831853 * fract(float(r) * 0.6180340);\n"
    "  return normalize((G.rot * vec4(cos(ph) * s, sin(ph) * s, z, 0.0)).xyz);\n"
    "}\n"
    "#if defined(GI_SCROLL)\n" /* the cells new to the grid: cleared, not used till traced */
    "layout(local_size_x = 64) in;\n"
    "layout(std430, set = 0, binding = 2) buffer PCB { ivec4 PC[]; };\n"
    "void main() {\n"
    "  uint l = gl_GlobalInvocationID.x;\n"
    "  if (l >= uint(G.dims.w)) return;\n"
    "  ivec3 c = gi_cell(l);\n"
    "  if (any(notEqual(PC[l].xyz, c)) || PC[l].w == 0) { PC[l] = ivec4(c, 1); PS[l] = 0u; }\n"
    "}\n"
    "#elif defined(GI_TRACE)\n"
    /* ray x of this update's probe y: what it brings back (rgb) and how far it went (a; < 0 a backface, 1e4 the sky) */
    "layout(local_size_x = 64) in;\n"
    "struct GL { vec4 pos, diffuse, ambient, att; };\n" /* GfxWorldLight */
    "layout(std430, set = 0, binding = 3) readonly buffer ATB { uvec2 attr[]; };\n"
    "layout(std430, set = 0, binding = 4) readonly buffer LTB { GL LT[]; };\n"
    "layout(std430, set = 0, binding = 5) writeonly buffer RBB { vec4 RB[]; };\n"
    "layout(set = 0, binding = 6) uniform accelerationStructureEXT gas;\n"
    /* the shadows' uniforms (SHADOW_GLSL's SH) and their zone layer */
    "layout(std140, set = 0, binding = 7) uniform SHB { mat4 m[4]; vec4 cas[4]; vec4 cbox[4]; vec4 dir; vec4 cam; vec4 info;\n"
    "  mat4 cm[4]; vec4 ctx; vec4 pk; } S;\n"
    "layout(set = 0, binding = 14) uniform sampler2DArray zm;\n"
    /* the sun's or moon's shadow at X (normal n) from the zone's cascades, one texel read: 1 lit, 0 not, -1 outside
     * them (a ray then) */
    "float gi_cascade(vec3 X, vec3 n) {\n"
    "  float d = length(X - S.cam.xyz);\n"
    "  for (uint c = 0u; c < uint(S.info.x); ++c) {\n"
    "    if (S.cas[c].w == 0.0 || d > S.cas[c].x) continue;\n"
    "    float t = S.cas[c].y;\n"
    "    vec4 h = S.m[c] * vec4(X + n * (2.0 * t) + S.dir.xyz * t, 1.0);\n"
    "    if (abs(h.x) > 0.99 || abs(h.y) > 0.99) continue;\n"
    "    float w = float(textureSize(zm, 0).x);\n"
    "    ivec2 px = ivec2(clamp(vec2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5) * w, 0.0, w - 1.0));\n"
    "    return h.z <= texelFetch(zm, ivec3(px, int(c)), 0).r ? 1.0 : 0.0;\n"
    "  }\n"
    "  return -1.0;\n"
    "}\n"
    "void main() {\n"
    "  uint r = gl_GlobalInvocationID.x, u = gl_GlobalInvocationID.y, l = u * G.n.y + G.n.z;\n"
    "  if (r >= G.n.x || l >= uint(G.dims.w)) return;\n"
    "  vec3 o = vec3(gi_cell(l)) * G.org.w, d = gi_ray(r);\n"
    "  vec4 res = vec4(0.0, 0.0, 0.0, 1e4);\n"
    "  rayQueryEXT q;\n"
    "  rayQueryInitializeEXT(q, gas, gl_RayFlagsOpaqueEXT, 0xFFu, o, 0.0, d, G.tr.x);\n"
    "  while (rayQueryProceedEXT(q)) {}\n"
    "  if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {\n"
    "    uvec2 at = attr[rayQueryGetIntersectionPrimitiveIndexEXT(q, true)];\n"
    "    vec4 alb = unpackUnorm4x8(at.x);\n"
    "    vec3 n = gi_unoct(unpackUnorm2x16(at.y) * 2.0 - 1.0);\n"
    "    float t = rayQueryGetIntersectionTEXT(q, true);\n"
    "    res = vec4(0.0, 0.0, 0.0, -t);\n"
    "    if (dot(n, d) > 0.0 && alb.a > 0.0) n = -n;\n" /* two-sided: lit from the side the ray came */
    "    if (dot(n, d) <= 0.0) {\n"
    "      vec3 X = o + d * t + n * 0.05;\n"
    "      vec3 E = G.amb.rgb * G.tr.w;\n"
    "      for (int k = 0; k < 2; ++k) {\n"
    "        vec4 L = k != 0 ? G.sun2_dir : G.sun_dir;\n"
    "        float nl = dot(n, L.xyz);\n"
    "        if (L.w <= 0.0 || nl <= 0.0) continue;\n"
    "        float v = G.org.y > 0.5 && dot(L.xyz, S.dir.xyz) > 0.98 ? gi_cascade(X, n) : -1.0;\n"
    "        if (v < 0.0) {\n"
    "          rayQueryEXT sq;\n"
    "          rayQueryInitializeEXT(sq, gas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, X, 0.0, L.xyz, 2000.0);\n"
    "          while (rayQueryProceedEXT(sq)) {}\n"
    "          v = rayQueryGetIntersectionTypeEXT(sq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;\n"
    "        }\n"
    "        E += (k != 0 ? G.sun2_col.rgb : G.sun_col.rgb) * (nl * v);\n"
    "      }\n"
    "      for (uint i = 0u; i < G.n.w; ++i) {\n"
    "        GL lt = LT[i];\n"
    "        vec3 v = lt.pos.xyz - X;\n"
    "        float dist = length(v);\n"
    "        if (dist >= lt.pos.w) continue;\n"
    "        float qq = dist / lt.pos.w, win = sat(1.0 - qq * qq * qq * qq);\n" /* the range's edge, smoothed */
    "        float a = win * win / max(lt.att.x + lt.att.y * dist + lt.att.z * dist * dist, 1e-6);\n"
    "        E += lt.ambient.rgb * a + lt.diffuse.rgb * a * max(dot(n, v / max(dist, 1e-6)), 0.0);\n"
    "      }\n"
    "      if (G.tr.z > 0.0) E += gi_at(X, n).rgb * G.tr.z;\n" /* the bounces before */
    "      res = vec4(alb.rgb * E, t);\n"
    "    }\n"
    "  }\n"
    "  RB[u * G.n.x + r] = res;\n"
    "}\n"
    "#else\n"
    /* GI_BLEND: a probe's map (GI_TS texels a side, a border of one round N inside) from its rays: texel tid of update
     * slot gid; the border copied from the inside as the octahedron folds (bilinear lookups wrap across its edges).
     * GI_DIS 1: the distance map (mean and square, sharp weights), else the irradiance (cosine-weighted, and the sky's
     * share). The irradiance's pass also marks the probe traced, and inside something when a quarter of its rays hit
     * backfaces */
    "layout(local_size_x = GI_TS, local_size_y = GI_TS) in;\n"
    "layout(std430, set = 0, binding = 5) readonly buffer RBB { vec4 RB[]; };\n"
    "layout(set = 0, binding = 15, rgba16f) uniform image2D m;\n"
    "shared vec4 tg[GI_TS * GI_TS];\n"
    "void main() {\n"
    "  const int N = GI_TS - 2;\n"
    "  uvec2 tid = gl_LocalInvocationID.xy;\n"
    "  uint u = gl_WorkGroupID.x, l = u * G.n.y + G.n.z;\n"
    "  if (l >= uint(G.dims.w)) return;\n" /* (the whole group) */
    "  ivec3 s = ivec3(int(l) % G.dims.x, (int(l) / G.dims.x) % G.dims.y, int(l) / (G.dims.x * G.dims.y));\n"
    "  ivec2 org = ivec2(s.x + G.dims.x * s.y, s.z) * GI_TS;\n"
    "  bool fresh = (PS[l] & 1u) == 0u;\n"
    "  if (tid.x >= 1u && tid.y >= 1u && tid.x <= uint(N) && tid.y <= uint(N)) {\n"
    "    vec3 dir = gi_unoct(((vec2(tid) - 0.5) / float(N)) * 2.0 - 1.0);\n"
    "    vec4 sum = vec4(0.0);\n"
    "    float ws = 0.0;\n"
    "    for (uint r = 0u; r < G.n.x; ++r) {\n"
    "      vec4 R = RB[u * G.n.x + r];\n"
    "      float c = max(dot(dir, gi_ray(r)), 0.0);\n"
    "#if GI_DIS\n"
    "      float w = pow(c, 50.0), dd = min(abs(R.w), G.tr.x);\n"
    "      sum += vec4(dd, dd * dd, 0.0, 0.0) * w;\n"
    "      ws += w;\n"
    "#else\n"
    "      if (R.w < 0.0) continue;\n" /* backfaces bring nothing */
    "      sum += vec4(R.rgb, R.w >= 1e3 ? 1.0 : 0.0) * c;\n"
    "      ws += c;\n"
    "#endif\n"
    "    }\n"
    "    vec4 nv = ws > 1e-6 ? sum / ws : vec4(0.0);\n"
    "    vec4 old = imageLoad(m, org + ivec2(tid));\n"
    "    tg[tid.y * uint(GI_TS) + tid.x] = fresh ? nv : mix(nv, old, G.tr.y);\n"
    "  }\n"
    "  barrier();\n"
    "  uint x = tid.x, y = tid.y, e = uint(GI_TS - 1), sx = x, sy = y;\n"
    "  if ((x == 0u || x == e) && (y == 0u || y == e)) { sx = x == 0u ? uint(N) : 1u; sy = y == 0u ? uint(N) : 1u; }\n"
    "  else if (y == 0u || y == e) { sx = e - x; sy = y == 0u ? 1u : uint(N); }\n"
    "  else if (x == 0u || x == e) { sx = x == 0u ? 1u : uint(N); sy = e - y; }\n"
    "  imageStore(m, org + ivec2(tid), tg[sy * uint(GI_TS) + sx]);\n"
    "#if !GI_DIS\n"
    "  if (x == 0u && y == 0u) {\n"
    "    uint bf = 0u;\n"
    "    for (uint r = 0u; r < G.n.x; ++r) bf += RB[u * G.n.x + r].w < 0.0 ? 1u : 0u;\n"
    "    PS[l] = 1u | (bf * 4u > G.n.x ? 2u : 0u);\n"
    "  }\n"
    "#endif\n"
    "}\n"
    "#endif\n"
    "#endif\n";

/* the kernels' bindings (GI_KERNELS_GLSL), pushed */
enum
{
    GB_GU = 0,
    GB_PS = 1,
    GB_PC = 2,
    GB_ATTR = 3,
    GB_LT = 4,
    GB_RB = 5,
    GB_AS = 6,
    GB_SH = 7,
    GB_IR = 8,
    GB_DS = 9,
    GB_VB = 10,
    GB_TX = 11,
    GB_POS = 12,
    GB_FI = 13,
    GB_ZM = 14,
    GB_IMG = 15,
    GB_COUNT = 16,
};

static VkPipeline gi_kernel(const char* defines)
{
    VkPipeline p = compute_pipeline(defines, GI_KERNELS_GLSL, g_gi.layout);
    if (!p)
        fprintf(stderr, "[recomp] gfx: scene bounce light: its kernel (%s) failed\n", defines);
    return p;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src_stage,
    VkAccessFlags src, VkPipelineStageFlags dst_stage, VkAccessFlags dst);

/* an atlas (or the stand-in for the shadows' zone layer: array 1): RGBA16F, sampled and stored, zeroed */
static GfxTex* gi_image(uint32_t w, uint32_t h, int array)
{
    return tex_make(GFX_TEX_2D, F_A8R8G8B8, w, h, 1, GFX_USE_SAMPLE, VK_FORMAT_R16G16B16A16_SFLOAT, 1, array ? 1 : 0,
        VK_IMAGE_USAGE_STORAGE_BIT);
}

static int gi_init(void)
{
    if (g_gi.tried)
        return g_gi.ok;
    g_gi.tried = 1;
    if (!g_has_rt || !rt_init())
    {
        fprintf(stderr, "[recomp] gfx: scene bounce light: this GPU has no ray queries\n");
        return 0;
    }
    static const VkDescriptorType types[GB_COUNT] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    };
    VkDescriptorSetLayoutBinding b[GB_COUNT];
    for (uint32_t i = 0; i < GB_COUNT; ++i)
        b[i] = (VkDescriptorSetLayoutBinding){ i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    push_layout(b, GB_COUNT, NULL, &g_gi.dsl, &g_gi.layout);
    if (!g_gi.layout)
        return 0;
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(g_dev, &si, NULL, &g_gi.rep));
    g_gi.flatten = gi_kernel("#define GI_FLATTEN\n");
    g_gi.scroll = gi_kernel("#define GI_SCROLL\n");
    g_gi.trace = gi_kernel("#define GI_TRACE\n");
    g_gi.bdis = gi_kernel("#define GI_BLEND\n#define GI_DIS 1\n#define GI_TS 16\n");
    g_gi.birr = gi_kernel("#define GI_BLEND\n#define GI_DIS 0\n#define GI_TS 8\n");
    g_gi.nozm = gi_image(1, 1, 1);
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, qi.queryCount = 1;
    VK_CHECK(vkCreateQueryPool(g_dev, &qi, NULL, &g_gi.csize));
    g_gi.ok = g_gi.flatten && g_gi.scroll && g_gi.trace && g_gi.bdis && g_gi.birr && g_gi.rep && g_gi.nozm;
    if (!g_gi.ok)
        atomic_fetch_add(&g_failures, 1);
    return g_gi.ok;
}

static void gi_push(VkCommandBuffer cb, const VkWriteDescriptorSet* w, uint32_t n)
{
    p_push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_gi.layout, 0, n, w);
}

/* one instance of blas, unmoved, both faces, into d */
static void tlas_instance(VkAccelerationStructureInstanceKHR* d, VkAccelerationStructureKHR blas, uint32_t custom, uint32_t mask)
{
    memset(d, 0, sizeof *d);
    d->transform.matrix[0][0] = d->transform.matrix[1][1] = d->transform.matrix[2][2] = 1.0f;
    d->instanceCustomIndex = custom, d->mask = mask;
    d->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    ai.accelerationStructure = blas;
    d->accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(g_dev, &ai);
}

/* a top-level structure's build over the instances at addr: its geometry (tg) and what tl builds of it (for fast tracing) */
static void tlas_info(VkAccelerationStructureGeometryKHR* tg, VkAccelerationStructureBuildGeometryInfoKHR* tl, VkDeviceAddress addr)
{
    *tg = (VkAccelerationStructureGeometryKHR){ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tg->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tg->geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tg->geometry.instances.data.deviceAddress = addr;
    *tl = (VkAccelerationStructureBuildGeometryInfoKHR){ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    tl->type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tl->flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tl->mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tl->geometryCount = 1, tl->pGeometries = tg;
}

/* the zone's structure let go (once the GPU is past it) */
static void gi_drop(void)
{
    if (g_gi.blas)
        trash(3, (uint64_t)(uintptr_t)g_gi.blas, VK_NULL_HANDLE), g_gi.blas = VK_NULL_HANDLE;
    if (g_gi.tlas)
        trash(3, (uint64_t)(uintptr_t)g_gi.tlas, VK_NULL_HANDLE), g_gi.tlas = VK_NULL_HANDLE;
    rt_free(&g_gi.blas_buf), rt_free(&g_gi.tlas_buf), rt_free(&g_gi.attr), rt_free(&g_gi.inst);
    g_gi.ntri = 0;
}

void gfx_gi_zone(const GfxGiZone* z)
{
    if (!g_dev)
        return;
    gi_drop();
    g_gi.updated = 0;
    g_gi.clear = 1; /* every probe afresh */
    if (!z || !z->nitem || !gi_init())
        return;
    uint64_t t0 = gfx_now_ns();
    uint32_t ntri = 0;
    for (uint32_t i = 0; i < z->nitem; ++i)
        if (z->item[i].vb && z->item[i].inst < z->nworld)
            ntri += z->item[i].count;
    if (!ntri)
        return;
    const VkBufferUsageFlags SB = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    RtBuf pos, scratch;
    memset(&pos, 0, sizeof pos), memset(&scratch, 0, sizeof scratch);
    if (rt_buffer_ex(&pos, (VkDeviceSize)ntri * 36, SB | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 0, 1) < 0 ||
        rt_buffer_ex(&g_gi.attr, (VkDeviceSize)ntri * 8, SB, 0, 1) < 0)
    {
        rt_free(&pos), gi_drop();
        return;
    }
    flush_pass();
    gpu_sync();
    VkCommandBuffer cb = g_cb;
    /* the triangles, an item at a time */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_gi.flatten);
    VkDescriptorBufferInfo pbi = { pos.b, 0, VK_WHOLE_SIZE }, abi = { g_gi.attr.b, 0, VK_WHOLE_SIZE };
    struct
    {
        float world[16], inv[16];
        uint32_t first, count, prim, tri, flags, level, pad[2];
    } fi;
    uint32_t at = 0;
    for (uint32_t i = 0; i < z->nitem; ++i)
    {
        const GfxWorldItem* it = &z->item[i];
        if (!it->vb || it->inst >= z->nworld)
            continue;
        memcpy(fi.world, z->world + 16 * (size_t)it->inst, 64);
        memcpy(fi.inv, z->world_inv + 16 * (size_t)it->inst, 64);
        fi.first = it->first, fi.count = it->count, fi.prim = it->prim, fi.tri = at;
        fi.flags = (it->flags & GFX_WORLD_TWOSIDED) ? 2u : 0u, fi.level = 0, fi.pad[0] = fi.pad[1] = 0;
        GfxTex* t = it->tex && it->tex->view && it->tex->type == GFX_TEX_2D ? it->tex : NULL;
        if (t)
        {
            fi.flags |= 1u;
            /* a level some 8 texels across, when the chain is all there */
            uint32_t lv = 0, w = t->w < t->h ? t->w : t->h;
            while (w > 8 && lv + 1 < t->levels)
                w >>= 1, ++lv;
            fi.level = t->levels > 1 && t->levels < 32 && t->filled == (1u << t->levels) - 1 ? lv : 0;
            t->used = g_serial, t->rec = g_cb_index;
        }
        VkDescriptorBufferInfo fbi = ring_put(&fi, sizeof fi), vbi = { it->vb->b, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo ti = { g_gi.rep, t ? t->view : g_dummy2d->view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w[5];
        desc_write(&w[0], GB_VB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &vbi, NULL);
        desc_write(&w[1], GB_TX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &ti);
        desc_write(&w[2], GB_POS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &pbi, NULL);
        desc_write(&w[3], GB_ATTR, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &abi, NULL);
        desc_write(&w[4], GB_FI, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &fbi, NULL);
        gi_push(cb, w, 5);
        vkCmdDispatch(cb, (it->count + 63) / 64, 1, 1);
        it->vb->used = g_serial;
        at += it->count;
    }
    full_barrier(cb);
    /* one structure over them (compacted, as on Metal), one instance of it */
    VkAccelerationStructureGeometryKHR g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    VkAccelerationStructureGeometryTrianglesDataKHR* td = &g.geometry.triangles;
    td->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    td->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    td->vertexData.deviceAddress = pos.addr;
    td->vertexStride = 12;
    td->maxVertex = 3 * ntri - 1;
    td->indexType = VK_INDEX_TYPE_NONE_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR bl = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bl.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bl.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | (g_gi.csize ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0);
    bl.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bl.geometryCount = 1, bl.pGeometries = &g;
    VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bl, &ntri, &sz);
    VkDeviceSize al = g_ray.scratch_align;
    if (!rt_structure_ex(&g_gi.blas, &g_gi.blas_buf, sz.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, 1) ||
        rt_buffer_ex(&scratch, sz.buildScratchSize + al, SB, 0, 1) < 0)
    {
        fprintf(stderr, "[recomp] gfx: scene bounce light: no room for the zone's structure (%u triangles)\n", ntri);
        rt_free(&pos), rt_free(&scratch), gi_drop();
        return;
    }
    bl.dstAccelerationStructure = g_gi.blas;
    bl.scratchData.deviceAddress = (scratch.addr + al - 1) / al * al;
    VkAccelerationStructureBuildRangeInfoKHR br = { ntri, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* brp = &br;
    vkCmdBuildAccelerationStructuresKHR(cb, 1, &bl, &brp);
    full_barrier(cb);
    if (g_gi.csize)
    {
        vkCmdResetQueryPool(cb, g_gi.csize, 0, 1);
        vkCmdWriteAccelerationStructuresPropertiesKHR(cb, 1, &g_gi.blas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, g_gi.csize, 0);
    }
    g_dirty = 1;
    uint64_t t1 = gfx_now_ns();
    submit(1); /* once a zone, while it loads: the corners and the scratch let go after, the compacted size read */
    uint64_t t2 = gfx_now_ns();
    rt_free(&pos), rt_free(&scratch);
    VkDeviceSize full = g_gi.blas_buf.size, csize = 0;
    if (g_gi.csize)
        vkGetQueryPoolResults(g_dev, g_gi.csize, 0, 1, sizeof csize, &csize, sizeof csize, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    cb = cmd();
    gpu_sync();
    if (csize && csize < full)
    {
        VkAccelerationStructureKHR small = VK_NULL_HANDLE;
        RtBuf sbuf;
        memset(&sbuf, 0, sizeof sbuf);
        if (rt_structure_ex(&small, &sbuf, csize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, 1))
        {
            VkCopyAccelerationStructureInfoKHR ci = { VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
            ci.src = g_gi.blas, ci.dst = small, ci.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            vkCmdCopyAccelerationStructureKHR(cb, &ci);
            full_barrier(cb);
            trash(3, (uint64_t)(uintptr_t)g_gi.blas, VK_NULL_HANDLE), rt_free(&g_gi.blas_buf);
            g_gi.blas = small, g_gi.blas_buf = sbuf;
        }
        else
            rt_free(&sbuf);
    }
    if (rt_buffer_ex(&g_gi.inst, sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1, 1) < 0)
    {
        gi_drop();
        return;
    }
    tlas_instance((VkAccelerationStructureInstanceKHR*)g_gi.inst.p, g_gi.blas, 0, 0xFF);
    VkAccelerationStructureGeometryKHR tg;
    VkAccelerationStructureBuildGeometryInfoKHR tl;
    tlas_info(&tg, &tl, g_gi.inst.addr);
    uint32_t one = 1;
    VkAccelerationStructureBuildSizesInfoKHR tsz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tl, &one, &tsz);
    if (!rt_structure_ex(&g_gi.tlas, &g_gi.tlas_buf, tsz.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, 1) ||
        rt_buffer_ex(&scratch, tsz.buildScratchSize + al, SB, 0, 1) < 0)
    {
        rt_free(&scratch), gi_drop();
        return;
    }
    tl.dstAccelerationStructure = g_gi.tlas;
    tl.scratchData.deviceAddress = (scratch.addr + al - 1) / al * al;
    VkAccelerationStructureBuildRangeInfoKHR tr = { 1, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
    vkCmdBuildAccelerationStructuresKHR(cb, 1, &tl, &trp);
    full_barrier(cb);
    rt_free(&scratch); /* (let go once the GPU is past this frame) */
    g_dirty = 1;
    g_gi.ntri = ntri;
    fprintf(stderr, "[recomp] gfx: scene bounce light: the zone's structure: %u items, %u triangles, %.1f MB (%.1f MB before "
                    "compaction) + %.1f MB the triangles' colours and normals, %.1f ms recorded, %.1f ms to the build's end (with the "
                    "frame's work before it), %.1f ms in all\n",
        z->nitem, ntri, (double)g_gi.blas_buf.size / 1048576.0, (double)full / 1048576.0, (double)ntri * 8.0 / 1048576.0,
        (double)(t1 - t0) / 1e6, (double)(t2 - t0) / 1e6, (double)(gfx_now_ns() - t0) / 1e6);
}

/* the grid at each quality (1 low, 2 high): cells each way, world units between them, rays a probe, 1 in how many
 * traced a frame, hysteresis */
typedef struct GiPreset
{
    int32_t d[3];
    float sp;
    uint32_t rays, stride;
    float hyst;
} GiPreset;
static const GiPreset GI_PRESETS[2] = { { { 24, 8, 24 }, 4.0f, 32, 16, 0.85f }, { { 32, 12, 32 }, 3.0f, 64, 12, 0.9f } };

static void gi_probes_free(void)
{
    gfx_tex_destroy(g_gi.irr), gfx_tex_destroy(g_gi.dis);
    g_gi.irr = g_gi.dis = NULL;
    rt_free(&g_gi.ps), rt_free(&g_gi.pc), rt_free(&g_gi.rb);
}

static int gi_probes_make(uint32_t q);

/* the probes at quality q, or the next lower that can be made (high's distance atlas is 6144 across, past some GPUs'
 * limit; one that failed is not tried again): g_gi.quality */
static int gi_probes(uint32_t q)
{
    for (; q >= 1; --q)
    {
        if (g_gi.irr && g_gi.quality == q)
            return 1;
        int over = q >= 2 && (uint32_t)(GI_PRESETS[1].d[0] * GI_PRESETS[1].d[1]) * (GI_DIS + 2) > g_props.limits.maxImageDimension2D;
        if (!over && !(g_gi.failed & (1u << q)) && gi_probes_make(q))
            return 1;
    }
    return 0;
}

/* the probes at quality q made in place of what was there; 0 when they could not be (remembered in g_gi.failed) */
static int gi_probes_make(uint32_t q)
{
    gi_probes_free();
    const int32_t* d = GI_PRESETS[q >= 2].d;
    uint32_t rays = GI_PRESETS[q >= 2].rays, stride = GI_PRESETS[q >= 2].stride;
    float sp = GI_PRESETS[q >= 2].sp;
    uint32_t np = (uint32_t)(d[0] * d[1] * d[2]), slots = (np + stride - 1) / stride;
    g_gi.irr = gi_image((uint32_t)(d[0] * d[1]) * (GI_IRR + 2), (uint32_t)d[2] * (GI_IRR + 2), 0);
    g_gi.dis = gi_image((uint32_t)(d[0] * d[1]) * (GI_DIS + 2), (uint32_t)d[2] * (GI_DIS + 2), 0);
    const VkBufferUsageFlags SB = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!g_gi.irr || !g_gi.dis || rt_buffer(&g_gi.ps, np * 4, SB, 0) < 0 || rt_buffer(&g_gi.pc, np * 16, SB, 0) < 0 ||
        rt_buffer(&g_gi.rb, (VkDeviceSize)slots * rays * 16, SB, 0) < 0)
    {
        gi_probes_free();
        g_gi.failed |= 1u << q;
        fprintf(stderr, "[recomp] gfx: scene bounce light: its probes at quality %u could not be made\n", q);
        return 0;
    }
    g_gi.quality = q, g_gi.updated = 0, g_gi.phase = 0, g_gi.clear = 1;
    fprintf(stderr, "[recomp] gfx: scene bounce light: %dx%dx%d probes %.1f apart, %u rays, 1 in %u a frame, %.1f MB\n", d[0], d[1],
        d[2], (double)sp, rays, stride,
        (double)((d[0] * d[1] * (GI_IRR + 2)) * (d[2] * (GI_IRR + 2)) * 8 + (d[0] * d[1] * (GI_DIS + 2)) * (d[2] * (GI_DIS + 2)) * 8 +
                    np * 20 + slots * rays * 16) / 1048576.0);
    return 1;
}

static uint32_t gi_rand(void)
{
    g_gi.seed = g_gi.seed * 1664525u + 1013904223u;
    return g_gi.seed;
}

/* a random rotation (a random unit quaternion) into m, as columns */
static void gi_rotation(float* m)
{
    float r[3];
    for (int i = 0; i < 3; ++i)
        r[i] = (float)(gi_rand() >> 8) / 16777216.0f;
    float a = sqrtf(1.0f - r[0]), b = sqrtf(r[0]), t1 = 6.2831853f * r[1], t2 = 6.2831853f * r[2];
    float x = a * sinf(t1), y = a * cosf(t1), z = b * sinf(t2), w = b * cosf(t2);
    float R[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w), 1 - 2 * (x * x + z * z),
        2 * (y * z - x * w), 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) };
    memset(m, 0, 64);
    for (int c = 0; c < 3; ++c)
        for (int rr = 0; rr < 3; ++rr)
            m[c * 4 + rr] = R[rr * 3 + c];
    m[15] = 1.0f;
}

void gfx_gi_frame(const GfxGiFrame* f)
{
    g_gi.on = 0;
    if (!g_dev || !f || f->strength <= 0.0f || !g_gi.tlas || !gi_init())
        return;
    uint32_t q = f->quality >= 2 ? 2 : 1;
    g_gi.u.org[0] = f->quality >= 3 ? 1.0f : 0.0f;
    if (!gi_probes(q))
        return;
    GiU* u = &g_gi.u;
    const GiPreset* gp = &GI_PRESETS[g_gi.quality >= 2]; /* (what gi_probes made) */
    const int32_t* d = gp->d;
    float sp = gp->sp, hyst = gp->hyst;
    uint32_t rays = gp->rays, stride = gp->stride;
    u->org[3] = sp;
    for (int a = 0; a < 3; ++a)
        u->dims[a] = d[a], u->base[a] = (int32_t)floorf(f->cam[a] / sp + 0.5f) - d[a] / 2;
    u->base[3] = 1, u->dims[3] = d[0] * d[1] * d[2];
    u->k[0] = f->strength, u->k[1] = f->sky, u->k[2] = f->debug, u->k[3] = 0.25f * sp;
    u->inv[0] = 1.0f / (float)(d[0] * d[1] * (GI_IRR + 2)), u->inv[1] = 1.0f / (float)(d[2] * (GI_IRR + 2));
    u->inv[2] = 1.0f / (float)(d[0] * d[1] * (GI_DIS + 2)), u->inv[3] = 1.0f / (float)(d[2] * (GI_DIS + 2));
    if (g_gi.updated != g_serial) /* the probes traced once a frame, at its first batch */
    {
        u->tr[0] = 16.0f * sp, u->tr[1] = hyst, u->tr[2] = f->bounce, u->tr[3] = f->ambient_bounce;
        /* the sun's visibility at a hit from the shadows' zone layer when it is there (one read), else a ray */
        int zl = g_shd.on && g_shd.zmap;
        u->org[1] = zl ? 1.0f : 0.0f;
        uint32_t nl = f->nlight < 64 ? f->nlight : 64;
        u->n[0] = rays, u->n[1] = stride, u->n[2] = g_gi.phase, u->n[3] = nl;
        memcpy(u->sun_dir, f->sun_dir, 16), memcpy(u->sun_col, f->sun_col, 16);
        memcpy(u->sun2_dir, f->sun2_dir, 16), memcpy(u->sun2_col, f->sun2_col, 16), memcpy(u->amb, f->ambient, 16);
        gi_rotation(u->rot);
        g_gi.phase = (g_gi.phase + 1) % stride;
        g_gi.updated = g_serial;
        static uint32_t told;
        if (++told % 2000 == 1)
            fprintf(stderr, "[recomp] gfx: scene bounce light: grid at (%d, %d, %d), %u lamps, sun %.2f %.2f %.2f, second %.2f %.2f %.2f, "
                            "ambient %.2f %.2f %.2f, zone layer %s\n",
                u->base[0], u->base[1], u->base[2], nl, u->sun_col[0] * u->sun_dir[3], u->sun_col[1] * u->sun_dir[3],
                u->sun_col[2] * u->sun_dir[3], u->sun2_col[0] * u->sun2_dir[3], u->sun2_col[1] * u->sun2_dir[3],
                u->sun2_col[2] * u->sun2_dir[3], u->amb[0], u->amb[1], u->amb[2], zl ? "read" : "not there");
        uint32_t np = (uint32_t)u->dims[3], slots = (np + stride - 1) / stride;
        VkBuffer buf;
        VkDeviceSize off;
        GfxWorldLight none;
        memset(&none, 0, sizeof none);
        size_t lsz = (nl ? nl : 1) * sizeof(GfxWorldLight);
        VkDescriptorBufferInfo lbi = ring_put(nl ? f->light : &none, lsz);
        VkDescriptorBufferInfo ubi = ring_put(u, sizeof *u);
        VkDescriptorBufferInfo sbi;
        if (!shadow_uniforms(&sbi))
        {
            memset(ring(sizeof g_shd.u, RING_ALIGN, &buf, &off), 0, sizeof g_shd.u);
            sbi = (VkDescriptorBufferInfo){ buf, off, sizeof g_shd.u };
        }
        flush_pass();
        gpu_sync();
        VkCommandBuffer cb = g_cb;
        if (g_gi.clear)
        {
            vkCmdFillBuffer(cb, g_gi.ps.b, 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(cb, g_gi.pc.b, 0, VK_WHOLE_SIZE, 0);
            full_barrier(cb);
            g_gi.clear = 0;
        }
        int ts = ts_begin(cb, GFX_PASS_GI);
        VkDescriptorBufferInfo psbi = { g_gi.ps.b, 0, VK_WHOLE_SIZE }, pcbi = { g_gi.pc.b, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo abi = { g_gi.attr.b, 0, VK_WHOLE_SIZE }, rbi = { g_gi.rb.b, 0, VK_WHOLE_SIZE };
        GfxTex* zm = zl ? g_shd.zmap : g_gi.nozm;
        VkDescriptorImageInfo iri = { g_lin_samp, g_gi.irr->view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo dsi = { g_lin_samp, g_gi.dis->view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo zmi = { g_near_samp, zm->view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSetAccelerationStructureKHR wa;
        VkWriteDescriptorSet w[GB_COUNT];
        /* the cells new to the grid */
        desc_write(&w[0], GB_GU, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ubi, NULL);
        desc_write(&w[1], GB_PS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &psbi, NULL);
        desc_write(&w[2], GB_PC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &pcbi, NULL);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_gi.scroll);
        gi_push(cb, w, 3);
        vkCmdDispatch(cb, (np + 63) / 64, 1, 1);
        full_barrier(cb);
        /* the rays */
        desc_write(&w[2], GB_ATTR, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &abi, NULL);
        desc_write(&w[3], GB_LT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lbi, NULL);
        desc_write(&w[4], GB_RB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &rbi, NULL);
        as_write(&w[5], GB_AS, &wa, &g_gi.tlas);
        desc_write(&w[6], GB_SH, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &sbi, NULL);
        desc_write(&w[7], GB_IR, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &iri);
        desc_write(&w[8], GB_DS, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &dsi);
        desc_write(&w[9], GB_ZM, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &zmi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_gi.trace);
        gi_push(cb, w, 10);
        vkCmdDispatch(cb, (rays + 63) / 64, slots, 1);
        full_barrier(cb);
        /* the maps: the distances' first (the irradiance's pass marks the probes traced) */
        VkDescriptorImageInfo mi[2] = { { VK_NULL_HANDLE, g_gi.dis->view, VK_IMAGE_LAYOUT_GENERAL },
            { VK_NULL_HANDLE, g_gi.irr->view, VK_IMAGE_LAYOUT_GENERAL } };
        VkPipeline bp[2] = { g_gi.bdis, g_gi.birr };
        desc_write(&w[2], GB_RB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &rbi, NULL);
        for (int k = 0; k < 2; ++k)
        {
            desc_write(&w[3], GB_IMG, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, NULL, &mi[k]);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, bp[k]);
            gi_push(cb, w, 4);
            vkCmdDispatch(cb, slots, 1, 1);
            full_barrier(cb);
        }
        ts_end(cb, ts);
        g_gi.irr->used = g_gi.dis->used = zm->used = g_serial;
        g_gi.irr->rec = g_gi.dis->rec = zm->rec = g_cb_index;
    }
    g_gi.on = 1;
}

/* one caster to capture: what it draws, through which matrix (clip space to this frame's view space) */
typedef struct RtItem
{
    const Caster* c;
    float m[16];
    uint32_t tris;
    uint8_t alpha; /* traced through its alpha test (rt_alpha) */
} RtItem;

/* traced through its alpha test: the zone's and its placed objects' (leaves, grass) whose vertex function
 * gives texture coordinates and whose first stage's texture is there (gfx_d3d12.c rt_alpha) */
static int rt_alpha(const Caster* c)
{
    return (c->fixed || c->keep) && alpha_tested(&c->lib.fs) && c->lib.vs.ntex >= 1 && c->tex[0] && c->up && c->lib.fs.st[0].tex == 1 &&
        !c->lib.fs.st[0].projected;
}

static uint32_t rt_tris(const Caster* c)
{
    if (c->prim == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        return c->n / 3;
    if (c->prim == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
        return c->n >= 3 ? c->n - 2 : 0;
    return 0;
}

static VkPipeline rt_pipeline(const Caster* c, int alpha)
{
    PipeKey pk;
    memset(&pk, 0, sizeof pk);
    pk.lib = c->lib;
    GfxVsKey* vk = &pk.lib.vs;
    vk->shadow = 2, vk->pixel = 0, vk->water = 0;
    /* the position alone - and an alpha test's texture coordinates and diffuse alpha (gfx_metal.m rt_pipeline) */
    vk->normalize = vk->localviewer = vk->specular = 0;
    vk->nlights = 0, memset(vk->light_type, 0, sizeof vk->light_type);
    vk->fog_vertex = vk->range_fog = 0, vk->flat = 0;
    if (!alpha)
    {
        vk->lighting = 0, vk->src_diffuse = vk->src_specular = vk->src_ambient = vk->src_emissive = 0;
        vk->ntex = 0, memset(vk->tci, 0, sizeof vk->tci), memset(vk->ttf, 0, sizeof vk->ttf);
    }
    memset(&pk.lib.fs, 0, sizeof pk.lib.fs);
    pk.color = VK_FORMAT_UNDEFINED, pk.depth = VK_FORMAT_UNDEFINED, pk.topo = 0; /* (points: one a corner) */
    return pipeline_for(&pk, c->vs, c->ps);
}


/* The world for this frame's rays (invP the projection's inverse, view the camera, cam where it is): 1
 * when it was made */
static int rt_capture(const float* invP, const float* view, const float* cam)
{
    static RtItem* items;
    static uint32_t cap;
    uint32_t n = 0, total = g_ncasters + g_ncache;
    if (!rt_init() || !total)
        return 0;
    if (cap < total)
        cap = total + total / 2, items = (RtItem*)realloc(items, cap * sizeof *items);
    uint64_t verts = 0;
    uint32_t ntex = 0;
    for (uint32_t i = 0; i < total; ++i)
    {
        RtItem* it = &items[n];
        if (i < g_ncasters)
        {
            it->c = &g_casters[i];
            memcpy(it->m, invP, 64);
        }
        else
        {
            Cached* ce = &g_cache[i - g_ncasters];
            if (ce->dead || cached_expired(ce) || !ce->c.n || !ce->c.ub || ce->seen == g_serial)
                continue;
            if (ce->c.has_pos)
            {
                float dx = ce->pos[0] - cam[0], dy = ce->pos[1] - cam[1], dz = ce->pos[2] - cam[2];
                if (dx * dx + dy * dy + dz * dz > RT_REACH * RT_REACH)
                    continue;
            }
            it->c = &ce->c;
            gfx_mat_mul(it->m, ce->clip_world, view);
        }
        if (!it->c->n || !(it->tris = rt_tris(it->c)))
            continue;
        it->alpha = (uint8_t)(rt_alpha(it->c) && ntex < RT_TEXS);
        ntex += it->alpha;
        if (!rt_pipeline(it->c, it->alpha))
        {
            g_ray.st_skipped++;
            continue;
        }
        verts += (uint64_t)it->tris * 3;
        n++;
    }
    if (!n || verts > (1u << 26))
        return 0;
    /* the solid first, then the alpha-tested: the two structures */
    {
        static RtItem* tmp;
        static uint32_t tcap;
        if (tcap < n)
            tcap = cap, tmp = (RtItem*)realloc(tmp, tcap * sizeof *tmp);
        uint32_t k = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (uint32_t i = 0; i < n; ++i)
                if (items[i].alpha == pass)
                    tmp[k++] = items[i];
        memcpy(items, tmp, n * sizeof *items);
    }
    const VkBufferUsageFlags SB = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (rt_buffer(&g_ray.tri, verts * 32, SB | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 0) < 0)
        return 0;
    /* the capture: a pass with nothing to draw into, each caster's corners written by its function */
    end_pass();
    gpu_sync();
    VkCommandBuffer cb = g_cb;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ 1, 1 };
    ri.layerCount = 1;
    vkCmdBeginRendering(cb, &ri);
    VkViewport v = { 0, 0, 1, 1, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { 1, 1 } };
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &sc);
    VkBuffer mb = VK_NULL_HANDLE, live_mb = VK_NULL_HANDLE;
    VkDeviceSize moff = 0, live_moff = 0;
    VkPipeline bound = VK_NULL_HANDLE;
    uint64_t vsolid = 0, valpha = 0;
    uint32_t first = 0;
    VkDeviceSize ialign = g_props.limits.minStorageBufferOffsetAlignment;
    for (uint32_t i = 0; i < n; ++i)
    {
        const RtItem* it = &items[i];
        const Caster* cs = it->c;
        int live = cs >= g_casters && cs < g_casters + g_ncasters;
        if (live && !live_mb)
            memcpy(ring(64, RING_ALIGN, &live_mb, &live_moff), it->m, 64);
        if (live)
            mb = live_mb, moff = live_moff;
        else
            memcpy(ring(64, RING_ALIGN, &mb, &moff), it->m, 64);
        VkPipeline ps = rt_pipeline(cs, it->alpha);
        if (ps != bound)
        {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ps), bound = ps;
            /* (rasterization off: the depth and stencil states are none of the pipeline's) */
            vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
            vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
            vkCmdSetDepthBiasEnable(cb, VK_FALSE);
            vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
            vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
        }
        /* where and how (emit_rt_index): the first corner written, the indices' size, a strip, the first
         * vertex or index (the indices bound from an aligned offset: the rest counted here) */
        VkDeviceSize iat = 0;
        uint32_t skip = 0;
        if (cs->itype)
            iat = cs->ioff / ialign * ialign, skip = (uint32_t)((cs->ioff - iat) / cs->itype);
        uint32_t rtc[4] = { first, cs->itype, cs->prim == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, cs->itype ? skip : cs->vstart };
        VkBuffer cbuf;
        VkDeviceSize coff;
        memcpy(ring(16, RING_ALIGN, &cbuf, &coff), rtc, 16);
        VkWriteDescriptorSet w[GFX_NSTREAMS + 5];
        VkDescriptorBufferInfo bi[GFX_NSTREAMS + 5];
        uint32_t nw = 0;
        bi[0] = (VkDescriptorBufferInfo){ cs->ub, cs->uoff, sizeof(GfxU) };
        bi[1] = (VkDescriptorBufferInfo){ mb, moff, 64 };
        bi[2] = (VkDescriptorBufferInfo){ g_ray.tri.b, 0, VK_WHOLE_SIZE };
        bi[3] = (VkDescriptorBufferInfo){ cbuf, coff, 16 };
        bi[4] = cs->itype ? (VkDescriptorBufferInfo){ cs->ib, iat, VK_WHOLE_SIZE } : (VkDescriptorBufferInfo){ g_dummy, 0, VK_WHOLE_SIZE };
        desc_write(&w[nw++], B_U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bi[0], NULL);
        desc_write(&w[nw++], B_SHADOW, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bi[1], NULL);
        desc_write(&w[nw++], B_RT_OUT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[2], NULL);
        desc_write(&w[nw++], B_RT_C, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bi[3], NULL);
        desc_write(&w[nw++], B_RT_IDX, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[4], NULL);
        for (int st = 0; st < GFX_NSTREAMS; ++st)
        {
            bi[5 + st] = cs->vb[st] ? (VkDescriptorBufferInfo){ cs->vb[st], cs->voff[st], VK_WHOLE_SIZE }
                                    : (VkDescriptorBufferInfo){ g_dummy, 0, VK_WHOLE_SIZE };
            desc_write(&w[nw++], B_STREAM0 + (uint32_t)st, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[5 + st], NULL);
        }
        p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, nw, w);
        vkCmdDraw(cb, it->tris * 3, 1, 0, 0);
        first += it->tris * 3;
        *(it->alpha ? &valpha : &vsolid) += (uint64_t)it->tris * 3;
        if (live)
            g_ray.st_live++;
        else
            g_ray.st_cached++;
    }
    vkCmdEndRendering(cb);
    g_bound = VK_NULL_HANDLE;
    full_barrier(cb); /* the corners written, before the structures' builds read them */
    if (!vsolid) /* (the bounce light's and occlusion's rays trace the solid ones) */
        return 0;
    /* the alpha tests' table and textures: each one's first triangle, its texture (its index in set 1), stage
     * 0's alpha op and arguments, its test with the texture factor's alpha */
    uint32_t nalpha = 0, at = (uint32_t)(vsolid / 3);
    for (uint32_t i = 0; i < n; ++i)
        nalpha += items[i].alpha;
    RtBuf* tab = &g_ray.tab[g_frame];
    if (rt_buffer(tab, (VkDeviceSize)(nalpha ? nalpha : 1) * 16, SB, 1) < 0)
        return 0;
    if (g_ray.pool_serial[g_frame] != g_serial)
        vkResetDescriptorPool(g_dev, g_ray.pool[g_frame], 0), g_ray.pool_serial[g_frame] = g_serial;
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = g_ray.pool[g_frame], dai.descriptorSetCount = 1, dai.pSetLayouts = &g_ray.tex_dsl;
    if (vkAllocateDescriptorSets(g_dev, &dai, &g_ray.texset) != VK_SUCCESS)
        return 0;
    if (nalpha)
    {
        static VkDescriptorImageInfo* ii;
        static uint32_t icap;
        if (icap < nalpha)
            icap = nalpha, ii = (VkDescriptorImageInfo*)realloc(ii, icap * sizeof *ii);
        uint32_t* t = (uint32_t*)tab->p, k = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            const RtItem* it = &items[i];
            if (!it->alpha)
                continue;
            const Caster* c = it->c;
            const GfxStage* st = &c->lib.fs.st[0];
            uint32_t ops = c->lib.fs.prog ? 0 : (uint32_t)(st->aop & 63) << 2 | (uint32_t)st->aa1 << 8 | (uint32_t)st->aa2 << 16;
            GfxSampler sk = c->samp[0];
            ii[k] = (VkDescriptorImageInfo){ sampler(&sk), c->tex[0], VK_IMAGE_LAYOUT_GENERAL };
            t[0] = at, t[1] = k, t[2] = ops;
            t[3] = (uint32_t)c->lib.fs.alpha_func << 8 | (uint32_t)fminf(fmaxf(c->up->params[1], 0.0f), 255.0f) |
                (uint32_t)(fminf(fmaxf(c->up->tfactor[3], 0.0f), 1.0f) * 255.0f + 0.5f) << 16;
            t += 4, at += it->tris, k++;
        }
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = g_ray.texset, w.dstBinding = 0, w.descriptorCount = nalpha;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w.pImageInfo = ii;
        vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
    }
    /* a structure over the solid triangles and one over the alpha-tested, and the one over both: two
     * instances, masks 1 and 2 (the bounce light's and occlusion's rays, mask 1, skip the leaves whole) */
    VkAccelerationStructureGeometryKHR g[2];
    VkAccelerationStructureBuildGeometryInfoKHR bl[2], tl;
    VkAccelerationStructureBuildRangeInfoKHR br[2], tr;
    VkDeviceSize scratch = 0, soff[2] = { 0, 0 };
    memset(g, 0, sizeof g), memset(bl, 0, sizeof bl), memset(&tl, 0, sizeof tl), memset(br, 0, sizeof br);
    uint32_t ninst = 0, nbl = 0;
    VkAccelerationStructureBuildGeometryInfoKHR bl_go[2];
    const VkAccelerationStructureBuildRangeInfoKHR* br_go[2];
    for (int k = 0; k < 2; ++k)
    {
        uint64_t from = k ? vsolid : 0, count = k ? valpha : vsolid;
        if (!count)
            continue;
        g[k].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g[k].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g[k].flags = k ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
        VkAccelerationStructureGeometryTrianglesDataKHR* td = &g[k].geometry.triangles;
        td->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        td->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        td->vertexData.deviceAddress = g_ray.tri.addr + from * 32;
        td->vertexStride = 32;
        td->maxVertex = (uint32_t)count - 1;
        td->indexType = VK_INDEX_TYPE_NONE_KHR;
        bl[k].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        bl[k].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bl[k].flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        bl[k].mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bl[k].geometryCount = 1, bl[k].pGeometries = &g[k];
        uint32_t prims = (uint32_t)(count / 3);
        VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        vkGetAccelerationStructureBuildSizesKHR(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bl[k], &prims, &sz);
        if (!rt_structure(&g_ray.blas[k], &g_ray.blas_buf[k], sz.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR))
            return 0;
        bl[k].dstAccelerationStructure = g_ray.blas[k];
        soff[k] = scratch, scratch += (sz.buildScratchSize + g_ray.scratch_align - 1) / g_ray.scratch_align * g_ray.scratch_align;
        br[k].primitiveCount = prims;
        bl_go[nbl] = bl[k], br_go[nbl] = &br[k], nbl++;
    }
    /* the instances: the solid (custom index 0, mask 1), the alpha-tested (1, mask 2) */
    RtBuf* ib = &g_ray.inst[g_frame];
    if (rt_buffer(ib, 2 * sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1) < 0)
        return 0;
    VkAccelerationStructureInstanceKHR* inst = (VkAccelerationStructureInstanceKHR*)ib->p;
    memset(inst, 0, 2 * sizeof *inst);
    for (int k = 0; k < 2; ++k)
        if (bl[k].geometryCount)
            tlas_instance(&inst[ninst++], g_ray.blas[k], (uint32_t)k, k ? 2 : 1);
    VkAccelerationStructureGeometryKHR tg;
    tlas_info(&tg, &tl, ib->addr);
    VkAccelerationStructureBuildSizesInfoKHR tsz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tl, &ninst, &tsz);
    if (!rt_structure(&g_ray.tlas, &g_ray.tlas_buf, tsz.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR))
        return 0;
    tl.dstAccelerationStructure = g_ray.tlas;
    VkDeviceSize tscratch = scratch;
    scratch += tsz.buildScratchSize + g_ray.scratch_align;
    if (rt_buffer(&g_ray.scratch, scratch, SB, 0) < 0)
        return 0;
    VkDeviceAddress sbase = (g_ray.scratch.addr + g_ray.scratch_align - 1) / g_ray.scratch_align * g_ray.scratch_align;
    for (uint32_t k = 0, j = 0; k < 2; ++k)
        if (bl[k].geometryCount)
            bl_go[j++].scratchData.deviceAddress = sbase + soff[k];
    vkCmdBuildAccelerationStructuresKHR(cb, nbl, bl_go, br_go);
    full_barrier(cb); /* (the top one stands on them) */
    tl.scratchData.deviceAddress = sbase + tscratch;
    tr = (VkAccelerationStructureBuildRangeInfoKHR){ ninst, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
    vkCmdBuildAccelerationStructuresKHR(cb, 1, &tl, &trp);
    /* the corners' smooth normals (RT_NORMALS_GLSL) */
    uint32_t slots = 1024;
    while (slots < verts * 2u && slots < (1u << 26))
        slots *= 2;
    if (rt_buffer(&g_ray.nrm, verts * 16, SB, 0) < 0 || rt_buffer(&g_ray.table, (VkDeviceSize)slots * 16, SB, 0) < 0)
        return 0;
    full_barrier(cb);
    VkDescriptorBufferInfo cbi[3] = { { g_ray.table.b, 0, VK_WHOLE_SIZE }, { g_ray.tri.b, 0, VK_WHOLE_SIZE }, { g_ray.nrm.b, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet cw[3];
    for (uint32_t i = 0; i < 3; ++i)
        desc_write(&cw[i], i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cbi[i], NULL);
    uint32_t nc[4] = { (uint32_t)(verts / 3), slots - 1, (uint32_t)verts, 0 };
    VkPipeline passes[3] = { g_ray.nclear, g_ray.nsum, g_ray.nresolve };
    uint32_t counts[3] = { slots, (uint32_t)(verts / 3), (uint32_t)verts };
    for (int i = 0; i < 3; ++i)
    {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, passes[i]);
        p_push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_ray.clayout, 0, 3, cw);
        vkCmdPushConstants(cb, g_ray.clayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, nc);
        vkCmdDispatch(cb, (counts[i] + 255) / 256, 1, 1);
        full_barrier(cb);
    }
    g_dirty = 1;
    g_ray.rtp[0] = (uint32_t)(vsolid / 3), g_ray.rtp[1] = nalpha, g_ray.rtp[2] = g_ray.rtp[3] = 0;
    g_ray.verts = (uint32_t)verts;
    g_ray.st_tris += verts / 3, g_ray.st_frames++;
    return 1;
}

/* one of ray tracing's passes (fx_pass's, with the world bound: RT_GLSL's 10-13 and set 1) */
static void rt_pass(GfxTex* target, int load, VkPipeline p, const float* vp, const VkImageView* in, int n, const int32_t* dir)
{
    end_pass();
    gpu_sync();
    VkCommandBuffer cb = g_cb;
    uint32_t tw, th;
    level_size(target, 0, &tw, &th);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = attachment_view(target, 0, 0);
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ tw, th };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    int ts = ts_begin(cb, g_pass);
    vkCmdBeginRendering(cb, &ri);
    VkViewport v = vp ? (VkViewport){ vp[0], vp[1], vp[2], vp[3], 0, 1 } : (VkViewport){ 0, 0, (float)tw, (float)th, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { tw, th } };
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkWriteDescriptorSet w[14];
    VkDescriptorBufferInfo bi[4] = { { g_fx.ub, g_fx.uoff, sizeof(FxU) }, { g_ray.tri.b, 0, VK_WHOLE_SIZE }, { g_ray.nrm.b, 0, VK_WHOLE_SIZE },
        { g_ray.tab[g_frame].b, 0, VK_WHOLE_SIZE } };
    VkDescriptorImageInfo ii[9];
    desc_write(&w[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bi[0], NULL);
    for (int i = 0; i < 9; ++i)
    {
        int t = i < 6 ? i : i == 8 ? 6 : -1;
        ii[i] = t >= 0 ? (VkDescriptorImageInfo){ g_lin_samp, t < n && in[t] ? in[t] : g_dummy2d->view, VK_IMAGE_LAYOUT_GENERAL }
                       : (VkDescriptorImageInfo){ g_cmp_samp, g_fx.sdummy->view, VK_IMAGE_LAYOUT_GENERAL };
        w[1 + i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[1 + i].dstBinding = 1 + (uint32_t)i, w[1 + i].descriptorCount = 1;
        w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[1 + i].pImageInfo = &ii[i];
    }
    VkWriteDescriptorSetAccelerationStructureKHR wa;
    as_write(&w[10], 10, &wa, &g_ray.tlas);
    for (uint32_t i = 0; i < 3; ++i)
        desc_write(&w[11 + i], 11 + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[1 + i], NULL);
    p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ray.layout, 0, 14, w);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ray.layout, 1, 1, &g_ray.texset, 0, NULL);
    uint32_t pc[8] = { dir ? (uint32_t)dir[0] : 0, dir ? (uint32_t)dir[1] : 0, 0, 0, g_ray.rtp[0], g_ray.rtp[1], g_ray.rtp[2], g_ray.rtp[3] };
    vkCmdPushConstants(cb, g_ray.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    ts_end(cb, ts);
    g_dirty = 1;
    g_bound = VK_NULL_HANDLE;
    target->used = g_serial, target->rec = g_cb_index;
}

void gfx_trace_dump(const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "frame own-sun strength day live cached no-pipeline map-across sun(x y z) camera(x y z) depth(1 as drawn, 2 another since) sun-light ambient fog(r g b)\n");
    uint32_t n = g_fx.ntrace < 1200 ? g_fx.ntrace : 1200;
    for (uint32_t k = g_fx.ntrace - n; k < g_fx.ntrace; ++k)
    {
        __typeof__(g_fx.trace[0])* t = &g_fx.trace[k % 1200];
        fprintf(f, "%llu %u %.2f %.2f %u %u %u %.0f %.3f %.3f %.3f %.1f %.1f %.1f %u %.3f %.3f %.3f %.3f %.3f\n", (unsigned long long)t->serial, t->own,
            t->strength, t->day, t->live, t->cached, t->skipped, t->across, t->sun[0], t->sun[1], t->sun[2], t->cam[0], t->cam[1],
            t->cam[2], t->depth, t->dl, t->al, t->fog[0], t->fog[1], t->fog[2]);
    }
    fclose(f);
}

/* --- water -----------------------------------------------------------------------------------------------------
 * The game's water draws come after the scene is done, so they have its camera and light (water_scene)
 * and can be drawn over a copy of what is behind them (water_capture, once a frame, at the first water
 * draw), as on Metal. */
static void water_scene(const GfxScene* s, const float* vinv, const FxU* u, const GfxTex* ct)
{
    struct WaterU* w = &g_fx.wu;
    memcpy(w->iv, vinv, 64), memcpy(w->view, s->view, 64);
    memcpy(w->zp, u->zp, 16);
    w->hand[0] = u->hand[0], w->hand[1] = (float)fmod((double)gfx_now_ns() * 1e-9, 3600.0);
    w->hand[2] = g_fxs.water_refract, w->hand[3] = g_fxs.water_foam;
    w->size[0] = (float)ct->w, w->size[1] = (float)ct->h, w->size[2] = 1.0f / (float)ct->w, w->size[3] = 1.0f / (float)ct->h;
    memcpy(w->sun, g_fx.sunw, 12);
    w->sun[3] = u->sun[3];
    memcpy(w->suncol, u->suncol, 12);
    w->suncol[3] = g_fxs.water_spec;
    if (g_fxs.fog > 0.0f && g_fx.fogc_set)
        memcpy(w->sky, g_fx.fogc, 12);
    else
        memcpy(w->sky, s->fogcolor, 12);
    w->sky[3] = g_fxs.water_reflect;
    w->p[0] = g_fxs.water_clarity, w->p[1] = fmaxf(g_fxs.water_soft, 1e-3f), w->p[2] = g_fxs.water_ripple, w->p[3] = g_fxs.water_scale;
    /* world up: the view's up back into the world */
    for (int j = 0; j < 3; ++j)
        w->p2[j] = g_fx.up[0] * vinv[j] + g_fx.up[1] * vinv[4 + j] + g_fx.up[2] * vinv[8 + j];
    gfx_normalize3(w->p2);
    w->p2[3] = fmaxf(g_fxs.water_foam_width, 1e-3f);
    g_fx.wu_serial = g_serial;
}

/* how a draw is drawn as water (GfxFsKey.water), 0 as the game drew it */
static int water_mode(const GfxDraw* d)
{
    if (!d->water || g_fxs.fx == 0.0f || g_fxs.water <= 0.0f || g_fx.wu_serial != g_serial || d->vs.rhw || d->vs.prog ||
        d->fs.prog || !d->pipe.blend || g_rt_face || g_rt_level)
        return 0;
    if (d->pipe.src == 5 && d->pipe.dst == 6 && d->pipe.op == 1) /* SRCALPHA, INVSRCALPHA, ADD */
        return 1;
    return d->pipe.src == 2 ? 3 : 2; /* ONE: the color fades as well */
}

/* the target and its depth as they are, before the frame's first water draw into it: 0 if they cannot be */
static int water_capture(void)
{
    GfxTex *ct = g_rt, *depth = depth_attachment();
    if (!depth || depth->w != ct->w || depth->h != ct->h || ct->w != (uint32_t)g_fx.wu.size[0] || ct->h != (uint32_t)g_fx.wu.size[1])
        return 0;
    if (g_fx.w_serial == g_serial && g_fx.w_from == ct)
        return 1;
    if (!fx_tex(&g_fx.wcol, ct->vf, ct->w, ct->h, GFX_USE_RT, 1) || !fx_tex(&g_fx.wdep, depth->vf, ct->w, ct->h, GFX_USE_DEPTH, 1))
        return 0;
    flush_pass();
    gpu_sync();
    image_copy(ct, g_fx.wcol, VK_IMAGE_ASPECT_COLOR_BIT);
    image_copy(depth, g_fx.wdep, VK_IMAGE_ASPECT_DEPTH_BIT);
    g_dirty = 1;
    g_fx.w_from = ct, g_fx.w_serial = g_serial;
    return 1;
}

/* the water's bindings for a draw (gfx_msl.h: 16-18) into w; how many */
static uint32_t water_bind(VkWriteDescriptorSet* w, VkDescriptorBufferInfo* bi, VkDescriptorImageInfo* ii)
{
    *bi = ring_put(&g_fx.wu, sizeof g_fx.wu);
    ii[0] = (VkDescriptorImageInfo){ g_lin_samp, g_fx.wcol->view, VK_IMAGE_LAYOUT_GENERAL };
    ii[1] = (VkDescriptorImageInfo){ g_lin_samp, g_fx.wdep->view, VK_IMAGE_LAYOUT_GENERAL };
    for (int i = 0; i < 3; ++i)
    {
        w[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[i].dstBinding = B_WATER_U + (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (i)
            w[i].pImageInfo = &ii[i - 1];
        else
            w[i].pBufferInfo = bi;
    }
    g_fx.wcol->used = g_fx.wdep->used = g_serial;
    return 3;
}

static void water_forget(const GfxTex* t)
{
    if (g_fx.w_from == t)
        g_fx.w_from = NULL, g_fx.w_serial = 0;
}

/* The finished scene anti-aliased (g_fxs.aa: FXAA), within its viewport, before the interface goes on */
static void scene_aa(GfxTex* color, const GfxScene* s)
{
    if (g_fxs.aa < 0.5f || !fx_init())
        return;
    flush_pass();
    float vp[4] = { (float)s->vp[0], (float)s->vp[1], (float)s->vp[2], (float)s->vp[3] };
    if (vp[2] < 16 || vp[3] < 16 || vp[0] + vp[2] > color->w || vp[1] + vp[3] > color->h)
        vp[0] = vp[1] = 0, vp[2] = (float)color->w, vp[3] = (float)color->h;
    g_fx.aa_pipe = fmt_pipe(color->vf, FP_FX_AA);
    if (!g_fx.aa_pipe || !fx_tex(&g_fx.aa_src, color->vf, color->w, color->h, GFX_USE_RT, 1))
        return;
    gpu_sync();
    image_copy(color, g_fx.aa_src, VK_IMAGE_ASPECT_COLOR_BIT);
    g_dirty = 1;
    FxU u;
    memset(&u, 0, sizeof u);
    u.size[0] = (float)color->w, u.size[1] = (float)color->h;
    fx_uniforms(&u);
    g_pass = GFX_PASS_FXAA;
    fx_pass(color, 0, 1, g_fx.aa_pipe, vp, &g_fx.aa_src->view, 1, NULL, NULL);
    color->scene = 0; /* its mips are behind (scene_mips) */
}

/* --- post effects: bloom and depth of field (post_bloom, post_dof), the sun shafts (post_shafts), the HDR scene ----
 * gfx_metal.m's POST_MSL and post_effects, on Vulkan (their notes there). On the finished scene, before the interface and
 * the nameplates go over it, with or without the effects (FFXI_FX) and the scene renderer: the lamps' glows, the sun
 * shafts, then the depth of field (circles of confusion at half size, a disc gather, blended back by each pixel's own
 * circle), then the bloom (its source the glow - the scene's additive draws, drawn again: glow_new - and the sky over a
 * soft threshold; five levels down, a tent back up, screen-blended onto the scene). Each pass a full-screen triangle
 * of its own on the effects' layout (fs_init): the uniforms, then three textures; timed as Metal's. */
static const char POST_GLSL[] =
    "layout(std140, set = 0, binding = 0) uniform PU {\n"
    "  vec4 vp;    // the scene's viewport in its target, pixels\n"
    "  vec4 src;   // the texture read: width, height, 1 / width, 1 / height\n"
    "  vec4 bloom; // threshold, knee, glow weight, strength\n"
    "  vec4 dof;   // focus (view z), sharp range behind it, ramp, largest radius (half-size pixels)\n"
    "  vec4 dof2;  // near strength, hand, levels, radius scale (up)\n"
    "  vec4 zp;    // projection [10], [14], viewport min z, max z\n"
    "  vec4 aux;   // x: a texel step down the target per step across; y: the sky's emissive weight; w: an HDR scene\n"
    "} u;\n"
    "layout(location = 0) in vec2 vuv;\n"
    "layout(location = 0) out vec4 oc;\n"
    "layout(set = 0, binding = 1) uniform sampler2D t0;\n"
    "layout(set = 0, binding = 2) uniform sampler2D t1;\n"
    "layout(set = 0, binding = 3) uniform sampler2D t2;\n"
    "float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "vec3 finite3(vec3 v) { return vec3(isnan(v.x) || isinf(v.x) ? 0.0 : v.x, isnan(v.y) || isinf(v.y) ? 0.0 : v.y,\n"
    "                                   isnan(v.z) || isinf(v.z) ? 0.0 : v.z); }\n"
    "#if defined(POST_BLOOM_PRE)\n"
    "// the bloom's source at a point of the scene: the glow, and where nothing was drawn with depth (the sky, the sun)\n"
    "// what is over the threshold (a soft knee) - lit ground and walls, however bright, do not glow\n"
    "vec3 bright(vec2 uv) {\n"
    "  vec3 c = texture(t0, uv).rgb;\n"
    "  ivec2 q = ivec2(clamp(uv * u.src.xy, vec2(0.0), u.src.xy - 1.0));\n"
    "  float sky = texelFetch(t2, q, 0).r >= u.zp.w - 1e-6 ? 1.0 : 0.0;\n"
    "  float br = max(c.r, max(c.g, c.b)), k = max(u.bloom.y, 1e-4);\n"
    "  float soft = clamp(br - u.bloom.x + k, 0.0, 2.0 * k);\n"
    "  soft = soft * soft / (4.0 * k);\n"
    "  float w = max(soft, br - u.bloom.x) / max(br, 1e-4);\n"
    "  // the glow past a little (faint, wide additive layers - a sea's shimmer - stay as they are)\n"
    "  vec3 g = texture(t1, uv).rgb;\n"
    "  float gb = max(g.r, max(g.g, g.b)), gs = clamp(gb - 0.1, 0.0, 0.2);\n"
    "  gs = gs * gs / 0.4;\n"
    "  float gw = max(gs, gb - 0.2) / max(gb, 1e-4);\n"
    "  vec3 r = c * (w * sky * u.aux.y) + g * (gw * u.bloom.z); // each by its emissive weight: past 1, the chain is float\n"
    "  return finite3(clamp(r, 0.0, 64.0)); // (no NaN or infinity into the chain: it would spread)\n"
    "}\n"
    "// the first level: four bilinear taps, each weighted by 1 / (1 + its luma) - a Karis average, so a lone bright texel\n"
    "// cannot flicker as the camera moves\n"
    "void main() {\n"
    "  vec2 uv = (u.vp.xy + vuv * u.vp.zw) * u.src.zw, t = u.src.zw * vec2(1.0, u.aux.x);\n"
    "  vec3 a = bright(uv + t * vec2(-1, -1)), b = bright(uv + t * vec2(1, -1));\n"
    "  vec3 c = bright(uv + t * vec2(-1, 1)), d = bright(uv + t * vec2(1, 1));\n"
    "  float wa = 1.0 / (1.0 + luma(a)), wb = 1.0 / (1.0 + luma(b)), wc = 1.0 / (1.0 + luma(c)), wd = 1.0 / (1.0 + luma(d));\n"
    "  oc = vec4((a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd), 1.0);\n"
    "}\n"
    "#elif defined(POST_BLOOM_DOWN)\n"
    "// the 13 taps a texel apart (Jimenez): a centre box and four corner boxes, weighted half and an eighth each\n"
    "void main() {\n"
    "  vec2 uv = vuv, t = u.src.zw * vec2(1.0, u.aux.x);\n"
    "  vec3 a = texture(t0, uv + t * vec2(-2, -2)).rgb, b = texture(t0, uv + t * vec2(0, -2)).rgb;\n"
    "  vec3 c = texture(t0, uv + t * vec2(2, -2)).rgb, d = texture(t0, uv + t * vec2(-1, -1)).rgb;\n"
    "  vec3 e = texture(t0, uv + t * vec2(1, -1)).rgb, f = texture(t0, uv + t * vec2(-2, 0)).rgb;\n"
    "  vec3 g = texture(t0, uv).rgb, h = texture(t0, uv + t * vec2(2, 0)).rgb;\n"
    "  vec3 i = texture(t0, uv + t * vec2(-1, 1)).rgb, j = texture(t0, uv + t * vec2(1, 1)).rgb;\n"
    "  vec3 k = texture(t0, uv + t * vec2(-2, 2)).rgb, l = texture(t0, uv + t * vec2(0, 2)).rgb;\n"
    "  vec3 m = texture(t0, uv + t * vec2(2, 2)).rgb;\n"
    "  oc = vec4((d + e + i + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + f + h + l) * 0.0625 + g * 0.125, 1.0);\n"
    "}\n"
    "#elif defined(POST_BLOOM_UP)\n"
    "// a 3x3 tent of the level below, added onto this one (blended): each level the sum of those under it\n"
    "void main() {\n"
    "  vec2 uv = vuv, t = u.src.zw * vec2(1.0, u.aux.x) * u.dof2.w;\n"
    "  vec3 o = texture(t0, uv).rgb * 4.0;\n"
    "  o += (texture(t0, uv + t * vec2(-1, 0)).rgb + texture(t0, uv + t * vec2(1, 0)).rgb +\n"
    "        texture(t0, uv + t * vec2(0, -1)).rgb + texture(t0, uv + t * vec2(0, 1)).rgb) * 2.0;\n"
    "  o += texture(t0, uv + t * vec2(-1, -1)).rgb + texture(t0, uv + t * vec2(1, -1)).rgb +\n"
    "       texture(t0, uv + t * vec2(-1, 1)).rgb + texture(t0, uv + t * vec2(1, 1)).rgb;\n"
    "  oc = vec4(o * (1.0 / 16.0), 1.0);\n"
    "}\n"
    "#elif defined(POST_BLOOM_COMP)\n"
    "// onto the scene, screen-blended by the pipeline (src x (1 - dst) + dst); the sum's highlights eased toward 1\n"
    "void main() {\n"
    "  vec3 b = texture(t0, vuv).rgb * (u.bloom.w * 1.5 / u.dof2.z);\n"
    "  oc = u.aux.w > 0.0 ? vec4(max(b, 0.0), 1.0) : vec4(1.0 - exp(-max(b, 0.0)), 1.0); // an HDR scene: added as it is\n"
    "}\n"
    "#elif defined(POST_DOF_COC) || defined(POST_DOF_GATHER) || defined(POST_DOF_COMP)\n"
    "float view_z(float d) {\n"
    "  d = (d - u.zp.z) / max(u.zp.w - u.zp.z, 1e-6);\n"
    "  float z = u.zp.y / (d * u.dof2.y - u.zp.x);\n"
    "  return d >= 0.999999 || !(z * u.dof2.y > 0.0) ? 1e6 : abs(z); // the sky: as far as can be\n"
    "}\n"
    "// the circle of confusion, 0 sharp to 1 the largest: behind the focus past its range, and (dof_near) well before it;\n"
    "// always sharp within 4 units of the focus (the player, whatever the settings)\n"
    "float coc(float z) {\n"
    "  float far = clamp((z - u.dof.x - max(u.dof.y, 4.0)) / max(u.dof.z, 1e-3), 0.0, 1.0);\n"
    "  float nr = min(u.dof.x * 0.5, u.dof.x - 4.0);\n"
    "  float near = nr > 0.0 ? u.dof2.x * clamp((nr - z) / max(nr, 1e-3), 0.0, 1.0) : 0.0;\n"
    "  return max(far, near);\n"
    "}\n"
    "#if defined(POST_DOF_COC)\n"
    "// half size: each texel the 2x2 under it, their colours weighted toward the blurred ones, and the largest circle\n"
    "void main() {\n"
    "  vec2 p = floor(u.vp.xy + gl_FragCoord.xy * 2.0 - 1.0);\n"
    "  vec3 c = vec3(0.0); float w = 0.0, m = 0.0;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    ivec2 q = ivec2(clamp(p + vec2(k & 1, k >> 1), u.vp.xy, u.vp.xy + u.vp.zw - 1.0));\n"
    "    float r = coc(view_z(texelFetch(t1, q, 0).r));\n"
    "    float wk = r + 0.02;\n"
    "    c += texelFetch(t0, q, 0).rgb * wk, w += wk, m = max(m, r);\n"
    "  }\n"
    "  oc = vec4(c / w, m);\n"
    "}\n"
    "#elif defined(POST_DOF_GATHER)\n"
    "// the gather: a disc of taps as wide as this texel's circle; a tap counts where its own circle (no wider than this\n"
    "// one's) reaches here; bright taps count a little more, for bokeh\n"
    "void main() {\n"
    "  vec4 c0 = texture(t0, vuv);\n"
    "  float R = c0.a * u.dof.w;\n"
    "  if (R < 0.5) { oc = c0; return; }\n"
    "  int n = R < 2.5 ? 8 : 16; // (the composite's tent fills between the taps)\n"
    "  vec3 acc = vec3(0.0); float wsum = 0.0;\n"
    "  for (int i = 0; i < n; ++i) {\n"
    "    float r = sqrt((float(i) + 0.5) / float(n)) * R, a = float(i) * 2.39996323;\n"
    "    vec2 o = vec2(cos(a), sin(a)) * r;\n"
    "    vec4 t = texture(t0, vuv + o * vec2(1.0, u.aux.x) * u.src.zw);\n"
    "    float w = clamp(min(t.a, c0.a) * u.dof.w - r + 1.0, 0.0, 1.0);\n"
    "    w *= 1.0 + 3.0 * clamp(luma(t.rgb) - 0.75, 0.0, 1.0);\n"
    "    acc += t.rgb * w, wsum += w;\n"
    "  }\n"
    "  oc = vec4(wsum > 0.0 ? acc / wsum : c0.rgb, c0.a);\n"
    "}\n"
    "#else\n"
    "// onto the scene by its own circle at full size (alpha-blended by the pipeline): sharp where it is in focus\n"
    "void main() {\n"
    "  vec2 px = gl_FragCoord.xy;\n"
    "  float r = coc(view_z(texelFetch(t1, ivec2(px), 0).r)) * u.dof.w;\n"
    "  float a = smoothstep(0.25, 1.5, r);\n"
    "  if (a <= 0.0) { oc = vec4(0.0); return; }\n"
    "  vec2 uv = (px - u.vp.xy) / u.vp.zw, t = u.src.zw * vec2(0.75, 0.75 * u.aux.x);\n"
    "  vec3 b = (texture(t0, uv + vec2(-t.x, -t.y)).rgb + texture(t0, uv + vec2(t.x, -t.y)).rgb +\n"
    "            texture(t0, uv + vec2(-t.x, t.y)).rgb + texture(t0, uv + vec2(t.x, t.y)).rgb) * 0.25;\n"
    "  oc = vec4(b, a);\n"
    "}\n"
    "#endif\n"
    "#elif defined(POST_HDR_COPY)\n"
    "// the HDR scene (scene_hdr): a target's first level copied into its float replacement, texel for texel\n"
    "void main() { oc = texelFetch(t0, ivec2(gl_FragCoord.xy), 0); }\n"
    "#elif defined(POST_HDR_TONE)\n"
    "// the tonemap (the pixel as it was, from a copy): 0 neutral (clamped), 1 a highlight shoulder, 2 ACES, normalised to\n"
    "// the white point; u.bloom: mode, exposure, white point, knee\n"
    "vec3 aces(vec3 x) { return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14); }\n"
    "void main() {\n"
    "  vec4 c = texelFetch(t0, ivec2(gl_FragCoord.xy), 0);\n"
    "  vec3 v = max(c.rgb, 0.0);\n"
    "  if (u.bloom.x < 0.5) { oc = vec4(min(v, 1.0), c.a); return; }\n"
    "  if (u.bloom.x < 1.5) {\n"
    "    v *= u.bloom.y;\n"
    "    float m = max(v.r, max(v.g, v.b)), kn = u.bloom.w;\n"
    "    if (m > kn) {\n"
    "      float r = kn + (1.0 - kn) * (1.0 - exp(-(m - kn) / (1.0 - kn)));\n"
    "      vec3 h = v * (r / m);\n"
    "      v = mix(h, vec3(r), clamp((m - 1.0) / (u.bloom.z - 1.0), 0.0, 1.0) * 0.5); // the brightest toward white\n"
    "    }\n"
    "    oc = vec4(clamp(v, 0.0, 1.0), c.a);\n"
    "    return;\n"
    "  }\n"
    "  vec3 l = pow(v, vec3(2.2)) * u.bloom.y;\n"
    "  vec3 t = aces(l) / aces(vec3(u.bloom.z));\n"
    "  oc = vec4(pow(clamp(t, 0.0, 1.0), vec3(1.0 / 2.2)), c.a);\n"
    "}\n"
    "#endif\n";

/* the air the modern fog (fog_modern) and its volume share (gfx_metal.m's fog_tau/su_tau, fog_lamp/su_lamp): the lamps'
 * list and tiles (lamps_bin's) at 10 and 11 of the post's layout, the air's optical depth along a ray in closed form, and
 * the lamps' light it scatters toward the eye */
#define AIR_GLSL                                                                                                      \
    "struct LL { vec4 pos, col, att, amb; };\n"                                                                       \
    "layout(std430, set = 0, binding = 10) readonly buffer LLB { LL LS[]; };\n"                                       \
    "layout(std430, set = 0, binding = 11) readonly buffer TLB { uint TL[]; };\n"                                     \
    "uint tl_at(uint i) { uint w = TL[i >> 1]; return (i & 1u) != 0u ? w >> 16 : w & 0xffffu; }\n"                   \
    /* along [0, d] of dir from the camera: density rho at the camera's height, falling as exp(-k * rise) (y down) */  \
    "float air_tau(float rho, float k, vec3 dir, float d) {\n"                                                        \
    "  float rise = -dir.y * d * k;\n"                                                                                \
    "  float f = abs(rise) < 1e-3 ? 1.0 - 0.5 * rise : (1.0 - exp(-rise)) / rise;\n"                                  \
    "  return rho * d * f;\n"                                                                                         \
    "}\n"                                                                                                             \
    /* a point light's light scattered toward the eye along the ray inside its reach: the integral of 1 / r^2 */      \
    "float air_lamp(vec3 Lp, float range, vec3 dir, float d) {\n"                                                     \
    "  float tc = dot(Lp, dir), h2 = max(dot(Lp, Lp) - tc * tc, 0.0), r2 = range * range;\n"                          \
    "  if (h2 >= r2) return 0.0;\n"                                                                                   \
    "  float w = sqrt(r2 - h2), a = max(tc - w, 0.0), b = min(tc + w, d);\n"                                          \
    "  if (b <= a) return 0.0;\n"                                                                                     \
    "  float h = sqrt(max(h2, 0.09));\n"                                                                              \
    "  return (atan((b - tc) / h) - atan((a - tc) / h)) / h;\n"                                                       \
    "}\n"                                                                                                             \
    /* the lamps of fp's tile (lt: the tiles' origin, across, down) along dir to d from cam: halos of strength s round \
     * a point lift above each light, dimmed by the air (rho, k) between */                                            \
    "vec3 air_lamps(vec4 lt, vec2 fp, vec3 cam, vec3 dir, float d, float s, float lift, float rho, float k) {\n"      \
    "  ivec2 tq = ivec2((fp - lt.xy) / 64.0);\n"                                                                      \
    "  if (any(lessThan(tq, ivec2(0))) || tq.x >= int(lt.z) || tq.y >= int(lt.w)) return vec3(0.0);\n"               \
    "  uint b = (uint(tq.y) * uint(lt.z) + uint(tq.x)) * 32u, n = tl_at(b);\n"                                        \
    "  vec3 I = vec3(0.0);\n"                                                                                         \
    "  for (uint i = 0u; i < n; ++i) {\n"                                                                             \
    "    LL l = LS[tl_at(b + 1u + i)];\n"                                                                             \
    "    vec3 Lp = l.pos.xyz - cam - vec3(0.0, lift, 0.0);\n"                                                         \
    "    I += l.col.rgb * (s * air_lamp(Lp, l.pos.w, dir, d) * exp(-air_tau(rho, k, dir, max(dot(Lp, dir), 0.0))));\n" \
    "  }\n"                                                                                                           \
    "  return I;\n"                                                                                                   \
    "}\n"                                                                                                             \
    "float air_dither(vec2 fp) { return fract(sin(dot(fp, vec2(12.9898, 78.233))) * 43758.5453) - 0.5; }\n"

/* the modern fog (fog_modern; gfx_metal.m's fog_comp and its notes): light scattered by the air between the eye and
 * each point, and the point's light dimmed by it - not a fade to grey. Over the scene by its own alpha (the colour times
 * what gets through, plus what is scattered in); t0 the scene's depth */
static const char FOG_GLSL[] =
    "layout(std140, set = 0, binding = 0) uniform FU {\n"
    "  mat4 ivp;  // the scene's clip space to the world round the camera (the view's turn alone)\n"
    "  vec4 cam;  // the camera, world; w: how far the fog goes (beyond: the sky)\n"
    "  vec4 sun;  // toward the sun (or moon), world; w: the forward lobe's g\n"
    "  vec4 sunc; // its scattered colour (0 with the shafts on: theirs)\n"
    "  vec4 airc; // the air's own colour (the game's fog colour, cooled)\n"
    "  vec4 p;    // density at the camera's height, its falloff with height, the lamps' halo strength, the 8-bit dither\n"
    "  vec4 vp;   // the scene's viewport in its target\n"
    "  vec4 lt;   // the lamps' tiles: origin x, y, tiles across, down (0: no lamps)\n"
    "  vec4 dbg;  // x: fog_debug; y: how far above the game's light its lantern hangs (the halo's centre)\n"
    "} u;\n"
    "layout(location = 0) in vec2 vuv;\n"
    "layout(location = 0) out vec4 oc;\n"
    "layout(set = 0, binding = 1) uniform sampler2D t0;\n"
    AIR_GLSL
    "void main() {\n"
    "  vec2 fp = gl_FragCoord.xy;\n"
    "  float d0 = texelFetch(t0, ivec2(fp), 0).r;\n"
    "  if (d0 >= 0.99999) { oc = vec4(0.0); return; } // the sky: the game's own, with its own haze\n"
    "  vec2 n = vec2((fp.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (fp.y - u.vp.y) / u.vp.w * 2.0);\n"
    "  vec4 w = u.ivp * vec4(n, d0, 1.0);\n"
    "  vec3 v = w.xyz / w.w;\n"
    "  float l = length(v), d = min(l, u.cam.w);\n"
    "  vec3 dir = v / max(l, 1e-4);\n"
    "  float T = exp(-air_tau(u.p.x, u.p.y, dir, d));\n"
    "  float g = u.sun.w, mu = dot(dir, u.sun.xyz);\n"
    "  float mie = 0.25 * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5);\n"
    "  float ray = 0.75 * (1.0 + mu * mu); // (Rayleigh: a little more toward and away from the sun than across)\n"
    "  vec3 I = (u.airc.rgb * ray + u.sunc.rgb * mie) * (1.0 - T);\n"
    "  if (u.lt.z > 0.0 && u.p.z > 0.0) I += air_lamps(u.lt, fp, u.cam.xyz, dir, d, u.p.z, u.dbg.y, u.p.x, u.p.y);\n"
    "  if (u.dbg.x > 0.5) { oc = vec4(u.dbg.x > 1.5 ? I : vec3(1.0 - T), 1.0); return; } // fog_debug: 1 how much it takes, 2 what it adds\n"
    "  float a = 1.0 - T;\n"
    "  if (a < 1e-4 && all(lessThan(I, vec3(1e-4)))) { oc = vec4(0.0); return; }\n"
    "  oc = vec4(max(I / max(a, 1e-4) + air_dither(fp) * u.p.w / max(a, 1e-4), 0.0), a); // over: I / a * a + scene * T; dithered at 8 bits\n"
    "}\n";

/* the sun shafts' passes (gfx_metal.m's shaft_march, shaft_resolve, shaft_comp), on the same layout with SU for PU */
static const char SHAFT_GLSL[] =
    "layout(std140, set = 0, binding = 0) uniform SU {\n"
    "  mat4 ivp;    // the scene's clip space to the world round the camera (the view's turn alone: no translation)\n"
    "  mat4 pvp;    // the world round last frame's camera to last frame's clip space\n"
    "  mat4 m[4];   // the zone's shadow cascades (world to the light's clip space)\n"
    "  vec4 cas[4]; // their reach from the camera, texel, depth range, valid\n"
    "  vec4 cam;    // the camera, world; w: how far the rays go\n"
    "  vec4 sun;    // toward the light, world; w: 1 when it shines\n"
    "  vec4 sunc;   // its colour times the shafts' strength; w: the phase's g\n"
    "  vec4 p;      // density at the camera's height, its falloff with height, steps, this frame's jitter\n"
    "  vec4 vp;     // the scene's viewport in its target\n"
    "  vec4 lo;     // the small textures' size, 1 / size\n"
    "  vec4 hist;   // x: last frame's history is there and lines up; y: its weight; z: shafts_debug; w: an HDR scene\n"
    "  vec4 pcam;   // last frame's camera, world\n"
    "  // the fog volume (fog_modern with fog_volume): the air's own colour; x the lamps' halo density, y how far above the\n"
    "  // game's light its lantern hangs, z the ground fog's strength, w its noise's scale; the noise's drift (xyz) and w 1\n"
    "  // in the volume; the lamps' tiles (origin x, y, across, down; 0 none)\n"
    "  vec4 airc;\n"
    "  vec4 fv;\n"
    "  vec4 wind;\n"
    "  vec4 lt;\n"
    "} u;\n"
    "layout(location = 0) in vec2 vuv;\n"
    "layout(location = 0) out vec4 oc;\n"
    "layout(set = 0, binding = 1) uniform sampler2D t0;\n"
    "#ifdef SHAFT_MARCH\n"
    "layout(location = 1) out float ot; // what gets through (the fog volume's)\n"
    "layout(set = 0, binding = 2) uniform sampler2DArrayShadow t1;\n"
    AIR_GLSL
    "#else\n"
    "layout(set = 0, binding = 2) uniform sampler2D t1;\n"
    "#endif\n"
    "layout(set = 0, binding = 3) uniform sampler2D t2;\n"
    "vec3 su_dir(vec2 fp, float d, out float dist) {\n"
    "  vec2 n = vec2((fp.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (fp.y - u.vp.y) / u.vp.w * 2.0);\n"
    "  bool sky = d >= 0.99999;\n"
    "  vec4 w = u.ivp * vec4(n, sky ? 0.5 : d, 1.0);\n"
    "  vec3 v = w.xyz / w.w;\n"
    "  float l = length(v);\n"
    "  dist = sky ? u.cam.w : min(l, u.cam.w);\n"
    "  return v / max(l, 1e-4);\n"
    "}\n"
    "#if defined(SHAFT_MARCH)\n"
    "// low, smooth noise for the ground fog: value noise, two octaves, 0..1\n"
    "float su_hash(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }\n"
    "float su_vnoise(vec3 x) {\n"
    "  vec3 i = floor(x), f = x - i; f = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(mix(su_hash(i), su_hash(i + vec3(1, 0, 0)), f.x), mix(su_hash(i + vec3(0, 1, 0)), su_hash(i + vec3(1, 1, 0)), f.x), f.y),\n"
    "             mix(mix(su_hash(i + vec3(0, 0, 1)), su_hash(i + vec3(1, 0, 1)), f.x), mix(su_hash(i + vec3(0, 1, 1)), su_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);\n"
    "}\n"
    "float su_ground(vec3 x) {\n"
    "  vec3 q = (x + u.wind.xyz) * u.fv.w;\n"
    "  return 0.65 * su_vnoise(q) + 0.35 * su_vnoise(q * 2.7 + 13.1);\n"
    "}\n"
    "float su_depth(vec2 fp) { return texelFetch(t0, ivec2(clamp(fp, u.vp.xy, u.vp.xy + u.vp.zw - 1.0)), 0).r; }\n"
    "// the ray to the surface (or as far as the rays go): steps spread as the square of their order, the first by this\n"
    "// pixel's jitter; the light in-scattered, each step lit where the cascades see the sun. In the fog volume the air's\n"
    "// own light too, the ground mist over the surface the ray ends on and the lamps' halos; ot what gets through\n"
    "void main() {\n"
    "  vec2 lp = floor(gl_FragCoord.xy);\n"
    "  vec2 fp = u.vp.xy + (lp + 0.5) * (u.vp.zw * u.lo.zw);\n"
    "  float dist, d0 = su_depth(fp);\n"
    "  vec3 dir = su_dir(fp, d0, dist);\n"
    "  bool vol = u.wind.w > 0.5, sky = d0 >= 0.99999;\n"
    "  // this texel's jitter: interleaved gradient noise (near blue), turned a golden ratio each frame\n"
    "  float j = fract(fract(52.9829189 * fract(dot(lp, vec2(0.06711056, 0.00583715)))) + u.p.w);\n"
    "  int n = int(u.p.z);\n"
    "  float g = u.sunc.w, mu = dot(dir, u.sun.xyz);\n"
    "  float ph = 0.25 * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5);\n"
    "  float ray = 0.75 * (1.0 + mu * mu);\n"
    "  float T = 1.0, L = 0.0, ls = 0.0, lin = 0.0, llast = 1.0, A = 0.0;\n"
    "  for (int i = 0; i < n; ++i) {\n"
    "    float f = (float(i) + j) / float(n), t = dist * f * f, seg = 2.0 * dist * f / float(n);\n"
    "    vec3 x = u.cam.xyz + dir * t;\n"
    "    float rho = u.p.x * exp(clamp(u.p.y * (x.y - u.cam.y), -10.0, 3.0)); // y down: above the camera thinner\n"
    "    float lit = 1.0, inr = 0.0;\n"
    "    for (int c = 0; c < 4; ++c) {\n"
    "      if (u.cas[c].w == 0.0 || t > u.cas[c].x) continue;\n"
    "      vec4 h = u.m[c] * vec4(x, 1.0);\n"
    "      if (all(lessThan(abs(h.xy), vec2(0.995))) && h.z > 0.0 && h.z < 1.0)\n"
    "        lit = texture(t1, vec4(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5, float(c), h.z - 2e-4)), inr = 1.0;\n"
    "      break;\n"
    "    }\n"
    "    ls += lit, lin += inr, llast = lit;\n"
    "    float a = rho * seg;\n"
    "    L += T * a * lit;\n"
    "    if (vol && !sky) A += T * a; // the air's own light: the sky's, unshadowed\n"
    "    T *= exp(-a);\n"
    "  }\n"
    "  ot = sky ? 1.0 : T;\n"
    "  if (u.hist.z < 0.0) { oc = vec4(ls / float(n), lin / float(n), 0.0, dist); return; } // shafts_debug < 0\n"
    "  vec3 o = u.sunc.rgb * (L * ph);\n"
    "  if (vol && !sky) {\n"
    "    o += u.airc.rgb * (A * ray);\n"
    "    if (u.fv.z > 0.0) {\n"
    "      // the ground mist: a layer some 1.5 units deep over the surface this ray ends on (the ground taken as level\n"
    "      // there), in drifting patches; its depth along the ray in closed form, lit as the surface is - only over\n"
    "      // ground (the surface's facing from the depths beside it; up, y down: -y), not characters or walls\n"
    "      float sl = max(abs(dir.y), 0.2), H = 1.5;\n"
    "      vec3 P = u.cam.xyz + dir * dist;\n"
    "      vec2 fx = fp + vec2(u.vp.z * u.lo.z, 0.0), fy = fp + vec2(0.0, u.vp.w * u.lo.w);\n"
    "      float d1, d2;\n"
    "      vec3 r1 = su_dir(fx, su_depth(fx), d1), r2 = su_dir(fy, su_depth(fy), d2);\n"
    "      vec3 nn = cross(r1 * d1 - dir * dist, r2 * d2 - dir * dist);\n"
    "      float up = length(nn) > 1e-6 ? abs(normalize(nn).y) : 0.0;\n"
    "      float edge = clamp(1.0 - max(abs(d1 - dist), abs(d2 - dist)) / (0.05 * dist + 0.3), 0.0, 1.0); // (a silhouette's edge: no facing to trust)\n"
    "      float tm = u.fv.z * 0.12 * smoothstep(0.75, 0.95, up) * edge * smoothstep(0.3, 0.65, su_ground(P)) * (H / sl) * (1.0 - exp(-dist * sl / H));\n"
    "      float am = 1.0 - exp(-tm);\n"
    "      o += (u.airc.rgb * ray + u.sunc.rgb * (ph * llast)) * (T * am);\n"
    "      T *= 1.0 - am;\n"
    "      ot = T;\n"
    "    }\n"
    "    if (u.lt.z > 0.0 && u.fv.x > 0.0) o += air_lamps(u.lt, fp, u.cam.xyz, dir, dist, u.fv.x, u.fv.y, u.p.x, u.p.y);\n"
    "  }\n"
    "  oc = vec4(any(isnan(o)) || any(isinf(o)) ? vec3(0.0) : o, dist);\n"
    "}\n"
    "#elif defined(SHAFT_RESOLVE)\n"
    "// over frames: this frame's march and last frame's result where this texel's point was then, clamped to this frame's\n"
    "// neighbours (no trails where the scene moved); t0 the scene's depth, t1 this frame's march, t2 last frame's\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy), hi = ivec2(u.lo.xy) - 1;\n"
    "  vec4 c = texelFetch(t1, p, 0);\n"
    "  if (u.hist.x <= 0.0) { oc = c; return; }\n"
    "  vec3 mn = c.rgb, mx = c.rgb;\n"
    "  for (int k = 0; k < 9; ++k) {\n"
    "    if (k == 4) continue;\n"
    "    vec3 v = texelFetch(t1, clamp(p + ivec2(k % 3 - 1, k / 3 - 1), ivec2(0), hi), 0).rgb;\n"
    "    mn = min(mn, v), mx = max(mx, v);\n"
    "  }\n"
    "  vec2 fp = u.vp.xy + (vec2(p) + 0.5) * (u.vp.zw * u.lo.zw);\n"
    "  float dist;\n"
    "  vec3 dir = su_dir(fp, texelFetch(t0, ivec2(clamp(fp, u.vp.xy, u.vp.xy + u.vp.zw - 1.0)), 0).r, dist);\n"
    "  vec4 pc = u.pvp * vec4(u.cam.xyz + dir * c.a - u.pcam.xyz, 1.0);\n"
    "  if (pc.w <= 1e-4) { oc = c; return; }\n"
    "  vec2 uv = vec2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
    "  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { oc = c; return; }\n"
    "  vec3 h = clamp(texture(t2, uv).rgb, mn, mx);\n"
    "  oc = vec4(mix(c.rgb, h, u.hist.y), c.a);\n"
    "}\n"
    "#else\n"
    "// onto the scene (screen-blended): the four small texels round each pixel, weighted by how near they are and how\n"
    "// alike in distance; t0 the scene's depth, t1 the resolved shafts, t2 what got through (the fog volume's)\n"
    "void main() {\n"
    "  vec2 fp = gl_FragCoord.xy;\n"
    "  float dist;\n"
    "  su_dir(fp, texelFetch(t0, ivec2(fp), 0).r, dist);\n"
    "  vec2 h = (fp - u.vp.xy) * (u.lo.xy / u.vp.zw) - 0.5;\n"
    "  ivec2 b = ivec2(floor(h)), hi = ivec2(u.lo.xy) - 1;\n"
    "  vec2 fr = h - vec2(b);\n"
    "  vec3 sum = vec3(0.0); float ws = 0.0, tsum = 0.0;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    ivec2 o = ivec2(k & 1, k >> 1), q = clamp(b + o, ivec2(0), hi);\n"
    "    vec4 t = texelFetch(t1, q, 0);\n"
    "    float w = (o.x != 0 ? fr.x : 1.0 - fr.x) * (o.y != 0 ? fr.y : 1.0 - fr.y) + 1e-3;\n"
    "    w *= exp(-abs(t.a - dist) / (0.05 * dist + 0.5));\n"
    "    sum += t.rgb * w, ws += w;\n"
    "    if (u.wind.w > 0.5) tsum += texelFetch(t2, q, 0).r * w;\n"
    "  }\n"
    "  vec3 c = ws > 1e-6 ? sum / ws : vec3(0.0);\n"
    "  if (u.hist.z != 0.0) { oc = vec4(clamp(c * abs(u.hist.z), 0.0, 1.0), 1.0); return; } // shafts_debug: the term alone\n"
    "  if (u.wind.w > 0.5) { // the fog volume: the scene dimmed by the air, what it scatters added (premultiplied over)\n"
    "    float a = texelFetch(t0, ivec2(fp), 0).r >= 0.99999 ? 0.0 : clamp(1.0 - (ws > 1e-6 ? tsum / ws : 1.0), 0.0, 1.0);\n"
    "    float r = u.hist.w > 0.0 ? 0.0 : (fract(sin(dot(fp, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255.0;\n"
    "    oc = vec4(max(c + r, 0.0), a);\n"
    "    return;\n"
    "  }\n"
    "  oc = u.hist.w > 0.0 ? vec4(max(c, 0.0), 1.0) : vec4(1.0 - exp(-max(c, 0.0)), 1.0); // an HDR scene: added as it is\n"
    "}\n"
    "#endif\n";

typedef struct PostU
{
    float vp[4], src[4], bloom[4], dof[4], dof2[4], zp[4], aux[4];
} PostU;

/* the sun shafts' uniforms: SU in SHAFT_GLSL */
typedef struct ShaftU
{
    float ivp[16], pvp[16], m[4][16], cas[4][4], cam[4], sun[4], sunc[4], p[4], vp[4], lo[4], hist[4], pcam[4];
    float airc[4], fv[4], wind[4], lt[4];
} ShaftU;

/* the modern fog's uniforms: FU in FOG_GLSL */
typedef struct FogU
{
    float ivp[16];
    float cam[4], sun[4], sunc[4], airc[4], p[4], vp[4], lt[4], dbg[4];
} FogU;

enum
{
    POST_LEVELS = 6
};
static struct
{
    int tried, ok;
    VkFormat lvl_fmt;                     /* the bloom's levels: R11G11B10 float where it blends, else RGBA16F */
    VkPipeline pre, down, up, coc, gather; /* into the post's own textures (onto the scene: fmt_pipe's FP_*) */
    VkPipeline smarch, sresolve;          /* the sun shafts' march and their resolve over frames */
    GfxTex* lvl[POST_LEVELS]; /* the bloom's levels, from half the scene's size */
    GfxTex* glow;             /* the glow's draws, as the scene's target */
    GfxTex *h0, *h1;          /* the depth of field at half size: colour and circle; gathered */
    GfxTex *sh, *shist[2];    /* the shafts at a third or a fifth of the size: this frame's march, the resolved two */
    GfxTex* sht;              /* what gets through, as marched (the fog volume's) */
    int shist_at;             /* which of shist[] the last frame wrote */
    uint64_t sh_serial;       /* that frame (0: none) */
    float sh_pvp[16], sh_pcam[3]; /* its view's turn times projection, its camera */
    float focus;              /* the focus distance (view z), eased */
    uint64_t focus_ns;
} g_post;

/* one pass's pipeline: the post's vertex function and POST_<name> (or SHAFT_<name> from SHAFT_GLSL, FOG FOG_GLSL) into
 * fmt; blended as fs_blend says */
static VkPipeline post_pipeline(const char* name, VkFormat fmt, int blend)
{
    int shaft = !strncmp(name, "SHAFT_", 6), fog = !strcmp(name, "FOG");
    char d[64];
    snprintf(d, sizeof d, "#define %s%s\n", shaft || fog ? "" : "POST_", name);
    VkPipeline p = fs_pipeline(g_fx.vs, d, shaft ? SHAFT_GLSL : fog ? FOG_GLSL : POST_GLSL, g_fx_layout, fmt, blend,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    if (!p)
        atomic_fetch_add(&g_failures, 1), fprintf(stderr, "[recomp] gfx: post effect %s failed\n", name);
    return p;
}

static int post_init(VkFormat scene_fmt)
{
    if (!g_post.tried)
    {
        g_post.tried = 1;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_phys, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &fp);
        g_post.lvl_fmt = (fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) ==
                (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            ? VK_FORMAT_B10G11R11_UFLOAT_PACK32
            : VK_FORMAT_R16G16B16A16_SFLOAT;
        const VkFormat F16 = VK_FORMAT_R16G16B16A16_SFLOAT;
        if (fs_init())
        {
            g_post.pre = post_pipeline("BLOOM_PRE", g_post.lvl_fmt, 0);
            g_post.down = post_pipeline("BLOOM_DOWN", g_post.lvl_fmt, 0);
            g_post.up = post_pipeline("BLOOM_UP", g_post.lvl_fmt, 1);
            g_post.coc = post_pipeline("DOF_COC", F16, 0);
            g_post.gather = post_pipeline("DOF_GATHER", F16, 0);
            /* the march: two targets, the in-scatter and distance, and what gets through */
            VkShaderModule fs = compile_with("#define SHAFT_MARCH\n", SHAFT_GLSL, 1);
            if (fs)
            {
                static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
                const VkFormat mf[2] = { F16, VK_FORMAT_R16_SFLOAT };
                VkPipelineColorBlendAttachmentState b[2] = { fs_blend(0), fs_blend(0) };
                g_post.smarch = make_pipeline_n(g_fx.vs, fs, g_fx_layout, b, mf, 2, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, dyn, 2, 0);
                vkDestroyShaderModule(g_dev, fs, NULL);
            }
            if (!g_post.smarch)
                atomic_fetch_add(&g_failures, 1), fprintf(stderr, "[recomp] gfx: post effect SHAFT_MARCH failed\n");
            g_post.sresolve = post_pipeline("SHAFT_RESOLVE", F16, 0);
        }
        g_post.ok = g_post.pre && g_post.down && g_post.up && g_post.coc && g_post.gather && g_post.smarch &&
                    g_post.sresolve;
    }
    if (!g_post.ok)
        return 0;
    for (int k = FP_BLOOM_COMP; k <= FP_SHAFT_DBG; ++k) /* (onto the scene) */
        if (!fmt_pipe(scene_fmt, k))
            return 0;
    return 1;
}

static VkPipeline fmt_build(VkFormat f, int which)
{
    switch (which)
    {
    case FP_FX_COMP: return fx_pipeline("COMP", f);
    case FP_FX_AA: return fx_pipeline("FXAA", f);
    case FP_BLOOM_COMP: return post_pipeline("BLOOM_COMP", f, 2);
    case FP_BLOOM_ADD: return post_pipeline("BLOOM_COMP", f, 1);
    case FP_DOF_COMP: return post_pipeline("DOF_COMP", f, 3);
    case FP_SHAFT_COMP: return post_pipeline("SHAFT_COMP", f, 2);
    case FP_SHAFT_ADD: return post_pipeline("SHAFT_COMP", f, 1);
    case FP_SHAFT_DBG: return post_pipeline("SHAFT_COMP", f, 0);
    case FP_SHAFT_VOL: return post_pipeline("SHAFT_COMP", f, 4);
    case FP_FOG: return post_pipeline("FOG", f, 3);
    case FP_HDR_COPY: return post_pipeline("HDR_COPY", f, 0);
    case FP_GLOW:
    {
        static VkShaderModule vs;
        if (!vs)
            vs = compile_glsl(GLOW_GLSL, 0);
        VkPipeline p = fs_pipeline(vs, "", GLOW_GLSL, g_world.layout, f, 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
        if (!p)
            fprintf(stderr, "[recomp] gfx: lamp glows failed\n");
        return p;
    }
    }
    return VK_NULL_HANDLE;
}

/* one full-screen triangle into the first level of target (and target2, NULL none) within vp (x y w h), its uniforms u
 * (un bytes) and t0..t2 (NULL: none) each through its sampler (VK_NULL_HANDLE: the linear one), with lamps (AIR_GLSL's
 * buffers: this frame's binned lamps, or with none an empty list); load keeps what is there; timed as pass */
static void post_pass_x(GfxTex* target, GfxTex* target2, int load, VkPipeline p, const float* vp, const void* u, size_t un,
    GfxTex* const* in, const VkSampler* samp, int lamps, int pass)
{
    VkViewport v = { vp[0], vp[1], vp[2], vp[3], 0, 1 };
    int ts = pass_open2(target, target2, 0, 0, load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE, &v, pass);
    VkCommandBuffer cb = g_cb;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkDescriptorBufferInfo bi[3] = { ring_put(u, un) };
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[6];
    desc_write(&w[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bi[0], NULL);
    for (int i = 0; i < 3; ++i)
    {
        GfxTex* t = in[i] ? in[i] : g_dummy2d;
        VkSampler s = samp && samp[i] ? samp[i] : g_lin_samp;
        ii[i] = (VkDescriptorImageInfo){ s, t->view, VK_IMAGE_LAYOUT_GENERAL };
        desc_write(&w[1 + i], 1 + (uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, &ii[i]);
        t->used = g_serial, t->rec = g_cb_index;
    }
    uint32_t nw = 4;
    if (lamps)
    {
        int have = g_lamps.binned == g_serial && g_lamps.lb && g_lamps.tb;
        static const uint32_t none[16];
        bi[1] = have ? (VkDescriptorBufferInfo){ g_lamps.lb, g_lamps.lo, g_lamps.lsz } : ring_put(none, sizeof none);
        bi[2] = have ? (VkDescriptorBufferInfo){ g_lamps.tb, g_lamps.to, g_lamps.tsz } : bi[1];
        desc_write(&w[nw++], 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[1], NULL);
        desc_write(&w[nw++], 11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[2], NULL);
    }
    p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fx_layout, 0, nw, w);
    vkCmdDraw(cb, 3, 1, 0, 0);
    pass_close(ts);
}

static void post_pass(GfxTex* target, int load, VkPipeline p, const float* vp, const void* u, size_t un, GfxTex* const* in,
    const VkSampler* samp, int pass)
{
    post_pass_x(target, NULL, load, p, vp, u, un, in, samp, 0, pass);
}

/* The modern fog (fog_modern; FOG_GLSL, gfx_metal.m's scene_fog and its notes): over the finished scene before the
 * shafts, depth of field and bloom. Its density from the game's fog end (some 4% of a point's light left there) times
 * fog_density, thinning with height by fog_thin; indoors (a zone's fixed light) a light dust. With the sun's cascades
 * (fog_volume) it is the shafts' march instead: one volume, shadowed. */

/* the fog's fixed height in the world (y down): the player's feet as they arrived in a zone, kept until they jump far
 * (a zone change, a teleport), so the haze and the ground mist stay put as the player climbs or descends */
static float fog_ref(void)
{
    static float ref, last[3];
    static int set;
    if (g_fx.has_focus)
    {
        float dx = g_fx.focus[0] - last[0], dy = g_fx.focus[1] - last[1], dz = g_fx.focus[2] - last[2];
        if (!set || dx * dx + dy * dy + dz * dz > 60.0f * 60.0f)
            ref = g_fx.focus[1], set = 1;
        memcpy(last, g_fx.focus, sizeof last);
    }
    return set ? ref : 0.0f;
}

/* the haze's density at the camera's height, from its density at the fixed height and its thinning upward */
static float fog_at_cam(float sigma, float cam_y)
{
    float e = fminf(fmaxf(g_fxs.fog_thin * (cam_y - fog_ref()), -3.0f), 3.0f);
    return sigma * expf(e);
}

/* the haze's density at the fixed height */
static float fog_sigma(const GfxScene* s)
{
    float fog_end = s->fog[2] != 0.0f && s->fog[1] > 1.0f ? s->fog[1] : 600.0f;
    fog_end = fminf(fmaxf(fog_end, 20.0f), 3000.0f);
    float k = fmaxf(g_fxs.fog_density, 0.0f) * 3.2f / fog_end; /* exp(-3.2): 4% */
    return k * (g_fixed_light ? 0.15f : 1.0f);
}

/* the air's own colour: the game's fog colour, a little cooler and brighter where the sky lights it */
static void fog_air(const GfxScene* s, float* airc)
{
    const float* fc = s->fogcolor;
    float lum = 0.2126f * fc[0] + 0.7152f * fc[1] + 0.0722f * fc[2];
    static const float cool[3] = { 0.88f, 0.97f, 1.18f };
    for (int c = 0; c < 3; ++c)
        airc[c] = fmaxf(fc[c] * 0.7f + lum * cool[c] * 0.3f, 0.0f);
}

/* the lamps' tiles for the fog's halos (origin x, y, across, down), 0 none */
static int fog_lamp_tiles(float* lt)
{
    if (g_fxs.scene_lamps <= 0.0f || g_lamps.binned != g_serial || !g_lamps.lb || !g_lamps.tb)
        return 0;
    lt[0] = g_lamps.lu[0], lt[1] = g_lamps.lu[1], lt[2] = g_lamps.lu[4], lt[3] = g_lamps.lu[5];
    return 1;
}

/* the sun's cascades this frame, to shade the shafts and the fog volume by */
static int sun_cascades(void)
{
    return g_shd.zmap && g_shd.cframe == g_serial && g_shd.u.dir[3] > 0.0f && g_shd.u.info[0] > 0.0f && g_shd.u.info[2] > 0.0f;
}

/* whether the fog is the shafts' volume this frame (fog_modern, fog_volume and the cascades); else scene_fog's pass */
static int fog_volume_on(void)
{
    return g_fxs.fog_modern > 0.0f && g_fxs.fog_volume > 0.0f && sun_cascades();
}

static void scene_fog(GfxTex* color, const GfxScene* s)
{
    if (g_fxs.fog_modern <= 0.0f || s->proj[11] == 0.0f || fog_volume_on())
        return;
    GfxTex *ct = color, *depth = color->depth_world ? color->depth_world : color->depth_seen;
    if (!depth || depth->w != ct->w || depth->h != ct->h || !post_init(ct->vf))
        return;
    VkPipeline pipe = fmt_pipe(ct->vf, FP_FOG);
    if (!pipe)
        return;
    int hdr = ct->vf == VK_FORMAT_R16G16B16A16_SFLOAT;
    float Vr[16], VP[16], iv[16];
    memcpy(Vr, s->view, 64);
    Vr[12] = Vr[13] = Vr[14] = 0.0f;
    gfx_mat_mul(VP, Vr, s->proj);
    FogU u;
    memset(&u, 0, sizeof u);
    if (!gfx_mat_inverse(u.ivp, VP) || !gfx_mat_inverse(iv, s->view))
        return;
    float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > ct->w || vy + vh > ct->h)
        vx = vy = 0, vw = (float)ct->w, vh = (float)ct->h;
    float fog_end = s->fog[2] != 0.0f && s->fog[1] > 1.0f ? s->fog[1] : 600.0f;
    u.cam[0] = iv[12], u.cam[1] = iv[13], u.cam[2] = iv[14], u.cam[3] = fminf(fmaxf(fog_end, 20.0f), 3000.0f) * 1.5f;
    /* the sun (or moon) toward it, world: the scene's is in camera space */
    float sd[3] = { 0, -1, 0 };
    if (s->sun_dir[3] != 0.0f)
        for (int j = 0; j < 3; ++j)
            sd[j] = s->sun_dir[0] * iv[j] + s->sun_dir[1] * iv[4 + j] + s->sun_dir[2] * iv[8 + j];
    float sl = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
    for (int j = 0; j < 3; ++j)
        u.sun[j] = sl > 1e-4f ? sd[j] / sl : 0.0f;
    u.sun[3] = 0.6f;
    /* the sun's forward scattering in its own colour, none with the shafts on (theirs is shadowed) */
    fog_air(s, u.airc);
    int shafts = g_fxs.post_shafts > 0.0f && g_shd.zmap && g_shd.cframe == g_serial && g_shd.u.dir[3] > 0.0f;
    float up = fmaxf(-u.sun[1], 0.0f), low = 1.0f - up;
    for (int c = 0; c < 3; ++c)
        u.sunc[c] = shafts || s->sun_dir[3] == 0.0f ? 0.0f : fmaxf(s->sun_color[c], 0.0f) * (0.15f + 0.6f * low * low);
    /* (the mist round a lamp: its own, not the haze's) */
    u.p[0] = fog_at_cam(fog_sigma(s), u.cam[1]), u.p[1] = fmaxf(g_fxs.fog_thin, 0.0f), u.p[2] = fmaxf(g_fxs.fog_lamps, 0.0f) * 0.05f;
    u.p[3] = hdr ? 0.0f : 1.0f / 255.0f;
    u.vp[0] = vx, u.vp[1] = vy, u.vp[2] = vw, u.vp[3] = vh;
    fog_lamp_tiles(u.lt);
    u.dbg[0] = g_fxs.fog_debug, u.dbg[1] = g_fxs.fog_lamp_lift;
    flush_pass();
    GfxTex* in[3] = { depth, NULL, NULL };
    VkSampler ns[3] = { g_near_samp, VK_NULL_HANDLE, VK_NULL_HANDLE };
    float fv[4] = { vx, vy, vw, vh };
    post_pass_x(ct, NULL, 1, pipe, fv, &u, sizeof u, in, ns, 1, GFX_PASS_POST_SHAFTS);
}

/* The sun shafts (post_shafts; gfx_metal.m's note): the sun's or moon's light scattered toward the eye along each
 * pixel's ray, through haze as thick as the game's fog says, where the scene renderer's zone shadow cascades see the
 * light; with the modern fog's volume (fog_volume_on) the fog itself, marched the same way. Needs the scene renderer's
 * shadows (scene 1 or 2, scene_shadows): without them, or indoors, the pass stays off. */
static void post_shafts(GfxTex* color, const GfxScene* s, GfxTex* depth, float vx, float vy, float vw, float vh)
{
    if (!sun_cascades())
    {
        g_post.sh_serial = 0;
        return;
    }
    float Vr[16], VP[16];
    memcpy(Vr, s->view, 64);
    Vr[12] = Vr[13] = Vr[14] = 0.0f;
    gfx_mat_mul(VP, Vr, s->proj);
    ShaftU u;
    memset(&u, 0, sizeof u);
    if (!gfx_mat_inverse(u.ivp, VP))
        return;
    int q = g_fxs.shafts_q >= 1.5f ? 2 : 1, div = q == 2 ? 3 : 5;
    uint32_t lw = (uint32_t)(vw / div), lh = (uint32_t)(vh / div);
    lw = lw ? lw : 1, lh = lh ? lh : 1;
    const VkFormat F16 = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!fx_tex(&g_post.sh, F16, lw, lh, GFX_USE_RT, 1) || !fx_tex(&g_post.sht, VK_FORMAT_R16_SFLOAT, lw, lh, GFX_USE_RT, 1) ||
        !fx_tex(&g_post.shist[0], F16, lw, lh, GFX_USE_RT, 1) || !fx_tex(&g_post.shist[1], F16, lw, lh, GFX_USE_RT, 1))
        return;
    memcpy(u.m, g_shd.u.m, sizeof u.m), memcpy(u.cas, g_shd.u.cas, sizeof u.cas);
    /* the haze: as thick as the game's fog, the nearer its end the thicker */
    float fog_end = s->fog[2] != 0.0f && s->fog[1] > 1.0f ? s->fog[1] : 1000.0f;
    fog_end = fminf(fmaxf(fog_end, 30.0f), 3000.0f);
    memcpy(u.cam, g_shd.u.cam, 12);
    u.cam[3] = fminf(fmaxf(g_fxs.shafts_dist, 10.0f), fog_end * 1.5f);
    memcpy(u.sun, g_shd.u.dir, 12), u.sun[3] = 1.0f;
    /* the light: this frame's sun (or moon) colour, as strong as the shadows are drawn; strongest with the light low */
    float sc[3] = { 1.0f, 0.95f, 0.85f };
    if (s->sun_dir[3] != 0.0f)
        sc[0] = s->sun_color[0], sc[1] = s->sun_color[1], sc[2] = s->sun_color[2];
    float up = fmaxf(-u.sun[1], 0.0f), low = 1.0f - up;
    float k = g_fxs.post_shafts * g_shd.u.info[2] * 1.5f * (0.25f + 0.75f * low * low);
    for (int c = 0; c < 3; ++c)
        u.sunc[c] = fmaxf(sc[c], 0.0f) * k;
    u.sunc[3] = fminf(fmaxf(g_fxs.shafts_g, 0.0f), 0.95f);
    u.p[0] = fmaxf(g_fxs.shafts_density, 0.0f) * 2.0f / fog_end, u.p[1] = 0.02f, u.p[2] = q == 2 ? 16.0f : 10.0f;
    if (g_fxs.fog_modern > 0.0f) /* one air: the modern fog's density and falloff */
        u.p[0] = fog_at_cam(fog_sigma(s), u.cam[1]) * fmaxf(g_fxs.shafts_density, 0.0f), u.p[1] = fmaxf(g_fxs.fog_thin, 0.0f);
    int vol = fog_volume_on();
    if (vol)
    {
        /* the fog volume: the air's own light, the ground fog's drift, the lamps; the sun's in-scatter the fog's when the
         * shafts are off; to the fog's far limit, not the shafts' reach */
        fog_air(s, u.airc);
        if (g_fxs.post_shafts <= 0.0f)
            for (int c = 0; c < 3; ++c)
                u.sunc[c] = fmaxf(sc[c], 0.0f) * g_shd.u.info[2] * (0.15f + 0.6f * low * low);
        u.p[0] = fog_at_cam(fog_sigma(s), u.cam[1]);
        u.cam[3] = fog_end * 1.5f;
        u.fv[0] = fmaxf(g_fxs.fog_lamps, 0.0f) * 0.05f, u.fv[1] = g_fxs.fog_lamp_lift;
        /* the weather: fog lies thick on the ground, rain and snow raise some, wind and gales thin it and blow it on */
        float wm = g_weather == 3 ? 2.0f : g_weather == 6 || g_weather == 7 || g_weather == 12 ? 1.8f : g_weather == 10 ? 0.6f
                 : g_weather == 11 || g_weather == 13 ? 0.35f : 1.0f;
        float ws = g_weather == 10 ? 3.0f : g_weather == 11 || g_weather == 13 || g_weather == 7 ? 6.0f : 1.0f;
        u.fv[2] = fmaxf(g_fxs.fog_ground, 0.0f) * wm, u.fv[3] = 1.0f / fmaxf(g_fxs.fog_ground_size, 1.0f);
        static double drift, last_t; /* the noise's offset, carried frame to frame (a speed change doesn't jump it) */
        double t = (double)gfx_now_ns() * 1e-9, dt = last_t > 0.0 ? fmin(t - last_t, 0.25) : 0.0;
        last_t = t, drift = fmod(drift + dt * fmaxf(g_fxs.fog_drift, 0.0f) * ws, 4096.0);
        u.wind[0] = (float)drift, u.wind[1] = 0.0f, u.wind[2] = (float)(drift * 0.6), u.wind[3] = 1.0f;
        fog_lamp_tiles(u.lt);
    }
    u.p[3] = (float)fmod((double)g_serial * 0.6180339887, 1.0);
    u.vp[0] = vx, u.vp[1] = vy, u.vp[2] = vw, u.vp[3] = vh;
    u.lo[0] = (float)lw, u.lo[1] = (float)lh, u.lo[2] = 1.0f / (float)lw, u.lo[3] = 1.0f / (float)lh;
    /* last frame's, when it was the frame before at this size and the camera didn't jump */
    float jx = u.cam[0] - g_post.sh_pcam[0], jy = u.cam[1] - g_post.sh_pcam[1], jz = u.cam[2] - g_post.sh_pcam[2];
    int have = g_post.sh_serial && g_post.sh_serial + 1 == g_serial && jx * jx + jy * jy + jz * jz < 25.0f && !cam_cut();
    memcpy(u.pvp, g_post.sh_pvp, 64), memcpy(u.pcam, g_post.sh_pcam, 12);
    u.hist[0] = have && g_fxs.shafts_debug >= 0.0f ? 1.0f : 0.0f, u.hist[1] = fminf(fmaxf(g_fxs.shafts_temporal, 0.0f), 0.97f),
    u.hist[2] = g_fxs.shafts_debug;
    u.hist[3] = color->vf == VK_FORMAT_R16G16B16A16_SFLOAT ? 1.0f : 0.0f;
    GfxTex *prev = g_post.shist[g_post.shist_at], *out = g_post.shist[g_post.shist_at ^ 1];
    float lv[4] = { 0, 0, (float)lw, (float)lh };
    GfxTex* march_in[3] = { depth, g_shd.zmap, NULL };
    VkSampler march_s[3] = { g_near_samp, g_cmp_samp, VK_NULL_HANDLE };
    post_pass_x(g_post.sh, g_post.sht, 0, g_post.smarch, lv, &u, sizeof u, march_in, march_s, 1, GFX_PASS_POST_SHAFTS);
    GfxTex* res_in[3] = { depth, g_post.sh, prev };
    VkSampler res_s[3] = { g_near_samp, g_near_samp, VK_NULL_HANDLE };
    post_pass(out, 0, g_post.sresolve, lv, &u, sizeof u, res_in, res_s, GFX_PASS_POST_SHAFTS);
    GfxTex* comp_in[3] = { depth, out, g_post.sht };
    VkSampler comp_s[3] = { g_near_samp, g_near_samp, g_near_samp };
    float fv[4] = { vx, vy, vw, vh };
    VkPipeline comp = fmt_pipe(color->vf, u.hist[2] != 0.0f ? FP_SHAFT_DBG : vol ? FP_SHAFT_VOL : u.hist[3] > 0.0f ? FP_SHAFT_ADD : FP_SHAFT_COMP);
    if (comp)
        post_pass(color, 1, comp, fv, &u, sizeof u, comp_in, comp_s, GFX_PASS_POST_SHAFTS);
    g_post.shist_at ^= 1, g_post.sh_serial = g_serial;
    memcpy(g_post.sh_pvp, VP, 64), memcpy(g_post.sh_pcam, u.cam, 12);
}

/* the glow's draws again, onto black over the scene's depth (glow_new's records); 0 when there were none */
static int post_glow(GfxTex* color)
{
    VkImageView depth = VK_NULL_HANDLE;
    int stencil = 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_nglows; ++i)
        if (g_glows[i].rt == color && g_glows[i].fmt == color->vf) /* (not those from before a format change) */
        {
            if (!n++)
                depth = g_glows[i].depth, stencil = g_glows[i].stencil;
        }
    if (!n || !fx_tex(&g_post.glow, color->vf, color->w, color->h, GFX_USE_RT, 1))
        return 0;
    end_pass();
    gpu_sync();
    VkCommandBuffer cb = g_cb;
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = attachment_view(g_post.glow, 0, 0);
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = depth;
    da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ color->w, color->h };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (depth)
    {
        ri.pDepthAttachment = &da;
        if (stencil)
            ri.pStencilAttachment = &da;
    }
    int ts = ts_begin(cb, GFX_PASS_POST_BLOOM);
    vkCmdBeginRendering(cb, &ri);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
    VkPipeline bound = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < g_nglows; ++i)
    {
        Glow* g = &g_glows[i];
        if (g->rt != color || g->depth != depth || g->fmt != color->vf)
            continue;
        if (g->pipe != bound)
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipe), bound = g->pipe;
        vkCmdSetViewport(cb, 0, 1, &g->vp);
        vkCmdSetScissor(cb, 0, 1, &g->sc);
        vkCmdSetDepthTestEnable(cb, g->ztest ? VK_TRUE : VK_FALSE);
        /* tested against the scene's depth, never written: Metal discards this pass's depth (DontCare), which here would
         * leave the game's depth undefined for what reads it later (water, nameplates); the draws are additive, so the
         * order among them that a write would have kept doesn't show */
        vkCmdSetDepthWriteEnable(cb, VK_FALSE);
        vkCmdSetDepthCompareOp(cb, g->zfunc);
        vkCmdSetCullMode(cb, g->cull);
        vkCmdSetDepthBiasEnable(cb, g->zbias ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthBias(cb, -(float)g->zbias, 0.0f, -(float)g->zbias * 0.5f);
        vkCmdSetPrimitiveTopology(cb, g->topo);
        VkWriteDescriptorSet w[B_COUNT];
        for (uint32_t k = 0; k < g->nw; ++k)
        {
            const GlowBind* b = &g_glow_binds[g->bind + k];
            int buf = b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            desc_write(&w[k], b->binding, b->type, buf ? &b->b : NULL, buf ? NULL : &b->i);
        }
        p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, g->nw, w);
        if (g->indexed)
        {
            vkCmdBindIndexBuffer(cb, g->ib, g->ioff, g->itype);
            vkCmdDrawIndexed(cb, g->n, 1, 0, 0, 0);
        }
        else
            vkCmdDraw(cb, g->n, 1, g->vstart, 0);
    }
    vkCmdEndRendering(cb);
    ts_end(cb, ts);
    g_dirty = 1;
    g_post.glow->used = g_serial, g_post.glow->rec = g_cb_index;
    return 1;
}

/* the textures of the effects turned off, let go (once the frames that drew with them are done) */
static void post_release(int bloom, int dof, int shafts)
{
    if (!bloom)
    {
        for (int i = 0; i < POST_LEVELS; ++i)
            gfx_tex_destroy(g_post.lvl[i]), g_post.lvl[i] = NULL;
        gfx_tex_destroy(g_post.glow), g_post.glow = NULL;
    }
    if (!dof)
        gfx_tex_destroy(g_post.h0), gfx_tex_destroy(g_post.h1), g_post.h0 = g_post.h1 = NULL;
    if (!shafts)
        gfx_tex_destroy(g_post.sh), gfx_tex_destroy(g_post.sht), gfx_tex_destroy(g_post.shist[0]), gfx_tex_destroy(g_post.shist[1]),
            g_post.sh = g_post.sht = g_post.shist[0] = g_post.shist[1] = NULL;
}

static void post_effects(GfxTex* color, const GfxScene* s)
{
    int bloom = g_fxs.post_bloom > 0.0f, dof = g_fxs.post_dof > 0.0f, shafts = g_fxs.post_shafts > 0.0f;
    int glows = g_fxs.scene_lamps > 0.0f && g_fxs.scene_lamp_glow > 0.0f && g_lamps.binned == g_serial;
    int vol = fog_volume_on();
    post_release(bloom, dof, shafts || (g_fxs.fog_modern > 0.0f && g_fxs.fog_volume > 0.0f)); /* (the volume: the shafts' textures) */
    if (!bloom && !dof && !shafts && !glows && !vol)
    {
        g_post.sh_serial = 0;
        return;
    }
    flush_pass();
    cam_cut_note(s->view);
    GfxTex *ct = color, *depth = color->depth_world ? color->depth_world : color->depth_seen;
    if (!post_init(ct->vf))
        return;
    float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > ct->w || vy + vh > ct->h)
        vx = vy = 0, vw = (float)ct->w, vh = (float)ct->h;
    float minz, maxz;
    memcpy(&minz, &s->vp[4], 4);
    memcpy(&maxz, &s->vp[5], 4);
    if (maxz <= minz)
        minz = 0, maxz = 1;
    int full_depth = depth && depth->w == ct->w && depth->h == ct->h;
    /* a texel step down the target per step across: the scene's square stretched to the screen's shape (the
     * projection's aspect), so a blur is round on screen */
    float aspect = s->proj[0] != 0.0f ? fabsf(s->proj[5] / s->proj[0]) : 1.0f;
    PostU u = { { vx, vy, vw, vh }, { 0 }, { 0 }, { 0 }, { 0 }, { s->proj[10], s->proj[14], minz, maxz }, { aspect * vh / vw, 0, 0, 0 } };
    u.dof2[1] = s->proj[11] < 0.0f ? -1.0f : 1.0f, u.dof2[2] = POST_LEVELS, u.dof2[3] = fmaxf(g_fxs.bloom_radius, 0.0f);
    uint32_t hw = (uint32_t)(vw + 1) / 2, hh = (uint32_t)(vh + 1) / 2;
    float fv[4] = { vx, vy, vw, vh };
    if (glows && full_depth && s->proj[11] != 0.0f)
        post_lamp_glows(color, s, depth, vx, vy, vw, vh);
    if ((shafts || vol) && full_depth && s->proj[11] != 0.0f)
        post_shafts(color, s, depth, vx, vy, vw, vh); /* (before the depth of field and the bloom: both take it; the fog volume too) */
    else
        g_post.sh_serial = 0;
    const VkFormat F16 = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (dof && full_depth && s->proj[11] != 0.0f)
    {
        /* the focus: the player's distance along the view (or dof_focus), eased over some 0.2 s; a jump (a new place,
         * the camera cut) taken at once */
        float want = g_fxs.dof_focus > 0.0f ? g_fxs.dof_focus : 0.0f;
        if (want <= 0.0f && g_fx.has_focus)
        {
            const float* V = s->view;
            const float* p = g_fx.focus;
            want = fabsf(p[0] * V[2] + p[1] * V[6] + p[2] * V[10] + V[14]);
        }
        if (!(want > 0.5f) || !isfinite(want)) /* (no player, or the camera in its head: first person) */
            want = g_post.focus > 0.5f && isfinite(g_post.focus) ? g_post.focus : 10.0f;
        uint64_t now = gfx_now_ns();
        double dt = g_post.focus_ns ? (double)(now - g_post.focus_ns) * 1e-9 : 1.0;
        g_post.focus_ns = now;
        if (!(g_post.focus > 0.0f) || !isfinite(g_post.focus) || dt > 0.5 || cam_cut() || fabsf(want - g_post.focus) > 0.5f * fmaxf(want, g_post.focus))
            g_post.focus = want;
        else
            g_post.focus += (want - g_post.focus) * (float)(1.0 - exp(-dt / 0.2));
        u.dof[0] = g_post.focus, u.dof[1] = fmaxf(g_fxs.dof_range, 0.0f), u.dof[2] = fmaxf(g_fxs.dof_ramp, 1e-3f);
        u.dof[3] = g_fxs.post_dof * 4.0f * vw / 2048.0f; /* at 1: some 8 pixels of a 2048 scene */
        u.dof2[0] = fminf(fmaxf(g_fxs.dof_near, 0.0f), 1.0f);
        if (fx_tex(&g_post.h0, F16, hw, hh, GFX_USE_RT, 1) && fx_tex(&g_post.h1, F16, hw, hh, GFX_USE_RT, 1))
        {
            float hv[4] = { 0, 0, (float)hw, (float)hh };
            u.src[0] = (float)hw, u.src[1] = (float)hh, u.src[2] = 1.0f / (float)hw, u.src[3] = 1.0f / (float)hh;
            GfxTex* coc_in[3] = { ct, depth, NULL };
            VkSampler coc_s[3] = { g_near_samp, g_near_samp, VK_NULL_HANDLE };
            post_pass(g_post.h0, 0, g_post.coc, hv, &u, sizeof u, coc_in, coc_s, GFX_PASS_POST_DOF);
            GfxTex* gat_in[3] = { g_post.h0, NULL, NULL };
            post_pass(g_post.h1, 0, g_post.gather, hv, &u, sizeof u, gat_in, NULL, GFX_PASS_POST_DOF);
            GfxTex* cmp_in[3] = { g_post.h1, depth, NULL };
            VkSampler cmp_s[3] = { VK_NULL_HANDLE, g_near_samp, VK_NULL_HANDLE };
            post_pass(ct, 1, fmt_pipe(ct->vf, FP_DOF_COMP), fv, &u, sizeof u, cmp_in, cmp_s, GFX_PASS_POST_DOF);
        }
    }
    if (bloom && full_depth)
    {
        u.bloom[0] = g_fxs.bloom_threshold, u.bloom[1] = g_fxs.bloom_knee, u.bloom[2] = g_fxs.bloom_emissive_fx;
        u.aux[1] = fmaxf(g_fxs.bloom_emissive_lights, 0.0f);
        u.bloom[3] = g_fxs.post_bloom;
        GfxTex* glow = post_glow(color) ? g_post.glow : NULL; /* (none: the dummy, black) */
        uint32_t w[POST_LEVELS], h[POST_LEVELS];
        int ok = 1;
        for (int i = 0; i < POST_LEVELS; ++i)
        {
            w[i] = (uint32_t)(vw / (float)(2 << i)), h[i] = (uint32_t)(vh / (float)(2 << i));
            w[i] = w[i] ? w[i] : 1, h[i] = h[i] ? h[i] : 1;
            ok = ok && fx_tex(&g_post.lvl[i], g_post.lvl_fmt, w[i], h[i], GFX_USE_RT, 1);
        }
        if (ok)
        {
            u.src[0] = (float)ct->w, u.src[1] = (float)ct->h, u.src[2] = 1.0f / (float)ct->w, u.src[3] = 1.0f / (float)ct->h;
            GfxTex* pre_in[3] = { ct, glow, depth };
            VkSampler pre_s[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, g_near_samp };
            float lv[4] = { 0, 0, (float)w[0], (float)h[0] };
            post_pass(g_post.lvl[0], 0, g_post.pre, lv, &u, sizeof u, pre_in, pre_s, GFX_PASS_POST_BLOOM);
            for (int i = 1; i < POST_LEVELS; ++i)
            {
                u.src[0] = (float)w[i - 1], u.src[1] = (float)h[i - 1], u.src[2] = 1.0f / (float)w[i - 1], u.src[3] = 1.0f / (float)h[i - 1];
                GfxTex* in[3] = { g_post.lvl[i - 1], NULL, NULL };
                float l[4] = { 0, 0, (float)w[i], (float)h[i] };
                post_pass(g_post.lvl[i], 0, g_post.down, l, &u, sizeof u, in, NULL, GFX_PASS_POST_BLOOM);
            }
            for (int i = POST_LEVELS - 2; i >= 0; --i)
            {
                u.src[0] = (float)w[i + 1], u.src[1] = (float)h[i + 1], u.src[2] = 1.0f / (float)w[i + 1], u.src[3] = 1.0f / (float)h[i + 1];
                GfxTex* in[3] = { g_post.lvl[i + 1], NULL, NULL };
                float l[4] = { 0, 0, (float)w[i], (float)h[i] };
                post_pass(g_post.lvl[i], 1, g_post.up, l, &u, sizeof u, in, NULL, GFX_PASS_POST_BLOOM);
            }
            u.aux[3] = ct->vf == VK_FORMAT_R16G16B16A16_SFLOAT ? 1.0f : 0.0f;
            GfxTex* in[3] = { g_post.lvl[0], NULL, NULL };
            post_pass(ct, 1, fmt_pipe(ct->vf, u.aux[3] > 0.0f ? FP_BLOOM_ADD : FP_BLOOM_COMP), fv, &u, sizeof u, in, NULL, GFX_PASS_POST_BLOOM);
        }
    }
    color->scene = 0; /* its mips are behind (scene_mips) */
    color->used = g_serial, color->rec = g_cb_index;
}

/* --- the HDR scene (scene_hdr; gfx_metal.m's note) -----------------------------------------------------------------
 * The game's world target (its large background) made RGBA16F in place: a new image of the same size and levels, the
 * old one's first level drawn into it, so the frame goes on with what was drawn; its draws build their pipelines for the
 * float target as they would for any. One tonemap pass at the scene's end (hdr_tone) brings it back to the display's
 * range before the nameplates and the interface go over it - from a copy of the scene, as Vulkan has no reading the
 * pixel being shaded. The game's copies between targets of other formats are skipped (gfx_copy), as on Metal. */
static struct
{
    GfxTex* t;    /* the target made float */
    VkPipeline tone;
    GfxTex* src;  /* the scene as it was, for the tonemap to read */
} g_hdr;

static void hdr_swap(GfxTex* t, VkFormat vf)
{
    if (t->vf == vf || !post_init(t->vf))
        return;
    VkPipeline cp = fmt_pipe(vf, FP_HDR_COPY);
    if (!cp)
        return;
    /* the new image as gfx_tex_create would make it in this format (the same levels and views), taken into t */
    GfxTex* n = tex_create(GFX_TEX_2D, t->fmt, t->w, t->h, t->levels, GFX_USE_RT, vf, 0);
    if (!n)
        return;
    end_pass();
    flush_pass();
    if (n->mips != t->mips)
    {
        gfx_tex_destroy(n);
        return;
    }
    PostU u;
    memset(&u, 0, sizeof u);
    GfxTex* in[3] = { t, NULL, NULL };
    VkSampler ns[3] = { g_near_samp, VK_NULL_HANDLE, VK_NULL_HANDLE };
    float vp[4] = { 0, 0, (float)t->w, (float)t->h };
    post_pass(n, 0, cp, vp, &u, sizeof u, in, ns, GFX_PASS_HDR);
    /* n's image, views and format (its texel and conversion: tex_make's) become t's; t's go with n once the frame is done */
#define SWAP(f) (k.f = t->f, t->f = n->f, n->f = k.f)
    GfxTex k;
    SWAP(img), SWAP(mem), SWAP(view), SWAP(mipview), SWAP(rtv), SWAP(levelv), SWAP(vf), SWAP(texel), SWAP(conv);
#undef SWAP
    for (int i = 0; i < GFX_READBACKS; ++i) /* (sized for the old texel; made again on the next read) */
        if (t->rb[i])
            trash(2, (uint64_t)(uintptr_t)t->rb[i], t->rbm[i]), t->rb[i] = VK_NULL_HANDLE, t->rbsize[i] = 0, t->rb_serial[i] = 0;
    gfx_tex_destroy(n);
    tex_forget(t);
    t->scene = 0, t->used = g_serial, t->rec = g_cb_index;
}

/* the scene's target float while scene_hdr is on (and back once it is off) */
static void hdr_update(GfxTex* color)
{
    int want = g_fxs.scene_hdr > 0.0f && color->use == GFX_USE_RT && color->mips > 1 && !color->block;
    if (g_hdr.t && g_hdr.t != color)
        hdr_swap(g_hdr.t, VK_FORMAT_B8G8R8A8_UNORM), g_hdr.t = NULL; /* (another target is the world now) */
    if (want && color->vf == VK_FORMAT_B8G8R8A8_UNORM)
        hdr_swap(color, VK_FORMAT_R16G16B16A16_SFLOAT), g_hdr.t = color->vf == VK_FORMAT_R16G16B16A16_SFLOAT ? color : NULL;
    else if (!want && g_hdr.t == color && color->vf == VK_FORMAT_R16G16B16A16_SFLOAT)
        hdr_swap(color, VK_FORMAT_B8G8R8A8_UNORM), g_hdr.t = NULL;
    if (!g_hdr.t && g_hdr.src) /* (the tonemap's copy, once no target is float) */
        gfx_tex_destroy(g_hdr.src), g_hdr.src = NULL;
}

/* the tonemap over the scene's viewport, from a copy of it */
static void hdr_tone(GfxTex* color, const GfxScene* s)
{
    if (g_hdr.t != color)
        return;
    if (!g_hdr.tone)
        g_hdr.tone = post_pipeline("HDR_TONE", VK_FORMAT_R16G16B16A16_SFLOAT, 0);
    if (!g_hdr.tone || !fx_tex(&g_hdr.src, VK_FORMAT_R16G16B16A16_SFLOAT, color->w, color->h, GFX_USE_RT, 1))
        return;
    flush_pass();
    float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > color->w || vy + vh > color->h)
        vx = vy = 0, vw = (float)color->w, vh = (float)color->h;
    gpu_sync();
    VkImageCopy c = { 0 };
    c.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.dstSubresource = c.srcSubresource;
    c.srcOffset = c.dstOffset = (VkOffset3D){ (int32_t)vx, (int32_t)vy, 0 };
    c.extent = (VkExtent3D){ (uint32_t)vw, (uint32_t)vh, 1 };
    int ts = ts_begin(g_cb, GFX_PASS_HDR);
    vkCmdCopyImage(g_cb, color->img, VK_IMAGE_LAYOUT_GENERAL, g_hdr.src->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    ts_end(g_cb, ts);
    g_dirty = 1;
    PostU u;
    memset(&u, 0, sizeof u);
    u.bloom[0] = g_fxs.tonemap, u.bloom[1] = fmaxf(g_fxs.exposure, 0.01f), u.bloom[2] = fmaxf(g_fxs.tone_white, 1.01f);
    u.bloom[3] = fminf(fmaxf(g_fxs.tone_knee, 0.3f), 0.95f);
    GfxTex* in[3] = { g_hdr.src, NULL, NULL };
    VkSampler ns[3] = { g_near_samp, VK_NULL_HANDLE, VK_NULL_HANDLE };
    float fv[4] = { vx, vy, vw, vh };
    post_pass(color, 1, g_hdr.tone, fv, &u, sizeof u, in, ns, GFX_PASS_HDR);
    color->scene = 0;
}

static void hdr_forget(const GfxTex* t)
{
    if (g_hdr.t == t)
        g_hdr.t = NULL;
}

static void scene_fx(GfxTex* color, const GfxScene* s);

void gfx_scene_done(GfxTex* color, const GfxScene* s)
{
    if (!g_dev || !color || color->type != GFX_TEX_2D || color->use != GFX_USE_RT)
        return;
    hdr_update(color);
    ao_make(color); /* the scene renderer's occlusion, for the next frame's zone and characters */
    if (g_fxs.fx != 0.0f)
        scene_fx(color, s);
    scene_fog(color, s);
    post_effects(color, s); /* (the lamps' glows among them, ahead of the bloom) */
    hdr_tone(color, s);
    scene_aa(color, s);
}

/* The effects on the finished scene, in place (gfx_metal.m's scene_fx, its notes there): the
 * occlusion and sun shadows (with the sun's maps), height fog, bloom, god rays and a color grade. */
static void scene_fx(GfxTex* color, const GfxScene* s)
{
    flush_pass();
    GfxTex *ct = color, *depth = color->depth_world ? color->depth_world : color->depth_seen;
    g_fx.tr_depth = color->depth_world && color->depth_world != color->depth_seen ? 2 : depth ? 1 : 0;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    if (depth && depth->w == ct->w && depth->h == ct->h && s->proj[11] != 0.0f && fx_init())
    {
        /* the scene's viewport, within the target */
        float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
        if (vw < 16 || vh < 16 || vx + vw > ct->w || vy + vh > ct->h)
            vx = vy = 0, vw = (float)ct->w, vh = (float)ct->h;
        float minz, maxz;
        memcpy(&minz, &s->vp[4], 4);
        memcpy(&maxz, &s->vp[5], 4);
        if (maxz <= minz)
            minz = 0, maxz = 1;
        /* the occlusion at 2048 pixels across or fewer (halved until it is: the background's 4096, 6144,
         * 8192); bloom and rays, soft anyway, at about 1000 */
        uint32_t div = 1, bdiv = vw > 2048 ? 4 : vw > 1024 ? 2 : 1;
        while (vw > 2048.0f * (float)div)
            div *= 2;
        uint32_t aw = (uint32_t)((vw + div - 1) / div), ah = (uint32_t)((vh + div - 1) / div);
        uint32_t bw = (uint32_t)((vw + bdiv - 1) / bdiv), bh = (uint32_t)((vh + bdiv - 1) / bdiv);
        float hand = s->proj[11] < 0.0f ? -1.0f : 1.0f;
        FxU u = {
            { s->proj[0], s->proj[5], s->proj[8], s->proj[9] },
            { s->proj[10], s->proj[14], minz, maxz },
            { vx, vy, vw, vh },
            { (float)ct->w, (float)ct->h, (float)aw, (float)ah },
            { g_fxs.radius, g_fxs.ao, 0.15f, vh * 0.1f },
            { g_fxs.grade, g_fxs.sat, g_fxs.contrast, g_fxs.debug },
            { hand, 0, 0, 0 },
        };
        u.aop[0] = g_fxs.ao_quality >= 1.5f ? 16.0f : g_fxs.ao_quality >= 0.5f ? 10.0f : 6.0f;
        u.aop[1] = gfx_contact_steps();
        u.smapn2[2] = g_fxs.sun_soft <= 0.0f ? 1.0f : 0.0f;
        /* world up in view space: the world's y axis through the inverse view matrix, pointing the way
         * the camera's own up does (FFXI's world y points down) */
        int gap = g_fx.eased_serial && g_serial - g_fx.eased_serial > 30; /* half a second with no scene: a fade, a loading screen */
        g_fx.eased = g_fx.eased_serial && g_fx.eased_serial + 1 == g_serial;
        g_fx.eased_serial = g_serial;
        float inv[16], up[3];
        if (gfx_mat_inverse(inv, s->view))
        {
            /* a new place (the camera jumped, or a gap: logging in, a zone change, a moghouse): what is
             * eased and the sun held take this one's at once (gfx_metal.m) */
            float jx = inv[12] - g_fx.last_cam[0], jy = inv[13] - g_fx.last_cam[1], jz = inv[14] - g_fx.last_cam[2];
            if (gap || jx * jx + jy * jy + jz * jz > 50.0f * 50.0f)
                g_fx.eased = 0, g_fx.fogc_set = 0, g_fx.sunw_seen = 0;
            memcpy(g_fx.last_cam, inv + 12, 12);
            float sign = inv[5] < 0.0f ? -1.0f : 1.0f;
            up[0] = inv[1] * sign, up[1] = inv[5] * sign, up[2] = inv[9] * sign;
            gfx_normalize3(up);
            /* fog where the game fogs its world, fading in and out */
            float on = s->fog[2] != 0.0f ? 1.0f : 0.0f;
            fx_ease(&g_fx.fog_on, &on, 1, 0.1f);
            /* the fog's colour eased over about a second; a new zone or a camera jump takes it at once */
            if (g_fxs.fog <= 0.0f)
                g_fx.fogc_set = 0;
            else if (!g_fx.fogc_set)
                memcpy(g_fx.fogc, s->fogcolor, 12), g_fx.fogc_set = 1;
            else
                for (int j = 0; j < 3; ++j)
                    g_fx.fogc[j] += (s->fogcolor[j] - g_fx.fogc[j]) * 0.04f;
            memcpy(g_fx.tr_fog, s->fogcolor, 12);
            fx_ease(g_fx.up, up, 3, 0.2f);
            gfx_normalize3(g_fx.up);
            memcpy(u.up, g_fx.up, 12);
            memcpy(u.fogc, g_fx.fogc, 12);
            u.fogc[3] = g_fxs.fog * g_fx.fog_on * expf(-g_fxs.fog_falloff * g_fxs.fog_height);
            u.fogp[0] = g_fxs.fog_falloff, u.fogp[1] = g_fxs.fog_max, u.fogp[2] = g_fxs.fog_sun;
            u.fogp[3] = fminf(fmaxf(g_fxs.fog_g, 0.0f), 0.95f);
        }
        /* the sun, kept in the world: a frame gives one only when a lit draw is in view */
        float vinv[16];
        int have_v = gfx_mat_inverse(vinv, s->view), own = s->sun_dir[3] != 0.0f && have_v;
        if (own)
        {
            float w[3];
            for (int j = 0; j < 3; ++j)
                w[j] = s->sun_dir[0] * vinv[j] + s->sun_dir[1] * vinv[4 + j] + s->sun_dir[2] * vinv[8 + j];
            gfx_normalize3(w);
            /* the sun moves on in the game's steps (a quarter degree each game minute, every 2.4 s): the shadows
             * glide from each to the next over the 150 frames to it rather than jump - long at dawn and dusk, a
             * jump of their whole edge (a step past a few degrees, a new hour or place, is taken at once) */
            float dot = w[0] * g_fx.sunt[0] + w[1] * g_fx.sunt[1] + w[2] * g_fx.sunt[2];
            if (!g_fx.sunw_seen || dot < 0.995f)
                memcpy(g_fx.sunw, w, 12), memcpy(g_fx.sun0, w, 12), memcpy(g_fx.sunt, w, 12), g_fx.sunp = 1.0f;
            else if (dot < 0.999995f) /* (past the noise of the game's own, short of its quarter-degree step) */
                memcpy(g_fx.sun0, g_fx.sunw, 12), memcpy(g_fx.sunt, w, 12), g_fx.sunp = 0.0f;
            if (g_fx.sunp < 1.0f)
            {
                g_fx.sunp = fminf(g_fx.sunp + 1.0f / 150.0f, 1.0f);
                for (int j = 0; j < 3; ++j)
                    g_fx.sunw[j] = g_fx.sun0[j] + (g_fx.sunt[j] - g_fx.sun0[j]) * g_fx.sunp;
                gfx_normalize3(g_fx.sunw);
            }
            g_fx.sunw_seen = g_serial;
            fx_ease(g_fx.suncol, s->sun_color, 3, 0.1f);
        }
        /* the sun kept for whatever wants it: its shadows, the god rays and the fog's glow toward it (Low has no
         * shadows, and its rays would go out with no lit draw in view - the sky, where they show) */
        int wanted = g_fxs.sun > 0.0f || g_fxs.shadow > 0.0f || g_fxs.rays > 0.0f || (g_fxs.fog > 0.0f && g_fxs.fog_sun > 0.0f);
        if (have_v && (own || (wanted && g_fx.sunw_seen && g_fx.sunw_seen + 600 > g_serial)))
        {
            for (int j = 0; j < 3; ++j)
                g_fx.sun[j] = g_fx.sunw[0] * s->view[j] + g_fx.sunw[1] * s->view[4 + j] + g_fx.sunw[2] * s->view[8 + j];
            gfx_normalize3(g_fx.sun);
            memcpy(u.sun, g_fx.sun, 12);
            u.sun[3] = 1.0f;
            memcpy(u.suncol, g_fx.suncol, 12);
            /* shadows while the sun (or moon) is up: fading as it nears the horizon */
            float e = g_fx.sun[0] * g_fx.up[0] + g_fx.sun[1] * g_fx.up[1] + g_fx.sun[2] * g_fx.up[2];
            float day = g_fxs.sun_dusk != 0.0f ? fminf(fmaxf(e / 0.05f, 0.0f), 1.0f) : fminf(fmaxf((e - 0.05f) / 0.15f, 0.0f), 1.0f);
            if (own)
            {
                float dl = 0.2126f * s->sun_color[0] + 0.7152f * s->sun_color[1] + 0.0722f * s->sun_color[2];
                float al = 0.2126f * s->ambient[0] + 0.7152f * s->ambient[1] + 0.0722f * s->ambient[2];
                float r = dl / fmaxf(dl + al, 1e-3f);
                float want = fminf(fmaxf((r - g_fxs.sun_direct) / 0.3f, 0.0f), 1.0f);
                g_fx.direct = g_fx.eased && g_fx.direct >= 0.0f ? g_fx.direct + (want - g_fx.direct) * 0.05f : want;
                g_fx.tr_dl = dl, g_fx.tr_al = al;
            }
            day *= g_fxs.sun_direct > 0.0f ? g_fx.direct : 1.0f;
            /* in a Mog House, or under a zone's fixed light (GfxScene.indoors, easing out once the sun moves) */
            float room = fminf(fmaxf(g_fxs.moghouse, 0.0f), 1.0f);
            day *= g_moghouse ? room : 1.0f - fminf(fmaxf(s->indoors, 0.0f), 1.0f) * (1.0f - room);
            u.shadow[0] = g_fxs.shadow * day;
            u.shadow[1] = g_fxs.shadow_length, u.shadow[2] = 0.3f, u.shadow[3] = 40.0f;
            if (g_fxs.sun > 0.0f && day > 0.0f && sun_map(s, g_fx.sunw, &u))
            {
                u.smap[0] = g_fxs.sun * day, g_fx.st_drawn_this = 1;
                u.gi[0] *= g_fxs.gi * day;
                if (day >= 0.25f)
                    g_sun_shown = g_serial;
            }
            else
                u.gi[0] = 0.0f;
            g_fx.tr_strength = u.smap[0], g_fx.tr_day = day;
            u.smap2[1] = fminf(fmaxf(g_fxs.sun_face, 0.0f), 1.0f), u.smap2[2] = fmaxf(g_fxs.sun_min, 0.0f);
            /* the sun's place on screen: far along its direction, through the projection */
            const float* sd = g_fx.sun;
            float cw = sd[2] * s->proj[11];
            if (cw > 0.05f)
            {
                float nx = (sd[0] * s->proj[0] + sd[2] * s->proj[8]) / cw;
                float ny = (sd[1] * s->proj[5] + sd[2] * s->proj[9]) / cw;
                u.sunuv[0] = nx * 0.5f + 0.5f, u.sunuv[1] = 0.5f - ny * 0.5f;
                float out = fmaxf(fabsf(u.sunuv[0] - 0.5f), fabsf(u.sunuv[1] - 0.5f)) - 0.5f; /* past the edge */
                u.sunuv[2] = fminf(fmaxf(1.0f - out / 0.5f, 0.0f), 1.0f) * fminf(cw / 0.3f, 1.0f);
            }
        }
        /* the frame before, for the temporal pass: its camera, if it was the frame just before, at this
         * size, and not a jump away */
        {
            float vinv2[16], m[16];
            int cam = g_fx.prev_serial && g_fx.prev_serial + 1 == g_serial && gfx_mat_inverse(vinv2, s->view);
            if (cam)
            {
                float dx = vinv2[12] - g_fx.prev_cam[0], dy = vinv2[13] - g_fx.prev_cam[1], dz = vinv2[14] - g_fx.prev_cam[2];
                cam = dx * dx + dy * dy + dz * dz < 25.0f;
            }
            if (cam)
            {
                gfx_mat_mul(m, vinv2, g_fx.prev_view);
                gfx_mat_mul(u.reproj, m, g_fx.prev_proj);
                u.hist[0] = g_fx.hist_serial && g_fx.hist_serial + 1 == g_serial && g_fx.hist[0] && g_fx.hist[0]->w == aw &&
                    g_fx.hist[0]->h == ah ? 1.0f : 0.0f;
                /* the bounce light's too, at its own size */
                u.gip[0] = g_fx.gih_serial && g_fx.gih_serial + 1 == g_serial && g_fx.gih[0] && g_fx.gih[0]->w == (aw + 1) / 2 &&
                    g_fx.gih[0]->h == (ah + 1) / 2 ? 1.0f : 0.0f;
            }
            g_fx.prev_serial = g_serial;
            /* the pattern's turn: a golden-ratio step each frame */
            u.hist[1] = (float)fmod((double)g_serial * 0.6180339887, 1.0);
            u.hist[2] = fminf(fmaxf(g_fxs.temporal, 0.0f), 0.95f);
            memcpy(g_fx.prev_view, s->view, 64), memcpy(g_fx.prev_proj, s->proj, 64);
            if (gfx_mat_inverse(vinv2, s->view))
                memcpy(g_fx.prev_cam, vinv2 + 12, 12);
        }
        u.bloom[0] = g_fxs.threshold, u.bloom[1] = g_fxs.bloom, u.bloom[2] = 0.25f;
        u.rays[0] = u.sunuv[2] > 0.0f ? g_fxs.rays : 0.0f, u.rays[1] = g_fxs.rays_decay, u.rays[2] = g_fxs.rays_length;
        if (g_fxs.water > 0.0f && have_v)
            water_scene(s, vinv, &u, ct);
        const VkFormat F16 = VK_FORMAT_R16G16B16A16_SFLOAT;
        /* the world for rays, before the passes that trace them */
        int rt = 0;
        if ((g_fxs.rt > 0.0f || (int)g_fxs.debug == 8) && have_v)
        {
            float invP[16];
            rt = gfx_mat_inverse(invP, s->proj) && rt_capture(invP, s->view, vinv + 12);
        }
        uint32_t gw = (aw + 1) / 2, gh = (ah + 1) / 2;
        int gi = u.gi[0] > 0.0f && fx_tex(&g_fx.gi0, F16, gw, gh, GFX_USE_RT, 1) && fx_tex(&g_fx.gih[0], F16, gw, gh, GFX_USE_RT, 1) &&
            fx_tex(&g_fx.gih[1], F16, gw, gh, GFX_USE_RT, 1) && (!rt || fx_tex(&g_fx.gi1, F16, gw, gh, GFX_USE_RT, 1));
        if (!gi)
            u.gi[0] = 0.0f;
        u.gip[1] = fminf(fmaxf(g_fxs.temporal, 0.0f), 0.95f), u.gip[2] = 12.0f; /* the history's share; the gather's samples */
        if (rt) /* traced: rays reach further, two a texel, kept longer over frames (their noise averaged away) */
            u.gi[2] = fmaxf(g_fxs.gi_radius * 2.5f, 4.0f), u.gip[2] = 2.0f, u.gip[1] = u.gip[1] > 0.0f ? fmaxf(u.gip[1], 0.95f) : 0.0f;
        if (fx_tex(&g_fx.src, ct->vf, ct->w, ct->h, GFX_USE_RT, 1) && fx_tex(&g_fx.ao0, F16, aw, ah, GFX_USE_RT, 1) &&
            fx_tex(&g_fx.ao1, F16, aw, ah, GFX_USE_RT, 1) && fx_tex(&g_fx.b1a, F16, bw, bh, GFX_USE_RT, 1) &&
            fx_tex(&g_fx.b1b, F16, bw, bh, GFX_USE_RT, 1) && fx_tex(&g_fx.b2a, F16, (bw + 1) / 2, (bh + 1) / 2, GFX_USE_RT, 1) &&
            fx_tex(&g_fx.b2b, F16, (bw + 1) / 2, (bh + 1) / 2, GFX_USE_RT, 1) && fx_tex(&g_fx.ra, F16, bw, bh, GFX_USE_RT, 1) &&
            fx_tex(&g_fx.rb, F16, bw, bh, GFX_USE_RT, 1))
        {
            g_fx.comp_pipe = fmt_pipe(ct->vf, FP_FX_COMP);
            if (g_fx.comp_pipe)
            {
                gpu_sync();
                image_copy(ct, g_fx.src, VK_IMAGE_ASPECT_COLOR_BIT);
                g_dirty = 1;
                static const int32_t across[2] = { 1, 0 }, down[2] = { 0, 1 }, across2[2] = { 2, 0 }, down2[2] = { 0, 2 };
                GfxTex* ao_out = g_fx.ao0;
                if (u.ao[1] > 0.0f || u.shadow[0] > 0.0f || u.smap[0] > 0.0f)
                {
                    GfxTex* lz = fx_lz(aw, ah);
                    if (!lz)
                        u.ao[1] = 0.0f;
                    fx_uniforms(&u);
                    g_pass = GFX_PASS_LINZ;
                    if (lz)
                    {
                        fx_pass(lz, 0, 0, g_fx.linz_pipe, NULL, &depth->view, 1, NULL, NULL);
                        for (uint32_t l = 1; l < 4; ++l)
                        {
                            VkImageView prev = level_view(lz, l - 1);
                            fx_pass(lz, l, 0, g_fx.zmip_pipe, NULL, &prev, 1, NULL, NULL);
                        }
                    }
                    int far_on = u.smap[0] > 0.0f, near_on = far_on && u.smapn2[1] > 0.0f;
                    VkImageView ao_in[4] = { depth->view, far_on ? g_fx.smap->view : g_fx.sdummy->view,
                        near_on ? g_fx.smapn->view : g_fx.sdummy->view, lz ? lz->view : VK_NULL_HANDLE };
                    VkImageView sh[2] = { ao_in[1], ao_in[2] };
                    g_pass = GFX_PASS_AO;
                    fx_pass(g_fx.ao0, 0, 0, g_fx.ao_pipe, NULL, ao_in, 4, sh, NULL);
                    /* traced (rt): the occlusion from rays in the world, the sun's as fx_ao found it; ao1 then holds it */
                    GfxTex *a = g_fx.ao0, *bt = g_fx.ao1;
                    if (rt && u.ao[1] > 0.0f)
                    {
                        VkImageView r_in[2] = { g_fx.ao0->view, depth->view };
                        g_pass = GFX_PASS_AO_RT;
                        rt_pass(g_fx.ao1, 0, g_ray.ao, NULL, r_in, 2, NULL);
                        a = g_fx.ao1, bt = g_fx.ao0;
                    }
                    g_pass = GFX_PASS_BLUR;
                    fx_pass(bt, 0, 0, g_fx.blur_pipe, NULL, &a->view, 1, NULL, across);
                    fx_pass(a, 0, 0, g_fx.blur_pipe, NULL, &bt->view, 1, NULL, down);
                    ao_out = a;
                    if (g_fxs.temporal > 0.0f && fx_tex(&g_fx.hist[0], F16, aw, ah, GFX_USE_RT, 1) &&
                        fx_tex(&g_fx.hist[1], F16, aw, ah, GFX_USE_RT, 1))
                    {
                        int to = g_fx.hist_at ^ 1;
                        VkImageView t_in[2] = { a->view, g_fx.hist[g_fx.hist_at]->view };
                        g_pass = GFX_PASS_TEMPORAL;
                        fx_pass(g_fx.hist[to], 0, 0, g_fx.temporal_pipe, NULL, t_in, 2, NULL, NULL);
                        ao_out = g_fx.hist[to];
                        g_fx.hist_at = to, g_fx.hist_serial = g_serial;
                    }
                }
                else
                    fx_uniforms(&u);
                /* the bounce light: gathered from its map at half the occlusion's size, then over frames */
                GfxTex* gi_out = NULL;
                if (gi)
                {
                    g_pass = GFX_PASS_GI;
                    if (rt)
                    {
                        /* traced (rt): rays from each texel into the world, the light they meet as the frame or the
                         * sun saw it; smoothed four times (one texel apart, two, four, one) before the temporal pass */
                        static const int32_t s1[2] = { 1, 1 }, s2[2] = { 2, 2 }, s4[2] = { 4, 4 };
                        VkImageView r_in[5] = { depth->view, g_fx.src->view, ao_out->view, g_fx.gimap->view, g_fx.gicol->view };
                        rt_pass(g_fx.gi0, 0, g_ray.gi, NULL, r_in, 5, NULL);
                        rt_pass(g_fx.gi1, 0, g_ray.giblur, NULL, &g_fx.gi0->view, 1, s1);
                        rt_pass(g_fx.gi0, 0, g_ray.giblur, NULL, &g_fx.gi1->view, 1, s2);
                        rt_pass(g_fx.gi1, 0, g_ray.giblur, NULL, &g_fx.gi0->view, 1, s4);
                        rt_pass(g_fx.gi0, 0, g_ray.giblur, NULL, &g_fx.gi1->view, 1, s1);
                    }
                    else
                    {
                        VkImageView gi_in[3] = { depth->view, g_fx.gimap->view, g_fx.gicol->view };
                        g_fx.mip_in = 1u << 2;
                        fx_pass(g_fx.gi0, 0, 0, g_fx.gi_pipe, NULL, gi_in, 3, NULL, NULL);
                        g_fx.mip_in = 0;
                    }
                    gi_out = g_fx.gi0;
                    if (u.gip[1] > 0.0f)
                    {
                        int to = g_fx.gih_at ^ 1;
                        VkImageView t_in[2] = { g_fx.gi0->view, g_fx.gih[g_fx.gih_at]->view };
                        fx_pass(g_fx.gih[to], 0, 0, g_fx.gitemp_pipe, NULL, t_in, 2, NULL, NULL);
                        gi_out = g_fx.gih[to];
                        g_fx.gih_at = to, g_fx.gih_serial = g_serial;
                    }
                }
                if (u.bloom[1] > 0.0f)
                {
                    VkImageView b_in[2] = { g_fx.src->view, ao_out->view }; /* shaded as the composite shades */
                    g_pass = GFX_PASS_BLOOM;
                    fx_pass(g_fx.b1a, 0, 0, g_fx.bright_pipe, NULL, b_in, 2, NULL, NULL);
                    fx_pass(g_fx.b1b, 0, 0, g_fx.gauss_pipe, NULL, &g_fx.b1a->view, 1, NULL, across2);
                    fx_pass(g_fx.b1a, 0, 0, g_fx.gauss_pipe, NULL, &g_fx.b1b->view, 1, NULL, down2);
                    fx_pass(g_fx.b2a, 0, 0, g_fx.down_pipe, NULL, &g_fx.b1a->view, 1, NULL, NULL);
                    fx_pass(g_fx.b2b, 0, 0, g_fx.gauss_pipe, NULL, &g_fx.b2a->view, 1, NULL, across2);
                    fx_pass(g_fx.b2a, 0, 0, g_fx.gauss_pipe, NULL, &g_fx.b2b->view, 1, NULL, down2);
                }
                if (u.rays[0] > 0.0f)
                {
                    VkImageView mask_in[2] = { g_fx.src->view, depth->view };
                    g_pass = GFX_PASS_RAYS;
                    fx_pass(g_fx.ra, 0, 0, g_fx.raymask_pipe, NULL, mask_in, 2, NULL, NULL);
                    fx_pass(g_fx.rb, 0, 0, g_fx.rays_pipe, NULL, &g_fx.ra->view, 1, NULL, NULL);
                    fx_pass(g_fx.ra, 0, 0, g_fx.gauss_pipe, NULL, &g_fx.rb->view, 1, NULL, across);
                    fx_pass(g_fx.rb, 0, 0, g_fx.gauss_pipe, NULL, &g_fx.ra->view, 1, NULL, down);
                }
                VkImageView comp_in[7] = { g_fx.src->view, ao_out->view, depth->view, g_fx.b1a->view, g_fx.b2a->view, g_fx.rb->view,
                    gi_out ? gi_out->view : VK_NULL_HANDLE };
                float vp[4] = { vx, vy, vw, vh };
                g_pass = GFX_PASS_COMP;
                fx_pass(ct, 0, 1, g_fx.comp_pipe, vp, comp_in, 7, NULL, NULL);
                if (rt && (int)g_fxs.debug == 8)
                {
                    if (!g_ray.clay || g_ray.clay_fmt != ct->vf)
                        g_ray.clay = rt_fx_pipeline("CLAY", ct->vf), g_ray.clay_fmt = ct->vf;
                    if (g_ray.clay)
                        rt_pass(ct, 1, g_ray.clay, vp, &depth->view, 1, NULL);
                }
                depth->used = g_serial, depth->rec = g_cb_index;
            }
        }
    }
    color->scene = 0; /* the effects changed it: its mips are behind (scene_mips) */
    color->used = g_serial;
    if (gfx_profiling)
    {
        g_fx.st_frames++;
        g_fx.st_own += s->sun_dir[3] != 0.0f;
        g_fx.st_map += g_fx.st_drawn_this;
        g_fx.st_cmin = g_fx.st_frames == 1 || g_ncasters < g_fx.st_cmin ? g_ncasters : g_fx.st_cmin;
        g_fx.st_cmax = g_ncasters > g_fx.st_cmax ? g_ncasters : g_fx.st_cmax;
        uint64_t now = gfx_now_ns();
        if (now - g_fx.st_last > 2000000000ull)
        {
            fprintf(stderr, "[recomp] gfx: shadows: %u frames, %u with the sun's own light, %u with a map; casters %u..%u; "
                "%u cached; map %.0f units across; sun %.2f %.2f %.2f\n", g_fx.st_frames, g_fx.st_own, g_fx.st_map, g_fx.st_cmin,
                g_fx.st_cmax, g_fx.st_cached, g_fx.st_across, g_fx.sunw[0], g_fx.sunw[1], g_fx.sunw[2]);
            g_fx.st_last = now, g_fx.st_frames = g_fx.st_own = g_fx.st_map = g_fx.st_cmax = 0;
            if (g_ray.st_frames)
                fprintf(stderr, "[recomp] gfx: ray tracing: %u frames; casters %u live, %u from the cache, %u waiting for pipelines; "
                    "%llu triangles a frame\n", g_ray.st_frames, g_ray.st_live, g_ray.st_cached, g_ray.st_skipped,
                    (unsigned long long)(g_ray.st_tris / g_ray.st_frames));
            g_ray.st_frames = g_ray.st_live = g_ray.st_cached = g_ray.st_skipped = 0, g_ray.st_tris = 0;
        }
    }
    {
        __typeof__(g_fx.trace[0])* t = &g_fx.trace[g_fx.ntrace++ % 1200];
        float inv[16];
        t->serial = g_serial, t->live = g_fx.tr_live, t->cached = g_fx.tr_cached, t->skipped = g_fx.tr_skipped;
        memcpy(t->fog, g_fx.tr_fog, 12);
        t->own = s->sun_dir[3] != 0.0f, t->depth = g_fx.tr_depth, t->dl = g_fx.tr_dl, t->al = g_fx.tr_al, t->strength = g_fx.tr_strength, t->day = g_fx.tr_day;
        memcpy(t->sun, g_fx.sunw, 12);
        if (gfx_mat_inverse(inv, s->view))
            memcpy(t->cam, inv + 12, 12);
        t->across = g_fx.st_across;
        g_fx.tr_live = g_fx.tr_cached = g_fx.tr_skipped = 0, g_fx.tr_strength = g_fx.tr_day = 0;
    }
    g_fx.st_drawn_this = 0;
    casters_clear();
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0;
}

/* --- the window: swapchain, present ------------------------------------------------------------------------------ */
static const char UTIL_GLSL[] =
    "#ifdef GFX_VS\n"
    "layout(location = 0) out vec2 uv;\n"
    "layout(push_constant) uniform PC { vec4 rect; vec2 size; float scale; uint n; uvec4 text[2]; } pc;\n"
    "void main() {\n"
    "#ifdef OVERLAY\n"
    "  vec2 c = pc.rect.xy + vec2((gl_VertexIndex & 1) != 0 ? pc.rect.z : 0.0, (gl_VertexIndex & 2) != 0 ? pc.rect.w : 0.0);\n"
    "  gl_Position = vec4(c / pc.size * 2.0 - 1.0, 0.0, 1.0);\n"
    "  uv = c;\n"
    "#else\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "  uv = p;\n"
    "#endif\n"
    "}\n"
    "#else\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 oc;\n"
    "layout(set = 0, binding = 0) uniform sampler2D t;\n"
    "layout(push_constant) uniform PC { vec4 rect; vec2 size; float scale; uint n; uvec4 text[2]; } pc;\n"
    "#ifdef OVERLAY\n"
    /* the frame-rate overlay: a 5x7 bitmap font drawn per pixel, no texture (gfx_metal.m's) */
    "const uint FONT[17 * 7] = uint[17 * 7](\n"
    "  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,\n"
    "  0x1F,0x02,0x04,0x02,0x01,0x11,0x0E, 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,\n"
    "  0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, 0x1F,0x01,0x02,0x04,0x08,0x08,0x08, 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,\n"
    "  0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C, 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,\n"
    "  0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, 0x00,0x00,0x1A,0x15,0x15,0x11,0x11, 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E,\n"
    "  0x00,0x00,0x00,0x00,0x00,0x0C,0x0C, 0x00,0x00,0x00,0x00,0x00,0x00,0x00);\n"
    "void main() {\n"
    "  vec2 p = (gl_FragCoord.xy - pc.rect.xy) / pc.scale - 2.0;\n"
    "  int cell = int(floor(p.x / 6.0)), gx = int(floor(p.x)) - cell * 6, gy = int(floor(p.y));\n"
    "  oc = vec4(0, 0, 0, 0.55);\n"
    "  if (p.x >= 0.0 && cell < int(pc.n) && gx < 5 && gy >= 0 && gy < 7) {\n"
    "    uint ch = (pc.text[cell >> 4][(cell >> 2) & 3] >> (8 * (cell & 3))) & 0xFFu;\n"
    "    if (((FONT[ch * 7u + uint(gy)] >> (4 - gx)) & 1u) != 0u) oc = vec4(1.0, 0.85, 0.2, 1.0);\n"
    "  }\n"
    "}\n"
    "#elif defined(CAS)\n"
    /* sharpened by k (0..1): contrast-adaptive, as gfx_metal.m's present_cas_fs */
    "void main() {\n"
    "  float k = pc.rect.x;\n"
    "  vec2 tx = 1.0 / vec2(textureSize(t, 0));\n"
    "  vec3 c = texture(t, uv).rgb;\n"
    "  vec3 n = texture(t, uv - vec2(0, tx.y)).rgb, so = texture(t, uv + vec2(0, tx.y)).rgb;\n"
    "  vec3 w = texture(t, uv - vec2(tx.x, 0)).rgb, e = texture(t, uv + vec2(tx.x, 0)).rgb;\n"
    "  vec3 mn = min(c, min(min(n, so), min(w, e))), mx = max(c, max(max(n, so), max(w, e)));\n"
    "  vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, 1e-4), 0.0, 1.0));\n"
    "  vec3 lobe = -amp * mix(0.125, 0.2, clamp(k, 0.0, 1.0));\n"
    "  oc = vec4(clamp((c + (n + so + w + e) * lobe) / (1.0 + 4.0 * lobe), 0.0, 1.0), 1.0);\n"
    "}\n"
    "#else\n"
    "void main() { oc = vec4(texture(t, uv).rgb, 1.0); }\n"
    "#endif\n"
    "#endif\n";

typedef struct UtilPC
{
    float rect[4], size[2], scale;
    uint32_t n, text[8];
} UtilPC;

static VkPipeline util_pipeline(const char* define, VkFormat fmt, int blend)
{
    size_t n = sizeof UTIL_GLSL + 64;
    char* src = (char*)malloc(n);
    snprintf(src, n, "#define %s\n%s", define, UTIL_GLSL);
    VkShaderModule vs = compile_glsl(src, 0), fs = compile_glsl(src, 1);
    free(src);
    VkPipeline p = VK_NULL_HANDLE;
    if (vs && fs)
    {
        VkPipelineColorBlendAttachmentState b = { 0 };
        b.colorWriteMask = 0xF;
        if (blend)
        {
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        }
        static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        p = make_pipeline(vs, fs, g_util_layout, &b, fmt, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
            blend ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, dyn, 2, 0);
    }
    if (vs)
        vkDestroyShaderModule(g_dev, vs, NULL);
    if (fs)
        vkDestroyShaderModule(g_dev, fs, NULL);
    return p;
}

static void swap_destroy(void)
{
    for (uint32_t i = 0; i < g_swap_count; ++i)
    {
        vkDestroyImageView(g_dev, g_swap_views[i], NULL);
        vkDestroySemaphore(g_dev, g_swap_done[i], NULL);
    }
    free(g_swap_images), free(g_swap_views), free(g_swap_done);
    g_swap_images = NULL, g_swap_views = NULL, g_swap_done = NULL, g_swap_count = 0;
}

static int swap_create(void)
{
    if (!g_surface)
        return 0;
    vkDeviceWaitIdle(g_dev);
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps));
    int pw = 0, ph = 0;
    if (g_window)
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu)
        ext = (VkExtent2D){ (uint32_t)(pw > 0 ? pw : 1280), (uint32_t)(ph > 0 ? ph : 720) };
    if (!ext.width || !ext.height)
        return 0; /* minimized */
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, NULL);
    VkSurfaceFormatKHR* fs = (VkSurfaceFormatKHR*)calloc(nf ? nf : 1, sizeof *fs);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, fs);
    VkSurfaceFormatKHR sf = nf ? fs[0] : (VkSurfaceFormatKHR){ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    for (uint32_t i = 0; i < nf; ++i) /* UNORM: the game's colors go out as they are, as on Metal */
        if ((fs[i].format == VK_FORMAT_B8G8R8A8_UNORM || fs[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fs[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            sf = fs[i];
            break;
        }
    free(fs);
    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &nm, NULL);
    VkPresentModeKHR* ms = (VkPresentModeKHR*)calloc(nm ? nm : 1, sizeof *ms);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &nm, ms);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_vsync)
        for (uint32_t i = 0; i < nm; ++i)
            if (ms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR || (ms[i] == VK_PRESENT_MODE_MAILBOX_KHR && mode == VK_PRESENT_MODE_FIFO_KHR))
                mode = ms[i];
    free(ms);
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = g_surface;
    ci.minImageCount = count;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                           : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g_swap;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(g_dev, &ci, NULL, &sc);
    swap_destroy();
    if (g_swap)
        vkDestroySwapchainKHR(g_dev, g_swap, NULL);
    g_swap = sc;
    if (r != VK_SUCCESS)
    {
        fprintf(stderr, "[recomp] gfx: swapchain failed: %d\n", (int)r);
        g_swap = VK_NULL_HANDLE;
        return 0;
    }
    g_swap_format = sf.format, g_swap_extent = ext;
    vkGetSwapchainImagesKHR(g_dev, g_swap, &g_swap_count, NULL);
    g_swap_images = (VkImage*)calloc(g_swap_count, sizeof(VkImage));
    g_swap_views = (VkImageView*)calloc(g_swap_count, sizeof(VkImageView));
    g_swap_done = (VkSemaphore*)calloc(g_swap_count, sizeof(VkSemaphore));
    vkGetSwapchainImagesKHR(g_dev, g_swap, &g_swap_count, g_swap_images);
    for (uint32_t i = 0; i < g_swap_count; ++i)
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = g_swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g_swap_format;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(g_dev, &vi, NULL, &g_swap_views[i]));
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(g_dev, &si, NULL, &g_swap_done[i]));
    }
    if (g_present_pipe_format != g_swap_format)
    {
        g_present_pipe = util_pipeline("PRESENT", g_swap_format, 0);
        g_present_cas_pipe = util_pipeline("CAS", g_swap_format, 0);
        g_overlay_pipe = util_pipeline("OVERLAY", g_swap_format, 1);
        g_present_pipe_format = g_swap_format;
    }
    g_swap_stale = 0;
    fprintf(stderr, "[recomp] gfx: swapchain %ux%u, %u images, %s\n", ext.width, ext.height, g_swap_count,
        mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
    return 1;
}

/* "60 FPS 16.7ms": the text of the overlay, from the presents of the last half second */
static void fps_tick(void)
{
    uint64_t now = gfx_now_ns();
    if (!g_fps_since)
        g_fps_since = now;
    g_fps_frames++;
    double dt = (double)(now - g_fps_since) * 1e-9;
    if (dt >= 0.5)
    {
        double fps = g_fps_frames / dt;
        snprintf(g_fps_text, sizeof g_fps_text, "%.0f FPS %.1fms", fps, dt * 1000.0 / g_fps_frames);
        g_fps_since = now, g_fps_frames = 0;
    }
}

static void draw_overlay(VkCommandBuffer cb, uint32_t w, uint32_t h)
{
    UtilPC u;
    memset(&u, 0, sizeof u);
    for (const char* c = g_fps_text; *c && u.n < 32; ++c, ++u.n)
    {
        uint32_t k = *c >= '0' && *c <= '9' ? (uint32_t)(*c - '0') : *c == 'F' ? 10 : *c == 'P' ? 11 : *c == 'S' ? 12
            : *c == 'm' ? 13 : *c == 's' ? 14 : *c == '.' ? 15 : 16;
        u.text[u.n >> 2] |= k << (8 * (u.n & 3));
    }
    u.scale = (float)(h >= 1400 ? 3 : 2);
    u.rect[0] = u.rect[1] = 4 * u.scale;
    u.rect[2] = (float)(u.n * 6 + 3) * u.scale, u.rect[3] = 11 * u.scale;
    u.size[0] = (float)w, u.size[1] = (float)h;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_overlay_pipe);
    vkCmdPushConstants(cb, g_util_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof u, &u);
    vkCmdDraw(cb, 4, 1, 0, 0);
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src_stage,
    VkAccessFlags src, VkPipelineStageFlags dst_stage, VkAccessFlags dst)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src, b.dstAccessMask = dst;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* how long a frame waits for the window's next image: one the window system never hands back (#75, KDE
 * on Wayland at /shutdown) leaves frames unshown, the game going on, not stuck in vkAcquireNextImageKHR
 * (gfx_metal.m's nextDrawable gives up after a second) */
#define ACQUIRE_TIMEOUT_NS 100000000ull
static uint32_t g_timeouts; /* acquires timed out since the window last gave images steadily (60 in a row) */
static uint32_t g_steady;   /* images in a row since one timed out */

/* the back buffer onto the window's next image; 0 when there is none to draw to now */
static int present_to_window(GfxTex* bb)
{
    if (g_swap_stale || !g_swap)
        if (!swap_create())
            return 0;
    Frame* f = &g_frames[g_frame];
    uint32_t idx = 0;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    VkResult r = vkAcquireNextImageKHR(g_dev, g_swap, ACQUIRE_TIMEOUT_NS, f->acquired, VK_NULL_HANDLE, &idx);
    if (gfx_profiling)
        g_prof.acquire_ns += gfx_now_ns() - t0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        g_swap_stale = 1;
        return 0;
    }
    if (r == VK_TIMEOUT || r == VK_NOT_READY)
    {
        if (!g_timeouts++)
            fprintf(stderr, "[recomp] gfx: no image from the window in %llu ms: frames go unshown until it gives one\n",
                ACQUIRE_TIMEOUT_NS / 1000000);
        g_steady = 0;
        return 0;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return 0;
    if (g_timeouts && ++g_steady == 60) /* (one that gives an image now and then logs once, not with each) */
        fprintf(stderr, "[recomp] gfx: images from the window again, after %u acquires timed out\n", g_timeouts), g_timeouts = 0;
    gpu_sync();
    VkCommandBuffer cb = g_cb;
    image_barrier(cb, g_swap_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = g_swap_views[idx];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = g_swap_extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    int ts = ts_begin(cb, GFX_PASS_PRESENT);
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)g_swap_extent.width, (float)g_swap_extent.height, 0, 1 };
    VkRect2D sc = { { 0, 0 }, g_swap_extent };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float sharpen = g_fxs.fx != 0.0f ? g_fxs.sharpen : 0.0f;
    VkPipeline p = sharpen > 0.0f && g_present_cas_pipe ? g_present_cas_pipe : g_present_pipe;
    if (p)
    {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        UtilPC u;
        memset(&u, 0, sizeof u);
        u.rect[0] = sharpen;
        vkCmdPushConstants(cb, g_util_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof u, &u);
        VkDescriptorImageInfo ii = { g_lin_samp, bb->view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_util_layout, 0, 1, &w);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    if (g_fxs.fps != 0.0f && g_overlay_pipe)
        draw_overlay(cb, g_swap_extent.width, g_swap_extent.height);
    vkCmdEndRendering(cb);
    ts_end(cb, ts);
    if (g_ts_state[g_frame] == 1)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ts_pool[g_frame], TS_FRAME + 1), g_ts_state[g_frame] = 2;
    image_barrier(cb, g_swap_images[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    bb->used = g_serial, bb->rec = g_cb_index;
    submit_ex(0, f->acquired, g_swap_done[idx]);
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_swap_done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swap;
    pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(g_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        g_swap_stale = 1;
    return 1;
}

static void frame_end(void)
{
    casters_clear();
    glows_clear();
    submit(0);
    Frame* f = &g_frames[g_frame];
    if (!f->value)
        f->value = g_submits; /* nothing of its own: done with whatever went before */
    g_frame_open = 0;
    g_frame = (g_frame + 1) % FRAMES;
    g_serial++;
}

void gfx_present(GfxTex* bb)
{
    if (!g_dev)
        return;
    uint64_t present_start = gfx_profiling ? gfx_now_ns() : 0;
    g_present_thread = pthread_self();
    flush_pass();
    shadow_frame_end(); /* the scene renderer's characters' shadows, for the next frame: timed with the frame's passes */
    if (g_surface && bb && bb->use != GFX_USE_DEPTH)
    {
        int pw = 0, ph = 0;
        if (g_window)
            SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        if (g_swap && pw > 0 && ph > 0 && ((uint32_t)pw != g_swap_extent.width || (uint32_t)ph != g_swap_extent.height))
            g_swap_stale = 1;
        cmd();
        present_to_window(bb);
    }
    fps_tick();
    fx_reload();
    frame_end();
    vk_cache_save();
    gfx_pass_tick();
    if (gfx_profiling)
        prof_frame(present_start);
}

void gfx_finish(void)
{
    if (!g_dev)
        return;
    submit(1);
}

void gfx_resize(uint32_t w, uint32_t h)
{
    (void)w, (void)h;
    g_swap_stale = 1;
}

/* --- bring-up ----------------------------------------------------------------------------------------------------- */
uint64_t gfx_window_flags(void) { return SDL_WINDOW_VULKAN; }

static int pick_device(void)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, NULL);
    if (!n)
        return 0;
    VkPhysicalDevice* ds = (VkPhysicalDevice*)calloc(n, sizeof *ds);
    vkEnumeratePhysicalDevices(g_inst, &n, ds);
    const char* want = getenv("FFXI_GPU"); /* a name to prefer, part of it */
    int best = -1, best_score = -1;
    uint32_t best_q = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(ds[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ds[i], &nq, NULL);
        VkQueueFamilyProperties* qs = (VkQueueFamilyProperties*)calloc(nq, sizeof *qs);
        vkGetPhysicalDeviceQueueFamilyProperties(ds[i], &nq, qs);
        int q = -1;
        for (uint32_t j = 0; j < nq && q < 0; ++j)
        {
            VkBool32 present = VK_TRUE;
            if (g_surface)
                vkGetPhysicalDeviceSurfaceSupportKHR(ds[i], j, g_surface, &present);
            if ((qs[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                q = (int)j;
        }
        free(qs);
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(ds[i], NULL, &ne, NULL);
        VkExtensionProperties* es = (VkExtensionProperties*)calloc(ne ? ne : 1, sizeof *es);
        vkEnumerateDeviceExtensionProperties(ds[i], NULL, &ne, es);
        int push = 0, swap = 0;
        for (uint32_t j = 0; j < ne; ++j)
            push |= !strcmp(es[j].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME),
                swap |= !strcmp(es[j].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        free(es);
        if (q < 0 || !push || (g_surface && !swap))
            continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
            : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 1 : 2;
        if (want && *want && strstr(p.deviceName, want))
            score += 100;
        if (score > best_score)
            best = (int)i, best_score = score, best_q = (uint32_t)q;
    }
    if (best >= 0)
        g_phys = ds[best], g_qfam = best_q;
    free(ds);
    return best >= 0;
}

static GfxTex* dummy_texture(int type)
{
    GfxTex* t = tex_create(type, F_A8R8G8B8, 1, 1, 1, GFX_USE_SAMPLE, VK_FORMAT_UNDEFINED, 1);
    return t; /* cleared to zeros at creation */
}

int gfx_init(void* window, int vsync)
{
    if (g_dev)
    {
        /* up already (host64's sign-in screen, on this same window): the game's present interval */
        if (g_vsync != vsync)
            g_vsync = vsync, g_swap_stale = 1;
        return 1;
    }
    g_window = (SDL_Window*)window;
    g_vsync = vsync;
    if (volkInitialize() != VK_SUCCESS)
    {
        fprintf(stderr, "[recomp] gfx: no Vulkan loader (libvulkan.so.1)\n");
        return 0;
    }
    glslang_initialize_process();

    /* the instance: what SDL needs for the window's surface */
    uint32_t nsdl = 0;
    const char* const* sdl_ext = g_window ? SDL_Vulkan_GetInstanceExtensions(&nsdl) : NULL;
    const char* ext[16];
    uint32_t next = 0;
    for (uint32_t i = 0; i < nsdl && next < 15; ++i)
        ext[next++] = sdl_ext[i];
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "FINAL FANTASY XI";
    app.pEngineName = "XI on Anything";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* layers[1] = { "VK_LAYER_KHRONOS_validation" };
    const char* val = getenv("FFXI_VK_VALIDATION");
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = next;
    ici.ppEnabledExtensionNames = ext;
    ici.enabledLayerCount = val && val[0] == '1' ? 1 : 0;
    ici.ppEnabledLayerNames = layers;
    VkResult r = vkCreateInstance(&ici, NULL, &g_inst);
    if (r != VK_SUCCESS && ici.enabledLayerCount)
        ici.enabledLayerCount = 0, r = vkCreateInstance(&ici, NULL, &g_inst);
    if (r != VK_SUCCESS)
    {
        fprintf(stderr, "[recomp] gfx: no Vulkan instance (%d)\n", (int)r);
        return 0;
    }
    volkLoadInstance(g_inst);
    if (g_window && !SDL_Vulkan_CreateSurface(g_window, g_inst, NULL, &g_surface))
    {
        fprintf(stderr, "[recomp] gfx: no Vulkan surface for the window: %s\n", SDL_GetError());
        g_surface = VK_NULL_HANDLE;
    }
    if (!pick_device())
    {
        fprintf(stderr, "[recomp] gfx: no Vulkan 1.3 device with push descriptors%s\n", g_surface ? " that can present" : "");
        return 0;
    }
    vkGetPhysicalDeviceProperties(g_phys, &g_props);

    /* the device: Vulkan 1.3's dynamic rendering and state, 1.2's timeline semaphores */
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(g_phys, &have);
    VkPhysicalDeviceVulkan12Features have12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR have_as = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR have_rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceFeatures2 have2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    have2.pNext = &have12;
    have12.pNext = &have_as, have_as.pNext = &have_rq;
    vkGetPhysicalDeviceFeatures2(g_phys, &have2);
    /* ray tracing (rt_capture): ray queries over acceleration structures, buffers by their addresses, the
     * capture's vertex functions writing, and the alpha tests' textures indexed per ray */
    {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(g_phys, NULL, &ne, NULL);
        VkExtensionProperties* es = (VkExtensionProperties*)calloc(ne ? ne : 1, sizeof *es);
        vkEnumerateDeviceExtensionProperties(g_phys, NULL, &ne, es);
        int found = 0;
        for (uint32_t j = 0; j < ne; ++j)
            found += !strcmp(es[j].extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ||
                !strcmp(es[j].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) ||
                !strcmp(es[j].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        free(es);
        /* FFXI_RT_OFF=1: none, for a driver whose builds misbehave. lavapipe on aarch64 (Mesa 25.2) faults in
         * its own structure builder - the Linux test box on an ARM Mac - so the CPU's device there has none */
        const char* no = getenv("FFXI_RT_OFF");
        int cpu_arm = 0;
#if defined(__aarch64__)
        cpu_arm = g_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
#endif
        g_has_rt = found == 3 && have_as.accelerationStructure && have_rq.rayQuery && have12.bufferDeviceAddress &&
            have.vertexPipelineStoresAndAtomics && have12.shaderSampledImageArrayNonUniformIndexing &&
            have12.descriptorBindingPartiallyBound && !(no && no[0] == '1') && !cpu_arm;
    }
    g_has_bc = have.textureCompressionBC, g_has_aniso = have.samplerAnisotropy, g_has_lines = have.fillModeNonSolid;
    g_has_large_points = have.largePoints, g_has_mirror_once = have12.samplerMirrorClampToEdge;
    g_max_aniso = g_props.limits.maxSamplerAnisotropy;
    VkPhysicalDeviceFeatures en = { 0 };
    en.textureCompressionBC = have.textureCompressionBC;
    en.samplerAnisotropy = have.samplerAnisotropy;
    en.fillModeNonSolid = have.fillModeNonSolid;
    en.largePoints = have.largePoints;
    en.depthClamp = have.depthClamp;
    g_has_clamp = have.depthClamp;
    en.vertexPipelineStoresAndAtomics = g_has_rt ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceRayQueryFeaturesKHR en_rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    en_rq.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR en_as = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    en_as.accelerationStructure = VK_TRUE;
    en_as.pNext = &en_rq;
    VkPhysicalDeviceVulkan13Features en13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    en13.dynamicRendering = VK_TRUE;
    en13.pNext = g_has_rt ? &en_as : NULL;
    VkPhysicalDeviceVulkan12Features en12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    en12.pNext = &en13;
    en12.timelineSemaphore = VK_TRUE;
    en12.samplerMirrorClampToEdge = have12.samplerMirrorClampToEdge;
    if (g_has_rt)
        en12.bufferDeviceAddress = en12.shaderSampledImageArrayNonUniformIndexing = en12.descriptorBindingPartiallyBound = VK_TRUE;
    VkPhysicalDeviceFeatures2 en2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    en2.pNext = &en12;
    en2.features = en;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = g_qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char* dext[5] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
    uint32_t ndext = 1;
    if (g_surface)
        dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (g_has_rt)
        dext[ndext++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, dext[ndext++] = VK_KHR_RAY_QUERY_EXTENSION_NAME,
        dext[ndext++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &en2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = ndext;
    dci.ppEnabledExtensionNames = dext;
    r = vkCreateDevice(g_phys, &dci, NULL, &g_dev);
    if (r != VK_SUCCESS)
    {
        fprintf(stderr, "[recomp] gfx: no Vulkan device (%d)\n", (int)r);
        g_dev = VK_NULL_HANDLE;
        return 0;
    }
    volkLoadDevice(g_dev);
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    p_push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(g_dev, "vkCmdPushDescriptorSetKHR");

    VmaVulkanFunctions vf = { 0 };
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ai = { 0 };
    ai.flags = g_has_rt ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0;
    ai.pVulkanFunctions = &vf;
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.physicalDevice = g_phys;
    ai.device = g_dev;
    ai.instance = g_inst;
    VK_CHECK(vmaCreateAllocator(&ai, &g_vma));

    /* D24S8: D32_SFLOAT_S8_UINT (Metal's choice), else D24_UNORM_S8_UINT */
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
    g_depth_format = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D32_SFLOAT_S8_UINT
                                                                                                 : VK_FORMAT_D24_UNORM_S8_UINT;

    VkSemaphoreTypeCreateInfo st = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sci.pNext = &st;
    VK_CHECK(vkCreateSemaphore(g_dev, &sci, NULL, &g_tl));
    for (int i = 0; i < FRAMES; ++i)
    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = g_qfam;
        VK_CHECK(vkCreateCommandPool(g_dev, &pci, NULL, &g_frames[i].pool));
        VkSemaphoreCreateInfo bsi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(g_dev, &bsi, NULL, &g_frames[i].acquired));
    }
    /* the timestamps per pass (gfx_pass_timing), where the queue has them */
    {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, NULL);
        VkQueueFamilyProperties* qp = (VkQueueFamilyProperties*)calloc(nq ? nq : 1, sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, qp);
        uint32_t bits = g_qfam < nq ? qp[g_qfam].timestampValidBits : 0;
        free(qp);
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = TS_QUERIES;
        int ok = bits > 0 && g_props.limits.timestampPeriod > 0.0f;
        for (int i = 0; ok && i < FRAMES; ++i)
            ok = vkCreateQueryPool(g_dev, &qi, NULL, &g_ts_pool[i]) == VK_SUCCESS;
        if (ok)
            g_ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1, g_ts_period = (double)g_props.limits.timestampPeriod;
    }

    /* the draw functions' bindings (gfx_msl.h), pushed per draw */
    VkDescriptorSetLayoutBinding b[B_COUNT];
    uint32_t nb = 0;
    b[nb++] = (VkDescriptorSetLayoutBinding){ B_U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (uint32_t s = 0; s < GFX_NSTREAMS; ++s)
        b[nb++] = (VkDescriptorSetLayoutBinding){ B_STREAM0 + s, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    b[nb++] = (VkDescriptorSetLayoutBinding){ B_SHADOW, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    b[nb++] = (VkDescriptorSetLayoutBinding){ B_RT_OUT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    b[nb++] = (VkDescriptorSetLayoutBinding){ B_RT_C, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    b[nb++] = (VkDescriptorSetLayoutBinding){ B_RT_IDX, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    for (uint32_t i = 0; i < 8; ++i)
        b[nb++] = (VkDescriptorSetLayoutBinding){ B_TEX0 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
            NULL };
    b[nb++] = (VkDescriptorSetLayoutBinding){ B_WATER_U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b[nb++] = (VkDescriptorSetLayoutBinding){ B_WATER_COL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
        NULL };
    b[nb++] = (VkDescriptorSetLayoutBinding){ B_WATER_DEPTH, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = nb;
    dl.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, &g_dsl));
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_dsl;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &pl, NULL, &g_layout));
    /* the present's and overlay's: one texture, and push constants */
    VkDescriptorSetLayoutBinding ub = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    dl.bindingCount = 1;
    dl.pBindings = &ub;
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, &g_util_dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(UtilPC) };
    pl.pSetLayouts = &g_util_dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &pl, NULL, &g_util_layout));

    void* p;
    host_buffer(256, STATIC_USAGE, 0, &g_dummy, &g_dummy_mem, &p);
    memset(p, 0, 256);
    g_dummy2d = dummy_texture(GFX_TEX_2D);
    g_dummycube = dummy_texture(GFX_TEX_CUBE);
    /* the fixed samplers, clamped: linear (the present's, the effects'), nearest, and the depth maps' comparing one
     * (LESSEQUAL), filtered where the device filters both depth formats the maps have */
    VkSamplerCreateInfo ps = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ps.magFilter = ps.minFilter = VK_FILTER_LINEAR;
    ps.addressModeU = ps.addressModeV = ps.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.maxLod = 0.25f;
    VK_CHECK(vkCreateSampler(g_dev, &ps, NULL, &g_lin_samp));
    ps.magFilter = ps.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(g_dev, &ps, NULL, &g_near_samp));
    VkFilter zf = VK_FILTER_LINEAR;
    for (int i = 0; i < 2; ++i)
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_phys, i ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            zf = VK_FILTER_NEAREST;
    }
    ps.magFilter = ps.minFilter = zf;
    ps.compareEnable = VK_TRUE;
    ps.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(g_dev, &ps, NULL, &g_cmp_samp));

    const char* prof = getenv("FFXI_PROFILE");
    gfx_profiling = prof && prof[0] && prof[0] != '0';
    fx_config();
    const char* show = getenv("FFXI_FPS");
    if (show && show[0] == '0')
        g_fxs.fps = 0.0f;

    vk_cache_load();
    if (!g_sync_pipelines)
    {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        g_workers = cores > 4 ? 3 : cores > 2 ? 2 : 1;
        for (int i = 0; i < g_workers; ++i)
        {
            pthread_t t;
            pthread_create(&t, NULL, worker, NULL);
            pthread_detach(t);
        }
        prewarm_pipelines();
    }
    if (g_surface)
        swap_create();
    fprintf(stderr, "[recomp] gfx: Vulkan %u.%u on %s%s%s\n", VK_API_VERSION_MAJOR(g_props.apiVersion),
        VK_API_VERSION_MINOR(g_props.apiVersion), g_props.deviceName, g_has_bc ? "" : " (no BC textures)",
        g_has_rt ? " (ray queries)" : "");
    return 1;
}
