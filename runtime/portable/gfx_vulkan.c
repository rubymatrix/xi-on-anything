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
 * the same passes in GLSL (FX_GLSL), the same CPU side (gfx_scene.c). Not here: the GPU timestamps
 * per part of the frame in the profile. */
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
};

/* the bindings of the draw functions' set (gfx_msl.h) */
enum
{
    B_U = 0,
    B_STREAM0 = 1,
    B_SHADOW = 5,
    B_TEX0 = 8,
    B_WATER_U = 16,
    B_WATER_COL = 17,
    B_WATER_DEPTH = 18,
    B_COUNT = 19,
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
};

struct GfxBuf
{
    VkBuffer b;
    VmaAllocation mem;
    void* p;
    uint32_t size;
    uint64_t used;
    uint64_t up_last, up_prev; /* the frames of its last two uploads (buf_volatile) */
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
    int kind; /* 0 image, 1 view, 2 buffer */
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
static VkSampler g_present_samp;

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

static uint64_t fnv(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h ? h : 1;
}

static void* map_get(Map* m, const void* key, size_t klen)
{
    if (!m->cap)
        return NULL;
    uint64_t h = fnv(key, klen);
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
    uint64_t h = fnv(key, klen);
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
static void trash(int kind, uint64_t h, VmaAllocation mem)
{
    if (!h)
        return;
    Frame* f = &g_frames[g_frame];
    if (f->ntrash == f->cap)
    {
        f->cap = f->cap ? f->cap * 2 : 64;
        f->trash = (Trash*)realloc(f->trash, f->cap * sizeof(Trash));
    }
    f->trash[f->ntrash++] = (Trash){ kind, h, mem };
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

static void frame_begin(void)
{
    if (g_frame_open)
        return;
    Frame* f = &g_frames[g_frame];
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    wait_value(f->value);
    if (gfx_profiling)
        g_prof.sem_ns += gfx_now_ns() - t0;
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

static void end_pass(void)
{
    if (g_in_pass)
    {
        vkCmdEndRendering(g_cb);
        g_in_pass = 0;
        g_dirty = 1;
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

/* one row of a 16-bit D3D format into BGRA8 */
static void convert_row(int conv, const uint8_t* src, uint8_t* dst, uint32_t w)
{
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

static uint32_t layers(const GfxTex* t) { return t->type == GFX_TEX_CUBE ? 6 : 1; }

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

/* force: the format (the effects' own targets), else the D3D format's; plain: no mip chain for a
 * large render target (the effects' own) */
static GfxTex* tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use, VkFormat force, int plain)
{
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    VkComponentMapping sw;
    t->vf = vk_format(fmt, use, &t->conv, &t->block, &t->texel, &sw);
    if (force)
        t->vf = force;
    t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->levels = levels ? levels : 1;
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
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = { 0 };
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(g_vma, &ii, &ai, &t->img, &t->mem, NULL) != VK_SUCCESS)
    {
        fprintf(stderr, "[recomp] gfx: texture %ux%u format %u failed\n", w, h, fmt);
        free(t);
        return NULL;
    }
    VkImageViewType vt = type == GFX_TEX_CUBE ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
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

GfxTex* gfx_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    return tex_create(type, fmt, w, h, levels, use, VK_FORMAT_UNDEFINED, 0);
}

static void sun_cache_forget_tex(const GfxTex* t);
static void water_forget(const GfxTex* t);

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
    sun_cache_forget_tex(t);
    water_forget(t);
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
static VkShaderModule compile_glsl(const char* text, int fragment)
{
    size_t n = strlen(text);
    char* src = (char*)malloc(n + 64);
    snprintf(src, n + 64, "#version 450\n#define %s\n%s", fragment ? "GFX_FS" : "GFX_VS", text);
    glslang_stage_t stage = fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
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
static VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, const VkPipelineColorBlendAttachmentState* blend,
    VkFormat color, VkFormat depth, VkFormat stencil, VkPrimitiveTopology topo, VkPolygonMode fill, const VkDynamicState* dyn,
    uint32_t ndyn)
{
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
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.front.compareOp = ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = color ? 1 : 0; /* none: depth alone (the sun's maps) */
    cb.pAttachments = blend;
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = ndyn;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = color ? 1 : 0;
    ri.pColorAttachmentFormats = &color;
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

static VkPipeline build_pipeline(const PipeKey* k, const uint32_t* vs, const uint32_t* ps)
{
    char* src = gfx_glsl_generate(&k->lib.vs, &k->lib.fs, vs, ps);
    if (!src)
    {
        atomic_fetch_add(&g_failures, 1);
        return VK_NULL_HANDLE;
    }
    VkShaderModule vm = compile_glsl(src, 0);
    int depth_only = k->lib.vs.shadow && !alpha_tested(&k->lib.fs); /* nothing for the fragments to do */
    VkShaderModule fm = depth_only ? VK_NULL_HANDLE : compile_glsl(src, 1);
    free(src);
    VkPipeline p = VK_NULL_HANDLE;
    if (vm && (fm || depth_only))
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
            k->fill ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL, DRAW_DYNAMIC, sizeof DRAW_DYNAMIC / sizeof DRAW_DYNAMIC[0]);
    }
    if (vm)
        vkDestroyShaderModule(g_dev, vm, NULL);
    if (fm)
        vkDestroyShaderModule(g_dev, fm, NULL);
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
    if (tw * th < 1024)
        return; /* not the sun flare's 16x16 occlusion probe */
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
    const GfxU* up; /* the uniforms, as bound */
    VkImageView tex[8]; /* only for an alpha test */
    GfxSampler samp[8];
    VkPrimitiveTopology prim;
    uint32_t n, vstart;
    uint8_t itype; /* 0 no indices, 2 or 4 bytes each */
    uint8_t fixed; /* every vertex and index from buffers the game keeps (the zone's): cached (sun_cache) */
    uint8_t keep;  /* not fixed, but a placed object (not a character): kept as a copy (sun_cache_update) */
    uint8_t has_pos;
    int32_t zbias;
    float clip0[4]; /* its first vertex in the camera's clip space (gfx_clip0): where this copy stands */
} Caster;

static Caster* g_casters;
static uint32_t g_ncasters, g_casters_cap;

static void casters_clear(void) { g_ncasters = 0; }

/* clip-space position of the draw's first vertex in out (gfx_clip0); 0 when it cannot be had */
static int draw_clip0(const GfxDraw* d, float out[4])
{
    const uint8_t* base[GFX_NSTREAMS];
    size_t have[GFX_NSTREAMS];
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        base[s] = NULL, have[s] = 0;
        if (d->buf[s])
            base[s] = (const uint8_t*)d->buf[s]->p + d->buf_off[s], have[s] = d->buf[s]->size > d->buf_off[s] ? d->buf[s]->size - d->buf_off[s] : 0;
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
    }
    Caster* c = &g_casters[g_ncasters++];
    memset(c, 0, sizeof *c);
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
    memcpy(u, &d->u, need);
    if (d->caster && !g_rt_face && !g_rt_level && depth)
        g_rt->depth_world = depth;
    Caster* rec = d->caster && g_fxs.fx != 0.0f && g_fxs.sun > 0.0f ? caster_new(d) : NULL;
    if (rec)
        rec->ub = ub, rec->uoff = uoff, rec->up = u;
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
                    u->offset[r] += (int32_t)rem;
        w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[nw].dstBinding = B_STREAM0 + (uint32_t)s;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw++].pBufferInfo = b;
    }
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
        if (rec && alpha_tested(&d->fs))
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
    "} u;\n"
    "layout(push_constant) uniform FxPC { ivec2 dir; } pc;\n"
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
    "  const int NS = 16;\n"
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
    "void main() {\n"
    "  vec2 px = gl_FragCoord.xy;\n"
    "  vec4 c = texelFetch(t0, ivec2(px), 0);\n"
    "  int dbg = int(u.grade.w);\n"
    "  vec3 os = u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0 ? ao_at(vuv, view_z(depth_at(t2, px)) * u.hand.x) : vec3(1.0);\n"
    "  float o = os.x, sun = mix(1.0, os.y, u.smap.x) * mix(1.0, os.z, u.shadow.x);\n"
    "  if (dbg == 1) { oc = vec4(o, o, o, c.a); return; }\n"
    "  if (dbg == 5) { oc = vec4(vec3(sun), c.a); return; }\n"
    "  c.rgb *= mix(1.0, o, u.ao.y) * sun;\n"
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
    "  x = mix(x, x * x * (3.0 - 2.0 * x), u.grade.z);\n"
    "  c.rgb = mix(c.rgb, x, u.grade.x);\n"
    "  oc = c;\n"
    "}\n"
    "#endif\n"
    "#endif\n";

static struct
{
    int tried;
    VkShaderModule vs;
    VkPipeline ao_pipe, blur_pipe, bright_pipe, down_pipe, gauss_pipe, raymask_pipe, rays_pipe, comp_pipe, temporal_pipe, aa_pipe,
        linz_pipe, zmip_pipe;
    VkFormat comp_fmt, aa_fmt;
    GfxTex* lz;     /* the occlusion's depth: view z at its size and three levels below (FX_LINZ, FX_ZMIP) */
    GfxTex* aa_src; /* the scene as it was, for the anti-aliasing pass to read (scene_aa) */
    GfxTex *src, *ao0, *ao1, *b1a, *b1b, *b2a, *b2b, *ra, *rb;
    GfxTex* hist[2];      /* the occlusion and shadows after the temporal pass: this frame's and the one before */
    int hist_at;          /* which of hist[] the frame before wrote */
    uint64_t hist_serial; /* the frame it was written (0: none) */
    float prev_view[16], prev_proj[16], prev_cam[3];
    float last_cam[3]; /* where the camera was at the last scene (a jump is a new place) */
    VkSampler samp, cmp;
    GfxTex *smap, *smapn, *sdummy; /* the sun's shadow maps (far, near), a stand-in */
    float focus[3];                /* the player's place in the world (gfx_set_focus) */
    int has_focus;
    /* what the fog and rays follow, eased from frame to frame (fx_ease) */
    int eased;
    uint64_t eased_serial;
    float fog_on, fogc[3], up[3], sun[3], suncol[3];
    float sunw[3]; /* toward the sun in the world, as the last lit draw gave it, and the frame it was seen */
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

void gfx_set_focus(const float* pos)
{
    g_fx.has_focus = pos != NULL;
    if (pos)
        memcpy(g_fx.focus, pos, sizeof g_fx.focus);
}

/* one pass's pipeline: the shared vertex function, FX_<name>, into a target of format fmt */
static VkPipeline fx_pipeline(const char* name, VkFormat fmt)
{
    size_t n = sizeof FX_GLSL + 64;
    char* src = (char*)malloc(n);
    snprintf(src, n, "#define FX_%s\n%s", name, FX_GLSL);
    VkShaderModule fs = compile_glsl(src, 1);
    free(src);
    VkPipeline p = VK_NULL_HANDLE;
    if (fs && g_fx.vs)
    {
        VkPipelineColorBlendAttachmentState b = { 0 };
        b.colorWriteMask = 0xF;
        static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        p = make_pipeline(g_fx.vs, fs, g_fx_layout, &b, fmt, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_POLYGON_MODE_FILL, dyn, 2);
    }
    if (fs)
        vkDestroyShaderModule(g_dev, fs, NULL);
    if (!p)
        fprintf(stderr, "[recomp] gfx: scene effect %s failed\n", name);
    return p;
}

static int fx_init(void)
{
    if (g_fx.tried)
        return g_fx.ao_pipe != VK_NULL_HANDLE;
    g_fx.tried = 1;
    /* the passes' bindings: their uniforms, six textures, the two sun maps with a compare sampler */
    VkDescriptorSetLayoutBinding b[9];
    b[0] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (uint32_t i = 1; i < 9; ++i)
        b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 9;
    dl.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, &g_fx_dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_fx_dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &pl, NULL, &g_fx_layout));
    g_fx.vs = compile_glsl(FX_GLSL, 0);
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
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.25f;
    VK_CHECK(vkCreateSampler(g_dev, &si, NULL, &g_fx.samp));
    /* 1 where the point is no deeper than the map; filtered where the device filters depth */
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_phys, VK_FORMAT_D32_SFLOAT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(g_dev, &si, NULL, &g_fx.cmp));
    g_fx.sdummy = tex_create(GFX_TEX_2D, F_D16, 1, 1, 1, GFX_USE_DEPTH, VK_FORMAT_D32_SFLOAT, 1);
    if (!g_fx.ao_pipe || !g_fx.blur_pipe || !g_fx.bright_pipe || !g_fx.down_pipe || !g_fx.gauss_pipe || !g_fx.raymask_pipe ||
        !g_fx.rays_pipe || !g_fx.temporal_pipe || !g_fx.linz_pipe || !g_fx.zmip_pipe || !g_fx.sdummy)
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

/* one full-screen triangle into a level of target (within vp, x y w h; NULL: all of it), reading
 * in[0..n) as t0.., and the sun maps sh[0..2) through the compare sampler; load keeps what is there */
static void fx_pass(GfxTex* target, uint32_t level, int load, VkPipeline p, const float* vp, const VkImageView* in, int n,
    const VkImageView* sh, const int32_t* dir)
{
    end_pass();
    gpu_sync();
    VkCommandBuffer cb = g_cb;
    uint32_t tw, th;
    level_size(target, level, &tw, &th);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = attachment_view(target, 0, level);
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ tw, th };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport v = vp ? (VkViewport){ vp[0], vp[1], vp[2], vp[3], 0, 1 } : (VkViewport){ 0, 0, (float)tw, (float)th, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { tw, th } };
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkWriteDescriptorSet w[9];
    VkDescriptorBufferInfo bi = { g_fx.ub, g_fx.uoff, sizeof(FxU) };
    VkDescriptorImageInfo ii[8];
    w[0] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &bi;
    for (int i = 0; i < 8; ++i)
    {
        if (i < 6)
            ii[i] = (VkDescriptorImageInfo){ g_fx.samp, i < n && in[i] ? in[i] : g_dummy2d->view, VK_IMAGE_LAYOUT_GENERAL };
        else
            ii[i] = (VkDescriptorImageInfo){ g_fx.cmp, sh && sh[i - 6] ? sh[i - 6] : g_fx.sdummy->view, VK_IMAGE_LAYOUT_GENERAL };
        w[1 + i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[1 + i].dstBinding = 1 + (uint32_t)i;
        w[1 + i].descriptorCount = 1;
        w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1 + i].pImageInfo = &ii[i];
    }
    p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fx_layout, 0, 9, w);
    int32_t d2[2] = { dir ? dir[0] : 0, dir ? dir[1] : 0 };
    vkCmdPushConstants(cb, g_fx_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, d2);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    g_dirty = 1;
    g_bound = VK_NULL_HANDLE;
    target->used = g_serial, target->rec = g_cb_index;
}

