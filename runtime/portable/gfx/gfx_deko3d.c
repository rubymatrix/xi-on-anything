/* The graphics back end on deko3d, the Switch's own low-level API (devkitPro): gfx.h for the Nintendo Switch,
 * next to gfx_gl.c (OpenGL on Mesa's nouveau), chosen at build time (tools/build_switch.py, XI_SWITCH_GFX=deko3d).
 * Why: nouveau's CPU work per draw is the limit at stock clocks; deko3d records a draw for under a microsecond
 * (platforms/switch/tests/drawcost).
 *
 * Threads: as gfx_gl.c, the game is built with the render queue (GFX_QUEUE, gfx_queue.c), whose render thread
 * makes the device and makes every call (GFXQ_OWNED). Shaders are compiled on a thread of their own (uam,
 * prog_main). Log lines are handed to the game's thread (gfx_gl_prof_take, from host64.c and the sign-in
 * screen), which writes them: stdio from another thread breaks the main thread's.
 *
 * Coordinates: deko3d's defaults are D3D8's (depth 0..1, the origin at the top left, clip-space y up), so the
 * shaders (gfx_msl.c's deko3d dialect, gfx_dk_generate) keep D3D's clip space and nothing is flipped; row 0 of
 * every image is D3D's top row.
 *
 * Memory: images in heaps of 32 MB blocks (GPU-cached), buffers and the uploads' staging in CPU-written ones,
 * suballocated (heap_*); what is freed is kept until the GPU is done with the frame that freed it (the slot's
 * fence: garbage). One command buffer, recorded through the frame into the slot's command memory (more is added
 * when it runs out: cmd_more) and submitted at the present; each command list starts by invalidating the GPU's
 * caches, so whatever the CPU wrote before it was sent (uploads, descriptors, shader code, the ring) is seen.
 * Uploads are copies from the slot's staging, which when full sends the frame so far and waits (cmd_sync), as do
 * reads. Transfers and drawing are kept apart by a barrier where one follows the other (work_kind).
 *
 * Draws (as gfx_gl.c's): the vertex functions read their streams as buffer textures of words. Each block of the
 * buffer heap (and the ring) is one buffer texture, so a stream is that block's texture with the buffer's place
 * in it added to its registers' offsets (GfxU.offset); the frame's uniforms, the draws' own vertices and indices
 * go into the slot's ring. Textures and samplers have descriptors in two tables; a stage's texture is a handle of
 * both. Programs: one per key pair (LibKey), generated here and compiled by uam on its thread (or read from the
 * card, shader-cache/dk); a draw whose program isn't ready yet is skipped.
 *
 * Where it is (the plan's phase 2, its milestones):
 *  1. bring-up: the device, the queue, the swapchain on the default window and the present;
 *  2. resources: buffers, textures in the game's formats, targets, clears, copies, reads (waiting), the back
 *     buffer shown at the present;
 *  3. programs, state and draws (this);
 *  4. the world, the characters, reads without waiting, the per-pixel lighting variant, the CRT pass. */
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <deko3d.h>
#include <switch.h>
#include <SDL3/SDL.h>

#include "../gfx.h"
#include "../plat.h"
#include "dk_shaders.h" /* (tools/build_switch.py: the dk_shaders folder's .glsl files as strings) */
#include "dk_uam.h"
#include "gfx_fx.h"
#include "gfx_msl.h"
#include "nx_bridge.h"

#define ALIGN(x, a) (((x) + (a) - 1) & ~((uint32_t)(a) - 1))
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FB_NUM 3 /* three: with two, a swap interval of 0 still waited for the display */
#define FB_W 1920 /* the swapchain's images: the docked screen's size, cropped to the handheld's at each present */
#define FB_H 1080
#define CMD_SLOTS 2
#define CMD_SLICE (1u << 20)
#define CMD_MORE (512u << 10) /* command memory added when a frame's runs out */
#define STAGE_SLICE (8u << 20) /* a slot's staging for uploads */
#define RING_SLICE (16u << 20) /* a slot's ring: the frame's uniforms, vertices and indices */
#define N_IMG_DESC 16384
#define N_SAMP_DESC 1024
#define CODE_BLOCK (8u << 20)
#define U_SIZE ((uint32_t)ALIGN(sizeof(GfxU), 256))
#define UNIT0 8 /* the stages' textures are bindings 8-15 (the generated functions'); streams are 1-4 */

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
    F_D16 = 80,
};

/* --- the log: lines handed to the game's thread --------------------------------------------------------- */
enum
{
    LOG_LINES = 64,
    LOG_LEN = 512
};
static char g_log[LOG_LINES][LOG_LEN];
static uint32_t g_log_head, g_log_tail; /* written at head, taken at tail (under g_log_lock) */
static uint32_t g_log_lost;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void dk_log(const char* fmt, ...)
{
    char line[LOG_LEN];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    svcOutputDebugString(line, (u64)(n < (int)sizeof line ? n : (int)sizeof line - 1));
    pthread_mutex_lock(&g_log_lock);
    if (g_log_head - g_log_tail < LOG_LINES)
        memcpy(g_log[g_log_head++ % LOG_LINES], line, sizeof line);
    else
        ++g_log_lost;
    pthread_mutex_unlock(&g_log_lock);
}
#define LOG(...) dk_log(__VA_ARGS__)
/* a message at most n times */
#define LOG_N(n, ...)                                                                                                 \
    do                                                                                                                \
    {                                                                                                                 \
        static int said_;                                                                                             \
        if (said_++ < (n))                                                                                            \
            dk_log(__VA_ARGS__);                                                                                      \
    } while (0)

/* host64.c's switch_fps, once a frame on the game's thread: one line at a time (the name is gfx_gl.c's) */
int gfx_gl_prof_take(char* out, size_t n)
{
    int got = 0;
    pthread_mutex_lock(&g_log_lock);
    if (g_log_tail != g_log_head)
    {
        snprintf(out, n, "%s", g_log[g_log_tail++ % LOG_LINES]);
        got = 1;
    }
    else if (g_log_lost)
    {
        snprintf(out, n, "[dk] %u log lines lost\n", g_log_lost);
        g_log_lost = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g_log_lock);
    return got;
}

static int env_on(const char* name)
{
    const char* e = getenv(name);
    return e && e[0] && e[0] != '0';
}

/* --- the profile ---------------------------------------------------------------------------------------- */
int gfx_profiling;

uint64_t gfx_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static struct
{
    uint64_t since, front_ns, shim_ns, rt_ns[64], acquire_ns, submit_ns, sync_ns, draw_ns, prog_wait_ns;
    uint32_t frames, draws, drawn, clears, copies, skips, pending, targets, tex_made, buf_made, uploads, barriers,
        syncs, reads, async_reads, cmd_more, shader_binds, chars_items, chars_draws;
    uint64_t upload_bytes, ring_bytes, chars_ns;
} g_prof;

void gfx_prof_rt(int kind, uint64_t ns)
{
    if (kind >= 0 && kind < 64)
        g_prof.rt_ns[kind] += ns;
}
void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason)
{
    (void)reason;
    g_prof.skips++;
}
void gfx_prof_shim(uint64_t ns) { g_prof.shim_ns += ns; }

static uint32_t g_tex_live, g_buf_live;

/* --- the device ----------------------------------------------------------------------------------------- */
static DkDevice g_dev;
static DkQueue g_q;
static DkMemBlock g_fb_mem, g_cmd_mem, g_stage_mem, g_ring_mem, g_desc_mem;
static uint8_t *g_stage_cpu, *g_ring_cpu;
static DkGpuAddr g_stage_gpu, g_ring_gpu;
static DkImageDescriptor* g_img_desc; /* the tables (CPU-written; seen from the next command list) */
static DkSamplerDescriptor* g_samp_desc;
static DkGpuAddr g_img_desc_gpu, g_samp_desc_gpu;
static DkImage g_fb[FB_NUM];
static DkSwapchain g_sc;
static DkCmdBuf g_cb;
static DkFence g_fence[CMD_SLOTS];
static uint32_t g_slot; /* the frames presented */
static uint32_t g_cur;  /* the slot being recorded (g_slot's) */
static int g_open;      /* the command buffer has this frame's memory */
static uint32_t g_stage_at, g_ring_at;
static int g_up, g_vsync, g_interval = 1;
static volatile uint32_t g_failures;
static uint32_t g_dk_errors;
static int g_prect[4];

/* command memory added in a frame that ran out of its slice, kept for the slot's next frames */
static struct
{
    DkMemBlock* b;
    uint32_t n, used;
} g_more[CMD_SLOTS];

static void on_debug(void* user, const char* context, DkResult result, const char* message)
{
    (void)user;
    ++g_failures;
    if (++g_dk_errors <= 50)
        LOG("[dk] deko3d: %s: result %d: %s\n", context ? context : "?", (int)result, message ? message : "");
}

static DkMemBlock block(uint32_t size, uint32_t flags)
{
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, g_dev, ALIGN(size, DK_MEMBLOCK_ALIGNMENT));
    m.flags = flags;
    return dkMemBlockCreate(&m);
}

/* --- descriptor slots: given out, given back once the GPU is past the frame that let them go -------------- */
typedef struct
{
    uint32_t* free;
    uint32_t nfree, total;
} Slots;

static Slots g_img_slots, g_samp_slots;

static void slots_init(Slots* s, uint32_t n, uint32_t first)
{
    s->free = (uint32_t*)malloc(n * sizeof(uint32_t));
    s->total = n;
    s->nfree = 0;
    for (uint32_t i = n; i-- > first;)
        s->free[s->nfree++] = i;
}

static int slot_take(Slots* s)
{
    if (!s->nfree)
    {
        LOG_N(5, "[dk] gfx: out of descriptors (%u)\n", s->total);
        return -1;
    }
    return (int)s->free[--s->nfree];
}

/* a buffer texture of 32-bit words over memory (a heap block, the ring) in descriptor slot `slot` */
static void buffer_descriptor(int slot, DkMemBlock mem, uint32_t off, uint32_t size)
{
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, g_dev);
    m.type = DkImageType_Buffer;
    m.format = DkImageFormat_R32_Uint;
    m.dimensions[0] = size / 4;
    DkImageLayout l;
    dkImageLayoutInitialize(&l, &m);
    DkImage img;
    dkImageInitialize(&img, &l, mem, off);
    DkImageView v;
    dkImageViewDefaults(&v, &img);
    dkImageDescriptorInitialize(&g_img_desc[slot], &v, false, false);
}

/* --- memory: heaps of blocks, suballocated -------------------------------------------------------------- */
typedef struct
{
    uint32_t off, size;
} Span;

typedef struct
{
    DkMemBlock mem;
    uint32_t size;
    int desc; /* the buffer heap's: its buffer texture (-1 none) */
    Span* free; /* sorted by offset, neighbours joined */
    uint32_t nfree, cap;
} Chunk;

typedef struct
{
    const char* name;
    uint32_t flags, chunk_size;
    int buffers; /* its blocks are read as buffer textures (each has a descriptor) */
    Chunk* c;
    uint32_t n;
    uint64_t used, own; /* bytes handed out from chunks; in blocks of their own */
} Heap;

/* a piece of a heap: a chunk's (chunk >= 0) or a block of its own (chunk -1, for what is over half a chunk) */
typedef struct
{
    Heap* heap;
    DkMemBlock mem;
    uint32_t off, size;
    int32_t chunk;
    int desc; /* the buffer texture over its block (the buffer heap's) */
} Mem;

static Heap g_img_heap = { "images", DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 32u << 20, 0, NULL, 0, 0, 0 };
static Heap g_data_heap = { "buffers", DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 16u << 20, 1, NULL, 0, 0, 0 };

static void span_insert(Chunk* c, uint32_t at, Span s)
{
    if (c->nfree == c->cap)
    {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->free = (Span*)realloc(c->free, c->cap * sizeof(Span));
    }
    memmove(&c->free[at + 1], &c->free[at], (c->nfree - at) * sizeof(Span));
    c->free[at] = s;
    c->nfree++;
}

static void span_remove(Chunk* c, uint32_t at)
{
    memmove(&c->free[at], &c->free[at + 1], (c->nfree - at - 1) * sizeof(Span));
    c->nfree--;
}

static int chunk_take(Chunk* c, uint32_t size, uint32_t align, uint32_t* off)
{
    for (uint32_t i = 0; i < c->nfree; ++i)
    {
        Span* f = &c->free[i];
        uint32_t a = ALIGN(f->off, align), end = f->off + f->size;
        if (a < f->off || a + size > end || a + size < a)
            continue;
        uint32_t pre = a - f->off, post = end - (a + size);
        if (pre && post)
            f->size = pre, span_insert(c, i + 1, (Span){ a + size, post });
        else if (pre)
            f->size = pre;
        else if (post)
            f->off = a + size, f->size = post;
        else
            span_remove(c, i);
        *off = a;
        return 1;
    }
    return 0;
}

static void chunk_give(Chunk* c, uint32_t off, uint32_t size)
{
    uint32_t i = 0;
    while (i < c->nfree && c->free[i].off < off)
        ++i;
    span_insert(c, i, (Span){ off, size });
    if (i + 1 < c->nfree && c->free[i].off + c->free[i].size == c->free[i + 1].off) /* the next */
        c->free[i].size += c->free[i + 1].size, span_remove(c, i + 1);
    if (i > 0 && c->free[i - 1].off + c->free[i - 1].size == c->free[i].off) /* the previous */
        c->free[i - 1].size += c->free[i].size, span_remove(c, i);
}

