/* The graphics back end on deko3d, the Switch's own low-level API (devkitPro): gfx.h for the Nintendo Switch,
 * next to gfx_gl.c (OpenGL on Mesa's nouveau), chosen at build time (tools/build_switch.py, XI_SWITCH_GFX=deko3d).
 * Why: nouveau's CPU work per draw is the limit at stock clocks; deko3d records a draw for under a microsecond
 * (platforms/switch/tests/drawcost).
 *
 * Threads: as gfx_gl.c, the game is built with the render queue (GFX_QUEUE, gfx_queue.c), whose render thread
 * makes the device and makes every call (GFXQ_OWNED). Log lines from it are handed to the game's thread
 * (gfx_gl_prof_take, from host64.c and the sign-in screen), which writes them: stdio from another thread breaks
 * the main thread's.
 *
 * Coordinates: deko3d's defaults are D3D8's (depth 0..1, the origin at the top left, clip-space y up), so the
 * shaders (gfx_msl.c's deko3d dialect, gfx_dk_generate) keep D3D's clip space and nothing is flipped; row 0 of
 * every image is D3D's top row.
 *
 * Memory: images in heaps of 32 MB blocks (GPU-cached), buffers and the uploads' staging in CPU-written ones,
 * suballocated (heap_*); what is freed is kept until the GPU is done with the frame that freed it (the slot's
 * fence: garbage). One command buffer, recorded through the frame into the slot's command memory (more is added
 * when it runs out: cmd_more) and submitted at the present; uploads are copies from the slot's staging, which
 * when full sends the frame so far and waits (cmd_sync), as do reads. Transfers and drawing are kept apart by a
 * barrier where one follows the other (work_kind).
 *
 * Where it is (the plan's phase 2, its milestones):
 *  1. bring-up: the device, the queue, the swapchain on the default window and the present;
 *  2. resources (this): buffers, textures in the game's formats, targets, clears, copies, reads (waiting), the
 *     back buffer shown at the present; draws are counted and dropped;
 *  3. programs (gfx_dk_generate, uam on a thread of its own, a DKSH cache on the card), state and draws;
 *  4. the world, the characters, reads without waiting, the present's letterboxing and CRT pass. */
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <deko3d.h>
#include <switch.h>
#include <SDL3/SDL.h>

#include "../gfx.h"
#include "gfx_fx.h"
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
    LOG_LINES = 32,
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
    uint64_t since, front_ns, shim_ns, rt_ns[64], acquire_ns, submit_ns, sync_ns;
    uint32_t frames, draws, clears, copies, skips, targets, tex_made, buf_made, uploads, barriers, syncs, reads;
    uint32_t cmd_more;
    uint64_t upload_bytes;
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
static DkMemBlock g_fb_mem, g_cmd_mem, g_stage_mem;
static uint8_t* g_stage_cpu;
static DkGpuAddr g_stage_gpu;
static DkImage g_fb[FB_NUM];
static DkSwapchain g_sc;
static DkCmdBuf g_cb;
static DkFence g_fence[CMD_SLOTS];
static uint32_t g_slot; /* the frames presented */
static uint32_t g_cur;  /* the slot being recorded (g_slot's) */
static int g_open;      /* the command buffer has this frame's memory */
static uint32_t g_stage_at;
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

/* --- memory: heaps of blocks, suballocated -------------------------------------------------------------- */
typedef struct
{
    uint32_t off, size;
} Span;

typedef struct
{
    DkMemBlock mem;
    uint32_t size;
    Span* free; /* sorted by offset, neighbours joined */
    uint32_t nfree, cap;
} Chunk;

typedef struct
{
    const char* name;
    uint32_t flags, chunk_size;
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
} Mem;

static Heap g_img_heap = { "images", DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 32u << 20, NULL, 0, 0, 0 };
static Heap g_data_heap = { "buffers", DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 16u << 20, NULL, 0, 0, 0 };

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
    m->heap = h, m->size = size;
    if (align < 256)
        align = 256;
    if (size > h->chunk_size / 2)
    {
        m->mem = block(size, h->flags), m->chunk = -1, m->off = 0;
        if (!m->mem)
            return 0;
        h->own += size;
        return 1;
    }
    for (uint32_t i = 0; i < h->n; ++i)
        if (chunk_take(&h->c[i], size, align, &m->off))
        {
            m->mem = h->c[i].mem, m->chunk = (int32_t)i;
            h->used += size;
            return 1;
        }
    Chunk c = { 0 };
    c.mem = block(h->chunk_size, h->flags);
    if (!c.mem)
    {
        LOG_N(10, "[dk] gfx: no memory for another %u MB of %s (%u blocks so far)\n", h->chunk_size >> 20, h->name, h->n);
        return 0;
    }
    c.size = h->chunk_size;
    span_insert(&c, 0, (Span){ 0, c.size });
    h->c = (Chunk*)realloc(h->c, (h->n + 1) * sizeof(Chunk));
    h->c[h->n] = c;
    chunk_take(&h->c[h->n], size, align, &m->off);
    m->mem = c.mem, m->chunk = (int32_t)h->n++;
    h->used += size;
    return 1;
}

