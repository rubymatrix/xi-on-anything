/* The graphics back end on Metal (macOS, R3.2): what gfx.h asks for, on one MTLDevice.
 *
 *   - Frames: one command buffer at a time, committed at Present (or when the CPU needs a result);
 *     up to three frames in flight, each with its own upload ring - vertex, index and uniform bytes
 *     are copied into the ring per draw, so the game may rewrite a buffer the moment a draw returns,
 *     as D3D lets it.
 *   - Render passes open on the first draw or clear after the targets change, and close when they
 *     change again, at Present, or for a blit. A clear of the whole target becomes the next pass's
 *     load action; a partial one draws a quad.
 *   - Pipelines come from the generated MSL (gfx_msl.c), cached by key; so are depth-stencil states
 *     and samplers.
 *   - Textures: sampled ones are shared storage, filled with replaceRegion when the GPU is done with
 *     them and through a blit otherwise; render targets and depth are private. Formats Metal has no
 *     match for (the 16-bit color ones) are widened to BGRA8 on upload.
 *
 * Built without ARC: every object here is retained and released by hand, and each entry point runs
 * in its own autorelease pool (the callers are guest threads with none). */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cachedir.h"
#include "gfx.h"
#include "gfx_fx.h"
#include "gfx_msl.h"
#include "gfx_scene.h"

#if __has_feature(objc_arc)
#error gfx_metal.m manages its references by hand: build it with -fno-objc-arc
#endif

#define FRAMES 3
#define GFX_PROBES 8     /* reads of one surface per frame that keep their own history */
#define GFX_READBACKS ((FRAMES + 1) * GFX_PROBES)
#define RING_CHUNK (8u << 20)

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

struct GfxTex
{
    id<MTLTexture> tex;  /* what is rendered to and uploaded into */
    id<MTLTexture> view; /* what is sampled: tex, or a swizzled view of it (only level 0 of a scene filter's chain) */
    id<MTLTexture> mipview; /* the whole chain of a large render target, once the scene filter filled it */
    int type, use, conv;
    uint32_t fmt, w, h, levels;
    uint32_t block;  /* bytes per 4x4 block for the compressed formats, else 0 */
    uint32_t texel;  /* bytes per texel in Metal's layout */
    uint64_t used;   /* the last frame serial that referenced it */
    int has_stencil, x8;
    id<MTLTexture> depth_seen; /* the depth its first level was last drawn with */
    id<MTLTexture> depth_world; /* the depth the scene's casters (its world) were last drawn with: what the scene
                                 * effects read - a later pass on it with another depth does not change it */
    uint32_t mips;   /* levels of tex: t->levels, or a whole chain for a large render target (the scene filter) */
    uint32_t filled; /* the levels uploaded so far, a bit each */
    uint64_t scene;  /* its mips hold its first level as it is now (the scene filter): 0 once it is drawn to again */
    /* asynchronous readbacks (gfx_tex_read_async): staging buffers and the frame each was recorded in */
    id<MTLBuffer> rb[GFX_READBACKS];
    uint64_t rb_serial[GFX_READBACKS];
    uint32_t rb_face[GFX_READBACKS], rb_level[GFX_READBACKS], rb_index[GFX_READBACKS];
    uint64_t rb_frame; /* the frame the reads below were counted in */
    uint32_t rb_count; /* reads of this surface so far in that frame */
};

typedef struct Chunk
{
    id<MTLBuffer> buf;
    uint32_t used, size;
} Chunk;

typedef struct Frame
{
    Chunk* chunks;
    uint32_t nchunks, cur;
} Frame;

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static CAMetalLayer* g_layer;
static SDL_MetalView g_view;
static SDL_Window* g_window;
static id<MTLCommandBuffer> g_cmd;
static id<MTLRenderCommandEncoder> g_enc;
/* What g_enc has bound, so a draw sets only what changed (the game's draws mostly repeat the last
 * one's state). Forgotten with each new encoder and after anything else sets state on it (clears).
 * The command buffer retains what was bound, so an address here cannot come back as another object. */
static struct
{
    id pipe, depth, vb[GFX_NSTREAMS + 1], ub, tex[8], samp[8]; /* vb: the streams, then the uniforms (4) */
    NSUInteger voff[GFX_NSTREAMS + 1], uoff;
    uint32_t stencil_ref;
    int32_t zbias;
    uint8_t cull, fill, valid, stencil_set; /* stencil_set: the reference is set only for stencil draws */
    MTLViewport vp;
    MTLScissorRect sc;
} g_bound;

static void bound_forget(void) { memset(&g_bound, 0, sizeof g_bound); }
static dispatch_semaphore_t g_frames_sem;
static Frame g_frames[FRAMES];
static uint32_t g_frame;              /* index into g_frames */
static uint64_t g_serial = 1;         /* the frame being recorded */
static _Atomic uint64_t g_completed;  /* the last frame the GPU finished */
static int g_frame_open;              /* the semaphore was taken for g_serial */
static uint32_t g_cmd_draws;          /* draws in the command buffer being recorded */
static _Atomic uint64_t g_gpu_ns;      /* GPU time of the committed command buffers (profile) */
/* GPU time per part of the frame (profile): timestamps at the boundaries of the sun maps' passes and
 * the effects' passes, read back when the frame completes. TS_AO1 ends the occlusion and shadow
 * passes (through the temporal one). */
enum { TS_SUN0, TS_SUN1, TS_FX0, TS_AO1, TS_FX1, TS_N };
static id<MTLCounterSampleBuffer> g_ts[FRAMES];
static int g_ts_ok = -1;               /* -1 not looked at, 0 the GPU cannot, 1 g_ts[] are there */
static uint8_t g_ts_started;           /* this frame: 1 << TS_SUN0, 1 << TS_FX0 once the first pass marked it */
static _Atomic uint64_t g_ts_sun_ns, g_ts_fx_ns, g_ts_ao_ns;
static double g_ts_scale;              /* ns per GPU timestamp tick */

static GfxTex* g_rt;
static uint32_t g_rt_face, g_rt_level;
static GfxTex* g_ds;
static uint32_t g_pending_clear; /* D3DCLEAR flags for the next pass's load actions */
static float g_clear_color[4], g_clear_z;
static uint32_t g_clear_stencil;
static id<MTLBuffer> g_dummy;
static id<MTLRenderPipelineState> g_present_pipe, g_present_cas_pipe, g_overlay_pipe;
/* the frame-rate overlay (g_fxs.fps): presents counted over half-second windows */
static double g_fps_since;
static uint32_t g_fps_frames;
static char g_fps_text[32] = "-- FPS";
static id<MTLSamplerState> g_present_samp;
static id<MTLTexture> g_scratch_depth;


/* --- small hash maps (key bytes -> object) ------------------------------------------------------------- */
typedef struct MapEnt
{
    uint64_t hash;
    void* key;
    size_t klen;
    id obj;
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

static MapEnt* map_find(Map* m, const void* key, size_t klen, uint64_t h)
{
    if (!m->cap)
        return NULL;
    for (uint32_t i = (uint32_t)h & (m->cap - 1);; i = (i + 1) & (m->cap - 1))
    {
        MapEnt* e = &m->e[i];
        if (!e->hash)
            return NULL;
        if (e->hash == h && e->klen == klen && !memcmp(e->key, key, klen))
            return e;
    }
}

static id map_get(Map* m, const void* key, size_t klen)
{
    MapEnt* e = map_find(m, key, klen, fnv(key, klen));
    return e ? e->obj : nil;
}

static void map_put(Map* m, const void* key, size_t klen, id obj) /* takes the reference */
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

static Map g_libs, g_pipes, g_depths, g_samplers, g_clear_pipes;

/* --- the frame profile ------------------------------------------------------------------------------------ */
int gfx_profiling;

typedef struct Prof
{
    uint64_t frames, draws, bytes, pipelines, front_ns, draw_ns, sem_ns, drawable_ns, present_ns, last_ns, since_ns;
    uint64_t skips[GFX_NSKIPS];
    uint64_t shim_ns, probe_ns;
} Prof;

static Prof g_prof;

uint64_t gfx_now_ns(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason) { if (reason >= 0 && reason < GFX_NSKIPS) g_prof.skips[reason]++; }

static pthread_t g_present_thread;

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
    double sem = (double)g_prof.sem_ns * ms, drw = (double)g_prof.drawable_ns * ms, pres = (double)g_prof.present_ns * ms;
    double shims = (double)g_prof.shim_ns * ms, probe = (double)g_prof.probe_ns * ms;
    if (shims > 0)
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game code %.2f + API calls %.2f (draws %.2f [encode %.2f], probe wait %.2f, "
            "present %.2f [drawable %.2f], other %.2f) | GPU %.2f ms | %.0f draws, %.0f KB up, %llu new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - shims, shims, front, enc, probe, pres, drw,
            shims - front - probe - pres, (double)atomic_exchange(&g_gpu_ns, 0) * ms, (double)g_prof.draws / f,
            (double)g_prof.bytes / f / 1024.0, (unsigned long long)g_prof.pipelines);
    else
    fprintf(stderr,
        "[gfx] %.1f fps: frame %.2f ms = game %.2f + d3d %.2f (encode %.2f) + present %.2f (gpu wait %.2f, drawable %.2f) | "
        "GPU %.2f ms | %.0f draws, %.0f KB up, %llu new pipelines\n",
        f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - front - pres, front, enc, pres, sem, drw,
        (double)atomic_exchange(&g_gpu_ns, 0) * ms,
        (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0, (unsigned long long)g_prof.pipelines);
    static const char* const WHY[GFX_NSKIPS] = { "no shader", "stream>=4", "no position", "no buffer data", "range past buffer",
        "no indices", "pipeline not ready", "no target" };
    int any = 0;
    for (int i = 0; i < GFX_NSKIPS; ++i)
        if (g_prof.skips[i])
            fprintf(stderr, "%s%s %llu", any++ ? ", " : "[gfx]   skipped draws (2 s): ", WHY[i], (unsigned long long)g_prof.skips[i]);
    if (any)
        fprintf(stderr, "\n");
    if (g_ts_ok > 0)
        fprintf(stderr, "[gfx]   GPU per frame: sun maps %.2f ms, effects %.2f ms (occlusion and shadows %.2f)\n",
            (double)atomic_exchange(&g_ts_sun_ns, 0) * ms, (double)atomic_exchange(&g_ts_fx_ns, 0) * ms,
            (double)atomic_exchange(&g_ts_ao_ns, 0) * ms);
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since_ns = now;
}
static uint32_t g_failures;

uint32_t gfx_failures(void) { return g_failures; }

/* --- frames and the upload ring ----------------------------------------------------------------------- */
static void frame_begin(void)
{
    if (g_frame_open)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    dispatch_semaphore_wait(g_frames_sem, DISPATCH_TIME_FOREVER);
    if (gfx_profiling)
        g_prof.sem_ns += gfx_now_ns() - t0;
    g_frame_open = 1;
    Frame* f = &g_frames[g_frame];
    for (uint32_t i = 0; i < f->nchunks; ++i)
        f->chunks[i].used = 0;
    f->cur = 0;
}

static id<MTLCommandBuffer> cmd(void)
{
    frame_begin();
    if (!g_cmd)
        g_cmd = [[g_queue commandBuffer] retain];
    return g_cmd;
}

/* n bytes of this frame's ring; the buffer and offset for binding */
static void* ring(size_t n, size_t align, id<MTLBuffer>* buf, NSUInteger* off)
{
    frame_begin();
    Frame* f = &g_frames[g_frame];
    for (;; f->cur++)
    {
        if (f->cur == f->nchunks)
        {
            uint32_t size = n + align > RING_CHUNK ? (uint32_t)(n + align) : RING_CHUNK;
            f->chunks = (Chunk*)realloc(f->chunks, (f->nchunks + 1) * sizeof(Chunk));
            f->chunks[f->nchunks].buf = [g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
            f->chunks[f->nchunks].size = size;
            f->chunks[f->nchunks].used = 0;
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
            return (uint8_t*)[c->buf contents] + at;
        }
    }
}

static void end_pass(void);
static int begin_pass(void);

/* closes the render pass, first opening one if a clear is still pending, so the clear happens
 * before whatever comes next (a blit, a readback, other targets) */
static void flush_pass(void)
{
    if (g_pending_clear)
        begin_pass();
    end_pass();
}

static void commit_cmd(int wait)
{
    if (gfx_profiling)
        [g_cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
            atomic_fetch_add(&g_gpu_ns, (uint64_t)((done.GPUEndTime - done.GPUStartTime) * 1e9));
        }];
    [g_cmd commit];
    if (wait)
        [g_cmd waitUntilCompleted];
    [g_cmd release];
    g_cmd = nil;
    g_cmd_draws = 0;
}

/* commits what is recorded; the frame stays open (its ring is still in use) */
static void submit(int wait)
{
    flush_pass();
    if (!g_cmd)
        return;
    commit_cmd(wait);
}

/* a chunk of the frame to the GPU (the render pass is already closed) */
static void commit_chunk(void)
{
    if (g_cmd)
        commit_cmd(0);
}

/* --- formats ---------------------------------------------------------------------------------------------- */
static MTLPixelFormat pixel_format(uint32_t fmt, int use, int* conv, uint32_t* block, uint32_t* texel, MTLTextureSwizzleChannels* sw,
    int* swizzled)
{
    *conv = CONV_NONE, *block = 0, *texel = 4, *swizzled = 0;
    *sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
    if (use == GFX_USE_DEPTH)
        return fmt == F_D16 ? MTLPixelFormatDepth16Unorm : MTLPixelFormatDepth32Float_Stencil8;
    switch (fmt)
    {
    case F_A8R8G8B8: return MTLPixelFormatBGRA8Unorm;
    case F_X8R8G8B8:
        sw->alpha = MTLTextureSwizzleOne, *swizzled = 1;
        return MTLPixelFormatBGRA8Unorm;
    case F_R5G6B5: *conv = CONV_565; return MTLPixelFormatBGRA8Unorm;
    case F_X1R5G5B5: *conv = CONV_X555; return MTLPixelFormatBGRA8Unorm;
    case F_A1R5G5B5: *conv = CONV_1555; return MTLPixelFormatBGRA8Unorm;
    case F_A4R4G4B4: *conv = CONV_4444; return MTLPixelFormatBGRA8Unorm;
    case F_A8: *texel = 1; return MTLPixelFormatA8Unorm;
    case F_L8:
        *texel = 1, *swizzled = 1;
        *sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne);
        return MTLPixelFormatR8Unorm;
    case F_A8L8:
        *texel = 2, *swizzled = 1;
        *sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen);
        return MTLPixelFormatRG8Unorm;
    case F_V8U8: *texel = 2; return MTLPixelFormatRG8Snorm;
    }
    if (fmt == FOURCC('D', 'X', 'T', '1'))
        return *block = 8, MTLPixelFormatBC1_RGBA;
    if (fmt == FOURCC('D', 'X', 'T', '2') || fmt == FOURCC('D', 'X', 'T', '3'))
        return *block = 16, MTLPixelFormatBC2_RGBA;
    if (fmt == FOURCC('D', 'X', 'T', '4') || fmt == FOURCC('D', 'X', 'T', '5'))
        return *block = 16, MTLPixelFormatBC3_RGBA;
    return MTLPixelFormatBGRA8Unorm;
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
struct GfxBuf
{
    id<MTLBuffer> b;
    uint32_t size;
    uint64_t used; /* the last frame serial that drew from it */
    uint64_t up_last, up_prev; /* the frames of its last two uploads (buf_volatile) */
};

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
    b->b = [g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
    return b;
}

static void sun_cache_forget(id<MTLBuffer> buf);

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    sun_cache_forget(b->b);
    [b->b release]; /* command buffers that draw from it hold their own references */
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
    if (b->used > atomic_load(&g_completed))
    {
        /* recorded or running work still reads the old contents: rename */
        [b->b release];
        b->b = [g_dev newBufferWithLength:b->size options:MTLResourceStorageModeShared];
    }
    memcpy([b->b contents], data, size);
}

/* --- textures ------------------------------------------------------------------------------------------------ */
GfxTex* gfx_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    @autoreleasepool
    {
        GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
        MTLTextureSwizzleChannels sw;
        int swizzled;
        MTLPixelFormat pf = pixel_format(fmt, use, &t->conv, &t->block, &t->texel, &sw, &swizzled);
        t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->levels = levels ? levels : 1;
        t->has_stencil = pf == MTLPixelFormatDepth32Float_Stencil8;
        t->x8 = fmt == F_X8R8G8B8;
        MTLTextureDescriptor* d = [[MTLTextureDescriptor alloc] init];
        d.textureType = type == GFX_TEX_CUBE ? MTLTextureTypeCube : MTLTextureType2D;
        d.pixelFormat = pf;
        d.width = t->w;
        d.height = type == GFX_TEX_CUBE ? t->w : t->h;
        t->mips = t->levels;
        /* a large color target (FFXI's background) gets a whole mip chain: the finished scene is made
         * smaller through it rather than one bilinear sample per screen pixel (gfx_scene_done) */
        if (use == GFX_USE_RT && type == GFX_TEX_2D && t->levels == 1 && t->w >= 1024 && t->h >= 1024)
            while ((t->w >> t->mips) || (t->h >> t->mips))
                t->mips++;
        d.mipmapLevelCount = t->mips;
        if (use == GFX_USE_SAMPLE)
        {
            d.storageMode = MTLStorageModeShared;
            d.usage = MTLTextureUsageShaderRead | (swizzled ? MTLTextureUsagePixelFormatView : 0);
        }
        else
        {
            d.storageMode = MTLStorageModePrivate;
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | (swizzled ? MTLTextureUsagePixelFormatView : 0);
        }
        t->tex = [g_dev newTextureWithDescriptor:d];
        [d release];
        if (!t->tex)
        {
            fprintf(stderr, "[recomp] gfx: texture %ux%u format %u failed\n", w, h, fmt);
            free(t);
            return NULL;
        }
        if (swizzled && use != GFX_USE_DEPTH)
            t->view = [t->tex newTextureViewWithPixelFormat:pf textureType:t->tex.textureType levels:NSMakeRange(0, t->levels)
                                                     slices:NSMakeRange(0, type == GFX_TEX_CUBE ? 6 : 1) swizzle:sw];
        else if (t->mips > t->levels)
        {
            /* the game's draws see the one level it made: the rest hold nothing until the scene
             * filter builds them, and a sampler with a mip filter would read them */
            t->view = [t->tex newTextureViewWithPixelFormat:pf textureType:MTLTextureType2D levels:NSMakeRange(0, 1)
                                                     slices:NSMakeRange(0, 1)];
            t->mipview = [t->tex retain];
        }
        else
            t->view = [t->tex retain];
        return t;
    }
}

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (g_rt == t)
        end_pass(), g_rt = NULL;
    if (g_ds == t)
        end_pass(), g_ds = NULL;
    [t->view release];
    [t->mipview release];
    [t->depth_seen release];
    [t->depth_world release];
    [t->tex release]; /* the command buffers that use it hold their own references */
    for (int i = 0; i < GFX_READBACKS; ++i)
        [t->rb[i] release];
    free(t);
}

static void level_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = t->w >> level ? t->w >> level : 1;
    *h = (t->type == GFX_TEX_CUBE ? t->w : t->h) >> level;
    if (!*h)
        *h = 1;
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels || !w || !h)
        return;
    if (level < 32)
        t->filled |= 1u << level;
    @autoreleasepool
    {
        uint32_t lw, lh;
        level_size(t, level, &lw, &lh);
        if (x >= lw || y >= lh)
            return;
        if (x + w > lw)
            w = lw - x;
        if (y + h > lh)
            h = lh - y;
        uint32_t rows = t->block ? (h + 3) / 4 : h;
        uint32_t row_bytes = t->block ? ((w + 3) / 4) * t->block : w * t->texel;
        /* the bytes in Metal's layout: as given, or widened (16-bit formats) - into the ring when they go
         * through it, else into a scratch kept between uploads */
        const uint8_t* data = (const uint8_t*)src;
        MTLRegion region = MTLRegionMake2D(x, y, w, h);
        int busy = t->used > atomic_load(&g_completed);
        if (t->use == GFX_USE_SAMPLE && !busy)
        {
            uint32_t data_pitch = pitch;
            if (t->conv)
            {
                static uint8_t* scratch;
                static size_t scratch_cap;
                size_t need = (size_t)row_bytes * rows;
                if (need > scratch_cap)
                {
                    free(scratch);
                    scratch_cap = need > 2 * scratch_cap ? need : 2 * scratch_cap;
                    scratch = (uint8_t*)malloc(scratch_cap);
                }
                for (uint32_t r = 0; r < rows; ++r)
                    convert_row(t->conv, data + (size_t)r * pitch, scratch + (size_t)r * row_bytes, w);
                data = scratch, data_pitch = row_bytes;
            }
            [t->tex replaceRegion:region mipmapLevel:level slice:face withBytes:data bytesPerRow:data_pitch bytesPerImage:0];
        }
        else
        {
            /* in use by recorded or running work, or private: through the ring, in order */
            id<MTLBuffer> buf;
            NSUInteger off;
            uint8_t* dst = (uint8_t*)ring((size_t)row_bytes * rows, 256, &buf, &off);
            for (uint32_t r = 0; r < rows; ++r)
                if (t->conv)
                    convert_row(t->conv, data + (size_t)r * pitch, dst + (size_t)r * row_bytes, w);
                else
                    memcpy(dst + (size_t)r * row_bytes, data + (size_t)r * pitch, row_bytes);
            flush_pass();
            id<MTLBlitCommandEncoder> blit = [cmd() blitCommandEncoder];
            [blit copyFromBuffer:buf sourceOffset:off sourceBytesPerRow:row_bytes sourceBytesPerImage:(NSUInteger)row_bytes * rows
                      sourceSize:MTLSizeMake(w, h, 1) toTexture:t->tex destinationSlice:face destinationLevel:level
               destinationOrigin:MTLOriginMake(x, y, 0)];
            [blit endEncoding];
            t->used = g_serial;
        }
    }
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    gfx_tex_upload_rect(t, face, level, 0, 0, w, h, src, pitch);
}

/* rows of a level read back in Metal's layout -> D3D's */
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

/* a blit of the level into buf, recorded with the frame's other work */
static void queue_readback(GfxTex* t, uint32_t face, uint32_t level, uint32_t w, uint32_t h, uint32_t row, id<MTLBuffer> buf)
{
    flush_pass();
    id<MTLBlitCommandEncoder> blit = [cmd() blitCommandEncoder];
    [blit copyFromTexture:t->tex sourceSlice:face sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:(NSUInteger)row * h];
    [blit endEncoding];
    t->used = g_serial;
}

void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || t->use == GFX_USE_DEPTH || t->block)
        return;
    @autoreleasepool
    {
        uint32_t w, h;
        level_size(t, level, &w, &h);
        uint32_t row = w * t->texel;
        id<MTLBuffer> buf = [g_dev newBufferWithLength:(NSUInteger)row * h options:MTLResourceStorageModeShared];
        queue_readback(t, face, level, w, h, row, buf);
        uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
        submit(1);
        if (gfx_profiling)
            g_prof.probe_ns += gfx_now_ns() - t0;
        copy_out(t, (const uint8_t*)[buf contents], w, h, row, dst, pitch);
        [buf release];
    }
}

/* A surface read several times a frame (the game reuses one 16x16 target for more than one
 * probe: copy a region, lock, read; copy another, lock, read) keeps each read's history apart:
 * the k-th read this frame gets the k-th read of the newest frame the GPU has finished, never
 * another probe's pixels. */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || t->use == GFX_USE_DEPTH || t->block)
        return;
    @autoreleasepool
    {
        uint32_t w, h;
        level_size(t, level, &w, &h);
        uint32_t row = w * t->texel;
        if (t->rb_frame != g_serial)
            t->rb_frame = g_serial, t->rb_count = 0;
        uint32_t index = t->rb_count++;
        if (index >= GFX_PROBES)
        {
            gfx_tex_read(t, face, level, dst, pitch); /* more reads a frame than we keep apart */
            return;
        }
        uint64_t done = atomic_load(&g_completed);
        int best = -1;
        for (int i = 0; i < GFX_READBACKS; ++i)
            if (t->rb[i] && t->rb_serial[i] && t->rb_serial[i] <= done && t->rb_index[i] == index && t->rb_face[i] == face &&
                t->rb_level[i] == level && (best < 0 || t->rb_serial[i] > t->rb_serial[best]))
                best = i;
        if (best < 0)
            gfx_tex_read(t, face, level, dst, pitch); /* nothing finished for this read yet: wait, once */
        else
            copy_out(t, (const uint8_t*)[t->rb[best] contents], w, h, row, dst, pitch);
        /* this read's copy for a later frame: a slot the GPU is done with, not the one just read */
        int slot = -1;
        for (int i = 0; i < GFX_READBACKS && slot < 0; ++i)
            if (i != best && (!t->rb_serial[i] || t->rb_serial[i] <= done))
                slot = i;
        if (slot < 0)
            return;
        if (!t->rb[slot] || [t->rb[slot] length] < (NSUInteger)row * h)
        {
            [t->rb[slot] release];
            t->rb[slot] = [g_dev newBufferWithLength:(NSUInteger)row * h options:MTLResourceStorageModeShared];
        }
        queue_readback(t, face, level, w, h, row, t->rb[slot]);
        t->rb_serial[slot] = g_serial, t->rb_face[slot] = face, t->rb_level[slot] = level, t->rb_index[slot] = index;
    }
}

void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || src->tex.pixelFormat != dst->tex.pixelFormat || !w || !h)
        return;
    @autoreleasepool
    {
        flush_pass();
        id<MTLBlitCommandEncoder> blit = [cmd() blitCommandEncoder];
        [blit copyFromTexture:src->tex sourceSlice:sface sourceLevel:slevel sourceOrigin:MTLOriginMake(sx, sy, 0)
                   sourceSize:MTLSizeMake(w, h, 1) toTexture:dst->tex destinationSlice:dface destinationLevel:dlevel
            destinationOrigin:MTLOriginMake(dx, dy, 0)];
        [blit endEncoding];
        src->used = dst->used = g_serial;
        if (!dlevel)
            dst->scene = 0;
    }
}

/* --- render passes --------------------------------------------------------------------------------------------- */
/* Draws recorded since the last commit. A frame goes to the GPU in chunks - the command buffer is
 * committed when a render pass ends with this many behind it - so the GPU works while the game
 * builds the rest, and a mid-frame readback (the game's per-frame probe) waits only for the tail. */
#define CHUNK_DRAWS 96
#define SPLIT_DRAWS 320
static void commit_chunk(void);

static void end_pass(void)
{
    if (g_enc)
    {
        [g_enc endEncoding];
        [g_enc release];
        g_enc = nil;
        if (g_cmd_draws >= CHUNK_DRAWS)
            commit_chunk();
    }
}

static void color_size(uint32_t* w, uint32_t* h)
{
    level_size(g_rt, g_rt_level, w, h);
}

/* the depth attachment for the current color target: the bound one, or a scratch one of the color
 * target's size when they differ (D3D lets depth be larger; Metal wants them equal) */
static id<MTLTexture> depth_attachment(void)
{
    if (!g_ds || !g_rt)
        return nil;
    uint32_t w, h;
    color_size(&w, &h);
    if (g_ds->w == w && g_ds->h == h)
        return g_ds->tex;
    if (!g_scratch_depth || g_scratch_depth.width != w || g_scratch_depth.height != h ||
        g_scratch_depth.pixelFormat != g_ds->tex.pixelFormat)
    {
        [g_scratch_depth release];
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:g_ds->tex.pixelFormat width:w height:h
                                                                                mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        g_scratch_depth = [g_dev newTextureWithDescriptor:d];
    }
    return g_scratch_depth;
}