static int heap_alloc(Heap* h, uint32_t size, uint32_t align, Mem* m)
{
    memset(m, 0, sizeof *m);
    m->heap = h, m->size = size, m->desc = -1;
    if (align < 256)
        align = 256;
    if (size > h->chunk_size / 2)
    {
        m->mem = block(size, h->flags), m->chunk = -1, m->off = 0;
        if (!m->mem)
            return 0;
        if (h->buffers && (m->desc = slot_take(&g_img_slots)) >= 0)
            buffer_descriptor(m->desc, m->mem, 0, ALIGN(size, 4));
        h->own += size;
        return 1;
    }
    for (uint32_t i = 0; i < h->n; ++i)
        if (chunk_take(&h->c[i], size, align, &m->off))
        {
            m->mem = h->c[i].mem, m->chunk = (int32_t)i, m->desc = h->c[i].desc;
            h->used += size;
            return 1;
        }
    Chunk c = { 0 };
    c.mem = block(h->chunk_size, h->flags);
    c.desc = -1;
    if (!c.mem)
    {
        LOG_N(10, "[dk] gfx: no memory for another %u MB of %s (%u blocks so far)\n", h->chunk_size >> 20, h->name, h->n);
        return 0;
    }
    c.size = h->chunk_size;
    if (h->buffers && (c.desc = slot_take(&g_img_slots)) >= 0)
        buffer_descriptor(c.desc, c.mem, 0, c.size);
    span_insert(&c, 0, (Span){ 0, c.size });
    h->c = (Chunk*)realloc(h->c, (h->n + 1) * sizeof(Chunk));
    h->c[h->n] = c;
    chunk_take(&h->c[h->n], size, align, &m->off);
    m->mem = c.mem, m->chunk = (int32_t)h->n++, m->desc = c.desc;
    h->used += size;
    return 1;
}

static void heap_free_now(Mem* m)
{
    if (!m->mem)
        return;
    if (m->chunk < 0)
    {
        dkMemBlockDestroy(m->mem), m->heap->own -= m->size;
        if (m->desc >= 0)
            g_img_slots.free[g_img_slots.nfree++] = (uint32_t)m->desc;
    }
    else
        chunk_give(&m->heap->c[m->chunk], m->off, m->size), m->heap->used -= m->size;
    m->mem = NULL;
}

/* what was freed in a frame: given back once the GPU is past it (its slot's fence) */
static struct
{
    Mem* m;
    uint32_t n, cap;
    uint32_t* desc; /* image descriptor slots */
    uint32_t nd, capd;
} g_garbage[CMD_SLOTS];

static void heap_free(Mem* m)
{
    if (!m->mem)
        return;
    if (!g_up)
    {
        heap_free_now(m);
        return;
    }
    __typeof__(g_garbage[0])* g = &g_garbage[g_cur];
    if (g->n == g->cap)
        g->cap = g->cap ? g->cap * 2 : 64, g->m = (Mem*)realloc(g->m, g->cap * sizeof(Mem));
    g->m[g->n++] = *m;
    m->mem = NULL;
}

static void desc_free(int slot)
{
    if (slot < 0)
        return;
    __typeof__(g_garbage[0])* g = &g_garbage[g_cur];
    if (g->nd == g->capd)
        g->capd = g->capd ? g->capd * 2 : 64, g->desc = (uint32_t*)realloc(g->desc, g->capd * sizeof(uint32_t));
    g->desc[g->nd++] = (uint32_t)slot;
}

static void garbage_release(uint32_t s)
{
    for (uint32_t i = 0; i < g_garbage[s].n; ++i)
        heap_free_now(&g_garbage[s].m[i]);
    g_garbage[s].n = 0;
    for (uint32_t i = 0; i < g_garbage[s].nd; ++i)
        g_img_slots.free[g_img_slots.nfree++] = g_garbage[s].desc[i];
    g_garbage[s].nd = 0;
}

static uint8_t* mem_cpu(const Mem* m) { return (uint8_t*)dkMemBlockGetCpuAddr(m->mem) + m->off; }
static DkGpuAddr mem_gpu(const Mem* m) { return dkMemBlockGetGpuAddr(m->mem) + m->off; }

/* --- the command buffer --------------------------------------------------------------------------------- */
static void cmd_more(void* user, DkCmdBuf cb, size_t need)
{
    (void)user;
    __typeof__(g_more[0])* m = &g_more[g_cur];
    uint32_t size = need > CMD_MORE ? ALIGN((uint32_t)need, DK_MEMBLOCK_ALIGNMENT) : CMD_MORE;
    DkMemBlock b = NULL;
    if (size == CMD_MORE && m->used < m->n)
        b = m->b[m->used++];
    else
    {
        b = block(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        if (size == CMD_MORE) /* (kept for the slot's next frames; a larger one is not) */
        {
            m->b = (DkMemBlock*)realloc(m->b, (m->n + 1) * sizeof(DkMemBlock));
            m->b[m->n++] = b, m->used = m->n;
        }
        else
            LOG_N(5, "[dk] gfx: %u KB of command memory asked for at once\n", size >> 10);
    }
    g_prof.cmd_more++;
    dkCmdBufAddMemory(cb, b, 0, size);
}

/* what the commands since the last barrier were: drawing (clears, draws) or transfers (uploads, copies, blits) */
enum
{
    WK_NONE,
    WK_DRAW,
    WK_XFER
};
static int g_work_kind;
static int g_bound; /* the targets are bound (bind_targets) */

static void barrier(void)
{
    dkCmdBufBarrier(g_cb, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    g_prof.barriers++;
}

static void work_kind(int k)
{
    if (g_work_kind != k && g_work_kind != WK_NONE)
        barrier();
    g_work_kind = k;
}

/* the draws' state as last set in this command list (st_*: set again only when it changes) */
static struct
{
    int valid;
    DkRasterizerState rs;
    DkColorState cs;
    DkBlendState bs;
    DkColorWriteState cws;
    DkDepthStencilState ds;
    uint32_t stencil[3];
    float bias[2];
    DkViewport vp;
    DkScissor sc;
    const void* prog;
    DkGpuAddr ubo;
    DkResHandle vtex[4], ftex[8];
    DkGpuAddr ib;
    int ib_fmt;
} S;

/* deko3d's memory on the CPU. A command buffer's control memory is a chain of 1 KB chunks (a frame's ~100 full
 * barriers take four), and dkCmdBufClear keeps only the newest of the chain, its link overwritten: the others leaked,
 * 3 KB a frame, the heap gone in an hour and a half. The chunks a list took are noted here and, at its clear, all
 * but the newest (deko3d's own free one until the next list, whose first chunk it is) are kept for later ones. */
#define CTL_CHUNK 1024u
static void* g_ctl_new[1024]; /* taken since the last clear, in order */
static uint32_t g_ctl_nnew;
static void* g_ctl_last;      /* the newest of them (g_ctl_new can be full) */
static void* g_ctl_held;      /* deko3d's free one */
static void** g_ctl_pool;
static uint32_t g_ctl_npool, g_ctl_cap;
static int g_ctl_on; /* a list is being recorded */

static DkResult dk_alloc(void* user, size_t alignment, size_t size, void** out)
{
    (void)user;
    int chunk = g_ctl_on && size == CTL_CHUNK && alignment <= 16;
    *out = chunk && g_ctl_npool ? g_ctl_pool[--g_ctl_npool] : memalign(alignment < 16 ? 16 : alignment, size);
    if (!*out)
        return DkResult_OutOfMemory;
    if (chunk)
    {
        g_ctl_last = *out;
        if (g_ctl_nnew < sizeof g_ctl_new / sizeof *g_ctl_new)
            g_ctl_new[g_ctl_nnew++] = *out;
    }
    return DkResult_Success;
}

static void dk_free(void* user, void* mem)
{
    (void)user;
    free(mem);
}

static void ctl_keep(void* p)
{
    if (g_ctl_npool == g_ctl_cap)
    {
        uint32_t cap = g_ctl_cap ? g_ctl_cap * 2 : 64;
        void** m = (void**)realloc(g_ctl_pool, cap * sizeof *m);
        if (!m)
            return;
        g_ctl_pool = m, g_ctl_cap = cap;
    }
    g_ctl_pool[g_ctl_npool++] = p;
}

/* before dkCmdBufClear, the list recorded so far submitted (deko3d has read its control memory) */
static void ctl_reclaim(void)
{
    if (g_ctl_nnew)
    {
        if (g_ctl_held)
            ctl_keep(g_ctl_held);
        for (uint32_t i = 0; i < g_ctl_nnew; ++i)
            if (g_ctl_new[i] != g_ctl_last)
                ctl_keep(g_ctl_new[i]);
        g_ctl_held = g_ctl_last;
        g_ctl_nnew = 0;
    }
    g_ctl_on = 1;
}

static void cmd_begin_memory(void)
{
    ctl_reclaim();
    dkCmdBufClear(g_cb);
    dkCmdBufAddMemory(g_cb, g_cmd_mem, g_cur * CMD_SLICE, CMD_SLICE);
    g_more[g_cur].used = 0;
    g_stage_at = 0, g_ring_at = 0;
    g_bound = 0;
    S.valid = 0;
    /* what the CPU wrote since the last list (uploads' sources, descriptors, shader code, the ring, buffers):
     * the caches that may hold the old contents of that memory forgotten */
    dkCmdBufBarrier(g_cb, DkBarrier_None,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors | DkInvalidateFlags_L2Cache);
    dkCmdBufBindImageDescriptorSet(g_cb, g_img_desc_gpu, N_IMG_DESC);
    dkCmdBufBindSamplerDescriptorSet(g_cb, g_samp_desc_gpu, N_SAMP_DESC);
    dkCmdBufBindVtxAttribState(g_cb, NULL, 0);
    dkCmdBufBindVtxBufferState(g_cb, NULL, 0);
    g_work_kind = WK_DRAW; /* (the frame before's drawing may still be running: a transfer first waits for it) */
}

/* the frame's commands: its slot's memory, once the GPU is done with that slot's frame before */
static void cmd_open(void)
{
    if (g_open)
        return;
    g_cur = g_slot % CMD_SLOTS;
    dkFenceWait(&g_fence[g_cur], -1);
    garbage_release(g_cur);
    cmd_begin_memory();
    g_open = 1;
}

/* what is recorded so far sent, and the GPU waited for (staging or the ring full, a read): then all is free */
static uint32_t g_syncs; /* how many: what was recorded before one is done after it */

static void cmd_sync(void)
{
    uint64_t t0 = gfx_now_ns();
    cmd_open();
    dkQueueSubmitCommands(g_q, dkCmdBufFinishList(g_cb));
    dkQueueWaitIdle(g_q);
    ++g_syncs;
    for (uint32_t s = 0; s < CMD_SLOTS; ++s)
        garbage_release(s);
    cmd_begin_memory();
    g_work_kind = WK_NONE;
    g_prof.syncs++;
    g_prof.sync_ns += gfx_now_ns() - t0;
}

/* a place in the slot's staging for an upload (at most STAGE_SLICE) */
static uint32_t stage(uint32_t size)
{
    cmd_open();
    uint32_t at = ALIGN(g_stage_at, 256);
    if (at + size > STAGE_SLICE)
        cmd_sync(), at = 0;
    g_stage_at = at + size;
    return g_cur * STAGE_SLICE + at;
}

/* --- the ring: room reserved for a draw at once (ring_reserve), then its pieces (ring_put) ------------------ */
static int ring_reserve(uint32_t n)
{
    if (n > RING_SLICE)
        return 0;
    if (ALIGN(g_ring_at, 256) + n > RING_SLICE)
        cmd_sync();
    return 1;
}

/* n bytes at an alignment in the reserved room: their offset in the ring's block; written when p */
static uint32_t ring_put(const void* p, uint32_t n, uint32_t align)
{
    uint32_t at = ALIGN(g_ring_at, align);
    g_ring_at = at + ALIGN(n, 4);
    at += g_cur * RING_SLICE;
    if (p)
        memcpy(g_ring_cpu + at, p, n);
    g_prof.ring_bytes += n;
    return at;
}

static int g_ring_desc;

/* --- textures ------------------------------------------------------------------------------------------- */
struct GfxTex
{
    DkImage img;
    DkImageLayout layout;
    Mem mem;
    DkImageFormat dkfmt;
    uint8_t sw[4]; /* the view's swizzle: D3D's channels from the format's */
    int desc;      /* its image descriptor (sampled) */
    int type, use, has_stencil, dropped;
    uint32_t fmt, w, h, levels, layers, block, bpp, id;
    struct AsyncRead* rd; /* gfx_tex_read_async's copies */
};

static uint32_t g_tex_ids;
static GfxTex* g_rt;
static GfxTex* g_ds;
static uint32_t g_rt_face, g_rt_level;
static GfxTex *g_dummy2d, *g_dummycube;

static uint32_t level_dim(uint32_t d, uint32_t level)
{
    d >>= level;
    return d ? d : 1;
}

static void swz(GfxTex* t, uint8_t r, uint8_t g, uint8_t b, uint8_t a) { t->sw[0] = r, t->sw[1] = g, t->sw[2] = b, t->sw[3] = a; }

/* deko3d's format for a D3DFORMAT, and the swizzle that makes the channels D3D's. Its packed formats are named
 * from their low bits up (format_traits.inc: RGB565 is red in the low bits), D3D's from the high bits down, so
 * D3D's R5G6B5 is BGR565, A1R5G5B5 BGR5A1 and A8R8G8B8 (bytes B, G, R, A) BGRA8; A4R4G4B4 has no such format
 * here: RGBA4 with red and blue swapped by the view (a view's swizzle picks from the format's channels). */
static DkImageFormat tex_format(GfxTex* t)
{
    swz(t, DkImageSwizzle_Red, DkImageSwizzle_Green, DkImageSwizzle_Blue, DkImageSwizzle_Alpha);
    t->bpp = 4, t->block = 0;
    if (t->use == GFX_USE_DEPTH)
    {
        if (t->fmt == F_D16)
            return t->bpp = 2, DkImageFormat_Z16;
        t->has_stencil = 1;
        return DkImageFormat_Z24S8;
    }
    switch (t->fmt)
    {
    case F_A8R8G8B8: return DkImageFormat_BGRA8_Unorm;
    case F_X8R8G8B8: return DkImageFormat_BGRX8_Unorm;
    case F_R5G6B5: t->bpp = 2; return DkImageFormat_BGR565_Unorm;
    case F_X1R5G5B5: t->bpp = 2; return DkImageFormat_BGR5_Unorm;
    case F_A1R5G5B5: t->bpp = 2; return DkImageFormat_BGR5A1_Unorm;
    case F_A4R4G4B4:
        t->bpp = 2;
        swz(t, DkImageSwizzle_Blue, DkImageSwizzle_Green, DkImageSwizzle_Red, DkImageSwizzle_Alpha);
        return DkImageFormat_RGBA4_Unorm;
    case F_A8:
        t->bpp = 1;
        swz(t, DkImageSwizzle_Zero, DkImageSwizzle_Zero, DkImageSwizzle_Zero, DkImageSwizzle_Red);
        return DkImageFormat_R8_Unorm;
    case F_L8:
        t->bpp = 1;
        swz(t, DkImageSwizzle_Red, DkImageSwizzle_Red, DkImageSwizzle_Red, DkImageSwizzle_One);
        return DkImageFormat_R8_Unorm;
    case F_A8L8:
        t->bpp = 2;
        swz(t, DkImageSwizzle_Red, DkImageSwizzle_Red, DkImageSwizzle_Red, DkImageSwizzle_Green);
        return DkImageFormat_RG8_Unorm;
    case F_V8U8: t->bpp = 2; return DkImageFormat_RG8_Snorm;
    }
    if (t->fmt == FOURCC('D', 'X', 'T', '1'))
        return t->block = 8, DkImageFormat_RGBA_BC1;
    if (t->fmt == FOURCC('D', 'X', 'T', '2') || t->fmt == FOURCC('D', 'X', 'T', '3'))
        return t->block = 16, DkImageFormat_RGBA_BC2;
    if (t->fmt == FOURCC('D', 'X', 'T', '4') || t->fmt == FOURCC('D', 'X', 'T', '5'))
        return t->block = 16, DkImageFormat_RGBA_BC3;
    LOG_N(10, "[dk] gfx: texture format %u not known; made as A8R8G8B8\n", t->fmt);
    return DkImageFormat_BGRA8_Unorm;
}

/* a view of one level of one face (a cube's faces are its layers) */
static void tex_view(DkImageView* v, GfxTex* t, uint32_t face, uint32_t level)
{
    dkImageViewDefaults(v, &t->img);
    v->mipLevelOffset = (uint8_t)level;
    v->mipLevelCount = 1;
    if (t->layers > 1)
        v->layerOffset = (uint16_t)face, v->layerCount = 1;
}

GfxTex* gfx_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_up || !w || !h)
        return NULL;
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->type = type, t->use = use, t->fmt = d3dfmt, t->w = w, t->h = h, t->id = ++g_tex_ids, t->desc = -1;
    t->dkfmt = tex_format(t);
    uint32_t maxl = 1;
    while ((w >> maxl) || (h >> maxl))
        maxl++;
    t->levels = levels && levels < maxl ? levels : maxl;
    if (use != GFX_USE_SAMPLE)
        t->levels = 1;
    t->layers = type == GFX_TEX_CUBE ? 6 : 1;
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, g_dev);
    m.type = type == GFX_TEX_CUBE ? DkImageType_Cubemap : DkImageType_2D;
    m.format = t->dkfmt;
    m.dimensions[0] = w, m.dimensions[1] = h;
    m.mipLevels = t->levels;
    if (use == GFX_USE_RT)
        m.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    else if (use == GFX_USE_DEPTH)
        m.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
    /* the tile height deko3d would pick, already shrunk to what fits the first level: deko3d picks it from 1.5
     * times the height, and the sampler shrinks it to the level's height (as calcLevelOffset does), but the copy
     * engine shrinks it only for the levels after the first. A texture 6-8 rows (of blocks) tall got two-GOB tiles
     * for the copies and one-GOB tiles for the sampler: the uploads put a 32x32 DXT3's right half where the
     * sampler doesn't read it (the compass), into the next texture's memory. */
    {
        uint32_t rows = t->block ? (h + 3) / 4 : h;
        uint32_t gobs = (rows + rows / 2 + 7) / 8;
        uint32_t shift = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
        while (shift && (8u << (shift - 1)) >= rows)
            --shift;
        m.flags |= DkImageFlags_CustomTileSize;
        m.tileSize = (DkTileSize)shift;
    }
    dkImageLayoutInitialize(&t->layout, &m);
    if (!heap_alloc(&g_img_heap, (uint32_t)dkImageLayoutGetSize(&t->layout), dkImageLayoutGetAlignment(&t->layout), &t->mem))
    {
        free(t);
        return NULL;
    }
    dkImageInitialize(&t->img, &t->layout, t->mem.mem, t->mem.off);
    if (use != GFX_USE_DEPTH && (t->desc = slot_take(&g_img_slots)) >= 0)
    {
        DkImageView v;
        dkImageViewDefaults(&v, &t->img);
        for (int c = 0; c < 4; ++c) /* (DkImageSwizzle is an enum: one at a time) */
            v.swizzle[c] = (DkImageSwizzle)t->sw[c];
        dkImageDescriptorInitialize(&g_img_desc[t->desc], &v, false, false);
    }
    ++g_tex_live, ++g_prof.tex_made;
    return t;
}

