/* The graphics back end on OpenGL 4.3 core: gfx.h for the Nintendo Switch (Mesa's nouveau, through EGL
 * from libnx) and, so that it can be built and checked without one, any desktop with SDL3's GL context.
 * The shaders are the generator's GL dialect (gfx_gl_generate). Modelled on gfx_webgpu.c, with
 * gfx_vulkan.c as the reference.
 *
 * Threads: the context is current on one thread, the one that called gfx_init. The game makes buffers
 * and reads its occlusion probes from threads of its own, so the game is built with the render queue
 * (GFX_QUEUE, gfx_queue.c), whose render thread makes the device and makes every call (GFXQ_OWNED).
 * gfx_test calls it directly, from one thread.
 *
 * What GL changes from WebGPU:
 *  - row 0 is D3D's top row in every texture and target: the vertex functions negate clip-space y
 *    (gfx_msl.c), so uploads, targets and reads need no flip, D3D's viewports and scissors are GL's as
 *    they are, and only the present flips (a blit). D3D's clockwise front faces are then GL's
 *    counter-clockwise ones. Clip-space z is D3D's 0..1 with glClipControl (GL 4.5, or the ARB
 *    extension), else the vertex functions move it to -1..1;
 *  - every D3D8 format the game uses is a GL one: BGRA uploads, the packed 16-bit types, swizzles for
 *    X8, L8, A8 and A8L8, S3TC for DXT - nothing is converted on the CPU;
 *  - state is set per draw against a cache of what is set, with one program per key pair; there are
 *    triangle fans, clears of rectangles (the scissor), border colors and copies between textures;
 *  - the vertex streams are texture buffers of words the generated functions read (texelFetch: a texture
 *    load, where Mesa makes storage buffers' reads generic loads, which Ryubing doesn't translate), each
 *    over a whole buffer - a static one's, or the ring's - with the stream's place in it added to
 *    GfxU.offset (as WebGPU binds its streams); the frame's uniforms, vertices and indices go into one
 *    ring, mapped persistently (GL 4.4's buffer storage) when the driver has it, in segments reused once
 *    a fence says the GPU is done with them.
 *
 * The Switch: the device is brought up on the default window through EGL (switch-mesa), its surface
 * 1920x1080 and cropped to the screen's size at each present. Its log lines go out as debug strings
 * (svcOutputDebugString): stdio from a thread other than the main one corrupted the main thread's there.
 *
 * Not yet: the scene effects (gfx_scene_done), the scene renderer, water, the sun's shadows, a
 * program cache file. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SWITCH__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <switch.h>
#include <time.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif
#include <SDL3/SDL.h>

#include "gfx.h"
#include "gfx_fx.h"
#include "gfx_gl_api.h"
#include "gfx_msl.h"
#if defined(__SWITCH__)
#include "nx_bridge.h"
#endif

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

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

#define RING_SEG (8u << 20) /* a segment of the ring: a draw's data fits in one */
#define RING_NSEG 4
#define U_SIZE ((uint32_t)sizeof(GfxU))
#define UNIT0 8 /* the stages' texture units are 8-15 (the generated bindings); 0 is for uploads */

static GlApi gl;

#if defined(__SWITCH__)
#define LOG(...)                                                                                                      \
    do                                                                                                                \
    {                                                                                                                 \
        char log_[1024];                                                                                              \
        int logn_ = snprintf(log_, sizeof log_, __VA_ARGS__);                                                         \
        svcOutputDebugString(log_, (u64)(logn_ < (int)sizeof log_ ? logn_ : (int)sizeof log_ - 1));                   \
    } while (0)
#else
#define LOG(...) (fprintf(stderr, __VA_ARGS__), fflush(stderr))
#endif

/* --- the frame profile (gfx_webgpu.c's) ------------------------------------------------------------------ */
int gfx_profiling;

typedef struct Prof
{
    uint64_t frames, draws, skips, bytes, front_ns, present_ns, since_ns, shim_ns, render_ns, programs;
} Prof;

static Prof g_prof;

/* FFXI_PROFILE: where the render thread's time goes (a draw's own preparation, its GL draw call - where the
 * driver's validation lands - the ring's waits for the GPU, the other calls, the present's blit and swap) and the
 * GL bindings made, by kind: the split line under prof_frame's */
typedef struct RProf
{
    uint64_t prep_ns, call_ns, wait_ns, ring_ns, blit_ns, swap_ns, op_ns[GFX_RT_N];
    uint64_t part_ns[8]; /* the preparation: setup (ring room, target, program, viewport), streams, uniforms, textures, state;
                          * [5] the program's lookup, [6] the ring's room, [7] the target, in the setup */
    uint64_t rd_first_ns, rd_take_ns, rd_poll_ns, rd_queue_ns, rd_first; /* (rd_poll_ns: the fence polls, in the pick-up) */ /* gfx_tex_read_async: a first read, the pick-up, the queuing */
    uint64_t fence_ns, mid_segs; /* the present's ring switch; the segments switched mid-frame */
    uint64_t ring_bytes, units, tex_stream, tex_stage, tex_other, samps, ubos, ebos, progs, fixed, waits;
} RProf;

static RProf g_rp;
static uint64_t g_serial; /* the presents so far */
static uint64_t g_tf_reads, g_pack_reads; /* queued reads by kind (gfx_tex_read_async) */
static double g_ring_mbs, g_mem_mbs;      /* ring_bench */
static uint64_t rp_now(void) { return gfx_profiling ? gfx_now_ns() : 0; }
static void rp_add(uint64_t* ns, uint64_t t0)
{
    if (t0)
        *ns += gfx_now_ns() - t0;
}
void gfx_prof_rt(int kind, uint64_t ns) { g_rp.op_ns[kind] += ns; } /* gfx_queue.c: a recorded call's whole time */

uint64_t gfx_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason)
{
    (void)reason;
    g_prof.skips++;
}
void gfx_prof_shim(uint64_t ns) { g_prof.shim_ns += ns; } /* one thread: the game's, which presents */

#if defined(__SWITCH__)
/* The render thread's stdio would break the main thread's (docs/switch.md): the profile's line is handed to
 * the game's thread, which logs it (gfx_gl_prof_take, from host64.c). One at a time: a line not yet taken
 * keeps its place. */
static char g_prof_line[512];
static uint32_t g_prof_ready;
#define PROF_LOG(...)                                                                                                 \
    do                                                                                                                \
    {                                                                                                                 \
        if (!__atomic_load_n(&g_prof_ready, __ATOMIC_ACQUIRE))                                                        \
        {                                                                                                             \
            snprintf(g_prof_line, sizeof g_prof_line, __VA_ARGS__);                                                   \
            __atomic_store_n(&g_prof_ready, 1, __ATOMIC_RELEASE);                                                     \
        }                                                                                                             \
    } while (0)

/* a second line in a slot of its own (the render thread's split), taken after the first */
static char g_prof_line2[2048];
static uint32_t g_prof_ready2;
#define PROF_LOG2(s)                                                                                                  \
    do                                                                                                                \
    {                                                                                                                 \
        if (!__atomic_load_n(&g_prof_ready2, __ATOMIC_ACQUIRE))                                                       \
        {                                                                                                             \
            snprintf(g_prof_line2, sizeof g_prof_line2, "%s", (s));                                                   \
            __atomic_store_n(&g_prof_ready2, 1, __ATOMIC_RELEASE);                                                    \
        }                                                                                                             \
    } while (0)

int gfx_gl_prof_take(char* out, size_t n)
{
    if (__atomic_load_n(&g_prof_ready, __ATOMIC_ACQUIRE))
    {
        snprintf(out, n, "%s", g_prof_line);
        __atomic_store_n(&g_prof_ready, 0, __ATOMIC_RELEASE);
        return 1;
    }
    if (__atomic_load_n(&g_prof_ready2, __ATOMIC_ACQUIRE))
    {
        snprintf(out, n, "%s", g_prof_line2);
        __atomic_store_n(&g_prof_ready2, 0, __ATOMIC_RELEASE);
        return 1;
    }
    return 0;
}
#else
#define PROF_LOG LOG
#endif

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
    PROF_LOG("[gfx] %.1f fps: frame %.2f ms = game %.2f + d3d %.2f (gl %.2f) + present %.2f | %.0f draws, %.0f skipped, %.0f KB up, "
        "%.0f new programs\n",
        f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - (double)(g_prof.front_ns + g_prof.present_ns) * ms,
        (double)g_prof.front_ns * ms, (double)g_prof.render_ns * ms, (double)g_prof.present_ns * ms, (double)g_prof.draws / f,
        (double)g_prof.skips / f, (double)g_prof.bytes / f / 1024.0, (double)g_prof.programs);
#if defined(__SWITCH__)
    /* the render thread's split (its line handed over like the one above, the first taken) */
    {
        const RProf* r = &g_rp;
        double dr = (double)r->op_ns[GFX_RT_DRAW], dg = (double)(r->prep_ns + r->call_ns);
        static char line[2048];
        snprintf(line, sizeof line,
            "[gfx]   render thread, ms/frame: draws %.2f (decoding %.2f, preparing %.2f of it %.2f waiting for the ring and "
            "%.2f writing %.0f KB into it, the GL draw calls %.2f; preparing: setup %.2f (the ring's room %.2f, the target %.2f, the program's lookup %.2f), streams %.2f, uniforms %.2f, "
            "textures %.2f, state %.2f), characters %.2f, clears %.2f, uploads %.2f, copies %.2f, "
            "reads %.2f (first reads %.1f taking %.2f, picking up %.2f of it polling %.2f, queuing %.2f), calls %.2f, other %.2f, present: "
            "blit %.2f, swap %.2f, the ring's fence %.2f | per frame: active units %.0f, stream "
            "textures %.0f, stage textures %.0f, other textures %.0f, samplers %.0f, uniform ranges %.0f, index buffers %.0f, "
            "programs %.0f, fixed state %.0f, ring waits %.1f (mid-frame %.1f), reads on the GPU %.1f and through a pack buffer %.1f | the "
            "ring's memory takes %.0f MB/s of writes, ordinary memory %.0f\n",
            dr * ms, (dr > dg ? dr - dg : 0) * ms, (double)r->prep_ns * ms, (double)r->wait_ns * ms, (double)r->ring_ns * ms,
            (double)r->ring_bytes / f / 1024.0, (double)r->call_ns * ms, (double)r->part_ns[0] * ms, (double)r->part_ns[6] * ms, (double)r->part_ns[7] * ms,
            (double)r->part_ns[5] * ms,
            (double)r->part_ns[1] * ms, (double)r->part_ns[2] * ms, (double)r->part_ns[3] * ms, (double)r->part_ns[4] * ms,
            (double)r->op_ns[GFX_RT_CHARS] * ms, (double)r->op_ns[GFX_RT_CLEAR] * ms, (double)r->op_ns[GFX_RT_UPLOAD] * ms,
            (double)r->op_ns[GFX_RT_COPY] * ms, (double)r->op_ns[GFX_RT_READ] * ms, (double)r->rd_first / f,
            (double)r->rd_first_ns * ms, (double)r->rd_take_ns * ms, (double)r->rd_poll_ns * ms, (double)r->rd_queue_ns * ms, (double)r->op_ns[GFX_RT_CALL] * ms,
            (double)r->op_ns[GFX_RT_OTHER] * ms, (double)r->blit_ns * ms, (double)r->swap_ns * ms, (double)r->fence_ns * ms, (double)r->units / f,
            (double)r->tex_stream / f, (double)r->tex_stage / f, (double)r->tex_other / f, (double)r->samps / f,
            (double)r->ubos / f, (double)r->ebos / f, (double)r->progs / f, (double)r->fixed / f, (double)r->waits / f, (double)r->mid_segs / f,
            (double)g_tf_reads / f, (double)g_pack_reads / f, g_ring_mbs, g_mem_mbs);
        g_tf_reads = g_pack_reads = 0;
        PROF_LOG2(line);
    }
#endif
    memset(&g_rp, 0, sizeof g_rp);
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since_ns = now;
}

/* --- a byte-keyed map (open addressing; keys are copied) ------------------------------------------------- */
typedef struct Map
{
    uint8_t* keys;
    void** vals;
    uint32_t* hashes;
    uint32_t cap, n, ksize;
} Map;

/* eight bytes a step (a draw's program key is a few hundred bytes, hashed every draw), the rest one at a time */
static uint32_t hash_bytes(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i)
        h = (h ^ b[i]) * 0x100000001b3ull;
    h ^= h >> 32;
    return (uint32_t)h | 1;
}

static void* map_get(Map* m, const void* key)
{
    if (!m->cap)
        return NULL;
    uint32_t h = hash_bytes(key, m->ksize);
    for (uint32_t i = h & (m->cap - 1);; i = (i + 1) & (m->cap - 1))
    {
        if (!m->hashes[i])
            return NULL;
        if (m->hashes[i] == h && !memcmp(m->keys + (size_t)i * m->ksize, key, m->ksize))
            return m->vals[i];
    }
}

