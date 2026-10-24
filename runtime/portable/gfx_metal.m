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
#include <TargetConditionals.h>
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
    uint32_t bc;     /* a DXT format kept decoded, as BGRA8, on a GPU without BC (iPhones), else 0 */
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
static int g_bc; /* the GPU samples BC (DXT) textures: every Mac; on iOS, M-series iPads */

/* Whether a depth format carries stencil. iOS has no 24-bit depth. */
static int format_has_stencil(MTLPixelFormat df)
{
#if TARGET_OS_OSX
    if (df == MTLPixelFormatDepth24Unorm_Stencil8)
        return 1;
#endif
    return df == MTLPixelFormatDepth32Float_Stencil8 || df == MTLPixelFormatX32_Stencil8;
}

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
static uint32_t g_shd_drawn[2];       /* character draws into the cascades and the fitted map since the last log line */
static _Atomic uint64_t g_gpu_ns;      /* GPU time of the committed command buffers (profile) */
/* GPU time per pass (gfx_pass_timing): each timed pass's encoder samples a timestamp at its start
 * (start of vertex) and its end (end of fragment) into the frame's counter sample buffer, two samples
 * a pass, read back when the frame completes. */
enum { TS_MAX = 128 };                 /* timed passes a frame; past them the rest go untimed */
static id<MTLCounterSampleBuffer> g_ts[FRAMES];
static int g_ts_ok = -1;               /* -1 not looked at, 0 the GPU cannot, 1 g_ts[] are there */
static uint8_t g_ts_pass[FRAMES][TS_MAX]; /* each frame's timed passes' GFX_PASS_* in order */
static uint32_t g_ts_n;                /* this frame's so far */
static int g_pass = GFX_PASS_COMP;     /* what the next fx_pass or rt_pass times as */
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
    if (gfx_pass_timing_on())
        [g_cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
            uint64_t ns = (uint64_t)((done.GPUEndTime - done.GPUStartTime) * 1e9);
            atomic_fetch_add(&g_gpu_ns, ns);
            gfx_pass_gpu(ns);
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

/* One DXT1-5 block into 16 pixels, BGRA8 (as gfx_vulkan.cpp's decode_block). */
static uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t color565(uint16_t p) { return 0xff000000u | expand5(p >> 11) << 16 | expand6((p >> 5) & 63) << 8 | expand5(p & 31); }

static void decode_dxt_block(const uint8_t* data, uint32_t fmt, uint32_t px[16])
{
    int dxt1 = fmt == FOURCC('D', 'X', 'T', '1');
    int explicit_alpha = fmt == FOURCC('D', 'X', 'T', '2') || fmt == FOURCC('D', 'X', 'T', '3');
    int interp_alpha = fmt == FOURCC('D', 'X', 'T', '4') || fmt == FOURCC('D', 'X', 'T', '5');
    const uint8_t* c = data + (dxt1 ? 0 : 8);
    uint16_t c0 = (uint16_t)(c[0] | c[1] << 8), c1 = (uint16_t)(c[2] | c[3] << 8);
    uint32_t pal[4] = { color565(c0), color565(c1), 0, 0 };
    int four = !dxt1 || c0 > c1;
    for (unsigned shift = 0; shift < 24; shift += 8)
    {
        unsigned a = (pal[0] >> shift) & 255, b = (pal[1] >> shift) & 255;
        pal[2] |= (four ? (2 * a + b) / 3 : (a + b) / 2) << shift;
        if (four)
            pal[3] |= ((a + 2 * b) / 3) << shift;
    }
    pal[2] |= 0xff000000u;
    if (four)
        pal[3] |= 0xff000000u;
    uint32_t idx = (uint32_t)c[4] | (uint32_t)c[5] << 8 | (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24;
    uint8_t alpha[8] = { 0 };
    uint64_t abits = 0;
    if (interp_alpha)
    {
        alpha[0] = data[0], alpha[1] = data[1];
        if (alpha[0] > alpha[1])
            for (unsigned i = 2; i < 8; ++i)
                alpha[i] = (uint8_t)(((8 - i) * alpha[0] + (i - 1) * alpha[1]) / 7);
        else
        {
            for (unsigned i = 2; i < 6; ++i)
                alpha[i] = (uint8_t)(((6 - i) * alpha[0] + (i - 1) * alpha[1]) / 5);
            alpha[6] = 0, alpha[7] = 255;
        }
        for (unsigned i = 0; i < 6; ++i)
            abits |= (uint64_t)data[2 + i] << (8 * i);
    }
    for (unsigned i = 0; i < 16; ++i)
    {
        px[i] = pal[(idx >> (2 * i)) & 3];
        if (explicit_alpha)
            px[i] = (px[i] & 0xffffffu) | (uint32_t)(((data[i / 2] >> ((i % 2) * 4)) & 15) * 17) << 24;
        else if (interp_alpha)
            px[i] = (px[i] & 0xffffffu) | (uint32_t)alpha[(abits >> (3 * i)) & 7] << 24;
    }
}

/* A w x h region of DXT blocks (rows of blocks pitch bytes apart) into tightly packed BGRA8. */
static void decode_dxt(uint32_t fmt, const uint8_t* src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t* out)
{
    uint32_t block = fmt == FOURCC('D', 'X', 'T', '1') ? 8 : 16;
    for (uint32_t by = 0; by < (h + 3) / 4; ++by)
        for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx)
        {
            uint32_t px[16];
            decode_dxt_block(src + (size_t)by * pitch + (size_t)bx * block, fmt, px);
            for (uint32_t yy = 0; yy < 4 && by * 4 + yy < h; ++yy)
                for (uint32_t xx = 0; xx < 4 && bx * 4 + xx < w; ++xx)
                    memcpy(out + ((size_t)(by * 4 + yy) * w + bx * 4 + xx) * 4, &px[yy * 4 + xx], 4);
        }
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
        if (t->block && !g_bc)
            t->bc = fmt, t->block = 0, t->texel = 4, pf = MTLPixelFormatBGRA8Unorm;
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

static void hdr_forget(GfxTex* t);
void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    hdr_forget(t);
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
        if (t->bc) /* blocks to BGRA8 first; then an upload like any other */
        {
            static uint8_t* decoded;
            static size_t decoded_cap;
            if ((size_t)w * h * 4 > decoded_cap)
            {
                free(decoded);
                decoded_cap = (size_t)w * h * 4;
                decoded = (uint8_t*)malloc(decoded_cap);
            }
            decode_dxt(t->bc, (const uint8_t*)src, pitch, w, h, decoded);
            src = decoded, pitch = w * 4;
        }
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
    if (!t || level >= t->levels || t->use == GFX_USE_DEPTH || t->block || t->bc)
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
    if (!t || level >= t->levels || t->use == GFX_USE_DEPTH || t->block || t->bc)
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

static void ts_mark(MTLRenderPassDescriptor* rp, int pass);
static int g_pass_next = -1; /* the next render pass is timed as this GFX_PASS_* (the scene renderer's) */

static int begin_pass(void)
{
    if (g_enc)
        return 1;
    if (!g_rt)
        return 0;
    MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
    if (g_pass_next >= 0)
        ts_mark(p, g_pass_next), g_pass_next = -1;
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



/* --- the glow: the scene's additive draws, drawn again for the post bloom (post_bloom) --------------------------
 * The world's lamps, spell effects and the sun's halo are additive draws (the destination blended by ONE). Each one into
 * a 3D scene's target is recorded as it is encoded - the very pipeline, depth state and everything bound - and when the
 * scene is done they are drawn again onto black over the scene's depth: what glows, as bright as it was added. */
typedef struct Glow
{
    GfxTex* rt;
    MTLPixelFormat fmt;              /* the target's format its pipeline was built for */
    id<MTLTexture> depth;            /* the depth it was tested against; retained, like everything below */
    id<MTLRenderPipelineState> pipe;
    id<MTLDepthStencilState> ds;
    id<MTLBuffer> vb[GFX_NSTREAMS], ub, ib;
    NSUInteger voff[GFX_NSTREAMS], uoff, ioff;
    id<MTLTexture> tex[8];
    id<MTLSamplerState> samp[8];
    MTLViewport vp;
    MTLScissorRect sc;
    MTLPrimitiveType prim;
    uint32_t n, vstart;
    uint8_t itype, cull, ntex;
    int32_t zbias;
} Glow;

static Glow* g_glows;
static uint32_t g_nglows, g_glows_cap;

static void glows_clear(void)
{
    for (uint32_t i = 0; i < g_nglows; ++i)
    {
        Glow* g = &g_glows[i];
        [g->depth release], [g->pipe release], [g->ds release], [g->ub release], [g->ib release];
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            [g->vb[s] release];
        for (int t = 0; t < 8; ++t)
            [g->tex[t] release];
    }
    g_nglows = 0;
}

/* a record for this draw when it glows: post bloom on, an additive 3D draw into the target's first level */
static Glow* glow_new(const GfxDraw* d, id<MTLRenderPipelineState> p, id<MTLDepthStencilState> ds, int water)
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
    g->rt = g_rt, g->fmt = g_rt->tex.pixelFormat, g->depth = [depth_attachment() retain], g->pipe = [p retain], g->ds = [ds retain], g->zbias = d->zbias;
    g->cull = d->cull == 3 ? 2 : d->cull == 2 ? 1 : 0;
    g->vp = g_bound.vp, g->sc = g_bound.sc;
    return g;
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
        Glow* gl = glow_new(d, p, ds, water);
        if (gl)
            gl->ub = [buf retain], gl->uoff = off;
        for (int s = 0; s < GFX_NSTREAMS; ++s)
        {
            if (d->buf[s])
            {
                bind_vb(d->buf[s]->b, d->buf_off[s], s);
                d->buf[s]->used = g_serial;
                if (gl)
                    gl->vb[s] = [d->buf[s]->b retain], gl->voff[s] = d->buf_off[s];
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
                if (gl)
                    gl->vb[s] = [buf retain], gl->voff[s] = off;
            }
            else
            {
                bind_vb(g_dummy, 0, s);
                if (rec)
                    rec->vb[s] = [g_dummy retain];
                if (gl)
                    gl->vb[s] = [g_dummy retain];
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
            if (gl)
                gl->tex[i] = [view retain], gl->samp[i] = ss, gl->ntex = (uint8_t)(i + 1);
        }

        if (water)
            water_bind();
        uint32_t n = vertex_count(d->prim, d->count);
        MTLPrimitiveType mp = metal_prim(d->prim);
        if (rec)
            rec->prim = mp, rec->n = n, rec->vstart = d->vertex_start;
        if (gl)
            gl->prim = mp, gl->n = n, gl->vstart = d->vertex_start;
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
            if (gl)
                gl->ib = [buf retain], gl->ioff = off, gl->itype = 4;
        }
        else if (d->ibuf)
        {
            d->ibuf->used = g_serial;
            if (gl)
                gl->ib = [d->ibuf->b retain], gl->ioff = d->ibuf_off, gl->itype = (uint8_t)(d->index_size == 2 ? 2 : 4);
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
            if (gl)
                gl->ib = [buf retain], gl->ioff = off, gl->itype = (uint8_t)(d->index_size == 2 ? 2 : 4);
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
    "  int NS = max(int(u.aop.y), 1);\n" /* contact_steps, jittered by k and gathered over frames */
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
    if (!gfx_pass_timing_on())
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
            d.sampleCount = 2 * TS_MAX;
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

/* a pass times itself as pass (GFX_PASS_*): its start and end into the frame's samples */
static void ts_mark(MTLRenderPassDescriptor* rp, int pass)
{
    frame_begin(); /* (g_frame is the frame the pass goes into) */
    if (g_ts_n >= TS_MAX)
        return;
    id<MTLCounterSampleBuffer> b = ts_buffer();
    if (!b)
        return;
    MTLRenderPassSampleBufferAttachmentDescriptor* a = rp.sampleBufferAttachments[0];
    a.sampleBuffer = b;
    a.startOfVertexSampleIndex = 2 * g_ts_n;
    a.endOfVertexSampleIndex = MTLCounterDontSample;
    a.startOfFragmentSampleIndex = MTLCounterDontSample;
    a.endOfFragmentSampleIndex = 2 * g_ts_n + 1;
    g_ts_pass[g_frame][g_ts_n++] = (uint8_t)pass;
}

/* at the frame's end: when it completes, its passes' times into the profile's sums. (The frame's
 * g_ts_pass row stands until then: the next frame in its slot waits for this one on g_frames_sem, whose
 * signal is in a handler added after this one.) */
static void ts_frame_end(id<MTLCommandBuffer> c)
{
    uint32_t n = g_ts_n;
    g_ts_n = 0;
    if (g_ts_ok <= 0 || !n)
        return;
    id<MTLCounterSampleBuffer> b = g_ts[g_frame];
    const uint8_t* pass = g_ts_pass[g_frame];
    double scale = g_ts_scale;
    [c addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        NSData* d = [b resolveCounterRange:NSMakeRange(0, 2 * n)];
        if (!d || d.length < 2 * n * sizeof(MTLCounterResultTimestamp))
            return;
        const MTLCounterResultTimestamp* t = (const MTLCounterResultTimestamp*)d.bytes;
        /* a pass from its start, or from the last one's end where it started before that: Apple's GPUs
         * begin a pass's vertices while the last pass's fragments still run, and a full-screen pass then
         * waits for them (its time would count the wait) */
        MTLTimestamp last = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            MTLTimestamp a = t[2 * i].timestamp, z = t[2 * i + 1].timestamp;
            if (a == MTLCounterErrorValue || z == MTLCounterErrorValue)
                continue;
            if (a < last)
                a = last;
            if (z > a)
                gfx_pass_add(pass[i], (uint64_t)((double)(z - a) * scale));
            if (z > last)
                last = z;
        }
        gfx_pass_frame();
    }];
}

/* one full-screen triangle into target, reading tex[0..n) */
static void fx_pass(id<MTLTexture> target, MTLLoadAction load, id<MTLRenderPipelineState> p, MTLViewport vp, const FxU* u,
    id<MTLTexture> const* tex, int n, const int32_t* dir)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = load;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    ts_mark(rp, g_pass);
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

enum { SUN_CACHE_FRAMES = 60 * 30, SUN_CACHE_MAX = 8192 };
#define SUN_CACHE_NEAR 40.0f                  /* what was seen within this of the camera stays past SUN_CACHE_FRAMES */
#define SUN_CACHE_FAR 4.0f                    /* kept within this many times the far map's reach (sun_distance, 100 at least), and 200 more */
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
static uint32_t g_ncache_over; /* where a trim left the cache past SUN_CACHE_MAX (else 0): the next waits for a quarter more */
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
    if (!(g_serial & 255) || (g_ncache > SUN_CACHE_MAX && g_ncache > g_ncache_over + SUN_CACHE_MAX / 4))
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
    /* none of what stands further out casts into the map (whose casters stand up to 200 units sunward
     * of it: gfx_sun_fit), and what moves out there (an airship's view at the longest draw distance)
     * would leave a new entry each frame, never expiring while the camera stays aboard */
    float keep_within = fmaxf(SUN_CACHE_FAR * fmaxf(g_fxs.sun_distance, 100.0f) + 200.0f, g_fxs.sun_prime); /* the primed zone's too */
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
    ts_mark(rp, GFX_PASS_SUN);
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
    ts_mark(rp, g_pass);
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
#include "gfx_gi_msl.h"
#include "gfx_ao_msl.h"

/* --- the scene renderer (gfx_world_draw, gfx.h) --------------------------------------------------------------
 * The zone from the game's own data (host/scene.c): one draw per submesh from the game's static vertex buffers,
 * its instances and lights in two arrays for the whole batch, lit with the game's fixed-function equation per
 * vertex (mode 1, to match what the game's draws gave) or per pixel (mode 2). Pipelines: the per-pixel switch and
 * the alpha test are function constants, blending is the pipeline's; built in place the first time (a handful). */
/* The lamps (scene_lamps): every lit point light of the pool, binned into 64-pixel screen tiles once a frame
 * (lamps_bin); a pixel takes its tile's. Each the game's own colour and attenuation, but faded to nothing at its range
 * (1 - (d/r)^4)^2 rather than cut off there - no circles on the ground. LU: viewport, tiles across and down, strength. */
#define LAMP_MSL                                                                                                    \
    "struct LL { float4 pos, col, att, amb; };\n"                                                                 \
    "struct LU { float4 vp; float4 t; float4 k; };\n" /* k: the zone's cubes are there, a lamp's size (soft), taps */ \
    /* the lamp's shadow at P: the cube face its direction falls in, the point pushed toward the lamp a little */      \
    "static float lamp_shadow(depth2d_array<float> SM, depth2d_array<float> ZM, constant float4x4* M, uint s, float3 P,\n" \
    "                         float3 Lp, float3 N, bool zone, float ls, int nt) {\n"                               \
    "  constexpr sampler c(coord::normalized, filter::linear, address::clamp_to_edge, compare_func::less_equal);\n" \
    "  float3 d = P - Lp, a = abs(d);\n"                                                                         \
    "  uint f = a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0u : 1u) : a.y >= a.z ? (d.y > 0.0 ? 2u : 3u) : (d.z > 0.0 ? 4u : 5u);\n" \
    "  float dl = length(d);\n"                                                                                  \
    "  float4 h = M[s * 6u + f] * float4(P - d * (0.08 / max(dl, 0.5)) * min(dl, 2.0) + N * 0.06, 1.0);\n"       \
    "  if (h.w <= 1e-4) return 1.0;\n"                                                                           \
    "  float2 uv = float2(h.x / h.w * 0.5 + 0.5, 0.5 - h.y / h.w * 0.5);\n"                                      \
    "  float z = h.z / h.w;\n"                                                                                   \
    "  if (ls <= 0.0) {\n"                                                                                       \
    "    float v = SM.sample_compare(c, uv, s * 6u + f, z);\n"                                                   \
    "    return zone ? min(v, ZM.sample_compare(c, uv, s * 6u + f, z)) : v;\n"                                   \
    "  }\n"                                                                                                      \
    /* soft (lamp_size): a disc as wide as the lamp seen from here, a face spanning twice the distance, one to six \
     * texels */                                                                                                   \
    "  float tx = 1.0 / float(SM.get_width()), r = clamp(ls / (4.0 * max(dl, 0.3)), tx, 6.0 * tx);\n" \
    "  float2 a0 = sh_turn(P);\n"                                                                                \
    "  float v = 0.0;\n"                                                                                         \
    "  for (int i = 0; i < nt; ++i) {\n"                                                                         \
    "    float2 q = uv + sh_vogel(i, nt, a0) * r;\n"                                                             \
    "    float w = SM.sample_compare(c, q, s * 6u + f, z);\n"                                                    \
    "    v += zone ? min(w, ZM.sample_compare(c, q, s * 6u + f, z)) : w;\n"                                      \
    "  }\n"                                                                                                      \
    "  return v / float(nt);\n"                                                                                  \
    "}\n"                                                                                                        \
    "static void lamp_light(constant LU& U, const device LL* LS, const device ushort* TL, float2 fp, float3 P, float3 N,\n" \
    "                       thread float3& dif, thread float3& amb, depth2d_array<float> SM, depth2d_array<float> ZM,\n" \
    "                       constant float4x4* SMM,\n"                                                         \
    "                       float3 Ps, bool soft) {\n"                                                                      \
    "  int2 t = int2((fp - U.vp.xy) / 64.0);\n"                                                                   \
    "  if (any(t < 0) || t.x >= int(U.t.x) || t.y >= int(U.t.y)) return;\n"                                       \
    "  const device ushort* L = TL + (uint(t.y) * uint(U.t.x) + uint(t.x)) * 32u;\n"                              \
    "  uint n = L[0];\n"                                                                                          \
    "  for (uint k = 0; k < n; ++k) {\n"                                                                          \
    "    LL l = LS[L[1 + k]];\n"                                                                                  \
    "    float3 v = l.pos.xyz - P;\n"                                                                             \
    "    float d = length(v), q = d / l.pos.w;\n"                                                                 \
    "    if (q >= 1.0) continue;\n"                                                                               \
    "    float w = saturate(1.0 - q * q * q * q);\n"                                                              \
    "    float at = w * w / max(l.att.x + l.att.y * d + l.att.z * d * d, 1e-3);\n"                                \
    "    amb += l.amb.rgb * at;\n"                                                                                \
    "    float sh = l.att.w > 0.5 && U.t.z > 0.5 ? lamp_shadow(SM, ZM, SMM, uint(l.att.w) - 1u, Ps, l.pos.xyz, N, U.k.x > 0.5, soft ? U.k.y : 0.0, int(U.k.z)) : 1.0;\n" \
    "    dif += l.col.rgb * at * sh * max(dot(N, v / max(d, 1e-4)), 0.0);\n"                                      \
    "    if (U.t.w > 0.5 && l.att.w > 0.5) amb += float3(1.0 - sh, sh, 0.0) * 4.0 * w;\n" /* scene_lamp_debug: red shadowed, green lit */ \
    "  }\n"                                                                                                       \
    "}\n"

/* Specular highlights (scene_spec): GGX with Schlick's Fresnel and Smith-Schlick visibility, energy kept by the
 * microfacet terms. FFXI has no specular or roughness maps, so a surface's are guessed: the zone is dielectric
 * stone, rough (a sheen at grazing sun, more when wet: scene_spec_wet); a character's texel bright and grey reads
 * as metal (armour): sharper, its Fresnel tinted by the texel, while skin and cloth (saturated or dark) stay low. The
 * roughness widened by how fast the normal turns across the pixel (specular anti-aliasing): no sparkle on coarse
 * meshes. SP: k (strength, lamps' share, wetness, anti-aliasing), unused. */
#define SPEC_MSL                                                                                                    \
    "struct SP { float4 k; float4 pad; };\n"                                                                      \
    "static float spec_ggx(float3 N, float3 V, float3 L, float a2, float f0) {\n"                                \
    "  float3 H = normalize(V + L);\n"                                                                           \
    "  float nl = saturate(dot(N, L)), nv = max(dot(N, V), 1e-3), nh = saturate(dot(N, H)), vh = saturate(dot(V, H));\n" \
    "  float d = nh * nh * (a2 - 1.0) + 1.0;\n"                                                                  \
    "  float D = a2 / (3.14159265 * d * d);\n"                                                                   \
    "  float k = 0.5 * sqrt(a2);\n"                                                                              \
    "  float vis = 0.25 / ((nl * (1.0 - k) + k) * (nv * (1.0 - k) + k));\n"                                      \
    "  float F = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);\n"                                                        \
    "  return min(D * vis * F * nl, 16.0);\n"                                                                    \
    "}\n"                                                                                                        \
    /* a roughness to a2, widened by the normal's variance across the pixel (Kaplanyan's filter) */                 \
    "static float spec_a2(float rough, float3 N, float aa) {\n"                                                  \
    "  float3 dx = dfdx(N), dy = dfdy(N);\n"                                                                     \
    "  float var = 0.5 * (dot(dx, dx) + dot(dy, dy)) * aa;\n"                                                    \
    "  float a = rough * rough;\n"                                                                               \
    "  return clamp(a * a + min(2.0 * var, 0.18), 0.002, 1.0);\n"                                                \
    "}\n"

/* The sun's or moon's shadow at a point, from the cascades gfx_shadow_begin drew (zone layer, and the characters'
 * in the first info.y, only inside their box): the cascade by the distance from the camera, the last tenth of each
 * one's reach blended into the next (the last fades out), the point pushed a texel toward the light and one to five
 * along its normal, the more the surface turns from the light (else long, grazing faces shade themselves in stripes);
 * in the two near cascades four compares a texel apart, beyond one, each the hardware's 2x2 filter. 1 lit, 0 in
 * shadow (strength applied). */
#define SHADOW_MSL                                                                                                  \
    "struct SH { float4x4 m[4]; float4 cas[4]; float4 cbox[4]; float4 dir; float4 cam; float4 info; float4x4 cm[4];\n" \
    "            float4 ctx; float4 pk; };\n"                                                                     \
    "static float sh_pcf(depth2d_array<float> t, sampler s, float2 uv, uint c, float z, float tx) {\n"           \
    "  if (c >= 2u) return t.sample_compare(s, uv, c, z);\n" /* far: the hardware's 2x2 alone */                 \
    "  return 0.25 * (t.sample_compare(s, uv + float2(-0.5, -0.5) * tx, c, z) +\n"                               \
    "                 t.sample_compare(s, uv + float2(0.5, -0.5) * tx, c, z) +\n"                                \
    "                 t.sample_compare(s, uv + float2(-0.5, 0.5) * tx, c, z) +\n"                                \
    "                 t.sample_compare(s, uv + float2(0.5, 0.5) * tx, c, z));\n"                                 \
    "}\n"                                                                                                        \
    /* the characters' layers: the fitted one when the point is in it (round the characters near the camera, a   \
     * texel some millimetres), else the cascade's own; nine of the hardware's 2x2 compares a texel apart (a 4x4    \
     * footprint, smooth edges), the normal offset scaled to the layer's texel */                                  \
    "static float sh_cpcf(depth2d_array<float> t, sampler s, float2 uv, uint l, float z, float tx) {\n"         \
    "  float v = 0.0;\n"                                                                                         \
    "  for (int y = -1; y <= 1; ++y)\n"                                                                          \
    "    for (int x = -1; x <= 1; ++x) v += t.sample_compare(s, uv + float2(x, y) * tx, l, z);\n"                \
    "  return v * (1.0 / 9.0);\n"                                                                                \
    "}\n"                                                                                                        \
    /* contact-hardening soft shadows (shadow_softness; pk: the light's tan(angular radius), the blocker search's  \
     * gathers, the filter's taps): the blockers' mean depth round the point (gathers on a disc), then that   \
     * far from them times the light's size wide, in hardware 2x2 compares on a disc turned by a hash of the place \
     * (world-locked: still under a moving camera). A map's texel and depth in world units come from its matrix;  \
     * the depths are lowered along the receiver's slope by the disc's reach (no acne at grazing angles, the slope  \
     * no blocker of itself); no blocker: lit, no filter; all blockers and a narrow penumbra: one compare */         \
    "static float2 sh_turn(float3 p) {\n"                                                                             \
    "  float a = 6.2831853 * fract(52.9829189 * fract(dot(floor(p * 40.0), float3(0.06711056, 0.00583715, 0.1234567))));\n" \
    "  return float2(cos(a), sin(a));\n"                                                                              \
    "}\n"                                                                                                             \
    "constant float2 sh_disc[16] = { float2(0.1768, 0.0000), float2(-0.2258, 0.2068), float2(0.0346, -0.3938), float2(0.2846, 0.3712),\n" \
    "  float2(-0.5222, -0.0924), float2(0.4947, -0.3147), float2(-0.1655, 0.6155), float2(-0.3156, -0.6076),\n"       \
    "  float2(0.6846, 0.2500), float2(-0.7123, 0.2940), float2(0.3434, -0.7337), float2(0.2537, 0.8089),\n"           \
    "  float2(-0.7647, -0.4432), float2(0.8971, -0.1972), float2(-0.5475, 0.7788), float2(-0.1265, -0.9761) };\n"     \
    "static float2 sh_vogel(int i, int n, float2 r) {\n"                                                              \
    "  float2 q = sh_disc[i * (16 / n) + (16 / n - 1)];\n"                                                            \
    "  return float2(q.x * r.x - q.y * r.y, q.x * r.y + q.y * r.x);\n"                                                \
    "}\n"                                                                                                             \
    "static float sh_pcss(depth2d_array<float> t, sampler cs, float4x4 M, uint l, float2 uv, float z, float tx, float tl,\n" \
    "                     int nb, int nf, float2 rot, float nl) {\n"                                                  \
    "  constexpr sampler ps(coord::normalized, filter::nearest, address::clamp_to_edge);\n"                           \
    "  float wpt = 2.0 * tx / length(float3(M[0][0], M[1][0], M[2][0]));\n"                                           \
    "  float wpd = 1.0 / length(float3(M[0][2], M[1][2], M[2][2]));\n"                                                \
    "  float rs = clamp(tl * 6.0 / wpt, 1.0, 12.0);\n"                                                                \
    "  float sl = sqrt(1.0 - nl * nl) / max(nl, 0.2), zs = z - min(rs * wpt * sl, 1.0) / wpd;\n" /* the slope's own fall */ \
    "  float sum = 0.0, n = 0.0;\n"                                                                                   \
    "  for (int i = 0; i < nb; ++i) {\n"                                                                              \
    "    float4 d = t.gather(ps, uv + sh_vogel(i, 4, rot) * (rs * tx), l);\n"                                         \
    "    float4 b = select(float4(0.0), float4(1.0), d < zs);\n"                                                       \
    "    sum += dot(d, b), n += b.x + b.y + b.z + b.w;\n"                                                             \
    "  }\n"                                                                                                           \
    "  if (n == 0.0) return 1.0;\n"                                                                                   \
    "  float pen = clamp((z - sum / n) * wpd * tl / wpt, 0.5, 12.0);\n"                                               \
    "  float zb = z - min(pen * wpt * sl, 0.5) / wpd;\n"                              \
    "  if (n == 4.0 * float(nb) && pen < 1.5) return t.sample_compare(cs, uv, l, zb);\n"                              \
    "  float v = 0.0;\n"                                                                                              \
    "  for (int i = 0; i < nf; ++i) v += t.sample_compare(cs, uv + sh_vogel(i, nf, rot) * (pen * tx), l, zb);\n"      \
    "  return v / float(nf);\n"                                                                                       \
    "}\n"                                                                                                             \
    "static float sh_char(constant SH& S, depth2d_array<float> cm, depth2d_array<float> ct, sampler cs, uint c, float3 wp,\n" \
    "                     float3 n, float nl, bool soft) {\n"                                                                  \
    "  float off = 1.0 + 4.0 * sqrt(1.0 - nl * nl);\n"                                                           \
    "  float t = S.ctx.x;\n" /* the fitted layer, when there is one and the point is in it */                     \
    "  if (t > 0.0) {\n"                                                                                          \
    "    float4 h = S.cm[0] * float4(wp + n * (t * off) + S.dir.xyz * t, 1.0);\n"                                 \
    "    float2 u0 = float2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5);\n"                                               \
    "    if (all(abs(h.xy) < 0.995)) return soft && S.pk.x > 0.0 ? sh_pcss(ct, cs, S.cm[0], 0u, u0, h.z, S.ctx.y, S.pk.x, max(int(S.pk.y) / 2, 1), int(S.pk.z) / 2, sh_turn(wp), nl)\n" \
    "                                                   : sh_cpcf(ct, cs, u0, 0u, h.z, S.ctx.y * (soft ? max(S.ctx.z, 1.0) : 1.0));\n" \
    "  }\n"                                                                                                       \
    "  if (c >= uint(S.info.y)) return 1.0;\n" /* the cascade's characters' layer, half size, only round them */      \
    "  t = 2.0 * S.cas[c].y;\n"                                                                                   \
    "  float4 h = S.cm[1u + c] * float4(wp + n * (t * off) + S.dir.xyz * t, 1.0);\n"                               \
    "  float4 b = S.cbox[c];\n"                                                                                   \
    "  if (any(abs(h.xy) > 0.995) || any(h.xy < b.xy) || any(h.xy > b.zw)) return 1.0;\n"                         \
    "  float2 u1 = float2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5);\n"                                                 \
    "  if (soft && S.pk.x > 0.0) return sh_pcss(cm, cs, S.cm[1u + c], c, u1, h.z, 2.0 * S.info.w, S.pk.x, max(int(S.pk.y) / 2, 1), int(S.pk.z) / 2, sh_turn(wp), nl);\n" \
    "  return sh_cpcf(cm, cs, u1, c, h.z, 2.0 * S.info.w * (soft ? max(S.ctx.z, 1.0) : 1.0));\n"                                \
    "}\n"                                                                                                        \
    "static float sh_at(constant SH& S, depth2d_array<float> zm, depth2d_array<float> cm, depth2d_array<float> ct,\n" \
    "                   sampler cs, uint c,\n"                                                                  \
    "                   float3 wp, float3 n, bool soft) {\n"                                                               \
    "  float nl = saturate(dot(n, S.dir.xyz)), t = S.cas[c].y;\n" /* more the more the surface turns from it */ \
    "  float4 h = S.m[c] * float4(wp + n * (t * (1.0 + 4.0 * sqrt(1.0 - nl * nl))) + S.dir.xyz * t, 1.0);\n"  \
    "  if (abs(h.x) > 0.995 || abs(h.y) > 0.995) return -1.0;\n"                                                \
    "  float2 uv = float2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5);\n"                                                 \
    "  float v;\n"                                                                                               \
    "  if (soft && S.pk.x > 0.0) {\n" /* soft: all the taps in the nearest cascade, half in the next, a quarter beyond */ \
    "    int k = c == 0u ? 1 : c == 1u ? 2 : 4;\n"                                                               \
    "    v = sh_pcss(zm, cs, S.m[c], c, uv, h.z, S.info.w, S.pk.x, max(int(S.pk.y) / k, 1), max(int(S.pk.z) / k, 4), sh_turn(wp), nl);\n" \
    "  } else\n"                                                                                                 \
    "    v = soft && S.ctx.z > 1.01 && c < 2u ? sh_cpcf(zm, cs, uv, c, h.z, S.info.w * S.ctx.z) : sh_pcf(zm, cs, uv, c, h.z, S.info.w);\n" /* the moon's wider */ \
    "  if (v > 0.0 && S.info.y > 0.0) v = min(v, sh_char(S, cm, ct, cs, c, wp, n, nl, soft));\n"                      \
    "  return v;\n"                                                                                              \
    "}\n"                                                                                                        \
    "static float shadow(constant SH& S, depth2d_array<float> zm, depth2d_array<float> cm, depth2d_array<float> ct,\n" \
    "                    sampler cs, float3 wp,\n"                                                              \
    "                    float3 n, bool soft) {\n" /* soft: PCSS (the zone); a character's own the fixed filter */ \
    "  float3 e = wp - S.cam.xyz;\n"                                                                             \
    "  float d = length(e);\n"                                                                                   \
    "  uint nc = uint(S.info.x);\n"                                                                              \
    "  for (uint c = 0; c < nc; ++c) {\n"                                                                        \
    "    if (S.cas[c].w == 0.0 || d > S.cas[c].x) continue;\n"                                                   \
    "    float v = sh_at(S, zm, cm, ct, cs, c, wp, n, soft);\n"                                                            \
    "    if (v < 0.0) continue;\n"                                                                               \
    "    float r = S.cas[c].x, f = saturate((d - 0.9 * r) / (0.1 * r));\n"                                       \
    "    if (f > 0.0) {\n"                                                                                       \
    "      float w = c + 1 < nc && S.cas[c + 1].w != 0.0 ? sh_at(S, zm, cm, ct, cs, c + 1, wp, n, soft) : 1.0;\n"          \
    "      v = mix(v, w < 0.0 ? 1.0 : w, f);\n"                                                                  \
    "    }\n"                                                                                                    \
    "    return mix(1.0, v, S.info.z);\n"                                                                        \
    "  }\n"                                                                                                      \
    "  return 1.0;\n"                                                                                            \
    "}\n"

static const char WORLD_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constant bool k_pixel [[function_constant(0)]];\n"
    "constant bool k_atest [[function_constant(1)]];\n"
    "constant bool k_shadow [[function_constant(2)]];\n"
    "constant bool k_gi [[function_constant(3)]];\n"
    "constant bool k_ao [[function_constant(4)]];\n"
    "constant bool k_spec [[function_constant(5)]];\n"
    "constant bool k_lamps [[function_constant(6)]];\n"
    SHADOW_MSL
    SPEC_MSL
    LAMP_MSL
    GI_MSL
    AO_MSL
    "struct WF { float4x4 vp; float4 params; };\n" /* params: alpha reference 0..255, the pixel-centre fixup */
    "struct WL { float4 pos, diffuse, ambient, att; };\n"
    "struct WI { float4x4 wvp, world, world_inv; float4 sun_dir, sun_col, sun2_dir, sun2_col, ambient, fog, fog_col; int4 light; };\n"
    "struct WD { uint inst, flags; };\n"
    "struct VI { float3 pos [[attribute(0)]]; float3 nrm [[attribute(1)]]; float4 col [[attribute(2)]]; float2 uv [[attribute(3)]]; };\n"
    "struct VO { float4 pos [[position]]; float2 uv; float4 col; float3 sun; float3 wpos; float3 nrm; float z;\n"
    "            float4 fog [[flat]]; float3 fog_col [[flat]]; uint inst [[flat]]; float4 gi;\n"
    "            float4 aop; float3 amb; float3 lmp; };\n"
    /* D3D's fixed function, COLORVERTEX with the vertex colour as diffuse and ambient reflectance, no specular:
     * saturate(vc * (ambient + sum of each light's ambient) + vc * sum of N.L x diffuse), point lights with
     * 1 / (a0 + a1 d + a2 d^2) out to their range. Unsaturated, without the directional light along sd (the shadows'
     * sun or moon), which goes into sun: saturate(the two's sum) is D3D's. g: the bounce light, added to the ambient,
     * and what the ambient is multiplied by (gfx_gi_metal.inc; (0, 0, 0, 1) without it). amb: the ambient's share of
     * the result, which the occlusion scales (gfx_ao_metal.inc) */
    "static float3 lit(float3 P, float3 N, float3 vc, constant WI& I, constant WL* L, float3 sd, float4 g, thread float3& sun,\n"
    "                  thread float3& amb, thread float3& lmp) {\n"
    "  float3 pl = float3(0.0);\n"
    "  float3 a = I.ambient.rgb * g.a + g.rgb, d = float3(0.0), s = float3(0.0);\n"
    "  if (I.sun_dir.w > 0.0) {\n"
    "    float3 t = I.sun_col.rgb * max(dot(N, I.sun_dir.xyz), 0.0);\n"
    "    if (dot(I.sun_dir.xyz, sd) > 0.98) s += t; else d += t;\n"
    "  }\n"
    "  if (I.sun2_dir.w > 0.0) {\n"
    "    float3 t = I.sun2_col.rgb * max(dot(N, I.sun2_dir.xyz), 0.0);\n"
    "    if (dot(I.sun2_dir.xyz, sd) > 0.98) s += t; else d += t;\n"
    "  }\n"
    "  for (int k = 0; k < 4 && !k_lamps; ++k) {\n" /* (the lamps, every one, per pixel instead: world_fs) */
    "    int li = I.light[k];\n"
    "    if (li < 0) continue;\n"
    "    WL l = L[li];\n"
    "    float3 v = l.pos.xyz - P;\n"
    "    float dist = length(v);\n"
    "    if (dist > l.pos.w) continue;\n"
    "    float at = 1.0 / max(l.att.x + l.att.y * dist + l.att.z * dist * dist, 1e-6);\n"
    "    float3 t = l.ambient.rgb * at + l.diffuse.rgb * at * max(dot(N, v / max(dist, 1e-6)), 0.0);\n"
    "    d += t, pl += t;\n"
    "  }\n"
    "  lmp = vc * pl;\n" /* the lamps' share: it fills the sun's (the moon's) shadow (shadow_fill) */
    "  sun = vc * s;\n"
    "  amb = vc * a;\n"
    "  return vc * a + vc * d;\n"
    "}\n"
    "vertex VO world_vs(VI v [[stage_in]], constant WF& F [[buffer(1)]], constant WI* I [[buffer(2)]],\n"
    "                   constant WL* L [[buffer(3)]], constant WD& D [[buffer(4)]],\n"
    "                   constant SH& S [[buffer(6), function_constant(k_shadow)]],\n"
    "                   constant GU& G [[buffer(8), function_constant(k_gi)]],\n"
    "                   const device uint* PS [[buffer(9), function_constant(k_gi)]],\n"
    "                   texture2d<float> gir [[texture(4), function_constant(k_gi)]],\n"
    "                   texture2d<float> gid [[texture(5), function_constant(k_gi)]],\n"
    "                   constant AU& A [[buffer(10), function_constant(k_ao)]]) {\n"
    "  constant WI& in = I[D.inst];\n"
    "  float4 wp = in.world * float4(v.pos, 1.0);\n"
    "  float3 n = (float4(v.nrm, 0.0) * in.world_inv).xyz;\n"
    "  if (D.flags & 32u) n = normalize(n);\n"
    "  VO o;\n"
    "  o.pos = in.wvp * float4(v.pos, 1.0);\n"
    "  o.z = o.pos.w;\n"
    "  o.pos.x += o.pos.w * F.params.y;\n" /* D3D's pixel centres onto Metal's, as gfx_msl.c's fixup */
    "  o.pos.y -= o.pos.w * F.params.z;\n"
    "  o.uv = v.uv;\n"
    "  o.wpos = wp.xyz;\n"
    "  o.nrm = n;\n"
    "  o.inst = D.inst;\n"
    "  o.fog = in.fog;\n"
    "  o.fog_col = in.fog_col.rgb;\n"
    "  o.col = v.col, o.sun = float3(0.0), o.gi = float4(0.0, 0.0, 0.0, 1.0), o.amb = float3(0.0), o.lmp = float3(0.0);\n"
    "  o.aop = k_ao ? A.pvp * wp : float4(0.0);\n" /* where the occlusion was made: this point last frame */
    "  if (k_gi && !(k_pixel && G.org.x > 0.5)) { o.gi = gi_at(G, PS, gir, gid, wp.xyz, normalize(n)); o.gi.rgb *= G.k.x; }\n"
    "  if (!k_pixel) {\n"
    "    float3 sun, rest = lit(wp.xyz, n, v.col.rgb, in, L, k_shadow ? S.dir.xyz : float3(0.0), o.gi, sun, o.amb, o.lmp);\n"
    "    o.col.rgb = k_shadow || k_ao ? rest : saturate(rest + sun);\n"
    "    o.sun = sun;\n"
    "  }\n"
    "  return o;\n"
    "}\n"
    "fragment float4 world_fs(VO i [[stage_in]], constant WF& F [[buffer(1)]], constant WI* I [[buffer(2)]],\n"
    "                         constant WL* L [[buffer(3)]], constant WD& D [[buffer(4)]],\n"
    "                         texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],\n"
    "                         constant SH& S [[buffer(6), function_constant(k_shadow)]],\n"
    "                         depth2d_array<float> zm [[texture(1), function_constant(k_shadow)]],\n"
    "                         depth2d_array<float> cm [[texture(2), function_constant(k_shadow)]],\n"
    "                         depth2d_array<float> ct [[texture(3), function_constant(k_shadow)]],\n"
    "                         sampler cs [[sampler(1), function_constant(k_shadow)]],\n"
    "                         constant GU& G [[buffer(8), function_constant(k_gi)]],\n"
    "                         const device uint* PS [[buffer(9), function_constant(k_gi)]],\n"
    "                         texture2d<float> gir [[texture(4), function_constant(k_gi)]],\n"
    "                         texture2d<float> gid [[texture(5), function_constant(k_gi)]],\n"
    "                         constant AU& A [[buffer(10), function_constant(k_ao)]],\n"
    "                         texture2d<float> aot [[texture(6), function_constant(k_ao)]],\n"
    "                         constant SP& SPc [[buffer(11), function_constant(k_spec)]],\n"
    "                         const device LL* LS [[buffer(12), function_constant(k_lamps)]],\n"
    "                         const device ushort* TL [[buffer(13), function_constant(k_lamps)]],\n"
    "                         constant LU& LUc [[buffer(14), function_constant(k_lamps)]],\n"
    "                         depth2d_array<float> lsm [[texture(7), function_constant(k_lamps)]],\n"
    "                         depth2d_array<float> zlsm [[texture(8), function_constant(k_lamps)]],\n"
    "                         constant float4x4* lsmm [[buffer(15), function_constant(k_lamps)]]) {\n"
    "  float3 c = i.col.rgb, sun = i.sun, amb = i.amb, lmp = i.lmp;\n"
    "  float shv = 1.0;\n"
    "  float3 N = (D.flags & 32u) ? normalize(i.nrm) : i.nrm;\n"
    "  float4 g = i.gi;\n"
    "  if (k_gi && k_pixel && G.org.x > 0.5) { g = gi_at(G, PS, gir, gid, i.wpos, normalize(N)); g.rgb *= G.k.x; }\n"
    "  if (k_pixel) c = lit(i.wpos, N, i.col.rgb, I[i.inst], L, k_shadow ? S.dir.xyz : float3(0.0), g, sun, amb, lmp);\n"
    "  if (k_lamps && k_pixel) {\n"
    "    float3 ld = float3(0.0), la = float3(0.0);\n"
    "    lamp_light(LUc, LS, TL, i.pos.xy, i.wpos, normalize(N), ld, la, lsm, zlsm, lsmm, i.wpos, true);\n"
    "    c += i.col.rgb * (ld + la);\n" /* (on the vertex colour, as lit() lights) */
    "    lmp += i.col.rgb * (ld + la);\n"
    "    amb += i.col.rgb * la;\n"
    "  }\n"
    "  if (k_ao) {\n" /* the occlusion takes from the ambient alone; scene_ao_debug 1 shows it */
    "    float ao = pow(ao_at(A, aot, i.aop), A.k.x);\n"
    "    if (A.k.z > 0.5) return float4(float3(ao), 1.0);\n"
    "    c -= amb * (1.0 - ao);\n"
    "  }\n"
    "  if (k_gi && G.k.z > 0.5)\n" /* scene_gi_debug: 1 the bounce light on the vertex colour, 2 the ambient's factor, 3 the bounce x 8 */
    "    return float4(saturate(G.k.z > 2.5 ? 8.0 * g.rgb : G.k.z > 1.5 ? float3(g.a) : 2.0 * i.col.rgb * g.rgb), 1.0);\n"
    "  if (k_shadow && S.cam.w > 1.5) {\n" /* scene_shadow_debug: the shadow, tinted by its cascade; black facing away */
    "    float d = length(i.wpos - S.cam.xyz);\n"
    "    uint k = 0;\n"
    "    while (k < 3 && d > S.cas[k].x) ++k;\n"
    "    float3 tint = k == 0 ? float3(1.0, 0.55, 0.55) : k == 1 ? float3(0.55, 1.0, 0.55) : k == 2 ? float3(0.55, 0.55, 1.0) : float3(1.0, 1.0, 0.55);\n"
    "    float nl = dot(normalize(N), S.dir.xyz);\n"
    "    return float4(nl > 0.0 ? tint * (0.2 + 0.8 * shadow(S, zm, cm, ct, cs, i.wpos, normalize(N), true)) : float3(0.05), 1.0);\n"
    "  }\n"
    "  if (k_shadow) {\n"
    "    if (any(sun > 0.0)) {\n"
    "      shv = shadow(S, zm, cm, ct, cs, i.wpos, normalize(N), true);\n"
    "      float3 Y = float3(0.2126, 0.7152, 0.0722);\n" /* where the lamps outshine the sun or moon, its shadow fades out */
    "      shv = mix(shv, 1.0, saturate(S.ctx.w * dot(lmp, Y) / max(dot(sun, Y), 1e-4)));\n"
    "      sun *= shv;\n"
    "    }\n"
    "    c = saturate(c + sun);\n"
    "  } else if (k_pixel || k_ao) c = saturate(c + sun);\n"
    "  float4 tx = t.sample(s, i.uv);\n"
    "  float4 o = float4(saturate(2.0 * tx.rgb * c), saturate(4.0 * tx.a * i.col.a));\n" /* MODULATE2X, MODULATE4X */
    "  if (k_atest && !(o.a * 255.0 > F.params.x)) discard_fragment();\n"
    "  if (k_spec && k_shadow) {\n" /* the sun's (or moon's) highlight, where it reaches, and the lamps' */
    "    constant WI& in = I[i.inst];\n"
    "    float3 Nn = normalize(N), V = normalize(S.cam.xyz - i.wpos);\n"
    "    float wet = SPc.k.z, lum = dot(tx.rgb, float3(0.299, 0.587, 0.114));\n"
    "    float a2 = spec_a2(mix(0.8, 0.5, wet), Nn, SPc.k.w);\n"
    "    float f0 = mix(0.03, 0.05, wet) * (0.5 + lum);\n"
    "    float occ = saturate(2.0 * dot(i.col.rgb, float3(0.333)));\n" /* the vertex colour's darkness: corners and crevices */
    "    float3 sp = float3(0.0);\n"
    "    float3 scol = in.sun2_dir.w > 0.0 && dot(in.sun2_dir.xyz, S.dir.xyz) > dot(in.sun_dir.xyz, S.dir.xyz) ? in.sun2_col.rgb : in.sun_col.rgb;\n"
    "    if (in.sun_dir.w > 0.0 || in.sun2_dir.w > 0.0) sp += scol * spec_ggx(Nn, V, S.dir.xyz, a2, f0) * shv;\n" /* (the light the shadows are drawn from) */
    "    for (int k = 0; k < 4 && SPc.k.y > 0.0; ++k) {\n"
    "      int li = in.light[k];\n"
    "      if (li < 0) continue;\n"
    "      WL l = L[li];\n"
    "      float3 v = l.pos.xyz - i.wpos;\n"
    "      float dist = length(v);\n"
    "      if (dist > l.pos.w) continue;\n"
    "      float at = 1.0 / max(l.att.x + l.att.y * dist + l.att.z * dist * dist, 1e-6);\n"
    "      sp += l.diffuse.rgb * at * SPc.k.y * spec_ggx(Nn, V, v / max(dist, 1e-6), a2, f0);\n"
    "    }\n"
    "    if (SPc.pad.x > 0.0) o.rgb += sp * (SPc.k.x * occ);\n" /* an HDR scene: past white, the tonemap's to roll off */
    "    else o.rgb = 1.0 - (1.0 - o.rgb) * exp(-sp * (SPc.k.x * occ));\n" /* (a soft shoulder: toward white, never flat white) */
    "  }\n"
    "  if (i.fog.z > 0.0) o.rgb = mix(i.fog_col, o.rgb, saturate((i.fog.y - i.z) / max(i.fog.y - i.fog.x, 1e-6)));\n"
    "  return o;\n"
    "}\n"
    /* the zone into a shadow cascade: depth alone, or alpha tested as the walk tests it (MODULATE4X, GREATER 0x60) */
    "struct SVO { float4 pos [[position]]; float2 uv; float a; };\n"
    "vertex SVO zshadow_vs(VI v [[stage_in]], constant float4x4& M [[buffer(1)]], const device float4x4* W [[buffer(2)]],\n"
    "                      constant uint& inst [[buffer(4)]]) {\n"
    "  SVO o;\n"
    "  o.pos = M * (W[inst] * float4(v.pos, 1.0));\n"
    "  o.uv = v.uv;\n"
    "  o.a = v.col.a;\n"
    "  return o;\n"
    "}\n"
    "fragment void zshadow_fs(SVO i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  if (!(saturate(4.0 * t.sample(s, i.uv).a * i.a) * 255.0 > 96.0)) discard_fragment();\n"
    "}\n";

static struct
{
    int tried;
    id<MTLLibrary> lib;
    MTLVertexDescriptor* vd;
    struct
    {
        uint32_t key;
        id<MTLRenderPipelineState> p;
    } pipe[64];
    uint32_t npipe;
    id<MTLDepthStencilState> ds[2]; /* opaque (depth written), blended */
} g_world;

/* the shadows' state for the draws (gfx_shadow_begin): their uniforms, as the SH struct of SHADOW_MSL */
typedef struct ShU
{
    float m[GFX_SHADOW_CASCADES][16];
    float cas[GFX_SHADOW_CASCADES][4]; /* reach from the camera, texel, depth range (world units), valid */
    float cbox[GFX_SHADOW_CASCADES][4]; /* the characters in the map (x0, y0, x1, y1), with a margin */
    float dir[4], cam[4];
    float info[4]; /* cascades, of them the characters', strength, 1 / size */
    float cm[GFX_SHADOW_CASCADES][16]; /* the characters' layers (last frame's): 0 fitted round those near the camera, 1
                                        * and on the first cascades' */
    float ctx[4];  /* world units a texel in the fitted layer (0: none), 1 / its size, the penumbra's width (1 the sun's), how
                    * the lamps fill the shadow */
    float pk[4];   /* soft shadows: the light's tan(angular radius) (0: off), the blocker search's taps, the filter's */
} ShU;
static struct
{
    int on;                        /* this batch's draws are shadowed */
    ShU u;
    /* the zone's layers (kept); the characters' (drawn at a frame's end, read by the next): in the first cascades, and
     * fitted round those near the camera */
    id<MTLTexture> zmap, cmap, cmt;
    uint32_t size;
    id<MTLSamplerState> cmp;
    id<MTLDepthStencilState> ds;
    id<MTLRenderPipelineState> zone[2], chars; /* depth alone, alpha tested; the characters' */
    int tried;
    uint32_t fsize;      /* the fitted layer's texels across */
    float freach;        /* the fitted layer's reach from the camera */
    uint64_t cframe;     /* the frame of the last batch */
    uint32_t cdone;      /* of the frame's characters (the host's list grows batch by batch), those taken so far */
    float near_lo[2], near_hi[2]; /* the frame's near characters in cascade 0's map */
    float cbox[GFX_SHADOW_CASCADES][4]; /* the frame's characters in each cascade's map */
    /* the frame's characters for the layers drawn at its end: its last batch (the host's palette and states hold until
     * the next frame) and every batch's items, with their buffers and textures held (the game may let them go first) */
    GfxShadowFrame lf;
    GfxCharFrame lc;
    struct ShItem* items;
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
/* a character's draw as the shadow passes take it, its buffers and texture held */
typedef struct ShItem
{
    id<MTLBuffer> vb, uv, ib;
    id<MTLTexture> tex;
    id<MTLSamplerState> samp;
    uint32_t index_size, first, count, prim, palette, state, mirror;
    float at[3]; /* its place, world: its first bone through its state's WORLD (a cached skin's bones are the model's) */
    uintptr_t key;
} ShItem;

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

static void shadow_items_clear(void)
{
    for (uint32_t i = 0; i < g_shd.nitems; ++i)
        [g_shd.items[i].vb release], [g_shd.items[i].uv release], [g_shd.items[i].ib release], [g_shd.items[i].tex release];
    g_shd.nitems = 0;
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
        if (!it->vb || !it->uv || !it->ib || !it->tex || !it->count)
            continue;
        it->vb->used = it->uv->used = it->ib->used = it->tex->used = g_serial;
        ShItem* o = &g_shd.items[g_shd.nitems++];
        o->vb = [it->vb->b retain], o->uv = [it->uv->b retain], o->ib = [it->ib->b retain], o->tex = [it->tex->view retain];
        o->samp = sampler(&it->samp);
        o->index_size = it->index_size, o->first = it->first, o->count = it->count, o->prim = it->prim;
        o->palette = it->palette, o->state = it->state, o->mirror = it->mirror;
        char_at(cf, it, o->at);
        o->key = char_key(it);
    }
}

static void shadow_bind(id<MTLRenderCommandEncoder> e)
{
    [e setVertexBytes:&g_shd.u length:sizeof g_shd.u atIndex:6];
    [e setFragmentBytes:&g_shd.u length:sizeof g_shd.u atIndex:6];
    [e setFragmentTexture:g_shd.zmap atIndex:1];
    [e setFragmentTexture:g_shd.cmap atIndex:2];
    [e setFragmentTexture:g_shd.cmt atIndex:3];
    [e setFragmentSamplerState:g_shd.cmp atIndex:1];
}

#include "gfx_gi_metal.inc"

int gfx_world_supported(void) { return g_dev != nil; }

void gfx_world_time(int pass)
{
    if (!g_dev || !gfx_pass_timing_on())
        return;
    @autoreleasepool
    {
        end_pass();
    }
    g_pass_next = pass == 1 ? GFX_PASS_WORLD : pass == 2 ? GFX_PASS_CHARS : -1;
}

static int world_init(void)
{
    if (g_world.tried)
        return g_world.lib != nil;
    g_world.tried = 1;
    g_world.lib = compile(WORLD_MSL);
    if (!g_world.lib)
        return 0;
    MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
    vd.attributes[0].format = MTLVertexFormatFloat3, vd.attributes[0].offset = 0, vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat3, vd.attributes[1].offset = 12, vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format = MTLVertexFormatUChar4Normalized_BGRA, vd.attributes[2].offset = 24, vd.attributes[2].bufferIndex = 0;
    vd.attributes[3].format = MTLVertexFormatFloat2, vd.attributes[3].offset = 28, vd.attributes[3].bufferIndex = 0;
    vd.layouts[0].stride = 36;
    g_world.vd = vd;
    for (int i = 0; i < 2; ++i)
    {
        MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
        d.depthCompareFunction = MTLCompareFunctionLessEqual;
        d.depthWriteEnabled = i == 0;
        g_world.ds[i] = [g_dev newDepthStencilStateWithDescriptor:d];
        [d release];
    }
    return 1;
}

/* --- the lamps (scene_lamps): gfx_lamps hands the frame's; binned into 64-pixel tiles of the scene's viewport at the
 * frame's first zone batch (its camera), the list and the tiles in the frame's ring, bound at 12-14 for both passes */
static struct
{
    GfxWorldLight l[256];
    uint32_t n;
    uint64_t set, binned;            /* the frames they were set, binned */
    id<MTLBuffer> lb, tb;            /* the lamps (LL), the tiles (32 ushort each: count, indices) */
    NSUInteger lo, to;
    float lu[12];                    /* LU: viewport, tiles across, down; shadows; debug; the zone's cubes */
    float vp[16];                    /* the camera's view x projection, for the glows */
    float cam[3];
    /* the characters' shadows from the nearest lamps (scene_lamp_shadows): drawn at a frame's end into a cube a lamp
     * (six layers each of lsm), read by the next frame; each its lamp's place, to find it again */
    id<MTLTexture> lsm;
    uint32_t lsize, nsh;
    float shpos[4][4];               /* the shadowed lamps' places (w: range) */
    float shm[24][16];               /* their faces' matrices */
    uint64_t sh_serial;              /* the frame they were drawn */
    /* the zone into the same lamps' cubes (zlsm, Depth32F), kept: drawn again when the shadowed lamps change or every
     * 600 frames (a door, a lift) */
    id<MTLTexture> zlsm;
    float zpos[4][4];
    uint32_t zn;
    uint64_t z_serial;
    /* the frame's opaque zone items, every batch's, and their world matrices, for the cubes at the frame's end */
    GfxWorldItem* zi;
    float (*zw)[16];
    uint32_t nzi, czi, nzw, czw;
    uint64_t zc_serial;
} g_lamps;

void gfx_lamps(const GfxWorldLight* l, uint32_t n)
{
    n = n > 256 ? 256 : n;
    if (n)
        memcpy(g_lamps.l, l, n * sizeof *l);
    g_lamps.n = n, g_lamps.set = g_serial;
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
    float* L = (float*)ring((size_t)g_lamps.n * 64 + 64, 256, &g_lamps.lb, &g_lamps.lo);
    uint16_t* T = (uint16_t*)ring((size_t)tx * ty * 64, 256, &g_lamps.tb, &g_lamps.to);
    memset(T, 0, (size_t)tx * ty * 64);
    for (uint32_t i = 0; i < g_lamps.n; ++i)
    {
        const GfxWorldLight* l = &g_lamps.l[i];
        float* o = L + 16 * i;
        memcpy(o, l->pos, 16);
        for (int c = 0; c < 3; ++c)
            o[4 + c] = l->diffuse[c] * k, o[12 + c] = l->ambient[c] * k;
        memcpy(o + 8, l->att, 16);
        o[11] = 0.0f;
        if (g_lamps.sh_serial && g_lamps.sh_serial + 1 == g_serial)
            for (uint32_t k = 0; k < g_lamps.nsh; ++k)
            {
                float ex = l->pos[0] - g_lamps.shpos[k][0], ey = l->pos[1] - g_lamps.shpos[k][1], ez = l->pos[2] - g_lamps.shpos[k][2];
                if (ex * ex + ey * ey + ez * ez < 0.01f)
                    o[11] = (float)(k + 1);
            }
        float r = l->pos[3];
        float dx = l->pos[0] - g_lamps.cam[0], dy = l->pos[1] - g_lamps.cam[1], dz = l->pos[2] - g_lamps.cam[2];
        if (dx * dx + dy * dy + dz * dz > far2 + r * r + 2.0f * r * sqrtf(far2))
            continue;
        /* its box's corners on screen: the rectangle it can light (all of it when a corner is behind the camera) */
        float sx0 = 1e30f, sy0 = 1e30f, sx1 = -1e30f, sy1 = -1e30f;
        int behind = 0;
        /* the lamp's sphere wholly behind the camera: none */
        {
            float vz = l->pos[0] * view[2] + l->pos[1] * view[6] + l->pos[2] * view[10] + view[14];
            float depth = proj[11] < 0.0f ? -vz : vz; /* along the view, toward what is seen */
            if (depth < -r)
                continue;
        }
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
    float lu[12] = { x0, y0, w, h, (float)tx, (float)ty, 0, 0, 0, 0, 0, 0 };
    memcpy(g_lamps.lu, lu, sizeof lu);
    g_lamps.binned = g_serial;
}

/* the lamps for a batch's draws: binned at the frame's first zone batch (view etc. given), then bound; 0 none */
static int lamps_bind(id<MTLRenderCommandEncoder> e, const float* view, const float* proj, const uint32_t* vp)
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
    [e setFragmentBuffer:g_lamps.lb offset:g_lamps.lo atIndex:12];
    [e setFragmentBuffer:g_lamps.tb offset:g_lamps.to atIndex:13];
    int sh = g_lamps.lsm && g_lamps.nsh && g_lamps.sh_serial + 1 == g_serial;
    g_lamps.lu[6] = sh ? 1.0f : 0.0f, g_lamps.lu[7] = g_fxs.scene_lamp_debug;
    int zs = sh && g_lamps.zlsm && g_lamps.zn == g_lamps.nsh && !memcmp(g_lamps.zpos, g_lamps.shpos, sizeof g_lamps.zpos[0] * g_lamps.nsh);
    g_lamps.lu[8] = zs ? 1.0f : 0.0f;
    g_lamps.lu[9] = fmaxf(g_fxs.lamp_size, 0.0f) * fmaxf(g_fxs.shadow_softness, 0.0f);
    g_lamps.lu[10] = g_fxs.shadow_soft_q >= 1.5f ? 8.0f : 4.0f;
    [e setFragmentBytes:g_lamps.lu length:sizeof g_lamps.lu atIndex:14];
    static id<MTLTexture> dummy, zdummy;
    if (!zdummy)
    {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:1 height:1 mipmapped:NO];
        td.textureType = MTLTextureType2DArray, td.arrayLength = 24;
        td.usage = MTLTextureUsageShaderRead, td.storageMode = MTLStorageModePrivate;
        zdummy = [g_dev newTextureWithDescriptor:td];
    }
    [e setFragmentTexture:zs ? g_lamps.zlsm : zdummy atIndex:8];
    if (!dummy)
    {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth16Unorm width:1 height:1 mipmapped:NO];
        td.textureType = MTLTextureType2DArray, td.arrayLength = 24;
        td.usage = MTLTextureUsageShaderRead, td.storageMode = MTLStorageModePrivate;
        dummy = [g_dev newTextureWithDescriptor:td];
    }
    [e setFragmentTexture:sh ? g_lamps.lsm : dummy atIndex:7];
    if (sh)
        [e setFragmentBytes:g_lamps.shm length:64 * 6 * g_lamps.nsh atIndex:15];
    else
        [e setFragmentBytes:g_lamps.shm length:64 atIndex:15];
    return 1;
}