static void async_free(GfxTex* t);

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (g_rt == t)
        g_rt = NULL, g_bound = 0;
    if (g_ds == t)
        g_ds = NULL, g_bound = 0;
    async_free(t);
    heap_free(&t->mem);
    desc_free(t->desc);
    --g_tex_live;
    free(t);
}

/* the rows of a rectangle (blocks' rows for a compressed format) and the bytes in one */
static uint32_t rect_rows(const GfxTex* t, uint32_t w, uint32_t h, uint32_t* row)
{
    *row = t->block ? (w + 3) / 4 * t->block : w * t->bpp;
    return t->block ? (h + 3) / 4 : h;
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const void* src, uint32_t pitch)
{
    if (!g_up || !t || level >= t->levels || face >= t->layers || t->dropped || t->use == GFX_USE_DEPTH || !w || !h)
        return;
    uint32_t lw = level_dim(t->w, level), lh = level_dim(t->h, level);
    if (x >= lw || y >= lh)
        return;
    if (x + w > lw)
        w = lw - x;
    if (y + h > lh)
        h = lh - y;
    uint32_t row, rows = rect_rows(t, w, h, &row);
    uint32_t unit = t->block ? 4 : 1; /* (rows of texels a row of the source is) */
    uint32_t band = STAGE_SLICE / row; /* rows a staging copy takes at most */
    if (!band)
        return;
    DkImageView v;
    tex_view(&v, t, face, level);
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t r0 = 0; r0 < rows; r0 += band)
    {
        uint32_t n = rows - r0 < band ? rows - r0 : band;
        uint32_t at = stage(n * row);
        uint8_t* d = g_stage_cpu + at;
        if (pitch == row)
            memcpy(d, s + (size_t)r0 * pitch, (size_t)n * row);
        else
            for (uint32_t r = 0; r < n; ++r)
                memcpy(d + (size_t)r * row, s + (size_t)(r0 + r) * pitch, row);
        work_kind(WK_XFER);
        uint32_t ty = y + r0 * unit, th = n * unit;
        if (ty + th > y + h)
            th = y + h - ty;
        DkCopyBuf cb = { g_stage_gpu + at, row, 0 };
        DkImageRect rect = { x, ty, 0, w, th, 1 };
        dkCmdBufCopyBufferToImage(g_cb, &cb, &v, &rect, 0);
    }
    ++g_prof.uploads, g_prof.upload_bytes += (uint64_t)row * rows;
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (t)
        gfx_tex_upload_rect(t, face, level, 0, 0, level_dim(t->w, level), level_dim(t->h, level), src, pitch);
}

/* a level's texels, tightly packed, then into D3D's pitch: the GPU waited for (cmd_sync) */
void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!g_up || !t || !dst || t->use == GFX_USE_DEPTH || t->dropped || level >= t->levels || face >= t->layers)
        return;
    uint32_t lw = level_dim(t->w, level), lh = level_dim(t->h, level);
    uint32_t row, rows = rect_rows(t, lw, lh, &row);
    Mem m;
    if (!heap_alloc(&g_data_heap, row * rows, 256, &m))
        return;
    cmd_open();
    work_kind(WK_XFER);
    DkImageView v;
    tex_view(&v, t, face, level);
    DkImageRect rect = { 0, 0, 0, lw, lh, 1 };
    DkCopyBuf cb = { mem_gpu(&m), row, 0 };
    dkCmdBufCopyImageToBuffer(g_cb, &v, &rect, &cb, 0);
    cmd_sync();
    const uint8_t* s = mem_cpu(&m);
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t y = 0; y < rows; ++y)
        memcpy(d + (size_t)y * pitch, s + (size_t)y * row, row < pitch ? row : pitch);
    heap_free_now(&m); /* (the GPU is done with it) */
    g_prof.reads++;
}

/* The newest copy of the level the GPU has finished, and a new one recorded with this frame's work (three in flight
 * at most); only the first read of a level waits. A copy is done once its frame's slot comes round again (cmd_open
 * has waited for that frame's fence) or the GPU was waited for since (cmd_sync): no fence of its own. FFXI's
 * occlusion probes: 16x16, the sun's read about every frame. */
typedef struct AsyncRead
{
    uint32_t face, level, size;
    uint8_t* last; /* the newest copy taken, its rows tight */
    struct
    {
        Mem mem;
        uint32_t frame, sync, pending;
    } slot[3];
} AsyncRead;

static void async_free(GfxTex* t)
{
    AsyncRead* r = t->rd;
    if (!r)
        return;
    for (int i = 0; i < 3; ++i)
        heap_free(&r->slot[i].mem);
    free(r->last);
    free(r);
    t->rd = NULL;
}

void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!g_up || !t || !dst || t->use == GFX_USE_DEPTH || t->dropped || level >= t->levels || face >= t->layers)
        return;
    uint32_t lw = level_dim(t->w, level), lh = level_dim(t->h, level);
    uint32_t row, rows = rect_rows(t, lw, lh, &row), size = row * rows;
    AsyncRead* r = t->rd;
    if (r && (r->face != face || r->level != level))
        async_free(t), r = NULL;
    if (!r)
    {
        r = (AsyncRead*)calloc(1, sizeof *r);
        uint8_t* last = (uint8_t*)calloc(1, size);
        if (!r || !last)
        {
            free(r), free(last);
            return;
        }
        r->face = face, r->level = level, r->size = size, r->last = last;
        t->rd = r;
        gfx_tex_read(t, face, level, r->last, row); /* the first: now */
    }
    cmd_open(); /* (this slot's frame before is done from here) */
    int best = -1, idle = -1;
    for (int i = 0; i < 3; ++i)
    {
        if (!r->slot[i].pending)
            continue;
        if (r->slot[i].sync == g_syncs && g_slot - r->slot[i].frame < CMD_SLOTS)
            continue;
        if (best < 0 || (int32_t)(r->slot[i].frame - r->slot[best].frame) >= 0)
            best = i;
    }
    if (best >= 0)
    {
        memcpy(r->last, mem_cpu(&r->slot[best].mem), size);
        for (int i = 0; i < 3; ++i) /* (it, and any older one it overtook) */
            if (r->slot[i].pending && (int32_t)(r->slot[best].frame - r->slot[i].frame) >= 0)
                r->slot[i].pending = 0;
    }
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t y = 0; y < rows; ++y)
        memcpy(d + (size_t)y * pitch, r->last + (size_t)y * row, row < pitch ? row : pitch);
    for (int i = 0; i < 3 && idle < 0; ++i)
        if (!r->slot[i].pending)
            idle = i;
    if (idle < 0 || (!r->slot[idle].mem.mem && !heap_alloc(&g_data_heap, size, 256, &r->slot[idle].mem)))
        return;
    work_kind(WK_XFER);
    DkImageView v;
    tex_view(&v, t, face, level);
    DkImageRect rect = { 0, 0, 0, lw, lh, 1 };
    DkCopyBuf cb = { mem_gpu(&r->slot[idle].mem), row, 0 };
    dkCmdBufCopyImageToBuffer(g_cb, &v, &rect, &cb, 0);
    r->slot[idle].frame = g_slot, r->slot[idle].sync = g_syncs, r->slot[idle].pending = 1;
    g_prof.async_reads++;
}

/* --- buffers: in CPU-written memory; an upload goes into fresh memory (the frames in flight keep the old) -- */
struct GfxBuf
{
    Mem mem;
    uint32_t size; /* as made: a multiple of 4 */
};

GfxBuf* gfx_buf_create(uint32_t size)
{
    if (!g_up || !size)
        return NULL;
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->size = (size + 3) & ~3u;
    if (!heap_alloc(&g_data_heap, b->size, 256, &b->mem))
    {
        free(b);
        return NULL;
    }
    ++g_buf_live, ++g_prof.buf_made;
    return b;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    heap_free(&b->mem); /* (a block of its own gives its buffer texture back with it) */
    --g_buf_live;
    free(b);
}