static void map_put(Map* m, const void* key, void* val)
{
    if ((m->n + 1) * 2 > m->cap)
    {
        Map o = *m;
        m->cap = o.cap ? o.cap * 2 : 256;
        m->n = 0;
        m->keys = (uint8_t*)malloc((size_t)m->cap * m->ksize);
        m->vals = (void**)calloc(m->cap, sizeof(void*));
        m->hashes = (uint32_t*)calloc(m->cap, 4);
        for (uint32_t i = 0; i < o.cap; ++i)
            if (o.hashes[i])
                map_put(m, o.keys + (size_t)i * o.ksize, o.vals[i]);
        free(o.keys), free(o.vals), free(o.hashes);
    }
    uint32_t h = hash_bytes(key, m->ksize);
    for (uint32_t i = h & (m->cap - 1);; i = (i + 1) & (m->cap - 1))
    {
        if (!m->hashes[i] || (m->hashes[i] == h && !memcmp(m->keys + (size_t)i * m->ksize, key, m->ksize)))
        {
            if (!m->hashes[i])
                m->n++;
            m->hashes[i] = h;
            memcpy(m->keys + (size_t)i * m->ksize, key, m->ksize);
            m->vals[i] = val;
            return;
        }
    }
}

/* --- the context: EGL on the Switch, SDL3's elsewhere ------------------------------------------------------- */
static int g_up;
static int g_vsync;
static volatile uint32_t g_failures;

#if defined(__SWITCH__)
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_surf = EGL_NO_SURFACE;
static EGLContext g_ctx = EGL_NO_CONTEXT;

static void* gl_proc(const char* name) { return (void*)eglGetProcAddress(name); }

/* the default window (the screen), its surface 1920x1080 and cropped to the screen's size each frame */
static int ctx_up(void* win, int debug)
{
    (void)win;
    /* not MESA_NO_ERROR: a call Mesa would turn down then crashes it (a DXT upload did, on a console) */
    (void)debug;
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, NULL, NULL))
    {
        LOG("[recomp] gfx: no EGL display (0x%x)\n", eglGetError());
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API))
    {
        LOG("[recomp] gfx: EGL has no desktop OpenGL (0x%x)\n", eglGetError());
        return 0;
    }
    static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &ncfg) || !ncfg)
    {
        LOG("[recomp] gfx: no EGL config (0x%x)\n", eglGetError());
        return 0;
    }
    NWindow* nw = nwindowGetDefault();
    nwindowSetDimensions(nw, 1920, 1080);
    g_surf = eglCreateWindowSurface(g_dpy, cfg, (EGLNativeWindowType)nw, NULL);
    if (g_surf == EGL_NO_SURFACE)
    {
        LOG("[recomp] gfx: no EGL window surface (0x%x)\n", eglGetError());
        return 0;
    }
    const EGLint ctx_attr[] = { EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_CONTEXT_FLAGS_KHR,
        debug ? EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR : 0, EGL_NONE };
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx))
    {
        LOG("[recomp] gfx: no OpenGL 4.3 core context (0x%x)\n", eglGetError());
        return 0;
    }
    return 1;
}

static int ctx_window(void* win)
{
    (void)win;
    return 1;
}
static void ctx_interval(int vsync) { eglSwapInterval(g_dpy, vsync ? 1 : 0); }
/* the part of the surface on the screen: the screen's size (sdl_nx.c's window: 1280x720 handheld, 1920x1080
 * docked), cropped from its top left */
static void ctx_drawable(int* w, int* h)
{
    *w = 1280, *h = 720;
    SDL_GetWindowSizeInPixels(NULL, w, h);
    if (*w <= 0 || *w > 1920 || *h <= 0 || *h > 1080)
        *w = 1280, *h = 720;
}
static void ctx_swap(int w, int h)
{
    nwindowSetCrop(nwindowGetDefault(), 0, 0, w, h);
    eglSwapBuffers(g_dpy, g_surf);
}
/* the surface's height, which GL's window coordinates count up from (the crop is at its top) */
static int ctx_surface_h(void) { return 1080; }
uint64_t gfx_window_flags(void) { return 0; }
#else
static SDL_Window* g_win;     /* the window presented to; NULL drawing offscreen (gfx_test) */
static SDL_Window* g_own_win; /* a hidden one made for a context without a window */
static SDL_GLContext g_ctx;

static void* gl_proc(const char* name) { return (void*)SDL_GL_GetProcAddress(name); }

static void ctx_attributes(int debug)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0); /* the window's own buffer is only presented to */
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

static int ctx_up(void* win, int debug)
{
    SDL_Window* w = (SDL_Window*)win;
    if (!w)
    {
        if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            LOG("[recomp] gfx: no video: %s\n", SDL_GetError());
            return 0;
        }
        ctx_attributes(debug);
        w = g_own_win = SDL_CreateWindow("gfx", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!w)
        {
            LOG("[recomp] gfx: no window for a context: %s\n", SDL_GetError());
            return 0;
        }
    }
    ctx_attributes(debug);
    g_ctx = SDL_GL_CreateContext(w);
    if (!g_ctx)
    {
        LOG("[recomp] gfx: no OpenGL 4.3 core context: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_MakeCurrent(w, g_ctx);
    g_win = win ? (SDL_Window*)win : NULL;
    return 1;
}

/* a second device on another window (the sign-in screen's, then the game's): the same context on it */
static int ctx_window(void* win)
{
    if (!win || win == g_win)
        return 1;
    if (!SDL_GL_MakeCurrent((SDL_Window*)win, g_ctx))
    {
        LOG("[recomp] gfx: the context won't move to the new window: %s\n", SDL_GetError());
        return 0;
    }
    g_win = (SDL_Window*)win;
    return 1;
}
static void ctx_interval(int vsync) { SDL_GL_SetSwapInterval(vsync ? 1 : 0); }
static void ctx_drawable(int* w, int* h)
{
    *w = *h = 0;
    if (g_win)
        SDL_GetWindowSizeInPixels(g_win, w, h);
}
static void ctx_swap(int w, int h)
{
    (void)w, (void)h;
    if (g_win)
        SDL_GL_SwapWindow(g_win);
}
static int ctx_surface_h(void)
{
    int w, h;
    ctx_drawable(&w, &h);
    return h;
}
/* the window's pixel format is chosen when it is made, so the attributes are set now (SDL is up: the
 * caller is about to make the window) */
uint64_t gfx_window_flags(void)
{
    const char* dbg = getenv("FFXI_GL_DEBUG");
    if (SDL_WasInit(SDL_INIT_VIDEO))
        ctx_attributes(dbg && dbg[0] && dbg[0] != '0');
    return SDL_WINDOW_OPENGL;
}
#endif

/* --- the state set, cached ---------------------------------------------------------------------------------- */
typedef struct BufBind
{
    GLuint buf;
    GLintptr off;
    GLsizeiptr size;
} BufBind;

typedef struct GlState
{
    int blend, depth_test, stencil_test, cull, poly_offset, scissor;
    GLenum bsrc, bdst, basrc, badst, bop;
    int cmask;
    GLboolean depth_write;
    GLenum depth_func;
    GLenum sfunc, sfail, szfail, spass;
    GLint sref;
    GLuint sread, swrite;
    GLenum cull_face, poly_mode;
    float po_factor, po_units;
    GLint sc[4], vp[4];
    float dr[2];
    GLuint program, draw_fbo, read_fbo, ebo;
    BufBind ubo;
    GLuint tex[16], samp[16];
    GLenum tex_target[16];
    int unit;
} GlState;

static GlState S;

static void st_reset(void)
{
    memset(&S, 0xFF, sizeof S); /* nothing known: every first set is made */
    S.unit = -1;
}

static void st_cap(GLenum cap, int* have, int on)
{
    on = !!on;
    if (*have != on)
        (on ? gl.Enable : gl.Disable)(cap), *have = on, g_rp.fixed++;
}

static void st_unit(int u)
{
    if (S.unit != u)
        gl.ActiveTexture(GL_TEXTURE0 + (GLenum)u), S.unit = u, g_rp.units++;
}

/* the texture bound on unit u, and u the active unit: uploads, reads and parameters act on the active unit's
 * binding, so a binding already in place still makes its unit active (else an upload went to another unit's
 * texture: DXT uploads failed on a console, a menu's texture was never rewritten) */
static void st_texture(int u, GLenum target, GLuint name)
{
    st_unit(u);
    if (S.tex[u] == name && S.tex_target[u] == target)
        return;
    gl.BindTexture(target, name); /* a unit keeps one per target: a stage samples only its own kind */
    S.tex[u] = name, S.tex_target[u] = target;
    if (u >= 1 && u <= GFX_NSTREAMS)
        g_rp.tex_stream++;
    else if (u >= UNIT0)
        g_rp.tex_stage++;
    else
        g_rp.tex_other++;
}

static void st_sampler(int u, GLuint s)
{
    if (S.samp[u] != s)
        gl.BindSampler((GLuint)u, s), S.samp[u] = s, g_rp.samps++;
}

static void st_draw_fbo(GLuint f)
{
    if (S.draw_fbo != f)
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, f), S.draw_fbo = f;
}

static void st_read_fbo(GLuint f)
{
    if (S.read_fbo != f)
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, f), S.read_fbo = f;
}

static void st_cmask(int m)
{
    if (S.cmask != m)
        gl.ColorMask(m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1), S.cmask = m, g_rp.fixed++;
}

static void st_depth_write(GLboolean w)
{
    if (S.depth_write != w)
        gl.DepthMask(w), S.depth_write = w, g_rp.fixed++;
}

static void st_stencil_write(GLuint m)
{
    if (S.swrite != m)
        gl.StencilMask(m), S.swrite = m, g_rp.fixed++;
}

static void st_scissor(int on, GLint x, GLint y, GLint w, GLint h)
{
    st_cap(GL_SCISSOR_TEST, &S.scissor, on);
    if (on && (S.sc[0] != x || S.sc[1] != y || S.sc[2] != w || S.sc[3] != h))
    {
        gl.Scissor(x, y, w, h);
        S.sc[0] = x, S.sc[1] = y, S.sc[2] = w, S.sc[3] = h;
        g_rp.fixed++;
    }
}

static void st_buffer(GLenum target, GLuint index, BufBind* have, GLuint buf, GLintptr off, GLsizeiptr size)
{
    if (have->buf == buf && have->off == off && have->size == size)
        return;
    gl.BindBufferRange(target, index, buf, off, size);
    have->buf = buf, have->off = off, have->size = size;
    g_rp.ubos++;
}

static void st_ebo(GLuint b)
{
    if (S.ebo != b)
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, b), S.ebo = b, g_rp.ebos++;
}

static void st_program(GLuint p)
{
    if (S.program != p)
        gl.UseProgram(p), S.program = p, g_rp.progs++;
}

/* a buffer is going: GL unbinds it everywhere, so the cache forgets it */
static void st_forget_buffer(GLuint b)
{
    if (S.ubo.buf == b)
        memset(&S.ubo, 0xFF, sizeof S.ubo);
    if (S.ebo == b)
        S.ebo = ~0u;
}

static void st_forget_texture(GLuint t)
{
    for (int u = 0; u < 16; ++u)
        if (S.tex[u] == t)
            S.tex[u] = ~0u, S.tex_target[u] = ~0u;
}

/* --- device limits and extensions --------------------------------------------------------------------------- */
static GLint g_ubo_align = 256;
static int g_has_s3tc, g_has_mirror_once, g_clip01;
static float g_max_aniso;

static int has_ext(const char* name)
{
    GLint n = 0;
    gl.GetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i)
    {
        const char* e = (const char*)gl.GetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, name))
            return 1;
    }
    return 0;
}

static void GLAPI_CALL on_debug(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* msg,
    const void* user)
{
    (void)source, (void)type, (void)id, (void)length, (void)user;
    static int said;
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION || said >= 60)
        return;
    said++;
    LOG("[recomp] gfx: GL %s: %s\n", severity == GL_DEBUG_SEVERITY_HIGH ? "error" : "warning", msg);
}

/* --- textures -------------------------------------------------------------------------------------------- */
typedef struct AsyncSlot
{
    GLuint pbo;
    GLsync fence;
    uint32_t serial;
    const void* map; /* a GPU read's buffer, mapped for good (read when its fence has passed), or NULL */
    uint64_t frame;  /* g_serial when it was queued */
    int pending;     /* a GPU read without a fence queued, its copy not taken yet */
} AsyncSlot;

typedef struct AsyncRead
{
    uint32_t face, level, size, serial, have_serial;
    AsyncSlot slot[3];
    uint8_t* last; /* the newest finished copy, tightly packed */
} AsyncRead;