static void heap_free_now(Mem* m)
{
    if (!m->mem)
        return;
    if (m->chunk < 0)
        dkMemBlockDestroy(m->mem), m->heap->own -= m->size;
    else
        chunk_give(&m->heap->c[m->chunk], m->off, m->size), m->heap->used -= m->size;
    m->mem = NULL;
}

/* what was freed in a frame: given back once the GPU is past it (its slot's fence) */
static struct
{
    Mem* m;
    uint32_t n, cap;
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

static void garbage_release(uint32_t s)
{
    for (uint32_t i = 0; i < g_garbage[s].n; ++i)
        heap_free_now(&g_garbage[s].m[i]);
    g_garbage[s].n = 0;
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

static void cmd_begin_memory(void)
{
    dkCmdBufClear(g_cb);
    dkCmdBufAddMemory(g_cb, g_cmd_mem, g_cur * CMD_SLICE, CMD_SLICE);
    g_more[g_cur].used = 0;
    g_stage_at = 0;
    g_bound = 0;
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

/* what is recorded so far sent, and the GPU waited for (staging full, a read): then everything is free */
static void cmd_sync(void)
{
    uint64_t t0 = gfx_now_ns();
    cmd_open();
    dkQueueSubmitCommands(g_q, dkCmdBufFinishList(g_cb));
    dkQueueWaitIdle(g_q);
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

/* --- textures ------------------------------------------------------------------------------------------- */
struct GfxTex
{
    DkImage img;
    DkImageLayout layout;
    Mem mem;
    DkImageFormat dkfmt;
    uint8_t sw[4]; /* the view's swizzle: D3D's channels from the format's */
    int type, use, has_stencil, dropped;
    uint32_t fmt, w, h, levels, layers, block, bpp, id;
};

static uint32_t g_tex_ids;
static GfxTex* g_rt;
static GfxTex* g_ds;
static uint32_t g_rt_face, g_rt_level;

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
    t->type = type, t->use = use, t->fmt = d3dfmt, t->w = w, t->h = h, t->id = ++g_tex_ids;
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
    dkImageLayoutInitialize(&t->layout, &m);
    if (!heap_alloc(&g_img_heap, (uint32_t)dkImageLayoutGetSize(&t->layout), dkImageLayoutGetAlignment(&t->layout), &t->mem))
    {
        free(t);
        return NULL;
    }
    dkImageInitialize(&t->img, &t->layout, t->mem.mem, t->mem.off);
    ++g_tex_live, ++g_prof.tex_made;
    return t;
}

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (g_rt == t)
        g_rt = NULL, g_bound = 0;
    if (g_ds == t)
        g_ds = NULL, g_bound = 0;
    heap_free(&t->mem);
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

/* not yet without waiting (phase 3): zeros, as the occlusion probes' "not seen" */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || !dst || level >= t->levels)
        return;
    uint32_t row, rows = rect_rows(t, level_dim(t->w, level), level_dim(t->h, level), &row);
    (void)face;
    for (uint32_t y = 0; y < rows; ++y)
        memset((uint8_t*)dst + (size_t)y * pitch, 0, row < pitch ? row : pitch);
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
    heap_free(&b->mem);
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
    DkImageRect sr = { sx, sy, 0, w, h, 1 }, dr = { dx, dy, 0, w, h, 1 };
    dkCmdBufCopyImage(g_cb, &sv, &sr, &dv, &dr, 0);
    ++g_prof.copies;
}

/* --- drawing (milestone 3) ------------------------------------------------------------------------------ */
void gfx_draw(const GfxDraw* d)
{
    (void)d;
    ++g_prof.draws;
}

/* the characters: not drawn by the back end yet (0: the game draws them through gfx_draw) */
int gfx_chars_draw(const GfxCharFrame* f)
{
    (void)f;
    return 0;
}
int gfx_chars_env(void) { return 0; }

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

int gfx_init(void* sdl_window, int vsync)
{
    (void)sdl_window;
    if (g_up) /* a second device (the game's after the sign-in screen's, or a D3D Reset): the same one */
    {
        if (vsync != g_vsync)
            g_vsync = vsync, set_interval();
        return 1;
    }
    const char* prof = getenv("FFXI_PROFILE");
    gfx_profiling = prof && prof[0] && prof[0] != '0';
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = on_debug;
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
    g_cmd_mem = block(CMD_SLICE * CMD_SLOTS, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    g_stage_mem = block(STAGE_SLICE * CMD_SLOTS, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_dev);
    qm.flags = DkQueueFlags_Graphics;
    g_q = dkQueueCreate(&qm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, g_dev);
    cm.cbAddMem = cmd_more;
    g_cb = dkCmdBufCreate(&cm);
    if (!g_q || !g_cb || !g_cmd_mem || !g_stage_mem)
    {
        LOG("[dk] gfx: no queue, command memory or staging\n");
        return 0;
    }
    g_stage_cpu = (uint8_t*)dkMemBlockGetCpuAddr(g_stage_mem);
    g_stage_gpu = dkMemBlockGetGpuAddr(g_stage_mem);
    g_vsync = vsync;
    set_interval();
    g_up = 1;
    LOG("[dk] gfx: deko3d up: %d swapchain images of %dx%d (%u KB), %u KB of command memory, %u KB of staging; "
        "milestone 2: textures, buffers, targets, clears and copies, no draws yet\n",
        FB_NUM, FB_W, FB_H, fb_size * FB_NUM >> 10, CMD_SLICE * CMD_SLOTS >> 10, STAGE_SLICE * CMD_SLOTS >> 10);
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
    LOG("[dk] %.1f fps; a frame: %.0f draws, %.1f clears, %.1f copies, %.1f target changes, %.1f barriers, %.0f uploads "
        "(%.1f KB); %u syncs (%.1f ms), %u reads, %u command memory added; textures %u live (%u made), buffers %u live "
        "(%u made); memory: images %.1f MB in %u blocks + %.1f MB own, buffers %.1f MB in %u blocks + %.1f MB own; "
        "acquire %.2f ms, submit %.2f ms a frame\n",
        g_prof.frames / s, g_prof.draws / f, g_prof.clears / f, g_prof.copies / f, g_prof.targets / f,
        g_prof.barriers / f, g_prof.uploads / f, (double)g_prof.upload_bytes / f / 1024.0, g_prof.syncs,
        (double)g_prof.sync_ns * 1e-6, g_prof.reads, g_prof.cmd_more, g_tex_live, g_prof.tex_made, g_buf_live,
        g_prof.buf_made, g_img_heap.used / 1048576.0, g_img_heap.n, g_img_heap.own / 1048576.0,
        g_data_heap.used / 1048576.0, g_data_heap.n, g_data_heap.own / 1048576.0, (double)g_prof.acquire_ns / f * 1e-6,
        (double)g_prof.submit_ns / f * 1e-6);
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since = now;
}

void gfx_present(GfxTex* backbuffer)
{
    if (!g_up)
        return;
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
    nx_present_rect(rx, ry, rw, rh);
    cmd_open();
    uint64_t t0 = gfx_now_ns();
    int img = dkQueueAcquireImage(g_q, g_sc);
    uint64_t t1 = gfx_now_ns();
    DkImageView fbv;
    dkImageViewDefaults(&fbv, &g_fb[img]);
    work_kind(WK_DRAW);
    dkCmdBufBindRenderTarget(g_cb, &fbv, NULL);
    g_bound = 0;
    DkScissor sc = { 0, 0, FB_W, FB_H };
    dkCmdBufSetScissors(g_cb, 0, &sc, 1);
    int show = backbuffer && backbuffer->use == GFX_USE_RT && rw > 0 && rh > 0;
    if (show)
        dkCmdBufClearColorFloat(g_cb, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    else
    {
        /* no picture: a slow pulse of blue, to show frames are presented */
        float pulse = (float)(g_slot % 120) / 120.0f;
        pulse = pulse < 0.5f ? pulse * 2.0f : 2.0f - pulse * 2.0f;
        dkCmdBufClearColorFloat(g_cb, 0, DkColorMask_RGBA, 0.02f, 0.03f, 0.10f + 0.15f * pulse, 1.0f);
    }
    if (show)
    {
        work_kind(WK_XFER);
        DkImageView bv;
        tex_view(&bv, backbuffer, 0, 0);
        DkImageRect sr = { 0, 0, 0, backbuffer->w, backbuffer->h, 1 };
        DkImageRect dr = { (uint32_t)rx, (uint32_t)ry, 0, (uint32_t)rw, (uint32_t)rh, 1 };
        int same = rw == (int)backbuffer->w && rh == (int)backbuffer->h;
        dkCmdBufBlitImage(g_cb, &bv, &sr, &fbv, &dr, same ? DkBlitFlag_FilterNearest : DkBlitFlag_FilterLinear, 0);
    }
    dkCmdBufSignalFence(g_cb, &g_fence[g_cur], false);
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
void gfx_set_sync_pipelines(int on) { (void)on; }

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