/* the occlusion's depth texture at w x h, four levels; made again when the size changes */
static GfxTex* fx_lz(uint32_t w, uint32_t h)
{
    if (w < 8 || h < 8)
        return NULL;
    return fx_tex(&g_fx.lz, VK_FORMAT_R32_SFLOAT, w, h, GFX_USE_RT, 4);
}

enum { SUN_CACHE_FRAMES = 60 * 30 };
#define SUN_CACHE_NEAR 40.0f               /* what was seen within this of the camera stays past SUN_CACHE_FRAMES */
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
    int dead;            /* a buffer it draws from went (the game let go of it: a zone left behind) */
    VkBuffer own, ubuf;
    VmaAllocation own_mem, ubuf_mem;
    void *own_p, *ubuf_p;
} Cached;

static Cached* g_cache;
static uint32_t g_ncache, g_cache_cap, g_copy_bytes;
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

/* a texture the game destroyed: the cache's alpha tests no longer read it */
static void sun_cache_forget_tex(const GfxTex* t)
{
    for (uint32_t i = 0; i < g_ncache; ++i)
        for (int k = 0; k < 8; ++k)
            if (g_cache[i].c.tex[k] && (g_cache[i].c.tex[k] == t->view || g_cache[i].c.tex[k] == t->mipview))
                g_cache[i].c.tex[k] = VK_NULL_HANDLE;
}