static int begin_pass(void)
{
    if (g_enc)
        return 1;
    if (!g_rt)
        return 0;
    MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = g_rt->tex;
    p.colorAttachments[0].slice = g_rt_face;
    p.colorAttachments[0].level = g_rt_level;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (g_pending_clear & 1)
    {
        p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].clearColor = MTLClearColorMake(g_clear_color[0], g_clear_color[1], g_clear_color[2], g_clear_color[3]);
    }
    else
        p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    id<MTLTexture> depth = depth_attachment();
    if (depth)
    {
        p.depthAttachment.texture = depth;
        p.depthAttachment.storeAction = MTLStoreActionStore;
        p.depthAttachment.loadAction = (g_pending_clear & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
        p.depthAttachment.clearDepth = g_clear_z;
        if (g_ds->has_stencil)
        {
            p.stencilAttachment.texture = depth;
            p.stencilAttachment.storeAction = MTLStoreActionStore;
            p.stencilAttachment.loadAction = (g_pending_clear & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
            p.stencilAttachment.clearStencil = g_clear_stencil;
        }
        if (!g_rt_face && !g_rt_level && g_rt->depth_seen != depth)
        {
            [g_rt->depth_seen release];
            g_rt->depth_seen = [depth retain];
        }
    }
    g_pending_clear = 0;
    g_enc = [[cmd() renderCommandEncoderWithDescriptor:p] retain];
    bound_forget();
    /* D3D's front faces are clockwise on screen; CULL_CCW (the default) culls the back ones */
    [g_enc setFrontFacingWinding:MTLWindingClockwise];
    g_rt->used = g_serial;
    if (!g_rt_face && !g_rt_level)
        g_rt->scene = 0; /* drawn to: its mips are behind */
    if (g_ds)
        g_ds->used = g_serial;
    return 1;
}

void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (color == g_rt && face == g_rt_face && level == g_rt_level && depth == g_ds)
        return;
    @autoreleasepool
    {
        flush_pass(); /* a clear nothing drew after still happens */
    }
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
}

/* --- pipelines ------------------------------------------------------------------------------------------------- */
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
    uint32_t color, depth, stencil; /* MTLPixelFormat */
    uint8_t x8, pad[3];
} PipeKey;

static MTLBlendFactor blend_factor(uint32_t f, int x8)
{
    switch (f)
    {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return x8 ? MTLBlendFactorOne : MTLBlendFactorDestinationAlpha;
    case 8: return x8 ? MTLBlendFactorZero : MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    default: return MTLBlendFactorOne;
    }
}

static MTLBlendOperation blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static id<MTLLibrary> compile(const char* src)
{
    NSError* err = nil;
    NSString* s = [[NSString alloc] initWithUTF8String:src];
    MTLCompileOptions* o = [[MTLCompileOptions alloc] init];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:s options:o error:&err];
    [o release];
    [s release];
    if (!lib)
        __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED), fprintf(stderr, "[recomp] gfx: MSL compile failed: %s\n%s\n", err ? [[err localizedDescription] UTF8String] : "?", src);
    return lib;
}

/* --- pipelines, built off the game's thread ---------------------------------------------------------------
 * A pipeline for a key seen for the first time is built on a background queue; the draws that need
 * it are skipped until it is ready (a new effect may miss its first frames; the game never waits for
 * the Metal compiler). Every key built is recorded in the pipeline cache file, and at start-up the
 * recorded keys are built again in the background, so a second session has them before it needs
 * them. gfx_set_sync_pipelines(1) (the tests) builds in place and records nothing. */
#define PIPE_FAILED ((void*)1)
#define PIPE_MAGIC 0x314B5053u /* "SPK1" */

typedef struct PipeEntry
{
    void* _Atomic state; /* NULL while building, PIPE_FAILED, or the pipeline (retained) */
} PipeEntry;

typedef struct PipeJob
{
    PipeKey k;
    uint32_t *vs, *ps; /* copies of the shader tokens behind k.lib.vs.prog / fs.prog */
    uint32_t nvs, nps;
    PipeEntry* e;
    int record;
} PipeJob;

static int g_sync_pipelines;
static char g_pipe_cache[1024];
static dispatch_queue_t g_pipe_queue, g_pipe_file_queue;
static _Atomic uint32_t g_pipes_building;

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

/* The functions for a pair of shader keys, compiled once: pipelines that differ only in blending or
 * their attachments share them (the source is the keys' alone). Kept for the session, g_libs under
 * g_lib_lock: the pipelines are built on a concurrent queue. nil if it did not compile. */
static pthread_mutex_t g_lib_lock = PTHREAD_MUTEX_INITIALIZER;

static id<MTLLibrary> library(const LibKey* k, const uint32_t* vs, const uint32_t* ps)
{
    pthread_mutex_lock(&g_lib_lock);
    id<MTLLibrary> lib = map_get(&g_libs, k, sizeof *k);
    pthread_mutex_unlock(&g_lib_lock);
    if (lib)
        return lib != (id)PIPE_FAILED ? lib : nil;
    char* src = gfx_msl_generate(&k->vs, &k->fs, vs, ps);
    if (src)
    {
        lib = compile(src);
        free(src);
    }
    else
        __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_lib_lock);
    id had = map_get(&g_libs, k, sizeof *k);
    if (had) /* another job compiled it meanwhile */
    {
        [lib release];
        lib = had;
    }
    else
        map_put(&g_libs, k, sizeof *k, lib ? lib : (id)PIPE_FAILED);
    pthread_mutex_unlock(&g_lib_lock);
    return lib != (id)PIPE_FAILED ? lib : nil;
}

/* the pipeline for a key (retained), or nil */
static id<MTLRenderPipelineState> build_pipeline(const PipeKey* k, const uint32_t* vs, const uint32_t* ps)
{
    id<MTLLibrary> lib = library(&k->lib, vs, ps);
    if (!lib)
        return nil;
    id p = nil;
    @autoreleasepool
    {
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        id<MTLFunction> vf = [lib newFunctionWithName:@"vs_main"], ff = [lib newFunctionWithName:@"fs_main"];
        /* depth alone: nothing for the fragments to do (the bounce light's map is in colour: write_mask) */
        if (k->lib.vs.shadow && !alpha_tested(&k->lib.fs) && !k->pipe.write_mask)
            [ff release], ff = nil;
        if (k->lib.vs.shadow == 2) /* captured for ray tracing (rt_capture): the corners written, nothing drawn */
            [ff release], ff = nil, pd.rasterizationEnabled = NO;
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassUnspecified;
        MTLRenderPipelineColorAttachmentDescriptor* c = pd.colorAttachments[0];
        c.pixelFormat = (MTLPixelFormat)k->color;
        uint32_t wm = k->pipe.write_mask;
        c.writeMask = ((wm & 1) ? MTLColorWriteMaskRed : 0) | ((wm & 2) ? MTLColorWriteMaskGreen : 0) |
            ((wm & 4) ? MTLColorWriteMaskBlue : 0) | ((wm & 8) ? MTLColorWriteMaskAlpha : 0);
        if (k->pipe.blend)
        {
            uint32_t sf = k->pipe.src, df = k->pipe.dst;
            if (sf == 12) /* BOTHSRCALPHA */
                sf = 5, df = 6;
            else if (sf == 13) /* BOTHINVSRCALPHA */
                sf = 6, df = 5;
            c.blendingEnabled = YES;
            c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = blend_factor(sf, k->x8);
            c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = blend_factor(df, k->x8);
            c.rgbBlendOperation = c.alphaBlendOperation = blend_op(k->pipe.op);
        }
        pd.depthAttachmentPixelFormat = (MTLPixelFormat)k->depth;
        pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k->stencil;
        NSError* err = nil;
        p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p)
        {
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
            fprintf(stderr, "[recomp] gfx: pipeline failed: %s\n", err ? [[err localizedDescription] UTF8String] : "?");
        }
        [vf release];
        [ff release];
        [pd release];
    }
    return p;
}

/* one record of the cache file: magic, the key, then each shader's tokens (count first) */
static void record_job(const PipeJob* j)
{
    if (!g_pipe_cache[0])
        return;
    size_t n = 4 + 4 + sizeof(PipeKey) + 4 + 4u * j->nvs + 4 + 4u * j->nps;
    uint8_t* rec = (uint8_t*)malloc(n);
    uint8_t* w = rec;
    uint32_t v = PIPE_MAGIC;
    memcpy(w, &v, 4), w += 4;
    v = (uint32_t)sizeof(PipeKey);
    memcpy(w, &v, 4), w += 4;
    memcpy(w, &j->k, sizeof(PipeKey)), w += sizeof(PipeKey);
    memcpy(w, &j->nvs, 4), w += 4;
    if (j->nvs)
        memcpy(w, j->vs, 4u * j->nvs), w += 4u * j->nvs;
    memcpy(w, &j->nps, 4), w += 4;
    if (j->nps)
        memcpy(w, j->ps, 4u * j->nps);
    dispatch_async(g_pipe_file_queue, ^{
        FILE* f = fopen(g_pipe_cache, "ab");
        if (f)
        {
            fwrite(rec, 1, n, f);
            fclose(f);
        }
        free(rec);
    });
}

static void run_job(PipeJob* j)
{
    id p = build_pipeline(&j->k, j->vs, j->ps);
    atomic_store(&j->e->state, p ? (void*)p : PIPE_FAILED);
    if (p && j->record)
        record_job(j);
    free(j->vs);
    free(j->ps);
    free(j);
    atomic_fetch_sub(&g_pipes_building, 1);
}

static void queue_job(const PipeKey* k, const uint32_t* vs, uint32_t nvs, const uint32_t* ps, uint32_t nps, int record)
{
    if (map_get(&g_pipes, k, sizeof *k))
        return;
    PipeEntry* e = (PipeEntry*)calloc(1, sizeof *e);
    map_put(&g_pipes, k, sizeof *k, (id)e);
    PipeJob* j = (PipeJob*)calloc(1, sizeof *j);
    j->k = *k, j->e = e, j->record = record;
    if (nvs)
        j->vs = (uint32_t*)malloc(4u * nvs), memcpy(j->vs, vs, 4u * nvs), j->nvs = nvs;
    if (nps)
        j->ps = (uint32_t*)malloc(4u * nps), memcpy(j->ps, ps, 4u * nps), j->nps = nps;
    g_prof.pipelines++;
    atomic_fetch_add(&g_pipes_building, 1);
    if (g_sync_pipelines)
        run_job(j);
    else
        dispatch_async(g_pipe_queue, ^{ run_job(j); });
}