void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b || !size)
        return;
    if (size > b->size)
        size = b->size;
    Mem fresh;
    if (!heap_alloc(&g_data_heap, b->size, 256, &fresh))
        return;
    memcpy(mem_cpu(&fresh), data, size);
    if (size < b->size) /* the rest as it was (a read of uncached memory: rare) */
        memcpy(mem_cpu(&fresh) + size, mem_cpu(&b->mem) + size, b->size - size);
    heap_free(&b->mem);
    b->mem = fresh;
    ++g_prof.uploads, g_prof.upload_bytes += size;
}

/* --- targets, clears, copies ---------------------------------------------------------------------------- */
void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (color != g_rt || face != g_rt_face || level != g_rt_level || depth != g_ds)
        g_bound = 0, ++g_prof.targets;
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
}

static void color_size(uint32_t* w, uint32_t* h)
{
    *w = g_rt ? level_dim(g_rt->w, g_rt_level) : 1;
    *h = g_rt ? level_dim(g_rt->h, g_rt_level) : 1;
}

/* the depth surface the draws get: the game's when it covers the target (as gfx_gl.c) */
static GfxTex* depth_attachment(void)
{
    uint32_t w, h;
    color_size(&w, &h);
    if (!g_ds || g_ds->w < w || g_ds->h < h)
        return NULL;
    return g_ds;
}

/* the current targets bound to draw to; 0 when there is no color target */
static int bind_targets(void)
{
    if (!g_rt)
        return 0;
    cmd_open();
    work_kind(WK_DRAW);
    if (!g_bound)
    {
        DkImageView c, d;
        tex_view(&c, g_rt, g_rt_face, g_rt_level);
        GfxTex* ds = depth_attachment();
        if (ds)
            tex_view(&d, ds, 0, 0);
        barrier(); /* (what was drawn to the targets before may be sampled now) */
        dkCmdBufBindRenderTarget(g_cb, &c, ds ? &d : NULL);
        g_bound = 1;
        S.sc.width = 0; /* (the scissor, set again) */
    }
    return 1;
}

/* each rectangle within the viewport, scissored (deko3d's clears keep to the scissor) */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
               const uint32_t vp[6])
{
    if (!g_up || !bind_targets())
        return;
    GfxTex* depth = depth_attachment();
    if (!depth)
        flags &= 1;
    else if (!depth->has_stencil)
        flags &= 3;
    if (!flags)
        return;
    uint32_t w, h;
    color_size(&w, &h);
    int32_t vx0 = (int32_t)vp[0], vy0 = (int32_t)vp[1], vx1 = (int32_t)(vp[0] + vp[2]), vy1 = (int32_t)(vp[1] + vp[3]);
    int32_t full[4] = { 0, 0, (int32_t)w, (int32_t)h };
    uint32_t nr = nrects ? nrects : 1;
    /* (the clears write what the masks let through: full masks for them, set back for the draws after) */
    DkColorWriteState cws = { 0xF };
    dkCmdBufBindColorWriteState(g_cb, &cws);
    S.valid = 0;
    for (uint32_t i = 0; i < nr; ++i)
    {
        const int32_t* r = nrects ? &rects[4 * i] : full;
        int32_t x0 = r[0] > vx0 ? r[0] : vx0, y0 = r[1] > vy0 ? r[1] : vy0;
        int32_t x1 = r[2] < vx1 ? r[2] : vx1, y1 = r[3] < vy1 ? r[3] : vy1;
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 > (int32_t)w)
            x1 = (int32_t)w;
        if (y1 > (int32_t)h)
            y1 = (int32_t)h;
        if (x1 <= x0 || y1 <= y0)
            continue;
        DkScissor sc = { (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
        dkCmdBufSetScissors(g_cb, 0, &sc, 1);
        if (flags & 1)
            dkCmdBufClearColorFloat(g_cb, 0, DkColorMask_RGBA, ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                                    (color & 255) / 255.0f, (color >> 24) / 255.0f);
        if (flags & 6)
            dkCmdBufClearDepthStencil(g_cb, (flags & 2) != 0, z, (flags & 4) ? 0xFF : 0, (uint8_t)stencil);
    }
    ++g_prof.clears;
}

/* between textures of one format (the copy engine: any format, compressed ones too) */
void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
              uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!g_up || !src || !dst || src->dkfmt != dst->dkfmt || src->use == GFX_USE_DEPTH || dst->use == GFX_USE_DEPTH ||
        src->dropped || dst->dropped || slevel >= src->levels || dlevel >= dst->levels || sface >= src->layers ||
        dface >= dst->layers || !w || !h)
        return;
    uint32_t slw = level_dim(src->w, slevel), slh = level_dim(src->h, slevel);
    uint32_t dlw = level_dim(dst->w, dlevel), dlh = level_dim(dst->h, dlevel);
    if (sx >= slw || sy >= slh || dx >= dlw || dy >= dlh)
        return;
    if (sx + w > slw)
        w = slw - sx;
    if (dx + w > dlw)
        w = dlw - dx;
    if (sy + h > slh)
        h = slh - sy;
    if (dy + h > dlh)
        h = dlh - dy;
    cmd_open();
    work_kind(WK_XFER);
    DkImageView sv, dv;
    tex_view(&sv, src, sface, slevel);
    tex_view(&dv, dst, dface, dlevel);
    /* a compressed format's rectangles in its 4x4 blocks: dkCmdBufCopyImage hands them to the copy engine as they
     * are, which counts blocks (dkCmdBufCopyBufferToImage converts; this doesn't) */
    if (src->block)
        sx /= 4, sy /= 4, dx /= 4, dy /= 4, w = (w + 3) / 4, h = (h + 3) / 4;
    DkImageRect sr = { sx, sy, 0, w, h, 1 }, dr = { dx, dy, 0, w, h, 1 };
    dkCmdBufCopyImage(g_cb, &sv, &sr, &dv, &dr, 0);
    ++g_prof.copies;
}

/* --- samplers: one descriptor per kind, kept ------------------------------------------------------------ */
static struct
{
    GfxSampler k;
    int used;
} g_samp_map[N_SAMP_DESC];
static uint32_t g_samp_n;

static DkWrapMode wrap(uint32_t a)
{
    switch (a)
    {
    case 2: return DkWrapMode_MirroredRepeat;
    case 3: return DkWrapMode_ClampToEdge;
    case 4: return DkWrapMode_ClampToBorder;
    case 5: return DkWrapMode_MirrorClampToEdge;
    default: return DkWrapMode_Repeat;
    }
}

static uint32_t sampler(const GfxSampler* k)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *k; ++i)
        h = (h ^ ((const uint8_t*)k)[i]) * 16777619u;
    uint32_t i = h % N_SAMP_DESC;
    for (uint32_t n = 0; n < N_SAMP_DESC; ++n, i = (i + 1) % N_SAMP_DESC)
    {
        if (g_samp_map[i].used && !memcmp(&g_samp_map[i].k, k, sizeof *k))
            return i;
        if (!g_samp_map[i].used)
            break;
    }
    if (g_samp_map[i].used || g_samp_n >= N_SAMP_DESC - 1)
    {
        LOG_N(3, "[dk] gfx: out of sampler descriptors\n");
        return 0;
    }
    DkSampler s;
    dkSamplerDefaults(&s);
    s.wrapMode[0] = wrap(k->addr_u), s.wrapMode[1] = wrap(k->addr_v), s.wrapMode[2] = wrap(k->addr_w);
    s.magFilter = k->mag >= 2 ? DkFilter_Linear : DkFilter_Nearest;
    int lin = k->min >= 2;
    s.minFilter = lin ? DkFilter_Linear : DkFilter_Nearest;
    s.mipFilter = k->mip == 0 ? DkMipFilter_None : k->mip == 1 ? DkMipFilter_Nearest : DkMipFilter_Linear;
    if (k->mip)
    {
        s.lodClampMin = (float)k->max_level;
        float maxl = k->lod_cap ? (float)(k->lod_cap - 1) : 1000.0f;
        s.lodClampMax = maxl < (float)k->max_level ? (float)k->max_level : maxl;
    }
    if ((k->min == 3 || k->mag == 3) && k->max_aniso > 1 && k->mip >= 2 && lin)
        s.maxAnisotropy = (float)(k->max_aniso < 16 ? k->max_aniso : 16);
    uint32_t c = k->border;
    s.borderColor[0].value_f = ((c >> 16) & 255) / 255.0f, s.borderColor[1].value_f = ((c >> 8) & 255) / 255.0f;
    s.borderColor[2].value_f = (c & 255) / 255.0f, s.borderColor[3].value_f = (c >> 24) / 255.0f;
    dkSamplerDescriptorInitialize(&g_samp_desc[i], &s);
    g_samp_map[i].k = *k, g_samp_map[i].used = 1, g_samp_n++;
    return i;
}

/* --- programs: one per key pair, compiled on a thread of their own (or read from the card) ---------------- */
typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

enum
{
    P_QUEUED,
    P_COMPILED, /* the code is there, not yet in the code memory (prog_install, the render thread's) */
    P_READY,
    P_FAILED
};

typedef struct Prog
{
    LibKey key;
    uint64_t hash;
    volatile int state;
    char* text;    /* the generated GLSL (both stages), until compiled */
    uint64_t text_hash;
    void* code[2]; /* the vertex and fragment DKSH (uam's), until installed */
    size_t code_n[2];
    int from_card;
    DkShader sh[2];
    struct Prog* next_job;
} Prog;

static Prog** g_progs;
static uint32_t g_progs_cap, g_progs_n;
static struct
{
    uint32_t queued, compiled, cached, failed, installed;
    double compile_ms;
} g_pstat; /* (the compile thread's counts: read on the render thread for the profile only) */
static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;
static Prog *g_job_head, *g_job_tail;
static int g_sync_progs; /* draws wait for their program (FFXI_DK_SYNC, gfx_set_sync_pipelines) */
static char g_cache_dir[256];

static uint64_t hash64(const void* p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; ++i)
        h = (h ^ ((const uint8_t*)p)[i]) * 1099511628211ull;
    return h;
}

/* shader code: blocks of code memory, filled in order and never given back (deko3d's rule for code) */
static DkMemBlock g_code_mem;
static uint32_t g_code_at;

static int code_put(const void* dksh, size_t n, DkShader* sh)
{
    if (!g_code_mem || ALIGN(g_code_at, DK_SHADER_CODE_ALIGNMENT) + n + DK_SHADER_CODE_UNUSABLE_SIZE > CODE_BLOCK)
    {
        g_code_mem = block(CODE_BLOCK, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
        g_code_at = 0;
        if (!g_code_mem)
            return 0;
    }
    uint32_t at = ALIGN(g_code_at, DK_SHADER_CODE_ALIGNMENT);
    memcpy((uint8_t*)dkMemBlockGetCpuAddr(g_code_mem) + at, dksh, n);
    g_code_at = at + (uint32_t)n;
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, g_code_mem, at);
    dkShaderInitialize(sh, &m);
    return 1;
}

/* the card's copy of a program: shader-cache/dk/<text hash>.bin, "XDK1", the text's length and hash, then each
 * stage's DKSH (size, bytes). Keyed by the text: a change to the generator makes new files. */
#define CACHE_MAGIC 0x324b4458u /* XDK2: uam's delay at a block's end */

static void cache_path(char* out, size_t n, uint64_t h) { snprintf(out, n, "%s/%016llx.bin", g_cache_dir, (unsigned long long)h); }

static int cache_load(Prog* p)
{
    if (!g_cache_dir[0])
        return 0;
    char path[320];
    cache_path(path, sizeof path, p->text_hash);
    PlatFile* f = plat_file_open(path, PLAT_READ);
    if (!f)
        return 0;
    uint32_t head[4];
    int ok = plat_file_read(f, head, sizeof head) == sizeof head && head[0] == CACHE_MAGIC &&
             head[1] == (uint32_t)strlen(p->text) && ((uint64_t)head[3] << 32 | head[2]) == p->text_hash;
    for (int s = 0; ok && s < 2; ++s)
    {
        uint32_t n = 0;
        ok = plat_file_read(f, &n, 4) == 4 && n && n < (8u << 20);
        if (ok)
        {
            p->code[s] = malloc(n), p->code_n[s] = n;
            ok = p->code[s] && plat_file_read(f, p->code[s], n) == (int64_t)n;
        }
    }
    plat_file_close(f);
    if (!ok)
        for (int s = 0; s < 2; ++s)
            free(p->code[s]), p->code[s] = NULL;
    return ok;
}

static void cache_store(const Prog* p)
{
    if (!g_cache_dir[0])
        return;
    char path[320], tmp[330];
    cache_path(path, sizeof path, p->text_hash);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    PlatFile* f = plat_file_open(tmp, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE);
    if (!f)
    {
        LOG_N(3, "[dk] gfx: the shader cache can't be written (%s)\n", tmp);
        return;
    }
    uint32_t head[4] = { CACHE_MAGIC, (uint32_t)strlen(p->text), (uint32_t)p->text_hash, (uint32_t)(p->text_hash >> 32) };
    plat_file_write(f, head, sizeof head);
    for (int s = 0; s < 2; ++s)
    {
        uint32_t n = (uint32_t)p->code_n[s];
        plat_file_write(f, &n, 4);
        plat_file_write(f, p->code[s], n);
    }
    plat_file_close(f);
    plat_unlink(path);
    plat_rename(tmp, path);
}