/* the highlights' settings for a batch's draws (buffer 11, fragment); 0 off (scene_spec 0, or no sun shadows to place
 * them by) */
static int spec_bind(id<MTLRenderCommandEncoder> e, int can)
{
    if (!can || g_fxs.scene_spec <= 0.0f)
        return 0;
    float k[8] = { g_fxs.scene_spec, fmaxf(g_fxs.scene_spec_lamps, 0.0f), fminf(fmaxf(g_fxs.scene_spec_wet, 0.0f), 1.0f),
        fmaxf(g_fxs.scene_spec_aa, 0.0f), g_rt && g_rt->tex.pixelFormat == MTLPixelFormatRGBA16Float ? 1.0f : 0.0f, 0, 0, 0 };
    [e setFragmentBytes:k length:sizeof k atIndex:11];
    return 1;
}

static id<MTLRenderPipelineState> world_pipeline(int pixel, int atest, int blend, int shadow, int gi, int ao, int spec, int lamps,
    MTLPixelFormat color, MTLPixelFormat depth, MTLPixelFormat stencil)
{
    uint32_t key = (uint32_t)pixel | (uint32_t)atest << 1 | (uint32_t)blend << 2 | (uint32_t)color << 3 | (uint32_t)depth << 13 |
        (uint32_t)(stencil != MTLPixelFormatInvalid) << 23 | (uint32_t)shadow << 24 | (uint32_t)gi << 25 | (uint32_t)ao << 26 |
        (uint32_t)spec << 27 | (uint32_t)lamps << 28;
    for (uint32_t i = 0; i < g_world.npipe; ++i)
        if (g_world.pipe[i].key == key)
            return g_world.pipe[i].p;
    if (g_world.npipe == sizeof g_world.pipe / sizeof g_world.pipe[0])
        return nil;
    id<MTLRenderPipelineState> p = nil;
    @autoreleasepool
    {
        MTLFunctionConstantValues* fc = [[MTLFunctionConstantValues alloc] init];
        bool bp = pixel != 0, ba = atest != 0, bs = shadow != 0, bg = gi != 0, bo = ao != 0, bsp = spec != 0, bl = lamps != 0;
        [fc setConstantValue:&bp type:MTLDataTypeBool atIndex:0];
        [fc setConstantValue:&ba type:MTLDataTypeBool atIndex:1];
        [fc setConstantValue:&bs type:MTLDataTypeBool atIndex:2];
        [fc setConstantValue:&bg type:MTLDataTypeBool atIndex:3];
        [fc setConstantValue:&bo type:MTLDataTypeBool atIndex:4];
        [fc setConstantValue:&bsp type:MTLDataTypeBool atIndex:5];
        [fc setConstantValue:&bl type:MTLDataTypeBool atIndex:6];
        NSError* err = nil;
        id<MTLFunction> vf = [g_world.lib newFunctionWithName:@"world_vs" constantValues:fc error:&err];
        id<MTLFunction> ff = [g_world.lib newFunctionWithName:@"world_fs" constantValues:fc error:&err];
        [fc release];
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.vertexDescriptor = g_world.vd;
        MTLRenderPipelineColorAttachmentDescriptor* c = pd.colorAttachments[0];
        c.pixelFormat = color;
        if (blend)
        {
            c.blendingEnabled = YES;
            c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
            c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        pd.depthAttachmentPixelFormat = depth;
        pd.stencilAttachmentPixelFormat = stencil;
        if (vf && ff)
            p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p)
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED),
                fprintf(stderr, "[recomp] gfx: scene renderer pipeline failed: %s\n", err ? [[err localizedDescription] UTF8String] : "?");
        [vf release];
        [ff release];
        [pd release];
    }
    if (p)
        g_world.pipe[g_world.npipe].key = key, g_world.pipe[g_world.npipe++].p = p;
    return p;
}