struct GfxTex
{
    GLuint name;
    GLenum target;     /* GL_TEXTURE_2D or GL_TEXTURE_CUBE_MAP */
    GLenum ifmt;       /* its internal format */
    GLenum ufmt, utype; /* how D3D's texels are given and read back (compressed: ufmt 0) */
    int type, use, has_stencil, dropped;
    uint32_t fmt, w, h, levels, layers, block, bpp, id;
    AsyncRead* rd;
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

/* the GL format for a D3DFORMAT: its internal format, how texels are given, and the swizzle that makes
 * the channels D3D's (sw NULL: as stored) */
static GLenum tex_format(GfxTex* t, const GLint** sw)
{
    static const GLint X8[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ONE };
    static const GLint A8[4] = { GL_ZERO, GL_ZERO, GL_ZERO, GL_RED };
    static const GLint L8[4] = { GL_RED, GL_RED, GL_RED, GL_ONE };
    static const GLint A8L8[4] = { GL_RED, GL_RED, GL_RED, GL_GREEN };
    *sw = NULL;
    t->ufmt = GL_BGRA, t->utype = GL_UNSIGNED_BYTE, t->bpp = 4, t->block = 0;
    if (t->use == GFX_USE_DEPTH)
    {
        t->ufmt = 0;
        if (t->fmt == F_D16)
            return t->bpp = 2, GL_DEPTH_COMPONENT16;
        t->has_stencil = 1;
        return GL_DEPTH24_STENCIL8;
    }
    switch (t->fmt)
    {
    case F_A8R8G8B8: return GL_RGBA8;
    case F_X8R8G8B8: *sw = X8; return GL_RGBA8;
    case F_R5G6B5: t->ufmt = GL_RGB, t->utype = GL_UNSIGNED_SHORT_5_6_5, t->bpp = 2; return GL_RGB565;
    case F_X1R5G5B5: *sw = X8; t->utype = GL_UNSIGNED_SHORT_1_5_5_5_REV, t->bpp = 2; return GL_RGB5_A1;
    case F_A1R5G5B5: t->utype = GL_UNSIGNED_SHORT_1_5_5_5_REV, t->bpp = 2; return GL_RGB5_A1;
    case F_A4R4G4B4: t->utype = GL_UNSIGNED_SHORT_4_4_4_4_REV, t->bpp = 2; return GL_RGBA4;
    case F_A8: *sw = A8; t->ufmt = GL_RED, t->bpp = 1; return GL_R8;
    case F_L8: *sw = L8; t->ufmt = GL_RED, t->bpp = 1; return GL_R8;
    case F_A8L8: *sw = A8L8; t->ufmt = GL_RG, t->bpp = 2; return GL_RG8;
    case F_V8U8: t->ufmt = GL_RG, t->utype = GL_BYTE, t->bpp = 2; return GL_RG8_SNORM;
    }
    GLenum bc = 0;
    if (t->fmt == FOURCC('D', 'X', 'T', '1'))
        t->block = 8, bc = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    else if (t->fmt == FOURCC('D', 'X', 'T', '2') || t->fmt == FOURCC('D', 'X', 'T', '3'))
        t->block = 16, bc = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    else if (t->fmt == FOURCC('D', 'X', 'T', '4') || t->fmt == FOURCC('D', 'X', 'T', '5'))
        t->block = 16, bc = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    if (bc)
    {
        if (g_has_s3tc)
            return t->ufmt = 0, bc;
        t->block = 0, t->dropped = 1; /* no S3TC: left as made */
    }
    return GL_RGBA8;
}

static GLenum face_target(const GfxTex* t, uint32_t face)
{
    return t->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
}

GfxTex* gfx_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_up || !w || !h)
        return NULL;
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    t->type = type, t->use = use, t->fmt = d3dfmt, t->w = w, t->h = h, t->id = ++g_tex_ids;
    const GLint* sw;
    t->ifmt = tex_format(t, &sw);
    uint32_t maxl = 1;
    while ((w >> maxl) || (h >> maxl))
        maxl++;
    t->levels = levels && levels < maxl ? levels : maxl;
    if (use != GFX_USE_SAMPLE)
        t->levels = 1;
    t->layers = type == GFX_TEX_CUBE ? 6 : 1;
    t->target = type == GFX_TEX_CUBE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    gl.GenTextures(1, &t->name);
    st_texture(0, t->target, t->name);
    gl.TexStorage2D(t->target, (GLsizei)t->levels, t->ifmt, (GLsizei)w, (GLsizei)h);
    gl.TexParameteri(t->target, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
    if (sw)
    {
        gl.TexParameteri(t->target, GL_TEXTURE_SWIZZLE_R, sw[0]);
        gl.TexParameteri(t->target, GL_TEXTURE_SWIZZLE_G, sw[1]);
        gl.TexParameteri(t->target, GL_TEXTURE_SWIZZLE_B, sw[2]);
        gl.TexParameteri(t->target, GL_TEXTURE_SWIZZLE_A, sw[3]);
    }
    return t;
}

/* --- framebuffers: one per color target (a level of a face) and depth surface, kept ----------------------- */
typedef struct Fbo
{
    GLuint fbo;
    uint32_t color, depth; /* GfxTex ids (0 none) */
    uint32_t face, level, used;
} Fbo;

#define NFBO 64
static Fbo g_fbos[NFBO];
static uint32_t g_fbo_clock;

static void fbo_attach(GLenum point, GfxTex* t, uint32_t face, uint32_t level)
{
    gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, point, t ? face_target(t, face) : GL_TEXTURE_2D, t ? t->name : 0, (GLint)level);
}

static GLuint fbo_for(GfxTex* c, uint32_t face, uint32_t level, GfxTex* d)
{
    uint32_t cid = c ? c->id : 0, did = d ? d->id : 0;
    Fbo* lru = &g_fbos[0];
    for (int i = 0; i < NFBO; ++i)
    {
        Fbo* f = &g_fbos[i];
        if (f->fbo && f->color == cid && f->depth == did && f->face == face && f->level == level)
        {
            f->used = ++g_fbo_clock;
            return f->fbo;
        }
        if (!f->fbo || f->used < lru->used)
            lru = f;
    }
    if (lru->fbo)
    {
        if (S.draw_fbo == lru->fbo)
            S.draw_fbo = ~0u;
        if (S.read_fbo == lru->fbo)
            S.read_fbo = ~0u;
        gl.DeleteFramebuffers(1, &lru->fbo);
    }
    memset(lru, 0, sizeof *lru);
    gl.GenFramebuffers(1, &lru->fbo);
    lru->color = cid, lru->depth = did, lru->face = face, lru->level = level, lru->used = ++g_fbo_clock;
    st_draw_fbo(lru->fbo);
    if (c)
        fbo_attach(GL_COLOR_ATTACHMENT0, c, face, level);
    else
        gl.DrawBuffer(GL_NONE);
    if (d)
        fbo_attach(d->has_stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, d, 0, 0);
    GLenum st = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
    {
        static int said;
        if (said++ < 10)
            LOG("[recomp] gfx: a framebuffer is incomplete (0x%x): color format 0x%x, depth 0x%x\n", st, c ? c->ifmt : 0,
                d ? d->ifmt : 0);
    }
    return lru->fbo;
}

static void fbo_forget(uint32_t id)
{
    for (int i = 0; i < NFBO; ++i)
        if (g_fbos[i].fbo && (g_fbos[i].color == id || g_fbos[i].depth == id))
        {
            if (S.draw_fbo == g_fbos[i].fbo)
                S.draw_fbo = ~0u;
            if (S.read_fbo == g_fbos[i].fbo)
                S.read_fbo = ~0u;
            gl.DeleteFramebuffers(1, &g_fbos[i].fbo);
            memset(&g_fbos[i], 0, sizeof g_fbos[i]);
        }
}

static void color_size(uint32_t* w, uint32_t* h)
{
    *w = g_rt ? level_dim(g_rt->w, g_rt_level) : 1;
    *h = g_rt ? level_dim(g_rt->h, g_rt_level) : 1;
}

/* the depth surface the draws get: the game's when it covers the target (D3D draws a smaller target with
 * a larger depth surface; GL then draws the part both cover, which is all of the target) */
static GfxTex* depth_attachment(void)
{
    uint32_t w, h;
    color_size(&w, &h);
    if (!g_ds || g_ds->w < w || g_ds->h < h)
        return NULL;
    return g_ds;
}

/* the current targets bound to draw to; 0 when there is no color target */
static int bind_target(void)
{
    if (!g_rt)
        return 0;
    st_draw_fbo(fbo_for(g_rt, g_rt_face, g_rt_level, depth_attachment()));
    return 1;
}

void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
}

static void async_free(GfxTex* t);

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (g_rt == t)
        g_rt = NULL;
    if (g_ds == t)
        g_ds = NULL;
    fbo_forget(t->id);
    async_free(t);
    st_forget_texture(t->name);
    gl.DeleteTextures(1, &t->name);
    free(t);
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels || face >= t->layers || t->dropped || t->use == GFX_USE_DEPTH || !w || !h)
        return;
    GLenum target = face_target(t, face);
    st_texture(0, t->target, t->name);
    uint32_t lw = level_dim(t->w, level), lh = level_dim(t->h, level);
    if (x >= lw || y >= lh)
        return;
    if (x + w > lw)
        w = lw - x;
    if (y + h > lh)
        h = lh - y;
    if (t->block)
    {
        uint32_t rows = (h + 3) / 4, row = (w + 3) / 4 * t->block;
        const uint8_t* s = (const uint8_t*)src;
        uint8_t* tmp = NULL;
        if (pitch != row) /* the blocks given tightly */
        {
            tmp = (uint8_t*)malloc((size_t)row * rows);
            for (uint32_t r = 0; r < rows; ++r)
                memcpy(tmp + (size_t)r * row, s + (size_t)r * pitch, row);
            s = tmp;
        }
        gl.CompressedTexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, t->ifmt,
            (GLsizei)(row * rows), s);
        g_prof.bytes += (uint64_t)row * rows;
        free(tmp);
        return;
    }
    if (pitch % t->bpp == 0)
    {
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / t->bpp));
        gl.TexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, t->ufmt, t->utype, src);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    else
    {
        uint32_t row = w * t->bpp;
        uint8_t* tmp = (uint8_t*)malloc((size_t)row * h);
        for (uint32_t r = 0; r < h; ++r)
            memcpy(tmp + (size_t)r * row, (const uint8_t*)src + (size_t)r * pitch, row);
        gl.TexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, t->ufmt, t->utype, tmp);
        free(tmp);
    }
    g_prof.bytes += (uint64_t)w * h * t->bpp;
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (t)
        gfx_tex_upload_rect(t, face, level, 0, 0, level_dim(t->w, level), level_dim(t->h, level), src, pitch);
}

/* --- buffers ------------------------------------------------------------------------------------------- */
struct GfxBuf
{
    GLuint name;
    GLuint tbo;    /* a texture buffer of its words, for the vertex functions (made at its first draw) */
    uint32_t size; /* as made: a multiple of 4 */
};

/* a texture buffer of 32-bit words over a whole buffer: a stream the generated functions read (texelFetch) */
static GLuint words_texture(GLuint buf)
{
    GLuint t;
    gl.GenTextures(1, &t);
    st_texture(0, GL_TEXTURE_BUFFER, t);
    gl.TexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, buf);
    return t;
}

static GLuint buf_texture(GfxBuf* b)
{
    if (!b->tbo)
        b->tbo = words_texture(b->name);
    return b->tbo;
}

GfxBuf* gfx_buf_create(uint32_t size)
{
    if (!g_up || !size)
        return NULL;
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    b->size = (size + 3) & ~3u;
    gl.GenBuffers(1, &b->name);
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, b->name);
    gl.BufferData(GL_COPY_WRITE_BUFFER, b->size, NULL, GL_DYNAMIC_DRAW);
    return b;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    st_forget_buffer(b->name);
    if (b->tbo)
    {
        st_forget_texture(b->tbo);
        gl.DeleteTextures(1, &b->tbo);
    }
    gl.DeleteBuffers(1, &b->name);
    free(b);
}

/* the whole buffer: into fresh storage (the draws already made keep reading the old) */
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b || !size)
        return;
    if (size > b->size)
        size = b->size;
    if (getenv("FFXI_GL_DEBUG") && gl.IsBuffer && !gl.IsBuffer(b->name))
    {
        static int said;
        if (said++ < 10)
            LOG("[recomp] gfx: an upload to buffer %p, whose name %u is not a buffer: made on a thread without the "
                "context?\n", (void*)b, b->name);
    }
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, b->name);
    if (size + 3 >= b->size)
        gl.BufferData(GL_COPY_WRITE_BUFFER, b->size, NULL, GL_DYNAMIC_DRAW);
    gl.BufferSubData(GL_COPY_WRITE_BUFFER, 0, size, data);
    g_prof.bytes += size;
}

/* --- the ring: the frame's uniforms, vertices and indices ----------------------------------------------------- */
static GLuint g_ring;
static uint8_t* g_ring_cpu;
static int g_ring_mapped; /* persistently mapped; else written through glBufferSubData */
static uint32_t g_seg, g_seg_used;
static GLsync g_seg_fence[RING_NSEG];