static void* prog_main(void* arg)
{
    (void)arg;
    for (;;)
    {
        pthread_mutex_lock(&g_job_lock);
        while (!g_job_head)
            pthread_cond_wait(&g_job_cond, &g_job_lock);
        Prog* p = g_job_head;
        g_job_head = p->next_job;
        if (!g_job_head)
            g_job_tail = NULL;
        pthread_mutex_unlock(&g_job_lock);
        int ok = 1;
        if (cache_load(p))
            p->from_card = 1, g_pstat.cached++;
        else
        {
            uint64_t t0 = gfx_now_ns();
            for (int s = 0; ok && s < 2; ++s)
            {
                const char* pre = s ? "#version 460\n#define GFX_FS\n" : "#version 460\n#define GFX_VS\n";
                size_t len = strlen(pre) + strlen(p->text) + 1;
                char* src = (char*)malloc(len);
                snprintf(src, len, "%s%s", pre, p->text);
                char* msgs = NULL;
                ok = dk_uam_compile(!s, src, &p->code[s], &p->code_n[s], &msgs);
                if (!ok)
                {
                    static int said;
                    if (said++ < 4)
                        LOG("[dk] gfx: a %s function did not compile: %.400s\n", s ? "fragment" : "vertex",
                            msgs ? msgs : "(no message)");
                }
                free(msgs), free(src);
            }
            g_pstat.compile_ms += (double)(gfx_now_ns() - t0) * 1e-6;
            if (ok)
                g_pstat.compiled++, cache_store(p);
            else
                g_pstat.failed++;
        }
        free(p->text), p->text = NULL;
        __atomic_store_n(&p->state, ok ? P_COMPILED : P_FAILED, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void prog_queue(Prog* p)
{
    pthread_mutex_lock(&g_job_lock);
    p->next_job = NULL;
    if (g_job_tail)
        g_job_tail->next_job = p;
    else
        g_job_head = p;
    g_job_tail = p;
    pthread_cond_signal(&g_job_cond);
    pthread_mutex_unlock(&g_job_lock);
    g_pstat.queued++;
}

/* compiled code into the code memory (the render thread's) */
static void prog_install(Prog* p)
{
    int ok = code_put(p->code[0], p->code_n[0], &p->sh[0]) && code_put(p->code[1], p->code_n[1], &p->sh[1]);
    for (int s = 0; s < 2; ++s)
        free(p->code[s]), p->code[s] = NULL;
    p->state = ok ? P_READY : P_FAILED;
    if (ok)
        g_pstat.installed++;
    else
        g_failures++;
}

static Prog* prog_find(const LibKey* k, uint64_t h)
{
    if (!g_progs_cap)
        return NULL;
    for (uint32_t i = (uint32_t)h & (g_progs_cap - 1);; i = (i + 1) & (g_progs_cap - 1))
    {
        Prog* p = g_progs[i];
        if (!p)
            return NULL;
        if (p->hash == h && !memcmp(&p->key, k, sizeof *k))
            return p;
    }
}

static void prog_add(Prog* p)
{
    if ((g_progs_n + 1) * 10 > g_progs_cap * 7)
    {
        uint32_t cap = g_progs_cap ? g_progs_cap * 2 : 1024;
        Prog** t = (Prog**)calloc(cap, sizeof(Prog*));
        for (uint32_t i = 0; i < g_progs_cap; ++i)
            if (g_progs[i])
            {
                uint32_t j = (uint32_t)g_progs[i]->hash & (cap - 1);
                while (t[j])
                    j = (j + 1) & (cap - 1);
                t[j] = g_progs[i];
            }
        free(g_progs);
        g_progs = t, g_progs_cap = cap;
    }
    uint32_t j = (uint32_t)p->hash & (g_progs_cap - 1);
    while (g_progs[j])
        j = (j + 1) & (g_progs_cap - 1);
    g_progs[j] = p;
    g_progs_n++;
}

/* a key's program, made and queued the first time (installed once compiled; waited for with g_sync_progs) */
static Prog* prog_get(const LibKey* k, const GfxDraw* d)
{
    static Prog* last[2];
    static int last_next;
    for (int i = 0; i < 2; ++i)
        if (last[i] && !memcmp(&last[i]->key, k, sizeof *k))
            return last[i];
    uint64_t h = hash64(k, sizeof *k, 1469598103934665603ull);
    Prog* p = prog_find(k, h);
    if (!p)
    {
        p = (Prog*)calloc(1, sizeof *p);
        p->key = *k, p->hash = h;
        p->text = gfx_dk_generate(&k->vs, &k->fs, d->vs_tokens, d->ps_tokens);
        prog_add(p);
        if (!p->text)
        {
            p->state = P_FAILED, g_failures++;
            LOG_N(4, "[dk] gfx: the generator made no program for a draw\n");
        }
        else
        {
            p->text_hash = hash64(p->text, strlen(p->text), 1469598103934665603ull ^ 0xD5D5u);
            p->state = P_QUEUED;
            prog_queue(p);
        }
    }
    int st = __atomic_load_n(&p->state, __ATOMIC_ACQUIRE);
    if (st == P_QUEUED && g_sync_progs)
    {
        uint64_t t0 = gfx_now_ns();
        while ((st = __atomic_load_n(&p->state, __ATOMIC_ACQUIRE)) == P_QUEUED)
            svcSleepThread(200000);
        g_prof.prog_wait_ns += gfx_now_ns() - t0;
    }
    if (st == P_COMPILED)
        prog_install(p);
    if (p->state == P_READY)
        last[last_next] = p, last_next ^= 1;
    return p;
}

/* the draw's program: ready, or NULL (queued, failed). The world's lit draws are lit per pixel when the effects'
 * light setting says so (gfx_msl.c pixel_lit, as gfx_gl.c), by the vertex's light when that program fails. */
static Prog* program(const GfxDraw* d)
{
    LibKey k;
    memset(&k, 0, sizeof k);
    k.vs = d->vs, k.fs = d->fs;
    k.vs.pixel = k.vs.shadow = k.vs.water = 0; /* the scene effects' variants are not drawn here yet */
    k.fs.water = 0;
    if (g_fxs.fx != 0.0f && g_fxs.light != 0.0f && d->vs.lighting && !d->vs.rhw && !d->vs.prog && !d->vs.flat)
    {
        k.vs.pixel = g_fxs.light >= 2.0f ? 2 : 1;
        Prog* p = prog_get(&k, d);
        if (p->state != P_FAILED)
            return p->state == P_READY ? p : NULL;
        k.vs.pixel = 0;
    }
    Prog* p = prog_get(&k, d);
    return p->state == P_READY ? p : NULL;
}

void gfx_set_sync_pipelines(int on) { g_sync_progs = on || env_on("FFXI_DK_SYNC"); }

/* --- fixed-function state ------------------------------------------------------------------------------- */
static DkBlendFactor blend_factor(uint32_t f, int x8)
{
    switch (f)
    {
    case 1: return DkBlendFactor_Zero;
    case 2: return DkBlendFactor_One;
    case 3: return DkBlendFactor_SrcColor;
    case 4: return DkBlendFactor_InvSrcColor;
    case 5: return DkBlendFactor_SrcAlpha;
    case 6: return DkBlendFactor_InvSrcAlpha;
    case 7: return x8 ? DkBlendFactor_One : DkBlendFactor_DstAlpha;
    case 8: return x8 ? DkBlendFactor_Zero : DkBlendFactor_InvDstAlpha;
    case 9: return DkBlendFactor_DstColor;
    case 10: return DkBlendFactor_InvDstColor;
    case 11: return DkBlendFactor_SrcAlphaSaturate;
    default: return DkBlendFactor_One;
    }
}

static DkBlendOp blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return DkBlendOp_Sub;
    case 3: return DkBlendOp_RevSub;
    case 4: return DkBlendOp_Min;
    case 5: return DkBlendOp_Max;
    default: return DkBlendOp_Add;
    }
}

static DkCompareOp compare(uint32_t f) { return f >= 1 && f <= 8 ? (DkCompareOp)f : DkCompareOp_Always; } /* (D3D's numbers) */

static DkStencilOp stencil_op(uint32_t op) { return op >= 1 && op <= 8 ? (DkStencilOp)op : DkStencilOp_Keep; } /* (D3D's) */

static int g_cull_flip; /* FFXI_DK_CULL_FLIP: the other faces culled (a check of the winding) */

static void set_state(const GfxDraw* d, const GfxTex* depth)
{
    DkCmdBuf cb = g_cb;
    /* blending */
    const GfxPipeKey* p = &d->pipe;
    int x8 = g_rt->fmt == F_X8R8G8B8 || g_rt->fmt == F_X1R5G5B5 || g_rt->fmt == F_R5G6B5;
    DkColorState cs;
    dkColorStateDefaults(&cs);
    cs.blendEnableMask = p->blend ? 1 : 0;
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
    if (p->blend)
    {
        uint32_t sf = p->src, df = p->dst;
        if (sf == 12) /* BOTHSRCALPHA */
            sf = 5, df = 6;
        else if (sf == 13) /* BOTHINVSRCALPHA */
            sf = 6, df = 5;
        DkBlendFactor s = blend_factor(sf, x8), dd = blend_factor(df, x8);
        bs.srcColorBlendFactor = s, bs.dstColorBlendFactor = dd;
        bs.srcAlphaBlendFactor = s == DkBlendFactor_SrcAlphaSaturate ? DkBlendFactor_One : s;
        bs.dstAlphaBlendFactor = dd == DkBlendFactor_SrcAlphaSaturate ? DkBlendFactor_One : dd;
        bs.colorBlendOp = bs.alphaBlendOp = blend_op(p->op);
    }
    DkColorWriteState cws = { (uint32_t)(p->write_mask & 15) };
    if (!S.valid || memcmp(&S.cs, &cs, sizeof cs))
        dkCmdBufBindColorState(cb, &cs), S.cs = cs;
    if (p->blend && (!S.valid || memcmp(&S.bs, &bs, sizeof bs)))
        dkCmdBufBindBlendState(cb, 0, &bs), S.bs = bs;
    if (!S.valid || S.cws.masks != cws.masks)
        dkCmdBufBindColorWriteState(cb, &cws), S.cws = cws;
    /* depth and stencil */
    const GfxDepthKey* k = &d->depth;
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    int zt = depth && k->zenable;
    ds.depthTestEnable = zt;
    ds.depthWriteEnable = zt && k->zwrite;
    ds.depthCompareOp = zt ? compare(k->zfunc) : DkCompareOp_Always;
    int st = depth && depth->has_stencil && k->stencil;
    ds.stencilTestEnable = st;
    if (st)
    {
        ds.stencilFrontFailOp = ds.stencilBackFailOp = stencil_op(k->sfail);
        ds.stencilFrontDepthFailOp = ds.stencilBackDepthFailOp = stencil_op(k->szfail);
        ds.stencilFrontPassOp = ds.stencilBackPassOp = stencil_op(k->spass);
        ds.stencilFrontCompareOp = ds.stencilBackCompareOp = compare(k->sfunc);
        uint32_t sv[3] = { k->swrite, d->stencil_ref & 0xFF, k->sread };
        if (!S.valid || memcmp(S.stencil, sv, sizeof sv))
            dkCmdBufSetStencil(cb, DkFace_FrontAndBack, (uint8_t)sv[0], (uint8_t)sv[1], (uint8_t)sv[2]), memcpy(S.stencil, sv, sizeof sv);
    }
    if (!S.valid || memcmp(&S.ds, &ds, sizeof ds))
        dkCmdBufBindDepthStencilState(cb, &ds), S.ds = ds;
    /* rasterizing: D3D's front faces are clockwise, as deko3d's clip space is D3D's */
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.frontFace = DkFrontFace_CW;
    rs.cullMode = d->cull == 2 ? (g_cull_flip ? DkFace_Back : DkFace_Front)
                : d->cull == 3 ? (g_cull_flip ? DkFace_Front : DkFace_Back)
                               : DkFace_None;
    rs.polygonModeFront = rs.polygonModeBack = d->fill == 1 ? DkPolygonMode_Point : d->fill == 2 ? DkPolygonMode_Line : DkPolygonMode_Fill;
    int po = zt && d->zbias && d->prim >= GFX_TRIANGLELIST;
    rs.depthBiasEnableMask = po ? (DkPolygonFlag_Point | DkPolygonFlag_Line | DkPolygonFlag_Fill) : 0;
    if (!S.valid || memcmp(&S.rs, &rs, sizeof rs))
        dkCmdBufBindRasterizerState(cb, &rs), S.rs = rs;
    if (po)
    {
        /* gfx_gl.c's glPolygonOffset(factor, units): deko3d doubles the constant for the hardware, as nouveau does GL's */
        float units = (float)-d->zbias, factor = -(float)d->zbias * 0.5f;
        if (!S.valid || S.bias[0] != units || S.bias[1] != factor)
            dkCmdBufSetDepthBias(cb, units, 0.0f, factor), S.bias[0] = units, S.bias[1] = factor;
    }
    /* the scissor: the draw's, or the target */
    uint32_t w, h;
    color_size(&w, &h);
    DkScissor sc = { 0, 0, w, h };
    const int32_t* s = d->scissor;
    if (s[2] > 0)
    {
        int64_t x0 = s[0] < 0 ? 0 : s[0], y0 = s[1] < 0 ? 0 : s[1];
        int64_t x1 = (int64_t)s[0] + s[2], y1 = (int64_t)s[1] + s[3];
        if (x1 > w)
            x1 = w;
        if (y1 > h)
            y1 = h;
        if (x0 > x1)
            x0 = x1;
        if (y0 > y1)
            y0 = y1;
        sc.x = (uint32_t)x0, sc.y = (uint32_t)y0, sc.width = (uint32_t)(x1 - x0), sc.height = (uint32_t)(y1 - y0);
    }
    if (!S.valid || memcmp(&S.sc, &sc, sizeof sc))
        dkCmdBufSetScissors(cb, 0, &sc, 1), S.sc = sc;
}

/* D3D's viewport, as it is. 0 when it is empty. */
static int set_viewport(const uint32_t vp[6])
{
    if (!vp[2] || !vp[3])
        return 0;
    float zmin, zmax;
    memcpy(&zmin, &vp[4], 4);
    memcpy(&zmax, &vp[5], 4);
    zmin = zmin < 0 ? 0 : zmin > 1 ? 1 : zmin;
    zmax = zmax < zmin ? zmin : zmax > 1 ? 1 : zmax;
    DkViewport v = { (float)vp[0], (float)vp[1], (float)vp[2], (float)vp[3], zmin, zmax };
    if (!S.valid || memcmp(&S.vp, &v, sizeof v))
        dkCmdBufSetViewports(g_cb, 0, &v, 1), S.vp = v;
    return 1;
}

/* --- drawing -------------------------------------------------------------------------------------------- */
static uint32_t vertex_count(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case GFX_POINTLIST: return n;
    case GFX_LINELIST: return n * 2;
    case GFX_LINESTRIP: return n + 1;
    case GFX_TRIANGLELIST: return n * 3;
    case GFX_TRIANGLESTRIP:
    case GFX_TRIANGLEFAN: return n + 2;
    default: return 0;
    }
}