/* gone from the cache's view: not seen for SUN_CACHE_FRAMES, and the camera has moved on from where it was */
static int cached_expired(const Cached* ce)
{
    if (ce->seen + SUN_CACHE_FRAMES >= g_serial)
        return 0;
    float dx = g_cache_cam[0] - ce->cam[0], dy = g_cache_cam[1] - ce->cam[1], dz = g_cache_cam[2] - ce->cam[2];
    return dx * dx + dy * dy + dz * dz > SUN_CACHE_NEAR * SUN_CACHE_NEAR;
}

/* drops what is dead or expired (all of it when all is true) and rebuilds the map and its chains */
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
    g_ncache = n;
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
    if (!(g_serial & 255))
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
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* c = &g_casters[i];
        const GfxU* src = c->up;
        if (!c->fixed && (!c->keep || !c->has_pos))
            continue;
        float pos[3] = { 0, 0, 0 };
        if (c->has_pos)
        {
            float h[4];
            gfx_xform4(h, c->clip0, clip_world);
            if (fabsf(h[3]) < 1e-6f)
                continue;
            pos[0] = h[0] / h[3], pos[1] = h[1] / h[3], pos[2] = h[2] / h[3];
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
        memcpy(ce->ubuf_p, src, sizeof(GfxU));
        ce->c.ub = ce->ubuf, ce->c.uoff = 0, ce->c.up = (const GfxU*)ce->ubuf_p;
        if (copy && !cache_copy(ce, c, (GfxU*)ce->ubuf_p))
        {
            ce->dead = 1;
            continue;
        }
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

/* the casters into one cascade's map: this frame's, and the zone's kept from before (cache) */
static uint32_t sun_draw(GfxTex* target, const float* invP, const float* invV, const SunCascade* k, int cache)
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
            /* sun_casters 1: characters alone cast; 2: the zone alone */
            if ((g_fxs.sun_casters == 1.0f && (cs->fixed || cs->keep)) || (g_fxs.sun_casters == 2.0f && !cs->fixed && !cs->keep))
                continue;
            /* more than 96 units outside the map's sides: no shadow of it falls in it */
            if (cs->has_pos)
            {
                float h[4];
                gfx_xform4(h, cs->clip0, clip_world);
                if (fabsf(h[3]) > 1e-6f)
                {
                    float q[3] = { h[0] / h[3], h[1] / h[3], h[2] / h[3] };
                    float mx = q[0] * k->S[0] + q[1] * k->S[4] + q[2] * k->S[8] + k->S[12];
                    float my = q[0] * k->S[1] + q[1] * k->S[5] + q[2] * k->S[9] + k->S[13];
                    float edge = 1.0f + 96.0f * 2.0f / k->across;
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
        int at = alpha_tested(&cs->lib.fs);
        if (!at)
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
        pk.color = VK_FORMAT_UNDEFINED, pk.depth = VK_FORMAT_D32_SFLOAT;
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
        if (at)
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
    g_dirty = 1;
    g_bound = VK_NULL_HANDLE;
    target->used = g_serial, target->rec = g_cb_index;
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
    uint32_t drawn = sun_draw(g_fx.smap, invP, invV, &far, 1);
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
        sun_draw(g_fx.smapn, invP, invV, &near, 1);
        memcpy(u->lmatn, near.lmat, 64);
        u->smapn[0] = near.texel, u->smapn[1] = near.bias, u->smapn[2] = near.soft, u->smapn[3] = near.slope;
        u->smapn2[0] = near.range, u->smapn2[1] = 1.0f;
    }
    g_fx.st_across = far.across;
    g_fx.st_cached = g_ncache;
    return drawn != 0;
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
    VkBuffer b;
    VkDeviceSize off;
    memcpy(ring(sizeof g_fx.wu, RING_ALIGN, &b, &off), &g_fx.wu, sizeof g_fx.wu);
    *bi = (VkDescriptorBufferInfo){ b, off, sizeof g_fx.wu };
    ii[0] = (VkDescriptorImageInfo){ g_fx.samp, g_fx.wcol->view, VK_IMAGE_LAYOUT_GENERAL };
    ii[1] = (VkDescriptorImageInfo){ g_fx.samp, g_fx.wdep->view, VK_IMAGE_LAYOUT_GENERAL };
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
    if (!g_fx.aa_pipe || g_fx.aa_fmt != color->vf)
    {
        g_fx.aa_pipe = fx_pipeline("FXAA", color->vf);
        g_fx.aa_fmt = color->vf;
    }
    if (!g_fx.aa_pipe || !fx_tex(&g_fx.aa_src, color->vf, color->w, color->h, GFX_USE_RT, 1))
        return;
    gpu_sync();
    image_copy(color, g_fx.aa_src, VK_IMAGE_ASPECT_COLOR_BIT);
    g_dirty = 1;
    FxU u;
    memset(&u, 0, sizeof u);
    u.size[0] = (float)color->w, u.size[1] = (float)color->h;
    fx_uniforms(&u);
    fx_pass(color, 0, 1, g_fx.aa_pipe, vp, &g_fx.aa_src->view, 1, NULL, NULL);
    color->scene = 0; /* its mips are behind (scene_mips) */
}

static void scene_fx(GfxTex* color, const GfxScene* s);

void gfx_scene_done(GfxTex* color, const GfxScene* s)
{
    if (!g_dev || !color || color->type != GFX_TEX_2D || color->use != GFX_USE_RT)
        return;
    if (g_fxs.fx != 0.0f)
        scene_fx(color, s);
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
            /* the sun moves on in steps of a quarter degree: the shadows' edges hold still between them */
            float dot = w[0] * g_fx.sunw[0] + w[1] * g_fx.sunw[1] + w[2] * g_fx.sunw[2];
            if (!g_fx.sunw_seen || dot < 0.99999f)
                memcpy(g_fx.sunw, w, 12);
            g_fx.sunw_seen = g_serial;
            fx_ease(g_fx.suncol, s->sun_color, 3, 0.1f);
        }
        int shadows = g_fxs.sun > 0.0f || g_fxs.shadow > 0.0f;
        if (have_v && (own || (shadows && g_fx.sunw_seen && g_fx.sunw_seen + 600 > g_serial)))
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
            if (g_moghouse)
                day *= fminf(fmaxf(g_fxs.moghouse, 0.0f), 1.0f);
            u.shadow[0] = g_fxs.shadow * day;
            u.shadow[1] = g_fxs.shadow_length, u.shadow[2] = 0.3f, u.shadow[3] = 40.0f;
            if (g_fxs.sun > 0.0f && day > 0.0f && sun_map(s, g_fx.sunw, &u))
            {
                u.smap[0] = g_fxs.sun * day, g_fx.st_drawn_this = 1;
                if (day >= 0.25f)
                    g_sun_shown = g_serial;
            }
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
            int ok = g_fx.hist_serial && g_fx.hist_serial + 1 == g_serial && g_fx.hist[0] && g_fx.hist[0]->w == aw &&
                g_fx.hist[0]->h == ah && gfx_mat_inverse(vinv2, s->view);
            if (ok)
            {
                float dx = vinv2[12] - g_fx.prev_cam[0], dy = vinv2[13] - g_fx.prev_cam[1], dz = vinv2[14] - g_fx.prev_cam[2];
                ok = dx * dx + dy * dy + dz * dz < 25.0f;
            }
            if (ok)
            {
                gfx_mat_mul(m, vinv2, g_fx.prev_view);
                gfx_mat_mul(u.reproj, m, g_fx.prev_proj);
                u.hist[0] = 1.0f;
            }
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
        if (fx_tex(&g_fx.src, ct->vf, ct->w, ct->h, GFX_USE_RT, 1) && fx_tex(&g_fx.ao0, F16, aw, ah, GFX_USE_RT, 1) &&
            fx_tex(&g_fx.ao1, F16, aw, ah, GFX_USE_RT, 1) && fx_tex(&g_fx.b1a, F16, bw, bh, GFX_USE_RT, 1) &&
            fx_tex(&g_fx.b1b, F16, bw, bh, GFX_USE_RT, 1) && fx_tex(&g_fx.b2a, F16, (bw + 1) / 2, (bh + 1) / 2, GFX_USE_RT, 1) &&
            fx_tex(&g_fx.b2b, F16, (bw + 1) / 2, (bh + 1) / 2, GFX_USE_RT, 1) && fx_tex(&g_fx.ra, F16, bw, bh, GFX_USE_RT, 1) &&
            fx_tex(&g_fx.rb, F16, bw, bh, GFX_USE_RT, 1))
        {
            if (!g_fx.comp_pipe || g_fx.comp_fmt != ct->vf)
            {
                g_fx.comp_pipe = fx_pipeline("COMP", ct->vf);
                g_fx.comp_fmt = ct->vf;
            }
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
                    fx_pass(g_fx.ao0, 0, 0, g_fx.ao_pipe, NULL, ao_in, 4, sh, NULL);
                    fx_pass(g_fx.ao1, 0, 0, g_fx.blur_pipe, NULL, &g_fx.ao0->view, 1, NULL, across);
                    fx_pass(g_fx.ao0, 0, 0, g_fx.blur_pipe, NULL, &g_fx.ao1->view, 1, NULL, down);
                    if (g_fxs.temporal > 0.0f && fx_tex(&g_fx.hist[0], F16, aw, ah, GFX_USE_RT, 1) &&
                        fx_tex(&g_fx.hist[1], F16, aw, ah, GFX_USE_RT, 1))
                    {
                        int to = g_fx.hist_at ^ 1;
                        VkImageView t_in[2] = { g_fx.ao0->view, g_fx.hist[g_fx.hist_at]->view };
                        fx_pass(g_fx.hist[to], 0, 0, g_fx.temporal_pipe, NULL, t_in, 2, NULL, NULL);
                        ao_out = g_fx.hist[to];
                        g_fx.hist_at = to, g_fx.hist_serial = g_serial;
                    }
                }
                else
                    fx_uniforms(&u);
                if (u.bloom[1] > 0.0f)
                {
                    VkImageView b_in[2] = { g_fx.src->view, ao_out->view }; /* shaded as the composite shades */
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
                    fx_pass(g_fx.ra, 0, 0, g_fx.raymask_pipe, NULL, mask_in, 2, NULL, NULL);
                    fx_pass(g_fx.rb, 0, 0, g_fx.rays_pipe, NULL, &g_fx.ra->view, 1, NULL, NULL);
                    fx_pass(g_fx.ra, 0, 0, g_fx.gauss_pipe, NULL, &g_fx.rb->view, 1, NULL, across);
                    fx_pass(g_fx.rb, 0, 0, g_fx.gauss_pipe, NULL, &g_fx.ra->view, 1, NULL, down);
                }
                VkImageView comp_in[6] = { g_fx.src->view, ao_out->view, depth->view, g_fx.b1a->view, g_fx.b2a->view, g_fx.rb->view };
                float vp[4] = { vx, vy, vw, vh };
                fx_pass(ct, 0, 1, g_fx.comp_pipe, vp, comp_in, 6, NULL, NULL);
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
            blend ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, dyn, 2);
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