static void mat_mul(float* out, const float* a, const float* b) /* D3D order: out = a x b */
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof r);
}

/* A camera cut (the view jumping: first person and back, a cutscene, a warp): the camera moved more than two units or
 * turned more than 25 degrees since the last frame that had a view. Effects that carry results over frames (the scene
 * renderer's occlusion, the sun shafts) start afresh and the depth of field's focus is taken at once, so none of them
 * shows the last view's results for a frame. Noted once a frame from the view (cam_cut_note), read as g_cam_cut. */
static uint64_t g_cam_cut;          /* the last frame found to be a cut */
static uint64_t g_cam_seen;         /* the last frame a view was noted */
static float g_cam_last[16];        /* its view matrix */
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

#include "gfx_ao_metal.inc"

static int g_fixed_light; /* the zone's light is fixed (scene.c, the weather record): the modern fog a light dust */
void gfx_set_fixed_light(int fixed) { g_fixed_light = fixed != 0; }
static int g_weather = -1; /* the zone's weather (host64, the server's packets): the ground mist and its drift */
void gfx_set_weather(int weather) { g_weather = weather; }

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
        GfxWorldItem* v = (GfxWorldItem*)realloc(g_lamps.zi, (size_t)c * sizeof *v);
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
        GfxWorldItem* o = &g_lamps.zi[g_lamps.nzi++];
        *o = *it, o->inst = base + it->inst;
    }
}

/* the frame's zone into the cubes of the lamps lamp_shadows_draw just took, when they changed or every 600 frames; read from
 * the next frame on. The zone is what the game drew this frame: none of what lies out of its view */
static void lamp_zone_cubes(id<MTLCommandBuffer> cb)
{
    if (!g_lamps.nsh || g_lamps.sh_serial != g_serial || g_lamps.zc_serial != g_serial || !g_lamps.nzi || !g_shd.zone[0] ||
        !g_shd.zone[1])
        return;
    int same = g_lamps.zn == g_lamps.nsh && g_lamps.z_serial && g_serial - g_lamps.z_serial < 600 &&
        !memcmp(g_lamps.zpos, g_lamps.shpos, sizeof g_lamps.zpos[0] * g_lamps.nsh);
    if (same)
        return;
    uint32_t size = g_lamps.lsize;
    if (!g_lamps.zlsm || g_lamps.zlsm.width != size)
    {
        [g_lamps.zlsm release];
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:size height:size
                                                                                   mipmapped:NO];
        td.textureType = MTLTextureType2DArray, td.arrayLength = 24;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, td.storageMode = MTLStorageModePrivate;
        g_lamps.zlsm = [g_dev newTextureWithDescriptor:td];
        if (!g_lamps.zlsm)
            return;
    }
    id<MTLBuffer> wb;
    NSUInteger wo;
    memcpy(ring((size_t)g_lamps.nzw * 64, 256, &wb, &wo), g_lamps.zw, (size_t)g_lamps.nzw * 64);
    for (uint32_t layer = 0; layer < 6 * g_lamps.nsh; ++layer)
    {
        const float* L = g_lamps.shpos[layer / 6];
        float reach = L[3] + 40.0f;
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = g_lamps.zlsm;
        rp.depthAttachment.slice = layer;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        ts_mark(rp, GFX_PASS_LAMPS);
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setViewport:(MTLViewport){ 0, 0, (double)size, (double)size, 0, 1 }];
        [e setDepthStencilState:g_shd.ds];
        [e setCullMode:MTLCullModeNone];
        [e setDepthClipMode:MTLDepthClipModeClamp];
        [e setDepthBias:0 slopeScale:2.0f clamp:0.0f];
        [e setVertexBytes:g_lamps.shm[layer] length:64 atIndex:1];
        [e setVertexBuffer:wb offset:wo atIndex:2];
        int cur = -1;
        for (uint32_t i = 0; i < g_lamps.nzi; ++i)
        {
            const GfxWorldItem* it = &g_lamps.zi[i];
            const float* w = g_lamps.zw[it->inst];
            float dx = w[12] - L[0], dy = w[13] - L[1], dz = w[14] - L[2];
            if (dx * dx + dy * dy + dz * dz > reach * reach)
                continue;
            int at = (it->flags & GFX_WORLD_ATEST) && it->tex;
            if (at != cur)
                [e setRenderPipelineState:g_shd.zone[at]], cur = at;
            if (at)
            {
                [e setFragmentTexture:it->tex->view atIndex:0];
                [e setFragmentSamplerState:sampler(&it->samp) atIndex:0];
            }
            [e setVertexBuffer:it->vb->b offset:0 atIndex:0];
            [e setVertexBytes:&it->inst length:4 atIndex:4];
            [e drawPrimitives:it->prim == GFX_TRIANGLESTRIP ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle
                  vertexStart:it->first
                  vertexCount:vertex_count(it->prim, it->count)];
        }
        [e endEncoding];
    }
    memcpy(g_lamps.zpos, g_lamps.shpos, sizeof g_lamps.zpos);
    g_lamps.zn = g_lamps.nsh, g_lamps.z_serial = g_serial;
}

int gfx_world_draw(const GfxWorldFrame* f)
{
    if (!g_dev || !f->nitem || !world_init())
        return 0;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    int drew = 0;
    @autoreleasepool
    {
        /* timed: a render pass of its own (an extra load and store of the target, only while timing) */
        int timed = gfx_pass_timing_on();
        if (timed)
            end_pass(), g_pass_next = GFX_PASS_WORLD;
        if (!begin_pass())
        {
            g_pass_next = -1;
            return 0;
        }
        id<MTLTexture> depth = depth_attachment();
        MTLPixelFormat cf = g_rt->tex.pixelFormat, df = depth ? depth.pixelFormat : MTLPixelFormatInvalid;
        MTLPixelFormat sf = depth && g_ds->has_stencil ? df : MTLPixelFormatInvalid;
        struct
        {
            float vp[16];
            float params[4];
        } fu;
        mat_mul(fu.vp, f->view, f->proj);
        fu.params[0] = f->alpha_ref, fu.params[1] = f->pixel[0], fu.params[2] = f->pixel[1], fu.params[3] = 0.0f;
        id<MTLBuffer> fb, ib, lb;
        NSUInteger fo, io, lo;
        memcpy(ring(sizeof fu, 256, &fb, &fo), &fu, sizeof fu);
        memcpy(ring((size_t)f->ninst * sizeof(GfxWorldInst), 256, &ib, &io), f->inst, (size_t)f->ninst * sizeof(GfxWorldInst));
        GfxWorldLight none;
        memset(&none, 0, sizeof none);
        const GfxWorldLight* lights = f->nlight ? f->light : &none;
        uint32_t nl = f->nlight ? f->nlight : 1;
        memcpy(ring((size_t)nl * sizeof(GfxWorldLight), 256, &lb, &lo), lights, (size_t)nl * sizeof(GfxWorldLight));
        [g_enc setVertexBuffer:fb offset:fo atIndex:1];
        [g_enc setFragmentBuffer:fb offset:fo atIndex:1];
        [g_enc setVertexBuffer:ib offset:io atIndex:2];
        [g_enc setFragmentBuffer:ib offset:io atIndex:2];
        [g_enc setVertexBuffer:lb offset:lo atIndex:3];
        [g_enc setFragmentBuffer:lb offset:lo atIndex:3];
        bound_forget();
        set_viewport(f->vp);
        int32_t none_sc[4] = { 0, 0, 0, 0 };
        set_scissor(none_sc);
        g_bound.valid = 1;
        int pixel = f->mode == 2, shadow = g_shd.on;
        if (shadow)
            shadow_bind(g_enc);
        int gi = gi_bind(g_enc);
        ao_note(f, depth);
        int ao = ao_bind(g_enc);
        int spec = spec_bind(g_enc, shadow && pixel);
        int lamps = pixel && lamps_bind(g_enc, f->view, f->proj, f->vp);
        if (lamps)
            lamp_zone_collect(f);
        id<MTLRenderPipelineState> cur = nil;
        id<MTLDepthStencilState> curds = nil;
        int cull = -1, bias = INT_MIN;
        for (uint32_t i = 0; i < f->nitem; ++i)
        {
            const GfxWorldItem* it = &f->item[i];
            if (!it->vb || !it->tex || !it->count)
                continue;
            int blend = (it->flags & GFX_WORLD_BLEND) != 0;
            id<MTLRenderPipelineState> p = world_pipeline(pixel, (it->flags & GFX_WORLD_ATEST) != 0, blend, shadow, gi, ao, spec, lamps, cf, df, sf);
            if (!p)
                continue;
            if (p != cur)
                [g_enc setRenderPipelineState:p], cur = p;
            id<MTLDepthStencilState> ds = g_world.ds[blend];
            if (ds != curds)
                [g_enc setDepthStencilState:ds], curds = ds;
            int c = (it->flags & GFX_WORLD_TWOSIDED) ? 0 : (it->flags & GFX_WORLD_MIRRORED) ? 1 : 2;
            if (c != cull)
                [g_enc setCullMode:c == 2 ? MTLCullModeBack : c == 1 ? MTLCullModeFront : MTLCullModeNone], cull = c;
            int zb = blend ? f->zbias_blend : f->zbias_opaque;
            if (zb != bias)
                [g_enc setDepthBias:-(float)zb slopeScale:-(float)zb * 0.5f clamp:0], bias = zb;
            [g_enc setVertexBuffer:it->vb->b offset:0 atIndex:0];
            it->vb->used = g_serial;
            struct
            {
                uint32_t inst, flags;
            } dc = { it->inst, it->flags };
            [g_enc setVertexBytes:&dc length:sizeof dc atIndex:4];
            [g_enc setFragmentBytes:&dc length:sizeof dc atIndex:4];
            GfxSampler sk = it->samp;
            sk.addr_u = sk.addr_v = sk.addr_w = (it->flags & GFX_WORLD_CLAMP) ? 3 : 1;
            if (g_fxs.fx != 0.0f && g_fxs.aniso > 1.0f && it->tex->levels > 1 && it->tex->levels < 32 && sk.min >= 2 &&
                it->tex->filled == (1u << it->tex->levels) - 1)
                sk.min = 3, sk.mip = 2, sk.max_aniso = (uint8_t)(g_fxs.aniso > 16.0f ? 16.0f : g_fxs.aniso);
            [g_enc setFragmentTexture:it->tex->view atIndex:0];
            [g_enc setFragmentSamplerState:sampler(&sk) atIndex:0];
            it->tex->used = g_serial;
            uint32_t n = vertex_count(it->prim, it->count);
            [g_enc drawPrimitives:it->prim == GFX_TRIANGLESTRIP ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle
                      vertexStart:it->first
                      vertexCount:n];
            ++drew;
        }
        bound_forget(); /* what the game's next draw finds bound is not what g_bound says */
        g_cmd_draws += drew;
        if (timed)
            end_pass();
    }
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws += drew;
    return drew > 0;
}