static DkPrimitive dk_prim(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return DkPrimitive_Points;
    case GFX_LINELIST: return DkPrimitive_Lines;
    case GFX_LINESTRIP: return DkPrimitive_LineStrip;
    case GFX_TRIANGLESTRIP: return DkPrimitive_TriangleStrip;
    case GFX_TRIANGLEFAN: return DkPrimitive_TriangleFan;
    default: return DkPrimitive_Triangles;
    }
}

/* the stages' texture kinds, two bits each (0 none, 1 2D, 2 cube) */
static uint32_t tex_mask(const GfxFsKey* fk)
{
    uint32_t m = 0;
    for (int i = 0; i < 8; ++i)
    {
        int t = fk->prog || i < fk->nstages ? fk->st[i].tex : 0;
        m |= (uint32_t)(t & 3) << (2 * i);
    }
    return m;
}

void gfx_draw(const GfxDraw* d)
{
    ++g_prof.draws;
    if (!g_up || !d->count)
        return;
    uint32_t n = vertex_count(d->prim, d->count);
    if (!n)
        return;
    uint64_t t0 = gfx_now_ns();
    if (!bind_targets())
    {
        gfx_prof_skip(GFX_SKIP_NO_TARGET);
        return;
    }
    Prog* p = program(d);
    if (!p)
    {
        g_prof.pending++;
        return;
    }
    /* the ring room the draw takes, reserved at once (a full ring sends the frame so far: before anything is set) */
    uint32_t need = U_SIZE + 256;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (!d->buf[s] && d->data[s] && d->size[s])
            need += d->size[s] + 16;
    if (d->indices && !d->ibuf)
        need += n * d->index_size + 16;
    if (!ring_reserve(need))
    {
        gfx_prof_skip(GFX_SKIP_RANGE);
        return;
    }
    if (!g_bound) /* (a sync in the reserve: a list of its own) */
        bind_targets();
    GfxTex* depth = depth_attachment();
    if (!set_viewport(d->vp))
        return;
    DkCmdBuf cb = g_cb;
    /* the streams: the block's buffer texture (a heap block's, or the ring's), the stream's place in it added to
     * its registers' offsets */
    GfxU* u = (GfxU*)(g_ring_cpu + ring_put(NULL, U_SIZE, 256));
    DkGpuAddr ubo = g_ring_gpu + (DkGpuAddr)((uint8_t*)u - g_ring_cpu);
    size_t un = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
    if (d->vs.prog)
        un = offsetof(GfxU, psc);
    if (d->fs.prog)
        un = sizeof(GfxU);
    int32_t offs[GFX_NREGS];
    memcpy(offs, d->u.offset, sizeof offs);
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        int desc = g_ring_desc;
        uint32_t at = 0;
        if (d->buf[s])
            desc = d->buf[s]->mem.desc, at = d->buf[s]->mem.off + d->buf_off[s];
        else if (d->data[s] && d->size[s])
            at = ring_put(d->data[s], d->size[s], 16);
        if (desc < 0)
            desc = g_ring_desc;
        DkResHandle hnd = dkMakeTextureHandle((uint32_t)desc, 0);
        if (!S.valid || S.vtex[s] != hnd)
            dkCmdBufBindTexture(cb, DkStage_Vertex, 1 + (uint32_t)s, hnd), S.vtex[s] = hnd;
        if (at)
            for (int r = 0; r < GFX_NREGS; ++r)
                if (d->vs.el[r].used && d->vs.el[r].stream == s)
                    offs[r] += (int32_t)at;
    }
    /* the uniforms: the part the functions read, with the streams' offsets */
    memcpy(u, &d->u, un);
    memcpy(u->offset, offs, sizeof offs);
    dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 0, ubo, U_SIZE);
    dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 0, ubo, U_SIZE);
    /* the stages' textures: none, the wrong kind, depth, or the target being drawn are the white dummy */
    uint32_t mask = tex_mask(&d->fs);
    for (int i = 0; i < 8; ++i)
    {
        uint32_t want = (mask >> (2 * i)) & 3;
        if (!want)
            continue;
        GfxTex* t = d->tex[i];
        if (!t || t->desc < 0 || (want == 2) != (t->type == GFX_TEX_CUBE) || t == g_rt)
            t = want == 2 ? g_dummycube : g_dummy2d;
        DkResHandle hnd = dkMakeTextureHandle((uint32_t)t->desc, sampler(&d->samp[i]));
        if (!S.valid || S.ftex[i] != hnd)
            dkCmdBufBindTexture(cb, DkStage_Fragment, UNIT0 + (uint32_t)i, hnd), S.ftex[i] = hnd;
    }
    if (!S.valid || S.prog != p)
    {
        DkShader const* sh[2] = { &p->sh[0], &p->sh[1] };
        dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
        S.prog = p;
        g_prof.shader_binds++;
    }
    set_state(d, depth);
    S.valid = 1;
    DkPrimitive mode = dk_prim(d->prim);
    DkIdxFormat ifmt = d->index_size == 2 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32;
    if (d->ibuf || d->indices)
    {
        DkGpuAddr ia = d->ibuf ? mem_gpu(&d->ibuf->mem) + d->ibuf_off
                               : g_ring_gpu + ring_put(d->indices, n * d->index_size, 4);
        if (S.ib != ia || S.ib_fmt != (int)ifmt)
            dkCmdBufBindIdxBuffer(cb, ifmt, ia), S.ib = ia, S.ib_fmt = (int)ifmt;
        dkCmdBufDrawIndexed(cb, mode, n, 1, 0, 0, 0);
    }
    else
        dkCmdBufDraw(cb, mode, n, 1, d->vertex_start, 0);
    g_prof.drawn++;
    g_prof.draw_ns += gfx_now_ns() - t0;
}

/* --- the characters (host/scene.c's take of the game's skinned meshes): gfx_gl.c's gfx_chars_draw --------
 * dk_shaders/chars.glsl, compiled at the start on the compile thread (g_chars). Skinned from the bone palette, lit
 * per vertex, two texture stages, alpha tested, table fog; no culling, the depth tested LEQUAL and written. The
 * vertices and coordinates are their buffers' heap blocks as buffer textures, the buffers' first words in the
 * draw's block (D.at.x, .y); a mirrored copy right after its own item is the same draw's second instance, and lists
 * going on from the one before are one draw (as gfx_gl.c's chars_merge). */
static Prog* g_chars;
static void crt_start(void);

static int chars_same(const GfxCharItem* a, const GfxCharItem* b)
{
    return a->vb == b->vb && a->uv == b->uv && a->ib == b->ib && a->index_size == b->index_size && a->prim == b->prim &&
        a->palette == b->palette && a->state == b->state && a->tex == b->tex && a->env == b->env && a->env_tex == b->env_tex &&
        !memcmp(&a->samp, &b->samp, sizeof a->samp) && !memcmp(&a->env_samp, &b->env_samp, sizeof a->env_samp);
}

static int chars_pair(const GfxCharItem* a, const GfxCharItem* b)
{
    return !a->mirror && b->mirror && chars_same(a, b) && a->first == b->first && a->count == b->count;
}

int gfx_chars_env(void) { return 1; }

static void ubo_both(uint32_t id, DkGpuAddr a, uint32_t size)
{
    dkCmdBufBindUniformBuffer(g_cb, DkStage_Vertex, id, a, size);
    dkCmdBufBindUniformBuffer(g_cb, DkStage_Fragment, id, a, size);
}

int gfx_chars_draw(const GfxCharFrame* f)
{
    if (!g_up || !f->nitem || !f->nbones || !g_chars)
        return 0;
    int st = __atomic_load_n(&g_chars->state, __ATOMIC_ACQUIRE);
    if (st == P_COMPILED)
        prog_install(g_chars), st = g_chars->state;
    if (st != P_READY)
    {
        if (st == P_FAILED)
            LOG_N(1, "[dk] gfx: the characters' functions failed: they are not drawn\n");
        g_prof.pending += f->nitem;
        return 0;
    }
    uint64_t t0 = gfx_now_ns();
    if (!bind_targets() || !set_viewport(f->vp))
        return 0;
    /* the ring: a state and a palette each time they change along the items, a block a draw */
    const uint32_t st_sz = (uint32_t)sizeof(GfxCharState), pal_max = 128u * 64u, env_sz = (uint32_t)sizeof(GfxCharEnv);
    uint32_t nst = 0, npal = 0, nenv = 1, ls = ~0u, lp = ~0u, le = ~0u;
    for (uint32_t i = 0; i < f->nitem; ++i)
    {
        nst += f->item[i].state != ls, ls = f->item[i].state;
        npal += f->item[i].palette != lp, lp = f->item[i].palette;
        nenv += f->item[i].env && f->item[i].env != le, le = f->item[i].env;
    }
    uint64_t need = (uint64_t)nst * (st_sz + 256) + (uint64_t)npal * (pal_max + 256) + (uint64_t)f->nitem * (32u + 256) +
        (uint64_t)nenv * (env_sz + 256) + pal_max;
    if (need > RING_SLICE || !ring_reserve((uint32_t)need))
    {
        gfx_prof_skip(GFX_SKIP_RANGE);
        return 0;
    }
    if (!g_bound) /* (a sync in the reserve: a list of its own) */
        bind_targets(), set_viewport(f->vp);
    GfxTex* depth = depth_attachment();
    DkCmdBuf cb = g_cb;
    /* the fixed state */
    DkShader const* sh[2] = { &g_chars->sh[0], &g_chars->sh[1] };
    dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    cs.blendEnableMask = 0;
    dkCmdBufBindColorState(cb, &cs);
    DkColorWriteState cws = { 0xF };
    dkCmdBufBindColorWriteState(cb, &cws);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = depth != NULL, ds.depthWriteEnable = depth != NULL;
    ds.depthCompareOp = DkCompareOp_Lequal, ds.stencilTestEnable = 0;
    dkCmdBufBindDepthStencilState(cb, &ds);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None, rs.depthBiasEnableMask = 0;
    dkCmdBufBindRasterizerState(cb, &rs);
    uint32_t w, h;
    color_size(&w, &h);
    DkScissor sc = { 0, 0, w, h };
    dkCmdBufSetScissors(cb, 0, &sc, 1);
    S.valid = 0; /* (the draws after set everything again) */
    /* the environment layer's block: an empty one for the items without it, and stage 1's textures something of
     * their kind */
    static const GfxCharEnv no_env;
    uint32_t env_at = ring_put(&no_env, env_sz, 256), last_env = 0;
    ubo_both(3, g_ring_gpu + env_at, ALIGN(env_sz, 16));
    uint32_t s0 = sampler(&f->item[0].samp);
    dkCmdBufBindTexture(cb, DkStage_Fragment, UNIT0 + 1, dkMakeTextureHandle((uint32_t)g_dummy2d->desc, s0));
    dkCmdBufBindTexture(cb, DkStage_Fragment, UNIT0 + 2, dkMakeTextureHandle((uint32_t)g_dummycube->desc, s0));
    uint32_t last_state = ~0u, last_pal = ~0u;
    DkResHandle last_vb = ~0u, last_uv = ~0u, last_t = ~0u;
    DkGpuAddr last_ib = 0;
    int last_ifmt = -1;
    int drew = 0;
    for (uint32_t i = 0; i < f->nitem; ++i)
    {
        const GfxCharItem* it = &f->item[i];
        if (!it->vb || !it->uv || !it->ib || !it->tex || !it->count || it->tex->type != GFX_TEX_2D || it->tex->desc < 0 ||
            it->state >= f->nstate || it->palette >= f->nbones || it->vb->mem.desc < 0 || it->uv->mem.desc < 0)
            continue;
        if (it->state != last_state)
        {
            uint32_t at = ring_put(&f->state[it->state], st_sz, 256);
            ubo_both(1, g_ring_gpu + at, ALIGN(st_sz, 16)), last_state = it->state;
        }
        if (it->palette != last_pal)
        {
            uint32_t nb = f->nbones - it->palette;
            uint32_t at = ring_put(f->palette + 16u * it->palette, (nb < 128u ? nb : 128u) * 64u, 256);
            dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 2, g_ring_gpu + at, pal_max), last_pal = it->palette;
        }
        GfxTex* et = it->env && it->env - 1 < f->nenv ? it->env_tex : NULL;
        uint32_t env = et && et->desc >= 0 && (et->type == GFX_TEX_2D || et->type == GFX_TEX_CUBE) && et != g_rt ? it->env : 0;
        if (env != last_env)
        {
            uint32_t at = env_at;
            if (env)
            {
                GfxCharEnv e = f->env[env - 1];
                e.kind[1] = et->type == GFX_TEX_CUBE ? 1.0f : 0.0f;
                at = ring_put(&e, env_sz, 256);
            }
            ubo_both(3, g_ring_gpu + at, ALIGN(env_sz, 16));
            last_env = env;
        }
        if (env)
            dkCmdBufBindTexture(cb, DkStage_Fragment, et->type == GFX_TEX_CUBE ? UNIT0 + 2 : UNIT0 + 1,
                                dkMakeTextureHandle((uint32_t)et->desc, sampler(&it->env_samp)));
        struct
        {
            float params[4];
            uint32_t at[4];
        } u = { { f->alpha_ref, f->pixel[0], f->pixel[1], 0.0f }, { it->vb->mem.off / 4, it->uv->mem.off / 4, it->mirror, 0 } };
        ubo_both(0, g_ring_gpu + ring_put(&u, sizeof u, 256), sizeof u);
        DkResHandle hv = dkMakeTextureHandle((uint32_t)it->vb->mem.desc, 0);
        DkResHandle hu = dkMakeTextureHandle((uint32_t)it->uv->mem.desc, 0);
        if (hv != last_vb)
            dkCmdBufBindTexture(cb, DkStage_Vertex, 1, hv), last_vb = hv;
        if (hu != last_uv)
            dkCmdBufBindTexture(cb, DkStage_Vertex, 2, hu), last_uv = hu;
        GfxTex* t = it->tex == g_rt ? g_dummy2d : it->tex;
        DkResHandle ht = dkMakeTextureHandle((uint32_t)t->desc, sampler(&it->samp));
        if (ht != last_t)
            dkCmdBufBindTexture(cb, DkStage_Fragment, UNIT0, ht), last_t = ht;
        int ifmt = it->index_size == 4 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16;
        DkGpuAddr ia = mem_gpu(&it->ib->mem);
        if (ia != last_ib || ifmt != last_ifmt)
            dkCmdBufBindIdxBuffer(cb, (DkIdxFormat)ifmt, ia), last_ib = ia, last_ifmt = ifmt;
        /* this item's draw, with its mirrored copy as its second instance and the lists going on from it */
        uint32_t count = it->count, inst = 1, used = 1;
        if (i + 1 < f->nitem && chars_pair(it, &f->item[i + 1]))
            inst = 2, used = 2;
        if (it->prim == GFX_TRIANGLELIST)
            for (;;)
            {
                uint32_t k = i + used;
                const GfxCharItem* nx = k < f->nitem ? &f->item[k] : NULL;
                if (!nx || !nx->count || nx->mirror != it->mirror || nx->prim != GFX_TRIANGLELIST || !chars_same(it, nx) ||
                    nx->first != it->first + count * 3)
                    break;
                int nx_pair = k + 1 < f->nitem && chars_pair(nx, &f->item[k + 1]);
                if (nx_pair != (inst == 2)) /* (its copy drawn as ours, or the two apart) */
                    break;
                used += inst, count += nx->count;
            }
        uint32_t n = vertex_count(it->prim, count);
        dkCmdBufDrawIndexed(cb, it->prim == GFX_TRIANGLESTRIP ? DkPrimitive_TriangleStrip : DkPrimitive_Triangles, n, inst,
                            it->first, 0, 0);
        ++drew;
        i += used - 1;
    }
    /* (the bindings the ordinary draws keep as theirs: forgotten) */
    S.ib = 0, S.ib_fmt = -1;
    g_prof.chars_items += f->nitem, g_prof.chars_draws += (uint32_t)drew;
    g_prof.chars_ns += gfx_now_ns() - t0;
    return drew > 0;
}