/* the back buffer onto the window's next image; 0 when there is none to draw to now */
static int present_to_window(GfxTex* bb)
{
    if (g_swap_stale || !g_swap)
        if (!swap_create())
            return 0;
    Frame* f = &g_frames[g_frame];
    uint32_t idx = 0;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    VkResult r = vkAcquireNextImageKHR(g_dev, g_swap, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &idx);
    if (gfx_profiling)
        g_prof.acquire_ns += gfx_now_ns() - t0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        g_swap_stale = 1;
        return 0;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return 0;
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
        VkDescriptorImageInfo ii = { g_present_samp, bb->view, VK_IMAGE_LAYOUT_GENERAL };
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
    VkPhysicalDeviceFeatures2 have2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    have2.pNext = &have12;
    vkGetPhysicalDeviceFeatures2(g_phys, &have2);
    g_has_bc = have.textureCompressionBC, g_has_aniso = have.samplerAnisotropy, g_has_lines = have.fillModeNonSolid;
    g_has_large_points = have.largePoints, g_has_mirror_once = have12.samplerMirrorClampToEdge;
    g_max_aniso = g_props.limits.maxSamplerAnisotropy;
    VkPhysicalDeviceFeatures en = { 0 };
    en.textureCompressionBC = have.textureCompressionBC;
    en.samplerAnisotropy = have.samplerAnisotropy;
    en.fillModeNonSolid = have.fillModeNonSolid;
    en.largePoints = have.largePoints;
    VkPhysicalDeviceVulkan13Features en13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    en13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features en12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    en12.pNext = &en13;
    en12.timelineSemaphore = VK_TRUE;
    en12.samplerMirrorClampToEdge = have12.samplerMirrorClampToEdge;
    VkPhysicalDeviceFeatures2 en2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    en2.pNext = &en12;
    en2.features = en;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = g_qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char* dext[2] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &en2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = g_surface ? 2 : 1;
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

    /* the draw functions' bindings (gfx_msl.h), pushed per draw */
    VkDescriptorSetLayoutBinding b[B_COUNT];
    uint32_t nb = 0;
    b[nb++] = (VkDescriptorSetLayoutBinding){ B_U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (uint32_t s = 0; s < GFX_NSTREAMS; ++s)
        b[nb++] = (VkDescriptorSetLayoutBinding){ B_STREAM0 + s, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    b[nb++] = (VkDescriptorSetLayoutBinding){ B_SHADOW, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
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
    VkSamplerCreateInfo ps = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ps.magFilter = ps.minFilter = VK_FILTER_LINEAR;
    ps.addressModeU = ps.addressModeV = ps.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.maxLod = 0.25f;
    VK_CHECK(vkCreateSampler(g_dev, &ps, NULL, &g_present_samp));

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
    fprintf(stderr, "[recomp] gfx: Vulkan %u.%u on %s%s\n", VK_API_VERSION_MAJOR(g_props.apiVersion),
        VK_API_VERSION_MINOR(g_props.apiVersion), g_props.deviceName, g_has_bc ? "" : " (no BC textures)");
    return 1;
}