/* --- the scene renderer's characters (gfx_chars_draw, gfx.h) -------------------------------------------------------
 * Skinned on the GPU: each vertex holds its bone-space position and normal for up to two bones (weights applied),
 * the bones and their mirror bones; the frame's palettes are one buffer. Lit per vertex with D3D's fixed function as
 * the game sets it for a character (its per-actor lights, ambient and material), then its two texture stages. */
static const char CHARS_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constant bool k_shadow [[function_constant(0)]];\n"
    "constant bool k_gi [[function_constant(1)]];\n"
    "constant bool k_ao [[function_constant(2)]];\n"
    "constant bool k_spec [[function_constant(3)]];\n"
    "constant bool k_lamps [[function_constant(4)]];\n"
    SHADOW_MSL
    SPEC_MSL
    LAMP_MSL
    GI_MSL
    AO_MSL
    "struct CF { float4 params; };\n" /* alpha reference 0..255, the pixel-centre fixup */
    "struct CL { float4 pos, dir, diffuse, ambient, att; };\n"
    "struct CS { float4x4 wvp, world; CL light[4]; float4 ambient, md, ma, me, tf, fog, fog_col; };\n"
    "struct CD { uint palette, state, mirror, pad; };\n"
    "struct CVI { float4 a [[attribute(0)]]; float4 b [[attribute(1)]]; float4 na [[attribute(2)]]; float4 nb [[attribute(3)]];\n"
    "             uint bones [[attribute(4)]]; uint mir [[attribute(5)]]; float2 uv [[attribute(6)]]; };\n"
    "struct CVO { float4 pos [[position]]; float2 uv; float4 col; float3 sun; float3 wp; float3 wn; float z; float3 lmd [[flat]];\n"
    "             float3 lma [[flat]];\n"
    "             float4 tf [[flat]]; float4 fog [[flat]]; float3 fog_col [[flat]]; float4 aop; float3 camb; };\n"
    "constant float3 SIGN[4] = { float3(1.0), float3(-1.0, 1.0, 1.0), float3(1.0, -1.0, 1.0), float3(1.0, 1.0, -1.0) };\n"
    /* the vertex in the game's skinned space: two bones, or their mirror bones mirrored */
    "static float3 skin(CVI v, constant CD& D, const device float4x4* P, thread float3& n) {\n"
    "  uint ba = v.bones & 0xffu, bb = (v.bones >> 8) & 0xffu;\n"
    "  float4 a = v.a, b = v.b;\n"
    "  float3 na = v.na.xyz, nb = v.nb.xyz;\n"
    "  if (D.mirror != 0u) {\n"
    "    ba = (v.bones >> 16) & 0xffu, bb = v.bones >> 24;\n"
    "    float3 sa = SIGN[v.mir & 3u], sb = SIGN[(v.mir >> 2) & 3u];\n"
    "    a.xyz *= sa; b.xyz *= sb; na *= sa; nb *= sb;\n"
    "  }\n"
    "  float4x4 MA = P[D.palette + ba], MB = P[D.palette + bb];\n"
    "  n = (MA * float4(na, 0.0)).xyz + (MB * float4(nb, 0.0)).xyz;\n"
    "  return (MA * a + MB * b).xyz;\n"
    "}\n"
    "vertex CVO char_vs(CVI v [[stage_in]], constant CF& F [[buffer(2)]], const device float4x4* P [[buffer(3)]],\n"
    "                   constant CS* S [[buffer(4)]], constant CD& D [[buffer(5)]],\n"
    "                   constant SH& H [[buffer(6), function_constant(k_shadow)]],\n"
    "                   constant GU& G [[buffer(8), function_constant(k_gi)]],\n"
    "                   const device uint* PS [[buffer(9), function_constant(k_gi)]],\n"
    "                   texture2d<float> gir [[texture(4), function_constant(k_gi)]],\n"
    "                   texture2d<float> gid [[texture(5), function_constant(k_gi)]],\n"
    "                   constant AU& A [[buffer(10), function_constant(k_ao)]]) {\n"
    "  constant CS& s = S[D.state];\n"
    "  float3 n, p = skin(v, D, P, n);\n"
    "  float3 wp = (s.world * float4(p, 1.0)).xyz;\n"
    "  float3 wn = normalize((s.world * float4(n, 0.0)).xyz);\n" /* NORMALIZENORMALS */
    "  CVO o;\n"
    "  o.pos = s.wvp * float4(p, 1.0);\n"
    "  o.z = o.pos.w;\n"
    "  o.pos.x += o.pos.w * F.params.y;\n"
    "  o.pos.y -= o.pos.w * F.params.z;\n"
    "  o.uv = v.uv;\n"
    "  float3 amb = s.ambient.rgb, dif = float3(0.0), sun = float3(0.0);\n"
    "  if (k_gi) { float4 g = gi_at(G, PS, gir, gid, wp, wn); amb = amb * g.a + g.rgb * G.k.x; }\n" /* the bounce light */
    "  for (int k = 0; k < 4; ++k) {\n"
    "    CL l = s.light[k];\n"
    "    if (l.dir.w == 0.0 || (k_lamps && l.dir.w > 1.5)) continue;\n" /* (point lights: the lamps, per pixel: char_fs) */
    "    float at = 1.0;\n"
    "    float3 L = l.dir.xyz;\n"
    "    if (l.dir.w > 1.5) {\n"
    "      float3 d = l.pos.xyz - wp;\n"
    "      float dist = length(d);\n"
    "      if (dist > l.pos.w) continue;\n"
    "      L = d / max(dist, 1e-6);\n"
    "      at = 1.0 / max(l.att.x + l.att.y * dist + l.att.z * dist * dist, 1e-6);\n"
    "    }\n"
    "    amb += l.ambient.rgb * at;\n"
    "    float3 t = l.diffuse.rgb * at * max(dot(wn, L), 0.0);\n"
    "    if (k_shadow && l.dir.w < 1.5 && dot(L, H.dir.xyz) > 0.98) sun += t; else dif += t;\n" /* the sun or moon */
    "  }\n"
    "  float3 rest = s.ma.rgb * amb + s.md.rgb * dif + s.me.rgb;\n"
    "  o.col = float4(k_shadow || k_ao ? rest : saturate(rest), s.md.a);\n"
    "  o.camb = s.ma.rgb * amb;\n" /* the ambient's share, which the occlusion scales */
    "  o.aop = k_ao ? A.pvp * float4(wp, 1.0) : float4(0.0);\n"
    "  o.sun = s.md.rgb * sun;\n"
    "  o.wp = wp, o.wn = wn;\n"
    "  o.lmd = s.md.rgb * s.fog_col.a, o.lma = s.ma.rgb;\n" /* a lamp's light as the game's would be: the material's scale and colours */
    "  o.tf = s.tf;\n"
    "  o.fog = s.fog;\n"
    "  o.fog_col = s.fog_col.rgb;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 char_fs(CVO i [[stage_in]], constant CF& F [[buffer(2)]], texture2d<float> t [[texture(0)]],\n"
    "                        sampler sm [[sampler(0)]], constant SH& H [[buffer(6), function_constant(k_shadow)]],\n"
    "                        depth2d_array<float> zm [[texture(1), function_constant(k_shadow)]],\n"
    "                        depth2d_array<float> cm [[texture(2), function_constant(k_shadow)]],\n"
    "                         depth2d_array<float> ct [[texture(3), function_constant(k_shadow)]],\n"
    "                        sampler cs [[sampler(1), function_constant(k_shadow)]],\n"
    "                        constant float4& LG [[buffer(7), function_constant(k_shadow)]],\n"
    "                        constant AU& A [[buffer(10), function_constant(k_ao)]],\n"
    "                        texture2d<float> aot [[texture(6), function_constant(k_ao)]],\n"
    "                        constant SP& SPc [[buffer(11), function_constant(k_spec)]],\n"
    "                        const device LL* LS [[buffer(12), function_constant(k_lamps)]],\n"
    "                        const device ushort* TL [[buffer(13), function_constant(k_lamps)]],\n"
    "                        constant LU& LUc [[buffer(14), function_constant(k_lamps)]],\n"
    "                        depth2d_array<float> lsm [[texture(7), function_constant(k_lamps)]],\n"
    "                        depth2d_array<float> zlsm [[texture(8), function_constant(k_lamps)]],\n"
    "                        constant float4x4* lsmm [[buffer(15), function_constant(k_lamps)]]) {\n"
    "  float4 tx = t.sample(sm, i.uv);\n"
    "  float3 col = i.col.rgb, lmp = float3(0.0);\n"
    "  if (k_lamps) {\n"
    "    float3 ld = float3(0.0), la = float3(0.0);\n"
    "    lamp_light(LUc, LS, TL, i.pos.xy, i.wp, normalize(i.wn), ld, la, lsm, zlsm, lsmm, k_shadow ? i.wp + LG.xyz : i.wp, false);\n"
    "    lmp = i.lmd * ld + i.lma * la;\n"
    "    col += lmp;\n"
    "  }\n"
    "  float shv = 1.0;\n"
    "  if (k_ao) {\n"
    "    float ao = pow(ao_at(A, aot, i.aop), A.k.y);\n"
    "    if (A.k.z > 0.5) return float4(float3(ao), 1.0);\n"
    "    col -= i.camb * (1.0 - ao);\n"
    "    if (!k_shadow) col = saturate(col);\n"
    "  }\n"
    "  if (k_shadow) {\n"
    "    float3 sun = i.sun;\n"
    "    if (any(sun > 0.0)) {\n"
    "      shv = shadow(H, zm, cm, ct, cs, i.wp + LG.xyz, normalize(i.wn), false);\n" /* (PCSS speckles curved, close self-shadow) */
    "      float3 Y = float3(0.2126, 0.7152, 0.0722);\n" /* the lamps fill it, as the zone's (shadow_fill) */
    "      shv = mix(shv, 1.0, saturate(H.ctx.w * dot(lmp, Y) / max(dot(sun, Y), 1e-4)));\n"
    "      sun *= shv;\n"
    "    }\n"
    "    col = saturate(col + sun);\n"
    "  }\n"
    "  float3 c = saturate(2.0 * saturate(2.0 * tx.rgb * col) * i.tf.rgb);\n" /* MODULATE2X, then MODULATE2X by the factor */
    "  float a = saturate(4.0 * saturate(2.0 * tx.a * i.tf.a) * i.tf.a);\n"            /* MODULATE2X, then MODULATE4X */
    "  if (!(a * 255.0 > F.params.x)) discard_fragment();\n"
    "  if (k_spec && k_shadow) {\n" /* the sun's highlight: its colour from what it lit here (its diffuse over N.L) */
    "    float3 Nn = normalize(i.wn), V = normalize(H.cam.xyz - i.wp), Ls = H.dir.xyz;\n"
    "    float3 tc = saturate(2.0 * tx.rgb * i.tf.rgb);\n"
    "    float lum = dot(tc, float3(0.299, 0.587, 0.114));\n"
    "    float sat = max(tc.r, max(tc.g, tc.b)) - min(tc.r, min(tc.g, tc.b));\n"
    "    float m = smoothstep(0.25, 0.6, lum) * (1.0 - smoothstep(0.08, 0.3, sat));\n" /* bright and grey: metal */
    "    float a2 = spec_a2(mix(0.7, 0.3, m), Nn, SPc.k.w);\n"
    "    float3 sc = i.sun / max(saturate(dot(Nn, Ls)), 0.2);\n"
    "    float3 f = mix(float3(0.03), tc * 0.6, m);\n"
    "    float g = spec_ggx(Nn, V, Ls, a2, 1.0);\n" /* (Fresnel's own, tinted below) */
    "    float3 H_ = normalize(V + Ls);\n"
    "    float fr = pow(1.0 - saturate(dot(V, H_)), 5.0);\n"
    "    float3 F_ = f + (1.0 - f) * fr;\n"
    "    if (SPc.pad.x > 0.0) c += sc * g * F_ * (SPc.k.x * shv);\n"
    "    else c = 1.0 - (1.0 - c) * exp(-sc * g * F_ * (SPc.k.x * shv));\n"
    "  }\n"
    "  if (i.fog.z > 0.0) c = mix(i.fog_col, c, saturate((i.fog.y - i.z) / max(i.fog.y - i.fog.x, 1e-6)));\n"
    "  return float4(c, a);\n"
    "}\n"
    /* the characters into the first cascades at once (one layer an instance), alpha tested as they are drawn */
    "struct CSO { float4 pos [[position]]; float2 uv; float4 tf [[flat]]; uint layer [[render_target_array_index]]; };\n"
    "vertex CSO cshadow_vs(CVI v [[stage_in]], const device float4x4* P [[buffer(3)]], constant CS* S [[buffer(4)]],\n"
    "                      constant CD& D [[buffer(5)]], constant float4x4* LM [[buffer(7)]],\n"
    "                      uint li [[instance_id]]) {\n"
    "  constant CS& s = S[D.state];\n"
    "  float3 n, p = skin(v, D, P, n);\n"
    "  CSO o;\n"
    "  o.pos = LM[li] * (s.world * float4(p, 1.0));\n"
    "  o.uv = v.uv;\n"
    "  o.tf = s.tf;\n"
    "  o.layer = li;\n"
    "  return o;\n"
    "}\n"
    "fragment void cshadow_fs(CSO i [[stage_in]], constant CF& F [[buffer(2)]], texture2d<float> t [[texture(0)]],\n"
    "                         sampler sm [[sampler(0)]]) {\n"
    "  float a = saturate(4.0 * saturate(2.0 * t.sample(sm, i.uv).a * i.tf.a) * i.tf.a);\n"
    "  if (!(a * 255.0 > F.params.x)) discard_fragment();\n"
    "}\n";

static struct
{
    int tried;
    id<MTLLibrary> lib;
    MTLVertexDescriptor* vd;
    struct
    {
        uint32_t key;
        id<MTLRenderPipelineState> p;
    } pipe[16];
    uint32_t npipe;
    id<MTLDepthStencilState> ds;
} g_chars;

static int chars_init(void)
{
    if (g_chars.tried)
        return g_chars.lib != nil;
    g_chars.tried = 1;
    g_chars.lib = compile(CHARS_MSL);
    if (!g_chars.lib)
        return 0;
    MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
    for (int i = 0; i < 4; ++i)
        vd.attributes[i].format = MTLVertexFormatFloat4, vd.attributes[i].offset = 16 * i, vd.attributes[i].bufferIndex = 0;
    vd.attributes[4].format = MTLVertexFormatUInt, vd.attributes[4].offset = 64, vd.attributes[4].bufferIndex = 0;
    vd.attributes[5].format = MTLVertexFormatUInt, vd.attributes[5].offset = 68, vd.attributes[5].bufferIndex = 0;
    vd.attributes[6].format = MTLVertexFormatFloat2, vd.attributes[6].offset = 0, vd.attributes[6].bufferIndex = 1;
    vd.layouts[0].stride = sizeof(GfxCharVertex);
    vd.layouts[1].stride = 8;
    g_chars.vd = vd;
    MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
    d.depthCompareFunction = MTLCompareFunctionLessEqual;
    d.depthWriteEnabled = YES;
    g_chars.ds = [g_dev newDepthStencilStateWithDescriptor:d];
    [d release];
    return 1;
}

static id<MTLRenderPipelineState> chars_pipeline(int shadow, int gi, int ao, int spec, int lamps, MTLPixelFormat color,
    MTLPixelFormat depth, MTLPixelFormat stencil)
{
    uint32_t key = (uint32_t)color | (uint32_t)depth << 10 | (uint32_t)(stencil != MTLPixelFormatInvalid) << 20 | (uint32_t)shadow << 21 |
        (uint32_t)gi << 22 | (uint32_t)ao << 23 | (uint32_t)spec << 24 | (uint32_t)lamps << 25;
    for (uint32_t i = 0; i < g_chars.npipe; ++i)
        if (g_chars.pipe[i].key == key)
            return g_chars.pipe[i].p;
    if (g_chars.npipe == sizeof g_chars.pipe / sizeof g_chars.pipe[0])
        return nil;
    id<MTLRenderPipelineState> p = nil;
    @autoreleasepool
    {
        MTLFunctionConstantValues* fc = [[MTLFunctionConstantValues alloc] init];
        bool bs = shadow != 0, bg = gi != 0, bo = ao != 0, bsp = spec != 0, bl = lamps != 0;
        [fc setConstantValue:&bs type:MTLDataTypeBool atIndex:0];
        [fc setConstantValue:&bg type:MTLDataTypeBool atIndex:1];
        [fc setConstantValue:&bo type:MTLDataTypeBool atIndex:2];
        [fc setConstantValue:&bsp type:MTLDataTypeBool atIndex:3];
        [fc setConstantValue:&bl type:MTLDataTypeBool atIndex:4];
        NSError* err = nil;
        id<MTLFunction> vf = [g_chars.lib newFunctionWithName:@"char_vs" constantValues:fc error:&err];
        id<MTLFunction> ff = [g_chars.lib newFunctionWithName:@"char_fs" constantValues:fc error:&err];
        [fc release];
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.vertexDescriptor = g_chars.vd;
        pd.colorAttachments[0].pixelFormat = color;
        pd.depthAttachmentPixelFormat = depth;
        pd.stencilAttachmentPixelFormat = stencil;
        if (vf && ff)
            p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p)
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED),
                fprintf(stderr, "[recomp] gfx: scene renderer characters pipeline failed: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "?");
        [vf release];
        [ff release];
        [pd release];
    }
    if (p)
        g_chars.pipe[g_chars.npipe].key = key, g_chars.pipe[g_chars.npipe++].p = p;
    return p;
}

int gfx_chars_draw(const GfxCharFrame* f)
{
    if (!g_dev || !f->nitem || !f->nbones || !chars_init())
        return 0;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    int drew = 0;
    @autoreleasepool
    {
        int timed = gfx_pass_timing_on();
        if (timed)
            end_pass(), g_pass_next = GFX_PASS_CHARS;
        if (!begin_pass())
        {
            g_pass_next = -1;
            return 0;
        }
        id<MTLTexture> depth = depth_attachment();
        MTLPixelFormat df = depth ? depth.pixelFormat : MTLPixelFormatInvalid;
        int ao = ao_bind(g_enc);
        int spec = spec_bind(g_enc, g_shd.on);
        int lamps = lamps_bind(g_enc, NULL, NULL, NULL);
        id<MTLRenderPipelineState> p = chars_pipeline(g_shd.on, g_gi.on, ao, spec, lamps, g_rt->tex.pixelFormat, df,
            depth && g_ds->has_stencil ? df : MTLPixelFormatInvalid);
        if (!p)
        {
            if (timed)
                end_pass();
            return 0;
        }
        float cf[4] = { f->alpha_ref, f->pixel[0], f->pixel[1], 0.0f };
        id<MTLBuffer> pb, sb;
        NSUInteger po, so;
        memcpy(ring((size_t)f->nbones * 64, 256, &pb, &po), f->palette, (size_t)f->nbones * 64);
        memcpy(ring((size_t)f->nstate * sizeof(GfxCharState), 256, &sb, &so), f->state, (size_t)f->nstate * sizeof(GfxCharState));
        [g_enc setRenderPipelineState:p];
        [g_enc setDepthStencilState:g_chars.ds];
        [g_enc setCullMode:MTLCullModeNone];
        [g_enc setDepthBias:0 slopeScale:0 clamp:0];
        [g_enc setVertexBytes:cf length:sizeof cf atIndex:2];
        [g_enc setFragmentBytes:cf length:sizeof cf atIndex:2];
        [g_enc setVertexBuffer:pb offset:po atIndex:3];
        [g_enc setVertexBuffer:sb offset:so atIndex:4];
        if (g_shd.on)
            shadow_bind(g_enc);
        gi_bind(g_enc);
        bound_forget();
        set_viewport(f->vp);
        int32_t none_sc[4] = { 0, 0, 0, 0 };
        set_scissor(none_sc);
        g_bound.valid = 1;
        for (uint32_t i = 0; i < f->nitem; ++i)
        {
            const GfxCharItem* it = &f->item[i];
            if (!it->vb || !it->uv || !it->ib || !it->tex || !it->count)
                continue;
            [g_enc setVertexBuffer:it->vb->b offset:0 atIndex:0];
            [g_enc setVertexBuffer:it->uv->b offset:0 atIndex:1];
            it->vb->used = it->uv->used = it->ib->used = g_serial;
            uint32_t dc[4] = { it->palette, it->state, it->mirror, 0 };
            [g_enc setVertexBytes:dc length:sizeof dc atIndex:5];
            if (g_shd.on)
            {
                float lg[4];
                char_lag(f, it, lg);
                [g_enc setFragmentBytes:lg length:sizeof lg atIndex:7];
            }
            [g_enc setFragmentTexture:it->tex->view atIndex:0];
            [g_enc setFragmentSamplerState:sampler(&it->samp) atIndex:0];
            it->tex->used = g_serial;
            uint32_t n = vertex_count(it->prim, it->count);
            [g_enc drawIndexedPrimitives:it->prim == GFX_TRIANGLESTRIP ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle
                              indexCount:n
                               indexType:it->index_size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                             indexBuffer:it->ib->b
                       indexBufferOffset:(NSUInteger)it->first * it->index_size];
            ++drew;
        }
        bound_forget();
        g_cmd_draws += drew;
        if (timed)
            end_pass();
    }
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws += drew;
    return drew > 0;
}

/* --- the scene renderer's sun and moon shadows (gfx_shadow_begin, gfx.h) --------------------------------------------
 * Two depth arrays a cascade a layer: the zone's, 32-bit and kept (a layer drawn again only when host/scene.c says),
 * and the characters', 16-bit, drawn every frame into the first cascades in one layered pass. Casters are drawn with
 * no culling, the depth clamped rather than clipped (what stands nearer the light than the cascade's box still
 * casts) and a slope bias; the receivers' side (SHADOW_MSL) adds a normal offset. */
static id<MTLRenderPipelineState> shadow_pipeline(id<MTLLibrary> lib, NSString* vs, NSString* fs, MTLVertexDescriptor* vd,
    MTLPixelFormat depth, int layered)
{
    id<MTLRenderPipelineState> p = nil;
    @autoreleasepool
    {
        id<MTLFunction> vf = [lib newFunctionWithName:vs], ff = fs ? [lib newFunctionWithName:fs] : nil;
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.vertexDescriptor = vd;
        pd.depthAttachmentPixelFormat = depth;
        if (layered)
            pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        NSError* err = nil;
        if (vf && (ff || !fs))
            p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p)
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED),
                fprintf(stderr, "[recomp] gfx: scene shadow pipeline %s failed: %s\n", [vs UTF8String],
                    err ? [[err localizedDescription] UTF8String] : "?");
        [vf release];
        [ff release];
        [pd release];
    }
    return p;
}

