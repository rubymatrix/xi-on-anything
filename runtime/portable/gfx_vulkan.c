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
 * Not here yet (gfx_metal.m has them): the scene effects (gfx_scene_done), the sun's shadow map,
 * the water, the GPU timestamps. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
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
#include <vulkan/vulkan.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "vk_mem_alloc.h"
#pragma clang diagnostic pop

#include "cachedir.h"
#include "gfx.h"
#include "gfx_fx.h"
#include "gfx_msl.h"

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
    trash(2, (uint64_t)(uintptr_t)b->b, b->mem);
    free(b);
}

void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b)
        return;
    if (size > b->size)
        size = b->size;
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

static GfxTex* tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use, VkFormat force)
{
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    VkComponentMapping sw;
    t->vf = vk_format(fmt, use, &t->conv, &t->block, &t->texel, &sw);
    if (force)
        t->vf = force;
    t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->levels = levels ? levels : 1;
    t->has_stencil = use == GFX_USE_DEPTH && t->vf != VK_FORMAT_D16_UNORM;
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
    if (use == GFX_USE_RT && type == GFX_TEX_2D && t->levels == 1 && t->w >= 1024 && t->h >= 1024)
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
    return t;
}

GfxTex* gfx_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    return tex_create(type, fmt, w, h, levels, use, VK_FORMAT_UNDEFINED);
}

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (g_rt == t)
        end_pass(), g_rt = NULL;
    if (g_ds == t)
        end_pass(), g_ds = NULL;
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
        return upcmd();
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
        g_scratch_depth = tex_create(GFX_TEX_2D, g_ds->fmt, w, h, 1, GFX_USE_DEPTH, g_ds->vf);
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
    cb.attachmentCount = 1;
    cb.pAttachments = blend;
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = ndyn;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = 1;
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

static VkPipeline pipeline(const GfxDraw* d, GfxTex* depth)
{
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.lib.vs = d->vs, k.lib.fs = d->fs, k.pipe = d->pipe;
    k.lib.vs.water = 0, k.lib.fs.water = 0; /* the water is not here yet */
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

static void draw_encode(const GfxDraw* d)
{
    if (!begin_pass())
    {
        gfx_prof_skip(GFX_SKIP_NO_TARGET);
        return;
    }
    GfxTex* depth = depth_attachment();
    VkPipeline p = pipeline(d, depth);
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
    VkDescriptorBufferInfo bi[GFX_NSTREAMS + 1];
    VkDescriptorImageInfo ii[8];
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
        }
        else if (d->data[s] && d->size[s])
        {
            VkBuffer vb;
            VkDeviceSize voff;
            void* v = ring(d->size[s], RING_ALIGN, &vb, &voff);
            memcpy(v, d->data[s], d->size[s]);
            *b = (VkDescriptorBufferInfo){ vb, voff, (d->size[s] + 3) & ~3u };
        }
        else
            *b = (VkDescriptorBufferInfo){ g_dummy, 0, VK_WHOLE_SIZE };
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
    }
    p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, nw, w);

    uint32_t n = vertex_count(d->prim, d->count);
    VkBuffer buf;
    VkDeviceSize off;
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
    }
    else if (d->ibuf)
    {
        d->ibuf->used = g_serial;
        vkCmdBindIndexBuffer(cb, d->ibuf->b, d->ibuf_off, d->index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, n, 1, 0, 0, 0);
    }
    else if (d->indices)
    {
        void* idx = ring((size_t)n * d->index_size, 16, &buf, &off);
        memcpy(idx, d->indices, (size_t)n * d->index_size);
        vkCmdBindIndexBuffer(cb, buf, off, d->index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, n, 1, 0, 0, 0);
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

/* --- the scene effects: not on Vulkan yet ---------------------------------------------------------------------- */
static float g_focus[3];
static int g_has_focus;

void gfx_scene_done(GfxTex* color, const GfxScene* s) { (void)color, (void)s; }

void gfx_set_focus(const float* pos)
{
    g_has_focus = pos != NULL;
    if (pos)
        memcpy(g_focus, pos, sizeof g_focus);
}

void gfx_trace_dump(const char* path) { (void)path; }

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
    GfxTex* t = tex_create(type, F_A8R8G8B8, 1, 1, 1, GFX_USE_SAMPLE, VK_FORMAT_UNDEFINED);
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
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    p_push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(g_dev, "vkCmdPushDescriptorSetKHR");

    VmaAllocatorCreateInfo ai = { 0 };
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