/* at start-up: the keys earlier sessions built, built again in the background */
static void prewarm_pipelines(void)
{
    char path[900];
    if (!cache_dir(path, sizeof path))
        return;
    snprintf(g_pipe_cache, sizeof g_pipe_cache, "%s/pipelines.v1", path);
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

/* the pipeline for a key, queued for building the first time (nil until built) */
static id<MTLRenderPipelineState> pipeline_for(const PipeKey* k, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
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
    return st && st != PIPE_FAILED ? (id)st : nil;
}

static id<MTLRenderPipelineState> pipeline(const GfxDraw* d, int* water)
{
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.lib.vs = d->vs, k.lib.fs = d->fs, k.pipe = d->pipe;
    k.lib.vs.water = *water == 1, k.lib.fs.water = (uint8_t)*water; /* water_mode */
    /* the world's lit draws lit per pixel (gfx_msl.c pixel_lit) */
    if (g_fxs.fx != 0.0f && g_fxs.light != 0.0f && d->vs.lighting && !d->vs.rhw && !d->vs.prog && !d->vs.flat)
        k.lib.vs.pixel = g_fxs.light >= 2.0f ? 2 : 1; /* 1: the sun per pixel, the game's torches per vertex */
    k.color = (uint32_t)g_rt->tex.pixelFormat;
    id<MTLTexture> depth = depth_attachment();
    k.depth = depth ? (uint32_t)depth.pixelFormat : 0;
    k.stencil = depth && g_ds->has_stencil ? k.depth : 0;
    k.x8 = (uint8_t)g_rt->x8;
    id<MTLRenderPipelineState> p = pipeline_for(&k, d->vs_tokens, d->ps_tokens);
    if (!p && k.lib.vs.pixel)
    {
        /* lit per pixel, still building: lit per vertex meanwhile (a new mix of the game's lights
         * would blink the object out for a few frames) */
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

/* --- the sun's shadow map: the scene's casters, drawn again from the sun (gfx_scene_done) --------------
 * Each opaque draw of the scene (GfxDraw.caster) is recorded as it is encoded - its pipeline key and
 * the buffers, uniforms and textures it was bound with, all alive until the frame ends - and when the
 * scene is done they are drawn again into a depth map from the sun: the same functions, with the
 * clip-space position they make taken through the camera's inverse into the sun's view (the shadow
 * key, gfx_msl_vs_return). The scene effects then look each pixel up in it. */
typedef struct Caster
{
    LibKey lib;
    const uint32_t *vs, *ps;
    id<MTLBuffer> vb[GFX_NSTREAMS], ub, ib; /* retained */
    NSUInteger voff[GFX_NSTREAMS], uoff, ioff;
    id<MTLTexture> tex[8]; /* retained; only for an alpha test */
    GfxSampler samp[8];
    MTLPrimitiveType prim;
    uint32_t n, vstart;
    uint8_t itype; /* 0 no indices, 2 or 4 bytes each */
    uint8_t fixed; /* every vertex and index from buffers the game keeps (the zone's): cached (sun_cache) */
    uint8_t keep;  /* not fixed, but a placed object (not a character): kept as a copy (sun_cache_update) */
    uint8_t has_pos;
    int32_t zbias;
    float clip0[4]; /* its first vertex in the camera's clip space (draw_clip0): where this copy stands */
} Caster;

static Caster* g_casters;
static uint32_t g_ncasters, g_casters_cap;

static void casters_clear(void)
{
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* c = &g_casters[i];
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            [c->vb[s] release];
        [c->ub release];
        [c->ib release];
        for (int t = 0; t < 8; ++t)
            [c->tex[t] release];
    }
    g_ncasters = 0;
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
            base[s] = (const uint8_t*)[d->buf[s]->b contents] + d->buf_off[s], have[s] = d->buf[s]->size > d->buf_off[s] ? d->buf[s]->size - d->buf_off[s] : 0;
        else if (d->data[s])
            base[s] = (const uint8_t*)d->data[s], have[s] = d->size[s];
    }
    return gfx_clip0(d, base, have, out);
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
    c->vs = d->vs_tokens, c->ps = d->ps_tokens;
    c->zbias = d->zbias;
    c->fixed = d->prim != GFX_TRIANGLEFAN && (!d->indices || d->ibuf);
    c->has_pos = (uint8_t)draw_clip0(d, c->clip0);
    return c;
}



static MTLCompareFunction compare(uint32_t f)
{
    switch (f)
    {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

static MTLStencilOperation stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

static id<MTLDepthStencilState> depth_state(const GfxDepthKey* k)
{
    id s = map_get(&g_depths, k, sizeof *k);
    if (s)
        return s;
    MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
    d.depthCompareFunction = k->zenable ? compare(k->zfunc) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = k->zenable && k->zwrite;
    if (k->stencil)
    {
        MTLStencilDescriptor* st = [[MTLStencilDescriptor alloc] init];
        st.stencilCompareFunction = compare(k->sfunc);
        st.stencilFailureOperation = stencil_op(k->sfail);
        st.depthFailureOperation = stencil_op(k->szfail);
        st.depthStencilPassOperation = stencil_op(k->spass);
        st.readMask = k->sread;
        st.writeMask = k->swrite;
        d.frontFaceStencil = st;
        d.backFaceStencil = st;
        [st release];
    }
    s = [g_dev newDepthStencilStateWithDescriptor:d];
    [d release];
    map_put(&g_depths, k, sizeof *k, s);
    return s;
}

static MTLSamplerAddressMode address(uint32_t a)
{
    switch (a)
    {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

static id<MTLSamplerState> sampler(const GfxSampler* k)
{
    id s = map_get(&g_samplers, k, sizeof *k);
    if (s)
        return s;
    MTLSamplerDescriptor* d = [[MTLSamplerDescriptor alloc] init];
    d.sAddressMode = address(k->addr_u);
    d.tAddressMode = address(k->addr_v);
    d.rAddressMode = address(k->addr_w);
    d.magFilter = k->mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = k->min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = k->mip == 0 ? MTLSamplerMipFilterNotMipmapped : k->mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    if ((k->min == 3 || k->mag == 3) && k->max_aniso > 1)
        d.maxAnisotropy = k->max_aniso > 16 ? 16 : k->max_aniso;
    d.lodMinClamp = k->max_level;
    if (k->lod_cap)
        d.lodMaxClamp = (float)(k->lod_cap - 1);
    uint32_t a = k->border >> 24, rgb = k->border & 0xFFFFFF;
    d.borderColor = a < 128 ? MTLSamplerBorderColorTransparentBlack
        : rgb >= 0x808080 ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
    s = [g_dev newSamplerStateWithDescriptor:d];
    [d release];
    map_put(&g_samplers, k, sizeof *k, s);
    return s;
}

/* --- drawing --------------------------------------------------------------------------------------------------- */
static void set_viewport(const uint32_t vp[6])
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
    MTLViewport v = { x, y, vw, vh, zmin, zmax };
    if (g_bound.valid && !memcmp(&v, &g_bound.vp, sizeof v))
        return;
    [g_enc setViewport:v];
    g_bound.vp = v;
}

/* GfxDraw.scissor, clamped to the target (Metal rejects a rectangle outside it); none: all of it */
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
        if (x1 < x0)
            x1 = x0;
        if (y1 < y0)
            y1 = y0;
    }
    MTLScissorRect r = { (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
    if (g_bound.valid && !memcmp(&r, &g_bound.sc, sizeof r))
        return;
    [g_enc setScissorRect:r];
    g_bound.sc = r;
}

/* a vertex stream's buffer: only its offset when the buffer is the one there */
static void bind_vb(id<MTLBuffer> b, NSUInteger off, int s)
{
    if (g_bound.vb[s] == b)
    {
        if (g_bound.voff[s] != off)
            [g_enc setVertexBufferOffset:off atIndex:(NSUInteger)s], g_bound.voff[s] = off;
        return;
    }
    [g_enc setVertexBuffer:b offset:off atIndex:(NSUInteger)s];
    g_bound.vb[s] = b, g_bound.voff[s] = off;
}

/* index count (Metal) for a D3D primitive count */
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

static MTLPrimitiveType metal_prim(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return MTLPrimitiveTypePoint;
    case GFX_LINELIST: return MTLPrimitiveTypeLine;
    case GFX_LINESTRIP: return MTLPrimitiveTypeLineStrip;
    case GFX_TRIANGLESTRIP: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

static void draw_encode(const GfxDraw* d);

/* The scene filter: a large render target drawn smaller onto a large target (FFXI's world onto the
 * back buffer - or the copy of it the game shows some frames, while the camera moves) is sampled
 * through its mips, made here from it as it is now. Every frame alike: when only some were, the fine
 * detail of the sky flickered between the two. */
static void scene_mips(const GfxDraw* d)
{
    if (g_fxs.fx == 0.0f || g_fxs.filter == 0.0f || !d->vs.rhw || !g_rt)
        return;
    NSUInteger tw = g_rt->tex.width, th = g_rt->tex.height;
    /* onto a large target only: not the sun flare's 16x16 occlusion probe, nor the game's 256x256 targets. In a
     * crowd the game draws its world into those some 40 times a frame, and rebuilding the world's mips for each
     * (13 levels of 4096x4096) kept the GPU busy all frame; the game never filtered them */
    if (tw < 512 || th < 512)
        return;
    for (int i = 0; i < 8; ++i)
    {
        GfxTex* t = d->tex[i];
        int wanted = d->fs.prog || i < d->fs.nstages ? d->fs.st[i].tex : 0;
        if (!wanted || !t || t == g_rt || !t->mipview || t->mips < 2 || t->scene)
            continue;
        if (t->tex.width <= tw && t->tex.height <= th)
            continue; /* not made smaller */
        flush_pass();
        id<MTLBlitCommandEncoder> b = [cmd() blitCommandEncoder];
        [b generateMipmapsForTexture:t->tex];
        [b endEncoding];
        t->scene = g_serial;
    }
}

void gfx_draw(const GfxDraw* d)
{
    if (!g_dev || !d->count)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    scene_mips(d);
    draw_encode(d);
    g_cmd_draws++;
    /* a long pass (the 3D scene) goes to the GPU in pieces too: end it here, commit, and the next
     * draw resumes it with its contents loaded */
    if (g_cmd_draws >= SPLIT_DRAWS && g_enc)
        end_pass();
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws++;
}

static int water_mode(const GfxDraw* d);
static int water_capture(void);
static void water_bind(void);

static void draw_encode(const GfxDraw* d)
{
    @autoreleasepool
    {
        int water = g_rt ? water_mode(d) : 0;
        if (water && !water_capture())
            water = 0;
        if (!begin_pass())
        {
            gfx_prof_skip(GFX_SKIP_NO_TARGET);
            return;
        }
        id<MTLRenderPipelineState> p = pipeline(d, &water);
        if (!p)
        {
            gfx_prof_skip(GFX_SKIP_PIPELINE); /* still building (or failed) */
            return;
        }
        if (g_bound.pipe != p)
            [g_enc setRenderPipelineState:p], g_bound.pipe = p;
        GfxDepthKey dk = d->depth;
        if (!depth_attachment())
            memset(&dk, 0, sizeof dk);
        id<MTLDepthStencilState> ds = depth_state(&dk);
        if (g_bound.depth != ds)
            [g_enc setDepthStencilState:ds], g_bound.depth = ds;
        if (dk.stencil && (!g_bound.stencil_set || g_bound.stencil_ref != d->stencil_ref))
            [g_enc setStencilReferenceValue:d->stencil_ref], g_bound.stencil_ref = d->stencil_ref, g_bound.stencil_set = 1;
        uint8_t cull = d->cull == 3 ? 2 : d->cull == 2 ? 1 : 0, fill = d->fill == 2;
        if (!g_bound.valid || g_bound.cull != cull)
            [g_enc setCullMode:cull == 2 ? MTLCullModeBack : cull == 1 ? MTLCullModeFront : MTLCullModeNone], g_bound.cull = cull;
        if (!g_bound.valid || g_bound.fill != fill)
            [g_enc setTriangleFillMode:fill ? MTLTriangleFillModeLines : MTLTriangleFillModeFill], g_bound.fill = fill;
        if (!g_bound.valid || g_bound.zbias != d->zbias)
            [g_enc setDepthBias:-(float)d->zbias slopeScale:-(float)d->zbias * 0.5f clamp:0], g_bound.zbias = d->zbias;
        set_viewport(d->vp);
        set_scissor(d->scissor);
        g_bound.valid = 1;

        id<MTLBuffer> buf;
        NSUInteger off;
        /* the uniforms the draw's functions read: the lights only when lit, the vertex shader's
         * constants only for a vertex shader, the pixel shader's only for a pixel shader (the ring
         * keeps room for the whole struct: that is what the functions are compiled against) */
        size_t need = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
        if (d->vs.prog)
            need = offsetof(GfxU, psc);
        if (d->fs.prog)
            need = sizeof(GfxU);
        void* u = ring(sizeof(GfxU), 256, &buf, &off);
        memcpy(u, &d->u, need);
        bind_vb(buf, off, 4);
        if (g_bound.ub != buf)
            [g_enc setFragmentBuffer:buf offset:off atIndex:4], g_bound.ub = buf, g_bound.uoff = off;
        else if (g_bound.uoff != off)
            [g_enc setFragmentBufferOffset:off atIndex:4], g_bound.uoff = off;
        if (d->caster && !g_rt_face && !g_rt_level)
        {
            id<MTLTexture> dw = depth_attachment();
            if (dw && g_rt->depth_world != dw)
                [g_rt->depth_world release], g_rt->depth_world = [dw retain];
        }
        Caster* rec = d->caster && g_fxs.fx != 0.0f && g_fxs.sun > 0.0f ? caster_new(d) : NULL;
        if (rec)
            rec->ub = [buf retain], rec->uoff = off;
        for (int s = 0; s < GFX_NSTREAMS; ++s)
        {
            if (d->buf[s])
            {
                bind_vb(d->buf[s]->b, d->buf_off[s], s);
                d->buf[s]->used = g_serial;
                if (rec)
                {
                    rec->vb[s] = [d->buf[s]->b retain], rec->voff[s] = d->buf_off[s];
                    if (buf_volatile(d->buf[s]))
                        rec->fixed = 0;
                }
            }
            else if (d->data[s] && d->size[s])
            {
                void* v = ring(d->size[s], 16, &buf, &off);
                memcpy(v, d->data[s], d->size[s]);
                bind_vb(buf, off, s);
                if (rec)
                    rec->vb[s] = [buf retain], rec->voff[s] = off, rec->fixed = 0;
            }
            else
            {
                bind_vb(g_dummy, 0, s);
                if (rec)
                    rec->vb[s] = [g_dummy retain];
            }
        }
        for (int i = 0; i < 8; ++i)
        {
            GfxTex* t = d->tex[i];
            int wanted = d->fs.prog || i < d->fs.nstages ? d->fs.st[i].tex : 0;
            if (!wanted || !t)
                continue;
            id<MTLTexture> view = t->view;
            GfxSampler sk = d->samp[i];
            if (g_fxs.fx != 0.0f)
            {
                /* the world's solid textures (not the interface's, not the sky's or effects' - which write
                 * no depth: the sky's clouds, mapped at steep angles near the horizon, flicker through
                 * an anisotropic filter as the camera turns), where every mip is there: trilinear and
                 * anisotropic, so ground and walls at a slant stay sharp and do not swim */
                if (!d->vs.rhw && d->depth.zwrite && !d->fs.st[i].projected && g_fxs.aniso > 1.0f && t->levels > 1 &&
                    t->levels < 32 &&
                    t->filled == (1u << t->levels) - 1 && sk.min >= 2)
                    sk.min = 3, sk.mip = 2, sk.max_aniso = (uint8_t)(g_fxs.aniso > 16.0f ? 16.0f : g_fxs.aniso);
                /* the finished scene made smaller (FFXI's background onto the back buffer): through
                 * its mips, anisotropic for a squeeze that differs across and down */
                else if (t->scene && t->mipview)
                    sk.min = 3, sk.mag = 2, sk.mip = 2, sk.max_aniso = 16, sk.max_level = 0, view = t->mipview;
            }
            if (g_bound.tex[i] != view)
                [g_enc setFragmentTexture:view atIndex:(NSUInteger)i], g_bound.tex[i] = view;
            id<MTLSamplerState> ss = sampler(&sk);
            if (g_bound.samp[i] != ss)
                [g_enc setFragmentSamplerState:ss atIndex:(NSUInteger)i], g_bound.samp[i] = ss;
            t->used = g_serial;
            if (rec) /* (an alpha test's, and the bounce light's map, which draws in colour) */
                rec->tex[i] = [view retain], rec->samp[i] = sk;
        }

        if (water)
            water_bind();
        uint32_t n = vertex_count(d->prim, d->count);
        MTLPrimitiveType mp = metal_prim(d->prim);
        if (rec)
            rec->prim = mp, rec->n = n, rec->vstart = d->vertex_start;
        if (d->prim == GFX_TRIANGLEFAN)
        {
            /* no fans in Metal: a list with the same vertices */
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
            [g_enc drawIndexedPrimitives:mp indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:buf indexBufferOffset:off];
            if (rec)
                rec->ib = [buf retain], rec->ioff = off, rec->itype = 4;
        }
        else if (d->ibuf)
        {
            d->ibuf->used = g_serial;
            if (rec)
            {
                rec->ib = [d->ibuf->b retain], rec->ioff = d->ibuf_off, rec->itype = (uint8_t)(d->index_size == 2 ? 2 : 4);
                if (buf_volatile(d->ibuf))
                    rec->fixed = 0;
            }
            [g_enc drawIndexedPrimitives:mp indexCount:n indexType:d->index_size == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                             indexBuffer:d->ibuf->b indexBufferOffset:d->ibuf_off];
        }
        else if (d->indices)
        {
            void* idx = ring((size_t)n * d->index_size, 16, &buf, &off);
            memcpy(idx, d->indices, (size_t)n * d->index_size);
            [g_enc drawIndexedPrimitives:mp indexCount:n indexType:d->index_size == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                             indexBuffer:buf indexBufferOffset:off];
            if (rec)
                rec->ib = [buf retain], rec->ioff = off, rec->itype = (uint8_t)(d->index_size == 2 ? 2 : 4);
        }
        else
            [g_enc drawPrimitives:mp vertexStart:d->vertex_start vertexCount:n];
    }
}

/* --- clears ---------------------------------------------------------------------------------------------------- */
static const char CLEAR_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct CU { float4 rect; float4 color; float z; };\n"
    "struct CO { float4 pos [[position]]; };\n"
    "vertex CO clear_vs(uint vid [[vertex_id]], constant CU& u [[buffer(0)]]) {\n"
    "  float2 c = float2((vid & 1) ? u.rect.z : u.rect.x, (vid & 2) ? u.rect.w : u.rect.y);\n"
    "  CO o; o.pos = float4(c, u.z, 1.0); return o;\n"
    "}\n"
    "fragment float4 clear_fs(constant CU& u [[buffer(0)]]) { return u.color; }\n"
    "struct PO { float4 pos [[position]]; float2 uv; };\n"
    "vertex PO present_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  PO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    /* the frame-rate overlay: a 5x7 bitmap font drawn per pixel, no texture */
    "struct OU { float4 rect; float scale; uint n; uint pad0, pad1; uint4 text[8]; };\n"
    "constant uchar FONT[17 * 7] = {\n"
    "  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,\n"
    "  0x1F,0x02,0x04,0x02,0x01,0x11,0x0E, 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,\n"
    "  0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, 0x1F,0x01,0x02,0x04,0x08,0x08,0x08, 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,\n"
    "  0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C, 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,\n"
    "  0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, 0x00,0x00,0x1A,0x15,0x15,0x11,0x11, 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E,\n"
    "  0x00,0x00,0x00,0x00,0x00,0x0C,0x0C, 0x00,0x00,0x00,0x00,0x00,0x00,0x00 };\n"
    "struct OO { float4 pos [[position]]; };\n"
    "vertex OO overlay_vs(uint vid [[vertex_id]], constant OU& u [[buffer(0)]], constant float2& size [[buffer(1)]]) {\n"
    "  float2 c = u.rect.xy + float2((vid & 1) ? u.rect.z : 0.0, (vid & 2) ? u.rect.w : 0.0);\n"
    "  OO o; o.pos = float4(c.x / size.x * 2.0 - 1.0, 1.0 - c.y / size.y * 2.0, 0, 1); return o;\n"
    "}\n"
    "fragment float4 overlay_fs(OO in [[stage_in]], constant OU& u [[buffer(0)]]) {\n"
    "  float2 p = (in.pos.xy - u.rect.xy) / u.scale - 2.0;\n"
    "  int cell = int(floor(p.x / 6.0)), gx = int(floor(p.x)) - cell * 6, gy = int(floor(p.y));\n"
    "  if (p.x >= 0.0 && cell < int(u.n) && gx < 5 && gy >= 0 && gy < 7) {\n"
    "    uint ch = u.text[cell >> 2][cell & 3];\n"
    "    if ((FONT[ch * 7 + uint(gy)] >> (4 - gx)) & 1) return float4(1.0, 0.85, 0.2, 1.0);\n"
    "  }\n"
    "  return float4(0, 0, 0, 0.55);\n"
    "}\n"
    "fragment float4 present_fs(PO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return float4(t.sample(s, in.uv).rgb, 1.0);\n"
    "}\n"
    /* the same, sharpened by k (0..1): contrast-adaptive, a negative lobe over the four neighbors
     * that shrinks where the neighborhood is already near black or white (no halos on hard edges) */
    "fragment float4 present_cas_fs(PO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],\n"
    "                               constant float& k [[buffer(0)]]) {\n"
    "  float2 tx = 1.0 / float2(t.get_width(), t.get_height());\n"
    "  float3 c = t.sample(s, in.uv).rgb;\n"
    "  float3 n = t.sample(s, in.uv - float2(0, tx.y)).rgb, so = t.sample(s, in.uv + float2(0, tx.y)).rgb;\n"
    "  float3 w = t.sample(s, in.uv - float2(tx.x, 0)).rgb, e = t.sample(s, in.uv + float2(tx.x, 0)).rgb;\n"
    "  float3 mn = min(c, min(min(n, so), min(w, e))), mx = max(c, max(max(n, so), max(w, e)));\n"
    "  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));\n"
    "  float3 lobe = -amp * mix(0.125, 0.2, saturate(k));\n"
    "  return float4(saturate((c + (n + so + w + e) * lobe) / (1.0 + 4.0 * lobe)), 1.0);\n"
    "}\n";

static id<MTLLibrary> g_util;

static id<MTLRenderPipelineState> clear_pipeline(uint32_t flags)
{
    uint32_t k[4] = { (uint32_t)g_rt->tex.pixelFormat, 0, 0, flags & 1 };
    id<MTLTexture> depth = depth_attachment();
    k[1] = depth ? (uint32_t)depth.pixelFormat : 0;
    k[2] = depth && g_ds->has_stencil ? k[1] : 0;
    id p = map_get(&g_clear_pipes, k, sizeof k);
    if (p)
        return p;
    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> vf = [g_util newFunctionWithName:@"clear_vs"], ff = [g_util newFunctionWithName:@"clear_fs"];
    pd.vertexFunction = vf;
    pd.fragmentFunction = ff;
    pd.colorAttachments[0].pixelFormat = (MTLPixelFormat)k[0];
    pd.colorAttachments[0].writeMask = (flags & 1) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    pd.depthAttachmentPixelFormat = (MTLPixelFormat)k[1];
    pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k[2];
    p = [g_dev newRenderPipelineStateWithDescriptor:pd error:NULL];
    [vf release];
    [ff release];
    [pd release];
    map_put(&g_clear_pipes, k, sizeof k, p);
    return p;
}

void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    if (!g_dev || !g_rt)
        return;
    if (!g_ds)
        flags &= 1;
    if (!flags)
        return;
    @autoreleasepool
    {
        float c[4] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f };
        uint32_t w, h;
        color_size(&w, &h);
        int whole = !nrects && vp[0] == 0 && vp[1] == 0 && vp[2] >= w && vp[3] >= h;
        if (whole)
        {
            /* the next pass starts cleared: close this one (what it drew to cleared attachments is
             * overwritten anyway) */
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
        /* the viewport, intersected with each rectangle, as a quad at depth z */
        int32_t vx0 = (int32_t)vp[0], vy0 = (int32_t)vp[1], vx1 = vx0 + (int32_t)vp[2], vy1 = vy0 + (int32_t)vp[3];
        int32_t whole_rect[4] = { vx0, vy0, vx1, vy1 };
        if (!nrects)
            rects = whole_rect, nrects = 1;
        [g_enc setRenderPipelineState:clear_pipeline(flags)];
        GfxDepthKey dk;
        memset(&dk, 0, sizeof dk);
        dk.zenable = (flags & 2) != 0, dk.zwrite = 1, dk.zfunc = 8;
        if (flags & 4)
            dk.stencil = 1, dk.sfunc = 8, dk.sfail = dk.szfail = dk.spass = 3, dk.sread = dk.swrite = 0xFF;
        [g_enc setDepthStencilState:depth_state(&dk)];
        [g_enc setStencilReferenceValue:stencil];
        [g_enc setCullMode:MTLCullModeNone];
        [g_enc setTriangleFillMode:MTLTriangleFillModeFill];
        [g_enc setDepthBias:0 slopeScale:0 clamp:0];
        [g_enc setViewport:(MTLViewport){ 0, 0, w, h, 0, 1 }];
        for (uint32_t i = 0; i < nrects; ++i)
        {
            int32_t x0 = rects[4 * i] > vx0 ? rects[4 * i] : vx0, y0 = rects[4 * i + 1] > vy0 ? rects[4 * i + 1] : vy0;
            int32_t x1 = rects[4 * i + 2] < vx1 ? rects[4 * i + 2] : vx1, y1 = rects[4 * i + 3] < vy1 ? rects[4 * i + 3] : vy1;
            if (x1 <= x0 || y1 <= y0)
                continue;
            struct
            {
                float rect[4], color[4], z, pad[3];
            } cu = { { x0 * 2.0f / w - 1, 1 - y0 * 2.0f / h, x1 * 2.0f / w - 1, 1 - y1 * 2.0f / h }, { c[0], c[1], c[2], c[3] }, z, { 0 } };
            [g_enc setVertexBytes:&cu length:sizeof cu atIndex:0];
            [g_enc setFragmentBytes:&cu length:sizeof cu atIndex:0];
            [g_enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        bound_forget(); /* its pipeline, depth state, viewport and buffer 0 in place of the draws' */
    }
}

/* --- scene effects (gfx_scene_done) -----------------------------------------------------------------------------
 * On the finished 3D scene, in place, when fx is on:
 *   - ambient occlusion: view-space positions rebuilt from the scene's depth and projection, a spiral
 *     of samples around each pixel (Scalable Ambient Obscurance, simplified) at about 1000 pixels
 *     across, then a depth-aware blur across and down;
 *   - sun shadows, in the same pass and blur: each pixel looked up in the sun's shadow map (the
 *     scene's casters drawn again from the sun, sun_map), and a short ray toward the sun through the
 *     depth buffer for contact shadows where things meet the ground; by day;
 *   - height fog: the density falls off with world height (up from the view matrix), integrated along
 *     each view ray, so low ground mists over while hills stand clear; lit by the sun when looking
 *     toward it (Henyey-Greenstein scattering);
 *   - bloom: the bright parts, at a quarter and an eighth of the scene, blurred and added back;
 *   - god rays: the sky's bright pixels around the sun, blurred along lines toward the sun's place
 *     on screen (the sun is the scene's directional light);
 *   - a color grade (saturation, a contrast curve).
 * Then the scene gets mips, and the draw that makes it smaller for the back buffer samples it
 * through them (draw_encode): FFXI's background is larger than the screen, and one bilinear sample
 * per screen pixel skips rows of it - the edges crawl as the camera moves.
 *
 * Settings: FFXI_FX=1 and FFXI_FX_<KEY> (the table in gfx_fx.c), then while the game runs
 * FFXI_FX_FILE (default fx.txt in the cache folder, cachedir.h), lines of key=value. debug shows one part
 * alone: 1 occlusion, 2 fog, 3 bloom, 4 god rays, 5 sun shadows (map and contact). light = 1 lights the world's
 * lit draws per pixel rather than per vertex (gfx_msl.c). */
static const char FX_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct FxU {\n"
    "  float4 proj;   // P00, P11, P20, P21\n"
    "  float4 zp;     // P22, P32, viewport MinZ, MaxZ\n"
    "  float4 vp;     // the scene viewport in target pixels: x, y, width, height\n"
    "  float4 size;   // target width, height; occlusion width, height\n"
    "  float4 ao;     // radius, strength, bias, largest radius in pixels\n"
    "  float4 grade;  // strength, saturation, contrast, debug\n"
    "  float4 hand;   // P23: 1 for a left-handed projection, -1 for a right-handed one\n"
    "  float4 up;     // world up in view space: height above the camera = dot(P, up.xyz)\n"
    "  float4 sun;    // view space, toward the light; w = 1 when the scene has one\n"
    "  float4 suncol; // its color\n"
    "  float4 sunuv;  // its place in the viewport (0..1), z = how much of it shows (0 behind the camera)\n"
    "  float4 fogc;   // fog color, a = density at the camera's height\n"
    "  float4 fogp;   // falloff with height, most fog, sun glow, its forward scattering (g)\n"
    "  float4 bloom;  // threshold, strength, knee\n"
    "  float4 rays;   // strength, decay, length\n"
    "  float4 shadow; // contact shadows: strength (0: none, or night), ray length, thickness, how far out\n"
    "  float4x4 lmat; // view space to the sun's map: x, y -1..1 (y up), z 0..1\n"
    "  float4 smap;   // the map: strength (0: none), a texel in world units, depth bias, penumbra (map width per depth unit)\n"
    "  float4 smap2;  // depth units per map width, sun_face, sun_min, world units per depth unit\n"
    "  float4x4 reproj; // view space to the frame before's clip space\n"
    "  float4 hist;   // 1 when the frame before is there to blend with, the pattern's turn this frame, its weight\n"
    "  float4x4 lmatn; // the near cascade: view space to its map\n"
    "  float4 smapn;  // its texel in world units, depth bias, penumbra, slope\n"
    "  float4 smapn2; // its depth units, 1 when it is there, 1 for hard edges (sun_soft 0)\n"
    "  float4 aop;    // the occlusion's taps this frame\n"
    "  float4x4 gimat; // the bounce light: view space to its map\n"
    "  float4x4 giinv; // its map back to view space\n"
    "  float4 gi;     // its strength (0: none), its reach in the map's uv and in world units, the level read\n"
    "  float4 gip;    // 1 when its frame before is there, that one's weight, its samples, how far it reaches\n"
    "};\n"
    "struct FO { float4 pos [[position]]; float2 uv; };\n"
    "vertex FO fx_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  FO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "constant float3 LUMA = float3(0.2126, 0.7152, 0.0722);\n"
    /* view-space z of a depth value (clip w = z * P23, so z is negative in front of a right-handed
     * camera); 0 for the far plane (the sky, cleared depth) */
    "static float view_z(constant FxU& u, float d) {\n"
    "  d = (d - u.zp.z) / max(u.zp.w - u.zp.z, 1e-6);\n"
    "  float z = u.zp.y / (d * u.hand.x - u.zp.x);\n"
    "  return d >= 0.999999 || !(z * u.hand.x > 0.0) ? 0.0 : z;\n"
    "}\n"
    /* px a pixel's centre in the target; every draw went through the position fixup (D3D's pixel
     * centres onto Metal's: half a pixel right and down), so the point it shows is the one half a
     * pixel up and left of it. Without that, a floor seen at a slant came back below itself - by a
     * few centimetres a few units out, more farther - and fell into its own shadow. */
    "static float3 view_pos(constant FxU& u, float2 px, float z) {\n"
    "  px -= 0.5;\n"
    "  float2 ndc = float2((px.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (px.y - u.vp.y) / u.vp.w * 2.0);\n"
    "  float w = z * u.hand.x;\n"
    "  return float3((ndc.x * w - u.proj.z * z) / u.proj.x, (ndc.y * w - u.proj.w * z) / u.proj.y, z);\n"
    "}\n"
    "static float3 pos_at(constant FxU& u, depth2d<float> dt, float2 px) {\n"
    "  px = clamp(px, u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n"
    "  px = floor(px) + 0.5;\n"
    "  return view_pos(u, px, view_z(u, dt.read(uint2(px))));\n"
    "}\n"
    /* The occlusion's depth (after McGuire, Mara and Luebke, Scalable Ambient Obscurance, 2012): the
     * view z of the scene pixel each occlusion texel stands on (the one fx_ao takes as its centre; 0
     * for the sky), in an R32F texture of the occlusion's size, with three smaller levels below it.
     * A far tap reads a small level, which stays in the cache; reading the whole-size depth buffer
     * at taps hundreds of pixels apart made the pass bound by memory. */
    "fragment float4 fx_linz(FO in [[stage_in]], constant FxU& u [[buffer(0)]], depth2d<float> dt [[texture(0)]]) {\n"
    "  float2 px = floor(u.vp.xy + in.uv * u.vp.zw) + 0.5;\n"
    "  return float4(view_z(u, dt.read(uint2(px))), 0.0, 0.0, 0.0);\n"
    "}\n"
    /* each texel one of the 2x2 above it, picked on a rotated grid: never their average, which across
     * a silhouette would make a surface that is not there */
    "fragment float4 fx_zmip(FO in [[stage_in]], texture2d<float> z [[texture(0)]]) {\n"
    "  int2 p = int2(in.pos.xy), hi = int2(z.get_width(), z.get_height()) - 1;\n"
    "  return z.read(uint2(min(p * 2 + int2(p.y & 1, p.x & 1), hi)));\n"
    "}\n"
    /* the view position at screen pixel q, r pixels from the tap's centre: its z from the level whose
     * texels are about r / 8 across */
    "static float3 pos_lz(constant FxU& u, texture2d<float> lz, float2 q, float r) {\n"
    "  float2 t = (q - u.vp.xy) * u.size.zw / u.vp.zw;\n"
    "  uint lv = uint(clamp(int(floor(log2(max(r * u.size.z / u.vp.z, 1.0)))) - 3, 0, 3));\n"
    "  uint2 p = min(uint2(max(t, 0.0)) >> lv, uint2(lz.get_width(lv), lz.get_height(lv)) - 1);\n"
    "  return view_pos(u, q, lz.read(p, lv).r);\n"
    "}\n"
    /* in the sun (1) or not (0): a ray from P toward the sun, stepped through the depth buffer; a
     * surface in front of it (by less than the thickness: what is far nearer the camera hides the
     * ray, it does not block it) shades P. Only on surfaces that face the sun (the rest are dark from
     * their own lighting), and fading out with distance, where the steps grow coarse. */
    "static float sun_shadow(constant FxU& u, depth2d<float> dt, float3 P, float3 N, float dist, float k) {\n"
    "  float nl = dot(N, u.sun.xyz);\n"
    "  float fade = smoothstep(0.0, 0.15, nl) * (1.0 - smoothstep(0.6 * u.shadow.w, u.shadow.w, dist));\n"
    "  if (fade <= 0.0) return 1.0;\n"
    "  const int NS = 16;\n"
    "  float3 O = P + N * (0.01 * dist);\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float a = (float(i) + k) / float(NS);\n"
    "    float3 R = O + u.sun.xyz * (u.shadow.y * a * a);\n"
    "    float rd = R.z * u.hand.x;\n"
    "    if (rd <= 0.05) break;\n"
    "    float2 ndc = float2(R.x * u.proj.x + R.z * u.proj.z, R.y * u.proj.y + R.z * u.proj.w) / rd;\n"
    "    float2 q = u.vp.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;\n"
    "    if (any(q < u.vp.xy) || any(q + 0.5 >= u.vp.xy + u.vp.zw)) break;\n" /* the pixel read within the viewport, not just q */
    "    float sd = view_z(u, dt.read(uint2(q + 0.5))) * u.hand.x;\n" /* where the fixup drew it */
    "    float in_front = rd - sd;\n"
    /* weaker the farther along the hit: no hard edge where the ray ends */
    "    if (sd > 0.0 && in_front > 0.005 * rd + 0.02 && in_front < u.shadow.z + 0.01 * rd) return 1.0 - fade * (1.0 - a * a);\n"
    "  }\n"
    "  return 1.0;\n"
    "}\n"
    /* in the sun (1) or not (0) by the shadow map, softer the farther the shadow falls from what casts
     * it (percentage-closer soft shadows): the casters' average depth around the point sets how wide
     * the filter is; sixteen samples on a disk turned by the pixel's step of the 4x4 pattern, which
     * the blur then averages. P is moved off its surface by a texel and more with distance (no acne).
     * Surfaces turned away from the sun shade gently by their angle alone - the map's own edges on
     * them are the facets of low-polygon rock. */
    "constant float2 DISK[16] = { float2(-0.94, -0.40), float2(0.95, -0.77), float2(-0.09, -0.93), float2(0.34, 0.29),\n"
    "  float2(-0.92, 0.46), float2(-0.82, -0.88), float2(-0.38, 0.28), float2(0.97, 0.76), float2(0.44, -0.98),\n"
    "  float2(0.54, -0.47), float2(-0.26, -0.42), float2(-0.42, 0.87), float2(0.31, 0.92), float2(0.79, 0.19),\n"
    "  float2(-0.03, 0.04), float2(0.15, -0.33) };\n"
    /* one cascade: p = its texel (world units), depth bias, penumbra (map width per depth unit),
     * slope (depth units per map width); du its depth units; edge how far inside it P is (0 out) */
    "static float sun_look(depth2d<float> sm, sampler cmp, float4x4 lm, float4 p, float du, float minw, float3 P,\n"
    "                      float3 N, float dist, float k, bool hard, thread float& edge) {\n"
    "  float3 Q = P + N * (1.5 * p.x + 0.002 * dist);\n"
    "  float4 lc = lm * float4(Q, 1.0);\n"
    "  float2 uv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);\n"
    "  float2 e = abs(lc.xy);\n"
    "  edge = lc.z >= 1.0 ? 0.0 : 1.0 - smoothstep(0.8, 0.95, max(e.x, e.y));\n"
    "  if (edge <= 0.0) return 1.0;\n"
    "  float z = lc.z - p.y, sz = float(sm.get_width()), tx = 1.0 / sz;\n"
    "  float a = k * 6.2831853, ca = cos(a), sa = sin(a);\n"
    "  float2x2 rot = float2x2(float2(ca, sa), float2(-sa, ca));\n"
    "  float bs = 0.0, bn = 0.0;\n"
    "  for (int i = 0; i < 16; ++i) {\n"
    "    float2 q = clamp((uv + DISK[i] * (32.0 * tx)) * sz, 0.0, sz - 1.0);\n"
    "    float d = sm.read(uint2(q));\n"
    "    if (d < z) bs += d, bn += 1.0;\n"
    "  }\n"
    "  if (bn == 0.0) return 1.0;\n"
    /* the penumbra: wider the farther the caster, never narrower than 2 cm (no hard pixel edge) -
     * unless sun_soft is 0, which asks for the hard edge: half a texel */
    "  float pen = clamp((z - bs / bn) * p.z, hard ? 0.5 * tx : max(1.5 * tx, 0.02 / (p.x * sz)), 32.0 * tx);\n"
    /* casters nearer the surface than sun_min are its own neighbouring faces (a ledge of the same
     * rock): no shadow from them. sun_min 0 keeps them all (smoothstep's edges must differ) */
    "  float near = minw > 0.0 ? smoothstep(0.5 * minw, 1.5 * minw, (z - bs / bn) * du) : 1.0;\n"
    /* a wider filter reaches farther across the surface, to points of it nearer the sun: the bias
     * grows with it (a slope of one) */
    "  float zc = z - pen * p.w, s = 0.0;\n"
    "  for (int i = 0; i < 16; ++i) s += sm.sample_compare(cmp, uv + rot * DISK[i] * pen, zc);\n"
    "  return mix(1.0, s / 16.0, near);\n"
    "}\n"
    /* in the sun (1) or not (0) by the maps: the near cascade where P is in it, the far one beyond,
     * blended across the near one's edge */
    "static float sun_map(constant FxU& u, depth2d<float> sm, depth2d<float> smn, sampler cmp, float3 P, float3 N,\n"
    "                     float dist, float k) {\n"
    "  float nl = dot(N, u.sun.xyz);\n"
    /* turned from the sun: shaded by the angle only as much as sun_face asks (FFXI's own lighting
     * shades its rock already; per face, low-polygon rock turns into a patchwork) */
    "  float face = mix(1.0 - 0.6 * u.smap2.y, 1.0, smoothstep(-0.3, 0.25, nl)), use = smoothstep(-0.05, 0.15, nl);\n"
    /* turned from the sun: looked up too, shaded only by casters a unit or more away (gfx_hlsl.c) */
    "  float mw = mix(max(u.smap2.z, 1.0), u.smap2.z, use);\n"
    "  float en = 0.0, ef = 0.0, s = 1.0;\n"
    "  if (u.smapn2.y > 0.0)\n"
    "    s = sun_look(smn, cmp, u.lmatn, u.smapn, u.smapn2.x, mw, P, N, dist, k, u.smapn2.z > 0.0, en);\n"
    "  if (en < 1.0) {\n"
    "    float sf = sun_look(sm, cmp, u.lmat, float4(u.smap.yzw, u.smap2.x), u.smap2.w, mw, P, N, dist, k, u.smapn2.z > 0.0, ef);\n"
    "    s = mix(sf, s, en);\n"
    "  }\n"
    "  return min(mix(1.0, s, max(en, ef)), face);\n" /* the face's shade with a map or without */
    "}\n"
    /* occlusion in x (1 open, 0 closed), distance in y (0: sky), the sun by the map in z and by
     * contact in w (1 lit, 0 shaded) */
    "fragment float4 fx_ao(FO in [[stage_in]], constant FxU& u [[buffer(0)]], depth2d<float> dt [[texture(0)]],\n"
    "                      depth2d<float> sm [[texture(1)]], depth2d<float> smn [[texture(2)]], texture2d<float> lz [[texture(3)]],\n"
    "                      sampler cmp [[sampler(1)]]) {\n"
    "  float2 px = floor(u.vp.xy + in.uv * u.vp.zw) + 0.5;\n"
    "  float3 P = pos_at(u, dt, px);\n"
    "  float dist = P.z * u.hand.x;\n"
    "  if (dist <= 0.0) return float4(1.0, 0.0, 1.0, 1.0);\n"
    /* the surface normal from the neighbors on the side nearer in depth (no smearing across edges) */
    "  float3 r = pos_at(u, dt, px + float2(1, 0)) - P, l = P - pos_at(u, dt, px - float2(1, 0));\n"
    "  float3 d = pos_at(u, dt, px + float2(0, 1)) - P, t = P - pos_at(u, dt, px - float2(0, 1));\n"
    /* at the viewport's edge one side is the pixel itself (pos_at clamps to it): zero, its cross a NaN */
    "  float3 dx = (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0 ? r : l;\n"
    "  float3 dy = (abs(d.z) < abs(t.z) && dot(d, d) > 0.0) || dot(t, t) == 0.0 ? d : t;\n"
    "  float3 nc = cross(dx, dy);\n"
    "  float3 N = dot(nc, nc) > 1e-24 ? normalize(nc) : -normalize(P);\n"
    "  if (dot(N, P) > 0.0) N = -N;\n"
    "  const uchar BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n"
    "  int2 cell = int2(in.pos.xy) & 3;\n"
    "  float k = fract((float(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0 + u.hist.y);\n"
    "  float sh = u.shadow.x > 0.0 && u.sun.w > 0.0 ? sun_shadow(u, dt, P, N, dist, k) : 1.0;\n"
    "  float mp = u.smap.x > 0.0 && u.sun.w > 0.0 ? sun_map(u, sm, smn, cmp, P, N, dist, k) : 1.0;\n"
    "  float rad = u.ao.x, rpx = min(rad * u.proj.y * 0.5 * u.vp.w / dist, u.ao.w);\n"
    "  if (u.ao.y <= 0.0 || rpx < 2.0) return float4(1.0, dist, mp, sh);\n"
    /* the spiral turned and scaled by one of 16 steps, a 4x4 ordered pattern over the pixels, the
     * same every frame; the blur averages exactly one 4x4 block (fx_blur), so each pixel ends up with
     * all 16 - even, and with no grain left to crawl as the camera moves. One pattern at every pixel
     * instead copies each occluder at the pattern's offsets: streaks and halos around characters.
     * The pattern also turns each frame, and the temporal pass averages the frames: a few taps a
     * frame (ao_quality) do what twenty did. */
    "  const int NS = max(int(u.aop.x), 1);\n"
    "  float sum = 0.0;\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float a = (float(i) + k) / float(NS);\n"
    "    float ang = float(i) * 2.3999632 + k * 6.2831853; /* the golden angle: an even spiral */\n"
    "    float2 q = px + float2(cos(ang), sin(ang)) * (a * rpx);\n"
    /* off the screen: nothing known there (clamped to the edge, the edge's own pixels would shade
     * it, and move with the camera) */
    "    if (any(q < u.vp.xy) || any(q >= u.vp.xy + u.vp.zw)) continue;\n"
    "    float3 Q = pos_lz(u, lz, q, a * rpx);\n"
    /* a surface far nearer the camera floats in front of this one (a leg before the floor or the
     * other leg): it hides it, it does not shade it */
    "    if (Q.z == 0.0 || dist - Q.z * u.hand.x > 0.5 * rad) continue;\n"
    "    float3 v = Q - P;\n"
    "    float vv = dot(v, v), vn = dot(v, N);\n"
    "    float q2 = vv / (rad * rad), fall = saturate(1.0 - q2 * q2);\n"
    "    sum += fall * max(vn * rsqrt(vv + 1e-6) - u.ao.z, 0.0);\n"
    "  }\n"
    /* none on surfaces seen edge-on: there the normal from depth is unreliable, and the surface
     * shades itself in bands along every silhouette */
    "  float facing = smoothstep(0.1, 0.4, dot(N, -P) / dist);\n"
    "  return float4(saturate(1.0 - 3.0 * facing * sum / float(NS)), dist, mp, sh);\n"
    "}\n"
    /* the occlusion (x) and sun (y) at a scene pixel from the four nearest occlusion texels, each
     * weighted by how near its distance is to this pixel's: an edge's occlusion stays on its own
     * side, however the low-resolution grid falls on it */
    "static float3 ao_at(constant FxU& u, texture2d<float> ao, float2 uv, float dist) {\n"
    "  if (dist <= 0.0) return float3(1.0);\n"
    "  float2 g = uv * u.size.zw - 0.5, f = fract(g);\n"
    "  int2 i0 = int2(floor(g)), hi = int2(u.size.zw) - 1;\n"
    "  float3 s = 0.0;\n"
    "  float w = 0.0;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    int2 o = int2(k & 1, k >> 1);\n"
    "    float4 t = ao.read(uint2(clamp(i0 + o, int2(0), hi)));\n"
    "    float bw = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);\n"
    "    float dw = t.y > 0.0 ? 1.0 / (1e-3 + abs(t.y - dist) / dist) : 1e-3;\n"
    "    s += t.xzw * bw * dw, w += bw * dw;\n"
    "  }\n"
    "  return w > 0.0 ? s / w : float3(1.0);\n"
    "}\n"
    "fragment float4 fx_blur(FO in [[stage_in]], constant FxU& u [[buffer(0)]], constant int2& dir [[buffer(1)]],\n"
    "                        texture2d<float> a [[texture(0)]]) {\n"
    "  int2 p = int2(in.pos.xy), hi = int2(u.size.zw) - 1;\n"
    "  float4 c = a.read(uint2(p));\n"
    "  if (c.y <= 0.0) return c;\n"
    /* four pixels' worth, centered (the ends at half weight): one period of the 4x4 pattern */
    /* the occlusion over four pixels; the shadows only over their neighbours (their softness is
     * the maps' own, and the temporal pass evens their noise) */
    "  float3 s = c.xzw;\n"
    "  float w = 1.0, sw = 1.0;\n"
    "  float2 ss = c.zw;\n"
    "  for (int i = -2; i <= 2; ++i) {\n"
    "    if (i == 0) continue;\n"
    "    float4 t = a.read(uint2(clamp(p + dir * i, int2(0), hi)));\n"
    "    float k = (abs(i) == 2 ? 0.5 : 1.0) * saturate(1.0 - abs(t.y - c.y) / (0.03 * c.y));\n"
    "    s.x += t.x * k, w += k;\n"
    "    if (abs(i) == 1 && u.smapn2.z == 0.0) ss += t.zw * (0.5 * k), sw += 0.5 * k;\n"
    "  }\n"
    "  return float4(s.x / w, c.y, ss / sw);\n"
    "}\n"
    /* the occlusion and shadows over time: each texel's world point found in the frame before (its
     * camera), and that frame's result blended in where its distance agrees (no smearing across what
     * moved or came into view). The sampling patterns turn every frame, so this averages them into
     * a result that holds still as the camera moves, and a frame gone wrong fades rather than
     * flashes. */
    "fragment float4 fx_temporal(FO in [[stage_in]], constant FxU& u [[buffer(0)]], texture2d<float> cur [[texture(0)]],\n"
    "                            texture2d<float> hist [[texture(1)]], sampler s [[sampler(0)]]) {\n"
    "  float4 c = cur.read(uint2(in.pos.xy));\n"
    "  if (c.y <= 0.0 || u.hist.x == 0.0) return c;\n"
    "  float2 px = u.vp.xy + in.uv * u.vp.zw;\n"
    "  float3 P = view_pos(u, px, c.y * u.hand.x);\n"
    "  float4 pc = u.reproj * float4(P, 1.0);\n"
    "  if (pc.w <= 1e-4) return c;\n"
    "  float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
    "  if (any(puv < 0.0) || any(puv > 1.0)) return c;\n"
    "  float4 h = hist.sample(s, puv);\n"
    "  if (!(h.y > 0.0) || abs(h.y - pc.w) > 0.04 * pc.w) return c;\n"
    /* the history held to what this frame's 3x3 neighbourhood on the same surface spans (a TAA
     * clamp): the ground under a running character never changes depth, and without it the
     * shadow arrived faint and left a trail. The pattern differs between neighbours, so its noise
     * is inside the span and still averages out; the occlusion, with fewer taps a frame, gets more
     * room than the shadows. */
    "  int2 p = int2(in.pos.xy), hi = int2(u.size.zw) - 1;\n"
    "  float3 lo = c.xzw, up = c.xzw;\n"
    "  for (int dy = -1; dy <= 1; ++dy)\n"
    "    for (int dx = -1; dx <= 1; ++dx) {\n"
    "      float4 t = cur.read(uint2(clamp(p + int2(dx, dy), int2(0), hi)));\n"
    "      if (t.y > 0.0 && abs(t.y - c.y) < 0.05 * c.y) lo = min(lo, t.xzw), up = max(up, t.xzw);\n"
    "    }\n"
    "  const float3 give = float3(0.06, 0.02, 0.02);\n"
    "  float3 m = mix(c.xzw, clamp(h.xzw, lo - give, up + give), u.hist.z);\n"
    "  return float4(m.x, c.y, m.y, m.z);\n"
    "}\n"
    /* bloom's source: the scene at a quarter size (four bilinear taps), what is over the threshold,
     * with a soft knee */
    "fragment float4 fx_bright(FO in [[stage_in]], constant FxU& u [[buffer(0)]], texture2d<float> src [[texture(0)]],\n"
    "                          texture2d<float> ao [[texture(1)]], sampler s [[sampler(0)]]) {\n"
    "  float2 uv = (u.vp.xy + in.uv * u.vp.zw) / u.size.xy, t = 1.0 / u.size.xy;\n"
    "  float3 c = 0.25 * (src.sample(s, uv + t * float2(-1, -1)).rgb + src.sample(s, uv + t * float2(1, -1)).rgb +\n"
    "                     src.sample(s, uv + t * float2(-1, 1)).rgb + src.sample(s, uv + t * float2(1, 1)).rgb);\n"
    /* as fx_comp will shade it: the scene is copied before the occlusion and the sun's shadows, and a
     * character in a cliff's shadow, drawn dark, still glowed as bright as in the open */
    "  if (u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0) {\n"
    "    float4 os = ao.sample(s, in.uv);\n"
    "    c *= mix(1.0, os.x, u.ao.y) * mix(1.0, os.z, u.smap.x) * mix(1.0, os.w, u.shadow.x);\n"
    "  }\n"
    "  float l = max(c.r, max(c.g, c.b)), k = u.bloom.z;\n"
    "  float soft = clamp(l - u.bloom.x + k, 0.0, 2.0 * k);\n"
    "  soft = soft * soft / (4.0 * k + 1e-5);\n"
    "  return float4(c * (max(soft, l - u.bloom.x) / max(l, 1e-5)), 1.0);\n"
    "}\n"
    /* FXAA (after Lottes' 3.11, its quality preset 12 in brief): along an edge the local contrast
     * finds, how far to either end, and a blend across it by where this pixel lies on it */
    "constant float FXAA_Q[10] = { 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
    "fragment float4 fx_fxaa(FO in [[stage_in]], constant FxU& u [[buffer(0)]], texture2d<float> t [[texture(0)]],\n"
    "                        sampler s [[sampler(0)]]) {\n"
    "  float2 rcp = 1.0 / u.size.xy, uv = in.pos.xy * rcp;\n"
    "  float3 c = t.sample(s, uv).rgb;\n"
    "  float lm = dot(c, LUMA);\n"
    "  float ln = dot(t.sample(s, uv + float2(0, -rcp.y)).rgb, LUMA), ls = dot(t.sample(s, uv + float2(0, rcp.y)).rgb, LUMA);\n"
    "  float lw = dot(t.sample(s, uv + float2(-rcp.x, 0)).rgb, LUMA), le = dot(t.sample(s, uv + float2(rcp.x, 0)).rgb, LUMA);\n"
    "  float mx = max(lm, max(max(ln, ls), max(lw, le))), mn = min(lm, min(min(ln, ls), min(lw, le))), range = mx - mn;\n"
    "  if (range < max(0.0312, mx * 0.125)) return float4(c, 1.0);\n"
    "  float lnw = dot(t.sample(s, uv + float2(-rcp.x, -rcp.y)).rgb, LUMA), lne = dot(t.sample(s, uv + float2(rcp.x, -rcp.y)).rgb, LUMA);\n"
    "  float lsw = dot(t.sample(s, uv + float2(-rcp.x, rcp.y)).rgb, LUMA), lse = dot(t.sample(s, uv + float2(rcp.x, rcp.y)).rgb, LUMA);\n"
    "  float eh = abs(lnw + lne - 2.0 * ln) + 2.0 * abs(lw + le - 2.0 * lm) + abs(lsw + lse - 2.0 * ls);\n"
    "  float ev = abs(lnw + lsw - 2.0 * lw) + 2.0 * abs(ln + ls - 2.0 * lm) + abs(lne + lse - 2.0 * le);\n"
    "  bool horz = eh >= ev;\n"
    "  float l1 = horz ? ln : lw, l2 = horz ? ls : le, g1 = abs(l1 - lm), g2 = abs(l2 - lm);\n"
    "  float stp = horz ? rcp.y : rcp.x, lavg, grad;\n"
    "  if (g1 >= g2) { stp = -stp; lavg = 0.5 * (l1 + lm); grad = g1; } else { lavg = 0.5 * (l2 + lm); grad = g2; }\n"
    "  float2 e = uv, along = horz ? float2(rcp.x, 0) : float2(0, rcp.y);\n"
    "  if (horz) e.y += stp * 0.5; else e.x += stp * 0.5;\n"
    "  float2 p1 = e - along, p2 = e + along;\n"
    "  float d1 = dot(t.sample(s, p1).rgb, LUMA) - lavg, d2 = dot(t.sample(s, p2).rgb, LUMA) - lavg;\n"
    "  bool r1 = abs(d1) >= grad * 0.25, r2 = abs(d2) >= grad * 0.25;\n"
    "  for (int i = 0; i < 10 && !(r1 && r2); ++i) {\n"
    "    if (!r1) { p1 -= along * FXAA_Q[i]; d1 = dot(t.sample(s, p1).rgb, LUMA) - lavg; r1 = abs(d1) >= grad * 0.25; }\n"
    "    if (!r2) { p2 += along * FXAA_Q[i]; d2 = dot(t.sample(s, p2).rgb, LUMA) - lavg; r2 = abs(d2) >= grad * 0.25; }\n"
    "  }\n"
    "  float dist1 = horz ? uv.x - p1.x : uv.y - p1.y, dist2 = horz ? p2.x - uv.x : p2.y - uv.y;\n"
    "  bool near1 = dist1 < dist2;\n"
    "  float dmin = min(dist1, dist2), len = dist1 + dist2;\n"
    "  bool mid_lower = lm < lavg, good = ((near1 ? d1 : d2) < 0.0) != mid_lower;\n"
    "  float off = good ? -dmin / len + 0.5 : 0.0;\n"
    "  float avg = (2.0 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 12.0;\n"
    "  float sub = saturate(abs(avg - lm) / range);\n"
    "  sub = (-2.0 * sub + 3.0) * sub * sub;\n"
    "  off = max(off, sub * sub * 0.75);\n"
    "  float2 f = uv;\n"
    "  if (horz) f.y += off * stp; else f.x += off * stp;\n"
    "  return float4(t.sample(s, f).rgb, 1.0);\n"
    "}\n"
    /* half the size of the source: four bilinear taps */
    "fragment float4 fx_down(FO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  float2 tx = 1.0 / float2(t.get_width(), t.get_height());\n"
    "  return 0.25 * (t.sample(s, in.uv + tx * float2(-1, -1)) + t.sample(s, in.uv + tx * float2(1, -1)) +\n"
    "                 t.sample(s, in.uv + tx * float2(-1, 1)) + t.sample(s, in.uv + tx * float2(1, 1)));\n"
    "}\n"
    /* a 9-tap gaussian in five bilinear taps, dir texels apart */
    "fragment float4 fx_gauss(FO in [[stage_in]], constant int2& dir [[buffer(1)]], texture2d<float> t [[texture(0)]],\n"
    "                         sampler s [[sampler(0)]]) {\n"
    "  float2 tx = float2(dir) / float2(t.get_width(), t.get_height());\n"
    "  float4 c = t.sample(s, in.uv) * 0.2270270;\n"
    "  c += (t.sample(s, in.uv + tx * 1.3846154) + t.sample(s, in.uv - tx * 1.3846154)) * 0.3162162;\n"
    "  c += (t.sample(s, in.uv + tx * 3.2307692) + t.sample(s, in.uv - tx * 3.2307692)) * 0.0702703;\n"
    "  return c;\n"
    "}\n"
    /* what the god rays start from: the sky's bright pixels, more of them nearer the sun */
    "fragment float4 fx_raymask(FO in [[stage_in]], constant FxU& u [[buffer(0)]], texture2d<float> src [[texture(0)]],\n"
    "                           depth2d<float> dt [[texture(1)]], sampler s [[sampler(0)]]) {\n"
    "  float2 px = floor(u.vp.xy + in.uv * u.vp.zw) + 0.5;\n"
    "  if (view_z(u, dt.read(uint2(px))) != 0.0) return float4(0.0);\n"
    "  float3 c = src.sample(s, px / u.size.xy).rgb;\n"
    "  float2 d = (in.uv - u.sunuv.xy) * float2(u.proj.y / u.proj.x, 1.0);\n"
    "  float glow = saturate(1.0 - length(d) / 0.6);\n"
    "  return float4(c * smoothstep(0.35, 0.9, dot(c, LUMA)) * glow * glow, 1.0);\n"
    "}\n"
    /* the mask gathered along the line toward the sun, fading with each step (no dither: it would
     * shimmer from frame to frame) */
    "fragment float4 fx_rays(FO in [[stage_in]], constant FxU& u [[buffer(0)]], texture2d<float> m [[texture(0)]],\n"
    "                        sampler s [[sampler(0)]]) {\n"
    "  const int NS = 64;\n"
    "  float2 uv = in.uv, step = (in.uv - u.sunuv.xy) * (u.rays.z / float(NS));\n"
    "  float3 acc = float3(0.0);\n"
    "  float w = 1.0;\n"
    "  for (int i = 0; i < NS; ++i) { acc += m.sample(s, uv).rgb * w; w *= u.rays.y; uv -= step; }\n"
    "  return float4(acc * (4.0 / float(NS)), 1.0);\n"
    "}\n"
    /* The bounce light (gfx_hlsl.c's fx_gi, which says what each step does): what the sunlit surfaces
     * near P throw onto it, gathered from the map of the casters near the camera as the sun sees them,
     * depth and colour. rgb the light, a the distance. */
    "fragment float4 fx_gi(FO in [[stage_in]], constant FxU& u [[buffer(0)]], depth2d<float> dt [[texture(0)]],\n"
    "                      depth2d<float> gd [[texture(1)]], texture2d<float> gc [[texture(2)]]) {\n"
    "  constexpr sampler ls(coord::normalized, filter::linear, mip_filter::linear, address::clamp_to_edge);\n"
    "  float2 px = floor(u.vp.xy + in.uv * u.vp.zw) + 0.5;\n"
    "  float3 P = pos_at(u, dt, px);\n"
    "  float dist = P.z * u.hand.x;\n"
    "  if (dist <= 0.0) return float4(0.0);\n"
    "  float4 q = u.gimat * float4(P, 1.0);\n"
    "  float2 e = abs(q.xy);\n"
    "  float edge = (1.0 - smoothstep(0.8, 0.95, max(e.x, e.y))) * (1.0 - smoothstep(0.7 * u.gip.w, u.gip.w, dist));\n"
    "  if (edge <= 0.0 || q.z >= 1.0) return float4(0.0, 0.0, 0.0, dist);\n"
    "  float3 r = pos_at(u, dt, px + float2(1, 0)) - P, l = P - pos_at(u, dt, px - float2(1, 0));\n"
    "  float3 d = pos_at(u, dt, px + float2(0, 1)) - P, t = P - pos_at(u, dt, px - float2(0, 1));\n"
    "  float3 dx = (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0 ? r : l;\n"
    "  float3 dy = (abs(d.z) < abs(t.z) && dot(d, d) > 0.0) || dot(t, t) == 0.0 ? d : t;\n"
    "  float3 nc = cross(dx, dy);\n"
    "  float3 N = dot(nc, nc) > 1e-24 ? normalize(nc) : -normalize(P);\n"
    "  if (dot(N, P) > 0.0) N = -N;\n"
    "  float2 uv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5), sz = float2(gd.get_width(), gd.get_height());\n"
    "  const uchar BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n"
    "  int2 cell = int2(in.pos.xy) & 3;\n"
    "  float k = fract((float(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0 + u.hist.y);\n"
    "  const int NS = max(int(u.gip.z), 1);\n"
    "  float r2 = u.gi.z * u.gi.z;\n"
    "  float3 sum = float3(0.0);\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float tt = (float(i) + k) / float(NS), a = float(i) * 2.3999632 + k * 6.2831853;\n"
    "    float2 us = uv + float2(cos(a), sin(a)) * (sqrt(tt) * u.gi.y);\n"
    "    if (any(us <= 0.0) || any(us >= 1.0)) continue;\n"
    "    float zs = gd.read(uint2(us * sz));\n"
    "    if (zs >= 1.0) continue;\n"
    "    float4 x = u.giinv * float4(us.x * 2.0 - 1.0, 1.0 - us.y * 2.0, zs, 1.0);\n"
    "    float3 v = x.xyz / x.w - P;\n"
    "    float dd = dot(v, v) + 1e-4;\n"
    "    float3 vn = v * rsqrt(dd);\n"
    "    float w = saturate(dot(N, vn)) * saturate(0.25 - 0.75 * dot(u.sun.xyz, vn)) * r2 / (dd + 0.25 * r2) * saturate(2.0 - dd / r2);\n"
    "    sum += gc.sample(ls, us, level(u.gi.w)).rgb * w;\n"
    "  }\n"
    "  return float4(sum * (u.gi.x * edge / float(NS)), dist);\n"
    "}\n"
    /* the bounce light over frames: last frame's where this point was then, kept within what its
     * neighbours have now */
    "fragment float4 fx_gitemp(FO in [[stage_in]], constant FxU& u [[buffer(0)]], texture2d<float> cur [[texture(0)]],\n"
    "                          texture2d<float> hist [[texture(1)]], sampler s [[sampler(0)]]) {\n"
    "  float4 c = cur.read(uint2(in.pos.xy));\n"
    "  if (c.a <= 0.0 || u.gip.x == 0.0) return c;\n"
    "  float2 px = u.vp.xy + in.uv * u.vp.zw;\n"
    "  float3 P = view_pos(u, px, c.a * u.hand.x);\n"
    "  float4 pc = u.reproj * float4(P, 1.0);\n"
    "  if (pc.w <= 1e-4) return c;\n"
    "  float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
    "  if (any(puv < 0.0) || any(puv > 1.0)) return c;\n"
    "  float4 h = hist.sample(s, puv);\n"
    "  if (!(h.a > 0.0) || abs(h.a - pc.w) > 0.04 * pc.w) return c;\n"
    "  int2 p = int2(in.pos.xy), hi = int2(cur.get_width(), cur.get_height()) - 1;\n"
    "  float3 lo = c.rgb, up = c.rgb;\n"
    "  for (int dy = -1; dy <= 1; ++dy)\n"
    "    for (int dx = -1; dx <= 1; ++dx) {\n"
    "      float4 t = cur.read(uint2(clamp(p + int2(dx, dy), int2(0), hi)));\n"
    "      if (t.a > 0.0 && abs(t.a - c.a) < 0.05 * c.a) lo = min(lo, t.rgb), up = max(up, t.rgb);\n"
    "    }\n"
    "  float3 give = 0.1 * (up - lo) + 0.01;\n"
    "  return float4(mix(c.rgb, clamp(h.rgb, lo - give, up + give), u.gip.y), c.a);\n"
    "}\n"
    /* the bounce light at uv from its half size: the four round it, each as near in distance as it is */
    "static float3 gi_at(texture2d<float> g, float2 uv, float dist) {\n"
    "  if (dist <= 0.0) return float3(0.0);\n"
    "  int2 sz = int2(g.get_width(), g.get_height()), hi = sz - 1;\n"
    "  float2 gg = uv * float2(sz) - 0.5, f = fract(gg);\n"
    "  int2 i0 = int2(floor(gg));\n"
    "  float3 s = 0.0;\n"
    "  float w = 0.0;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    int2 o = int2(k & 1, k >> 1);\n"
    "    float4 t = g.read(uint2(clamp(i0 + o, int2(0), hi)));\n"
    "    float bw = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);\n"
    "    float dw = t.a > 0.0 ? 1.0 / (1e-3 + abs(t.a - dist) / dist) : 1e-3;\n"
    "    s += t.rgb * bw * dw, w += bw * dw;\n"
    "  }\n"
    "  return w > 0.0 ? s / w : float3(0.0);\n"
    "}\n"
    "static float3 screen(float3 a, float3 b) { return 1.0 - (1.0 - saturate(a)) * (1.0 - saturate(b)); }\n"
    "fragment float4 fx_comp(FO in [[stage_in]], constant FxU& u [[buffer(0)]], texture2d<float> src [[texture(0)]],\n"
    "                        texture2d<float> ao [[texture(1)]], depth2d<float> dt [[texture(2)]],\n"
    "                        texture2d<float> b1 [[texture(3)]], texture2d<float> b2 [[texture(4)]],\n"
    "                        texture2d<float> ry [[texture(5)]], texture2d<float> gt [[texture(6)]], sampler s [[sampler(0)]]) {\n"
    "  float2 px = in.pos.xy;\n"
    "  float4 c = src.read(uint2(px));\n"
    "  int dbg = int(u.grade.w);\n"
    "  float3 os = u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0 ? ao_at(u, ao, in.uv, view_z(u, dt.read(uint2(px))) * u.hand.x) : float3(1.0);\n"
    /* what glows (a lamp's glass, a lit doorway: near white in a colour) is light, not a surface: the occlusion
     * leaves it, as it greyed the lamps it stood next to */
    "  float o = mix(os.x, 1.0, smoothstep(0.6, 0.95, max(c.r, max(c.g, c.b)))), sun = mix(1.0, os.y, u.smap.x) * mix(1.0, os.z, u.shadow.x);\n"
    "  if (dbg == 1) return float4(o, o, o, c.a);\n"
    "  if (dbg == 5) return float4(float3(sun), c.a);\n"
    /* the bounce light (t6) on the surface's own colour, mostly where the sun does not reach, shaded by
     * the occlusion like any light that is not the sun's */
    "  float3 gl = u.gi.x > 0.0 ? gi_at(gt, in.uv, view_z(u, dt.read(uint2(px))) * u.hand.x) : float3(0.0);\n"
    "  if (dbg == 6) return float4(gl * 3.0, c.a);\n"
    "  if (dbg == 7 && px.x < u.vp.x + u.vp.z * 0.5) gl = float3(0.0);\n"
    "  float3 base = c.rgb;\n"
    "  c.rgb *= mix(1.0, o, u.ao.y) * sun;\n"
    "  c.rgb += base * gl * mix(1.0, o, u.ao.y) * (1.0 - 0.75 * mix(1.0, os.y, u.smap.x));\n"
    "  float f = 0.0;\n"
    "  if (u.fogc.a > 0.0) {\n"
    "    float z = view_z(u, dt.read(uint2(px)));\n"
    "    if (z != 0.0) {\n"
    "      float3 P = view_pos(u, px, z);\n"
    "      float d = length(P), bd = u.fogp.x * dot(P, u.up.xyz);\n"
    /* density a * exp(-b * height above the camera), integrated from the camera to P */
    "      float k = abs(bd) > 1e-4 ? (1.0 - exp(-bd)) / bd : 1.0;\n"
    "      f = min(1.0 - exp(-u.fogc.a * d * k), u.fogp.y);\n"
    /* the sun's light scattered toward the camera (Henyey-Greenstein, 1 looking straight at it) */
    "      float g = u.fogp.w, cs = dot(P / max(d, 1e-5), u.sun.xyz);\n"
    "      float sunk = u.sun.w * u.fogp.z * pow((1.0 - g) * (1.0 - g) / max(1.0 + g * g - 2.0 * g * cs, 1e-5), 1.5);\n"
    "      c.rgb = mix(c.rgb, u.fogc.rgb + u.suncol.rgb * sunk, f);\n"
    "    }\n"
    "  }\n"
    "  if (dbg == 2) return float4(f, f, f, c.a);\n"
    "  float3 add = float3(0.0);\n"
    "  if (u.bloom.y > 0.0) {\n"
    "    float3 bl = b1.sample(s, in.uv).rgb * 0.6 + b2.sample(s, in.uv).rgb * 0.8;\n"
    "    if (dbg == 3) return float4(bl, c.a);\n"
    "    add += bl * u.bloom.y;\n"
    "  }\n"
    "  if (u.rays.x > 0.0 && u.sunuv.z > 0.0) {\n"
    "    float3 r = ry.sample(s, in.uv).rgb * u.suncol.rgb * u.sunuv.z;\n"
    "    if (dbg == 4) return float4(r, c.a);\n"
    "    add += r * u.rays.x;\n"
    "  }\n"
    "  c.rgb = screen(c.rgb, add);\n"
    "  float3 x = mix(float3(dot(c.rgb, LUMA)), c.rgb, u.grade.y);\n"
    "  x = saturate(x);\n"
    /* the contrast curve leaves what nothing was drawn on (no depth). (The game's sky dome has depth: it
     * stands round the camera, no farther than the walls, so the curve still darkens it.) */
    "  x = mix(x, x * x * (3.0 - 2.0 * x), u.grade.z * (view_z(u, dt.read(uint2(px))) != 0.0 ? 1.0 : 0.0));\n"
    "  c.rgb = mix(c.rgb, x, u.grade.x);\n"
    "  return c;\n"
    "}\n";

/* Ray tracing's passes (rt_*: gfx_d3d12.c's, gfx_rt.hlsl, as Metal's inline intersection queries), built
 * with FX_MSL ahead of them, which they share. The world (rt_capture): every caster's triangles in this
 * frame's view space, the solid ones and then the alpha-tested (leaves, hair, grass), as two instances of
 * one structure - mask 1 the solid, 2 the alpha-tested; each corner two float4s in tri, its point, and its
 * first texture coordinates with its diffuse alpha. RtU: the solid triangles' count, the alpha tests'
 * count (tab, each its first triangle; its texture in texs; its sampler's address mode | stage 0's alpha
 * op << 2, its arguments << 8 and << 16; its test, D3DCMPFUNC << 8 | reference | the texture factor's
 * alpha << 16), the alpha-tested instance's index (2: none). The passes' buffers: FxU at 0, the blur's step at 1, RtU at 2,
 * tri 3, the smooth normals 4, tab 5, texs 6, the structure 7, the solid triangles' own 8 (rays that skip the
 * leaves trace it alone: no instances to walk). */
static const char RT_MSL[] =
    "using namespace metal::raytracing;\n"
    "struct RtU { uint4 rtp; };\n"
    "struct RtTex { texture2d<float> t; };\n"
    "typedef intersection_query<triangle_data, instancing> RtQ;\n"
    /* the ray through pixel px from the camera (view space) */
    "static float3 rt_view_dir(constant FxU& u, float2 px) { return normalize(view_pos(u, px, u.hand.x)); }\n"
    "static float3 rt_corner(device const float4* tri, uint prim, uint k) { return tri[(prim * 3u + k) * 2u].xyz; }\n"
    /* the triangle a ray hit: its face's normal, turned toward the ray's origin */
    "static float3 rt_tri_normal(device const float4* tri, uint prim, float3 dir) {\n"
    "  float3 a = rt_corner(tri, prim, 0), b = rt_corner(tri, prim, 1), c = rt_corner(tri, prim, 2);\n"
    "  float3 n = cross(b - a, c - a);\n"
    "  n = dot(n, n) > 1e-20 ? normalize(n) : -dir;\n"
    "  return dot(n, dir) > 0.0 ? -n : n;\n"
    "}\n"
    /* the surface's own smooth normal where a ray hit it (rt_nresolve's, across the triangle by bary), on
     * the side the ray came from (geo, the face's) */
    "static float3 rt_smooth_normal(device const float4* nrm, uint prim, float2 bary, float3 geo) {\n"
    "  float3 n = nrm[prim * 3u].xyz * (1.0 - bary.x - bary.y) + nrm[prim * 3u + 1u].xyz * bary.x + nrm[prim * 3u + 2u].xyz * bary.y;\n"
    "  if (dot(n, n) < 1e-8) return geo;\n"
    "  n = normalize(n);\n"
    "  return dot(n, geo) < 0.0 ? -n : n;\n"
    "}\n"
    /* does an alpha-tested triangle (prim, of all of them) let a ray through where it meets it (bary)? Its
     * caster found by its first triangle, its texture read at its corners' coordinates, tested as the draw
     * tests it */
    "static bool rt_alpha_holds(constant RtU& r, device const uint4* tab, device const float4* tri, device const RtTex* texs,\n"
    "                           uint prim, float2 bary) {\n"
    "  uint lo = 0, hi = r.rtp.y;\n"
    "  while (hi - lo > 1u) { uint mid = (lo + hi) / 2u; if (tab[mid].x <= prim) lo = mid; else hi = mid; }\n"
    "  uint4 e = tab[lo];\n"
    "  float3 uv = tri[prim * 6u + 1u].xyz * (1.0 - bary.x - bary.y) + tri[prim * 6u + 3u].xyz * bary.x + tri[prim * 6u + 5u].xyz * bary.y;\n"
    "  constexpr sampler sw(filter::linear, address::repeat), sc(filter::linear, address::clamp_to_edge),\n"
    "    sm(filter::linear, address::mirrored_repeat);\n"
    "  texture2d<float> t = texs[e.y].t;\n"
    "  uint am = e.z & 3u;\n"
    "  float ta = (am == 1u ? t.sample(sc, uv.xy, level(0)) : am == 2u ? t.sample(sm, uv.xy, level(0)) : t.sample(sw, uv.xy, level(0))).a;\n"
    /* stage 0's alpha as the draw makes it: its op over its two arguments (D3DTA: the diffuse or current,
     * the texture, the texture factor; the complement flag) */
    "  float tf = float((e.w >> 16) & 255u) / 255.0, ar[2];\n"
    "  for (int i = 0; i < 2; ++i) {\n"
    "    uint x = (e.z >> (8 + 8 * i)) & 255u, w = x & 15u;\n"
    "    float v = w == 2u ? ta : w == 3u ? tf : w <= 1u ? uv.z : 1.0;\n"
    "    ar[i] = (x & 16u) != 0u ? 1.0 - v : v;\n"
    "  }\n"
    "  uint op = (e.z >> 2) & 63u;\n"
    "  float a = op == 1u ? uv.z : op == 2u ? ar[0] : op == 3u ? ar[1] : op == 4u ? ar[0] * ar[1] : op == 5u ? ar[0] * ar[1] * 2.0 :\n"
    "    op == 6u ? ar[0] * ar[1] * 4.0 : op == 7u ? ar[0] + ar[1] : ta;\n"
    "  a = saturate(a) * 255.0;\n"
    "  float ref = float(e.w & 255u);\n"
    "  switch ((e.w >> 8) & 255u) {\n"
    "  case 1: return false;\n"
    "  case 2: return a < ref;\n"
    "  case 3: return abs(a - ref) < 0.5;\n"
    "  case 4: return a <= ref;\n"
    "  case 5: return a > ref;\n"
    "  case 6: return abs(a - ref) >= 0.5;\n"
    "  case 7: return a >= ref;\n"
    "  default: return true;\n"
    "  }\n"
    "}\n"
    /* the nearest hit along a ray, t in [tmin, tmax]; prim the triangle (of all of them; -1 none), bary where */
    "static float rt_trace(instance_acceleration_structure w, constant RtU& r, device const uint4* tab, device const float4* tri,\n"
    "                      device const RtTex* texs, float3 o, float3 d, float tmin, float tmax, thread int& prim, thread float2& bary) {\n"
    "  ray rr(o, d, tmin, tmax);\n"
    "  intersection_params p;\n"
    "  RtQ q;\n"
    "  q.reset(rr, w, 0xFFu, p);\n"
    "  while (q.next())\n"
    "    if (q.get_candidate_intersection_type() == intersection_type::triangle &&\n"
    "        rt_alpha_holds(r, tab, tri, texs, r.rtp.x + q.get_candidate_primitive_id(), q.get_candidate_triangle_barycentric_coord()))\n"
    "      q.commit_triangle_intersection();\n"
    "  prim = -1, bary = float2(0.0);\n"
    "  if (q.get_committed_intersection_type() != intersection_type::triangle) return tmax;\n"
    "  prim = int(q.get_committed_primitive_id() + (q.get_committed_instance_id() == r.rtp.z ? r.rtp.x : 0u));\n"
    "  bary = q.get_committed_triangle_barycentric_coord();\n"
    "  return q.get_committed_distance();\n"
    "}\n"
    /* the nearest hit among the solid triangles alone (gfx_rt.hlsl trace_solid: the bounce light's and the
     * occlusion's rays pass the leaves and grass) */
    "static float rt_trace_solid(primitive_acceleration_structure w, float3 o, float3 d, float tmin, float tmax, thread int& prim) {\n"
    "  ray rr(o, d, tmin, tmax);\n"
    "  intersector<triangle_data> is;\n"
    "  is.assume_geometry_type(geometry_type::triangle);\n"
    "  is.force_opacity(forced_opacity::opaque);\n"
    "  intersection_result<triangle_data> h = is.intersect(rr, w);\n"
    "  prim = -1;\n"
    "  if (h.type != intersection_type::triangle) return tmax;\n"
    "  prim = int(h.primitive_id);\n"
    "  return h.distance;\n"
    "}\n"
    /* is anything between o + d * tmin and o + d * tmax? */
    "static bool rt_blocked(instance_acceleration_structure w, constant RtU& r, device const uint4* tab, device const float4* tri,\n"
    "                       device const RtTex* texs, float3 o, float3 d, float tmin, float tmax) {\n"
    "  ray rr(o, d, tmin, tmax);\n"
    "  intersection_params p;\n"
    "  p.accept_any_intersection(true);\n"
    "  RtQ q;\n"
    "  q.reset(rr, w, 0xFFu, p);\n"
    "  while (q.next())\n"
    "    if (q.get_candidate_intersection_type() == intersection_type::triangle &&\n"
    "        rt_alpha_holds(r, tab, tri, texs, r.rtp.x + q.get_candidate_primitive_id(), q.get_candidate_triangle_barycentric_coord()))\n"
    "      q.commit_triangle_intersection();\n"
    "  return q.get_committed_intersection_type() == intersection_type::triangle;\n"
    "}\n"
    "constant uchar RT_BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n"
    /* a pixel's two numbers for ray i this frame (gfx_rt.hlsl pattern) */
    "static float2 rt_pattern(constant FxU& u, float2 pos, int i) {\n"
    "  int2 cell = int2(pos) & 3;\n"
    "  float k = fract((float(RT_BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0 + u.hist.y + float(i) * 0.6180340);\n"
    "  uint h = (uint(pos.x) * 73856093u) ^ (uint(pos.y) * 19349663u);\n"
    "  float j = fract(float(h & 1023u) / 1024.0 + u.hist.y * 1.3247180 + float(i) * 0.7548777);\n"
    "  return float2(k, j);\n"
    "}\n"
    "static void rt_basis(float3 N, thread float3& t, thread float3& s) {\n"
    "  t = abs(N.y) < 0.99 ? normalize(cross(N, float3(0, 1, 0))) : normalize(cross(N, float3(1, 0, 0)));\n"
    "  s = cross(N, t);\n"
    "}\n"
    /* a direction over the hemisphere round N, cosine-weighted */
    "static float3 rt_hemi(float3 N, float a, float b) {\n"
    "  float3 t, s;\n"
    "  rt_basis(N, t, s);\n"
    "  float r = sqrt(a), phi = 6.2831853 * b;\n"
    "  return normalize(t * (r * cos(phi)) + s * (r * sin(phi)) + N * sqrt(max(1.0 - a, 0.0)));\n"
    "}\n"
    /* how much of the sun reaches p (gfx_rt.hlsl sun_seen) */
    "static float rt_sun_seen(constant FxU& u, instance_acceleration_structure w, constant RtU& r, device const uint4* tab,\n"
    "                         device const float4* tri, device const RtTex* texs, float3 p, float3 n, float3 g, float2 pos, int rays) {\n"
    "  float nl = dot(n, u.sun.xyz);\n"
    "  if (nl <= 0.0) return 0.0;\n"
    "  float3 t, s;\n"
    "  rt_basis(u.sun.xyz, t, s);\n"
    "  float3 o = p + g * 0.02 + n * 0.03;\n"
    "  float spread = 0.035, tmin = max(u.smap2.z, 0.05), lit = 0.0;\n"
    "  for (int i = 0; i < rays; ++i) {\n"
    "    float2 rn = rt_pattern(u, pos, i + 3);\n"
    "    float rr = sqrt(rn.x) * spread, phi = 6.2831853 * rn.y;\n"
    "    float3 d = normalize(u.sun.xyz + t * (rr * cos(phi)) + s * (rr * sin(phi)));\n"
    "    lit += rt_blocked(w, r, tab, tri, texs, o, d, tmin, 500.0) ? 0.0 : 1.0;\n"
    "  }\n"
    "  return smoothstep(0.0, 0.2, nl) * lit / float(rays);\n"
    "}\n"
    /* debug=clay (8): the world as the rays see it (gfx_rt.hlsl rt_clay). The depth at texture 0. */
    "fragment float4 rt_clay(FO in [[stage_in]], constant FxU& u [[buffer(0)]], constant RtU& r [[buffer(2)]],\n"
    "                        device const float4* tri [[buffer(3)]], device const float4* nrm [[buffer(4)]],\n"
    "                        device const uint4* tab [[buffer(5)]], device const RtTex* texs [[buffer(6)]],\n"
    "                        instance_acceleration_structure world [[buffer(7)]], depth2d<float> dt [[texture(0)]]) {\n"
    "  float2 px = floor(u.vp.xy + in.uv * u.vp.zw) + 0.5;\n"
    "  float z = view_z(u, dt.read(uint2(px)));\n"
    "  float drawn = z != 0.0 ? length(view_pos(u, px, z)) : 0.0;\n"
    "  float3 d = rt_view_dir(u, px);\n"
    "  int prim;\n"
    "  float2 bary;\n"
    "  float t = rt_trace(world, r, tab, tri, texs, float3(0.0), d, 0.05, 2000.0, prim, bary);\n"
    "  if (prim < 0) return drawn > 0.0 ? float4(0.1, 0.2, 0.9, 1) : float4(0.35, 0.45, 0.6, 1);\n"
    "  float3 g = rt_tri_normal(tri, uint(prim), d), n = rt_smooth_normal(nrm, uint(prim), bary, g), p = d * t;\n"
    "  float sky = 0.25 + 0.15 * dot(n, u.up.xyz);\n"
    "  float lit = sky + (u.sun.w > 0.0 ? 0.7 * saturate(dot(n, u.sun.xyz)) * rt_sun_seen(u, world, r, tab, tri, texs, p, n, g, in.pos.xy, 8) : 0.0);\n"
    "  float3 c = float3(lit);\n"
    "  if (drawn <= 0.0) c *= float3(1.0, 0.85, 0.2);\n"
    "  else if (abs(t - drawn) > max(0.05 * drawn, 0.3)) c *= t < drawn ? float3(1.0, 0.3, 0.3) : float3(0.3, 0.4, 1.0);\n"
    "  return float4(c, 1);\n"
    "}\n"
    /* the surface's normal at px (P its point) from the depth: each way the neighbour nearer in depth */
    "static float3 rt_normal_at(constant FxU& u, depth2d<float> dt, float2 px, float3 P) {\n"
    "  float3 r = pos_at(u, dt, px + float2(1, 0)) - P, l = P - pos_at(u, dt, px - float2(1, 0));\n"
    "  float3 d = pos_at(u, dt, px + float2(0, 1)) - P, t = P - pos_at(u, dt, px - float2(0, 1));\n"
    "  float3 dx = (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0 ? r : l;\n"
    "  float3 dy = (abs(d.z) < abs(t.z) && dot(d, d) > 0.0) || dot(t, t) == 0.0 ? d : t;\n"
    "  float3 nc = cross(dx, dy);\n"
    "  float3 N = dot(nc, nc) > 1e-24 ? normalize(nc) : -normalize(P);\n"
    "  return dot(N, P) > 0.0 ? -N : N;\n"
    "}\n"
    /* the light leaving the surface at Q (view space) toward a ray, as the frame or the sun saw it; 0
     * unseen (gfx_rt.hlsl radiance) */
    "static float3 rt_radiance(constant FxU& u, float3 Q, depth2d<float> dt, texture2d<float> src, texture2d<float> occ,\n"
    "                          depth2d<float> gd, texture2d<float> gc, sampler ls) {\n"
    "  float qd = Q.z * u.hand.x;\n"
    "  if (qd > 0.05) {\n"
    "    float2 ndc = float2(Q.x * u.proj.x + Q.z * u.proj.z, Q.y * u.proj.y + Q.z * u.proj.w) / qd;\n"
    "    float2 q = u.vp.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;\n"
    "    if (all(q >= u.vp.xy) && all(q < u.vp.xy + u.vp.zw - 1.0)) {\n"
    "      float sd = view_z(u, dt.read(uint2(q + 0.5))) * u.hand.x;\n"
    "      if (sd > 0.0 && abs(sd - qd) < 0.03 * qd + 0.1) {\n"
    "        float3 c = src.read(uint2(q + 0.5)).rgb;\n"
    "        float sun = occ.sample(ls, (q - u.vp.xy) / u.vp.zw, level(0)).z;\n"
    "        return c * mix(1.0, sun, u.smap.x);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  if (u.gi.x > 0.0) {\n"
    "    float4 lc = u.gimat * float4(Q, 1.0);\n"
    "    if (all(abs(lc.xy) < 0.98) && lc.z < 1.0) {\n"
    "      float2 uv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);\n"
    "      float z = gd.read(uint2(uv * float2(gd.get_width(), gd.get_height())));\n"
    "      if (lc.z - z < 0.002) return gc.sample(ls, uv, level(0)).rgb;\n"
    "    }\n"
    "  }\n"
    "  return float3(0.0);\n"
    "}\n"
    /* the bounce light traced (gfx_rt.hlsl rt_gi). Textures: the depth (0), the scene as drawn (1), the
     * occlusion and sun (2: fx_ao's, the sun's map in z), the bounce map's depth (3) and colour (4) */
    "fragment float4 rt_gi(FO in [[stage_in]], constant FxU& u [[buffer(0)]], constant RtU& r [[buffer(2)]],\n"
    "                      device const float4* tri [[buffer(3)]], primitive_acceleration_structure solid [[buffer(8)]],\n"
    "                      depth2d<float> dt [[texture(0)]], texture2d<float> src [[texture(1)]], texture2d<float> occ [[texture(2)]],\n"
    "                      depth2d<float> gd [[texture(3)]], texture2d<float> gc [[texture(4)]], sampler ls [[sampler(0)]]) {\n"
    "  float2 px = floor(u.vp.xy + in.uv * u.vp.zw) + 0.5;\n"
    "  float3 P = pos_at(u, dt, px);\n"
    "  float dist = P.z * u.hand.x;\n"
    "  if (dist <= 0.0) return float4(0.0);\n"
    "  float fade = 1.0 - smoothstep(0.7 * u.gip.w, u.gip.w, dist);\n"
    "  if (fade <= 0.0) return float4(0.0, 0.0, 0.0, dist);\n"
    "  float3 N = rt_normal_at(u, dt, px, P);\n"
    "  float3 o = P + N * (0.02 + 0.002 * dist);\n"
    "  int NS = max(int(u.gip.z), 1);\n"
    "  float3 sum = float3(0.0);\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float2 rn = rt_pattern(u, in.pos.xy, i);\n"
    "    float3 d = rt_hemi(N, rn.x, rn.y);\n"
    "    int prim;\n"
    "    float t = rt_trace_solid(solid, o, d, 0.0, u.gi.z, prim);\n"
    "    if (prim < 0) continue;\n"
    "    float3 Q = o + d * t, nq = rt_tri_normal(tri, uint(prim), d);\n"
    "    sum += rt_radiance(u, Q + nq * 0.02, dt, src, occ, gd, gc, ls);\n"
    "  }\n"
    "  return float4(sum * (u.gi.x * fade / float(NS)), dist);\n"
    "}\n"
    /* the occlusion traced (gfx_rt.hlsl rt_ao): fx_ao's result (texture 0) with x traced; the depth at 1 */
    "fragment float4 rt_ao(FO in [[stage_in]], constant FxU& u [[buffer(0)]], primitive_acceleration_structure solid [[buffer(8)]],\n"
    "                      texture2d<float> ao [[texture(0)]], depth2d<float> dt [[texture(1)]]) {\n"
    "  float4 c = ao.read(uint2(in.pos.xy));\n"
    "  if (c.y <= 0.0) return c;\n"
    "  float2 px = floor(u.vp.xy + in.uv * u.vp.zw) + 0.5;\n"
    "  float3 P = pos_at(u, dt, px);\n"
    "  float3 N = rt_normal_at(u, dt, px, P);\n"
    "  float3 o = P + N * (0.01 + 0.002 * c.y);\n"
    "  float R = u.ao.x, occl = 0.0;\n"
    "  const int NS = 6;\n"
    "  for (int i = 0; i < NS; ++i) {\n"
    "    float2 rn = rt_pattern(u, in.pos.xy, i + 7);\n"
    "    float3 d = rt_hemi(N, rn.x, rn.y);\n"
    "    int prim;\n"
    "    float t = rt_trace_solid(solid, o, d, 0.0, R, prim);\n"
    "    if (prim >= 0) { float f = 1.0 - t / R; occl += f * f; }\n"
    "  }\n"
    "  c.x = saturate(1.0 - occl / float(NS));\n"
    "  return c;\n"
    "}\n"
    /* the bounce light smoothed (gfx_rt.hlsl rt_giblur): a 5x5 at the step dir, by how near each distance is */
    "fragment float4 rt_giblur(FO in [[stage_in]], constant int2& stp [[buffer(1)]], texture2d<float> g [[texture(0)]]) {\n"
    "  int2 p = int2(in.pos.xy), hi = int2(g.get_width(), g.get_height()) - 1;\n"
    "  float4 c = g.read(uint2(p));\n"
    "  if (c.a <= 0.0) return c;\n"
    "  const float K[3] = { 0.375, 0.25, 0.0625 };\n"
    "  float3 s = float3(0.0);\n"
    "  float sw = 0.0;\n"
    "  for (int y = -2; y <= 2; ++y)\n"
    "    for (int x = -2; x <= 2; ++x) {\n"
    "      float4 t = g.read(uint2(clamp(p + int2(x, y) * stp, int2(0), hi)));\n"
    "      float k = K[abs(x)] * K[abs(y)] * (t.a > 0.0 ? saturate(1.0 - abs(t.a - c.a) / (0.04 * c.a)) : 0.0);\n"
    "      s += t.rgb * k, sw += k;\n"
    "    }\n"
    "  return float4(sw > 0.0 ? s / sw : c.rgb, c.a);\n"
    "}\n"
    /* the world's smooth normals (gfx_rt.hlsl rt_nclear, rt_nsum, rt_nresolve): the corners that share a
     * place (to 1/256 of a unit) found through a hash table, each place summing its faces' normals (in
     * 1/4096ths), each corner taking its place's average unless it turns more than 60 degrees from its own
     * face. NC: the triangles' count, the table's mask (its slots less one), the corners' count. Buffers:
     * NC 0, the table 1 (four words a slot: its tag, the sum), the triangles 2, the normals 3. */
    "struct NC { uint tris, mask, verts, pad; };\n"
    "static uint3 rt_key(float3 p) { return uint3(int3(floor(p * 256.0 + 0.5))); }\n"
    "static uint rt_hash(uint3 k) { return (k.x * 73856093u) ^ (k.y * 19349663u) ^ (k.z * 83492791u); }\n"
    "static uint rt_tag(uint3 k) { return ((k.x * 2654435761u) ^ (k.y * 2246822519u) ^ (k.z * 3266489917u)) | 1u; }\n"
    "static uint rt_slot(constant NC& n, device atomic_uint* table, float3 p, bool make) {\n"
    "  uint3 k = rt_key(p);\n"
    "  uint h = rt_hash(k) & n.mask, tag = rt_tag(k);\n"
    "  for (uint i = 0; i < 64u; ++i) {\n"
    "    uint slot = (h + i) & n.mask, was = 0;\n"
    "    if (make) {\n"
    "      while (!atomic_compare_exchange_weak_explicit(&table[slot * 4u], &was, tag, memory_order_relaxed, memory_order_relaxed) &&\n"
    "             was == 0u) {}\n"
    "    } else\n"
    "      was = atomic_load_explicit(&table[slot * 4u], memory_order_relaxed);\n"
    "    if (was == tag || (make && was == 0u)) return slot;\n"
    "    if (!make && was == 0u) break;\n"
    "  }\n"
    "  return n.mask + 1u;\n"
    "}\n"
    "kernel void rt_nclear(uint id [[thread_position_in_grid]], constant NC& n [[buffer(0)]], device atomic_uint* table [[buffer(1)]]) {\n"
    "  if (id > n.mask) return;\n"
    "  for (uint k = 0; k < 4u; ++k) atomic_store_explicit(&table[id * 4u + k], 0u, memory_order_relaxed);\n"
    "}\n"
    "kernel void rt_nsum(uint id [[thread_position_in_grid]], constant NC& n [[buffer(0)]], device atomic_uint* table [[buffer(1)]],\n"
    "                    device const float4* tri [[buffer(2)]]) {\n"
    "  if (id >= n.tris) return;\n"
    "  float3 v[3] = { rt_corner(tri, id, 0), rt_corner(tri, id, 1), rt_corner(tri, id, 2) };\n"
    "  float3 f = cross(v[1] - v[0], v[2] - v[0]);\n"
    "  if (!(dot(f, f) > 1e-14)) return;\n"
    "  int3 q = int3(normalize(f) * 4096.0);\n"
    "  for (int k = 0; k < 3; ++k) {\n"
    "    uint slot = rt_slot(n, table, v[k], true);\n"
    "    if (slot > n.mask) continue;\n"
    "    atomic_fetch_add_explicit(&table[slot * 4u + 1u], uint(q.x), memory_order_relaxed);\n"
    "    atomic_fetch_add_explicit(&table[slot * 4u + 2u], uint(q.y), memory_order_relaxed);\n"
    "    atomic_fetch_add_explicit(&table[slot * 4u + 3u], uint(q.z), memory_order_relaxed);\n"
    "  }\n"
    "}\n"
    "kernel void rt_nresolve(uint id [[thread_position_in_grid]], constant NC& n [[buffer(0)]], device atomic_uint* table [[buffer(1)]],\n"
    "                        device const float4* tri [[buffer(2)]], device float4* nrm [[buffer(3)]]) {\n"
    "  if (id >= n.verts) return;\n"
    "  uint t0 = id - id % 3u;\n"
    "  float3 a = tri[t0 * 2u].xyz, b = tri[(t0 + 1u) * 2u].xyz, c = tri[(t0 + 2u) * 2u].xyz;\n"
    "  float3 f = cross(b - a, c - a);\n"
    "  if (!(dot(f, f) > 1e-14)) { nrm[id] = float4(0.0); return; }\n"
    "  f = normalize(f);\n"
    "  float3 nn = f;\n"
    "  uint slot = rt_slot(n, table, tri[id * 2u].xyz, false);\n"
    "  if (slot <= n.mask) {\n"
    "    float3 sum = float3(int3(int(atomic_load_explicit(&table[slot * 4u + 1u], memory_order_relaxed)),\n"
    "                             int(atomic_load_explicit(&table[slot * 4u + 2u], memory_order_relaxed)),\n"
    "                             int(atomic_load_explicit(&table[slot * 4u + 3u], memory_order_relaxed))));\n"
    "    if (dot(sum, sum) > 1.0) { sum = normalize(sum); nn = dot(sum, f) > 0.5 ? sum : f; }\n"
    "  }\n"
    "  nrm[id] = float4(nn, 0.0);\n"
    "}\n";


static struct
{
    int tried;
    id<MTLLibrary> lib;
    id<MTLRenderPipelineState> ao_pipe, blur_pipe, bright_pipe, down_pipe, gauss_pipe, raymask_pipe, rays_pipe, comp_pipe,
        temporal_pipe, aa_pipe, linz_pipe, zmip_pipe, gi_pipe, gitemp_pipe;
    /* the bounce light: the casters near the camera as the sun sees them, depth and colour (with three
     * levels below); the gather at half the occlusion's size; after the temporal pass, this frame's and
     * the one before */
    id<MTLTexture> gimap, gicol, gi0, gi1, gih[2]; /* (gi1: the traced bounce light's smoothing, rt_giblur) */
    int gih_at;
    uint64_t gih_serial, prev_serial; /* the frames the bounce's history and the camera (prev_view) were kept */
    MTLPixelFormat comp_fmt, aa_fmt;
    /* the occlusion's depth: view z at its size and three levels below (fx_linz, fx_zmip), and a view
     * of each level to draw into */
    id<MTLTexture> lz, lzv[4];
    id<MTLTexture> aa_src; /* the scene as it was, for the anti-aliasing pass to read (scene_aa) */
    id<MTLTexture> src, ao0, ao1, b1a, b1b, b2a, b2b, ra, rb;
    id<MTLTexture> hist[2]; /* the occlusion and shadows after the temporal pass: this frame's and the one before */
    int hist_at;            /* which of hist[] the frame before wrote */
    uint64_t hist_serial;   /* the frame it was written (0: none) */
    float prev_view[16], prev_proj[16], prev_cam[3];
    id<MTLSamplerState> samp, cmp;
    id<MTLTexture> smap, smapn, scol, scol8, sdummy; /* the sun's shadow maps (far, near), their (memoryless) colors at the same sizes, a stand-in */
    float focus[3]; /* the player's place in the world (gfx_set_focus) */
    int has_focus;
    id<MTLDepthStencilState> sdepth;
    /* what the fog and rays follow, eased from frame to frame (fx_ease): the game's values can
     * change between frames, and the effects should not pop with them */
    int eased;           /* ease (the last scene was the frame before); else take the new values */
    uint64_t eased_serial;
    float fog_on, fogc[3], up[3], sun[3], suncol[3];
    /* toward the sun in the world, as the last lit draw gave it, and the frame it was seen */
    float sunw[3];
    float sun0[3], sunt[3], sunp; /* the sun's glide from its last step (sun0) to the game's (sunt), sunp of the way */
    /* how much of the game's light is the sun's (its diffuse against the ambient), eased: weather and
     * clouds dim it, and the shadows fade with it */
    float direct;
    uint64_t sunw_seen;
    /* the shadows' profile (FFXI_PROFILE): frames, frames with their own sun, with a map, the
     * fewest and most casters */
    uint32_t st_frames, st_own, st_map, st_cmin, st_cmax, st_drawn_this, st_cached;
    float st_across;
    /* the trace (gfx_trace_dump): each scene done */
    uint32_t tr_live, tr_cached, tr_skipped, tr_depth;
    float tr_strength, tr_day, tr_dl, tr_al, tr_fog[3];
    int fogc_set;
    float last_cam[3]; /* where the camera was at the last scene (a jump is a new place) */
    struct { uint64_t serial; uint32_t live, cached, skipped, own, depth; float fog[3], dl, al, strength, day, sun[3], cam[3], across; } trace[1200];
    uint32_t ntrace;
    /* the water (water_mode): the scene's camera and light as its draws need them (WaterU, set by
     * gfx_scene_done the frame it was), and the copies of the target it draws over */
    struct WaterU
    {
        float iv[16], view[16], zp[4], hand[4], size[4], sun[4], suncol[4], sky[4], p[4], p2[4];
    } wu;
    uint64_t wu_serial;
    id<MTLTexture> wcol, wdep;
    id<MTLTexture> w_from; /* the target they were copied from, and the frame */
    uint64_t w_serial;
} g_fx;

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

static id<MTLRenderPipelineState> fx_pipeline(NSString* frag, MTLPixelFormat fmt)
{
    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> vf = [g_fx.lib newFunctionWithName:@"fx_vs"], ff = [g_fx.lib newFunctionWithName:frag];
    pd.vertexFunction = vf;
    pd.fragmentFunction = ff;
    pd.colorAttachments[0].pixelFormat = fmt;
    NSError* err = nil;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p)
        __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED),
            fprintf(stderr, "[recomp] gfx: scene effect %s failed: %s\n", [frag UTF8String], err ? [[err localizedDescription] UTF8String] : "?");
    [vf release];
    [ff release];
    [pd release];
    return p;
}

static int fx_init(void)
{
    if (g_fx.tried)
        return g_fx.ao_pipe != nil;
    g_fx.tried = 1;
    g_fx.lib = compile(FX_MSL);
    if (!g_fx.lib)
        return 0;
    g_fx.ao_pipe = fx_pipeline(@"fx_ao", MTLPixelFormatRGBA16Float);
    g_fx.blur_pipe = fx_pipeline(@"fx_blur", MTLPixelFormatRGBA16Float);
    g_fx.bright_pipe = fx_pipeline(@"fx_bright", MTLPixelFormatRGBA16Float);
    g_fx.down_pipe = fx_pipeline(@"fx_down", MTLPixelFormatRGBA16Float);
    g_fx.gauss_pipe = fx_pipeline(@"fx_gauss", MTLPixelFormatRGBA16Float);
    g_fx.raymask_pipe = fx_pipeline(@"fx_raymask", MTLPixelFormatRGBA16Float);
    g_fx.rays_pipe = fx_pipeline(@"fx_rays", MTLPixelFormatRGBA16Float);
    g_fx.temporal_pipe = fx_pipeline(@"fx_temporal", MTLPixelFormatRGBA16Float);
    g_fx.linz_pipe = fx_pipeline(@"fx_linz", MTLPixelFormatR32Float);
    g_fx.zmip_pipe = fx_pipeline(@"fx_zmip", MTLPixelFormatR32Float);
    g_fx.gi_pipe = fx_pipeline(@"fx_gi", MTLPixelFormatRGBA16Float);
    g_fx.gitemp_pipe = fx_pipeline(@"fx_gitemp", MTLPixelFormatRGBA16Float);
    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g_fx.samp = [g_dev newSamplerStateWithDescriptor:sd];
    sd.compareFunction = MTLCompareFunctionLessEqual; /* 1 where the point is no deeper than the map */
    g_fx.cmp = [g_dev newSamplerStateWithDescriptor:sd];
    [sd release];
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                  width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    g_fx.sdummy = [g_dev newTextureWithDescriptor:td];
    MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = MTLCompareFunctionLess;
    dd.depthWriteEnabled = YES;
    g_fx.sdepth = [g_dev newDepthStencilStateWithDescriptor:dd];
    [dd release];
    if (!g_fx.ao_pipe || !g_fx.blur_pipe || !g_fx.bright_pipe || !g_fx.down_pipe || !g_fx.gauss_pipe || !g_fx.raymask_pipe ||
        !g_fx.rays_pipe || !g_fx.temporal_pipe || !g_fx.linz_pipe || !g_fx.zmip_pipe || !g_fx.gi_pipe || !g_fx.gitemp_pipe)
    {
        [g_fx.ao_pipe release], g_fx.ao_pipe = nil;
        return 0;
    }
    fprintf(stderr, "[recomp] gfx: scene effects ready\n");
    return 1;
}

/* a private texture of this size and format in *t, made again when either changes */
static id<MTLTexture> fx_tex(id<MTLTexture>* t, MTLPixelFormat fmt, NSUInteger w, NSUInteger h)
{
    if (*t && (*t).width == w && (*t).height == h && (*t).pixelFormat == fmt)
        return *t;
    [*t release];
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    *t = [g_dev newTextureWithDescriptor:d];
    return *t;
}

/* --- GPU time per part of the frame (profile) ---
 * A counter sample buffer per frame in flight, when the GPU samples timestamps at pass boundaries;
 * ticks to ns from pairs of CPU and GPU timestamps taken at least a second apart. */
static id<MTLCounterSampleBuffer> ts_buffer(void)
{
    if (!gfx_profiling)
        return nil;
    static MTLTimestamp gpu0;
    static uint64_t ns0;
    if (g_ts_ok < 0)
    {
        g_ts_ok = 0;
        id<MTLCounterSet> set = nil;
        for (id<MTLCounterSet> cs in g_dev.counterSets)
            if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp])
                set = cs;
        if (set && [g_dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
        {
            MTLCounterSampleBufferDescriptor* d = [[MTLCounterSampleBufferDescriptor alloc] init];
            d.counterSet = set;
            d.storageMode = MTLStorageModeShared;
            d.sampleCount = TS_N;
            g_ts_ok = 1;
            for (int i = 0; i < FRAMES; ++i)
                if (!(g_ts[i] = [g_dev newCounterSampleBufferWithDescriptor:d error:NULL]))
                    g_ts_ok = 0;
            [d release];
        }
        if (!g_ts_ok)
            fprintf(stderr, "[recomp] gfx: no GPU timestamps at pass boundaries: no time per pass in the profile\n");
    }
    if (g_ts_ok <= 0)
        return nil;
    uint64_t now = gfx_now_ns();
    if (!ns0 || now - ns0 > 1000000000ull)
    {
        MTLTimestamp cpu, gpu;
        [g_dev sampleTimestamps:&cpu gpuTimestamp:&gpu];
        now = gfx_now_ns();
        if (ns0 && gpu > gpu0)
            g_ts_scale = (double)(now - ns0) / (double)(gpu - gpu0);
        gpu0 = gpu, ns0 = now;
    }
    return g_ts_scale > 0.0 ? g_ts[g_frame] : nil;
}

/* a pass marks its start as `start` (the frame's first such pass only) and its end as `end` (the
 * last one's stands); -1 for neither */
static void ts_mark(MTLRenderPassDescriptor* rp, int start, int end)
{
    id<MTLCounterSampleBuffer> b = ts_buffer();
    if (!b)
        return;
    MTLRenderPassSampleBufferAttachmentDescriptor* a = rp.sampleBufferAttachments[0];
    a.sampleBuffer = b;
    a.startOfVertexSampleIndex = MTLCounterDontSample;
    a.endOfVertexSampleIndex = MTLCounterDontSample;
    a.startOfFragmentSampleIndex = MTLCounterDontSample;
    a.endOfFragmentSampleIndex = end >= 0 ? (NSUInteger)end : MTLCounterDontSample;
    if (start >= 0 && !(g_ts_started & (1u << start)))
        a.startOfVertexSampleIndex = (NSUInteger)start, g_ts_started |= (uint8_t)(1u << start);
    if (end >= 0)
        g_ts_started |= (uint8_t)(1u << end);
}

/* at the frame's end: when it completes, its marks into the profile's sums */
static void ts_frame_end(id<MTLCommandBuffer> c)
{
    if (!gfx_profiling || g_ts_ok <= 0 || !g_ts_started)
    {
        g_ts_started = 0;
        return;
    }
    id<MTLCounterSampleBuffer> b = g_ts[g_frame];
    uint8_t got = g_ts_started;
    double scale = g_ts_scale;
    g_ts_started = 0;
    [c addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        NSData* d = [b resolveCounterRange:NSMakeRange(0, TS_N)];
        if (!d || d.length < TS_N * sizeof(MTLCounterResultTimestamp))
            return;
        const MTLCounterResultTimestamp* t = (const MTLCounterResultTimestamp*)d.bytes;
#define TS_SPAN(a, z, sum) \
    if ((got >> (a) & 1) && (got >> (z) & 1) && t[a].timestamp != MTLCounterErrorValue && t[z].timestamp != MTLCounterErrorValue && \
        t[z].timestamp > t[a].timestamp) \
        atomic_fetch_add(&(sum), (uint64_t)((double)(t[z].timestamp - t[a].timestamp) * scale));
        TS_SPAN(TS_SUN0, TS_SUN1, g_ts_sun_ns)
        TS_SPAN(TS_FX0, TS_FX1, g_ts_fx_ns)
        TS_SPAN(TS_FX0, TS_AO1, g_ts_ao_ns)
#undef TS_SPAN
    }];
}

static int g_ts_fx_end = TS_FX1; /* the end mark the effects' passes set (TS_AO1 for the occlusion's) */

/* one full-screen triangle into target, reading tex[0..n) */
static void fx_pass(id<MTLTexture> target, MTLLoadAction load, id<MTLRenderPipelineState> p, MTLViewport vp, const FxU* u,
    id<MTLTexture> const* tex, int n, const int32_t* dir)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = load;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    ts_mark(rp, TS_FX0, g_ts_fx_end);
    id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p];
    [e setViewport:vp];
    [e setFragmentBytes:u length:sizeof *u atIndex:0];
    if (dir)
        [e setFragmentBytes:dir length:8 atIndex:1];
    for (int i = 0; i < n; ++i)
        [e setFragmentTexture:tex[i] atIndex:(NSUInteger)i];
    [e setFragmentSamplerState:g_fx.samp atIndex:0];
    [e setFragmentSamplerState:g_fx.cmp atIndex:1];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

static MTLViewport fx_full(id<MTLTexture> t) { return (MTLViewport){ 0, 0, (double)t.width, (double)t.height, 0, 1 }; }

/* the occlusion's depth texture at w x h, four levels, with a view of each; made again when the size changes */
static id<MTLTexture> fx_lz(NSUInteger w, NSUInteger h)
{
    if (g_fx.lz && g_fx.lz.width == w && g_fx.lz.height == h)
        return g_fx.lz;
    [g_fx.lz release], g_fx.lz = nil;
    for (int i = 0; i < 4; ++i)
        [g_fx.lzv[i] release], g_fx.lzv[i] = nil;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:w height:h mipmapped:YES];
    d.mipmapLevelCount = 4;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
    g_fx.lz = [g_dev newTextureWithDescriptor:d];
    if (!g_fx.lz || g_fx.lz.mipmapLevelCount < 4)
        return [g_fx.lz release], g_fx.lz = nil;
    for (NSUInteger i = 0; i < 4; ++i)
        g_fx.lzv[i] = [g_fx.lz newTextureViewWithPixelFormat:MTLPixelFormatR32Float textureType:MTLTextureType2D
                                                      levels:NSMakeRange(i, 1) slices:NSMakeRange(0, 1)];
    return g_fx.lz;
}

enum { SUN_CACHE_FRAMES = 60 * 30 };
#define SUN_CACHE_NEAR 40.0f                  /* what was seen within this of the camera stays past SUN_CACHE_FRAMES */
#define SUN_COPY_BYTES (48u * 1024 * 1024)    /* the copies' vertices and indices, all told */
enum { SUN_PRIME_FRAMES = 3 };          /* frames the game draws the zone round the camera (gfx_sun_prime) */
#define SUN_PRIME_MOVE 20.0f              /* and again once the camera is this far from where it last did */

/* The zone's casters, kept after they leave the view: the game draws only what the camera sees, but
 * a low sun throws the shadows of what is behind and beside the camera into it. Each caster drawn
 * from buffers the game keeps is kept with a copy of its uniforms and that frame's camera (clip
 * space back to the world), and drawn into the map from there while it is out of view - for 30
 * seconds after it was last seen, longer while the camera stays within 40 units of where it was then,
 * and until the camera jumps (a new zone).
 *
 * Copies of one mesh (FFXI draws every tree of a kind from the same buffers, each placed by its vertex
 * shader's constants) share a key; each copy is told apart by where its first vertex stands
 * (draw_clip0), and entries of one key are chained. What the game draws from buffers it rewrites (a
 * swaying tree, a prop) is kept as a copy of its vertices and indices as last drawn. An entry in
 * plain view the game did not draw this frame has gone (moved, swapped for another level of detail,
 * no longer drawn) and is dropped. */
typedef struct CacheKey
{
    void* vb[GFX_NSTREAMS];
    NSUInteger voff[GFX_NSTREAMS];
    void* ib;
    NSUInteger ioff;
    uint32_t n, vstart, copy;
    LibKey lib;
} CacheKey;

typedef struct Cached
{
    Caster c; /* retains its buffers; c.ub is the cache's own copy of the uniforms */
    float clip_world[16];
    float pos[3], cam[3]; /* where it stood and where the camera was, when last seen */
    uint64_t seen, replayed;
    int32_t next;         /* the next entry with its key, -1 none */
    uint32_t copy_bytes;  /* a copy's vertices and indices (c.vb[] and c.ib are the copy) */
    int dead; /* a buffer it draws from was destroyed (the game let go of it: a zone left behind) */
} Cached;

static Cached* g_cache;
static uint32_t g_ncache, g_cache_cap, g_copy_bytes;
static Map g_cache_map; /* CacheKey -> the first entry's index + 1 */
static float g_cache_cam[3];
/* What the cache holds is only what the game drew, and it draws only what the camera sees: until the
 * camera had looked their way, the trees behind it cast nothing (at a low sun the character stood lit
 * in their shadow). So after the cache is cleared, and as the camera moves on, the game is asked for
 * a few frames to draw the zone round the camera, out of view as well (gfx_sun_prime, host64's
 * cull_test); out of view it shows nothing, and the cache keeps it. */
static int g_prime_set;
static uint64_t g_prime_serial; /* the frame it was asked for */
static float g_prime_at[3];

float gfx_sun_prime(float* center)
{
    if (!g_prime_set || g_serial - g_prime_serial >= SUN_PRIME_FRAMES || g_fxs.fx == 0.0f || g_fxs.sun_prime <= 0.0f || g_fxs.sun_casters == 1.0f)
        return 0.0f;
    memcpy(center, g_prime_at, sizeof g_prime_at);
    return g_fxs.sun_prime;
}

static void cache_key(CacheKey* k, const Caster* c, int copy)
{
    memset(k, 0, sizeof *k);
    if (!copy)
    {
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            k->vb[s] = (void*)c->vb[s], k->voff[s] = c->voff[s];
        k->ib = (void*)c->ib, k->ioff = c->ioff;
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
    Caster* c = &ce->c;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        [c->vb[s] release], c->vb[s] = nil;
    [c->ub release], c->ub = nil;
    [c->ib release], c->ib = nil;
    for (int t = 0; t < 8; ++t)
        [c->tex[t] release], c->tex[t] = nil;
    g_copy_bytes -= ce->copy_bytes, ce->copy_bytes = 0;
}

/* a buffer the game destroyed: what the cache draws from it goes */
static void sun_cache_forget(id<MTLBuffer> buf)
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
            map_put(&g_cache_map, &k, sizeof k, (id)(uintptr_t)(i + 1));
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
        map_put(&g_cache_map, k, sizeof *k, (id)(uintptr_t)(at + 1));
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
        const uint8_t* ip = (const uint8_t*)[c->ib contents] + c->ioff;
        if ((size_t)c->ioff + (size_t)c->n * c->itype > [c->ib length])
            return 0;
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
    uint32_t off[GFX_NSTREAMS], len[GFX_NSTREAMS], total = 0;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        off[s] = total, len[s] = 0;
        if (!c->vb[s] || c->vb[s] == g_dummy)
            continue;
        long stride = ub->stride[s], from = lo * stride, n = (hi - lo + 1) * stride;
        if (!stride)
            from = 0, n = 64;
        long have = (long)[c->vb[s] length] - (long)c->voff[s] - from;
        if (have <= 0)
            return 0;
        len[s] = (uint32_t)(n < have ? n : have);
        total += (len[s] + 15) & ~15u;
    }
    uint32_t ioff = total, ilen = c->ib ? c->n * c->itype : 0;
    total += (ilen + 15) & ~15u;
    if (!total)
        return 0;
    /* the copy's buffer again when it fits and no frame in flight reads it */
    id<MTLBuffer> buf = nil;
    if (ce->copy_bytes == total && ce->replayed + FRAMES < g_serial)
        for (int s = 0; s < GFX_NSTREAMS && !buf; ++s)
            if (ce->c.vb[s] && ce->c.vb[s] != g_dummy)
                buf = ce->c.vb[s];
    if (!buf)
    {
        if (!copy_room(total, ce))
            return 0;
        buf = [g_dev newBufferWithLength:total options:MTLResourceStorageModeShared];
        if (!buf)
            return 0;
    }
    else
        [buf retain];
    uint8_t* dst = (uint8_t*)[buf contents];
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (len[s])
            memcpy(dst + off[s], (const uint8_t*)[c->vb[s] contents] + c->voff[s] + (ub->stride[s] ? lo * ub->stride[s] : 0), len[s]);
    if (ilen)
        memcpy(dst + ioff, (const uint8_t*)[c->ib contents] + c->ioff, ilen);
    /* the copy's own buffers in place of the caster's */
    g_copy_bytes -= ce->copy_bytes;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        [ce->c.vb[s] release];
        ce->c.vb[s] = len[s] ? [buf retain] : [g_dummy retain];
        ce->c.voff[s] = len[s] ? off[s] : 0;
    }
    [ce->c.ib release];
    ce->c.ib = ilen ? [buf retain] : nil, ce->c.ioff = ioff;
    [buf release];
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
 * plain view the game did not draw dropped. clip_world: the camera's clip space to the world; view:
 * its view matrix; vp: the world to its clip space; cam: where it is. */
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
        g_prime_set = 0;
    }
    memcpy(g_cache_cam, cam, 12);
    float px = cam[0] - g_prime_at[0], py = cam[1] - g_prime_at[1], pz = cam[2] - g_prime_at[2];
    if (!g_prime_set || px * px + py * py + pz * pz > SUN_PRIME_MOVE * SUN_PRIME_MOVE)
    {
        if (gfx_profiling && g_fxs.sun_prime > 0.0f)
            fprintf(stderr, "[recomp] gfx: shadows: the zone within %.0f units of (%.1f %.1f %.1f) asked for (frame %llu)\n",
                g_fxs.sun_prime, cam[0], cam[1], cam[2], (unsigned long long)g_serial);
        memcpy(g_prime_at, cam, 12);
        g_prime_set = 1, g_prime_serial = g_serial;
    }
    if (!(g_serial & 255))
        sun_cache_trim(0);
    /* what is not drawn from the zone's own buffers: a placed object (keep: kept as a copy) or a
     * character (not kept: its vertices are in the world already, and it moves). A fixed-function
     * draw whose world matrix is the identity is a character; a vertex shader's draw is the zone's. */
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
            const GfxU* src = (const GfxU*)((const uint8_t*)[c->ub contents] + c->uoff);
            float w[16], off = 0.0f;
            gfx_mat_mul(w, src->wv, invView);
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
        const GfxU* src = (const GfxU*)((const uint8_t*)[c->ub contents] + c->uoff);
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
            for (int s = 0; s < GFX_NSTREAMS; ++s)
                ce->c.vb[s] = copy ? nil : [ce->c.vb[s] retain];
            ce->c.ib = copy ? nil : [ce->c.ib retain];
            for (int t = 0; t < 8; ++t)
                ce->c.tex[t] = alpha_tested(&c->lib.fs) ? [ce->c.tex[t] retain] : nil;
            ce->c.ub = nil;
        }
        else if (copy)
        {
            /* the textures as of this frame (an alpha test's) */
            for (int t = 0; t < 8 && alpha_tested(&c->lib.fs); ++t)
            {
                [c->tex[t] retain];
                [ce->c.tex[t] release];
                ce->c.tex[t] = c->tex[t], ce->c.samp[t] = c->samp[t];
            }
            ce->c.n = c->n, ce->c.vstart = c->vstart, ce->c.prim = c->prim, ce->c.itype = c->itype;
        }
        /* the uniforms as of this frame: in place, unless a frame the GPU may still be drawing
         * read them */
        if (ce->c.ub && ce->replayed + FRAMES < g_serial)
            memcpy([ce->c.ub contents], src, sizeof(GfxU));
        else
        {
            [ce->c.ub release];
            ce->c.ub = [g_dev newBufferWithBytes:src length:sizeof(GfxU) options:MTLResourceStorageModeShared];
        }
        ce->c.uoff = 0;
        if (copy && !cache_copy(ce, c, (GfxU*)[ce->c.ub contents]))
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
    /* in plain view, and the game did not draw it: it has gone (moved, another level of detail, no
     * longer drawn) */
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