static int shadow_init(uint32_t size)
{
    if (!world_init() || !chars_init())
        return 0;
    if (!g_shd.tried)
    {
        g_shd.tried = 1;
        g_shd.zone[0] = shadow_pipeline(g_world.lib, @"zshadow_vs", nil, g_world.vd, MTLPixelFormatDepth32Float, 0);
        g_shd.zone[1] = shadow_pipeline(g_world.lib, @"zshadow_vs", @"zshadow_fs", g_world.vd, MTLPixelFormatDepth32Float, 0);
        g_shd.chars = shadow_pipeline(g_chars.lib, @"cshadow_vs", @"cshadow_fs", g_chars.vd, MTLPixelFormatDepth16Unorm, 1);
        MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
        d.depthCompareFunction = MTLCompareFunctionLessEqual;
        d.depthWriteEnabled = YES;
        g_shd.ds = [g_dev newDepthStencilStateWithDescriptor:d];
        [d release];
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.compareFunction = MTLCompareFunctionLessEqual;
        g_shd.cmp = [g_dev newSamplerStateWithDescriptor:sd];
        [sd release];
    }
    if (!g_shd.zone[0] || !g_shd.zone[1] || !g_shd.chars || !g_shd.cmp)
        return 0;
    uint32_t fsz = (uint32_t)g_fxs.scene_shadow_fit_size;
    fsz = fsz < 256 ? 256 : fsz > 4096 ? 4096 : fsz;
    if (g_shd.size != size || g_shd.fsize != fsz || !g_shd.zmap || !g_shd.cmap || !g_shd.cmt)
    {
        int zone = g_shd.size != size || !g_shd.zmap; /* the zone's layers are kept: only a new size draws them anew */
        if (zone)
            [g_shd.zmap release], g_shd.zmap = nil;
        [g_shd.cmap release], [g_shd.cmt release];
        g_shd.cmap = g_shd.cmt = nil;
        for (int k = zone ? 0 : 1; k < 3; ++k) /* the zone's; the characters' in the cascades, half size; the fitted one */
        {
            MTLTextureDescriptor* td = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:k ? MTLPixelFormatDepth16Unorm : MTLPixelFormatDepth32Float
                                             width:k == 1 ? size / 2 : k == 2 ? fsz : size
                                            height:k == 1 ? size / 2 : k == 2 ? fsz : size mipmapped:NO];
            td.textureType = MTLTextureType2DArray;
            td.arrayLength = k == 2 ? 1 : GFX_SHADOW_CASCADES;
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            *(k == 2 ? &g_shd.cmt : k ? &g_shd.cmap : &g_shd.zmap) = [g_dev newTextureWithDescriptor:td];
        }
        g_shd.size = size, g_shd.fsize = fsz;
        g_shd.cframe = 0, g_shd.lag.serial = 0, g_shd.lserial = 0;
    }
    return g_shd.zmap && g_shd.cmap && g_shd.cmt;
}

/* one cascade of the zone's layer drawn again */
static uint32_t shadow_zone(id<MTLCommandBuffer> cb, const GfxShadowFrame* f)
{
    int c = f->redraw;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.texture = g_shd.zmap;
    rp.depthAttachment.slice = (NSUInteger)c;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    ts_mark(rp, GFX_PASS_SHADOW_ZONE);
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setViewport:(MTLViewport){ 0, 0, (double)f->size, (double)f->size, 0, 1 }];
    [e setDepthStencilState:g_shd.ds];
    [e setCullMode:MTLCullModeNone];
    [e setDepthClipMode:MTLDepthClipModeClamp];
    [e setDepthBias:0 slopeScale:2.0f clamp:0.0f];
    uint32_t drawn = 0;
    if (f->nitem && f->nworld)
    {
        id<MTLBuffer> wb;
        NSUInteger wo;
        memcpy(ring((size_t)f->nworld * 64, 256, &wb, &wo), f->world, (size_t)f->nworld * 64);
        [e setVertexBytes:f->c[c].lmat length:64 atIndex:1];
        [e setVertexBuffer:wb offset:wo atIndex:2];
        int cur = -1;
        for (uint32_t i = 0; i < f->nitem; ++i)
        {
            const GfxWorldItem* it = &f->item[i];
            if (!it->vb || !it->count || it->inst >= f->nworld)
                continue;
            int at = (it->flags & GFX_WORLD_ATEST) && it->tex;
            if (at != cur)
                [e setRenderPipelineState:g_shd.zone[at]], cur = at;
            if (at)
            {
                [e setFragmentTexture:it->tex->view atIndex:0];
                [e setFragmentSamplerState:sampler(&it->samp) atIndex:0];
                it->tex->used = g_serial;
            }
            [e setVertexBuffer:it->vb->b offset:0 atIndex:0];
            it->vb->used = g_serial;
            [e setVertexBytes:&it->inst length:4 atIndex:4];
            [e drawPrimitives:it->prim == GFX_TRIANGLESTRIP ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle
                  vertexStart:it->first
                  vertexCount:vertex_count(it->prim, it->count)];
            ++drawn;
        }
    }
    [e endEncoding];
    return drawn;
}

/* characters into a characters' map (tex, side texels across): the frame's taken items from from, each into the layers of mats, one an
 * instance. reach NULL: the one layer, the characters whose place is within two units of its square; else a layer each cascade, a character
 * into the one its place (its first bone) from the camera falls in and the next (the cascades blend over their last
 * tenth), none beyond scene_shadow_char_far */
static void shadow_chars_pass(id<MTLCommandBuffer> cb, const GfxCharFrame* cf, const float* cam, id<MTLTexture> tex, double side,
    int clear, const float (*mats)[16], uint32_t nmat, const float* reach, uint32_t from)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.texture = tex;
    rp.depthAttachment.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.renderTargetArrayLength = nmat;
    ts_mark(rp, GFX_PASS_SHADOW_CHARS);
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setViewport:(MTLViewport){ 0, 0, side, side, 0, 1 }];
    [e setRenderPipelineState:g_shd.chars];
    [e setDepthStencilState:g_shd.ds];
    [e setCullMode:MTLCullModeNone];
    [e setDepthClipMode:MTLDepthClipModeClamp];
    [e setDepthBias:0 slopeScale:2.0f clamp:0.0f];
    id<MTLBuffer> pb, sb;
    NSUInteger po, so;
    memcpy(ring((size_t)cf->nbones * 64, 256, &pb, &po), cf->palette, (size_t)cf->nbones * 64);
    memcpy(ring((size_t)cf->nstate * sizeof(GfxCharState), 256, &sb, &so), cf->state, (size_t)cf->nstate * sizeof(GfxCharState));
    float cu[4] = { cf->alpha_ref, 0.0f, 0.0f, 0.0f };
    [e setFragmentBytes:cu length:sizeof cu atIndex:2];
    [e setVertexBuffer:pb offset:po atIndex:3];
    [e setVertexBuffer:sb offset:so atIndex:4];
    [e setVertexBytes:mats length:64 * (NSUInteger)nmat atIndex:7];
    const float* M = mats[0];
    float lim = 1.0f + 2.0f * sqrtf(M[0] * M[0] + M[4] * M[4] + M[8] * M[8]); /* the square and two units, in the layer */
    float far2 = g_fxs.scene_shadow_char_far * g_fxs.scene_shadow_char_far;
    for (uint32_t i = from; i < g_shd.nitems; ++i)
    {
        const ShItem* it = &g_shd.items[i];
        const float* r = it->at;
        float dx = r[0] - cam[0], dy = r[1] - cam[1], dz = r[2] - cam[2], d2 = dx * dx + dy * dy + dz * dz;
        uint32_t base = 0, count = 1;
        if (!reach)
        {
            if (fabsf(r[0] * M[0] + r[1] * M[4] + r[2] * M[8] + M[12]) > lim || fabsf(r[0] * M[1] + r[1] * M[5] + r[2] * M[9] + M[13]) > lim)
                continue;
        }
        else
        {
            if (d2 > far2)
                continue;
            while (base < nmat && d2 > (reach[base] + 3.0f) * (reach[base] + 3.0f))
                ++base;
            if (base >= nmat)
                continue;
            count = base + 1 < nmat ? 2 : 1;
        }
        g_shd_drawn[!reach] += count;
        [e setVertexBuffer:it->vb offset:0 atIndex:0];
        [e setVertexBuffer:it->uv offset:0 atIndex:1];
        uint32_t dc[4] = { it->palette, it->state, it->mirror, 0 };
        [e setVertexBytes:dc length:sizeof dc atIndex:5];
        [e setFragmentTexture:it->tex atIndex:0];
        [e setFragmentSamplerState:it->samp atIndex:0];
        [e drawIndexedPrimitives:it->prim == GFX_TRIANGLESTRIP ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle
                      indexCount:vertex_count(it->prim, it->count)
                       indexType:it->index_size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                     indexBuffer:it->ib
               indexBufferOffset:(NSUInteger)it->first * it->index_size
                   instanceCount:count
                      baseVertex:0
                    baseInstance:base];
    }
    [e endEncoding];
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

/* The characters' layers drawn at the frame's end, after all its draws (into the frame's last command buffer), from all
 * of the frame's characters: the first cascades' (half size) and a fitted one round those near the camera. The next
 * frame's zone reads them (its draws come before any character is known), and so do its characters beyond the near
 * ones, a frame late. */
/* the characters into the nearest lamps' cubes (scene_lamp_shadows of them, those with a character within reach): each
 * face a perspective of 90 degrees from the lamp out to its range, six layers a lamp, a character drawn into the faces
 * of the lamps it stands near (from shadow_frame_end, with the frame's taken characters) */
static void lamp_shadows_draw(id<MTLCommandBuffer> cb, const GfxCharFrame* cf)
{
    g_lamps.nsh = 0;
    int want = (int)g_fxs.scene_lamp_shadows;
    want = want < 0 ? 0 : want > 4 ? 4 : want;
    if (g_fxs.scene_lamps <= 0.0f || !want || g_lamps.set != g_serial || !g_lamps.n || !g_shd.nitems || !g_shd.chars)
        return;
    uint32_t size = (uint32_t)g_fxs.scene_lamp_shadow_size;
    size = size < 64 ? 64 : size > 1024 ? 1024 : size;
    if (!g_lamps.lsm || g_lamps.lsize != size)
    {
        [g_lamps.lsm release];
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth16Unorm width:size height:size
                                                                                   mipmapped:NO];
        td.textureType = MTLTextureType2DArray, td.arrayLength = 24;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, td.storageMode = MTLStorageModePrivate;
        g_lamps.lsm = [g_dev newTextureWithDescriptor:td];
        g_lamps.lsize = size;
        if (!g_lamps.lsm)
            return;
    }
    /* the nearest lamps to the camera with a character within their reach */
    float best[4] = { 1e30f, 1e30f, 1e30f, 1e30f };
    int pick[4] = { -1, -1, -1, -1 };
    for (uint32_t i = 0; i < g_lamps.n; ++i)
    {
        const GfxWorldLight* l = &g_lamps.l[i];
        float r = l->pos[3] + 1.5f;
        int near = 0;
        for (uint32_t k = 0; k < g_shd.nitems && !near; ++k)
        {
            const float* a = g_shd.items[k].at;
            float dx = a[0] - l->pos[0], dy = a[1] - l->pos[1], dz = a[2] - l->pos[2];
            near = dx * dx + dy * dy + dz * dz < r * r;
        }
        if (!near)
            continue;
        float cx = l->pos[0] - g_lamps.cam[0], cy = l->pos[1] - g_lamps.cam[1], cz = l->pos[2] - g_lamps.cam[2];
        float d = cx * cx + cy * cy + cz * cz;
        for (int j = 0; j < want; ++j)
            if (d < best[j])
            {
                for (int m = want - 1; m > j; --m)
                    best[m] = best[m - 1], pick[m] = pick[m - 1];
                best[j] = d, pick[j] = (int)i;
                break;
            }
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
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.texture = g_lamps.lsm;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.renderTargetArrayLength = 24;
    ts_mark(rp, GFX_PASS_LAMPS);
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setViewport:(MTLViewport){ 0, 0, (double)size, (double)size, 0, 1 }];
    [e setRenderPipelineState:g_shd.chars];
    [e setDepthStencilState:g_shd.ds];
    [e setCullMode:MTLCullModeNone];
    [e setDepthClipMode:MTLDepthClipModeClamp];
    [e setDepthBias:2.0f slopeScale:2.0f clamp:0.0f];
    id<MTLBuffer> pb, sb;
    NSUInteger po, so;
    memcpy(ring((size_t)cf->nbones * 64, 256, &pb, &po), cf->palette, (size_t)cf->nbones * 64);
    memcpy(ring((size_t)cf->nstate * sizeof(GfxCharState), 256, &sb, &so), cf->state, (size_t)cf->nstate * sizeof(GfxCharState));
    float cu[4] = { cf->alpha_ref, 0.0f, 0.0f, 0.0f };
    [e setFragmentBytes:cu length:sizeof cu atIndex:2];
    [e setVertexBuffer:pb offset:po atIndex:3];
    [e setVertexBuffer:sb offset:so atIndex:4];
    [e setVertexBytes:g_lamps.shm length:64 * 6 * ns atIndex:7];
    for (uint32_t i = 0; i < g_shd.nitems; ++i)
    {
        const ShItem* it = &g_shd.items[i];
        for (uint32_t k = 0; k < ns; ++k)
        {
            float r = g_lamps.shpos[k][3] + 1.5f;
            float dx = it->at[0] - g_lamps.shpos[k][0], dy = it->at[1] - g_lamps.shpos[k][1], dz = it->at[2] - g_lamps.shpos[k][2];
            if (dx * dx + dy * dy + dz * dz > r * r)
                continue;
            [e setVertexBuffer:it->vb offset:0 atIndex:0];
            [e setVertexBuffer:it->uv offset:0 atIndex:1];
            uint32_t dc[4] = { it->palette, it->state, it->mirror, 0 };
            [e setVertexBytes:dc length:sizeof dc atIndex:5];
            [e setFragmentTexture:it->tex atIndex:0];
            [e setFragmentSamplerState:it->samp atIndex:0];
            [e drawIndexedPrimitives:it->prim == GFX_TRIANGLESTRIP ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle
                          indexCount:vertex_count(it->prim, it->count)
                           indexType:it->index_size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                         indexBuffer:it->ib
                   indexBufferOffset:(NSUInteger)it->first * it->index_size
                       instanceCount:6
                          baseVertex:0
                        baseInstance:k * 6];
        }
    }
    [e endEncoding];
    g_lamps.nsh = ns, g_lamps.sh_serial = g_serial;
}

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
    flush_pass();
    id<MTLCommandBuffer> cb = cmd();
    shadow_chars_pass(cb, &g_shd.lc, f->cam, g_shd.cmap, (double)(f->size / 2), 1, (const float (*)[16])g_shd.lag.cm, n, reach, 0);
    if (g_shd.lag.ttx > 0.0f)
        shadow_chars_pass(cb, &g_shd.lc, f->cam, g_shd.cmt, (double)g_shd.fsize, 1, (const float (*)[16])g_shd.lag.tm, 1, NULL, 0);
    lamp_shadows_draw(cb, &g_shd.lc);
    lamp_zone_cubes(cb);
    g_shd.lag.serial = g_serial;
    g_shd.lserial = 0;
    shadow_items_clear(); /* (the command buffer holds what it draws) */
}

void gfx_shadow_begin(const GfxShadowFrame* f)
{
    g_shd.on = 0;
    if (!g_dev || !f || f->dir[3] <= 0.0f || !f->size || !f->ncascades)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    @autoreleasepool
    {
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
            shadow_items_clear();
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
        {
            /* a command buffer of its own, committed now: the GPU runs it before the frame's (not yet committed) rest,
             * and the game's render pass is not broken off and taken up again round it */
            frame_begin();
            id<MTLCommandBuffer> cb = [g_queue commandBuffer];
            shadow_zone(cb, f);
            if (gfx_pass_timing_on())
                [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                    uint64_t ns = (uint64_t)((done.GPUEndTime - done.GPUStartTime) * 1e9);
                    atomic_fetch_add(&g_gpu_ns, ns);
                    gfx_pass_gpu(ns);
                }];
            [cb commit];
        }
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
    }
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
        g_pass = GFX_PASS_FXAA;
        fx_pass(ct, MTLLoadActionLoad, g_fx.aa_pipe, (MTLViewport){ vx, vy, vw, vh, 0, 1 }, &u, &g_fx.aa_src, 1, NULL);
        color->scene = 0; /* its mips are behind (scene_mips) */
        color->used = g_serial;
    }
}

/* --- post effects: bloom and depth of field (post_bloom, post_dof) ----------------------------------------------
 * On the finished scene, before the interface and the nameplates go over it, with or without the effects (FFXI_FX) and
 * the scene renderer. The sun shafts first (post_shafts), then the depth of field: circles of confusion from the scene's depth at half size, a disc gather,
 * blended back by each pixel's own circle. Then the bloom: its source the glow (the scene's additive draws, drawn
 * again: glow_new) and the sky over a soft threshold, at half size through four bilinear taps Karis-weighted, five
 * levels more by a 13-tap downsample, a tent upsample back up adding each level onto the one above, and the
 * sum screen-blended onto the scene. All in the frame's command buffer; each pass timed. */