/* --- the device's start ---------------------------------------------------------------------------------- */
static void image_layout(DkImageLayout* l, uint32_t flags, DkImageFormat fmt, uint32_t w, uint32_t h)
{
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, g_dev);
    m.flags = flags;
    m.format = fmt;
    m.dimensions[0] = w;
    m.dimensions[1] = h;
    dkImageLayoutInitialize(l, &m);
}

/* the part of the swapchain's images on the screen: the screen's size (sdl_nx.c's window: 1280x720 handheld,
 * 1920x1080 docked), from their top left */
static void screen_size(int* w, int* h)
{
    *w = 1280, *h = 720;
    SDL_GetWindowSizeInPixels(NULL, w, h);
    if (*w <= 0 || *w > FB_W || *h <= 0 || *h > FB_H)
        *w = 1280, *h = 720;
}

static void set_interval(void) { dkSwapchainSetSwapInterval(g_sc, g_vsync ? (uint32_t)g_interval : 0); }

/* the white textures a stage without one samples */
static void make_dummies(void)
{
    static const uint32_t white[16] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    g_dummy2d = gfx_tex_create(GFX_TEX_2D, F_A8R8G8B8, 1, 1, 1, GFX_USE_SAMPLE);
    g_dummycube = gfx_tex_create(GFX_TEX_CUBE, F_A8R8G8B8, 1, 1, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(g_dummy2d, 0, 0, white, 4);
    for (uint32_t f = 0; f < 6; ++f)
        gfx_tex_upload(g_dummycube, f, 0, white, 4);
}

int gfx_init(void* sdl_window, int vsync)
{
    (void)sdl_window;
    if (g_up) /* a second device (the game's after the sign-in screen's, or a D3D Reset): the same one */
    {
        if (vsync != g_vsync)
            g_vsync = vsync, set_interval();
        return 1;
    }
    gfx_profiling = env_on("FFXI_PROFILE");
    g_cull_flip = env_on("FFXI_DK_CULL_FLIP");
    g_sync_progs = env_on("FFXI_DK_SYNC");
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = on_debug;
    dm.cbAlloc = dk_alloc, dm.cbFree = dk_free;
    /* D3D's conventions are deko3d's defaults: depth 0..1, the origin at the top left, y up */
    g_dev = dkDeviceCreate(&dm);
    if (!g_dev)
    {
        LOG("[dk] gfx: no deko3d device\n");
        return 0;
    }
    DkImageLayout fb;
    image_layout(&fb, DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine,
                 DkImageFormat_RGBA8_Unorm, FB_W, FB_H);
    uint32_t fb_size = ALIGN((uint32_t)dkImageLayoutGetSize(&fb), dkImageLayoutGetAlignment(&fb));
    g_fb_mem = block(fb_size * FB_NUM, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    DkImage const* fbs[FB_NUM];
    for (int i = 0; i < FB_NUM; ++i)
        dkImageInitialize(&g_fb[i], &fb, g_fb_mem, (uint32_t)i * fb_size), fbs[i] = &g_fb[i];
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, g_dev, nwindowGetDefault(), fbs, FB_NUM);
    g_sc = dkSwapchainCreate(&sm);
    if (!g_sc)
    {
        LOG("[dk] gfx: no swapchain\n");
        return 0;
    }
    uint32_t cpu = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_cmd_mem = block(CMD_SLICE * CMD_SLOTS, cpu);
    g_stage_mem = block(STAGE_SLICE * CMD_SLOTS, cpu);
    g_ring_mem = block(RING_SLICE * CMD_SLOTS, cpu);
    uint32_t img_desc_size = ALIGN(N_IMG_DESC * (uint32_t)sizeof(DkImageDescriptor), 256);
    g_desc_mem = block(img_desc_size + N_SAMP_DESC * (uint32_t)sizeof(DkSamplerDescriptor), cpu);
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_dev);
    qm.flags = DkQueueFlags_Graphics;
    g_q = dkQueueCreate(&qm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, g_dev);
    cm.cbAddMem = cmd_more;
    g_cb = dkCmdBufCreate(&cm);
    if (!g_q || !g_cb || !g_cmd_mem || !g_stage_mem || !g_ring_mem || !g_desc_mem)
    {
        LOG("[dk] gfx: no queue, command memory, staging, ring or descriptors\n");
        return 0;
    }
    g_stage_cpu = (uint8_t*)dkMemBlockGetCpuAddr(g_stage_mem);
    g_stage_gpu = dkMemBlockGetGpuAddr(g_stage_mem);
    g_ring_cpu = (uint8_t*)dkMemBlockGetCpuAddr(g_ring_mem);
    g_ring_gpu = dkMemBlockGetGpuAddr(g_ring_mem);
    g_img_desc = (DkImageDescriptor*)dkMemBlockGetCpuAddr(g_desc_mem);
    g_samp_desc = (DkSamplerDescriptor*)((uint8_t*)dkMemBlockGetCpuAddr(g_desc_mem) + img_desc_size);
    g_img_desc_gpu = dkMemBlockGetGpuAddr(g_desc_mem);
    g_samp_desc_gpu = g_img_desc_gpu + img_desc_size;
    memset(g_img_desc, 0, img_desc_size + N_SAMP_DESC * sizeof(DkSamplerDescriptor));
    slots_init(&g_img_slots, N_IMG_DESC, 0);
    /* sampler 0: the defaults, for the streams' handles (a buffer texture's sampler is not used); never a key's */
    DkSampler s0;
    dkSamplerDefaults(&s0);
    dkSamplerDescriptorInitialize(&g_samp_desc[0], &s0);
    memset(&g_samp_map[0].k, 0xFF, sizeof g_samp_map[0].k);
    g_samp_map[0].used = 1, g_samp_n = 1;
    g_ring_desc = slot_take(&g_img_slots);
    buffer_descriptor(g_ring_desc, g_ring_mem, 0, RING_SLICE * CMD_SLOTS);
    /* the compile thread (uam: one compile at a time, its own stack) and the card's cache */
    const char* cd = getenv("FFXI_DK_CACHE");
    snprintf(g_cache_dir, sizeof g_cache_dir, "%s", cd ? cd : "sdmc:/switch/xi-on-anything/shader-cache/dk");
    if (!strcmp(g_cache_dir, "0"))
        g_cache_dir[0] = 0;
    else
        plat_mkdir("sdmc:/switch/xi-on-anything/shader-cache"), plat_mkdir(g_cache_dir);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    pthread_t th;
    if (pthread_create(&th, &at, prog_main, NULL) != 0)
    {
        LOG("[dk] gfx: no compile thread\n");
        return 0;
    }
    pthread_attr_destroy(&at);
    g_chars = (Prog*)calloc(1, sizeof *g_chars);
    g_chars->text = strdup(DK_CHARS_GLSL);
    g_chars->text_hash = hash64(g_chars->text, strlen(g_chars->text), 1469598103934665603ull ^ 0xC4A5u);
    g_chars->state = P_QUEUED;
    prog_queue(g_chars);
    crt_start();
    g_vsync = vsync;
    set_interval();
    g_up = 1;
    fx_config();
    const char* show_fps = getenv("FFXI_FPS"); /* (the Switch's is 0 unless run.args says) */
    if (show_fps && show_fps[0] == '0')
        g_fxs.fps = 0.0f;
    make_dummies();
    LOG("[dk] gfx: deko3d up: %d swapchain images of %dx%d, %u KB of command memory, %u KB of staging, %u KB of "
        "ring; programs compiled by uam on a thread of their own%s, kept in %s; milestone 3: draws\n",
        FB_NUM, FB_W, FB_H, CMD_SLICE * CMD_SLOTS >> 10, STAGE_SLICE * CMD_SLOTS >> 10, RING_SLICE * CMD_SLOTS >> 10,
        g_sync_progs ? " (draws wait for them: FFXI_DK_SYNC)" : "", g_cache_dir[0] ? g_cache_dir : "(nowhere)");
    return 1;
}

void gfx_resize(uint32_t w, uint32_t h) { (void)w, (void)h; } /* the screen's size is asked at each present */
uint64_t gfx_window_flags(void) { return 0; }

/* vblanks each frame is shown for (gfx_queue.c's adaptive cap): 1 for 60 fps, 2 for an even 30 */
void gfx_nx_swap_interval(int n)
{
    g_interval = n > 0 ? n : 1;
    if (g_up)
        set_interval();
}

/* --- the present ---------------------------------------------------------------------------------------- */
void gfx_set_present_rect(int x, int y, int w, int h)
{
    g_prect[0] = x, g_prect[1] = y, g_prect[2] = w, g_prect[3] = h;
}
void gfx_set_wallpaper(const uint8_t* rgba, int w, int h) { (void)rgba, (void)w, (void)h; }
void gfx_set_touch_shapes(const GfxTouchShape* shapes, int n) { (void)shapes, (void)n; }

/* every 5 seconds (bring-up: always; FFXI_PROFILE or not): what the game asked of the back end */
static void prof_frame(void)
{
    uint64_t now = gfx_now_ns();
    if (!g_prof.since)
        g_prof.since = now;
    ++g_prof.frames;
    if (now - g_prof.since < 5000000000ull)
        return;
    double s = (double)(now - g_prof.since) * 1e-9, f = g_prof.frames ? (double)g_prof.frames : 1.0;
    LOG("[dk] %.1f fps; a frame: %.0f draws asked, %.0f drawn (%.0f waiting for their program, %.1f skipped), %.3f ms "
        "recording them (%.2f us a draw), %.0f program binds; %.1f clears, %.1f copies, %.1f target changes, %.1f barriers, "
        "%.0f uploads (%.1f KB), ring %.0f KB; characters %.0f items in %.0f draws (%.3f ms); acquire %.2f ms, submit "
        "%.2f ms\n",
        g_prof.frames / s, g_prof.draws / f, g_prof.drawn / f, g_prof.pending / f, g_prof.skips / f,
        (double)g_prof.draw_ns / f * 1e-6, g_prof.drawn ? (double)g_prof.draw_ns / g_prof.drawn * 1e-3 : 0.0,
        g_prof.shader_binds / f, g_prof.clears / f, g_prof.copies / f, g_prof.targets / f, g_prof.barriers / f,
        g_prof.uploads / f, (double)g_prof.upload_bytes / f / 1024.0, (double)g_prof.ring_bytes / f / 1024.0,
        g_prof.chars_items / f, g_prof.chars_draws / f, (double)g_prof.chars_ns / f * 1e-6,
        (double)g_prof.acquire_ns / f * 1e-6, (double)g_prof.submit_ns / f * 1e-6);
    LOG("[dk]   programs: %u known, %u queued so far, %u compiled (%.0f ms each), %u from the card, %u failed, %u "
        "installed; %u syncs (%.1f ms), %u reads, %u without waiting, %u command memory added, %.1f ms waiting for programs; textures %u "
        "live, buffers %u; memory: images %.1f MB in %u blocks + %.1f MB own, buffers %.1f MB in %u blocks + %.1f MB "
        "own; %u samplers; %u deko3d errors\n",
        g_progs_n, g_pstat.queued, g_pstat.compiled, g_pstat.compiled ? g_pstat.compile_ms / g_pstat.compiled : 0.0,
        g_pstat.cached, g_pstat.failed, g_pstat.installed, g_prof.syncs, (double)g_prof.sync_ns * 1e-6, g_prof.reads, g_prof.async_reads,
        g_prof.cmd_more, (double)g_prof.prog_wait_ns * 1e-6, g_tex_live, g_buf_live, g_img_heap.used / 1048576.0,
        g_img_heap.n, g_img_heap.own / 1048576.0, g_data_heap.used / 1048576.0, g_data_heap.n,
        g_data_heap.own / 1048576.0, g_samp_n, g_dk_errors);
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since = now;
}