/* FFXI_PROFILE: how fast the ring's memory takes the CPU's writes, against ordinary memory's (the split line), measured
 * once while nothing uses it: the draws' preparation is mostly writes into it */
static void ring_bench(void)
{
    const char* e = getenv("FFXI_PROFILE");
    if (!(e && *e && *e != '0') || !g_ring_cpu)
        return;
    const size_t n = 1u << 20;
    uint8_t *src = (uint8_t*)malloc(n), *dst = (uint8_t*)malloc(n);
    if (!src || !dst)
    {
        free(src), free(dst);
        return;
    }
    memset(src, 0x5A, n), memset(dst, 0, n);
    for (int k = 0; k < 2; ++k)
    {
        uint8_t* to = k ? dst : g_ring_cpu;
        uint64_t t0 = gfx_now_ns();
        volatile uint8_t sink = 0;
        for (int i = 0; i < 4; ++i)
            memcpy(to, src, n), sink = to[n - 1 - (size_t)i];
        (void)sink;
        uint64_t ns = gfx_now_ns() - t0;
        *(k ? &g_mem_mbs : &g_ring_mbs) = ns ? 4.0 * (double)n / (1024.0 * 1024.0) / ((double)ns * 1e-9) : 0;
    }
    free(src), free(dst);
}

static void ring_make(void)
{
    gl.GenBuffers(1, &g_ring);
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, g_ring);
    const char* env = getenv("FFXI_GL_RING_MAP");
    if (gl.BufferStorage && !(env && env[0] == '0'))
    {
        GLbitfield f = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        gl.BufferStorage(GL_COPY_WRITE_BUFFER, (GLsizeiptr)RING_SEG * RING_NSEG, NULL, f);
        g_ring_cpu = (uint8_t*)gl.MapBufferRange(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)RING_SEG * RING_NSEG, f);
        g_ring_mapped = g_ring_cpu != NULL;
    }
    if (!g_ring_mapped)
    {
        if (gl.BufferStorage) /* storage is immutable once given: a buffer of its own */
        {
            gl.DeleteBuffers(1, &g_ring);
            gl.GenBuffers(1, &g_ring);
            gl.BindBuffer(GL_COPY_WRITE_BUFFER, g_ring);
        }
        gl.BufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)RING_SEG * RING_NSEG, NULL, GL_STREAM_DRAW);
        g_ring_cpu = (uint8_t*)malloc((size_t)RING_SEG * RING_NSEG);
    }
    ring_bench();
}

/* the next segment, once the GPU is done with it. The fence behind the current one flushes the work queued so
 * far, which on Mesa's nouveau costs 5-7 ms with a frame's worth queued: so a segment a frame, the switch made
 * just after the present (gfx_present: little queued), and mid-frame only when a frame fills one. */
static void ring_next(void)
{
    g_seg_fence[g_seg] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    g_seg = (g_seg + 1) % RING_NSEG;
    g_seg_used = 0;
    if (g_seg_fence[g_seg])
    {
        uint64_t tw = rp_now();
        g_rp.waits++;
        GLbitfield flags = GL_SYNC_FLUSH_COMMANDS_BIT;
        for (;;)
        {
            GLenum r = gl.ClientWaitSync(g_seg_fence[g_seg], flags, 1000000000ull);
            if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED || r == GL_WAIT_FAILED)
                break;
            flags = 0;
        }
        gl.DeleteSync(g_seg_fence[g_seg]);
        g_seg_fence[g_seg] = NULL;
        rp_add(&g_rp.wait_ns, tw);
    }
}

/* room for n bytes of one draw in the current segment: the next one when it has not (waiting for the GPU to
 * be done with it). 0 when n is more than a segment. */
static int ring_reserve(uint32_t n)
{
    if (n > RING_SEG)
        return 0;
    if (g_seg_used + n > RING_SEG)
        ring_next(), g_rp.mid_segs++;
    return 1;
}

/* n bytes at an alignment (a power of two) in the reserved room: the offset in the ring; written when p */
static uint32_t ring_put(const void* p, uint32_t n, uint32_t align)
{
    uint32_t at = (g_seg_used + align - 1) & ~(align - 1);
    g_seg_used = at + ((n + 3) & ~3u);
    at += g_seg * RING_SEG;
    if (p)
    {
        uint64_t tr = rp_now();
        memcpy(g_ring_cpu + at, p, n);
        rp_add(&g_rp.ring_ns, tr);
        g_rp.ring_bytes += n;
        if (!g_ring_mapped)
        {
            gl.BindBuffer(GL_COPY_WRITE_BUFFER, g_ring);
            gl.BufferSubData(GL_COPY_WRITE_BUFFER, at, n, p);
        }
    }
    g_prof.bytes += n;
    return at;
}

/* --- samplers ------------------------------------------------------------------------------------------- */
static Map g_samplers = { .ksize = sizeof(GfxSampler) };

static GLint address(uint32_t a)
{
    switch (a)
    {
    case 2: return GL_MIRRORED_REPEAT;
    case 3: return GL_CLAMP_TO_EDGE;
    case 4: return GL_CLAMP_TO_BORDER;
    case 5: return g_has_mirror_once ? GL_MIRROR_CLAMP_TO_EDGE : GL_MIRRORED_REPEAT;
    default: return GL_REPEAT;
    }
}

static GLuint sampler(const GfxSampler* k)
{
    GLuint s = (GLuint)(uintptr_t)map_get(&g_samplers, k);
    if (s)
        return s;
    gl.GenSamplers(1, &s);
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_S, address(k->addr_u));
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_T, address(k->addr_v));
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_R, address(k->addr_w));
    gl.SamplerParameteri(s, GL_TEXTURE_MAG_FILTER, k->mag >= 2 ? GL_LINEAR : GL_NEAREST);
    int lin = k->min >= 2;
    GLint min = lin ? GL_LINEAR : GL_NEAREST;
    if (k->mip == 1)
        min = lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else if (k->mip >= 2)
        min = lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    gl.SamplerParameteri(s, GL_TEXTURE_MIN_FILTER, min);
    if (k->mip)
    {
        gl.SamplerParameterf(s, GL_TEXTURE_MIN_LOD, (float)k->max_level);
        float maxl = k->lod_cap ? (float)(k->lod_cap - 1) : 1000.0f;
        gl.SamplerParameterf(s, GL_TEXTURE_MAX_LOD, maxl < (float)k->max_level ? (float)k->max_level : maxl);
    }
    if ((k->min == 3 || k->mag == 3) && k->max_aniso > 1 && g_max_aniso > 1.0f && k->mip >= 2 && lin)
        gl.SamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY, (float)k->max_aniso < g_max_aniso ? (float)k->max_aniso : g_max_aniso);
    uint32_t c = k->border;
    float bc[4] = { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, (c >> 24) / 255.0f };
    gl.SamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, bc);
    map_put(&g_samplers, k, (void*)(uintptr_t)s);
    return s;
}

/* --- programs, one per key pair ------------------------------------------------------------------------------- */
typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

static Map g_progs = { .ksize = sizeof(LibKey) };
#define PROG_FAILED ((void*)1)
static int g_fail_said;

static GLuint compile(GLenum stage, const char* text)
{
    const char* src[3] = { "#version 430 core\n",
        stage == GL_VERTEX_SHADER ? (g_clip01 ? "#define GFX_VS\n#define GFX_GL_CLIP01\n" : "#define GFX_VS\n") : "#define GFX_FS\n",
        text };
    GLuint sh = gl.CreateShader(stage);
    gl.ShaderSource(sh, 3, src, NULL);
    gl.CompileShader(sh);
    GLint ok = 0;
    gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        if (g_fail_said++ < 4)
        {
            char log[4096] = "";
            gl.GetShaderInfoLog(sh, sizeof log, NULL, log);
            LOG("[recomp] gfx: a %s function did not compile:\n%s\n--- its text ---\n%s\n---\n",
                stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log, text);
        }
        gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint program_from(const char* text)
{
    GLuint vs = compile(GL_VERTEX_SHADER, text);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, text) : 0;
    if (!fs)
    {
        if (vs)
            gl.DeleteShader(vs);
        return 0;
    }
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs);
    gl.AttachShader(p, fs);
    gl.LinkProgram(p);
    gl.DetachShader(p, vs), gl.DetachShader(p, fs);
    gl.DeleteShader(vs), gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        if (g_fail_said++ < 4)
        {
            char log[4096] = "";
            gl.GetProgramInfoLog(p, sizeof log, NULL, log);
            LOG("[recomp] gfx: a program did not link:\n%s\n--- its text ---\n%s\n---\n", log, text);
        }
        gl.DeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint program_for(const LibKey* k, const uint32_t* vs, const uint32_t* ps)
{
    /* the last two keys asked for (most draws repeat the one before; a per-pixel draw asks for two) */
    static LibKey last[2];
    static GLuint last_p[2];
    static int last_ok[2], last_next;
    for (int i = 0; i < 2; ++i)
        if (last_ok[i] && !memcmp(&last[i], k, sizeof *k))
            return last_p[i];
    GLuint got = 0;
    int known = 0;
    void* v = map_get(&g_progs, k);
    if (v)
        got = v == PROG_FAILED ? 0 : (GLuint)(uintptr_t)v, known = 1;
    if (known)
    {
        last[last_next] = *k, last_p[last_next] = got, last_ok[last_next] = 1, last_next ^= 1;
        return got;
    }
    char* src = gfx_gl_generate(&k->vs, &k->fs, vs, ps);
    GLuint p = src ? program_from(src) : 0;
    free(src);
    g_prof.programs++;
    if (!p)
        g_failures++;
    map_put(&g_progs, k, p ? (void*)(uintptr_t)p : PROG_FAILED);
    return p;
}