static const char POST_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct PV { float4 pos [[position]]; float2 uv; };\n"
    "vertex PV post_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  PV o; o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); o.uv = p; return o;\n"
    "}\n"
    "struct PU {\n"
    "  float4 vp;    // the scene's viewport in its target, pixels\n"
    "  float4 src;   // the texture read: width, height, 1 / width, 1 / height\n"
    "  float4 bloom; // threshold, knee, glow weight, strength\n"
    "  float4 dof;   // focus (view z), sharp range behind it, ramp, largest radius (half-size pixels)\n"
    "  float4 dof2;  // near strength, hand, levels, radius scale (up)\n"
    "  float4 zp;    // projection [10], [14], viewport min z, max z\n"
    "  float4 aux;   // x: a texel step down the target per step across (the target's square drawn onto a wide screen);\n"
    "                // y: the sky's (sun, moon) emissive weight\n"
    "};\n"
    "static float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }\n"
    "// the bloom's source at a point of the scene: the glow, and where nothing was drawn with depth (the sky, the sun)\n"
    "// what is over the threshold (a soft knee) - lit ground and walls, however bright, do not glow\n"
    "static float3 bright(constant PU& u, texture2d<float> sc, texture2d<float> gl, depth2d<float> dt, sampler s, float2 uv) {\n"
    "  float3 c = sc.sample(s, uv).rgb;\n"
    "  uint2 q = uint2(clamp(uv * u.src.xy, float2(0.0), u.src.xy - 1.0));\n"
    "  float sky = dt.read(q) >= u.zp.w - 1e-6 ? 1.0 : 0.0;\n"
    "  float br = max(c.r, max(c.g, c.b)), k = max(u.bloom.y, 1e-4);\n"
    "  float soft = clamp(br - u.bloom.x + k, 0.0, 2.0 * k);\n"
    "  soft = soft * soft / (4.0 * k);\n"
    "  float w = max(soft, br - u.bloom.x) / max(br, 1e-4);\n"
    "  // the glow past a little (faint, wide additive layers - a sea's shimmer - stay as they are)\n"
    "  float3 g = gl.sample(s, uv).rgb;\n"
    "  float gb = max(g.r, max(g.g, g.b)), gs = clamp(gb - 0.1, 0.0, 0.2);\n"
    "  gs = gs * gs / 0.4;\n"
    "  float gw = max(gs, gb - 0.2) / max(gb, 1e-4);\n"
    "  float3 r = c * (w * sky * u.aux.y) + g * (gw * u.bloom.z); // each by its emissive weight: past 1, the chain is float\n"
    "  return select(float3(0.0), clamp(r, 0.0, 64.0), isfinite(r)); // (no NaN or infinity into the chain: it would spread)\n"
    "}\n"
    "// the first level: four bilinear taps (each the 2x2 under it, a 4x4 footprint), each weighted by 1 / (1 + its luma) - a\n"
    "// Karis average, so a lone bright texel cannot flicker as the camera moves\n"
    "fragment float4 bloom_pre(PV in [[stage_in]], constant PU& u [[buffer(0)]], texture2d<float> sc [[texture(0)]],\n"
    "                          texture2d<float> gl [[texture(1)]], depth2d<float> dt [[texture(2)]],\n"
    "                          sampler s [[sampler(0)]]) {\n"
    "  float2 uv = (u.vp.xy + in.uv * u.vp.zw) * u.src.zw, t = u.src.zw * float2(1.0, u.aux.x);\n"
    "  float3 a = bright(u, sc, gl, dt, s, uv + t * float2(-1, -1)), b = bright(u, sc, gl, dt, s, uv + t * float2(1, -1));\n"
    "  float3 c = bright(u, sc, gl, dt, s, uv + t * float2(-1, 1)), d = bright(u, sc, gl, dt, s, uv + t * float2(1, 1));\n"
    "  float wa = 1.0 / (1.0 + luma(a)), wb = 1.0 / (1.0 + luma(b)), wc = 1.0 / (1.0 + luma(c)), wd = 1.0 / (1.0 + luma(d));\n"
    "  return float4((a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd), 1.0);\n"
    "}\n"
    "// the 13 taps a texel apart (Jimenez, \"Next Generation Post Processing in Call of Duty: AW\"): a centre box and four\n"
    "// corner boxes, weighted half and an eighth each\n"
    "fragment float4 bloom_down(PV in [[stage_in]], constant PU& u [[buffer(0)]], texture2d<float> t0 [[texture(0)]],\n"
    "                           sampler s [[sampler(0)]]) {\n"
    "  float2 uv = in.uv, t = u.src.zw * float2(1.0, u.aux.x);\n"
    "  float3 a = t0.sample(s, uv + t * float2(-2, -2)).rgb, b = t0.sample(s, uv + t * float2(0, -2)).rgb;\n"
    "  float3 c = t0.sample(s, uv + t * float2(2, -2)).rgb, d = t0.sample(s, uv + t * float2(-1, -1)).rgb;\n"
    "  float3 e = t0.sample(s, uv + t * float2(1, -1)).rgb, f = t0.sample(s, uv + t * float2(-2, 0)).rgb;\n"
    "  float3 g = t0.sample(s, uv).rgb, h = t0.sample(s, uv + t * float2(2, 0)).rgb;\n"
    "  float3 i = t0.sample(s, uv + t * float2(-1, 1)).rgb, j = t0.sample(s, uv + t * float2(1, 1)).rgb;\n"
    "  float3 k = t0.sample(s, uv + t * float2(-2, 2)).rgb, l = t0.sample(s, uv + t * float2(0, 2)).rgb;\n"
    "  float3 m = t0.sample(s, uv + t * float2(2, 2)).rgb;\n"
    "  float3 o = (d + e + i + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + f + h + l) * 0.0625 + g * 0.125;\n"
    "  return float4(o, 1.0);\n"
    "}\n"
    "// a 3x3 tent of the level below, added onto this one (blended): each level the sum of those under it\n"
    "fragment float4 bloom_up(PV in [[stage_in]], constant PU& u [[buffer(0)]], texture2d<float> t0 [[texture(0)]],\n"
    "                         sampler s [[sampler(0)]]) {\n"
    "  float2 uv = in.uv, t = u.src.zw * float2(1.0, u.aux.x) * u.dof2.w;\n"
    "  float3 o = t0.sample(s, uv).rgb * 4.0;\n"
    "  o += (t0.sample(s, uv + t * float2(-1, 0)).rgb + t0.sample(s, uv + t * float2(1, 0)).rgb +\n"
    "        t0.sample(s, uv + t * float2(0, -1)).rgb + t0.sample(s, uv + t * float2(0, 1)).rgb) * 2.0;\n"
    "  o += t0.sample(s, uv + t * float2(-1, -1)).rgb + t0.sample(s, uv + t * float2(1, -1)).rgb +\n"
    "       t0.sample(s, uv + t * float2(-1, 1)).rgb + t0.sample(s, uv + t * float2(1, 1)).rgb;\n"
    "  return float4(o * (1.0 / 16.0), 1.0);\n"
    "}\n"
    "// onto the scene, screen-blended by the pipeline (src x (1 - dst) + dst): it brightens toward white, never past it; the\n"
    "// sum's highlights eased toward 1 (1 - e^-b), so a strong emissive glows wide rather than clipping to a white disc\n"
    "fragment float4 bloom_comp(PV in [[stage_in]], constant PU& u [[buffer(0)]], texture2d<float> t0 [[texture(0)]],\n"
    "                           sampler s [[sampler(0)]]) {\n"
    "  float3 b = t0.sample(s, in.uv).rgb * (u.bloom.w * 1.5 / u.dof2.z);\n"
    "  if (u.aux.w > 0.0) return float4(max(b, 0.0), 1.0); // an HDR scene: added as it is (the tonemap rolls it off)\n"
    "  return float4(1.0 - exp(-max(b, 0.0)), 1.0);\n"
    "}\n"
    "// --- depth of field\n"
    "static float view_z(constant PU& u, float d) {\n"
    "  d = (d - u.zp.z) / max(u.zp.w - u.zp.z, 1e-6);\n"
    "  float z = u.zp.y / (d * u.dof2.y - u.zp.x);\n"
    "  return d >= 0.999999 || !(z * u.dof2.y > 0.0) ? 1e6 : abs(z); // the sky: as far as can be\n"
    "}\n"
    "// the circle of confusion, 0 sharp to 1 the largest: behind the focus past its range, and (dof_near) well before it;\n"
    "// always sharp within 4 units of the focus (the player, whatever the settings)\n"
    "static float coc(constant PU& u, float z) {\n"
    "  float far = saturate((z - u.dof.x - max(u.dof.y, 4.0)) / max(u.dof.z, 1e-3));\n"
    "  float nr = min(u.dof.x * 0.5, u.dof.x - 4.0);\n"
    "  float near = nr > 0.0 ? u.dof2.x * saturate((nr - z) / max(nr, 1e-3)) : 0.0;\n"
    "  return max(far, near);\n"
    "}\n"
    "// half size: each texel the 2x2 under it, their colours weighted toward the blurred ones (so a sharp edge's colour\n"
    "// does not spread into the blur behind it), and the largest of their circles\n"
    "fragment float4 dof_coc(PV in [[stage_in]], constant PU& u [[buffer(0)]], texture2d<float> sc [[texture(0)]],\n"
    "                        depth2d<float> dt [[texture(1)]]) {\n"
    "  float2 p = floor(u.vp.xy + in.pos.xy * 2.0 - 1.0);\n"
    "  float3 c = 0.0; float w = 0.0, m = 0.0;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    uint2 q = uint2(clamp(p + float2(k & 1, k >> 1), u.vp.xy, u.vp.xy + u.vp.zw - 1.0));\n"
    "    float r = coc(u, view_z(u, dt.read(q)));\n"
    "    float wk = r + 0.02;\n"
    "    c += sc.read(q).rgb * wk, w += wk, m = max(m, r);\n"
    "  }\n"
    "  return float4(c / w, m);\n"
    "}\n"
    "// the gather: a disc of taps as wide as this texel's circle; a tap counts where its own circle (no wider than this\n"
    "// one's) reaches here - a sharp foreground spreads nothing into the blur behind it, the blur nothing onto it; bright\n"
    "// taps count a little more, for bokeh\n"
    "constant int NT = 16;\n"
    "fragment float4 dof_gather(PV in [[stage_in]], constant PU& u [[buffer(0)]], texture2d<float> t0 [[texture(0)]],\n"
    "                           sampler s [[sampler(0)]]) {\n"
    "  float4 c0 = t0.sample(s, in.uv);\n"
    "  float R = c0.a * u.dof.w;\n"
    "  if (R < 0.5) return c0;\n"
    "  int n = R < 2.5 ? 8 : NT; // (the composite's tent fills between the taps)\n"
    "  float3 acc = 0.0; float wsum = 0.0;\n"
    "  for (int i = 0; i < n; ++i) {\n"
    "    float r = sqrt((float(i) + 0.5) / float(n)) * R, a = float(i) * 2.39996323;\n"
    "    float2 o = float2(cos(a), sin(a)) * r;\n"
    "    float4 t = t0.sample(s, in.uv + o * float2(1.0, u.aux.x) * u.src.zw);\n"
    "    float w = saturate(min(t.a, c0.a) * u.dof.w - r + 1.0);\n"
    "    w *= 1.0 + 3.0 * saturate(luma(t.rgb) - 0.75);\n"
    "    acc += t.rgb * w, wsum += w;\n"
    "  }\n"
    "  return float4(wsum > 0.0 ? acc / wsum : c0.rgb, c0.a);\n"
    "}\n"
    "// onto the scene by its own circle at full size (alpha-blended by the pipeline): sharp where it is in focus\n"
    "fragment float4 dof_comp(PV in [[stage_in]], constant PU& u [[buffer(0)]], texture2d<float> t0 [[texture(0)]],\n"
    "                         depth2d<float> dt [[texture(1)]], sampler s [[sampler(0)]]) {\n"
    "  float2 px = in.pos.xy;\n"
    "  float r = coc(u, view_z(u, dt.read(uint2(px)))) * u.dof.w;\n"
    "  float a = smoothstep(0.25, 1.5, r);\n"
    "  if (a <= 0.0) return float4(0.0);\n"
    "  float2 uv = (px - u.vp.xy) / u.vp.zw, t = u.src.zw * float2(0.75, 0.75 * u.aux.x);\n"
    "  float3 b = (t0.sample(s, uv + float2(-t.x, -t.y)).rgb + t0.sample(s, uv + float2(t.x, -t.y)).rgb +\n"
    "              t0.sample(s, uv + float2(-t.x, t.y)).rgb + t0.sample(s, uv + float2(t.x, t.y)).rgb) * 0.25;\n"
    "  return float4(b, a);\n"
    "}\n"
    "// --- the modern fog (fog_modern): light scattered by the air between the eye and each point, and the point's light dimmed\n"
    "// by it - not a fade to grey. Thick as the game's fog says (its end the far limit, where little is left to see) and\n"
    "// thinning with height; its colour the game's fog colour, bluer away from the sun and warm toward it (the air's forward\n"
    "// scattering); the lamps' light scattered round them (halos in mist). Over the scene by its own alpha: the colour times\n"
    "// what gets through, plus what is scattered in\n"
    "struct FU {\n"
    "  float4x4 ivp; // the scene's clip space to the world round the camera (the view's turn alone)\n"
    "  float4 cam;   // the camera, world; w: how far the fog goes (beyond: the sky)\n"
    "  float4 sun;   // toward the sun (or moon), world; w: the forward lobe's g\n"
    "  float4 sunc;  // its scattered colour (0 with the shafts on: theirs)\n"
    "  float4 airc;  // the air's own colour (the game's fog colour, cooled)\n"
    "  float4 p;     // density at the camera's height, its falloff with height, the lamps' halo strength, the 8-bit dither\n"
    "  float4 vp;    // the scene's viewport in its target\n"
    "  float4 lt;    // the lamps' tiles: origin x, y, tiles across, down (0: no lamps)\n"
    "  float4 dbg;   // x: fog_debug; y: how far above the game's light its lantern hangs (the halo's centre)\n"
    "};\n"
    "struct FL { float4 pos, col, att, amb; };\n"
    "// optical depth along t in [0, d] of dir from the camera: density p.x at the camera's height, falling as exp(-k * rise)\n"
    "static float fog_tau(constant FU& u, float3 dir, float d) {\n"
    "  float rise = -dir.y * d * u.p.y;\n" /* (y down: up is -y) */
    "  float f = abs(rise) < 1e-3 ? 1.0 - 0.5 * rise : (1.0 - exp(-rise)) / rise;\n"
    "  return u.p.x * d * f;\n"
    "}\n"
    "// a point light's light scattered toward the eye along the ray inside its reach: the integral of 1 / r^2 in closed form\n"
    "static float fog_lamp(float3 Lp, float range, float3 dir, float d) {\n"
    "  float tc = dot(Lp, dir), h2 = max(dot(Lp, Lp) - tc * tc, 0.0), r2 = range * range;\n"
    "  if (h2 >= r2) return 0.0;\n"
    "  float w = sqrt(r2 - h2), a = max(tc - w, 0.0), b = min(tc + w, d);\n"
    "  if (b <= a) return 0.0;\n"
    "  float h = sqrt(max(h2, 0.09));\n"
    "  return (atan((b - tc) / h) - atan((a - tc) / h)) / h;\n"
    "}\n"
    "fragment float4 fog_comp(PV in [[stage_in]], constant FU& u [[buffer(0)]], depth2d<float> dt [[texture(0)]],\n"
    "                         const device FL* LS [[buffer(1)]], const device ushort* TL [[buffer(2)]]) {\n"
    "  float2 fp = in.pos.xy;\n"
    "  float d0 = dt.read(uint2(fp));\n"
    "  if (d0 >= 0.99999) return float4(0.0);\n" /* the sky: the game's own, with its own haze */
    "  float2 n = float2((fp.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (fp.y - u.vp.y) / u.vp.w * 2.0);\n"
    "  float4 w = u.ivp * float4(n, d0, 1.0);\n"
    "  float3 v = w.xyz / w.w;\n"
    "  float l = length(v), d = min(l, u.cam.w);\n"
    "  float3 dir = v / max(l, 1e-4);\n"
    "  float T = exp(-fog_tau(u, dir, d));\n"
    "  float g = u.sun.w, mu = dot(dir, u.sun.xyz);\n"
    "  float mie = 0.25 * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5);\n"
    "  float ray = 0.75 * (1.0 + mu * mu);\n" /* (Rayleigh: a little more toward and away from the sun than across) */
    "  float3 I = (u.airc.rgb * ray + u.sunc.rgb * mie) * (1.0 - T);\n"
    "  if (u.lt.z > 0.0 && u.p.z > 0.0) {\n" /* the lamps in this tile: their light scattered in the mist round them */
    "    int2 tq = int2((fp - u.lt.xy) / 64.0);\n"
    "    if (all(tq >= 0) && tq.x < int(u.lt.z) && tq.y < int(u.lt.w)) {\n"
    "      const device ushort* L = TL + (uint(tq.y) * uint(u.lt.z) + uint(tq.x)) * 32u;\n"
    "      float sig = u.p.z;\n"
    "      for (uint k = 0; k < uint(L[0]); ++k) {\n"
    "        FL lm = LS[L[1 + k]];\n"
    "        float3 Lp = lm.pos.xyz - u.cam.xyz - float3(0.0, u.dbg.y, 0.0);\n"
    "        I += lm.col.rgb * (sig * fog_lamp(Lp, lm.pos.w, dir, d) * exp(-fog_tau(u, dir, max(dot(Lp, dir), 0.0))));\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  if (u.dbg.x > 0.5) return float4(u.dbg.x > 1.5 ? I : float3(1.0 - T), 1.0);\n" /* fog_debug: 1 how much it takes, 2 what it adds */
    "  float a = 1.0 - T;\n"
    "  if (a < 1e-4 && all(I < 1e-4)) return float4(0.0);\n"
    "  float r = fract(sin(dot(fp, float2(12.9898, 78.233))) * 43758.5453) - 0.5;\n"
    "  return float4(max(I / max(a, 1e-4) + r * u.p.w / max(a, 1e-4), 0.0), a);\n" /* over: I / a * a + scene * T; dithered at 8 bits */
    "}\n"
    "// --- sun shafts: the sun's (or moon's) light scattered toward the eye along each view ray, where the scene renderer's\n"
    "// shadow cascades say it reaches; at half or quarter size, over frames, then onto the scene by depth\n"
    "constant float SU_BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n"
    "struct SU {\n"
    "  float4x4 ivp;  // the scene's clip space to the world round the camera (the view's turn alone: no translation)\n"
    "  float4x4 pvp;  // the world round last frame's camera to last frame's clip space\n"
    "  float4x4 m[4]; // the zone's shadow cascades (world to the light's clip space)\n"
    "  float4 cas[4]; // their reach from the camera, texel, depth range, valid\n"
    "  float4 cam;    // the camera, world; w: how far the rays go\n"
    "  float4 sun;    // toward the light, world; w: 1 when it shines\n"
    "  float4 sunc;   // its colour times the shafts' strength; w: the phase's g\n"
    "  float4 p;      // density at the camera's height, its falloff with height, steps, this frame's jitter\n"
    "  float4 vp;     // the scene's viewport in its target\n"
    "  float4 lo;     // the small textures' size, 1 / size\n"
    "  float4 hist;   // x: last frame's history is there and lines up; y: its weight\n"
    "  float4 pcam;   // last frame's camera, world\n"
    "  // the fog volume (fog_modern with fog_volume): the air's own colour;\n"
    "  // x the lamps' halo density, y how far above the\n"
    "  // game's light its lantern hangs, z the ground fog's strength, w its noise's scale; the noise's drift (xyz) and w 1\n"
    "  // in the volume; the lamps' tiles (origin x, y, across, down; 0 none)\n"
    "  float4 airc;\n"
    "  float4 fv;\n"
    "  float4 wind;\n"
    "  float4 lt;\n"
    "};\n"
    "struct SL { float4 pos, col, att, amb; };\n"
    "// low, smooth noise for the ground fog: value noise, two octaves, 0..1\n"
    "static float su_hash(float3 p) { return fract(sin(dot(p, float3(127.1, 311.7, 74.7))) * 43758.5453); }\n"
    "static float su_vnoise(float3 x) {\n"
    "  float3 i = floor(x), f = x - i; f = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(mix(su_hash(i), su_hash(i + float3(1, 0, 0)), f.x), mix(su_hash(i + float3(0, 1, 0)), su_hash(i + float3(1, 1, 0)), f.x), f.y),\n"
    "             mix(mix(su_hash(i + float3(0, 0, 1)), su_hash(i + float3(1, 0, 1)), f.x), mix(su_hash(i + float3(0, 1, 1)), su_hash(i + float3(1, 1, 1)), f.x), f.y), f.z);\n"
    "}\n"
    "static float su_ground(constant SU& u, float3 x) {\n"
    "  float3 q = (x + u.wind.xyz) * u.fv.w;\n"
    "  return 0.65 * su_vnoise(q) + 0.35 * su_vnoise(q * 2.7 + 13.1);\n"
    "}\n"
    "// optical depth from the camera along dir for d, the smooth density alone (closed form; the volume's comp)\n"
    "static float su_tau(constant SU& u, float3 dir, float d) {\n"
    "  float rise = -dir.y * d * u.p.y;\n"
    "  float f = abs(rise) < 1e-3 ? 1.0 - 0.5 * rise : (1.0 - exp(-rise)) / rise;\n"
    "  return u.p.x * d * f;\n"
    "}\n"
    "static float su_lamp(float3 Lp, float range, float3 dir, float d) {\n"
    "  float tc = dot(Lp, dir), h2 = max(dot(Lp, Lp) - tc * tc, 0.0), r2 = range * range;\n"
    "  if (h2 >= r2) return 0.0;\n"
    "  float w = sqrt(r2 - h2), a = max(tc - w, 0.0), b = min(tc + w, d);\n"
    "  if (b <= a) return 0.0;\n"
    "  float h = sqrt(max(h2, 0.09));\n"
    "  return (atan((b - tc) / h) - atan((a - tc) / h)) / h;\n"
    "}\n"
    "static float3 su_dir(constant SU& u, float2 fp, float d, thread float& dist) {\n"
    "  float2 n = float2((fp.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (fp.y - u.vp.y) / u.vp.w * 2.0);\n"
    "  bool sky = d >= 0.99999;\n"
    "  float4 w = u.ivp * float4(n, sky ? 0.5 : d, 1.0);\n"
    "  float3 v = w.xyz / w.w;\n"
    "  float l = length(v);\n"
    "  dist = sky ? u.cam.w : min(l, u.cam.w);\n"
    "  return v / max(l, 1e-4);\n"
    "}\n"
    "// the ray to the surface (or as far as the rays go): steps spread as the square of their order, the first by this pixel's jitter (a 4x4 Bayer\n"
    "// order turned a golden ratio each frame); the light in-scattered, each step lit where the cascades see the sun\n"
    "struct SMO { float4 c [[color(0)]]; float t [[color(1)]]; };\n" /* the in-scatter and distance; what gets through */
    "fragment SMO shaft_march(PV in [[stage_in]], constant SU& u [[buffer(0)]], depth2d<float> dt [[texture(0)]],\n"
    "                            depth2d_array<float> sm [[texture(1)]], const device SL* LS [[buffer(1)]],\n"
    "                            const device ushort* TL [[buffer(2)]]) {\n"
    "  constexpr sampler cmp(coord::normalized, filter::linear, address::clamp_to_edge, compare_func::less_equal);\n"
    "  float2 lp = floor(in.pos.xy);\n"
    "  float2 fp = u.vp.xy + (lp + 0.5) * (u.vp.zw * u.lo.zw);\n"
    "  uint2 q = uint2(clamp(fp, u.vp.xy, u.vp.xy + u.vp.zw - 1.0));\n"
    "  float dist;\n"
    "  float d0 = dt.read(q);\n"
    "  float3 dir = su_dir(u, fp, d0, dist);\n"
    "  bool vol = u.wind.w > 0.5, sky = d0 >= 0.99999;\n"
    "  // this texel's jitter: interleaved gradient noise (near blue), turned a golden ratio each frame\n"
    "  float j = fract(fract(52.9829189 * fract(dot(lp, float2(0.06711056, 0.00583715)))) + u.p.w);\n"
    "  int n = int(u.p.z);\n"
    "  float g = u.sunc.w, mu = dot(dir, u.sun.xyz);\n"
    "  float ph = 0.25 * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5);\n"
    "  float ray = 0.75 * (1.0 + mu * mu);\n"
    "  float T = 1.0, L = 0.0, ls = 0.0, lin = 0.0, llast = 1.0; float3 A = 0.0;\n"
    "  for (int i = 0; i < n; ++i) {\n"
    "    // steps closer together near the camera (t grows as the square): beams through trunks and arches are near\n"
    "    float f = (float(i) + j) / float(n), t = dist * f * f, seg = 2.0 * dist * f / float(n);\n"
    "    float3 x = u.cam.xyz + dir * t;\n"
    "    float hf = exp(clamp(u.p.y * (x.y - u.cam.y), -10.0, 3.0)); // y down: above the camera thinner\n"
    "    float rho = u.p.x * hf;\n"

    "    float lit = 1.0, inr = 0.0;\n"
    "    for (int c = 0; c < 4; ++c) {\n"
    "      if (u.cas[c].w == 0.0 || t > u.cas[c].x) continue;\n"
    "      float4 h = u.m[c] * float4(x, 1.0);\n"
    "      if (all(abs(h.xy) < 0.995) && h.z > 0.0 && h.z < 1.0)\n"
    "        lit = sm.sample_compare(cmp, float2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5), uint(c), h.z - 2e-4), inr = 1.0;\n"
    "      break;\n"
    "    }\n"
    "    ls += lit, lin += inr, llast = lit;\n"
    "    float a = rho * seg;\n"
    "    L += T * a * lit;\n"
    "    if (vol && !sky) A += T * a;\n" /* the air's own light: the sky's, unshadowed */
    "    T *= exp(-a);\n"
    "  }\n"
    "  SMO r; r.t = sky ? 1.0 : T;\n"
    "  if (u.hist.z < 0.0) { r.c = float4(ls / float(n), lin / float(n), 0.0, dist); return r; } // shafts_debug < 0: lit fraction, in-map fraction\n"
    "  float3 o = u.sunc.rgb * (L * ph);\n"
    "  if (vol && !sky) {\n"
    "    o += u.airc.rgb * (A * ray);\n"
    "    if (u.fv.z > 0.0) {\n" /* the ground mist: a layer some 1.5 units deep over the surface this ray ends on (the ground taken as */
    "      // level there), in drifting patches; its depth along the ray in closed form, lit as the surface is\n"
    "      float sl = max(abs(dir.y), 0.2), H = 1.5;\n"
    "      float3 P = u.cam.xyz + dir * dist;\n"
    "      // only over ground: the surface's facing from the depths beside it (up, y down: -y); characters and walls none\n"
    "      float2 fx = fp + float2(u.vp.z * u.lo.z, 0.0), fy = fp + float2(0.0, u.vp.w * u.lo.w);\n"
    "      float d1, d2;\n"
    "      float3 r1 = su_dir(u, fx, dt.read(uint2(clamp(fx, u.vp.xy, u.vp.xy + u.vp.zw - 1.0))), d1);\n"
    "      float3 r2 = su_dir(u, fy, dt.read(uint2(clamp(fy, u.vp.xy, u.vp.xy + u.vp.zw - 1.0))), d2);\n"
    "      float3 nn = cross(r1 * d1 - dir * dist, r2 * d2 - dir * dist);\n"
    "      float up = length(nn) > 1e-6 ? abs(normalize(nn).y) : 0.0;\n"
    "      float edge = saturate(1.0 - max(abs(d1 - dist), abs(d2 - dist)) / (0.05 * dist + 0.3));\n" /* (a silhouette's edge: no facing to trust) */
    "      float tm = u.fv.z * 0.12 * smoothstep(0.75, 0.95, up) * edge * smoothstep(0.3, 0.65, su_ground(u, P)) * (H / sl) * (1.0 - exp(-dist * sl / H));\n"
    "      float am = 1.0 - exp(-tm);\n"
    "      o += (u.airc.rgb * ray + u.sunc.rgb * (ph * llast)) * (T * am);\n"
    "      T *= 1.0 - am;\n"
    "    }\n"
    "    if (u.lt.z > 0.0 && u.fv.x > 0.0) {\n" /* the lamps in this pixel's tile: their light in the mist round them */
    "      int2 tq = int2((fp - u.lt.xy) / 64.0);\n"
    "      if (all(tq >= 0) && tq.x < int(u.lt.z) && tq.y < int(u.lt.w)) {\n"
    "        const device ushort* LT = TL + (uint(tq.y) * uint(u.lt.z) + uint(tq.x)) * 32u;\n"
    "        for (uint k = 0; k < uint(LT[0]); ++k) {\n"
    "          SL lm = LS[LT[1 + k]];\n"
    "          float3 Lp = lm.pos.xyz - u.cam.xyz - float3(0.0, u.fv.y, 0.0);\n"
    "          o += lm.col.rgb * (u.fv.x * su_lamp(Lp, lm.pos.w, dir, dist) * exp(-su_tau(u, dir, max(dot(Lp, dir), 0.0))));\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  r.c = float4(select(float3(0.0), o, isfinite(o)), dist);\n"
    "  r.t = sky ? 1.0 : T;\n"
    "  return r;\n"
    "}\n"
    "// over frames: this frame's march and last frame's result where this texel's point was then, clamped to this frame's\n"
    "// neighbours (no trails where the scene moved)\n"
    "fragment float4 shaft_resolve(PV in [[stage_in]], constant SU& u [[buffer(0)]], texture2d<float> cur [[texture(0)]],\n"
    "                              texture2d<float> hist [[texture(1)]], depth2d<float> dt [[texture(2)]], sampler s [[sampler(0)]]) {\n"
    "  int2 p = int2(in.pos.xy), hi = int2(u.lo.xy) - 1;\n"
    "  float4 c = cur.read(uint2(p));\n"
    "  if (u.hist.x <= 0.0) return c;\n"
    "  float3 mn = c.rgb, mx = c.rgb;\n"
    "  for (int k = 0; k < 9; ++k) {\n"
    "    if (k == 4) continue;\n"
    "    float3 v = cur.read(uint2(clamp(p + int2(k % 3 - 1, k / 3 - 1), int2(0), hi))).rgb;\n"
    "    mn = min(mn, v), mx = max(mx, v);\n"
    "  }\n"
    "  float2 fp = u.vp.xy + (float2(p) + 0.5) * (u.vp.zw * u.lo.zw);\n"
    "  float dist;\n"
    "  float3 dir = su_dir(u, fp, dt.read(uint2(clamp(fp, u.vp.xy, u.vp.xy + u.vp.zw - 1.0))), dist);\n"
    "  float4 pc = u.pvp * float4(u.cam.xyz + dir * c.a - u.pcam.xyz, 1.0);\n"
    "  if (pc.w <= 1e-4) return c;\n"
    "  float2 uv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
    "  if (any(uv < 0.0) || any(uv > 1.0)) return c;\n"
    "  float3 h = clamp(hist.sample(s, uv).rgb, mn, mx);\n"
    "  return float4(mix(c.rgb, h, u.hist.y), c.a);\n"
    "}\n"
    "// onto the scene (screen-blended): the four small texels round each pixel, weighted by how near they are and how alike\n"
    "// in distance - a wall's edge takes the wall's shafts, the sky beside it the sky's\n"
    "fragment float4 shaft_comp(PV in [[stage_in]], constant SU& u [[buffer(0)]], texture2d<float> ft [[texture(0)]],\n"
    "                           depth2d<float> dt [[texture(1)]], texture2d<float> tt [[texture(2)]]) {\n"
    "  float2 fp = in.pos.xy;\n"
    "  float dist;\n"
    "  su_dir(u, fp, dt.read(uint2(fp)), dist);\n"
    "  float2 h = (fp - u.vp.xy) * (u.lo.xy / u.vp.zw) - 0.5;\n"
    "  int2 b = int2(floor(h)), hi = int2(u.lo.xy) - 1;\n"
    "  float2 fr = h - float2(b);\n"
    "  float3 sum = 0.0; float ws = 0.0, tsum = 0.0;\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    int2 o = int2(k & 1, k >> 1);\n"
    "    uint2 q = uint2(clamp(b + o, int2(0), hi));\n"
    "    float4 t = ft.read(q);\n"
    "    float w = (o.x ? fr.x : 1.0 - fr.x) * (o.y ? fr.y : 1.0 - fr.y) + 1e-3;\n"
    "    w *= exp(-abs(t.a - dist) / (0.05 * dist + 0.5));\n"
    "    sum += t.rgb * w, ws += w;\n"
    "    if (u.wind.w > 0.5) tsum += tt.read(q).r * w;\n"
    "  }\n"
    "  float3 c = ws > 1e-6 ? sum / ws : float3(0.0);\n"
    "  if (u.hist.z != 0.0) return float4(saturate(c * abs(u.hist.z)), 1.0); // shafts_debug: the term alone, scaled (no blend)\n"
    "  if (u.wind.w > 0.5) {\n" /* the fog volume: the scene dimmed by the air, what it scatters added (ONE, 1 - src alpha) */
    "    float a = dt.read(uint2(fp)) >= 0.99999 ? 0.0 : saturate(1.0 - (ws > 1e-6 ? tsum / ws : 1.0));\n" /* (as marched: the mist's patches too) */
    "    float r = (fract(sin(dot(fp, float2(12.9898, 78.233))) * 43758.5453) - 0.5) * (u.hist.w > 0.0 ? 0.0 : 1.0 / 255.0);\n"
    "    return float4(max(c + r, 0.0), a);\n"
    "  }\n"
    "  if (u.hist.w > 0.0) return float4(max(c, 0.0), 1.0); // an HDR scene: added as it is\n"
    "  return float4(1.0 - exp(-max(c, 0.0)), 1.0);\n"
    "}\n"
    "// --- the HDR scene (scene_hdr): a target's first level copied into its float replacement, texel for texel\n"
    "fragment float4 hdr_copy(PV in [[stage_in]], texture2d<float> t0 [[texture(0)]]) {\n"
    "  return t0.read(uint2(in.pos.xy));\n"
    "}\n"
    "// the tonemap, in place (the pixel as it is, read back): 0 neutral (clamped, today's image), 1 filmic (ACES fit\n"
    "// on the linear light, exposure, normalised to the white point), back to the display's encoding\n"
    "struct TU { float4 k; }; // mode, exposure, white point, unused\n"
    "static float3 aces(float3 x) { return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14); }\n"
    "fragment float4 hdr_tone(PV in [[stage_in]], constant TU& u [[buffer(0)]], float4 c [[color(0)]]) {\n"
    "  float3 v = max(c.rgb, 0.0);\n"
    "  if (u.k.x < 0.5) return float4(min(v, 1.0), c.a);\n"
    "  if (u.k.x < 1.5) {\n"
    "    // 1: a shoulder - the image as it was below the knee (k.w), what is past it (highlights, glows, glints) rolled\n"
    "    // off toward white by its brightest channel, the hue kept; the exposure first\n"
    "    v *= u.k.y;\n"
    "    float m = max(v.r, max(v.g, v.b)), kn = u.k.w;\n"
    "    if (m > kn) {\n"
    "      float r = kn + (1.0 - kn) * (1.0 - exp(-(m - kn) / (1.0 - kn)));\n"
    "      float3 h = v * (r / m);\n"
    "      v = mix(h, float3(r), saturate((m - 1.0) / (u.k.z - 1.0)) * 0.5); // the brightest toward white\n"
    "    }\n"
    "    return float4(saturate(v), c.a);\n"
    "  }\n"
    "  // 2: ACES fit on the linear light, normalised to the white point\n"
    "  float3 l = pow(v, 2.2) * u.k.y;\n"
    "  float3 t = aces(l) / aces(float3(u.k.z));\n"
    "  return float4(pow(saturate(t), 1.0 / 2.2), c.a);\n"
    "}\n"
    "// --- the lamps' glows (scene_lamp_glow): a soft sprite at each lamp, its own colour, hidden by what stands in front of\n"
    "// it (the scene's depth, softly), faded with distance; added to the scene before the bloom takes it\n"
    "struct GLL { float4 pos, col, att, amb; };\n"
    "struct GLU { float4x4 vp; float4 p; float4 cam; float4 zp; float4 vpr; float4 pr; };\n"
    "struct GV { float4 pos [[position]]; float2 c; float3 col [[flat]]; float lz [[flat]]; };\n"
    "vertex GV lamp_glow_vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant GLU& G [[buffer(0)]],\n"
    "                       const device GLL* LS [[buffer(1)]]) {\n"
    "  GLL l = LS[iid];\n"
    "  float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);\n"
    "  float4 q = G.vp * float4(l.pos.xyz, 1.0);\n"
    "  float dist = length(l.pos.xyz - G.cam.xyz);\n"
    "  float fade = saturate(1.0 - dist / G.p.z) * step(0.3, q.w);\n"
    "  float size = G.p.x * clamp(l.pos.w / 8.0, 0.5, 2.0);\n"
    "  GV o;\n"
    "  o.pos = q + float4(c * size * G.pr.xy, 0.0, 0.0);\n"
    "  if (fade <= 0.0) o.pos = float4(2.0, 2.0, 2.0, 1.0); // (off screen)\n"
    "  o.c = c;\n"
    "  o.col = l.col.rgb * (G.p.y * fade);\n"
    "  o.lz = q.w;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 lamp_glow_fs(GV in [[stage_in]], constant GLU& G [[buffer(0)]], depth2d<float> dt [[texture(0)]]) {\n"
    "  float r2 = dot(in.c, in.c);\n"
    "  if (r2 >= 1.0) discard_fragment();\n"
    "  float d = dt.read(uint2(in.pos.xy));\n"
    "  float dn = (d - G.zp.z) / max(G.zp.w - G.zp.z, 1e-6);\n"
    "  float sz = dn >= 0.999999 ? 1e6 : abs(G.zp.y / (dn * G.pr.z - G.zp.x)); // the scene's view depth here\n"
    "  float vis = saturate((sz - in.lz) / max(G.p.x, 0.1) + 1.0);\n"
    "  float f = exp(-r2 * 5.0) - 0.0067;\n"
    "  return float4(in.col * (f * vis), 1.0);\n"
    "}\n";