#define GI_MAP 1024 /* the bounce light's map's texels across */

static id<MTLTexture> sun_target(id<MTLTexture>* t, int size)
{
    if (*t && (*t).width != (NSUInteger)size)
        [*t release], *t = nil;
    if (!*t)
    {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                      width:size height:size mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        *t = [g_dev newTextureWithDescriptor:td];
    }
    if (t == &g_fx.gimap)
        return *t;
    id<MTLTexture>* col = t == &g_fx.smapn ? &g_fx.scol8 : &g_fx.scol;
    if (*col && (*col).width != (NSUInteger)size)
        [*col release], *col = nil;
    if (!*col)
    {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                      width:size height:size mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeMemoryless;
        *col = [g_dev newTextureWithDescriptor:td];
    }
    return *t && *col ? *t : nil;
}

/* the casters into one cascade's map: this frame's, and the zone's kept from before (cache). With col,
 * the bounce light's: every caster (whoever casts) in its own colour as well, through its own pixel
 * function, unfogged (gfx_d3d12.c's) */
static uint32_t sun_draw(id<MTLTexture> target, id<MTLTexture> col, const float* invP, const float* invV, const SunCascade* k, int cache)
{
    float clip_world[16], M[16];
    gfx_mat_mul(clip_world, invP, invV);
    gfx_mat_mul(M, clip_world, k->S); /* the camera's clip space -> the map */
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.texture = target;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].texture = col ? col : target == g_fx.smapn ? g_fx.scol8 : g_fx.scol;
    rp.colorAttachments[0].loadAction = col ? MTLLoadActionClear : MTLLoadActionDontCare;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rp.colorAttachments[0].storeAction = col ? MTLStoreActionStore : MTLStoreActionDontCare;
    ts_mark(rp, TS_SUN0, TS_SUN1);
    id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
    [e setViewport:(MTLViewport){ 0, 0, (double)k->size, (double)k->size, 0, 1 }];
    [e setDepthStencilState:g_fx.sdepth];
    [e setCullMode:MTLCullModeNone];
    [e setDepthBias:0 slopeScale:1.5f clamp:0];
    uint32_t drawn = 0, total = g_ncasters + (cache ? g_ncache : 0);
    for (uint32_t i = 0; i < total; ++i)
    {
        const Caster* cs;
        if (i < g_ncasters)
        {
            cs = &g_casters[i];
            if (i == 0)
                [e setVertexBytes:M length:64 atIndex:5];
            /* sun_casters 1: characters alone cast - the zone's shadows are baked into its colours
             * already, and the game tints them for the hour and the weather (the bounce light's map has
             * them all) */
            if (!col && ((g_fxs.sun_casters == 1.0f && (cs->fixed || cs->keep)) || (g_fxs.sun_casters == 2.0f && !cs->fixed && !cs->keep)))
                continue;
            /* this frame's too: more than 96 units outside the map's sides (the bounce light's: 16), no
             * shadow of it falls in it */
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
            if (ce->dead || ce->seen == g_serial || cached_expired(ce))
                continue;
            /* standing more than 96 units outside the map's sides: no shadow of it falls in the map
             * (with a long draw distance the cache holds thousands, each drawn into both maps) */
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
            [e setVertexBytes:m length:64 atIndex:5];
            ce->replayed = g_serial;
            cs = &ce->c;
        }
        PipeKey pk;
        memset(&pk, 0, sizeof pk);
        pk.lib = cs->lib;
        pk.lib.vs.shadow = 1, pk.lib.vs.pixel = 0;
        int at = alpha_tested(&cs->lib.fs), tex = at || col;
        if (col)
            pk.lib.fs.fog = 0, pk.pipe.write_mask = 15; /* its colour as the sun sees it: no fog of the camera's */
        else if (!at)
        {
            /* the position alone: one pipeline serves every draw with the same vertex layout */
            GfxVsKey* v = &pk.lib.vs;
            v->lighting = v->normalize = v->localviewer = v->specular = 0;
            v->src_diffuse = v->src_specular = v->src_ambient = v->src_emissive = 0;
            v->nlights = 0, memset(v->light_type, 0, sizeof v->light_type);
            v->fog_vertex = v->range_fog = 0, v->ntex = 0, v->flat = 0;
            memset(v->tci, 0, sizeof v->tci), memset(v->ttf, 0, sizeof v->ttf);
            memset(&pk.lib.fs, 0, sizeof pk.lib.fs);
        }
        pk.color = (uint32_t)(col ? col.pixelFormat : MTLPixelFormatR8Unorm), pk.depth = (uint32_t)MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> ps = pipeline_for(&pk, cs->vs, cs->ps);
        if (!ps)
        {
            g_fx.tr_skipped++;
            continue;
        }
        if (i < g_ncasters)
            g_fx.tr_live++;
        else
            g_fx.tr_cached++;
        [e setRenderPipelineState:ps];
        for (int st = 0; st < GFX_NSTREAMS; ++st)
            [e setVertexBuffer:cs->vb[st] offset:cs->voff[st] atIndex:(NSUInteger)st];
        [e setVertexBuffer:cs->ub offset:cs->uoff atIndex:4];
        if (tex)
        {
            [e setFragmentBuffer:cs->ub offset:cs->uoff atIndex:4];
            for (int t = 0; t < 8; ++t)
                if (cs->tex[t])
                {
                    GfxSampler sk = cs->samp[t];
                    [e setFragmentTexture:cs->tex[t] atIndex:(NSUInteger)t];
                    [e setFragmentSamplerState:sampler(&sk) atIndex:(NSUInteger)t];
                }
        }
        if (cs->itype)
            [e drawIndexedPrimitives:cs->prim indexCount:cs->n
                           indexType:cs->itype == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                         indexBuffer:cs->ib indexBufferOffset:cs->ioff];
        else
            [e drawPrimitives:cs->prim vertexStart:cs->vstart vertexCount:cs->n];
        drawn++;
    }
    [e endEncoding];
    return drawn;
}