static GLuint program(const GfxDraw* d)
{
    LibKey k;
    memset(&k, 0, sizeof k);
    k.vs = d->vs, k.fs = d->fs;
    k.vs.pixel = k.vs.shadow = k.vs.water = 0; /* the scene effects' variants are not drawn here yet */
    k.fs.water = 0;
    /* the world's lit draws lit per pixel (gfx_msl.c pixel_lit), by the vertex's light when that fails */
    if (g_fxs.fx != 0.0f && g_fxs.light != 0.0f && d->vs.lighting && !d->vs.rhw && !d->vs.prog && !d->vs.flat)
    {
        k.vs.pixel = g_fxs.light >= 2.0f ? 2 : 1;
        GLuint p = program_for(&k, d->vs_tokens, d->ps_tokens);
        if (p)
            return p;
        k.vs.pixel = 0;
    }
    return program_for(&k, d->vs_tokens, d->ps_tokens);
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

/* --- fixed-function state ------------------------------------------------------------------------------------- */
static GLenum blend_factor(uint32_t f, int x8)
{
    switch (f)
    {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return GL_SRC_COLOR;
    case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return x8 ? GL_ONE : GL_DST_ALPHA;
    case 8: return x8 ? GL_ZERO : GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR;
    case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    default: return GL_ONE;
    }
}

static GLenum blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return GL_FUNC_SUBTRACT;
    case 3: return GL_FUNC_REVERSE_SUBTRACT;
    case 4: return GL_MIN;
    case 5: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

static GLenum compare(uint32_t f)
{
    switch (f)
    {
    case 1: return GL_NEVER;
    case 2: return GL_LESS;
    case 3: return GL_EQUAL;
    case 4: return GL_LEQUAL;
    case 5: return GL_GREATER;
    case 6: return GL_NOTEQUAL;
    case 7: return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

static GLenum stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return GL_ZERO;
    case 3: return GL_REPLACE;
    case 4: return GL_INCR;
    case 5: return GL_DECR;
    case 6: return GL_INVERT;
    case 7: return GL_INCR_WRAP;
    case 8: return GL_DECR_WRAP;
    default: return GL_KEEP;
    }
}

static void set_blend(const GfxPipeKey* p, int x8)
{
    st_cmask(p->write_mask & 15);
    st_cap(GL_BLEND, &S.blend, p->blend);
    if (!p->blend)
        return;
    uint32_t sf = p->src, df = p->dst;
    if (sf == 12) /* BOTHSRCALPHA */
        sf = 5, df = 6;
    else if (sf == 13) /* BOTHINVSRCALPHA */
        sf = 6, df = 5;
    GLenum s = blend_factor(sf, x8), d = blend_factor(df, x8);
    GLenum as = s == GL_SRC_ALPHA_SATURATE ? GL_ONE : s, ad = d == GL_SRC_ALPHA_SATURATE ? GL_ONE : d;
    if (S.bsrc != s || S.bdst != d || S.basrc != as || S.badst != ad)
    {
        gl.BlendFuncSeparate(s, d, as, ad), g_rp.fixed++;
        S.bsrc = s, S.bdst = d, S.basrc = as, S.badst = ad;
    }
    GLenum op = blend_op(p->op); /* MIN and MAX ignore the factors, as D3D's do */
    if (S.bop != op)
        gl.BlendEquation(op), S.bop = op, g_rp.fixed++;
}

static void set_depth(const GfxDraw* d, const GfxTex* depth)
{
    const GfxDepthKey* k = &d->depth;
    int zt = depth && k->zenable;
    st_cap(GL_DEPTH_TEST, &S.depth_test, zt);
    if (zt)
    {
        GLenum f = compare(k->zfunc);
        if (S.depth_func != f)
            gl.DepthFunc(f), S.depth_func = f, g_rp.fixed++;
        st_depth_write(k->zwrite ? GL_TRUE : GL_FALSE);
    }
    int st = depth && depth->has_stencil && k->stencil;
    st_cap(GL_STENCIL_TEST, &S.stencil_test, st);
    if (st)
    {
        GLenum f = compare(k->sfunc);
        GLint ref = (GLint)(d->stencil_ref & 0xFF);
        if (S.sfunc != f || S.sref != ref || S.sread != k->sread)
            gl.StencilFunc(f, ref, k->sread), S.sfunc = f, S.sref = ref, S.sread = k->sread, g_rp.fixed++;
        GLenum a = stencil_op(k->sfail), b = stencil_op(k->szfail), c = stencil_op(k->spass);
        if (S.sfail != a || S.szfail != b || S.spass != c)
            gl.StencilOp(a, b, c), S.sfail = a, S.szfail = b, S.spass = c, g_rp.fixed++;
        st_stencil_write(k->swrite);
    }
    /* depth bias is for triangles only (WebGPU's and Vulkan's numbers) */
    int po = zt && d->zbias && d->prim >= GFX_TRIANGLELIST;
    st_cap(GL_POLYGON_OFFSET_FILL, &S.poly_offset, po);
    if (po)
    {
        float units = (float)-d->zbias, factor = -(float)d->zbias * 0.5f;
        if (S.po_units != units || S.po_factor != factor)
            gl.PolygonOffset(factor, units), S.po_units = units, S.po_factor = factor, g_rp.fixed++;
    }
}

static void set_raster(const GfxDraw* d)
{
    /* D3D's front faces are clockwise on its screen, counter-clockwise in GL's window (y negated) */
    st_cap(GL_CULL_FACE, &S.cull, d->cull == 2 || d->cull == 3);
    if (d->cull == 2 || d->cull == 3)
    {
        GLenum f = d->cull == 3 ? GL_BACK : GL_FRONT;
        if (S.cull_face != f)
            gl.CullFace(f), S.cull_face = f, g_rp.fixed++;
    }
    GLenum pm = d->fill == 1 ? GL_POINT : d->fill == 2 ? GL_LINE : GL_FILL;
    if (S.poly_mode != pm)
        gl.PolygonMode(GL_FRONT_AND_BACK, pm), S.poly_mode = pm, g_rp.fixed++;
    uint32_t w, h;
    color_size(&w, &h);
    const int32_t* sc = d->scissor;
    if (sc[2] > 0)
    {
        int64_t x0 = sc[0] < 0 ? 0 : sc[0], y0 = sc[1] < 0 ? 0 : sc[1];
        int64_t x1 = (int64_t)sc[0] + sc[2], y1 = (int64_t)sc[1] + sc[3];
        if (x1 > w)
            x1 = w;
        if (y1 > h)
            y1 = h;
        if (x0 > x1)
            x0 = x1;
        if (y0 > y1)
            y0 = y1;
        st_scissor(1, (GLint)x0, (GLint)y0, (GLint)(x1 - x0), (GLint)(y1 - y0));
    }
    else
        st_scissor(0, 0, 0, 0, 0);
}

/* D3D's viewport, as it is (GL's window has D3D's rows). 0 when it is empty. */
static int set_viewport(const uint32_t vp[6])
{
    if (!vp[2] || !vp[3])
        return 0;
    float zmin, zmax;
    memcpy(&zmin, &vp[4], 4);
    memcpy(&zmax, &vp[5], 4);
    zmin = zmin < 0 ? 0 : zmin > 1 ? 1 : zmin;
    zmax = zmax < zmin ? zmin : zmax > 1 ? 1 : zmax;
    GLint v[4] = { (GLint)vp[0], (GLint)vp[1], (GLint)vp[2], (GLint)vp[3] };
    if (memcmp(v, S.vp, sizeof v))
        gl.Viewport(v[0], v[1], v[2], v[3]), memcpy(S.vp, v, sizeof v), g_rp.fixed++;
    if (S.dr[0] != zmin || S.dr[1] != zmax)
        gl.DepthRangef(zmin, zmax), S.dr[0] = zmin, S.dr[1] = zmax, g_rp.fixed++;
    return 1;
}

/* --- drawing --------------------------------------------------------------------------------------------- */
static GLuint g_dummy_buf, g_dummy_tbo, g_ring_tbo;
static GfxTex *g_dummy2d, *g_dummycube;

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

static GLenum gl_prim(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return GL_POINTS;
    case GFX_LINELIST: return GL_LINES;
    case GFX_LINESTRIP: return GL_LINE_STRIP;
    case GFX_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    case GFX_TRIANGLEFAN: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLES;
    }
}

void gfx_draw(const GfxDraw* d)
{
    if (!g_up || !d->count)
        return;
    uint64_t t0 = gfx_now_ns();
    uint32_t n = vertex_count(d->prim, d->count);
    if (!n)
        return;
    /* the ring room the draw takes, reserved at once (its pieces then share a segment) */
    uint32_t need = U_SIZE + (uint32_t)g_ubo_align;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (!d->buf[s] && d->data[s] && d->size[s])
            need += d->size[s] + 16;
    if (d->indices && !d->ibuf)
        need += n * d->index_size + 16;
    uint64_t tr0 = rp_now();
    if (!ring_reserve(need))
    {
        gfx_prof_skip(GFX_SKIP_RANGE);
        return;
    }
    uint64_t tr1 = rp_now();
    if (!bind_target())
    {
        gfx_prof_skip(GFX_SKIP_NO_TARGET);
        return;
    }
    GfxTex* depth = depth_attachment();
    if (tr0)
        g_rp.part_ns[6] += tr1 - tr0, g_rp.part_ns[7] += gfx_now_ns() - tr1;
    uint64_t tp = rp_now();
    GLuint p = program(d);
    rp_add(&g_rp.part_ns[5], tp);
    if (!p)
    {
        gfx_prof_skip(GFX_SKIP_PIPELINE);
        return;
    }
    if (!set_viewport(d->vp))
        return;
    uint64_t ta = rp_now();
    /* the streams: texture buffers over whole buffers (a static one's, or the ring's for the draw's own
     * vertices), the stream's place in it added to its registers' offsets */
    GfxU u;
    size_t un = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
    if (d->vs.prog)
        un = offsetof(GfxU, psc);
    if (d->fs.prog)
        un = sizeof(GfxU);
    memcpy(&u, &d->u, un);
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        GLuint tex = g_dummy_tbo;
        uint32_t at = 0;
        if (d->buf[s])
            tex = buf_texture(d->buf[s]), at = d->buf_off[s];
        else if (d->data[s] && d->size[s])
            tex = g_ring_tbo, at = ring_put(d->data[s], d->size[s], 16);
        st_texture(1 + s, GL_TEXTURE_BUFFER, tex);
        if (at)
            for (int r = 0; r < GFX_NREGS; ++r)
                if (d->vs.el[r].used && d->vs.el[r].stream == s)
                    u.offset[r] += (int32_t)at;
    }
    uint64_t tb = rp_now();
    /* the uniforms: the part the functions read (the rest of the block's room is left as it was) */
    uint32_t uoff = ring_put(NULL, U_SIZE, (uint32_t)g_ubo_align);
    uint64_t tr = rp_now();
    memcpy(g_ring_cpu + uoff, &u, un);
    rp_add(&g_rp.ring_ns, tr);
    g_rp.ring_bytes += un;
    if (!g_ring_mapped)
    {
        gl.BindBuffer(GL_COPY_WRITE_BUFFER, g_ring);
        gl.BufferSubData(GL_COPY_WRITE_BUFFER, uoff, (GLsizeiptr)un, &u);
    }
    st_buffer(GL_UNIFORM_BUFFER, 0, &S.ubo, g_ring, uoff, U_SIZE);
    uint64_t tu = rp_now();
    /* the stages' textures: none, the wrong kind, depth, or the target being drawn are the white dummy */
    uint32_t mask = tex_mask(&d->fs);
    for (int i = 0; i < 8; ++i)
    {
        uint32_t want = (mask >> (2 * i)) & 3;
        if (!want)
            continue;
        GfxTex* t = d->tex[i];
        if (!t || t->use == GFX_USE_DEPTH || (want == 2) != (t->type == GFX_TEX_CUBE) || t == g_rt)
            t = want == 2 ? g_dummycube : g_dummy2d;
        st_texture(UNIT0 + i, t->target, t->name);
        st_sampler(UNIT0 + i, sampler(&d->samp[i]));
    }
    uint64_t tt = rp_now();
    st_program(p);
    set_blend(&d->pipe, g_rt->fmt == F_X8R8G8B8 || g_rt->fmt == F_X1R5G5B5 || g_rt->fmt == F_R5G6B5);
    set_depth(d, depth);
    set_raster(d);
    GLenum mode = gl_prim(d->prim);
    uint64_t tc = rp_now();
    if (d->ibuf)
    {
        st_ebo(d->ibuf->name);
        gl.DrawElements(mode, (GLsizei)n, d->index_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, (const void*)(uintptr_t)d->ibuf_off);
    }
    else if (d->indices)
    {
        uint32_t at = ring_put(d->indices, n * d->index_size, 4);
        st_ebo(g_ring);
        gl.DrawElements(mode, (GLsizei)n, d->index_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, (const void*)(uintptr_t)at);
    }
    else
        gl.DrawArrays(mode, (GLint)d->vertex_start, (GLsizei)n);
    g_prof.draws++;
    uint64_t t1 = gfx_now_ns();
    g_prof.render_ns += t1 - t0;
    if (tc)
    {
        g_rp.call_ns += t1 - tc, g_rp.prep_ns += tc - t0; /* (the prep's ring waits are in it, and also apart) */
        g_rp.part_ns[0] += ta - t0, g_rp.part_ns[1] += tb - ta, g_rp.part_ns[2] += tu - tb;
        g_rp.part_ns[3] += tt - tu, g_rp.part_ns[4] += tc - tt;
    }
}

/* --- clears: each rectangle within the viewport, scissored ------------------------------------------------------ */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    if (!g_up || !bind_target())
        return;
    GfxTex* depth = depth_attachment();
    if (!depth)
        flags &= 1;
    else if (!depth->has_stencil)
        flags &= 3;
    if (!flags)
        return;
    GLbitfield bits = 0;
    if (flags & 1)
    {
        st_cmask(15);
        gl.ClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & 2)
    {
        st_depth_write(GL_TRUE);
        gl.ClearDepthf(z);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & 4)
    {
        st_stencil_write(0xFF);
        gl.ClearStencil((GLint)(stencil & 0xFF));
        bits |= GL_STENCIL_BUFFER_BIT;
    }
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
        st_scissor(1, x0, y0, x1 - x0, y1 - y0);
        gl.Clear(bits);
    }
}

/* --- copies and reads ----------------------------------------------------------------------------------------- */
void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!g_up || !src || !dst || src->ifmt != dst->ifmt || src->use == GFX_USE_DEPTH || dst->use == GFX_USE_DEPTH ||
        src->dropped || dst->dropped || slevel >= src->levels || dlevel >= dst->levels)
        return;
    gl.CopyImageSubData(src->name, src->target, (GLint)slevel, (GLint)sx, (GLint)sy, (GLint)sface, dst->name, dst->target,
        (GLint)dlevel, (GLint)dx, (GLint)dy, (GLint)dface, (GLsizei)w, (GLsizei)h, 1);
}

/* a level's texels, tightly packed (the size its rows take), into the bound pack buffer or out */
static uint32_t read_rows(const GfxTex* t, uint32_t level, uint32_t* row)
{
    uint32_t lw = level_dim(t->w, level), lh = level_dim(t->h, level);
    *row = t->block ? (lw + 3) / 4 * t->block : lw * t->bpp;
    return t->block ? (lh + 3) / 4 : lh;
}

static void read_level(GfxTex* t, uint32_t face, uint32_t level, void* out)
{
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    if (t->use == GFX_USE_RT) /* a target: read through a framebuffer, the drivers' fast path */
    {
        st_read_fbo(fbo_for(t, face, level, NULL));
        gl.ReadPixels(0, 0, (GLsizei)level_dim(t->w, level), (GLsizei)level_dim(t->h, level), t->ufmt, t->utype, out);
        return;
    }
    st_texture(0, t->target, t->name);
    if (t->block)
        gl.GetCompressedTexImage(face_target(t, face), (GLint)level, out);
    else
        gl.GetTexImage(face_target(t, face), (GLint)level, t->ufmt, t->utype, out);
}

