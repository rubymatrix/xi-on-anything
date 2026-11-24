/* The graphics back end on deko3d, the Switch's own low-level API (devkitPro): gfx.h for the Nintendo Switch,
 * next to gfx_gl.c (OpenGL on Mesa's nouveau), chosen at build time (tools/build_switch.py, XI_SWITCH_GFX=deko3d).
 * Why: nouveau's CPU work per draw is the limit at stock clocks; deko3d records a draw for under a microsecond
 * (platforms/switch/tests/drawcost).
 *
 * Threads: as gfx_gl.c, the game is built with the render queue (GFX_QUEUE, gfx_queue.c), whose render thread
 * makes the device and makes every call (GFXQ_OWNED). Log lines from it are handed to the game's thread
 * (gfx_gl_prof_take, from host64.c), which writes them: stdio from another thread breaks the main thread's.
 *
 * Coordinates: deko3d's defaults are D3D8's (depth 0..1, the origin at the top left, clip-space y up), so the
 * shaders (gfx_msl.c's deko3d dialect, gfx_dk_generate) keep D3D's clip space and nothing is flipped.
 *
 * Where it is (the plan's phase 2, its milestones):
 *  1. bring-up (this): the device, the queue, the swapchain on the default window and the present, which
 *     clears the screen; textures and buffers are kept as records only, draws are counted and dropped, reads
 *     give zeros;
 *  2. resources: buffers, textures and their formats, targets, clears, copies;
 *  3. programs (gfx_dk_generate, uam on a thread of its own, a DKSH cache on the card), state and draws;
 *  4. the world, the characters, the reads, the present's letterboxing and CRT pass. */
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
#define FB_NUM 3 /* three: with two, a swap interval of 0 still waited for the display */
#define FB_W 1920 /* the swapchain's images: the docked screen's size, cropped to the handheld's at each present */
#define FB_H 1080
#define CMD_SLOTS 2
#define CMD_SLICE (1u << 20)

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
    uint64_t since, front_ns, shim_ns, rt_ns[64], acquire_ns, submit_ns;
    uint32_t frames, draws, clears, copies, skips, targets, tex_made, buf_made, uploads;
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
    LOG("[dk] %.1f fps; a frame: %.0f draws, %.1f clears, %.1f copies, %.1f target changes, %.0f uploads (%.1f KB); "
        "textures %u live (%u made), buffers %u live (%u made); acquire %.2f ms, submit %.2f ms a frame\n",
        g_prof.frames / s, g_prof.draws / f, g_prof.clears / f, g_prof.copies / f, g_prof.targets / f, g_prof.uploads / f,
        (double)g_prof.upload_bytes / f / 1024.0, g_tex_live, g_prof.tex_made, g_buf_live, g_prof.buf_made,
        (double)g_prof.acquire_ns / f * 1e-6, (double)g_prof.submit_ns / f * 1e-6);
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since = now;
}

/* --- the device ----------------------------------------------------------------------------------------- */
static DkDevice g_dev;
static DkQueue g_q;
static DkMemBlock g_fb_mem, g_cmd_mem;
static DkImage g_fb[FB_NUM];
static DkSwapchain g_sc;
static DkCmdBuf g_cb;
static DkFence g_fence[CMD_SLOTS];
static uint32_t g_slot;
static int g_up, g_vsync, g_interval = 1;
static volatile uint32_t g_failures;
static uint32_t g_dk_errors;
static int g_prect[4];

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
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_dev);
    qm.flags = DkQueueFlags_Graphics;
    g_q = dkQueueCreate(&qm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, g_dev);
    g_cb = dkCmdBufCreate(&cm);
    if (!g_q || !g_cb || !g_cmd_mem)
    {
        LOG("[dk] gfx: no queue or command memory\n");
        return 0;
    }
    g_vsync = vsync;
    set_interval();
    g_up = 1;
    LOG("[dk] gfx: deko3d up: %d swapchain images of %dx%d (%u KB), %u KB of command memory; bring-up: nothing is "
        "drawn yet\n", FB_NUM, FB_W, FB_H, fb_size * FB_NUM >> 10, CMD_SLICE * CMD_SLOTS >> 10);
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

/* --- resources (bring-up: records, no memory on the GPU) ------------------------------------------------ */
struct GfxTex
{
    int type, use;
    uint32_t fmt, w, h, levels;
};

struct GfxBuf
{
    uint32_t size;
};