typedef struct PostU
{
    float vp[4], src[4], bloom[4], dof[4], dof2[4], zp[4], aux[4];
} PostU;

/* the sun shafts' uniforms: SU in POST_MSL */
typedef struct ShaftU
{
    float ivp[16], pvp[16], m[4][16], cas[4][4], cam[4], sun[4], sunc[4], p[4], vp[4], lo[4], hist[4], pcam[4];
    float airc[4], fv[4], wind[4], lt[4];
} ShaftU;

enum { POST_LEVELS = 6 };
static struct
{
    int tried;
    id<MTLLibrary> lib;
    id<MTLRenderPipelineState> pre, down, up, coc, gather;
    id<MTLRenderPipelineState> comp, dcomp, scomp, sdbg; /* onto the scene, built for its format */
    id<MTLRenderPipelineState> comp_add, scomp_add;      /* the same added (an HDR scene: no screen blend) */
    id<MTLRenderPipelineState> svol;                     /* the shafts as the fog volume: premultiplied over */
    id<MTLRenderPipelineState> smarch, sresolve; /* the sun shafts' march and their resolve over frames */
    id<MTLTexture> sh, shist[2];     /* the shafts at half or quarter size: this frame's march, the resolved two */
    id<MTLTexture> sht;              /* what gets through, as marched (the fog volume's) */
    int shist_at;                    /* which of shist[] the last frame wrote */
    uint64_t sh_serial;              /* that frame (0: none) */
    float sh_pvp[16], sh_pcam[3];    /* its view's turn times projection, its camera */
    MTLPixelFormat comp_fmt;
    id<MTLSamplerState> lin;
    id<MTLTexture> lvl[POST_LEVELS]; /* the bloom's levels, from half the scene's size */
    id<MTLTexture> glow;             /* the glow's draws, as the scene's target */
    id<MTLTexture> h0, h1;           /* the depth of field at half size: colour and circle; gathered */
    id<MTLTexture> black;
    float focus;                     /* the focus distance (view z), eased */
    uint64_t focus_ns;
} g_post;

static id<MTLRenderPipelineState> post_pipeline(NSString* frag, MTLPixelFormat fmt, int blend)
{
    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> vf = [g_post.lib newFunctionWithName:@"post_vs"], ff = [g_post.lib newFunctionWithName:frag];
    pd.vertexFunction = vf, pd.fragmentFunction = ff;
    MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
    ca.pixelFormat = fmt;
    if (blend)
    {
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationAdd;
        /* 1: added; 2: screen (src (1 - dst) + dst); 3: over, by the source's alpha (the colour only); 4: over, premultiplied */
        ca.sourceRGBBlendFactor = blend == 1 || blend == 4 ? MTLBlendFactorOne : blend == 2 ? MTLBlendFactorOneMinusDestinationColor
                                                                                            : MTLBlendFactorSourceAlpha;
        ca.destinationRGBBlendFactor = blend >= 3 ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
        ca.sourceAlphaBlendFactor = MTLBlendFactorZero, ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
    }
    NSError* err = nil;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p)
        __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED),
            fprintf(stderr, "[recomp] gfx: post effect %s failed: %s\n", [frag UTF8String], err ? [[err localizedDescription] UTF8String] : "?");
    [vf release], [ff release], [pd release];
    return p;
}

static int post_init(MTLPixelFormat scene_fmt)
{
    if (!g_post.tried)
    {
        g_post.tried = 1;
        g_post.lib = compile(POST_MSL);
        if (!g_post.lib)
            return 0;
        g_post.pre = post_pipeline(@"bloom_pre", MTLPixelFormatRG11B10Float, 0);
        g_post.down = post_pipeline(@"bloom_down", MTLPixelFormatRG11B10Float, 0);
        g_post.up = post_pipeline(@"bloom_up", MTLPixelFormatRG11B10Float, 1);
        g_post.coc = post_pipeline(@"dof_coc", MTLPixelFormatRGBA16Float, 0);
        g_post.gather = post_pipeline(@"dof_gather", MTLPixelFormatRGBA16Float, 0);
        {
            /* two targets: the in-scatter and distance, and what gets through */
            MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
            id<MTLFunction> vf = [g_post.lib newFunctionWithName:@"post_vs"], ff = [g_post.lib newFunctionWithName:@"shaft_march"];
            pd.vertexFunction = vf, pd.fragmentFunction = ff;
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
            pd.colorAttachments[1].pixelFormat = MTLPixelFormatR16Float;
            NSError* err = nil;
            g_post.smarch = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
            if (!g_post.smarch)
                fprintf(stderr, "[recomp] gfx: post effect shaft_march failed: %s\n", err ? [[err localizedDescription] UTF8String] : "?");
            [vf release], [ff release], [pd release];
        }
        g_post.sresolve = post_pipeline(@"shaft_resolve", MTLPixelFormatRGBA16Float, 0);
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        g_post.lin = [g_dev newSamplerStateWithDescriptor:sd];
        [sd release];
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead, td.storageMode = MTLStorageModeShared;
        g_post.black = [g_dev newTextureWithDescriptor:td];
        uint32_t zero = 0;
        [g_post.black replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];
    }
    if (!g_post.lib || !g_post.pre || !g_post.down || !g_post.up || !g_post.coc || !g_post.gather || !g_post.smarch || !g_post.sresolve)
        return 0;
    if (g_post.comp_fmt != scene_fmt || !g_post.comp || !g_post.dcomp || !g_post.scomp)
    {
        [g_post.comp release], [g_post.dcomp release], [g_post.scomp release], [g_post.sdbg release];
        [g_post.comp_add release], [g_post.scomp_add release], [g_post.svol release];
        g_post.comp = post_pipeline(@"bloom_comp", scene_fmt, 2);
        g_post.dcomp = post_pipeline(@"dof_comp", scene_fmt, 3);
        g_post.scomp = post_pipeline(@"shaft_comp", scene_fmt, 2);
        g_post.sdbg = post_pipeline(@"shaft_comp", scene_fmt, 0);
        g_post.comp_add = post_pipeline(@"bloom_comp", scene_fmt, 1);
        g_post.scomp_add = post_pipeline(@"shaft_comp", scene_fmt, 1);
        g_post.svol = post_pipeline(@"shaft_comp", scene_fmt, 4);
        g_post.comp_fmt = scene_fmt;
    }
    return g_post.comp && g_post.dcomp && g_post.scomp && g_post.comp_add && g_post.scomp_add;
}

/* a texture kept at this size and format (made again when either changes) */
static id<MTLTexture> post_tex(id<MTLTexture>* t, NSUInteger w, NSUInteger h, MTLPixelFormat fmt)
{
    if (*t && (*t).width == w && (*t).height == h && (*t).pixelFormat == fmt)
        return *t;
    [*t release];
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, d.storageMode = MTLStorageModePrivate;
    return *t = [g_dev newTextureWithDescriptor:d];
}

static void post_pass_n(id<MTLTexture> target, MTLLoadAction load, id<MTLRenderPipelineState> p, MTLViewport vp, const void* u,
    size_t un, id<MTLTexture> t0, id<MTLTexture> t1, id<MTLTexture> t2, int pass)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = load;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    ts_mark(rp, pass);
    id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p];
    [e setViewport:vp];
    [e setFragmentBytes:u length:un atIndex:0];
    [e setFragmentTexture:t0 atIndex:0];
    if (t1)
        [e setFragmentTexture:t1 atIndex:1];
    if (t2)
        [e setFragmentTexture:t2 atIndex:2];
    [e setFragmentSamplerState:g_post.lin atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

static void post_pass(id<MTLTexture> target, MTLLoadAction load, id<MTLRenderPipelineState> p, MTLViewport vp, const PostU* u,
    id<MTLTexture> t0, id<MTLTexture> t1, id<MTLTexture> t2, int pass)
{
    post_pass_n(target, load, p, vp, u, sizeof *u, t0, t1, t2, pass);
}

/* The sun shafts (post_shafts): the sun's or moon's light scattered toward the eye along each pixel's ray, through haze
 * as thick as the game's own fog says (thinning with height), where the scene renderer's zone shadow cascades see the
 * light - beams through trees, arches and gaps, and a glow round the sun by the scattering's forward lobe. Marched at
 * a third of the scene's size (shafts_q 2: 16 steps) or a fifth (1: 10), each texel's steps jittered and the result carried over frames
 * by where its point was, clamped to its neighbours; screen-blended onto the scene through a depth-aware upsample. Needs
 * the scene renderer's shadows (scene 1 or 2, scene_shadows): without them there is nothing to shade the beams by and the
 * pass stays off; indoors (no sun shadows) likewise, so nothing leaks through a roof. */
static float fog_sigma(const GfxScene* s);
static float fog_ref(void);
static float fog_at_cam(float sigma, float cam_y);

/* whether the fog is the shafts' volume this frame (fog_modern, fog_volume, and the sun's cascades to shade it by);
 * else the analytic pass (scene_fog) */
static int fog_volume_on(void)
{
    return g_fxs.fog_modern > 0.0f && g_fxs.fog_volume > 0.0f && g_shd.zmap && g_shd.cframe == g_serial && g_shd.u.dir[3] > 0.0f &&
        g_shd.u.info[0] > 0.0f && g_shd.u.info[2] > 0.0f;
}

static void post_shafts(GfxTex* color, const GfxScene* s, id<MTLTexture> depth, float vx, float vy, float vw, float vh)
{
    if (!g_shd.zmap || g_shd.cframe != g_serial || g_shd.u.dir[3] <= 0.0f || g_shd.u.info[0] <= 0.0f || g_shd.u.info[2] <= 0.0f)
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
    int q = g_fxs.shafts_q >= 1.5f ? 2 : 1, div = q == 2 ? 3 : 5; /* (of the 2048 scene: some 0.47 M and 0.17 M texels) */
    NSUInteger lw = (NSUInteger)(vw / div), lh = (NSUInteger)(vh / div);
    lw = lw ? lw : 1, lh = lh ? lh : 1;
    if (!post_tex(&g_post.sh, lw, lh, MTLPixelFormatRGBA16Float) || !post_tex(&g_post.sht, lw, lh, MTLPixelFormatR16Float) ||
        !post_tex(&g_post.shist[0], lw, lh, MTLPixelFormatRGBA16Float) ||
        !post_tex(&g_post.shist[1], lw, lh, MTLPixelFormatRGBA16Float))
        return;
    memcpy(u.m, g_shd.u.m, sizeof u.m), memcpy(u.cas, g_shd.u.cas, sizeof u.cas);
    /* the haze: as thick as the game's fog, the nearer its end the thicker */
    float fog_end = s->fog[2] != 0.0f && s->fog[1] > 1.0f ? s->fog[1] : 1000.0f;
    fog_end = fminf(fmaxf(fog_end, 30.0f), 3000.0f);
    memcpy(u.cam, g_shd.u.cam, 12);
    u.cam[3] = fminf(fmaxf(g_fxs.shafts_dist, 10.0f), fog_end * 1.5f);
    memcpy(u.sun, g_shd.u.dir, 12), u.sun[3] = 1.0f;
    /* the light: this frame's sun (or moon) colour, as strong as the shadows are drawn */
    float sc[3] = { 1.0f, 0.95f, 0.85f };
    if (s->sun_dir[3] != 0.0f)
        sc[0] = s->sun_color[0], sc[1] = s->sun_color[1], sc[2] = s->sun_color[2];
    /* strongest with the light low (long paths through the haze, the look of dawn and dusk), about a quarter of that
     * overhead: clear midday air shows little */
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
        float fc[3] = { s->fogcolor[0], s->fogcolor[1], s->fogcolor[2] };
        float lum = 0.2126f * fc[0] + 0.7152f * fc[1] + 0.0722f * fc[2];
        static const float cool[3] = { 0.88f, 0.97f, 1.18f };
        for (int c = 0; c < 3; ++c)
            u.airc[c] = fmaxf(fc[c] * 0.7f + lum * cool[c] * 0.3f, 0.0f);
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
        if (g_fxs.scene_lamps > 0.0f && g_lamps.binned == g_serial && g_lamps.lb && g_lamps.tb)
            u.lt[0] = g_lamps.lu[0], u.lt[1] = g_lamps.lu[1], u.lt[2] = g_lamps.lu[4], u.lt[3] = g_lamps.lu[5];
    }
    u.p[3] = (float)fmod((double)g_serial * 0.6180339887, 1.0);
    u.vp[0] = vx, u.vp[1] = vy, u.vp[2] = vw, u.vp[3] = vh;
    u.lo[0] = (float)lw, u.lo[1] = (float)lh, u.lo[2] = 1.0f / (float)lw, u.lo[3] = 1.0f / (float)lh;
    /* last frame's, when it was the frame before at this size and the camera didn't jump */
    float jx = u.cam[0] - g_post.sh_pcam[0], jy = u.cam[1] - g_post.sh_pcam[1], jz = u.cam[2] - g_post.sh_pcam[2];
    int have = g_post.sh_serial && g_post.sh_serial + 1 == g_serial && jx * jx + jy * jy + jz * jz < 25.0f && !cam_cut();
    memcpy(u.pvp, g_post.sh_pvp, 64), memcpy(u.pcam, g_post.sh_pcam, 12);
    u.hist[0] = have && g_fxs.shafts_debug >= 0.0f ? 1.0f : 0.0f, u.hist[1] = fminf(fmaxf(g_fxs.shafts_temporal, 0.0f), 0.97f), u.hist[2] = g_fxs.shafts_debug;
    id<MTLTexture> prev = g_post.shist[g_post.shist_at], out = g_post.shist[g_post.shist_at ^ 1];
    MTLViewport lv = { 0, 0, (double)lw, (double)lh, 0, 1 };
    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = g_post.sh;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[1].texture = g_post.sht;
        rp.colorAttachments[1].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[1].storeAction = MTLStoreActionStore;
        ts_mark(rp, GFX_PASS_POST_SHAFTS);
        id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:g_post.smarch];
        [e setViewport:lv];
        [e setFragmentBytes:&u length:sizeof u atIndex:0];
        [e setFragmentTexture:depth atIndex:0];
        [e setFragmentTexture:g_shd.zmap atIndex:1];
        int lt = u.lt[2] > 0.0f;
        [e setFragmentBuffer:lt ? g_lamps.lb : g_dummy offset:lt ? g_lamps.lo : 0 atIndex:1];
        [e setFragmentBuffer:lt ? g_lamps.tb : g_dummy offset:lt ? g_lamps.to : 0 atIndex:2];
        [e setFragmentSamplerState:g_post.lin atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    post_pass_n(out, MTLLoadActionDontCare, g_post.sresolve, lv, &u, sizeof u, g_post.sh, prev, depth, GFX_PASS_POST_SHAFTS);
    u.hist[3] = color->tex.pixelFormat == MTLPixelFormatRGBA16Float ? 1.0f : 0.0f;
    post_pass_n(color->tex, MTLLoadActionLoad, u.hist[2] != 0.0f ? g_post.sdbg : vol ? g_post.svol : u.hist[3] > 0.0f ? g_post.scomp_add : g_post.scomp, (MTLViewport){ vx, vy, vw, vh, 0, 1 }, &u, sizeof u, out, depth, g_post.sht,
        GFX_PASS_POST_SHAFTS);
    g_post.shist_at ^= 1, g_post.sh_serial = g_serial;
    memcpy(g_post.sh_pvp, VP, 64), memcpy(g_post.sh_pcam, u.cam, 12);
}

/* The modern fog (fog_modern; FOG_MSL above): over the finished scene before the shafts, depth of field and bloom. Its
 * density from the game's fog end (some 4% of a point's light left there, so the draw distance's edge stays hidden) times
 * fog_density, thinning with height by fog_thin; indoors (a zone's fixed light) a light dust. The scene renderer's own
 * draws leave the game's fog off while it is on (scene.c). */
typedef struct
{
    float ivp[16];
    float cam[4], sun[4], sunc[4], airc[4], p[4], vp[4], lt[4], dbg[4];
} FogU;

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

static float fog_sigma(const GfxScene* s)
{

    float fog_end = s->fog[2] != 0.0f && s->fog[1] > 1.0f ? s->fog[1] : 600.0f;
    fog_end = fminf(fmaxf(fog_end, 20.0f), 3000.0f);
    float k = fmaxf(g_fxs.fog_density, 0.0f) * 3.2f / fog_end; /* exp(-3.2): 4% */
    (void)s;
    return k * (g_fixed_light ? 0.15f : 1.0f); /* (GfxScene.indoors waits for the sun to be seen moving: too slow for this) */
}

static void scene_fog(GfxTex* color, const GfxScene* s)
{
    if (g_fxs.fog_modern <= 0.0f || !gfx_world_supported() || s->proj[11] == 0.0f || fog_volume_on())
        return;
    static id<MTLRenderPipelineState> pipe[2];
    id<MTLTexture> ct = color->tex, depth = color->depth_world ? color->depth_world : color->depth_seen;
    if (!depth || depth.width != ct.width || depth.height != ct.height || !post_init(ct.pixelFormat))
        return;
    int hdr = ct.pixelFormat == MTLPixelFormatRGBA16Float;
    if (!pipe[hdr] && !(pipe[hdr] = post_pipeline(@"fog_comp", ct.pixelFormat, 3)))
        return;
    float Vr[16], VP[16], iv[16];
    memcpy(Vr, s->view, 64);
    Vr[12] = Vr[13] = Vr[14] = 0.0f;
    gfx_mat_mul(VP, Vr, s->proj);
    FogU u;
    memset(&u, 0, sizeof u);
    if (!gfx_mat_inverse(u.ivp, VP) || !gfx_mat_inverse(iv, s->view))
        return;
    float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > ct.width || vy + vh > ct.height)
        vx = vy = 0, vw = (float)ct.width, vh = (float)ct.height;
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
    /* the air's colour: the game's fog colour, a little cooler and brighter where the sky lights it; the sun's forward
     * scattering in its own colour, none with the shafts on (theirs is shadowed) */
    float fc[3] = { s->fogcolor[0], s->fogcolor[1], s->fogcolor[2] };
    float lum = 0.2126f * fc[0] + 0.7152f * fc[1] + 0.0722f * fc[2];
    static const float cool[3] = { 0.88f, 0.97f, 1.18f };
    for (int c = 0; c < 3; ++c)
        u.airc[c] = fmaxf(fc[c] * 0.7f + lum * cool[c] * 0.3f, 0.0f);
    int shafts = g_fxs.post_shafts > 0.0f && g_shd.zmap && g_shd.cframe == g_serial && g_shd.u.dir[3] > 0.0f;
    float up = fmaxf(-u.sun[1], 0.0f), low = 1.0f - up;
    for (int c = 0; c < 3; ++c)
        u.sunc[c] = shafts || s->sun_dir[3] == 0.0f ? 0.0f : fmaxf(s->sun_color[c], 0.0f) * (0.15f + 0.6f * low * low);
    u.p[0] = fog_at_cam(fog_sigma(s), u.cam[1]), u.p[1] = fmaxf(g_fxs.fog_thin, 0.0f), u.p[2] = fmaxf(g_fxs.fog_lamps, 0.0f) * 0.05f; /* (mist round a lamp: its own, not the haze's) */
    u.p[3] = hdr ? 0.0f : 1.0f / 255.0f;
    u.vp[0] = vx, u.vp[1] = vy, u.vp[2] = vw, u.vp[3] = vh;
    int lamps = g_fxs.scene_lamps > 0.0f && g_lamps.binned == g_serial && g_lamps.lb && g_lamps.tb;
    if (lamps)
        u.lt[0] = g_lamps.lu[0], u.lt[1] = g_lamps.lu[1], u.lt[2] = g_lamps.lu[4], u.lt[3] = g_lamps.lu[5];
    u.dbg[0] = g_fxs.fog_debug, u.dbg[1] = g_fxs.fog_lamp_lift;
    flush_pass();
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = ct;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    ts_mark(rp, GFX_PASS_POST_SHAFTS);
    id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:pipe[hdr]];
    [e setViewport:(MTLViewport){ vx, vy, vw, vh, 0, 1 }];
    [e setFragmentBytes:&u length:sizeof u atIndex:0];
    [e setFragmentTexture:depth atIndex:0];
    [e setFragmentBuffer:lamps ? g_lamps.lb : g_dummy offset:lamps ? g_lamps.lo : 0 atIndex:1];
    [e setFragmentBuffer:lamps ? g_lamps.tb : g_dummy offset:lamps ? g_lamps.to : 0 atIndex:2];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