/* tight rows into D3D's pitch */
static void unpack_rows(const GfxTex* t, uint32_t level, const uint8_t* s, void* dst, uint32_t pitch)
{
    uint32_t row, rows = read_rows(t, level, &row);
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t y = 0; y < rows; ++y)
        memcpy(d + (size_t)y * pitch, s + (size_t)y * row, row < pitch ? row : pitch);
}

void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!g_up || !t || t->use == GFX_USE_DEPTH || t->dropped || level >= t->levels || face >= t->layers)
        return;
    uint32_t row, rows = read_rows(t, level, &row);
    if (pitch == row)
    {
        read_level(t, face, level, dst);
        return;
    }
    uint8_t* tmp = (uint8_t*)malloc((size_t)row * rows);
    read_level(t, face, level, tmp);
    unpack_rows(t, level, tmp, dst, pitch);
    free(tmp);
}

static int g_debug;
/* FFXI_GL_DEBUG: the errors since the last check, and where */
static void gl_check(const char* where)
{
    if (!g_debug)
        return;
    for (GLenum e; (e = gl.GetError()) != GL_NO_ERROR;)
        LOG("[recomp] gfx: GL error 0x%x at %s\n", e, where);
}

static void async_free(GfxTex* t)
{
    AsyncRead* r = t->rd;
    if (!r)
        return;
    for (int i = 0; i < 3; ++i)
    {
        if (r->slot[i].fence)
            gl.DeleteSync(r->slot[i].fence);
        if (r->slot[i].pbo)
            gl.DeleteBuffers(1, &r->slot[i].pbo);
    }
    free(r->last);
    free(r);
    t->rd = NULL;
}

/* --- a texture's texels into a buffer, on the GPU alone -------------------------------------------------------------
 * A read into a pack buffer (glReadPixels, glGetTexImage) has no GPU path in the Switch's Mesa (20.1, nouveau): it
 * waits for everything queued before it - 5 to 12 ms a frame of the render thread's in the field, the sun's occlusion
 * probe read about every frame. Here a draw of a point per texel fetches each in its vertex function and writes it out
 * through transform feedback, with a tag after each: the copy waits for nothing, and is taken from a buffer mapped
 * for good once all its tags are there (gfx_tex_read_async). 4-byte BGRA levels of 2D textures (the probes' A8R8G8B8 and X8R8G8B8), read as glReadPixels reads
 * them (X8's stored alpha, not its swizzled one); other formats read as before. FFXI_GL_TF_READ=0 turns it off. */
static const char TF_READ_GLSL[] = "layout(binding = 0) uniform sampler2D u_t;\n"
                                   "layout(location = 0) uniform vec4 u_p; /* the level's width, the level, the tag */\n"
                                   "flat out uvec2 o_px;\n"
                                   "void main()\n"
                                   "{\n"
                                   "    int w = int(u_p.x), i = gl_VertexID;\n"
                                   "    o_px = uvec2(packUnorm4x8(texelFetch(u_t, ivec2(i % w, i / w), int(u_p.y)).bgra), uint(u_p.z));\n"
                                   "    gl_Position = vec4(0.0);\n"
                                   "}\n";
static GLuint g_tf_prog;
static int g_tf_state; /* 0 not tried, 1 ready, -1 off or failed */

static int tf_ready(void)
{
    if (g_tf_state)
        return g_tf_state > 0;
    g_tf_state = -1;
    const char* e = getenv("FFXI_GL_TF_READ");
    if ((e && *e == '0') || !gl.TransformFeedbackVaryings || !gl.BeginTransformFeedback || !gl.EndTransformFeedback)
        return 0;
    GLuint vs = compile(GL_VERTEX_SHADER, TF_READ_GLSL);
    if (!vs)
        return 0;
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs);
    const GLchar* out[1] = { "o_px" };
    gl.TransformFeedbackVaryings(p, 1, out, GL_INTERLEAVED_ATTRIBS);
    gl.LinkProgram(p);
    gl.DetachShader(p, vs);
    gl.DeleteShader(vs);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        gl.DeleteProgram(p);
        return 0;
    }
    g_tf_prog = p, g_tf_state = 1;
    return 1;
}

static int tf_readable(const GfxTex* t, uint32_t face)
{
    return face == 0 && t->type == GFX_TEX_2D && t->use != GFX_USE_DEPTH && !t->block && t->bpp == 4 && t->ufmt == GL_BGRA &&
        t->utype == GL_UNSIGNED_BYTE && tf_ready();
}

/* the level's texels, D3D's rows in order and tightly packed, into buf (size * 2 bytes): each texel with tag
 * after it (1..2^24-1, exact in the float it goes through), so the CPU sees when all of a copy has landed */
static uint32_t tf_tag(uint32_t serial) { return serial % 0xFFFFFEu + 1u; }
static void tf_read(GfxTex* t, uint32_t level, GLuint buf, uint32_t size, uint32_t tag)
{
    st_texture(0, t->target, t->name);
    st_sampler(0, 0);
    int x8 = t->fmt == F_X8R8G8B8; /* (its alpha swizzled to one for sampling) */
    if (x8)
        gl.TexParameteri(t->target, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
    st_program(g_tf_prog);
    gl.Uniform4f(0, (GLfloat)level_dim(t->w, level), (GLfloat)level, (GLfloat)tag, 0.0f);
    gl.BindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buf);
    gl.Enable(GL_RASTERIZER_DISCARD);
    gl.BeginTransformFeedback(GL_POINTS);
    gl.DrawArrays(GL_POINTS, 0, (GLsizei)(size / 4));
    gl.EndTransformFeedback();
    gl.Disable(GL_RASTERIZER_DISCARD);
    gl.BindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    if (x8)
        gl.TexParameteri(t->target, GL_TEXTURE_SWIZZLE_A, GL_ONE);
}

/* a GPU read's copy (texel, tag pairs) into tight texels when every tag is the copy's: 1, else 0 */
static int tf_take(uint8_t* out, const void* src, uint32_t size, uint32_t tag)
{
    const volatile uint32_t* w = (const volatile uint32_t*)src; /* (coherent memory the GPU writes) */
    uint32_t n = size / 4;
    for (uint32_t i = 0; i < n; ++i)
        if (w[2 * i + 1] != tag)
            return 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t px = w[2 * i];
        memcpy(out + 4 * i, &px, 4);
    }
    return 1;
}

/* the newest copy the GPU has finished, and a new one queued behind this frame's work (three in flight at
 * most); the first read of a level waits.
 * A GPU read (tf_read) into a buffer mapped for good has no fence: its copy is taken once every texel's tag
 * has landed. A fence costs a flush of the work queued so far, 5-10 ms on Mesa's nouveau with a frame's
 * worth queued, and so does waiting on one (a zero-timeout poll included). The other reads keep their fence,
 * looked at once two presents old (FFXI_GL_READ_AGE: 0 polls every frame). */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!g_up || !t || t->use == GFX_USE_DEPTH || t->dropped || level >= t->levels || face >= t->layers)
        return;
    uint32_t row, rows = read_rows(t, level, &row), size = row * rows;
    AsyncRead* r = t->rd;
    if (r && (r->face != face || r->level != level))
        async_free(t), r = NULL;
    if (!r)
    {
        r = t->rd = (AsyncRead*)calloc(1, sizeof *r);
        r->face = face, r->level = level, r->size = size;
        r->last = (uint8_t*)malloc(size);
        uint64_t tf = rp_now();
        read_level(t, face, level, r->last); /* the first: now */
        rp_add(&g_rp.rd_first_ns, tf);
        g_rp.rd_first++;
        gl_check("async: the first read");
    }
    uint64_t tk = rp_now();
    static int age = -1, map_on = -1;
    if (age < 0)
    {
        const char* e = getenv("FFXI_GL_READ_AGE");
        age = e && *e ? atoi(e) : 2;
        if (age < 0 || age > 2)
            age = 2; /* (three slots: one queued each frame, the oldest two presents back) */
        e = getenv("FFXI_GL_READ_MAP");
        map_on = gl.BufferStorage && !(e && e[0] == '0');
    }
    int tf = tf_readable(t, face);
    uint32_t bytes = tf ? size * 2 : size; /* (a GPU read's texels each with its tag) */
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    for (int i = 0; i < 3; ++i)
    {
        AsyncSlot* s = &r->slot[i];
        if (s->pending) /* a GPU read without a fence */
        {
            if (s->serial > r->have_serial && tf_take(r->last, s->map, size, tf_tag(s->serial)))
                r->have_serial = s->serial, s->pending = 0;
            else if (s->serial <= r->have_serial || g_serial - s->frame > 30) /* overtaken, or lost: reused */
                s->pending = 0;
            continue;
        }
        if (!s->fence || g_serial - s->frame < (uint64_t)age)
            continue;
        uint64_t tw = rp_now();
        GLenum st = age ? gl.ClientWaitSync(s->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) : gl.ClientWaitSync(s->fence, 0, 0);
        rp_add(&g_rp.rd_poll_ns, tw);
        if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED)
            continue;
        gl.DeleteSync(s->fence);
        s->fence = NULL;
        if (s->serial > r->have_serial)
        {
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, s->pbo);
            const void* m = gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
            if (m && (tf ? tf_take(r->last, m, size, tf_tag(s->serial)) : (memcpy(r->last, m, size), 1)))
                r->have_serial = s->serial;
            gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            gl_check("async: a copy mapped");
        }
    }
    unpack_rows(t, level, r->last, dst, pitch);
    rp_add(&g_rp.rd_take_ns, tk);
    uint64_t tq = rp_now();
    for (int i = 0; i < 3; ++i)
    {
        AsyncSlot* s = &r->slot[i];
        if (s->fence || s->pending)
            continue;
        if (!s->pbo)
        {
            gl.GenBuffers(1, &s->pbo);
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, s->pbo);
            /* a GPU read's buffer mapped once, coherent (FFXI_GL_READ_MAP=0: not): no map, and no fence */
            if (tf && map_on)
            {
                GLbitfield f = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                void* zero = calloc(1, bytes); /* (no tag yet) */
                gl.BufferStorage(GL_PIXEL_PACK_BUFFER, bytes, zero, f);
                free(zero);
                s->map = gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, f);
            }
            else
                gl.BufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
        }
        else
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, s->pbo);
        gl_check("async: the pack buffer");
        s->serial = ++r->serial;
        s->frame = g_serial;
        if (tf)
            tf_read(t, level, s->pbo, size, tf_tag(s->serial)), g_tf_reads++;
        else
            read_level(t, face, level, NULL), g_pack_reads++;
        gl_check("async: the copy queued");
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (tf && s->map)
            s->pending = 1;
        else
            s->fence = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        break;
    }
    rp_add(&g_rp.rd_queue_ns, tq);
}

/* --- present: the back buffer onto the window, flipped (GL's window counts rows from the bottom) ------------- */
static int g_prect[4];

/* --- the CRT filter: the present through a shader of a picture tube's ------------------------------------------
 * FFXI_CRT=1 (the Switch's settings app: Scanlines): a scanline for each of the picture's rows, as an electron
 * beam draws them - brighter rows wider, the gaps between them dark. FFXI_CRT=2 (CRT): with an aperture grille
 * too, the screen's columns in red, green and blue. FFXI_CRT_CURVE=1: the glass's curve and its darker corners.
 * A row's scanline needs about two of the screen's lines to look like one: at fewer (a picture as tall as the
 * screen, or 448 rows in handheld's 720) the lines are made fainter so they don't beat against the screen's
 * rows, and a picture of half the screen's height or less is shown at a whole multiple of its height (448 rows
 * at 896 of a TV's 1080), so that every scanline is as thick as the next. Compiled once, at the first present;
 * if it doesn't compile, the present is the blit it was. */
static const char CRT_GLSL[] =
    "layout(location = 0) uniform vec4 u_src;  /* the picture: w, h, 1/w, 1/h */\n"
    "layout(location = 1) uniform vec4 u_mode; /* x: 1 scanlines, 2 with the grille; y: the curve; z: the lines' depth */\n"
    "#ifdef GFX_VS\n"
    "out vec2 v_uv;\n"
    "void main()\n"
    "{\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    v_uv = vec2(p.x, 1.0 - p.y); /* 0..1 over the screen (the triangle reaches 2); row 0 (D3D's top) at the top */\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "#else\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "layout(binding = 0) uniform sampler2D u_tex;\n"
    "vec3 row_color(float x, float row) { vec3 c = texture(u_tex, vec2(x, row * u_src.w)).rgb; return c * c; }\n"
    "vec3 beam(float d, vec3 c)\n"
    "{\n"
    "    vec3 w = mix(vec3(0.24), vec3(0.42), sqrt(c)); /* a brighter row's beam is wider */\n"
    "    return exp(-0.5 * d * d / (w * w));\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = v_uv;\n"
    "    float edge = 1.0;\n"
    "    if (u_mode.y > 0.5)\n"
    "    {\n"
    "        vec2 c = uv * 2.0 - 1.0;\n"
    "        c *= 1.0 + vec2(0.035, 0.05) * (c.yx * c.yx);\n"
    "        uv = c * 0.5 + 0.5;\n"
    "        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "        vec2 v = uv * (1.0 - uv.yx);\n"
    "        edge = clamp(pow(v.x * v.y * 24.0, 0.25), 0.0, 1.0);\n"
    "    }\n"
    "    float y = uv.y * u_src.y, row = floor(y) + 0.5, d = y - row;\n"
    "    float other = row + (d < 0.0 ? -1.0 : 1.0);\n"
    "    vec3 c0 = row_color(uv.x, row), c1 = row_color(uv.x, clamp(other, 0.5, u_src.y - 0.5));\n"
    "    vec3 lit = c0 * beam(abs(d), c0) + c1 * beam(1.0 - abs(d), c1);\n"
    "    vec3 c = mix(c0, lit * 1.45, u_mode.z);\n"
    "    if (u_mode.x > 1.5)\n"
    "    {\n"
    "        int m = int(mod(gl_FragCoord.x, 3.0));\n"
    "        vec3 grille = vec3(0.72);\n"
    "        grille[m] = 1.0;\n"
    "        c *= grille * 1.22;\n"
    "    }\n"
    "    o_color = vec4(sqrt(clamp(c, 0.0, 1.0)) * edge, 1.0);\n"
    "}\n"
    "#endif\n";