/* --- the CRT filter: the present through dk_shaders/crt.glsl (gfx_gl.c's) --------------------------------
 * FFXI_CRT=1 scanlines, 2 with an aperture grille too; FFXI_CRT_CURVE=1 the glass's curve. Until its program is
 * compiled (the first frames) the picture is blitted as without it. */
static int g_crt, g_crt_curve;
static Prog* g_crt_prog;

static void crt_start(void)
{
    const char* e = getenv("FFXI_CRT");
    g_crt = e ? atoi(e) : 0;
    g_crt = g_crt < 0 ? 0 : g_crt > 2 ? 2 : g_crt;
    g_crt_curve = env_on("FFXI_CRT_CURVE");
    if (!g_crt || !(g_crt_prog = (Prog*)calloc(1, sizeof *g_crt_prog)))
        return;
    g_crt_prog->text = strdup(DK_CRT_GLSL);
    g_crt_prog->text_hash = hash64(g_crt_prog->text, strlen(g_crt_prog->text), 1469598103934665603ull ^ 0xC27u);
    g_crt_prog->state = P_QUEUED;
    prog_queue(g_crt_prog);
    LOG("[gfx] CRT filter: %s%s\n", g_crt == 2 ? "scanlines and aperture grille" : "scanlines", g_crt_curve ? ", curved" : "");
}

static int crt_ready(void)
{
    if (!g_crt || !g_crt_prog)
        return 0;
    int st = __atomic_load_n(&g_crt_prog->state, __ATOMIC_ACQUIRE);
    if (st == P_COMPILED)
        prog_install(g_crt_prog), st = g_crt_prog->state;
    if (st == P_FAILED)
    {
        LOG("[gfx] CRT filter: its shader did not compile: off\n");
        g_crt = 0;
    }
    return st == P_READY;
}

/* the picture at a whole multiple of its height when it is half the screen's or less (even scanlines) */
static void crt_fit(const GfxTex* b, int dw, int dh, int* rx, int* ry, int* rw, int* rh)
{
    int k = *rh / (int)b->h;
    if (k < 2)
        return;
    int h = (int)b->h * k, w = (int)((long long)h * *rw / *rh);
    if (w > dw)
        return;
    *rx = (dw - w) / 2, *ry = (dh - h) / 2, *rw = w, *rh = h;
}

/* the back buffer into the rectangle of the bound swapchain image, its constants in the ring at ubo */
static void crt_present(GfxTex* b, DkGpuAddr ubo, int x, int y, int w, int h)
{
    DkCmdBuf cb = g_cb;
    DkShader const* sh[2] = { &g_crt_prog->sh[0], &g_crt_prog->sh[1] };
    dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    cs.blendEnableMask = 0;
    dkCmdBufBindColorState(cb, &cs);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = ds.depthWriteEnable = ds.stencilTestEnable = 0;
    dkCmdBufBindDepthStencilState(cb, &ds);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None, rs.depthBiasEnableMask = 0;
    dkCmdBufBindRasterizerState(cb, &rs);
    DkViewport vp = { (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f };
    dkCmdBufSetViewports(cb, 0, &vp, 1);
    ubo_both(0, ubo, 32);
    GfxSampler k;
    memset(&k, 0, sizeof k);
    k.addr_u = k.addr_v = k.addr_w = 3, k.mag = k.min = 2;
    dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle((uint32_t)b->desc, sampler(&k)));
    dkCmdBufDraw(cb, DkPrimitive_Triangles, 3, 1, 0, 0);
    S.valid = 0;
}

/* --- the frame-rate counter (FFXI_FPS=1; Config's FPS Counter): the presents of the last half second, at the
 * screen's top left - the other back ends' 5x7 font, each run of its pixels a scissored clear --------------- */
static void fps_draw(int dh)
{
    static const uint8_t font[13][7] = {
        { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
        { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
        { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
        { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
        { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, /* F P S */
    };
    static uint64_t since;
    static uint32_t frames;
    static char text[16] = "-- FPS";
    uint64_t now = gfx_now_ns();
    if (!since)
        since = now;
    ++frames;
    if (now - since >= 500000000ull)
    {
        snprintf(text, sizeof text, "%.0f FPS", frames * 1e9 / (double)(now - since));
        since = now, frames = 0;
    }
    if (g_fxs.fps == 0.0f)
        return;
    uint32_t k = dh >= 1000 ? 3 : 2, n = (uint32_t)strlen(text), x0 = 4, y0 = 4;
    work_kind(WK_DRAW);
    DkScissor box = { x0, y0, (6 * n + 3) * k, 11 * k };
    dkCmdBufSetScissors(g_cb, 0, &box, 1);
    dkCmdBufClearColorFloat(g_cb, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    for (uint32_t i = 0; i < n; ++i)
    {
        char c = text[i];
        int g = c >= '0' && c <= '9' ? c - '0' : c == 'F' ? 10 : c == 'P' ? 11 : c == 'S' ? 12 : -1;
        for (uint32_t y = 0; g >= 0 && y < 7; ++y)
            for (uint32_t x = 0; x < 5;)
            {
                uint32_t w = 0;
                while (x + w < 5 && font[g][y] >> (4 - x - w) & 1)
                    ++w;
                if (!w)
                {
                    ++x;
                    continue;
                }
                DkScissor r = { x0 + (2 + 6 * i + x) * k, y0 + (2 + y) * k, w * k, k };
                dkCmdBufSetScissors(g_cb, 0, &r, 1);
                dkCmdBufClearColorFloat(g_cb, 0, DkColorMask_RGBA, 1.0f, 0.85f, 0.2f, 1.0f);
                x += w;
            }
    }
}

void gfx_present(GfxTex* backbuffer)
{
    if (!g_up)
        return;
    if (g_slot % 30 == 0)
        fx_reload();
    int dw, dh;
    screen_size(&dw, &dh);
    int rx = 0, ry = 0, rw = dw, rh = dh;
    if (g_prect[2] > 0 && g_prect[3] > 0)
        rx = g_prect[0], ry = g_prect[1], rw = g_prect[2], rh = g_prect[3];
    else if (backbuffer && backbuffer->w && backbuffer->h)
    {
        /* a picture of another shape than the screen's: as large as fits, in the middle, black beside it */
        long long bw = backbuffer->w, bh = backbuffer->h;
        if (llabs(bw * dh - bh * dw) * 50 > (long long)dw * dh)
        {
            if (bw * dh > bh * dw)
                rh = (int)(dw * bh / bw) & ~1, ry = (dh - rh) / 2;
            else
                rw = (int)(dh * bw / bh) & ~1, rx = (dw - rw) / 2;
        }
    }
    int show = backbuffer && backbuffer->use == GFX_USE_RT && rw > 0 && rh > 0;
    int crt = show && backbuffer->desc >= 0 && crt_ready();
    if (crt)
        crt_fit(backbuffer, dw, dh, &rx, &ry, &rw, &rh);
    nx_present_rect(rx, ry, rw, rh);
    cmd_open();
    DkGpuAddr crt_ubo = 0;
    if (crt && !ring_reserve(32 + 256))
        crt = 0;
    if (crt) /* (before the image is bound: a full ring starts another list) */
    {
        /* the scanlines' depth: full at two of the screen's lines a row, none at one */
        float per_row = (float)rh / (float)backbuffer->h;
        float depth = per_row <= 1.0f ? 0.0f : per_row >= 2.0f ? 1.0f : per_row - 1.0f;
        float c[8] = { (float)backbuffer->w, (float)backbuffer->h, 1.0f / (float)backbuffer->w, 1.0f / (float)backbuffer->h,
                       (float)g_crt, (float)g_crt_curve, depth, 0.0f };
        crt_ubo = g_ring_gpu + ring_put(c, sizeof c, 256);
    }
    uint64_t t0 = gfx_now_ns();
    int img = dkQueueAcquireImage(g_q, g_sc);
    uint64_t t1 = gfx_now_ns();
    DkImageView fbv;
    dkImageViewDefaults(&fbv, &g_fb[img]);
    work_kind(WK_DRAW);
    dkCmdBufBindRenderTarget(g_cb, &fbv, NULL);
    g_bound = 0;
    S.valid = 0;
    DkScissor sc = { 0, 0, FB_W, FB_H };
    dkCmdBufSetScissors(g_cb, 0, &sc, 1);
    DkColorWriteState cws = { 0xF };
    dkCmdBufBindColorWriteState(g_cb, &cws);
    if (show)
        dkCmdBufClearColorFloat(g_cb, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    else
    {
        /* no picture: a slow pulse of blue, to show frames are presented */
        float pulse = (float)(g_slot % 120) / 120.0f;
        pulse = pulse < 0.5f ? pulse * 2.0f : 2.0f - pulse * 2.0f;
        dkCmdBufClearColorFloat(g_cb, 0, DkColorMask_RGBA, 0.02f, 0.03f, 0.10f + 0.15f * pulse, 1.0f);
    }
    if (show && crt)
    {
        barrier(); /* (the picture drawn, then sampled) */
        crt_present(backbuffer, crt_ubo, rx, ry, rw, rh);
    }
    else if (show)
    {
        work_kind(WK_XFER);
        DkImageView bv;
        tex_view(&bv, backbuffer, 0, 0);
        DkImageRect sr = { 0, 0, 0, backbuffer->w, backbuffer->h, 1 };
        DkImageRect dr = { (uint32_t)rx, (uint32_t)ry, 0, (uint32_t)rw, (uint32_t)rh, 1 };
        int same = rw == (int)backbuffer->w && rh == (int)backbuffer->h;
        dkCmdBufBlitImage(g_cb, &bv, &sr, &fbv, &dr, same ? DkBlitFlag_FilterNearest : DkBlitFlag_FilterLinear, 0);
    }
    fps_draw(dh);
    /* flushed: the caches' writes are out when it signals, as the slot's memory is written again after */
    dkCmdBufSignalFence(g_cb, &g_fence[g_cur], true);
    dkQueueSubmitCommands(g_q, dkCmdBufFinishList(g_cb));
    nwindowSetCrop(nwindowGetDefault(), 0, 0, (s32)dw, (s32)dh);
    dkQueuePresentImage(g_q, g_sc, img);
    g_open = 0;
    ++g_slot;
    uint64_t t2 = gfx_now_ns();
    g_prof.acquire_ns += t1 - t0, g_prof.submit_ns += t2 - t1;
    prof_frame();
}

/* --- the rest of gfx.h: as gfx_gl.c's, not on this back end -------------------------------------------- */
void gfx_set_focus(const float* pos) { (void)pos; }
void gfx_set_moghouse(int in) { (void)in; }
int gfx_sun_shadows_shown(void) { return 0; }
float gfx_sun_prime(float* center)
{
    (void)center;
    return 0.0f;
}
void gfx_scene_done(GfxTex* color, const GfxScene* s) { (void)color, (void)s; }
int gfx_rt_supported(void) { return 0; }
int gfx_world_supported(void) { return 0; }
int gfx_world_draw(const GfxWorldFrame* f)
{
    (void)f;
    return 0;
}
void gfx_world_time(int pass) { (void)pass; }
void gfx_shadow_begin(const GfxShadowFrame* f) { (void)f; }
void gfx_gi_zone(const GfxGiZone* z) { (void)z; }
void gfx_gi_frame(const GfxGiFrame* f) { (void)f; }
void gfx_lamps(const GfxWorldLight* l, uint32_t n) { (void)l, (void)n; }
void gfx_set_fixed_light(int fixed) { (void)fixed; }
void gfx_set_weather(int weather) { (void)weather; }
void gfx_trace_dump(const char* path) { (void)path; }
void gfx_finish(void)
{
    if (g_up)
        cmd_sync();
}
uint32_t gfx_failures(void) { return g_failures; }

#if defined(GFX_QUEUE)
#define GFX_QUEUE_IMPL /* the back end's own gfx_* names, not the queue's */
#include "gfx_queue.h"
/* the render queue's hooks (gfx_queue.h), on its render thread: the read and the device at once */
void gfx_tex_read_then(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch, GfxqCall* call)
{
    gfx_tex_read(t, face, level, dst, pitch);
    gfxq_call_done(call);
}
void gfx_init_then(void* sdl_window, int vsync, GfxqCall* call)
{
    gfxq_call_result(call, gfx_init(sdl_window, vsync));
    gfxq_call_done(call);
}
void gfx_web_tick(void) {}
#endif