/* The sun's maps for the scene: a near cascade out to sun_near units past the player, where
 * characters stand and shadows are looked at closely (a 4096 map over some 40 units: texels of a
 * centimetre), and a far one out to sun_distance. Fills the effects' uniforms for both; 0 when
 * nothing was drawn. */
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
    float dfar = fmaxf(g_fxs.sun_distance, 4.0f), dnear = fminf(fmaxf(g_fxs.sun_near, 0.0f), dfar);
    SunCascade far, near;
    gfx_sun_fit(s, invV, L, 0.5f, dfar, GFX_SUN_MAP, &far);
    if (!sun_target(&g_fx.smap, GFX_SUN_MAP))
        return 0;
    uint32_t drawn = sun_draw(g_fx.smap, nil, invP, invV, &far, 1);
    memcpy(u->lmat, far.lmat, 64);
    u->smap[1] = far.texel, u->smap[2] = far.bias, u->smap[3] = far.soft;
    u->smap2[0] = far.slope, u->smap2[3] = far.range;
    u->smapn2[1] = 0.0f;
    /* the near map reaches sun_near past the player, not the camera: the camera stands some units
     * behind the character, and a reach from it left the far half of the player's own shadow in the
     * coarse map. A player farther off than 40 units is not the one in view (a cutscene) */
    float lead = 0.0f;
    if (g_fx.has_focus)
    {
        float dx = g_fx.focus[0] - invV[12], dy = g_fx.focus[1] - invV[13], dz = g_fx.focus[2] - invV[14];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        lead = d < 40.0f ? d : 0.0f;
    }
    float tnear = dnear + lead;
    int nsize = gfx_sun_near_size();
    if (dnear >= 2.0f && tnear < dfar && sun_target(&g_fx.smapn, nsize))
    {
        gfx_sun_fit(s, invV, L, 0.5f, tnear, nsize, &near);
        sun_draw(g_fx.smapn, nil, invP, invV, &near, 1);
        memcpy(u->lmatn, near.lmat, 64);
        u->smapn[0] = near.texel, u->smapn[1] = near.bias, u->smapn[2] = near.soft, u->smapn[3] = near.slope;
        u->smapn2[0] = near.range, u->smapn2[1] = 1.0f;
    }
    /* the bounce light's map: this frame's casters over the first gi_distance units the camera sees, in
     * colour, 1024 across, and its levels below (gfx_d3d12.c's) */
    u->gi[0] = 0.0f;
    float gd = fminf(fmaxf(g_fxs.gi_distance, 8.0f), dfar);
    if (g_fxs.gi > 0.0f && sun_target(&g_fx.gimap, GI_MAP))
    {
        if (!g_fx.gicol)
        {
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:GI_MAP height:GI_MAP mipmapped:YES];
            td.mipmapLevelCount = 4;
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            g_fx.gicol = [g_dev newTextureWithDescriptor:td];
        }
        SunCascade gk;
        float inv[16];
        gfx_sun_fit(s, invV, L, 0.5f, gd, GI_MAP, &gk);
        if (g_fx.gicol && sun_draw(g_fx.gimap, g_fx.gicol, invP, invV, &gk, 0) && gfx_mat_inverse(inv, gk.lmat))
        {
            id<MTLBlitCommandEncoder> b = [cmd() blitCommandEncoder];
            [b generateMipmapsForTexture:g_fx.gicol];
            [b endEncoding];
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
 * The world for rays (gfx_d3d12.c's, which says more), made from the frame's casters as the sun's maps
 * are: each drawn once more through its own vertex function (the capture key, GfxVsKey.shadow 2: the
 * function runs once a corner of its triangles, rasterization off, and writes the corner's point in this
 * frame's view space into one buffer, gfx_msl.c emit_rt_index), one structure built over each of the
 * solid triangles and the alpha-tested ones, and one over both on top, every frame. The casters out of
 * view come from the sun's cache, through the camera they were drawn with. Rays are traced inline
 * (intersection queries) from the scene effects' passes (RT_MSL). Where the GPU cannot, or before macOS
 * 13 (the textures' resource IDs), none are. */
#define RT_REACH 150.0f /* the cache's casters further than this from the camera are left out */

static struct
{
    int tried, ok;
    int hw; /* the GPU traces rays in hardware (Apple's M3 and A17 on): else it does in its shaders, some four times slower */
    id<MTLLibrary> lib;
    id<MTLRenderPipelineState> gi, ao, giblur, clay;
    MTLPixelFormat clay_fmt;
    id<MTLComputePipelineState> nclear, nsum, nresolve;
    id<MTLBuffer> tri, nrm, table, scratch; /* the corners, their smooth normals, the table that finds them */
    id<MTLAccelerationStructure> blas[2], tlas; /* (blas[1]: the alpha-tested) */
    id<MTLTexture>* texs;                       /* the alpha tests' textures this frame (their table's) */
    uint32_t ntexs, texs_cap;
    id<MTLBuffer> tab, texbuf; /* this frame's alpha tests' table and their textures' IDs (the ring's) */
    NSUInteger tab_off, texbuf_off;
    uint32_t rtp[4]; /* RT_MSL RtU */
    uint32_t verts;
    /* the profile: casters captured, from the cache, skipped (no pipeline yet), triangles */
    uint32_t st_live, st_cached, st_skipped, st_frames;
    uint64_t st_tris;
} g_ray;

static id<MTLRenderPipelineState> rt_fx_pipeline(NSString* frag, MTLPixelFormat fmt);

static int rt_init(void)
{
    if (g_ray.tried)
        return g_ray.ok;
    g_ray.tried = 1;
    if (@available(macOS 13.0, *))
    {
        if (!g_dev.supportsRaytracing || ![g_dev supportsFamily:MTLGPUFamilyApple7] && ![g_dev supportsFamily:MTLGPUFamilyMac2])
        {
            fprintf(stderr, "[recomp] gfx: ray tracing: not on this GPU\n");
            return 0;
        }
    }
    else
    {
        fprintf(stderr, "[recomp] gfx: ray tracing: needs macOS 13\n");
        return 0;
    }
    size_t a = strlen(FX_MSL), b = strlen(RT_MSL);
    char* src = (char*)malloc(a + b + 1);
    memcpy(src, FX_MSL, a), memcpy(src + a, RT_MSL, b + 1);
    g_ray.lib = compile(src);
    free(src);
    if (!g_ray.lib)
        return 0;
    g_ray.gi = rt_fx_pipeline(@"rt_gi", MTLPixelFormatRGBA16Float);
    g_ray.ao = rt_fx_pipeline(@"rt_ao", MTLPixelFormatRGBA16Float);
    g_ray.giblur = rt_fx_pipeline(@"rt_giblur", MTLPixelFormatRGBA16Float);
    id<MTLComputePipelineState>* cs[3] = { &g_ray.nclear, &g_ray.nsum, &g_ray.nresolve };
    NSString* names[3] = { @"rt_nclear", @"rt_nsum", @"rt_nresolve" };
    g_ray.ok = g_ray.gi && g_ray.ao && g_ray.giblur;
    for (int i = 0; i < 3; ++i)
    {
        id<MTLFunction> f = [g_ray.lib newFunctionWithName:names[i]];
        NSError* err = nil;
        *cs[i] = f ? [g_dev newComputePipelineStateWithFunction:f error:&err] : nil;
        [f release];
        g_ray.ok &= *cs[i] != nil;
    }
    if (@available(macOS 14.0, *))
        g_ray.hw = [g_dev supportsFamily:MTLGPUFamilyApple9];
    fprintf(stderr, !g_ray.ok ? "[recomp] gfx: ray tracing: its pipelines failed\n"
        : g_ray.hw ? "[recomp] gfx: ray tracing ready\n"
        : "[recomp] gfx: ray tracing ready (no ray tracing hardware: traced at half the occlusion's resolution)\n");
    return g_ray.ok;
}

int gfx_rt_supported(void) { return rt_init(); }

/* one of RT_MSL's passes into fmt */
static id<MTLRenderPipelineState> rt_fx_pipeline(NSString* frag, MTLPixelFormat fmt)
{
    id<MTLLibrary> keep = g_fx.lib;
    g_fx.lib = g_ray.lib;
    id<MTLRenderPipelineState> p = fx_pipeline(frag, fmt);
    g_fx.lib = keep;
    return p;
}

/* a private buffer of at least need bytes in *b (made again, half as large again, when smaller) */
static int rt_buffer(id<MTLBuffer>* b, NSUInteger need)
{
    if (*b && (*b).length >= need)
        return 1;
    [*b release];
    need = (need + need / 2 + 65535) & ~(NSUInteger)65535;
    *b = [g_dev newBufferWithLength:need options:MTLResourceStorageModePrivate];
    if (!*b)
        fprintf(stderr, "[recomp] gfx: ray tracing: a buffer of %lu bytes failed\n", (unsigned long)need);
    return *b != nil;
}

static int rt_structure(id<MTLAccelerationStructure>* s, NSUInteger need)
{
    if (*s && (*s).size >= need)
        return 1;
    [*s release];
    *s = [g_dev newAccelerationStructureWithSize:need + need / 2];
    return *s != nil;
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
    return (c->fixed || c->keep) && alpha_tested(&c->lib.fs) && c->lib.vs.ntex >= 1 && c->tex[0] &&
        c->tex[0].textureType == MTLTextureType2D;
}

static uint32_t rt_tris(const Caster* c)
{
    if (c->prim == MTLPrimitiveTypeTriangle)
        return c->n / 3;
    if (c->prim == MTLPrimitiveTypeTriangleStrip)
        return c->n >= 3 ? c->n - 2 : 0;
    return 0;
}

static id<MTLRenderPipelineState> rt_pipeline(const Caster* c, int alpha)
{
    PipeKey pk;
    memset(&pk, 0, sizeof pk);
    pk.lib = c->lib;
    GfxVsKey* vk = &pk.lib.vs;
    vk->shadow = 2, vk->pixel = 0, vk->water = 0;
    /* the position alone, as the sun's depth maps draw it - and an alpha test's texture coordinates and
     * diffuse alpha (its colour sources kept, lit by none of the lights: the alpha is the material's) */
    vk->normalize = vk->localviewer = vk->specular = 0;
    vk->nlights = 0, memset(vk->light_type, 0, sizeof vk->light_type);
    vk->fog_vertex = vk->range_fog = 0, vk->flat = 0;
    if (!alpha)
    {
        vk->lighting = 0, vk->src_diffuse = vk->src_specular = vk->src_ambient = vk->src_emissive = 0;
        vk->ntex = 0, memset(vk->tci, 0, sizeof vk->tci), memset(vk->ttf, 0, sizeof vk->ttf);
    }
    memset(&pk.lib.fs, 0, sizeof pk.lib.fs);
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
            if (ce->dead || cached_expired(ce) || !ce->c.n || ce->seen == g_serial)
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
        it->alpha = (uint8_t)rt_alpha(it->c);
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
    if (!rt_buffer(&g_ray.tri, (NSUInteger)verts * 32))
        return 0;
    /* the capture: a pass with nothing to draw into, each caster's corners written by its function */
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.renderTargetWidth = 1, rp.renderTargetHeight = 1, rp.defaultRasterSampleCount = 1;
    id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
    [e setVertexBuffer:g_ray.tri offset:0 atIndex:6];
    uint64_t vsolid = 0, valpha = 0;
    uint32_t first = 0, live_m = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        const RtItem* it = &items[i];
        const Caster* cs = it->c;
        [e setRenderPipelineState:rt_pipeline(cs, it->alpha)];
        if (it->c >= g_casters && it->c < g_casters + g_ncasters)
        {
            if (!live_m)
                [e setVertexBytes:it->m length:64 atIndex:5], live_m = 1;
        }
        else
            [e setVertexBytes:it->m length:64 atIndex:5], live_m = 0;
        for (int st = 0; st < GFX_NSTREAMS; ++st)
            [e setVertexBuffer:cs->vb[st] offset:cs->voff[st] atIndex:(NSUInteger)st];
        [e setVertexBuffer:cs->ub offset:cs->uoff atIndex:4];
        uint32_t rtc[4] = { first, cs->itype, cs->prim == MTLPrimitiveTypeTriangleStrip, cs->itype ? 0 : cs->vstart };
        [e setVertexBytes:rtc length:16 atIndex:7];
        [e setVertexBuffer:cs->itype ? cs->ib : g_dummy offset:cs->itype ? cs->ioff : 0 atIndex:8];
        [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:it->tris * 3];
        first += it->tris * 3;
        *(it->alpha ? &valpha : &vsolid) += (uint64_t)it->tris * 3;
        if (cs >= g_casters && cs < g_casters + g_ncasters)
            g_ray.st_live++;
        else
            g_ray.st_cached++;
    }
    [e endEncoding];
    if (!vsolid) /* (the passes' rays that skip the leaves trace the solid ones' structure) */
        return 0;
    /* the alpha tests' table: each one's first triangle, its texture (its index in texs), its sampler's
     * address mode (0 wrap, 1 clamp, 2 mirror), its test */
    uint32_t nalpha = 0;
    for (uint32_t i = 0; i < n; ++i)
        nalpha += items[i].alpha;
    g_ray.ntexs = 0, g_ray.tab = g_ray.texbuf = g_dummy, g_ray.tab_off = g_ray.texbuf_off = 0;
    if (nalpha)
    {
        uint32_t* t = (uint32_t*)ring((size_t)nalpha * 16, 16, &g_ray.tab, &g_ray.tab_off);
        uint64_t* ids = (uint64_t*)ring((size_t)nalpha * 8, 16, &g_ray.texbuf, &g_ray.texbuf_off);
        if (g_ray.texs_cap < nalpha)
            g_ray.texs_cap = nalpha + nalpha / 2, g_ray.texs = (id<MTLTexture>*)realloc(g_ray.texs, g_ray.texs_cap * sizeof(id));
        uint32_t at = (uint32_t)(vsolid / 3);
        for (uint32_t i = 0; i < n; ++i)
        {
            const RtItem* it = &items[i];
            if (!it->alpha)
                continue;
            const Caster* c = it->c;
            const GfxU* gu = (const GfxU*)((const uint8_t*)[c->ub contents] + c->uoff);
            float ref = c->ub.storageMode == MTLStorageModeShared ? gu->params[1] : 128.0f;
            uint8_t au = c->samp[0].addr_u; /* D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 mirror once */
            uint32_t am = au == 3 || au == 4 ? 1 : au == 2 || au == 5 ? 2 : 0;
            const GfxStage* st = &c->lib.fs.st[0];
            if (!c->lib.fs.prog && c->lib.fs.nstages) /* (a pixel shader's alpha: the texture's alone) */
                am |= (uint32_t)(st->aop & 63) << 2 | (uint32_t)st->aa1 << 8 | (uint32_t)st->aa2 << 16;
            float tfa = c->ub.storageMode == MTLStorageModeShared ? gu->tfactor[3] : 1.0f;
            t[0] = at, t[1] = g_ray.ntexs, t[2] = am;
            t[3] = (uint32_t)c->lib.fs.alpha_func << 8 | (uint32_t)fminf(fmaxf(ref, 0.0f), 255.0f) |
                (uint32_t)(fminf(fmaxf(tfa, 0.0f), 1.0f) * 255.0f + 0.5f) << 16;
            if (@available(macOS 13.0, *))
                ids[g_ray.ntexs] = c->tex[0].gpuResourceID._impl;
            g_ray.texs[g_ray.ntexs++] = c->tex[0];
            t += 4, at += it->tris;
        }
    }
    /* a structure over the solid triangles and one over the alpha-tested, and the one over both: two
     * instances, masks 1 and 2 (the bounce light's and occlusion's rays, mask 1, skip the leaves whole) */
    MTLPrimitiveAccelerationStructureDescriptor* pd[2] = { nil, nil };
    MTLAccelerationStructureSizes sz[2];
    NSUInteger scratch = 0, soff[2] = { 0, 0 };
    for (int k = 0; k < 2; ++k)
    {
        uint64_t from = k ? vsolid : 0, count = k ? valpha : vsolid;
        if (!count)
            continue;
        MTLAccelerationStructureTriangleGeometryDescriptor* g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        g.vertexBuffer = g_ray.tri, g.vertexBufferOffset = (NSUInteger)from * 32, g.vertexStride = 32;
        g.triangleCount = (NSUInteger)(count / 3);
        g.opaque = k == 0;
        pd[k] = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        pd[k].geometryDescriptors = @[ g ];
        pd[k].usage = MTLAccelerationStructureUsagePreferFastBuild; /* (built for trace speed it cost more than it saved) */
        sz[k] = [g_dev accelerationStructureSizesWithDescriptor:pd[k]];
        if (!rt_structure(&g_ray.blas[k], sz[k].accelerationStructureSize))
            return 0;
        soff[k] = scratch, scratch += (sz[k].buildScratchBufferSize + 255) & ~(NSUInteger)255;
    }
    /* the instances: the solid (mask 1), then the alpha-tested (mask 2), whose index RT_MSL takes as theirs */
    MTLAccelerationStructureInstanceDescriptor* inst;
    id<MTLBuffer> ibuf;
    NSUInteger ioff;
    inst = (MTLAccelerationStructureInstanceDescriptor*)ring(2 * sizeof *inst, 16, &ibuf, &ioff);
    memset(inst, 0, 2 * sizeof *inst);
    NSMutableArray* used = [NSMutableArray arrayWithCapacity:2];
    uint32_t ninst = 0;
    g_ray.rtp[2] = 2;
    for (int k = 0; k < 2; ++k)
        if (pd[k])
        {
            MTLAccelerationStructureInstanceDescriptor* d = &inst[ninst];
            d->transformationMatrix.columns[0] = (MTLPackedFloat3){ { { 1, 0, 0 } } };
            d->transformationMatrix.columns[1] = (MTLPackedFloat3){ { { 0, 1, 0 } } };
            d->transformationMatrix.columns[2] = (MTLPackedFloat3){ { { 0, 0, 1 } } };
            d->options = k ? MTLAccelerationStructureInstanceOptionNonOpaque : MTLAccelerationStructureInstanceOptionOpaque;
            d->options |= MTLAccelerationStructureInstanceOptionDisableTriangleCulling;
            d->mask = k ? 2 : 1;
            d->accelerationStructureIndex = ninst;
            if (k)
                g_ray.rtp[2] = ninst;
            [used addObject:g_ray.blas[k]];
            ninst++;
        }
    MTLInstanceAccelerationStructureDescriptor* td = [MTLInstanceAccelerationStructureDescriptor descriptor];
    td.instanceDescriptorBuffer = ibuf, td.instanceDescriptorBufferOffset = ioff;
    td.instanceCount = ninst, td.instancedAccelerationStructures = used;
    td.usage = MTLAccelerationStructureUsagePreferFastBuild;
    MTLAccelerationStructureSizes tsz = [g_dev accelerationStructureSizesWithDescriptor:td];
    if (!rt_structure(&g_ray.tlas, tsz.accelerationStructureSize))
        return 0;
    NSUInteger tscratch = scratch;
    scratch += tsz.buildScratchBufferSize;
    if (!rt_buffer(&g_ray.scratch, scratch))
        return 0;
    id<MTLAccelerationStructureCommandEncoder> ae = [cmd() accelerationStructureCommandEncoder];
    for (int k = 0; k < 2; ++k)
        if (pd[k])
            [ae buildAccelerationStructure:g_ray.blas[k] descriptor:pd[k] scratchBuffer:g_ray.scratch scratchBufferOffset:soff[k]];
    [ae endEncoding];
    ae = [cmd() accelerationStructureCommandEncoder]; /* (after the two it stands on) */
    [ae buildAccelerationStructure:g_ray.tlas descriptor:td scratchBuffer:g_ray.scratch scratchBufferOffset:tscratch];
    [ae endEncoding];
    /* the corners' smooth normals (RT_MSL rt_nclear, rt_nsum, rt_nresolve) */
    uint32_t slots = 1024;
    while (slots < verts * 2u && slots < (1u << 26))
        slots *= 2;
    if (!rt_buffer(&g_ray.nrm, (NSUInteger)verts * 16) || !rt_buffer(&g_ray.table, (NSUInteger)slots * 16))
        return 0;
    uint32_t nc[4] = { (uint32_t)(verts / 3), slots - 1, (uint32_t)verts, 0 };
    id<MTLComputeCommandEncoder> ce = [cmd() computeCommandEncoder];
    [ce setBytes:nc length:16 atIndex:0];
    [ce setBuffer:g_ray.table offset:0 atIndex:1];
    [ce setBuffer:g_ray.tri offset:0 atIndex:2];
    [ce setBuffer:g_ray.nrm offset:0 atIndex:3];
    id<MTLComputePipelineState> passes[3] = { g_ray.nclear, g_ray.nsum, g_ray.nresolve };
    uint32_t counts[3] = { slots, (uint32_t)(verts / 3), (uint32_t)verts };
    for (int i = 0; i < 3; ++i)
    {
        [ce setComputePipelineState:passes[i]];
        [ce dispatchThreads:MTLSizeMake(counts[i], 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    }
    [ce endEncoding];
    g_ray.rtp[0] = (uint32_t)(vsolid / 3), g_ray.rtp[1] = nalpha, g_ray.rtp[3] = 0;
    g_ray.verts = (uint32_t)verts;
    g_ray.st_tris += verts / 3, g_ray.st_frames++;
    return 1;
}

/* one of ray tracing's passes (fx_pass, with the world bound: RT_MSL's buffers 2 to 7) */
static void rt_pass(id<MTLTexture> target, MTLLoadAction load, id<MTLRenderPipelineState> p, MTLViewport vp, const FxU* u,
    id<MTLTexture> const* tex, int n, const int32_t* dir)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = load;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    ts_mark(rp, TS_FX0, g_ts_fx_end);
    id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p];
    [e setViewport:vp];
    [e setFragmentBytes:u length:sizeof *u atIndex:0];
    if (dir)
        [e setFragmentBytes:dir length:8 atIndex:1];
    [e setFragmentBytes:g_ray.rtp length:16 atIndex:2];
    [e setFragmentBuffer:g_ray.tri offset:0 atIndex:3];
    [e setFragmentBuffer:g_ray.nrm offset:0 atIndex:4];
    [e setFragmentBuffer:g_ray.tab offset:g_ray.tab_off atIndex:5];
    [e setFragmentBuffer:g_ray.texbuf offset:g_ray.texbuf_off atIndex:6];
    [e setFragmentAccelerationStructure:g_ray.tlas atBufferIndex:7];
    [e setFragmentAccelerationStructure:g_ray.blas[0] atBufferIndex:8];
    if (@available(macOS 13.0, *))
    {
        for (int k = 0; k < 2; ++k)
            if (g_ray.blas[k])
                [e useResource:g_ray.blas[k] usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
        if (g_ray.ntexs)
            [e useResources:g_ray.texs count:g_ray.ntexs usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    }
    for (int i = 0; i < n; ++i)
        [e setFragmentTexture:tex[i] atIndex:(NSUInteger)i];
    [e setFragmentSamplerState:g_fx.samp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
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
 * The game's water draws (GfxDraw.water: vertices from its water models) come after the scene is done,
 * so they have its camera and light (water_scene) and can be drawn over a copy of what is behind them
 * (water_capture, once a frame, at the first water draw). */
static void water_scene(const GfxScene* s, const float* vinv, const FxU* u, id<MTLTexture> ct)
{
    struct WaterU* w = &g_fx.wu;
    memcpy(w->iv, vinv, 64), memcpy(w->view, s->view, 64);
    memcpy(w->zp, u->zp, 16);
    w->hand[0] = u->hand[0], w->hand[1] = (float)fmod(CACurrentMediaTime(), 3600.0);
    w->hand[2] = g_fxs.water_refract, w->hand[3] = g_fxs.water_foam;
    w->size[0] = (float)ct.width, w->size[1] = (float)ct.height, w->size[2] = 1.0f / (float)ct.width, w->size[3] = 1.0f / (float)ct.height;
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

/* how a draw is drawn as water (GfxFsKey.water), 0 as the game drew it: over the scene behind it when
 * it blends by its alpha, else only its edge softened */
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
    id<MTLTexture> ct = g_rt->tex, depth = depth_attachment();
    if (!depth || depth.width != ct.width || depth.height != ct.height || ct.sampleCount != 1 ||
        ct.width != (NSUInteger)g_fx.wu.size[0] || ct.height != (NSUInteger)g_fx.wu.size[1])
        return 0;
    if (g_fx.w_serial == g_serial && g_fx.w_from == ct)
        return 1;
    if (!fx_tex(&g_fx.wcol, ct.pixelFormat, ct.width, ct.height) || !fx_tex(&g_fx.wdep, depth.pixelFormat, ct.width, ct.height))
        return 0;
    flush_pass();
    id<MTLBlitCommandEncoder> b = [cmd() blitCommandEncoder];
    [b copyFromTexture:ct sourceSlice:0 sourceLevel:0 toTexture:g_fx.wcol destinationSlice:0 destinationLevel:0 sliceCount:1 levelCount:1];
    [b copyFromTexture:depth sourceSlice:0 sourceLevel:0 toTexture:g_fx.wdep destinationSlice:0 destinationLevel:0 sliceCount:1 levelCount:1];
    [b endEncoding];
    g_fx.w_from = ct, g_fx.w_serial = g_serial;
    return 1;
}

static void water_bind(void)
{
    [g_enc setFragmentTexture:g_fx.wcol atIndex:8];
    [g_enc setFragmentTexture:g_fx.wdep atIndex:9];
    [g_enc setFragmentBytes:&g_fx.wu length:sizeof g_fx.wu atIndex:5];
}

/* The finished scene anti-aliased (g_fxs.aa: FXAA), within its viewport, before the interface goes on */
static void scene_aa(GfxTex* color, const GfxScene* s)
{
    if (g_fxs.aa < 0.5f || !fx_init())
        return;
    @autoreleasepool
    {
        flush_pass();
        id<MTLTexture> ct = color->tex;
        float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
        if (vw < 16 || vh < 16 || vx + vw > ct.width || vy + vh > ct.height)
            vx = vy = 0, vw = (float)ct.width, vh = (float)ct.height;
        if (!g_fx.aa_pipe || g_fx.aa_fmt != ct.pixelFormat)
        {
            [g_fx.aa_pipe release];
            g_fx.aa_pipe = fx_pipeline(@"fx_fxaa", ct.pixelFormat);
            g_fx.aa_fmt = ct.pixelFormat;
        }
        if (!g_fx.aa_pipe || !fx_tex(&g_fx.aa_src, ct.pixelFormat, ct.width, ct.height))
            return;
        id<MTLBlitCommandEncoder> b = [cmd() blitCommandEncoder];
        [b copyFromTexture:ct sourceSlice:0 sourceLevel:0 toTexture:g_fx.aa_src destinationSlice:0 destinationLevel:0
                sliceCount:1 levelCount:1];
        [b endEncoding];
        FxU u;
        memset(&u, 0, sizeof u);
        u.size[0] = (float)ct.width, u.size[1] = (float)ct.height;
        fx_pass(ct, MTLLoadActionLoad, g_fx.aa_pipe, (MTLViewport){ vx, vy, vw, vh, 0, 1 }, &u, &g_fx.aa_src, 1, NULL);
        color->scene = 0; /* its mips are behind (scene_mips) */
        color->used = g_serial;
    }
}

static void scene_fx(GfxTex* color, const GfxScene* s);

void gfx_scene_done(GfxTex* color, const GfxScene* s)
{
    if (!g_dev || !color || color->type != GFX_TEX_2D)
        return;
    if (g_fxs.fx != 0.0f)
        scene_fx(color, s);
    scene_aa(color, s);
}

static void scene_fx(GfxTex* color, const GfxScene* s)
{
    @autoreleasepool
    {
        flush_pass();
        id<MTLTexture> ct = color->tex, depth = color->depth_world ? color->depth_world : color->depth_seen;
        g_fx.tr_depth = color->depth_world && color->depth_world != color->depth_seen ? 2 : depth ? 1 : 0;
        uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
        if (depth && depth.width == ct.width && depth.height == ct.height && s->proj[11] != 0.0f && fx_init())
        {
            /* the scene's viewport, within the target */
            float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
            if (vw < 16 || vh < 16 || vx + vw > ct.width || vy + vh > ct.height)
                vx = vy = 0, vw = (float)ct.width, vh = (float)ct.height;
            float minz, maxz;
            memcpy(&minz, &s->vp[4], 4);
            memcpy(&maxz, &s->vp[5], 4);
            if (maxz <= minz)
                minz = 0, maxz = 1;
            /* the occlusion at about 2000 pixels across (half a 4096 background, all of 1920): fine
             * enough that its edges hold still; bloom and rays, soft anyway, at about 1000 */
            uint32_t div = vw > 2048 ? 2 : 1, bdiv = vw > 2048 ? 4 : vw > 1024 ? 2 : 1;
            /* traced on a GPU with no ray tracing hardware (before Apple's M3): the occlusion and the bounce
             * light at half that, a quarter of the rays (at 1920 by 1080, an M1 Max's traced occlusion took
             * some 30 ms a frame, its bounce light 10) */
            if ((g_fxs.rt > 0.0f || (int)g_fxs.debug == 8) && rt_init() && !g_ray.hw)
                div *= 2;
            NSUInteger aw = (NSUInteger)((vw + div - 1) / div), ah = (NSUInteger)((vh + div - 1) / div);
            NSUInteger bw = (NSUInteger)((vw + bdiv - 1) / bdiv), bh = (NSUInteger)((vh + bdiv - 1) / bdiv);
            float hand = s->proj[11] < 0.0f ? -1.0f : 1.0f;
            FxU u = {
                { s->proj[0], s->proj[5], s->proj[8], s->proj[9] },
                { s->proj[10], s->proj[14], minz, maxz },
                { vx, vy, vw, vh },
                { (float)ct.width, (float)ct.height, (float)aw, (float)ah },
                { g_fxs.radius, g_fxs.ao, 0.15f, vh * 0.1f },
                { g_fxs.grade, g_fxs.sat, g_fxs.contrast, g_fxs.debug },
                { hand, 0, 0, 0 },
            };
            u.aop[0] = g_fxs.ao_quality >= 1.5f ? 16.0f : g_fxs.ao_quality >= 0.5f ? 10.0f : 6.0f;
            u.smapn2[2] = g_fxs.sun_soft <= 0.0f ? 1.0f : 0.0f;
            /* world up in view space: the world's y axis through the inverse view matrix, pointing
             * the way the camera's own up does (FFXI's world y points down) */
            int gap = g_fx.eased_serial && g_serial - g_fx.eased_serial > 30; /* half a second with no scene: a fade, a loading screen */
            g_fx.eased = g_fx.eased_serial && g_fx.eased_serial + 1 == g_serial;
            g_fx.eased_serial = g_serial;
            float inv[16], up[3];
            if (gfx_mat_inverse(inv, s->view))
            {
                /* a new place (the camera jumped, or a gap: logging in, a zone change, a moghouse): what
                 * is eased and the sun held take this one's at once - eased from the last place's, the
                 * sun's strength outdoors shaded a room from its ceiling for the seconds it took to fade */
                float jx = inv[12] - g_fx.last_cam[0], jy = inv[13] - g_fx.last_cam[1], jz = inv[14] - g_fx.last_cam[2];
                if (gap || jx * jx + jy * jy + jz * jz > 50.0f * 50.0f)
                    g_fx.eased = 0, g_fx.fogc_set = 0, g_fx.sunw_seen = 0;
                memcpy(g_fx.last_cam, inv + 12, 12);
                float sign = inv[5] < 0.0f ? -1.0f : 1.0f;
                up[0] = inv[1] * sign, up[1] = inv[5] * sign, up[2] = inv[9] * sign;
                gfx_normalize3(up);
                /* fog where the game fogs its world (its fog color is the zone's), fading in and out */
                float on = s->fog[2] != 0.0f ? 1.0f : 0.0f;
                fx_ease(&g_fx.fog_on, &on, 1, 0.1f);
                /* the fog's colour eased over about a second, whether or not the last scene was the frame
                 * before: FFXI sets it per object, and the first fogged object differs from frame to
                 * frame near its lights; a new zone or a camera jump (the cache's) takes it at once */
                if (g_fxs.fog <= 0.0f)
                    g_fx.fogc_set = 0; /* no fog: the next one takes its colour at once */
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
            /* the sun, kept in the world: the zone's shader draws carry no light, so a frame gives one
             * only when a lit draw (a character) is in view - the shadows hold the last one rather
             * than come and go with what the camera sees */
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
            int shadows = g_fxs.sun > 0.0f || g_fxs.shadow > 0.0f;
            if (have_v && (own || (shadows && g_fx.sunw_seen && g_fx.sunw_seen + 600 > g_serial)))
            {
                /* back into this frame's view */
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
                    u.hist[0] = g_fx.hist_serial && g_fx.hist_serial + 1 == g_serial && g_fx.hist[0] && g_fx.hist[0].width == aw &&
                        g_fx.hist[0].height == ah ? 1.0f : 0.0f;
                    /* the bounce light's too, at its own size */
                    u.gip[0] = g_fx.gih_serial && g_fx.gih_serial + 1 == g_serial && g_fx.gih[0] && g_fx.gih[0].width == (aw + 1) / 2 &&
                        g_fx.gih[0].height == (ah + 1) / 2 ? 1.0f : 0.0f;
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
            /* the world for rays, before the passes that trace them */
            int rt = 0;
            if ((g_fxs.rt > 0.0f || (int)g_fxs.debug == 8) && have_v)
            {
                float invP[16];
                rt = gfx_mat_inverse(invP, s->proj) && rt_capture(invP, s->view, vinv + 12);
            }
            NSUInteger gw = (aw + 1) / 2, gh = (ah + 1) / 2;
            int gi = u.gi[0] > 0.0f && fx_tex(&g_fx.gi0, MTLPixelFormatRGBA16Float, gw, gh) &&
                fx_tex(&g_fx.gih[0], MTLPixelFormatRGBA16Float, gw, gh) && fx_tex(&g_fx.gih[1], MTLPixelFormatRGBA16Float, gw, gh) &&
                (!rt || fx_tex(&g_fx.gi1, MTLPixelFormatRGBA16Float, gw, gh));
            if (!gi)
                u.gi[0] = 0.0f;
            u.gip[1] = fminf(fmaxf(g_fxs.temporal, 0.0f), 0.95f), u.gip[2] = 12.0f; /* the history's share; the gather's samples */
            if (rt) /* traced: rays reach further, two a texel, kept longer over frames (their noise averaged away) */
                u.gi[2] = fmaxf(g_fxs.gi_radius * 2.5f, 4.0f), u.gip[2] = 2.0f, u.gip[1] = u.gip[1] > 0.0f ? fmaxf(u.gip[1], 0.95f) : 0.0f;
            if (fx_tex(&g_fx.src, ct.pixelFormat, ct.width, ct.height) && fx_tex(&g_fx.ao0, MTLPixelFormatRGBA16Float, aw, ah) &&
                fx_tex(&g_fx.ao1, MTLPixelFormatRGBA16Float, aw, ah) && fx_tex(&g_fx.b1a, MTLPixelFormatRGBA16Float, bw, bh) &&
                fx_tex(&g_fx.b1b, MTLPixelFormatRGBA16Float, bw, bh) &&
                fx_tex(&g_fx.b2a, MTLPixelFormatRGBA16Float, (bw + 1) / 2, (bh + 1) / 2) &&
                fx_tex(&g_fx.b2b, MTLPixelFormatRGBA16Float, (bw + 1) / 2, (bh + 1) / 2) &&
                fx_tex(&g_fx.ra, MTLPixelFormatRGBA16Float, bw, bh) && fx_tex(&g_fx.rb, MTLPixelFormatRGBA16Float, bw, bh))
            {
                if (!g_fx.comp_pipe || g_fx.comp_fmt != ct.pixelFormat)
                {
                    [g_fx.comp_pipe release];
                    g_fx.comp_pipe = fx_pipeline(@"fx_comp", ct.pixelFormat);
                    g_fx.comp_fmt = ct.pixelFormat;
                }
                if (g_fx.comp_pipe)
                {
                    id<MTLBlitCommandEncoder> b = [cmd() blitCommandEncoder];
                    [b copyFromTexture:ct sourceSlice:0 sourceLevel:0 toTexture:g_fx.src destinationSlice:0 destinationLevel:0
                            sliceCount:1 levelCount:1];
                    [b endEncoding];
                    static const int32_t across[2] = { 1, 0 }, down[2] = { 0, 1 }, across2[2] = { 2, 0 }, down2[2] = { 0, 2 };
                    MTLViewport q = fx_full(g_fx.ao0), qb = fx_full(g_fx.b1a), e = fx_full(g_fx.b2a);
                    id<MTLTexture> ao_out = g_fx.ao0;
                    if (u.ao[1] > 0.0f || u.shadow[0] > 0.0f || u.smap[0] > 0.0f)
                    {
                        g_ts_fx_end = TS_AO1;
                        id<MTLTexture> lz = fx_lz(aw, ah);
                        if (lz)
                        {
                            fx_pass(g_fx.lzv[0], MTLLoadActionDontCare, g_fx.linz_pipe, q, &u, &depth, 1, NULL);
                            for (int l = 1; l < 4; ++l)
                                fx_pass(g_fx.lzv[l], MTLLoadActionDontCare, g_fx.zmip_pipe, fx_full(g_fx.lzv[l]), &u, &g_fx.lzv[l - 1], 1, NULL);
                        }
                        else
                            u.ao[1] = 0.0f;
                        id<MTLTexture> ao_in[4] = { depth, u.smap[0] > 0.0f ? g_fx.smap : g_fx.sdummy,
                            u.smap[0] > 0.0f && u.smapn2[1] > 0.0f ? g_fx.smapn : g_fx.sdummy, lz ? lz : g_fx.sdummy };
                        fx_pass(g_fx.ao0, MTLLoadActionDontCare, g_fx.ao_pipe, q, &u, ao_in, 4, NULL);
                        /* traced (rt): the occlusion from rays in the world, the sun's as fx_ao found it; ao1 then holds it */
                        id<MTLTexture> a = g_fx.ao0, bt = g_fx.ao1;
                        if (rt && u.ao[1] > 0.0f)
                        {
                            id<MTLTexture> r_in[2] = { g_fx.ao0, depth };
                            rt_pass(g_fx.ao1, MTLLoadActionDontCare, g_ray.ao, q, &u, r_in, 2, NULL);
                            a = g_fx.ao1, bt = g_fx.ao0;
                        }
                        fx_pass(bt, MTLLoadActionDontCare, g_fx.blur_pipe, q, &u, &a, 1, across);
                        fx_pass(a, MTLLoadActionDontCare, g_fx.blur_pipe, q, &u, &bt, 1, down);
                        ao_out = a;
                        if (g_fxs.temporal > 0.0f && fx_tex(&g_fx.hist[0], MTLPixelFormatRGBA16Float, aw, ah) &&
                            fx_tex(&g_fx.hist[1], MTLPixelFormatRGBA16Float, aw, ah))
                        {
                            int to = g_fx.hist_at ^ 1;
                            id<MTLTexture> t_in[2] = { a, g_fx.hist[g_fx.hist_at] };
                            fx_pass(g_fx.hist[to], MTLLoadActionDontCare, g_fx.temporal_pipe, q, &u, t_in, 2, NULL);
                            ao_out = g_fx.hist[to];
                            g_fx.hist_at = to, g_fx.hist_serial = g_serial;
                        }
                        g_ts_fx_end = TS_FX1;
                    }
                    /* the bounce light: gathered from its map at half the occlusion's size, then over frames */
                    id<MTLTexture> gi_out = nil;
                    if (gi)
                    {
                        MTLViewport qg = fx_full(g_fx.gi0);
                        if (rt)
                        {
                            /* traced (rt): rays from each texel into the world, the light they meet as the frame or the
                             * sun saw it; smoothed four times (one texel apart, two, four, one) before the temporal pass */
                            static const int32_t s1[2] = { 1, 1 }, s2[2] = { 2, 2 }, s4[2] = { 4, 4 };
                            id<MTLTexture> r_in[5] = { depth, g_fx.src, ao_out, g_fx.gimap, g_fx.gicol };
                            rt_pass(g_fx.gi0, MTLLoadActionDontCare, g_ray.gi, qg, &u, r_in, 5, NULL);
                            fx_pass(g_fx.gi1, MTLLoadActionDontCare, g_ray.giblur, qg, &u, &g_fx.gi0, 1, s1);
                            fx_pass(g_fx.gi0, MTLLoadActionDontCare, g_ray.giblur, qg, &u, &g_fx.gi1, 1, s2);
                            fx_pass(g_fx.gi1, MTLLoadActionDontCare, g_ray.giblur, qg, &u, &g_fx.gi0, 1, s4);
                            fx_pass(g_fx.gi0, MTLLoadActionDontCare, g_ray.giblur, qg, &u, &g_fx.gi1, 1, s1);
                        }
                        else
                        {
                            id<MTLTexture> gi_in[3] = { depth, g_fx.gimap, g_fx.gicol };
                            fx_pass(g_fx.gi0, MTLLoadActionDontCare, g_fx.gi_pipe, qg, &u, gi_in, 3, NULL);
                        }
                        gi_out = g_fx.gi0;
                        if (u.gip[1] > 0.0f)
                        {
                            int to = g_fx.gih_at ^ 1;
                            id<MTLTexture> t_in[2] = { g_fx.gi0, g_fx.gih[g_fx.gih_at] };
                            fx_pass(g_fx.gih[to], MTLLoadActionDontCare, g_fx.gitemp_pipe, qg, &u, t_in, 2, NULL);
                            gi_out = g_fx.gih[to];
                            g_fx.gih_at = to, g_fx.gih_serial = g_serial;
                        }
                    }
                    if (u.bloom[1] > 0.0f)
                    {
                        id<MTLTexture> b_in[2] = { g_fx.src, ao_out }; /* shaded as fx_comp shades */
                        fx_pass(g_fx.b1a, MTLLoadActionDontCare, g_fx.bright_pipe, qb, &u, b_in, 2, NULL);
                        fx_pass(g_fx.b1b, MTLLoadActionDontCare, g_fx.gauss_pipe, qb, &u, &g_fx.b1a, 1, across2);
                        fx_pass(g_fx.b1a, MTLLoadActionDontCare, g_fx.gauss_pipe, qb, &u, &g_fx.b1b, 1, down2);
                        fx_pass(g_fx.b2a, MTLLoadActionDontCare, g_fx.down_pipe, e, &u, &g_fx.b1a, 1, NULL);
                        fx_pass(g_fx.b2b, MTLLoadActionDontCare, g_fx.gauss_pipe, e, &u, &g_fx.b2a, 1, across2);
                        fx_pass(g_fx.b2a, MTLLoadActionDontCare, g_fx.gauss_pipe, e, &u, &g_fx.b2b, 1, down2);
                    }
                    if (u.rays[0] > 0.0f)
                    {
                        id<MTLTexture> mask_in[2] = { g_fx.src, depth };
                        fx_pass(g_fx.ra, MTLLoadActionDontCare, g_fx.raymask_pipe, qb, &u, mask_in, 2, NULL);
                        fx_pass(g_fx.rb, MTLLoadActionDontCare, g_fx.rays_pipe, qb, &u, &g_fx.ra, 1, NULL);
                        fx_pass(g_fx.ra, MTLLoadActionDontCare, g_fx.gauss_pipe, qb, &u, &g_fx.rb, 1, across);
                        fx_pass(g_fx.rb, MTLLoadActionDontCare, g_fx.gauss_pipe, qb, &u, &g_fx.ra, 1, down);
                    }
                    id<MTLTexture> comp_in[7] = { g_fx.src, ao_out, depth, g_fx.b1a, g_fx.b2a, g_fx.rb, gi_out ? gi_out : g_fx.rb };
                    fx_pass(ct, MTLLoadActionLoad, g_fx.comp_pipe, (MTLViewport){ vx, vy, vw, vh, 0, 1 }, &u, comp_in, 7, NULL);
                    if (rt && (int)g_fxs.debug == 8)
                    {
                        if (!g_ray.clay || g_ray.clay_fmt != ct.pixelFormat)
                            [g_ray.clay release], g_ray.clay = rt_fx_pipeline(@"rt_clay", ct.pixelFormat), g_ray.clay_fmt = ct.pixelFormat;
                        if (g_ray.clay)
                            rt_pass(ct, MTLLoadActionLoad, g_ray.clay, (MTLViewport){ vx, vy, vw, vh, 0, 1 }, &u, &depth, 1, NULL);
                    }
                }
            }
        }
        color->scene = 0; /* the effects changed it: its mips are behind (scene_mips) */
        color->used = g_serial;
        if (gfx_profiling)
        {
            static double last;
            g_fx.st_frames++;
            g_fx.st_own += s->sun_dir[3] != 0.0f;
            g_fx.st_map += g_fx.st_drawn_this;
            g_fx.st_cmin = g_fx.st_frames == 1 || g_ncasters < g_fx.st_cmin ? g_ncasters : g_fx.st_cmin;
            g_fx.st_cmax = g_ncasters > g_fx.st_cmax ? g_ncasters : g_fx.st_cmax;
            double now = CACurrentMediaTime();
            if (now - last > 2.0)
            {
                fprintf(stderr, "[recomp] gfx: shadows: %u frames, %u with the sun's own light, %u with a map; casters %u..%u; "
                    "%u cached; map %.0f units across; sun %.2f %.2f %.2f\n", g_fx.st_frames, g_fx.st_own, g_fx.st_map, g_fx.st_cmin,
                    g_fx.st_cmax, g_fx.st_cached, g_fx.st_across, g_fx.sunw[0], g_fx.sunw[1], g_fx.sunw[2]);
                last = now, g_fx.st_frames = g_fx.st_own = g_fx.st_map = g_fx.st_cmax = 0;
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
}

/* --- frames ---------------------------------------------------------------------------------------------------- */
static void frame_end(void)
{
    casters_clear();
    id<MTLCommandBuffer> c = cmd();
    uint64_t serial = g_serial;
    dispatch_semaphore_t sem = g_frames_sem;
    ts_frame_end(c);
    [c addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        atomic_store(&g_completed, serial);
        dispatch_semaphore_signal(sem);
    }];
    submit(0);
    g_frame_open = 0;
    g_frame = (g_frame + 1) % FRAMES;
    g_serial++;
}

/* "60 FPS 16.7ms": the text of the overlay, from the presents of the last half second */
static void fps_tick(void)
{
    double now = CACurrentMediaTime();
    if (!g_fps_since)
        g_fps_since = now;
    g_fps_frames++;
    double dt = now - g_fps_since;
    if (dt >= 0.5)
    {
        double fps = g_fps_frames / dt;
        snprintf(g_fps_text, sizeof g_fps_text, "%.0f FPS %.1fms", fps, dt * 1000.0 / g_fps_frames);
        g_fps_since = now, g_fps_frames = 0;
    }
}

static void draw_overlay(id<MTLRenderCommandEncoder> e, double w, double h)
{
    struct
    {
        float rect[4], scale;
        uint32_t n, pad[2], text[32];
    } u;
    memset(&u, 0, sizeof u);
    for (const char* c = g_fps_text; *c && u.n < 32; ++c)
    {
        uint32_t k = *c >= '0' && *c <= '9' ? (uint32_t)(*c - '0') : *c == 'F' ? 10 : *c == 'P' ? 11 : *c == 'S' ? 12
            : *c == 'm' ? 13 : *c == 's' ? 14 : *c == '.' ? 15 : 16;
        u.text[u.n++] = k;
    }
    u.scale = (float)(h >= 1400 ? 3 : 2);
    u.rect[0] = u.rect[1] = 4 * u.scale;
    u.rect[2] = (float)(u.n * 6 + 3) * u.scale, u.rect[3] = 11 * u.scale;
    float size[2] = { (float)w, (float)h };
    [e setRenderPipelineState:g_overlay_pipe];
    [e setVertexBytes:&u length:sizeof u atIndex:0];
    [e setVertexBytes:size length:sizeof size atIndex:1];
    [e setFragmentBytes:&u length:sizeof u atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

void gfx_present(GfxTex* bb)
{
    if (!g_dev)
        return;
    uint64_t present_start = gfx_profiling ? gfx_now_ns() : 0;
    g_present_thread = pthread_self();
    @autoreleasepool
    {
        flush_pass();
        if (g_layer && bb)
        {
            int pw = 0, ph = 0;
            if (g_window)
                SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
            if (pw > 0 && ph > 0 && (g_layer.drawableSize.width != pw || g_layer.drawableSize.height != ph))
                g_layer.drawableSize = CGSizeMake(pw, ph);
            uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
            id<CAMetalDrawable> drawable = [g_layer nextDrawable];
            if (gfx_profiling)
                g_prof.drawable_ns += gfx_now_ns() - t0;
            if (drawable)
            {
                MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = drawable.texture;
                p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                p.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:p];
                float sharpen = g_fxs.fx != 0.0f ? g_fxs.sharpen : 0.0f;
                [e setRenderPipelineState:sharpen > 0.0f && g_present_cas_pipe ? g_present_cas_pipe : g_present_pipe];
                [e setFragmentBytes:&sharpen length:sizeof sharpen atIndex:0];
                [e setFragmentTexture:bb->view atIndex:0];
                [e setFragmentSamplerState:g_present_samp atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                if (g_fxs.fps != 0.0f && g_overlay_pipe)
                    draw_overlay(e, drawable.texture.width, drawable.texture.height);
                [e endEncoding];
                [cmd() presentDrawable:drawable];
                bb->used = g_serial;
            }
        }
        fps_tick();
        fx_reload();
        frame_end();
    }
    if (gfx_profiling)
        prof_frame(present_start);
}

void gfx_finish(void)
{
    if (!g_dev)
        return;
    @autoreleasepool
    {
        submit(1);
    }
}

void gfx_resize(uint32_t w, uint32_t h)
{
    if (g_layer)
        g_layer.drawableSize = CGSizeMake(w, h);
}

uint64_t gfx_window_flags(void) { return 0; }

int gfx_init(void* window, int vsync)
{
    if (g_dev)
    {
        /* up already (host64's sign-in screen, on this same window): the game's present interval */
        if (g_layer)
            g_layer.displaySyncEnabled = vsync ? YES : NO;
        return 1;
    }
    @autoreleasepool
    {
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev)
        {
            fprintf(stderr, "[recomp] gfx: no Metal device\n");
            return 0;
        }
        g_queue = [g_dev newCommandQueue];
        g_frames_sem = dispatch_semaphore_create(FRAMES);
        g_dummy = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        g_util = compile(CLEAR_MSL);
        g_pipe_queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        g_pipe_file_queue = dispatch_queue_create("ffxi.pipeline-cache", DISPATCH_QUEUE_SERIAL);
        if (!g_sync_pipelines)
            prewarm_pipelines();
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        id<MTLFunction> vf = [g_util newFunctionWithName:@"present_vs"], ff = [g_util newFunctionWithName:@"present_fs"];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_present_pipe = [g_dev newRenderPipelineStateWithDescriptor:pd error:NULL];
        [ff release];
        ff = [g_util newFunctionWithName:@"present_cas_fs"];
        pd.fragmentFunction = ff;
        g_present_cas_pipe = [g_dev newRenderPipelineStateWithDescriptor:pd error:NULL];
        [vf release];
        [ff release];
        [pd release];
        /* the overlay: blended over the frame; FFXI_FPS=0 hides it (the settings file's fps, from
         * Config > Modern, has the last word) */
        const char* prof = getenv("FFXI_PROFILE");
        gfx_profiling = prof && prof[0] && prof[0] != '0';
        fx_config();
        const char* show = getenv("FFXI_FPS");
        if (show && show[0] == '0')
            g_fxs.fps = 0.0f;
        pd = [[MTLRenderPipelineDescriptor alloc] init];
        vf = [g_util newFunctionWithName:@"overlay_vs"], ff = [g_util newFunctionWithName:@"overlay_fs"];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        g_overlay_pipe = [g_dev newRenderPipelineStateWithDescriptor:pd error:NULL];
        [vf release];
        [ff release];
        [pd release];
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        g_present_samp = [g_dev newSamplerStateWithDescriptor:sd];
        [sd release];
        if (window)
        {
            g_window = (SDL_Window*)window;
            g_view = SDL_Metal_CreateView(g_window);
            g_layer = g_view ? (CAMetalLayer*)SDL_Metal_GetLayer(g_view) : nil;
            if (g_layer)
            {
                g_layer.device = g_dev;
                g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
                g_layer.framebufferOnly = YES;
                g_layer.displaySyncEnabled = vsync ? YES : NO;
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
                if (pw > 0 && ph > 0)
                    g_layer.drawableSize = CGSizeMake(pw, ph);
            }
            else
                fprintf(stderr, "[recomp] gfx: no Metal layer for the window: %s\n", SDL_GetError());
        }
        fprintf(stderr, "[recomp] gfx: Metal on %s\n", [[g_dev name] UTF8String]);
        return 1;
    }
}