static int g_crt = -1, g_crt_curve;  /* FFXI_CRT, FFXI_CRT_CURVE: -1 not read yet */
static GLuint g_crt_prog, g_crt_samp;

static int crt_ready(void)
{
    if (g_crt < 0)
    {
        const char* e = getenv("FFXI_CRT");
        const char* k = getenv("FFXI_CRT_CURVE");
        g_crt = e ? atoi(e) : 0;
        g_crt = g_crt < 0 ? 0 : g_crt > 2 ? 2 : g_crt;
        g_crt_curve = k && atoi(k) > 0;
        if (g_crt)
        {
            g_crt_prog = program_from(CRT_GLSL);
            gl.GenSamplers(1, &g_crt_samp);
            gl.SamplerParameteri(g_crt_samp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.SamplerParameteri(g_crt_samp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.SamplerParameteri(g_crt_samp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.SamplerParameteri(g_crt_samp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            LOG("[gfx] CRT filter: %s%s%s\n", g_crt == 2 ? "scanlines and aperture grille" : "scanlines",
                g_crt_curve ? ", curved" : "", g_crt_prog ? "" : " - its shader did not compile: off");
            if (!g_crt_prog)
                g_crt = 0;
        }
    }
    return g_crt;
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

/* the back buffer into the rectangle (window coordinates from the bottom) through the CRT shader */
static void crt_present(GfxTex* b, int x, int y, int w, int h)
{
    st_draw_fbo(0);
    st_program(g_crt_prog);
    st_texture(0, GL_TEXTURE_2D, b->name);
    st_sampler(0, g_crt_samp);
    st_cap(GL_BLEND, &S.blend, 0);
    st_cap(GL_DEPTH_TEST, &S.depth_test, 0);
    st_cap(GL_STENCIL_TEST, &S.stencil_test, 0);
    st_cap(GL_CULL_FACE, &S.cull, 0);
    st_cap(GL_POLYGON_OFFSET_FILL, &S.poly_offset, 0);
    st_scissor(0, 0, 0, 0, 0);
    st_cmask(15);
    if (S.poly_mode != GL_FILL)
        gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL), S.poly_mode = GL_FILL;
    GLint v[4] = { x, y, w, h };
    if (memcmp(v, S.vp, sizeof v))
        gl.Viewport(v[0], v[1], v[2], v[3]), memcpy(S.vp, v, sizeof v), g_rp.fixed++;
    /* the scanlines' depth: full at two of the screen's lines a row, none at one */
    float per_row = (float)h / (float)b->h;
    float depth = per_row <= 1.0f ? 0.0f : per_row >= 2.0f ? 1.0f : per_row - 1.0f;
    gl.Uniform4f(0, (float)b->w, (float)b->h, 1.0f / (float)b->w, 1.0f / (float)b->h);
    gl.Uniform4f(1, (float)g_crt, (float)g_crt_curve, depth, 0.0f);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

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
    uint64_t t0 = gfx_now_ns();
    if (++g_serial % 30 == 0)
        fx_reload();
    int dw, dh;
    ctx_drawable(&dw, &dh);
    if (backbuffer && dw > 0 && dh > 0)
    {
        int rx = 0, ry = 0, rw = dw, rh = dh;
        if (g_prect[2] > 0 && g_prect[3] > 0)
            rx = g_prect[0], ry = g_prect[1], rw = g_prect[2], rh = g_prect[3];
#if defined(__SWITCH__)
        /* a picture of another shape than the screen's (a 4:3 resolution, the settings app's 640x480 or the
         * PS2's 512x448): as large as fits, in the middle, black beside it - not stretched */
        else if (backbuffer->w && backbuffer->h)
        {
            long long bw = backbuffer->w, bh = backbuffer->h;
            if (llabs(bw * dh - bh * dw) * 50 > (long long)dw * dh)
            {
                if (bw * dh > bh * dw)
                    rh = (int)(dw * bh / bw) & ~1, ry = (dh - rh) / 2;
                else
                    rw = (int)(dh * bw / bh) & ~1, rx = (dw - rw) / 2;
            }
        }
#endif
        int crt = crt_ready();
        if (crt)
            crt_fit(backbuffer, dw, dh, &rx, &ry, &rw, &rh);
#if defined(__SWITCH__)
        nx_present_rect(rx, ry, rw, rh);
#endif
        int sh = ctx_surface_h(); /* window rows count from the surface's bottom; the picture is at its top */
        st_read_fbo(fbo_for(backbuffer, 0, 0, NULL));
        st_draw_fbo(0);
        st_scissor(0, 0, 0, 0, 0);
        st_cmask(15);
        if (rx > 0 || ry > 0 || rw < dw || rh < dh)
        {
            st_scissor(1, 0, sh - dh, dw, dh);
            gl.ClearColor(0, 0, 0, 1);
            gl.Clear(GL_COLOR_BUFFER_BIT);
            st_scissor(0, 0, 0, 0, 0);
        }
        int same = rw == (int)backbuffer->w && rh == (int)backbuffer->h;
        if (crt)
            crt_present(backbuffer, rx, sh - ry - rh, rw, rh);
        else
            gl.BlitFramebuffer(0, 0, (GLint)backbuffer->w, (GLint)backbuffer->h, rx, sh - ry, rx + rw, sh - ry - rh, GL_COLOR_BUFFER_BIT,
            same ? GL_NEAREST : GL_LINEAR);
        uint64_t ts = rp_now();
        if (ts)
            g_rp.blit_ns += ts - t0;
        ctx_swap(dw, dh);
        rp_add(&g_rp.swap_ns, ts);
    }
    /* the next frame in a segment of its own (ring_next); FFXI_GL_RING_FRAME=0: only when one is full */
    static int ring_frame = -1;
    if (ring_frame < 0)
    {
        const char* e = getenv("FFXI_GL_RING_FRAME");
        ring_frame = !(e && e[0] == '0');
    }
    if (ring_frame && g_seg_used)
    {
        uint64_t tf = rp_now();
        ring_next();
        rp_add(&g_rp.fence_ns, tf);
    }
    if (gfx_profiling)
        prof_frame(t0);
}

void gfx_resize(uint32_t w, uint32_t h) { (void)w, (void)h; } /* the window's size is asked at each present */

/* --- the device ------------------------------------------------------------------------------------------- */
static int gl_load(void)
{
    int ok = 1;
#define GL_LOAD(name, ret, params, optional)                                                                         \
    gl.name = (PFN_gl##name)gl_proc("gl" #name);                                                                      \
    if (!gl.name && !(optional))                                                                                      \
    {                                                                                                                 \
        LOG("[recomp] gfx: the driver has no gl%s\n", #name);                                                         \
        ok = 0;                                                                                                       \
    }
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    return ok;
}

static void make_statics(void)
{
    gl.GenBuffers(1, &g_dummy_buf);
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, g_dummy_buf);
    static const uint8_t zero[256];
    gl.BufferData(GL_COPY_WRITE_BUFFER, sizeof zero, zero, GL_STATIC_DRAW);
    g_dummy_tbo = words_texture(g_dummy_buf);
    ring_make();
    g_ring_tbo = words_texture(g_ring);
    uint32_t white[1] = { 0xFFFFFFFFu };
    g_dummy2d = gfx_tex_create(GFX_TEX_2D, F_A8R8G8B8, 1, 1, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(g_dummy2d, 0, 0, white, 4);
    g_dummycube = gfx_tex_create(GFX_TEX_CUBE, F_A8R8G8B8, 1, 1, 1, GFX_USE_SAMPLE);
    for (uint32_t f = 0; f < 6; ++f)
        gfx_tex_upload(g_dummycube, f, 0, white, 4);
}

#if defined(_WIN32)
static LONG CALLBACK dbg_crash(EXCEPTION_POINTERS* e)
{
    static int said;
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && said++ < 3)
    {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)e->ExceptionRecord->ExceptionAddress, &m);
        char name[260] = "?";
        if (m)
            GetModuleFileNameA(m, name, sizeof name);
        LOG("[dbg] access violation at %p = %s+0x%llx, %s %p\n", e->ExceptionRecord->ExceptionAddress, name,
            (unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)m),
            e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (void*)e->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int gfx_init(void* sdl_window, int vsync)
{
    if (g_up) /* a second device (the game's after the sign-in screen's, or a D3D Reset): the same one */
    {
        if (!ctx_window(sdl_window))
            return 0;
        if (vsync != g_vsync)
            ctx_interval(vsync), g_vsync = vsync;
        return 1;
    }
    const char* prof = getenv("FFXI_PROFILE");
    gfx_profiling = prof && prof[0] && prof[0] != '0';
    const char* dbg = getenv("FFXI_GL_DEBUG");
    int debug = dbg && dbg[0] && dbg[0] != '0';
    g_debug = debug;
    if (!ctx_up(sdl_window, debug))
        return 0;
    if (!gl_load())
        return 0;
    GLint major = 0, minor = 0;
    gl.GetIntegerv(GL_MAJOR_VERSION, &major);
    gl.GetIntegerv(GL_MINOR_VERSION, &minor);
    if (major * 10 + minor < 43)
    {
        LOG("[recomp] gfx: OpenGL %d.%d; 4.3 is needed\n", major, minor);
        return 0;
    }
    GLint tbs = 0; /* the ring is one texture buffer of words */
    gl.GetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &tbs);
    if ((uint32_t)tbs < RING_SEG / 4 * RING_NSEG)
    {
        LOG("[recomp] gfx: texture buffers of %d words; %u are needed\n", tbs, RING_SEG / 4 * RING_NSEG);
        return 0;
    }
    gl.GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g_ubo_align);
    if (g_ubo_align < 4)
        g_ubo_align = 4;
    g_has_s3tc = has_ext("GL_EXT_texture_compression_s3tc");
    g_has_mirror_once = major * 10 + minor >= 44 || has_ext("GL_ARB_texture_mirror_clamp_to_edge") ||
                        has_ext("GL_EXT_texture_mirror_clamp");
    if (major * 10 + minor >= 46 || has_ext("GL_EXT_texture_filter_anisotropic") || has_ext("GL_ARB_texture_filter_anisotropic"))
        gl.GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &g_max_aniso);
    const char* clip = getenv("FFXI_GL_CLIP"); /* 0: the shaders' z fixup, as without glClipControl (tests) */
    if (gl.ClipControl && (major * 10 + minor >= 45 || has_ext("GL_ARB_clip_control")) && !(clip && clip[0] == '0'))
    {
        gl.ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        g_clip01 = 1;
    }
#if defined(_WIN32)
    if (debug)
        AddVectoredExceptionHandler(1, dbg_crash);
#endif
    if (debug && gl.DebugMessageCallback)
    {
        gl.Enable(GL_DEBUG_OUTPUT);
        gl.Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        gl.DebugMessageCallback(on_debug, NULL);
    }
    st_reset();
    GLuint vao;
    gl.GenVertexArrays(1, &vao);
    gl.BindVertexArray(vao);
    gl.Enable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    gl.Enable(GL_PROGRAM_POINT_SIZE);
    gl.Disable(GL_DITHER);
    gl.FrontFace(GL_CCW);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    g_up = 1;
    fx_config();
    make_statics();
    g_vsync = vsync;
    ctx_interval(vsync);
    LOG("[recomp] gfx: OpenGL %d.%d on %s (%s)%s%s%s\n", major, minor, (const char*)gl.GetString(GL_RENDERER),
        (const char*)gl.GetString(GL_VENDOR), g_has_s3tc ? ", S3TC" : ", no S3TC (DXT textures are not drawn)",
        g_clip01 ? ", clip control" : "", g_ring_mapped ? ", persistent ring" : "");
    return 1;
}

/* --- the rest of gfx.h --------------------------------------------------------------------------------------- */
void gfx_set_focus(const float* pos) { (void)pos; }
void gfx_set_moghouse(int in) { (void)in; }
int gfx_sun_shadows_shown(void) { return 0; }
float gfx_sun_prime(float* center) { (void)center; return 0.0f; }
void gfx_scene_done(GfxTex* color, const GfxScene* s) { (void)color, (void)s; }
int gfx_rt_supported(void) { return 0; }
/* the scene renderer is Metal's only so far: the game's own draws stay */
int gfx_world_supported(void) { return 0; }
int gfx_world_draw(const GfxWorldFrame* f) { (void)f; return 0; }
void gfx_world_time(int pass) { (void)pass; }
void gfx_shadow_begin(const GfxShadowFrame* f) { (void)f; }
void gfx_gi_zone(const GfxGiZone* z) { (void)z; }
void gfx_gi_frame(const GfxGiFrame* f) { (void)f; }
/* --- the scene renderer's characters (gfx.h's GfxCharFrame; gfx_vulkan.c's CHARS_GLSL without the effects) ----------
 * Skinned here from the game's bone palette and the meshes' bone-space vertices, lit per vertex as the game's fixed
 * function lights them, then its two texture stages, alpha tested GREATER the reference, table fog; no culling, the
 * depth tested LESSEQUAL and written. The vertices (unit 1, GfxCharVertex's 18 words) and texture coordinates (unit 2)
 * are texture buffers of words, as the generated functions read their streams (Ryubing doesn't translate Mesa's
 * storage-buffer reads); a draw's state (GfxCharState: std140's layout as it is) and its actor's palette (up to 128
 * bones) are uniform blocks in the ring (bindings 1 and 2), the alpha reference, pixel offset and mirror the draw's own
 * block (binding 0). Read per vertex from a texture buffer instead, the state cost ~130 fetches a vertex: the GPU went
 * from ~47% to ~77% busy in a town at 614 MHz. */
static const char CHARS_GL[] =
    "#ifdef GFX_GL_CLIP01\n#define gl_clip_z(p) (p).z\n#else\n#define gl_clip_z(p) (2.0 * (p).z - (p).w)\n#endif\n"
    "#define sat(x) clamp(x, 0.0, 1.0)\n"
    "layout(std140, binding = 0) uniform CU { vec4 params; uvec4 at; } D;\n" /* alpha ref, pixel; -, -, mirror */
    "struct CL { vec4 pos, dir, diffuse, ambient, att; };\n"
    "layout(std140, binding = 1) uniform CSB { mat4 wvp, world; CL light[4]; vec4 ambient, md, ma, me, tf, fog, fog_col; } S;\n"
    "#ifdef GFX_VS\n"
    "layout(std140, binding = 2) uniform CPB { mat4 P[128]; } B;\n" /* D3D's rows: GLSL's columns */
    "layout(binding = 1) uniform usamplerBuffer vbt;\n"
    "layout(binding = 2) uniform usamplerBuffer uvt;\n"
    "out vec2 o_uv;\n"
    "out vec4 o_col;\n"
    "out float o_z;\n"
    "const vec3 SIGN[4] = vec3[4](vec3(1.0), vec3(-1.0, 1.0, 1.0), vec3(1.0, -1.0, 1.0), vec3(1.0, 1.0, -1.0));\n"
    "vec4 vw4(int b) { return uintBitsToFloat(uvec4(texelFetch(vbt, b).x, texelFetch(vbt, b + 1).x, texelFetch(vbt, b + 2).x,"
    " texelFetch(vbt, b + 3).x)); }\n"
    "void main() {\n"
    "  int vid = gl_VertexID, b = vid * 18;\n"
    "  vec4 va = vw4(b), vb4 = vw4(b + 4);\n"
    "  vec3 na = vw4(b + 8).xyz, nb = vw4(b + 12).xyz;\n"
    "  uint bones = texelFetch(vbt, b + 16).x, mir = texelFetch(vbt, b + 17).x;\n"
    "  uint ba = bones & 0xffu, bb = (bones >> 8) & 0xffu;\n"
    "  if (D.at.z != 0u) {\n"
    "    ba = (bones >> 16) & 0xffu, bb = bones >> 24;\n"
    "    vec3 sa = SIGN[mir & 3u], sb = SIGN[(mir >> 2) & 3u];\n"
    "    va.xyz *= sa; vb4.xyz *= sb; na *= sa; nb *= sb;\n"
    "  }\n"
    "  mat4 MA = B.P[ba & 127u], MB = B.P[bb & 127u];\n"
    "  vec3 n = (MA * vec4(na, 0.0)).xyz + (MB * vec4(nb, 0.0)).xyz;\n"
    "  vec3 p = (MA * va + MB * vb4).xyz;\n"
    "  vec3 wp = (S.world * vec4(p, 1.0)).xyz;\n"
    "  vec3 wn = normalize((S.world * vec4(n, 0.0)).xyz);\n" /* NORMALIZENORMALS */
    "  vec4 pos = S.wvp * vec4(p, 1.0);\n"
    "  o_z = pos.w;\n"
    "  pos.x += pos.w * D.params.y;\n"
    "  pos.y -= pos.w * D.params.z;\n"
    "  gl_Position = vec4(pos.x, -pos.y, gl_clip_z(pos), pos.w);\n" /* (GL's window has D3D's rows: y negated) */
    "  o_uv = uintBitsToFloat(uvec2(texelFetch(uvt, 2 * vid).x, texelFetch(uvt, 2 * vid + 1).x));\n"
    "  vec3 amb = S.ambient.rgb, dif = vec3(0.0);\n"
    "  for (int k = 0; k < 4; ++k) {\n"
    "    CL l = S.light[k];\n"
    "    if (l.dir.w == 0.0) continue;\n"
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
    "    dif += l.diffuse.rgb * at * max(dot(wn, Ld), 0.0);\n"
    "  }\n"
    "  o_col = vec4(sat(S.ma.rgb * amb + S.md.rgb * dif + S.me.rgb), S.md.a);\n"
    "}\n"
    "#endif\n"
    "#ifdef GFX_FS\n"
    "layout(binding = 8) uniform sampler2D t;\n"
    "in vec2 o_uv;\n"
    "in vec4 o_col;\n"
    "in float o_z;\n"
    "out vec4 oc;\n"
    "void main() {\n"
    "  vec4 tx = texture(t, o_uv), tf = S.tf;\n"
    "  vec3 c = sat(2.0 * sat(2.0 * tx.rgb * o_col.rgb) * tf.rgb);\n" /* MODULATE2X, then MODULATE2X by the factor */
    "  float a = sat(4.0 * sat(2.0 * tx.a * tf.a) * tf.a);\n"          /* MODULATE2X, then MODULATE4X */
    "  if (!(a * 255.0 > D.params.x)) discard;\n"
    "  if (S.fog.z > 0.0) c = mix(S.fog_col.rgb, c, sat((S.fog.y - o_z) / max(S.fog.y - S.fog.x, 1e-6)));\n"
    "  oc = vec4(c, a);\n"
    "}\n"
    "#endif\n";

static GLuint g_chars_prog;
static int g_chars_failed;

int gfx_chars_draw(const GfxCharFrame* f)
{
    if (!g_up || !f->nitem || !f->nbones || g_chars_failed)
        return 0;
    uint64_t t0 = gfx_now_ns();
    if (!g_chars_prog && !(g_chars_prog = program_from(CHARS_GL)))
    {
        g_chars_failed = 1, g_failures++;
        LOG("[recomp] gfx: the characters' functions failed: they are left to the game\n");
        return 0;
    }
    if (!bind_target() || !set_viewport(f->vp))
        return 0;
    /* the room in one segment of the ring: a state and a palette each time they change along the items (an actor's
     * items are in a run, as are a material's), a block a draw, every one aligned as a uniform block's range is */
    const uint32_t al = (uint32_t)g_ubo_align, st_sz = (uint32_t)sizeof(GfxCharState), pal_max = 128u * 64u;
    uint32_t nst = 0, npal = 0, ls = ~0u, lp = ~0u;
    for (uint32_t i = 0; i < f->nitem; ++i)
    {
        nst += f->item[i].state != ls, ls = f->item[i].state;
        npal += f->item[i].palette != lp, lp = f->item[i].palette;
    }
    uint64_t need = (uint64_t)nst * (st_sz + al) + (uint64_t)npal * (pal_max + al) + (uint64_t)f->nitem * (32u + al);
    if (need > RING_SEG || !ring_reserve((uint32_t)need))
    {
        gfx_prof_skip(GFX_SKIP_RANGE);
        return 0;
    }
    GfxTex* depth = depth_attachment();
    st_program(g_chars_prog);
    st_cmask(15);
    st_cap(GL_BLEND, &S.blend, 0);
    st_cap(GL_DEPTH_TEST, &S.depth_test, depth != NULL);
    if (depth)
    {
        if (S.depth_func != GL_LEQUAL)
            gl.DepthFunc(GL_LEQUAL), S.depth_func = GL_LEQUAL;
        st_depth_write(GL_TRUE);
    }
    st_cap(GL_STENCIL_TEST, &S.stencil_test, 0);
    st_cap(GL_POLYGON_OFFSET_FILL, &S.poly_offset, 0);
    st_cap(GL_CULL_FACE, &S.cull, 0);
    if (S.poly_mode != GL_FILL)
        gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL), S.poly_mode = GL_FILL;
    st_scissor(0, 0, 0, 0, 0);
    BufBind b1, b2; /* bindings 1 and 2 as set here (nothing else binds them) */
    memset(&b1, 0xFF, sizeof b1), memset(&b2, 0xFF, sizeof b2);
    uint32_t last_state = ~0u, state_at = 0, last_pal = ~0u, pal_at = 0, pal_size = 0;
    int drew = 0;
    for (uint32_t i = 0; i < f->nitem; ++i)
    {
        const GfxCharItem* it = &f->item[i];
        if (!it->vb || !it->uv || !it->ib || !it->tex || !it->count || it->tex->type != GFX_TEX_2D || it->state >= f->nstate ||
            it->palette >= f->nbones)
            continue;
        if (it->state != last_state)
            state_at = ring_put(&f->state[it->state], st_sz, al), last_state = it->state;
        if (it->palette != last_pal)
        {
            uint32_t nb = f->nbones - it->palette;
            pal_size = (nb < 128u ? nb : 128u) * 64u;
            pal_at = ring_put(f->palette + 16u * it->palette, pal_size, al), last_pal = it->palette;
        }
        struct
        {
            float params[4];
            uint32_t at[4];
        } u = { { f->alpha_ref, f->pixel[0], f->pixel[1], 0.0f }, { 0, 0, it->mirror, 0 } };
        uint32_t uoff = ring_put(&u, sizeof u, al);
        st_buffer(GL_UNIFORM_BUFFER, 0, &S.ubo, g_ring, uoff, (GLsizeiptr)sizeof u);
        st_buffer(GL_UNIFORM_BUFFER, 1, &b1, g_ring, state_at, st_sz);
        /* (the block's whole size bound, as GL wants it backed: the room after a short palette is the segment's) */
        st_buffer(GL_UNIFORM_BUFFER, 2, &b2, g_ring, pal_at, pal_max);
        st_texture(1, GL_TEXTURE_BUFFER, buf_texture(it->vb));
        st_texture(2, GL_TEXTURE_BUFFER, buf_texture(it->uv));
        GfxTex* t = it->tex == g_rt ? g_dummy2d : it->tex;
        st_texture(UNIT0, t->target, t->name);
        st_sampler(UNIT0, sampler(&it->samp));
        st_ebo(it->ib->name);
        uint32_t n = vertex_count(it->prim, it->count);
        gl.DrawElements(it->prim == GFX_TRIANGLESTRIP ? GL_TRIANGLE_STRIP : GL_TRIANGLES, (GLsizei)n,
            it->index_size == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void*)(uintptr_t)(it->first * it->index_size));
        ++drew;
    }
    g_prof.draws += (uint64_t)drew;
    g_prof.render_ns += gfx_now_ns() - t0;
    return drew > 0;
}
void gfx_lamps(const GfxWorldLight* l, uint32_t n) { (void)l, (void)n; }
void gfx_set_fixed_light(int fixed) { (void)fixed; }
void gfx_set_weather(int weather) { (void)weather; }
void gfx_trace_dump(const char* path) { (void)path; }
void gfx_finish(void)
{
    if (g_up)
        gl.Finish();
}
uint32_t gfx_failures(void) { return g_failures; }
void gfx_set_sync_pipelines(int on) { (void)on; } /* programs are always built at their first draw */

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

#if defined(__SWITCH__)
/* vblanks each frame is shown for (gfx_queue.c's adaptive cap): 1 for 60 fps, 2 for an even 30. On the window itself:
 * eglSwapInterval(2) does nothing here (Mesa's maximum is 1), and the swap doesn't wait for the vblank, so pacing
 * by the clock after it gave 38 fps, a frame shown for one vblank or two */
void gfx_nx_swap_interval(int n)
{
    nwindowSetSwapInterval(nwindowGetDefault(), (u32)n);
}
#endif