/* the lamps' glows: a sprite each, added to the scene (scene_lamp_glow) */
static void post_lamp_glows(GfxTex* color, const GfxScene* s, id<MTLTexture> depth, float vx, float vy, float vw, float vh)
{
    if (g_fxs.scene_lamps <= 0.0f || g_fxs.scene_lamp_glow <= 0.0f || g_lamps.binned != g_serial || !g_lamps.n || !g_lamps.lb)
        return;
    static id<MTLRenderPipelineState> pipe[2];
    int hdr = color->tex.pixelFormat == MTLPixelFormatRGBA16Float;
    if (!pipe[hdr])
    {
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        id<MTLFunction> vf = [g_post.lib newFunctionWithName:@"lamp_glow_vs"], ff = [g_post.lib newFunctionWithName:@"lamp_glow_fs"];
        pd.vertexFunction = vf, pd.fragmentFunction = ff;
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
        ca.pixelFormat = color->tex.pixelFormat;
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = MTLBlendFactorOne, ca.destinationRGBBlendFactor = MTLBlendFactorOne;
        ca.sourceAlphaBlendFactor = MTLBlendFactorZero, ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
        NSError* err = nil;
        pipe[hdr] = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pipe[hdr])
            fprintf(stderr, "[recomp] gfx: lamp glows failed: %s\n", err ? [[err localizedDescription] UTF8String] : "?");
        [vf release], [ff release], [pd release];
        if (!pipe[hdr])
            return;
    }
    float minz, maxz;
    memcpy(&minz, &s->vp[4], 4);
    memcpy(&maxz, &s->vp[5], 4);
    if (maxz <= minz)
        minz = 0, maxz = 1;
    struct { float vp[16], p[4], cam[4], zp[4], vpr[4], pr[4]; } u = { { 0 } };
    memcpy(u.vp, g_lamps.vp, 64);
    u.p[0] = fmaxf(g_fxs.scene_lamp_glow_size, 0.05f), u.p[1] = g_fxs.scene_lamp_glow, u.p[2] = fmaxf(g_fxs.scene_lamp_far, 1.0f);
    memcpy(u.cam, g_lamps.cam, 12);
    u.zp[0] = s->proj[10], u.zp[1] = s->proj[14], u.zp[2] = minz, u.zp[3] = maxz;
    u.vpr[0] = vx, u.vpr[1] = vy, u.vpr[2] = vw, u.vpr[3] = vh;
    u.pr[0] = s->proj[0], u.pr[1] = s->proj[5], u.pr[2] = s->proj[11] < 0.0f ? -1.0f : 1.0f;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = color->tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    ts_mark(rp, GFX_PASS_LAMPS);
    id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:pipe[hdr]];
    [e setViewport:(MTLViewport){ vx, vy, vw, vh, 0, 1 }];
    [e setVertexBytes:&u length:sizeof u atIndex:0];
    [e setFragmentBytes:&u length:sizeof u atIndex:0];
    [e setVertexBuffer:g_lamps.lb offset:g_lamps.lo atIndex:1];
    [e setFragmentTexture:depth atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:g_lamps.n];
    [e endEncoding];
}

/* the glow's draws again, onto black over the scene's depth; 0 when there were none */
static int post_glow(GfxTex* color)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_nglows; ++i)
        n += g_glows[i].rt == color && g_glows[i].fmt == color->tex.pixelFormat; /* (not those from before a format change) */
    if (!n)
        return 0;
    id<MTLTexture> ct = color->tex;
    if (!post_tex(&g_post.glow, ct.width, ct.height, ct.pixelFormat))
        return 0;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = g_post.glow;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLTexture> depth = nil;
    for (uint32_t i = 0; i < g_nglows && !depth; ++i)
        if (g_glows[i].rt == color && g_glows[i].fmt == ct.pixelFormat)
            depth = g_glows[i].depth;
    if (depth)
    {
        rp.depthAttachment.texture = depth;
        rp.depthAttachment.loadAction = MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
        MTLPixelFormat df = depth.pixelFormat;
        if (format_has_stencil(df))
        {
            rp.stencilAttachment.texture = depth;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
            rp.stencilAttachment.storeAction = MTLStoreActionDontCare;
        }
    }
    ts_mark(rp, GFX_PASS_POST_BLOOM);
    id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
    for (uint32_t i = 0; i < g_nglows; ++i)
    {
        Glow* g = &g_glows[i];
        if (g->rt != color || g->depth != depth || g->fmt != ct.pixelFormat)
            continue;
        [e setRenderPipelineState:g->pipe];
        [e setDepthStencilState:g->ds];
        [e setCullMode:g->cull == 2 ? MTLCullModeBack : g->cull == 1 ? MTLCullModeFront : MTLCullModeNone];
        [e setDepthBias:-(float)g->zbias slopeScale:-(float)g->zbias * 0.5f clamp:0];
        [e setViewport:g->vp];
        [e setScissorRect:g->sc];
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            if (g->vb[s])
                [e setVertexBuffer:g->vb[s] offset:g->voff[s] atIndex:(NSUInteger)s];
        [e setVertexBuffer:g->ub offset:g->uoff atIndex:4];
        [e setFragmentBuffer:g->ub offset:g->uoff atIndex:4];
        for (int t = 0; t < g->ntex; ++t)
            if (g->tex[t])
                [e setFragmentTexture:g->tex[t] atIndex:(NSUInteger)t], [e setFragmentSamplerState:g->samp[t] atIndex:(NSUInteger)t];
        if (g->ib)
            [e drawIndexedPrimitives:g->prim indexCount:g->n indexType:g->itype == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                         indexBuffer:g->ib indexBufferOffset:g->ioff];
        else
            [e drawPrimitives:g->prim vertexStart:g->vstart vertexCount:g->n];
    }
    [e endEncoding];
    return 1;
}

static void post_effects(GfxTex* color, const GfxScene* s)
{
    int bloom = g_fxs.post_bloom > 0.0f, dof = g_fxs.post_dof > 0.0f, shafts = g_fxs.post_shafts > 0.0f;
    int glows = g_fxs.scene_lamps > 0.0f && g_fxs.scene_lamp_glow > 0.0f && g_lamps.binned == g_serial;
    if (!bloom && !dof && !shafts && !glows && !fog_volume_on())
    {
        g_post.sh_serial = 0;
        return;
    }
    @autoreleasepool
    {
        flush_pass();
        cam_cut_note(s->view);
        id<MTLTexture> ct = color->tex, depth = color->depth_world ? color->depth_world : color->depth_seen;
        if (!post_init(ct.pixelFormat))
            return;
        float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
        if (vw < 16 || vh < 16 || vx + vw > ct.width || vy + vh > ct.height)
            vx = vy = 0, vw = (float)ct.width, vh = (float)ct.height;
        float minz, maxz;
        memcpy(&minz, &s->vp[4], 4);
        memcpy(&maxz, &s->vp[5], 4);
        if (maxz <= minz)
            minz = 0, maxz = 1;
        /* a texel step down the target per step across: the scene's square stretched to the screen's shape (the
         * projection's aspect), so a blur is round on screen */
        float aspect = s->proj[0] != 0.0f ? fabsf(s->proj[5] / s->proj[0]) : 1.0f;
        PostU u = { { vx, vy, vw, vh }, { 0 }, { 0 }, { 0 }, { 0 }, { s->proj[10], s->proj[14], minz, maxz },
            { aspect * vh / vw, 0, 0, 0 } };
        u.dof2[1] = s->proj[11] < 0.0f ? -1.0f : 1.0f, u.dof2[2] = POST_LEVELS, u.dof2[3] = fmaxf(g_fxs.bloom_radius, 0.0f);
        NSUInteger hw = (NSUInteger)(vw + 1) / 2, hh = (NSUInteger)(vh + 1) / 2;
        if (glows && depth && depth.width == ct.width && depth.height == ct.height && s->proj[11] != 0.0f)
            post_lamp_glows(color, s, depth, vx, vy, vw, vh);
        if ((shafts || fog_volume_on()) && depth && depth.width == ct.width && depth.height == ct.height && s->proj[11] != 0.0f)
            post_shafts(color, s, depth, vx, vy, vw, vh); /* (before the depth of field and the bloom: both take it; the fog volume too) */
        else
            g_post.sh_serial = 0;
        if (dof && depth && depth.width == ct.width && depth.height == ct.height && s->proj[11] != 0.0f)
        {
            /* the focus: the player's distance along the view (or dof_focus), eased over some 0.2 s; a jump (a new
             * place, the camera cut) taken at once */
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
            if (!(g_post.focus > 0.0f) || !isfinite(g_post.focus) || dt > 0.5 || cam_cut() ||
                fabsf(want - g_post.focus) > 0.5f * fmaxf(want, g_post.focus))
                g_post.focus = want;
            else
                g_post.focus += (want - g_post.focus) * (float)(1.0 - exp(-dt / 0.2));
            u.dof[0] = g_post.focus, u.dof[1] = fmaxf(g_fxs.dof_range, 0.0f), u.dof[2] = fmaxf(g_fxs.dof_ramp, 1e-3f);
            u.dof[3] = g_fxs.post_dof * 4.0f * vw / 2048.0f; /* at 1: some 8 pixels of a 2048 scene */
            u.dof2[0] = fminf(fmaxf(g_fxs.dof_near, 0.0f), 1.0f);
            if (post_tex(&g_post.h0, hw, hh, MTLPixelFormatRGBA16Float) && post_tex(&g_post.h1, hw, hh, MTLPixelFormatRGBA16Float))
            {
                MTLViewport hv = { 0, 0, (double)hw, (double)hh, 0, 1 };
                u.src[0] = (float)hw, u.src[1] = (float)hh, u.src[2] = 1.0f / (float)hw, u.src[3] = 1.0f / (float)hh;
                post_pass(g_post.h0, MTLLoadActionDontCare, g_post.coc, hv, &u, ct, depth, nil, GFX_PASS_POST_DOF);
                post_pass(g_post.h1, MTLLoadActionDontCare, g_post.gather, hv, &u, g_post.h0, nil, nil, GFX_PASS_POST_DOF);
                post_pass(ct, MTLLoadActionLoad, g_post.dcomp, (MTLViewport){ vx, vy, vw, vh, 0, 1 }, &u, g_post.h1, depth,
                    nil, GFX_PASS_POST_DOF);
            }
        }
        if (bloom && depth && depth.width == ct.width && depth.height == ct.height)
        {
            u.bloom[0] = g_fxs.bloom_threshold, u.bloom[1] = g_fxs.bloom_knee, u.bloom[2] = g_fxs.bloom_emissive_fx;
            u.aux[1] = fmaxf(g_fxs.bloom_emissive_lights, 0.0f);
            u.bloom[3] = g_fxs.post_bloom;
            id<MTLTexture> glow = post_glow(color) ? g_post.glow : g_post.black;
            NSUInteger w[POST_LEVELS], h[POST_LEVELS];
            int ok = 1;
            for (int i = 0; i < POST_LEVELS; ++i)
            {
                w[i] = (NSUInteger)(vw / (float)(2 << i)), h[i] = (NSUInteger)(vh / (float)(2 << i));
                w[i] = w[i] ? w[i] : 1, h[i] = h[i] ? h[i] : 1;
                ok = ok && post_tex(&g_post.lvl[i], w[i], h[i], MTLPixelFormatRG11B10Float);
            }
            if (ok)
            {
                u.src[0] = (float)ct.width, u.src[1] = (float)ct.height, u.src[2] = 1.0f / (float)ct.width, u.src[3] = 1.0f / (float)ct.height;
                post_pass(g_post.lvl[0], MTLLoadActionDontCare, g_post.pre, (MTLViewport){ 0, 0, (double)w[0], (double)h[0], 0, 1 }, &u, ct,
                    glow, depth, GFX_PASS_POST_BLOOM);
                for (int i = 1; i < POST_LEVELS; ++i)
                {
                    u.src[0] = (float)w[i - 1], u.src[1] = (float)h[i - 1], u.src[2] = 1.0f / (float)w[i - 1], u.src[3] = 1.0f / (float)h[i - 1];
                    post_pass(g_post.lvl[i], MTLLoadActionDontCare, g_post.down, (MTLViewport){ 0, 0, (double)w[i], (double)h[i], 0, 1 }, &u,
                        g_post.lvl[i - 1], nil, nil, GFX_PASS_POST_BLOOM);
                }
                for (int i = POST_LEVELS - 2; i >= 0; --i)
                {
                    u.src[0] = (float)w[i + 1], u.src[1] = (float)h[i + 1], u.src[2] = 1.0f / (float)w[i + 1], u.src[3] = 1.0f / (float)h[i + 1];
                    post_pass(g_post.lvl[i], MTLLoadActionLoad, g_post.up, (MTLViewport){ 0, 0, (double)w[i], (double)h[i], 0, 1 }, &u,
                        g_post.lvl[i + 1], nil, nil, GFX_PASS_POST_BLOOM);
                }
                u.aux[3] = ct.pixelFormat == MTLPixelFormatRGBA16Float ? 1.0f : 0.0f;
                post_pass(ct, MTLLoadActionLoad, u.aux[3] > 0.0f ? g_post.comp_add : g_post.comp, (MTLViewport){ vx, vy, vw, vh, 0, 1 }, &u, g_post.lvl[0], nil,
                    nil, GFX_PASS_POST_BLOOM);
            }
        }
        color->scene = 0; /* its mips are behind (scene_mips) */
        color->used = g_serial;
    }
}

/* --- the HDR scene (scene_hdr) ------------------------------------------------------------------------------------
 * The game's world target (its 2048 background) made RGBA16F in place: a new texture of the same size and levels, the
 * old one's first level drawn into it, so the frame goes on with what was drawn. Every draw after renders linear light
 * past 1 where it has it (the scene renderer's specular, the sun shafts, the bloom, the game's additive effects summed
 * without clipping), and one tonemap pass at the scene's end (hdr_tone) brings it back to the display's range before
 * the nameplates and the interface go over it: tonemap 0 neutral (today's image: clamped), 1 a highlight shoulder
 * (today's image below its knee, what is past it rolled off), 2 ACES. The game's own
 * draws go in unchanged (their pipelines are built for the target's format); its copy of the scene onto the back buffer
 * and the water's screen copy read the float target. scene_hdr 0 turns it back. */
static struct
{
    GfxTex* t;                              /* the target made float */
    id<MTLRenderPipelineState> copy[2];     /* hdr_copy into RGBA16F, into BGRA8 */
    id<MTLRenderPipelineState> tone;        /* hdr_tone on RGBA16F */
} g_hdr;

static void hdr_swap(GfxTex* t, MTLPixelFormat pf)
{
    id<MTLTexture> old = t->tex;
    if (old.pixelFormat == pf || !post_init(old.pixelFormat))
        return;
    id<MTLRenderPipelineState>* cp = &g_hdr.copy[pf == MTLPixelFormatRGBA16Float ? 0 : 1];
    if (!*cp)
        *cp = post_pipeline(@"hdr_copy", pf, 0);
    if (!*cp)
        return;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:old.width height:old.height mipmapped:NO];
    d.mipmapLevelCount = old.mipmapLevelCount;
    d.storageMode = MTLStorageModePrivate;
    d.usage = old.usage;
    id<MTLTexture> nt = [g_dev newTextureWithDescriptor:d];
    if (!nt)
        return;
    end_pass();
    flush_pass();
    PostU u;
    memset(&u, 0, sizeof u);
    MTLTextureDescriptor* vd = nil;
    (void)vd;
    id<MTLTexture> src = [old newTextureViewWithPixelFormat:old.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
    post_pass(nt, MTLLoadActionDontCare, *cp, (MTLViewport){ 0, 0, (double)old.width, (double)old.height, 0, 1 }, &u, src, nil, nil,
        GFX_PASS_HDR);
    [src release];
    /* the views as gfx_tex_create made them: the first level (or all of it), the alpha forced to one for X8 */
    int mipped = t->mipview != nil;
    [t->view release], [t->mipview release], t->view = t->mipview = nil;
    t->tex = nt;
    if (t->x8)
        t->view = [nt newTextureViewWithPixelFormat:pf textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)
                                            swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                                        MTLTextureSwizzleBlue, MTLTextureSwizzleOne)];
    else if (mipped)
        t->view = [nt newTextureViewWithPixelFormat:pf textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
    else
        t->view = [nt retain];
    if (mipped)
        t->mipview = [nt retain];
    [old release]; /* (the command buffers that used it hold their own) */
    t->scene = 0, t->used = g_serial;
    bound_forget();
}

/* the scene's target float while scene_hdr is on (and back once it is off) */
static void hdr_update(GfxTex* color)
{
    int want = g_fxs.scene_hdr > 0.0f && color->use == GFX_USE_RT && color->tex.mipmapLevelCount > 1 && !color->block;
    if (g_hdr.t && g_hdr.t != color && g_hdr.t->tex.pixelFormat == MTLPixelFormatRGBA16Float)
        hdr_swap(g_hdr.t, MTLPixelFormatBGRA8Unorm), g_hdr.t = NULL; /* (another target is the world now) */
    if (want && color->tex.pixelFormat == MTLPixelFormatBGRA8Unorm)
        hdr_swap(color, MTLPixelFormatRGBA16Float), g_hdr.t = color;
    else if (!want && g_hdr.t == color && color->tex.pixelFormat == MTLPixelFormatRGBA16Float)
        hdr_swap(color, MTLPixelFormatBGRA8Unorm), g_hdr.t = NULL;
}

/* the tonemap over the scene's viewport, in place (the GPU reads each pixel back as it shades it) */
static void hdr_tone(GfxTex* color, const GfxScene* s)
{
    if (g_hdr.t != color || color->tex.pixelFormat != MTLPixelFormatRGBA16Float)
        return;
    @autoreleasepool
    {
        if (!g_hdr.tone)
            g_hdr.tone = post_pipeline(@"hdr_tone", MTLPixelFormatRGBA16Float, 0);
        if (!g_hdr.tone)
            return;
        flush_pass();
        id<MTLTexture> ct = color->tex;
        float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
        if (vw < 16 || vh < 16 || vx + vw > ct.width || vy + vh > ct.height)
            vx = vy = 0, vw = (float)ct.width, vh = (float)ct.height;
        float k[4] = { g_fxs.tonemap, fmaxf(g_fxs.exposure, 0.01f), fmaxf(g_fxs.tone_white, 1.01f), fminf(fmaxf(g_fxs.tone_knee, 0.3f), 0.95f) };
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = ct;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        ts_mark(rp, GFX_PASS_HDR);
        id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:g_hdr.tone];
        [e setViewport:(MTLViewport){ vx, vy, vw, vh, 0, 1 }];
        [e setFragmentBytes:k length:sizeof k atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        color->scene = 0, color->used = g_serial;
    }
}

static void hdr_forget(GfxTex* t)
{
    if (g_hdr.t == t)
        g_hdr.t = NULL;
}

static void scene_fx(GfxTex* color, const GfxScene* s);

void gfx_scene_done(GfxTex* color, const GfxScene* s)
{
    if (!g_dev || !color || color->type != GFX_TEX_2D)
        return;
    @autoreleasepool
    {
        hdr_update(color);
    }
    ao_make(color); /* the scene renderer's occlusion, for the next frame's zone and characters */
    if (g_fxs.fx != 0.0f)
        scene_fx(color, s);
    scene_fog(color, s);
    post_effects(color, s);
    hdr_tone(color, s);
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
            u.aop[1] = gfx_contact_steps();
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
            /* the sun kept for whatever wants it: its shadows, the god rays and the fog's glow toward it (Low has no
             * shadows, and its rays would go out with no lit draw in view - the sky, where they show) */
            int wanted = g_fxs.sun > 0.0f || g_fxs.shadow > 0.0f || g_fxs.rays > 0.0f || (g_fxs.fog > 0.0f && g_fxs.fog_sun > 0.0f);
            if (have_v && (own || (wanted && g_fx.sunw_seen && g_fx.sunw_seen + 600 > g_serial)))
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
            u.bloom[0] = g_fxs.threshold, u.bloom[1] = g_fxs.post_bloom > 0.0f ? 0.0f : g_fxs.bloom, u.bloom[2] = 0.25f;
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
                        id<MTLTexture> lz = fx_lz(aw, ah);
                        g_pass = GFX_PASS_LINZ;
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
                        g_pass = GFX_PASS_AO;
                        fx_pass(g_fx.ao0, MTLLoadActionDontCare, g_fx.ao_pipe, q, &u, ao_in, 4, NULL);
                        /* traced (rt): the occlusion from rays in the world, the sun's as fx_ao found it; ao1 then holds it */
                        id<MTLTexture> a = g_fx.ao0, bt = g_fx.ao1;
                        if (rt && u.ao[1] > 0.0f)
                        {
                            id<MTLTexture> r_in[2] = { g_fx.ao0, depth };
                            g_pass = GFX_PASS_AO_RT;
                            rt_pass(g_fx.ao1, MTLLoadActionDontCare, g_ray.ao, q, &u, r_in, 2, NULL);
                            a = g_fx.ao1, bt = g_fx.ao0;
                        }
                        g_pass = GFX_PASS_BLUR;
                        fx_pass(bt, MTLLoadActionDontCare, g_fx.blur_pipe, q, &u, &a, 1, across);
                        fx_pass(a, MTLLoadActionDontCare, g_fx.blur_pipe, q, &u, &bt, 1, down);
                        ao_out = a;
                        if (g_fxs.temporal > 0.0f && fx_tex(&g_fx.hist[0], MTLPixelFormatRGBA16Float, aw, ah) &&
                            fx_tex(&g_fx.hist[1], MTLPixelFormatRGBA16Float, aw, ah))
                        {
                            int to = g_fx.hist_at ^ 1;
                            id<MTLTexture> t_in[2] = { a, g_fx.hist[g_fx.hist_at] };
                            g_pass = GFX_PASS_TEMPORAL;
                            fx_pass(g_fx.hist[to], MTLLoadActionDontCare, g_fx.temporal_pipe, q, &u, t_in, 2, NULL);
                            ao_out = g_fx.hist[to];
                            g_fx.hist_at = to, g_fx.hist_serial = g_serial;
                        }
                    }
                    /* the bounce light: gathered from its map at half the occlusion's size, then over frames */
                    id<MTLTexture> gi_out = nil;
                    if (gi)
                    {
                        MTLViewport qg = fx_full(g_fx.gi0);
                        g_pass = GFX_PASS_GI;
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
                        g_pass = GFX_PASS_BLOOM;
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
                        g_pass = GFX_PASS_RAYS;
                        fx_pass(g_fx.ra, MTLLoadActionDontCare, g_fx.raymask_pipe, qb, &u, mask_in, 2, NULL);
                        fx_pass(g_fx.rb, MTLLoadActionDontCare, g_fx.rays_pipe, qb, &u, &g_fx.ra, 1, NULL);
                        fx_pass(g_fx.ra, MTLLoadActionDontCare, g_fx.gauss_pipe, qb, &u, &g_fx.rb, 1, across);
                        fx_pass(g_fx.rb, MTLLoadActionDontCare, g_fx.gauss_pipe, qb, &u, &g_fx.ra, 1, down);
                    }
                    id<MTLTexture> comp_in[7] = { g_fx.src, ao_out, depth, g_fx.b1a, g_fx.b2a, g_fx.rb, gi_out ? gi_out : g_fx.rb };
                    g_pass = GFX_PASS_COMP;
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
static void shadow_frame_end(void);
static void frame_end(void)
{
    casters_clear();
    glows_clear();
    shadow_frame_end();
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
                ts_mark(p, GFX_PASS_PRESENT);
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
    gfx_pass_tick();
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

/* The present interval. iOS has no switch: its layer always waits for the display (docs/ios.md). */
static void layer_vsync(int vsync)
{
#if TARGET_OS_OSX
    g_layer.displaySyncEnabled = vsync ? YES : NO;
#else
    (void)vsync;
#endif
}

int gfx_init(void* window, int vsync)
{
    if (g_dev)
    {
        /* up already (host64's sign-in screen, on this same window): the game's present interval */
        if (g_layer)
            layer_vsync(vsync);
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
        const char* bc = getenv("FFXI_BC"); /* 0: decode DXT as an iPhone must (to test that on a Mac) */
        g_bc = g_dev.supportsBCTextureCompression && !(bc && bc[0] == '0');
        if (!g_bc)
            fprintf(stderr, "[recomp] gfx: no BC textures on this GPU: DXT decoded on upload\n");
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
                layer_vsync(vsync);
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