GfxTex* gfx_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->type = type, t->use = use, t->fmt = d3dfmt, t->w = w, t->h = h, t->levels = levels ? levels : 1;
    ++g_tex_live, ++g_prof.tex_made;
    return t;
}

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    --g_tex_live;
    free(t);
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const void* src, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)x, (void)y, (void)src;
    ++g_prof.uploads, g_prof.upload_bytes += (uint64_t)pitch * h;
    (void)w;
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (!t)
        return;
    uint32_t h = t->h >> level;
    gfx_tex_upload_rect(t, face, level, 0, 0, t->w >> level, h ? h : 1, src, pitch);
}

static uint32_t read_rows(GfxTex* t, uint32_t level)
{
    if (!t)
        return 0;
    uint32_t h = t->h >> level;
    return h ? h : 1;
}

void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    (void)face;
    if (dst)
        memset(dst, 0, (size_t)pitch * read_rows(t, level));
}

void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    gfx_tex_read(t, face, level, dst, pitch);
}

GfxBuf* gfx_buf_create(uint32_t size)
{
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->size = size;
    ++g_buf_live, ++g_prof.buf_made;
    return b;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    --g_buf_live;
    free(b);
}

void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    (void)b, (void)data;
    ++g_prof.uploads, g_prof.upload_bytes += size;
}

/* --- drawing (bring-up: counted) ------------------------------------------------------------------------ */
void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    (void)color, (void)face, (void)level, (void)depth;
    ++g_prof.targets;
}

void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
               const uint32_t vp[6])
{
    (void)nrects, (void)rects, (void)flags, (void)color, (void)z, (void)stencil, (void)vp;
    ++g_prof.clears;
}

void gfx_draw(const GfxDraw* d)
{
    (void)d;
    ++g_prof.draws;
}

void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
              uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    (void)src, (void)sface, (void)slevel, (void)sx, (void)sy, (void)w, (void)h, (void)dst, (void)dface, (void)dlevel,
        (void)dx, (void)dy;
    ++g_prof.copies;
}

/* the characters: not drawn by the back end yet (0: the game draws them through gfx_draw) */
int gfx_chars_draw(const GfxCharFrame* f)
{
    (void)f;
    return 0;
}
int gfx_chars_env(void) { return 0; }

/* --- the present ---------------------------------------------------------------------------------------- */
void gfx_set_present_rect(int x, int y, int w, int h)
{
    g_prect[0] = x, g_prect[1] = y, g_prect[2] = w, g_prect[3] = h;
}
void gfx_set_wallpaper(const uint8_t* rgba, int w, int h) { (void)rgba, (void)w, (void)h; }
void gfx_set_touch_shapes(const GfxTouchShape* shapes, int n) { (void)shapes, (void)n; }

void gfx_present(GfxTex* backbuffer)
{
    if (!g_up)
        return;
    int dw, dh;
    screen_size(&dw, &dh);
    int rx = 0, ry = 0, rw = dw, rh = dh;
    if (g_prect[2] > 0 && g_prect[3] > 0)
        rx = g_prect[0], ry = g_prect[1], rw = g_prect[2], rh = g_prect[3];
    nx_present_rect(rx, ry, rw, rh);
    (void)backbuffer;
    uint32_t s = g_slot++ % CMD_SLOTS;
    dkFenceWait(&g_fence[s], -1); /* this slot's commands, CMD_SLOTS frames back */
    uint64_t t0 = gfx_now_ns();
    int img = dkQueueAcquireImage(g_q, g_sc);
    uint64_t t1 = gfx_now_ns();
    dkCmdBufClear(g_cb);
    dkCmdBufAddMemory(g_cb, g_cmd_mem, s * CMD_SLICE, CMD_SLICE);
    DkImageView rt;
    dkImageViewDefaults(&rt, &g_fb[img]);
    dkCmdBufBindRenderTarget(g_cb, &rt, NULL);
    /* bring-up: a slow pulse of blue, to show frames are presented */
    float pulse = (float)(g_slot % 120) / 120.0f;
    pulse = pulse < 0.5f ? pulse * 2.0f : 2.0f - pulse * 2.0f;
    dkCmdBufClearColorFloat(g_cb, 0, DkColorMask_RGBA, 0.02f, 0.03f, 0.10f + 0.15f * pulse, 1.0f);
    dkCmdBufSignalFence(g_cb, &g_fence[s], false);
    dkQueueSubmitCommands(g_q, dkCmdBufFinishList(g_cb));
    nwindowSetCrop(nwindowGetDefault(), 0, 0, (s32)dw, (s32)dh);
    dkQueuePresentImage(g_q, g_sc, img);
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
        dkQueueWaitIdle(g_q);
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
