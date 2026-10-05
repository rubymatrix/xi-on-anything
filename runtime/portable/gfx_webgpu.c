/* The graphics back end on WebGPU (the browser build): gfx.h over Dawn's webgpu.h for Emscripten
 * (emdawnwebgpu), with the shaders as WGSL (gfx_wgsl_generate). Modelled on gfx_vulkan.c.
 *
 * Threads: every call here runs on the browser build's render thread (gfx_queue.c), a worker that goes
 * back to its event loop between frames: WebGPU only presents a canvas, and answers its callbacks
 * (the adapter, the device, a pipeline built, a buffer mapped), between a worker's tasks. The page hands
 * that worker its canvas as an OffscreenCanvas (tools/web/web_pre.js), which is Module.canvas there.
 *
 * What WebGPU changes from Vulkan:
 *  - no push descriptors: one bind group per draw, cached by what it holds (the uniform block is the one
 *    dynamic offset); the vertex streams are bound whole, at offset 0, and a stream's byte offset goes
 *    into GfxU.offset with the rest (the generated fetch adds it);
 *  - no dynamic state but the viewport, scissor and stencil reference: blending, depth, stencil, cull,
 *    depth bias and topology are all in the pipeline's key;
 *  - one ring for the frame's uniforms, vertices and indices, written with the queue before each submit
 *    (queue writes come after earlier submits, so one buffer serves every frame); a texture or buffer the
 *    open encoder has used is submitted before it is rewritten, as D3D's order needs;
 *  - y is up in clip space and down in the framebuffer, as D3D: no flip;
 *  - no view swizzles or 16-bit color formats: those textures are widened to BGRA8 on upload;
 *  - partial clears are a quad.
 *
 * The scene effects (gfx_scene_done): FX_GLSL's passes as WGSL (gfx_webgpu_fx.h) - occlusion, contact
 * shadows, height fog, bloom, god rays, the grade, FXAA - and the world lit per pixel.
 *
 * Not yet: the sun's shadow maps and water, the async read's frame-late copies (it answers zeros), the
 * pipeline cache file. */
#include <emscripten/emscripten.h>
#include <webgpu/webgpu.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#define GFX_QUEUE_IMPL /* the back end's own gfx_* names */
#include "gfx_queue.h"
#include "gfx_fx.h"
#include "gfx_msl.h"
#include "gfx_scene.h"
#include "gfx_webgpu_fx.h"
#include "plat.h"
#include "runtime.h"

#define SV(s) ((WGPUStringView){ (s), WGPU_STRLEN })
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

enum
{
    F_R5G6B5 = 23,
    F_X1R5G5B5 = 24,
    F_A1R5G5B5 = 25,
    F_A4R4G4B4 = 26,
    F_A8 = 28,
    F_A8R8G8B8 = 21,
    F_X8R8G8B8 = 22,
    F_A8L8 = 51,
    F_L8 = 50,
    F_V8U8 = 60,
    F_D16 = 80,
};

/* how an upload's texels become the texture's */
enum
{
    CONV_NONE,
    CONV_X8,   /* XRGB8: alpha set to one */
    CONV_565,
    CONV_X555,
    CONV_1555,
    CONV_4444,
    CONV_A8,   /* to BGRA8 (0, 0, 0, a) */
    CONV_L8,   /* to BGRA8 (l, l, l, 1) */
    CONV_A8L8, /* to BGRA8 (l, l, l, a) */
    CONV_DROP, /* a compressed format the device lacks: left as created */
};

#define RING_SIZE (32u << 20)
#define U_SIZE ((uint32_t)sizeof(GfxU))
#define U_STRIDE ((U_SIZE + 255u) & ~255u)

/* --- the frame profile (gfx_null.c's) -------------------------------------------------------------------- */
int gfx_profiling;

typedef struct Prof
{
    uint64_t frames, draws, skips, bytes, front_ns, present_ns, since_ns, shim_ns, render_ns, pipelines, bindgroups;
} Prof;

static Prof g_prof;
static uint32_t g_present_thread;

uint64_t gfx_now_ns(void) { return rt_monotonic_ns(); }
void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason)
{
    (void)reason;
    g_prof.skips++;
}
void gfx_prof_shim(uint64_t ns)
{
    if (plat_thread_id() == g_present_thread)
        g_prof.shim_ns += ns;
}

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
    fprintf(stderr,
        "[gfx] %.1f fps (render thread): %.2f ms a frame encoding, %.2f presenting | %.0f draws, %.0f skipped, %.0f KB up, "
        "%.0f new pipelines, %.0f bind groups made\n",
        f / ((double)(now - g_prof.since_ns) * 1e-9), (double)g_prof.render_ns * ms, (double)g_prof.present_ns * ms,
        (double)g_prof.draws / f, (double)g_prof.skips / f, (double)g_prof.bytes / f / 1024.0, (double)g_prof.pipelines,
        (double)g_prof.bindgroups / f);
    fflush(stderr);
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

static uint32_t hash_bytes(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 16777619u;
    return h | 1;
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

/* every value, then empty (the bind groups: dropped when the cache grows large) */
static void map_clear(Map* m, void (*release)(void*))
{
    for (uint32_t i = 0; i < m->cap; ++i)
        if (m->hashes[i] && release)
            release(m->vals[i]);
    if (m->cap)
        memset(m->hashes, 0, (size_t)m->cap * 4);
    m->n = 0;
}

/* --- the device ---------------------------------------------------------------------------------------------- */
static WGPUInstance g_inst;
static WGPUAdapter g_adapter;
static WGPUDevice g_dev;
static WGPUQueue g_queue;
static WGPUSurface g_surface;
static WGPUTextureFormat g_surface_fmt = WGPUTextureFormat_BGRA8Unorm;
static uint32_t g_sw, g_sh; /* the canvas's size, as configured */
static int g_has_bc;
static volatile uint32_t g_failures;
static int g_errors_said;

/* the page's canvas, as the render worker holds it (tools/web/web_pre.js) */
EM_JS(int, web_canvas_ready, (void), { return (typeof Module != 'undefined' && Module['canvas']) ? 1 : 0; });
EM_JS(void, web_canvas_size, (uint32_t w, uint32_t h), {
    var c = Module['canvas'];
    if (c && (c.width != w || c.height != h)) { c.width = w; c.height = h; }
});
EM_JS(int, web_has_gpu, (void), { return typeof navigator != 'undefined' && navigator.gpu ? 1 : 0; });

static void on_error(WGPUDevice const* dev, WGPUErrorType type, WGPUStringView msg, void* u1, void* u2)
{
    (void)dev, (void)u1, (void)u2;
    if (g_errors_said++ < 40)
        rt_log("[recomp] gfx: WebGPU error %d: %.*s\n", (int)type, (int)(msg.length == WGPU_STRLEN ? strlen(msg.data) : msg.length),
            msg.data ? msg.data : "");
    plat_atomic_add32(&g_failures, 1);
}

/* the start-up, a step at a time from gfx_init_then and each frame (gfx_web_tick): the adapter, the
 * device, then the canvas once the page has sent it; the waiting game is told how it went */
static GfxqCall* g_init_call;
static int g_init_state; /* 0 not asked, 1 adapter asked, 2 device asked, 3 waiting for the canvas, 4 up, -1 failed */
static int g_vsync;

static void init_done(int ok)
{
    g_init_state = ok ? 4 : -1;
    if (g_init_call)
    {
        gfxq_call_result(g_init_call, ok);
        GfxqCall* c = g_init_call;
        g_init_call = NULL;
        gfxq_call_done(c);
    }
}

static void on_device(WGPURequestDeviceStatus st, WGPUDevice dev, WGPUStringView msg, void* u1, void* u2)
{
    (void)msg, (void)u1, (void)u2;
    if (st != WGPURequestDeviceStatus_Success || !dev)
    {
        rt_log("[recomp] gfx: no WebGPU device\n");
        init_done(0);
        return;
    }
    g_dev = dev;
    g_queue = wgpuDeviceGetQueue(dev);
    g_init_state = 3;
}

static void on_adapter(WGPURequestAdapterStatus st, WGPUAdapter ad, WGPUStringView msg, void* u1, void* u2)
{
    (void)msg, (void)u1, (void)u2;
    if (st != WGPURequestAdapterStatus_Success || !ad)
    {
        rt_log("[recomp] gfx: no WebGPU adapter\n");
        init_done(0);
        return;
    }
    g_adapter = ad;
    WGPUFeatureName feats[2];
    size_t nf = 0;
    if (wgpuAdapterHasFeature(ad, WGPUFeatureName_TextureCompressionBC))
        feats[nf++] = WGPUFeatureName_TextureCompressionBC, g_has_bc = 1;
    WGPUDeviceDescriptor dd = { 0 };
    dd.label = SV("ffxi");
    dd.requiredFeatureCount = nf;
    dd.requiredFeatures = feats;
    dd.uncapturedErrorCallbackInfo.callback = on_error;
    WGPURequestDeviceCallbackInfo cb = { 0 };
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = on_device;
    wgpuAdapterRequestDevice(ad, &dd, cb);
    g_init_state = 2;
}

static void make_statics(void);

static int surface_up(void)
{
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector src = { 0 };
    src.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    src.selector = SV("#canvas");
    WGPUSurfaceDescriptor sd = { 0 };
    sd.nextInChain = &src.chain;
    g_surface = wgpuInstanceCreateSurface(g_inst, &sd);
    if (!g_surface)
        return 0;
    WGPUSurfaceCapabilities caps = { 0 };
    if (wgpuSurfaceGetCapabilities(g_surface, g_adapter, &caps) == WGPUStatus_Success && caps.formatCount)
        g_surface_fmt = caps.formats[0];
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    return 1;
}

static void surface_size(uint32_t w, uint32_t h)
{
    if (!g_surface || !w || !h || (w == g_sw && h == g_sh))
        return;
    web_canvas_size(w, h);
    WGPUSurfaceConfiguration sc = { 0 };
    sc.device = g_dev;
    sc.format = g_surface_fmt;
    sc.usage = WGPUTextureUsage_RenderAttachment;
    sc.width = w, sc.height = h;
    sc.alphaMode = WGPUCompositeAlphaMode_Opaque;
    sc.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(g_surface, &sc);
    g_sw = w, g_sh = h;
}

void gfx_web_tick(void)
{
    if (g_init_state == 3 && web_canvas_ready())
    {
        if (!surface_up())
        {
            rt_log("[recomp] gfx: no WebGPU canvas\n");
            init_done(0);
            return;
        }
        make_statics();
        const char* p = getenv("FFXI_PROFILE");
        gfx_profiling = p && p[0] && p[0] != '0';
        rt_log("[recomp] gfx: WebGPU up%s\n", g_has_bc ? ", BC textures" : ", no BC textures");
        init_done(1);
    }
}

void gfx_init_then(void* sdl_window, int vsync, GfxqCall* call)
{
    (void)sdl_window;
    g_vsync = vsync;
    if (g_init_state == 4 || g_init_state == -1) /* a second device (D3D Reset): the same one */
    {
        gfxq_call_result(call, g_init_state == 4);
        gfxq_call_done(call);
        return;
    }
    g_init_call = call;
    if (g_init_state)
        return;
    if (!web_has_gpu())
    {
        rt_log("[recomp] gfx: this browser has no WebGPU\n");
        init_done(0);
        return;
    }
    g_inst = wgpuCreateInstance(NULL);
    WGPURequestAdapterOptions ao = { 0 };
    ao.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo cb = { 0 };
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = on_adapter;
    wgpuInstanceRequestAdapter(g_inst, &ao, cb);
    g_init_state = 1;
}

int gfx_init(void* sdl_window, int vsync)
{
    (void)sdl_window, (void)vsync;
    return g_init_state == 4; /* the queue starts it with gfx_init_then */
}

uint64_t gfx_window_flags(void) { return 0; }

/* --- textures -------------------------------------------------------------------------------------------- */
struct GfxTex
{
    WGPUTexture tex;
    WGPUTextureView view;     /* every level (and face), for sampling */
    WGPUTextureView* rtv;     /* per face x level, as attachments (render targets and depth) */
    WGPUTextureFormat wf;
    int type, use, conv;
    uint32_t fmt, w, h, levels, layers, block, texel, id;
    int has_stencil;
    uint32_t used; /* the encoder serial that last read it */
    GfxTex *depth_seen, *depth_world; /* render targets: the depth they were last drawn with; by the scene's casters */
};

static uint32_t g_tex_ids;
static uint32_t g_enc_serial = 1; /* the open encoder's (its resources compare used against it) */

static WGPUTextureFormat tex_format(uint32_t fmt, int use, int* conv, uint32_t* block, uint32_t* texel, int* stencil)
{
    *conv = CONV_NONE, *block = 0, *texel = 4, *stencil = 0;
    if (use == GFX_USE_DEPTH)
    {
        if (fmt == F_D16)
            return *texel = 2, WGPUTextureFormat_Depth16Unorm;
        *stencil = 1;
        return WGPUTextureFormat_Depth24PlusStencil8;
    }
    switch (fmt)
    {
    case F_A8R8G8B8: return WGPUTextureFormat_BGRA8Unorm;
    case F_X8R8G8B8: *conv = use == GFX_USE_RT ? CONV_NONE : CONV_X8; return WGPUTextureFormat_BGRA8Unorm;
    case F_R5G6B5: *conv = CONV_565; return WGPUTextureFormat_BGRA8Unorm;
    case F_X1R5G5B5: *conv = CONV_X555; return WGPUTextureFormat_BGRA8Unorm;
    case F_A1R5G5B5: *conv = CONV_1555; return WGPUTextureFormat_BGRA8Unorm;
    case F_A4R4G4B4: *conv = CONV_4444; return WGPUTextureFormat_BGRA8Unorm;
    case F_A8: *conv = CONV_A8; return WGPUTextureFormat_BGRA8Unorm;
    case F_L8: *conv = CONV_L8; return WGPUTextureFormat_BGRA8Unorm;
    case F_A8L8: *conv = CONV_A8L8; return WGPUTextureFormat_BGRA8Unorm;
    case F_V8U8: *texel = 2; return WGPUTextureFormat_RG8Snorm;
    }
    WGPUTextureFormat bc = WGPUTextureFormat_Undefined;
    if (fmt == FOURCC('D', 'X', 'T', '1'))
        *block = 8, bc = WGPUTextureFormat_BC1RGBAUnorm;
    else if (fmt == FOURCC('D', 'X', 'T', '2') || fmt == FOURCC('D', 'X', 'T', '3'))
        *block = 16, bc = WGPUTextureFormat_BC2RGBAUnorm;
    else if (fmt == FOURCC('D', 'X', 'T', '4') || fmt == FOURCC('D', 'X', 'T', '5'))
        *block = 16, bc = WGPUTextureFormat_BC3RGBAUnorm;
    if (bc)
    {
        if (g_has_bc)
            return bc;
        *conv = CONV_DROP, *block = 0;
    }
    return WGPUTextureFormat_BGRA8Unorm;
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

/* one row of the D3D format into the texture's (w texels) */
static void convert_row(int conv, const uint8_t* s, uint8_t* d, uint32_t w)
{
    for (uint32_t x = 0; x < w; ++x)
    {
        uint32_t r, g, b, a;
        switch (conv)
        {
        case CONV_X8: d[4 * x] = s[4 * x], d[4 * x + 1] = s[4 * x + 1], d[4 * x + 2] = s[4 * x + 2], d[4 * x + 3] = 255; continue;
        case CONV_A8: r = g = b = 0, a = s[x]; break;
        case CONV_L8: r = g = b = s[x], a = 255; break;
        case CONV_A8L8: r = g = b = s[2 * x], a = s[2 * x + 1]; break;
        default:
        {
            uint32_t p = (uint32_t)s[2 * x] | ((uint32_t)s[2 * x + 1] << 8);
            switch (conv)
            {
            case CONV_565: r = expand(p >> 11, 5), g = expand((p >> 5) & 63, 6), b = expand(p & 31, 5), a = 255; break;
            case CONV_X555: r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = 255; break;
            case CONV_1555:
                r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = (p & 0x8000) ? 255 : 0;
                break;
            default: r = (p >> 8 & 15) * 17, g = (p >> 4 & 15) * 17, b = (p & 15) * 17, a = (p >> 12) * 17; break;
            }
        }
        }
        d[4 * x] = (uint8_t)b, d[4 * x + 1] = (uint8_t)g, d[4 * x + 2] = (uint8_t)r, d[4 * x + 3] = (uint8_t)a;
    }
}

static uint32_t level_dim(uint32_t d, uint32_t level)
{
    d >>= level;
    return d ? d : 1;
}

static void submit(void);

GfxTex* gfx_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_dev || !w || !h)
        return NULL;
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    t->type = type, t->use = use, t->fmt = d3dfmt, t->w = w, t->h = h, t->id = ++g_tex_ids;
    t->wf = tex_format(d3dfmt, use, &t->conv, &t->block, &t->texel, &t->has_stencil);
    uint32_t maxl = 1;
    while ((w >> maxl) || (h >> maxl))
        maxl++;
    t->levels = levels && levels < maxl ? levels : maxl;
    if (use != GFX_USE_SAMPLE)
        t->levels = 1;
    if (t->block) /* BC needs whole blocks at each level: the levels below 4x4 are dropped */
        while (t->levels > 1 && (level_dim(w, t->levels - 1) % 4 || level_dim(h, t->levels - 1) % 4))
            t->levels--;
    if (t->block && (w % 4 || h % 4))
        t->wf = WGPUTextureFormat_BGRA8Unorm, t->conv = CONV_DROP, t->block = 0;
    t->layers = type == GFX_TEX_CUBE ? 6 : 1;
    WGPUTextureDescriptor td = { 0 };
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
    if (use != GFX_USE_SAMPLE)
        td.usage |= WGPUTextureUsage_RenderAttachment;
    if (use == GFX_USE_DEPTH)
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    td.dimension = WGPUTextureDimension_2D;
    td.size = (WGPUExtent3D){ w, h, t->layers };
    td.format = t->wf;
    td.mipLevelCount = t->levels;
    td.sampleCount = 1;
    t->tex = wgpuDeviceCreateTexture(g_dev, &td);
    WGPUTextureViewDescriptor vd = { 0 };
    vd.format = use == GFX_USE_DEPTH ? WGPUTextureFormat_Undefined : t->wf; /* depth: the depth aspect's own format */
    vd.dimension = type == GFX_TEX_CUBE ? WGPUTextureViewDimension_Cube : WGPUTextureViewDimension_2D;
    vd.mipLevelCount = t->levels;
    vd.arrayLayerCount = t->layers;
    vd.aspect = use == GFX_USE_DEPTH ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
    t->view = wgpuTextureCreateView(t->tex, &vd);
    if (use != GFX_USE_SAMPLE)
    {
        t->rtv = (WGPUTextureView*)calloc((size_t)t->layers * t->levels, sizeof *t->rtv);
        for (uint32_t f = 0; f < t->layers; ++f)
            for (uint32_t l = 0; l < t->levels; ++l)
            {
                WGPUTextureViewDescriptor av = { 0 };
                av.format = t->wf;
                av.dimension = WGPUTextureViewDimension_2D;
                av.baseMipLevel = l, av.mipLevelCount = 1;
                av.baseArrayLayer = f, av.arrayLayerCount = 1;
                av.aspect = WGPUTextureAspect_All;
                t->rtv[f * t->levels + l] = wgpuTextureCreateView(t->tex, &av);
            }
    }
    return t;
}

static GfxTex* g_rt;
static GfxTex* g_ds;

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (t->used == g_enc_serial || t == g_rt || t == g_ds)
        submit(); /* the open encoder may still draw with it */
    if (g_rt == t)
        g_rt = NULL;
    if (g_ds == t)
        g_ds = NULL;
    if (t->rtv)
    {
        for (uint32_t i = 0; i < t->layers * t->levels; ++i)
            wgpuTextureViewRelease(t->rtv[i]);
        free(t->rtv);
    }
    wgpuTextureViewRelease(t->view);
    wgpuTextureDestroy(t->tex);
    wgpuTextureRelease(t->tex);
    free(t);
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels || face >= t->layers || t->conv == CONV_DROP || t->use == GFX_USE_DEPTH)
        return;
    if (t->used == g_enc_serial)
        submit(); /* draws already encoded read the old texels */
    const uint8_t* s = (const uint8_t*)src;
    uint32_t rows = t->block ? (h + 3) / 4 : h, row = t->block ? (w + 3) / 4 * t->block : w * t->texel;
    uint8_t* tmp = NULL;
    uint32_t bpr = pitch;
    if (t->conv != CONV_NONE)
    {
        tmp = (uint8_t*)malloc((size_t)row * rows);
        for (uint32_t r = 0; r < rows; ++r)
            convert_row(t->conv, s + (size_t)r * pitch, tmp + (size_t)r * row, w);
        s = tmp, bpr = row;
    }
    WGPUTexelCopyTextureInfo dst = { t->tex, level, { x, y, face }, WGPUTextureAspect_All };
    WGPUTexelCopyBufferLayout lay = { 0, bpr, rows };
    WGPUExtent3D ext = { w, h, 1 };
    if (t->block) /* whole blocks: a rectangle's last ones may stick out past a small level */
    {
        uint32_t lw = level_dim(t->w, level), lh = level_dim(t->h, level);
        ext.width = x + w > lw ? lw - x : w, ext.height = y + h > lh ? lh - y : h;
        ext.width = (ext.width + 3) & ~3u, ext.height = (ext.height + 3) & ~3u;
    }
    wgpuQueueWriteTexture(g_queue, &dst, s, (size_t)bpr * (rows - 1) + row, &lay, &ext);
    g_prof.bytes += (uint64_t)row * rows;
    free(tmp);
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (t)
        gfx_tex_upload_rect(t, face, level, 0, 0, level_dim(t->w, level), level_dim(t->h, level), src, pitch);
}

/* --- buffers ------------------------------------------------------------------------------------------- */
struct GfxBuf
{
    WGPUBuffer b;
    uint32_t size; /* as made: a multiple of 4 */
    uint32_t used;
};

GfxBuf* gfx_buf_create(uint32_t size)
{
    if (!g_dev || !size)
        return NULL;
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    b->size = (size + 3) & ~3u;
    WGPUBufferDescriptor bd = { 0 };
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
    bd.size = b->size;
    b->b = wgpuDeviceCreateBuffer(g_dev, &bd);
    return b;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    if (b->used == g_enc_serial)
        submit();
    wgpuBufferDestroy(b->b);
    wgpuBufferRelease(b->b);
    free(b);
}

void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b || !size)
        return;
    if (b->used == g_enc_serial)
        submit();
    if (size > b->size)
        size = b->size;
    uint32_t n = size & ~3u;
    if (n)
        wgpuQueueWriteBuffer(g_queue, b->b, 0, data, n);
    if (n < size) /* the last bytes, padded to a word */
    {
        uint8_t tail[4] = { 0 };
        memcpy(tail, (const uint8_t*)data + n, size - n);
        wgpuQueueWriteBuffer(g_queue, b->b, n, tail, 4);
    }
    g_prof.bytes += size;
}

/* --- the ring: the frame's uniforms, vertices and indices, written before each submit ----------------------- */
static WGPUBuffer g_ring;
static uint8_t* g_ring_cpu;
static uint32_t g_ring_used;

/* room for n bytes in one piece; 0 when the encoder had to be submitted first (the caller starts over) */
static int ring_room(uint32_t n)
{
    if (g_ring_used + n <= RING_SIZE)
        return 1;
    submit();
    return 0;
}

static uint32_t ring_put(const void* p, uint32_t n, uint32_t align)
{
    uint32_t at = (g_ring_used + align - 1) & ~(align - 1);
    if (p)
        memcpy(g_ring_cpu + at, p, n);
    g_ring_used = at + ((n + 3) & ~3u);
    return at;
}

/* --- the encoder and its passes ---------------------------------------------------------------------------- */
static WGPUCommandEncoder g_enc;
static WGPURenderPassEncoder g_pass;
static uint32_t g_rt_face, g_rt_level;
static uint32_t g_clear_flags, g_clear_color, g_clear_stencil; /* a whole-target clear waiting for the pass */
static float g_clear_z;
static WGPURenderPipeline g_bound;
static WGPUBindGroup g_bound_bg;
static uint32_t g_bound_uoff = ~0u;
static GfxTex* g_scratch_ds; /* depth for a target larger than the game's depth surface */

static void end_pass(void)
{
    if (!g_pass)
        return;
    wgpuRenderPassEncoderEnd(g_pass);
    wgpuRenderPassEncoderRelease(g_pass);
    g_pass = NULL;
    g_bound = NULL, g_bound_bg = NULL, g_bound_uoff = ~0u;
}

static void enc(void)
{
    if (!g_enc)
        g_enc = wgpuDeviceCreateCommandEncoder(g_dev, NULL);
}

static void submit(void)
{
    if (!g_enc)
        return;
    end_pass();
    if (g_ring_used)
        wgpuQueueWriteBuffer(g_queue, g_ring, 0, g_ring_cpu, (g_ring_used + 3) & ~3u);
    g_prof.bytes += g_ring_used;
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(g_enc, NULL);
    wgpuQueueSubmit(g_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(g_enc);
    g_enc = NULL;
    g_ring_used = 0;
    g_enc_serial++;
}

static void color_size(uint32_t* w, uint32_t* h)
{
    *w = g_rt ? level_dim(g_rt->w, g_rt_level) : 1;
    *h = g_rt ? level_dim(g_rt->h, g_rt_level) : 1;
}

/* the depth the pass gets: the game's surface when it is the target's size, a scratch one when larger
 * (WebGPU wants equal sizes; D3D draws a smaller target with a larger depth surface) */
static GfxTex* depth_attachment(void)
{
    uint32_t w, h;
    color_size(&w, &h);
    if (!g_ds)
        return NULL;
    if (g_ds->w == w && g_ds->h == h)
        return g_ds;
    if (g_ds->w < w || g_ds->h < h)
        return NULL;
    if (!g_scratch_ds || g_scratch_ds->w != w || g_scratch_ds->h != h)
    {
        if (g_scratch_ds)
            gfx_tex_destroy(g_scratch_ds);
        g_scratch_ds = gfx_tex_create(GFX_TEX_2D, 75 /* D24S8 */, w, h, 1, GFX_USE_DEPTH);
    }
    return g_scratch_ds;
}

static int begin_pass(void)
{
    if (g_pass)
        return 1;
    if (!g_rt || !g_rt->rtv)
        return 0;
    enc();
    WGPURenderPassColorAttachment ca = { 0 };
    ca.view = g_rt->rtv[g_rt_face * g_rt->levels + g_rt_level];
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.storeOp = WGPUStoreOp_Store;
    ca.loadOp = (g_clear_flags & 1) ? WGPULoadOp_Clear : WGPULoadOp_Load;
    uint32_t c = g_clear_color;
    ca.clearValue = (WGPUColor){ ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, (c >> 24) / 255.0 };
    WGPURenderPassDepthStencilAttachment da = { 0 };
    GfxTex* d = depth_attachment();
    WGPURenderPassDescriptor pd = { 0 };
    pd.colorAttachmentCount = 1;
    pd.colorAttachments = &ca;
    if (d)
    {
        da.view = d->rtv[0];
        da.depthLoadOp = (g_clear_flags & 2) ? WGPULoadOp_Clear : WGPULoadOp_Load;
        da.depthStoreOp = WGPUStoreOp_Store;
        da.depthClearValue = g_clear_z;
        if (d->has_stencil)
        {
            da.stencilLoadOp = (g_clear_flags & 4) ? WGPULoadOp_Clear : WGPULoadOp_Load;
            da.stencilStoreOp = WGPUStoreOp_Store;
            da.stencilClearValue = g_clear_stencil;
        }
        pd.depthStencilAttachment = &da;
    }
    g_pass = wgpuCommandEncoderBeginRenderPass(g_enc, &pd);
    g_clear_flags = 0;
    g_rt->used = g_enc_serial;
    return 1;
}

void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (color == g_rt && face == g_rt_face && level == g_rt_level && depth == g_ds)
        return;
    if (g_clear_flags && g_rt) /* a clear of the old target nothing drew after: done now */
        begin_pass();
    end_pass();
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
    g_clear_flags = 0;
}

/* --- samplers ------------------------------------------------------------------------------------------- */
static Map g_samplers = { .ksize = sizeof(GfxSampler) };

static WGPUAddressMode address(uint32_t a)
{
    switch (a)
    {
    case 2: return WGPUAddressMode_MirrorRepeat;
    case 3:
    case 4: return WGPUAddressMode_ClampToEdge; /* BORDER: WebGPU has no border color */
    case 5: return WGPUAddressMode_MirrorRepeat;
    default: return WGPUAddressMode_Repeat;
    }
}

static WGPUSampler sampler(const GfxSampler* k)
{
    WGPUSampler s = (WGPUSampler)map_get(&g_samplers, k);
    if (s)
        return s;
    WGPUSamplerDescriptor sd = { 0 };
    sd.addressModeU = address(k->addr_u);
    sd.addressModeV = address(k->addr_v);
    sd.addressModeW = address(k->addr_w);
    sd.magFilter = k->mag >= 2 ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    sd.minFilter = k->min >= 2 ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    sd.mipmapFilter = k->mip >= 2 ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    sd.maxAnisotropy = 1;
    if ((k->min == 3 || k->mag == 3) && k->max_aniso > 1 && sd.minFilter == WGPUFilterMode_Linear &&
        sd.magFilter == WGPUFilterMode_Linear && sd.mipmapFilter == WGPUMipmapFilterMode_Linear)
        sd.maxAnisotropy = k->max_aniso > 16 ? 16 : k->max_aniso;
    if (k->mip == 0)
        sd.lodMinClamp = 0.0f, sd.lodMaxClamp = 0.25f;
    else
    {
        sd.lodMinClamp = (float)k->max_level;
        sd.lodMaxClamp = k->lod_cap ? (float)(k->lod_cap - 1) : 32.0f;
        if (sd.lodMaxClamp < sd.lodMinClamp)
            sd.lodMaxClamp = sd.lodMinClamp;
    }
    s = wgpuDeviceCreateSampler(g_dev, &sd);
    map_put(&g_samplers, k, s);
    return s;
}

/* --- layouts: one bind group layout per set of texture stages (2D or cube each) --------------------------- */
typedef struct Layout
{
    WGPUBindGroupLayout bgl;
    WGPUPipelineLayout pl;
} Layout;

static Map g_layouts = { .ksize = 4 };

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

static Layout* layout_for(uint32_t mask)
{
    Layout* l = (Layout*)map_get(&g_layouts, &mask);
    if (l)
        return l;
    WGPUBindGroupLayoutEntry e[1 + GFX_NSTREAMS + 16];
    memset(e, 0, sizeof e);
    int n = 0;
    e[n].binding = 0;
    e[n].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    e[n].buffer.type = WGPUBufferBindingType_Uniform;
    e[n].buffer.hasDynamicOffset = 1;
    e[n++].buffer.minBindingSize = U_SIZE;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        e[n].binding = 1 + (uint32_t)s;
        e[n].visibility = WGPUShaderStage_Vertex;
        e[n++].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    }
    for (int i = 0; i < 8; ++i)
    {
        uint32_t t = (mask >> (2 * i)) & 3;
        if (!t)
            continue;
        e[n].binding = 8 + (uint32_t)i;
        e[n].visibility = WGPUShaderStage_Fragment;
        e[n].texture.sampleType = WGPUTextureSampleType_Float;
        e[n++].texture.viewDimension = t == 2 ? WGPUTextureViewDimension_Cube : WGPUTextureViewDimension_2D;
        e[n].binding = 24 + (uint32_t)i;
        e[n].visibility = WGPUShaderStage_Fragment;
        e[n++].sampler.type = WGPUSamplerBindingType_Filtering;
    }
    WGPUBindGroupLayoutDescriptor bd = { 0 };
    bd.entryCount = (size_t)n;
    bd.entries = e;
    l = (Layout*)calloc(1, sizeof *l);
    l->bgl = wgpuDeviceCreateBindGroupLayout(g_dev, &bd);
    WGPUPipelineLayoutDescriptor pd = { 0 };
    pd.bindGroupLayoutCount = 1;
    pd.bindGroupLayouts = &l->bgl;
    l->pl = wgpuDeviceCreatePipelineLayout(g_dev, &pd);
    map_put(&g_layouts, &mask, l);
    return l;
}

/* --- shader modules, one per key pair ------------------------------------------------------------------------ */
typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

static Map g_libs = { .ksize = sizeof(LibKey) };
static WGPUShaderModule LIB_FAILED = (WGPUShaderModule)1;

static WGPUShaderModule library(const LibKey* k, const uint32_t* vs, const uint32_t* ps)
{
    WGPUShaderModule m = (WGPUShaderModule)map_get(&g_libs, k);
    if (m)
        return m == LIB_FAILED ? NULL : m;
    char* src = gfx_wgsl_generate(&k->vs, &k->fs, vs, ps);
    if (!src)
    {
        map_put(&g_libs, k, LIB_FAILED);
        plat_atomic_add32(&g_failures, 1);
        return NULL;
    }
    WGPUShaderSourceWGSL w = { 0 };
    w.chain.sType = WGPUSType_ShaderSourceWGSL;
    w.code = SV(src);
    WGPUShaderModuleDescriptor md = { 0 };
    md.nextInChain = &w.chain;
    m = wgpuDeviceCreateShaderModule(g_dev, &md);
    free(src);
    map_put(&g_libs, k, m ? m : LIB_FAILED);
    return m;
}

/* --- pipelines --------------------------------------------------------------------------------------------- */
typedef struct PipeKey
{
    LibKey lib;
    GfxPipeKey pipe;
    GfxDepthKey depth; /* normalized: zero where the attachment has no depth or stencil */
    int32_t zbias;
    uint32_t color, ds; /* WGPUTextureFormat; ds 0: no depth attachment */
    uint8_t topo, strip_index, cull, x8;
} PipeKey;

typedef struct PipeEntry
{
    WGPURenderPipeline p;
    int state; /* 0 building, 1 ready, -1 failed */
} PipeEntry;

static Map g_pipes = { .ksize = sizeof(PipeKey) };
static uint32_t g_building;
static int g_sync_pipelines;

static WGPUBlendFactor blend_factor(uint32_t f, int x8)
{
    switch (f)
    {
    case 1: return WGPUBlendFactor_Zero;
    case 2: return WGPUBlendFactor_One;
    case 3: return WGPUBlendFactor_Src;
    case 4: return WGPUBlendFactor_OneMinusSrc;
    case 5: return WGPUBlendFactor_SrcAlpha;
    case 6: return WGPUBlendFactor_OneMinusSrcAlpha;
    case 7: return x8 ? WGPUBlendFactor_One : WGPUBlendFactor_DstAlpha;
    case 8: return x8 ? WGPUBlendFactor_Zero : WGPUBlendFactor_OneMinusDstAlpha;
    case 9: return WGPUBlendFactor_Dst;
    case 10: return WGPUBlendFactor_OneMinusDst;
    case 11: return WGPUBlendFactor_SrcAlphaSaturated;
    default: return WGPUBlendFactor_One;
    }
}

static WGPUBlendOperation blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return WGPUBlendOperation_Subtract;
    case 3: return WGPUBlendOperation_ReverseSubtract;
    case 4: return WGPUBlendOperation_Min;
    case 5: return WGPUBlendOperation_Max;
    default: return WGPUBlendOperation_Add;
    }
}

static WGPUCompareFunction compare(uint32_t f)
{
    switch (f)
    {
    case 1: return WGPUCompareFunction_Never;
    case 2: return WGPUCompareFunction_Less;
    case 3: return WGPUCompareFunction_Equal;
    case 4: return WGPUCompareFunction_LessEqual;
    case 5: return WGPUCompareFunction_Greater;
    case 6: return WGPUCompareFunction_NotEqual;
    case 7: return WGPUCompareFunction_GreaterEqual;
    default: return WGPUCompareFunction_Always;
    }
}

static WGPUStencilOperation stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return WGPUStencilOperation_Zero;
    case 3: return WGPUStencilOperation_Replace;
    case 4: return WGPUStencilOperation_IncrementClamp;
    case 5: return WGPUStencilOperation_DecrementClamp;
    case 6: return WGPUStencilOperation_Invert;
    case 7: return WGPUStencilOperation_IncrementWrap;
    case 8: return WGPUStencilOperation_DecrementWrap;
    default: return WGPUStencilOperation_Keep;
    }
}

static void on_pipeline(WGPUCreatePipelineAsyncStatus st, WGPURenderPipeline p, WGPUStringView msg, void* u1, void* u2)
{
    (void)u2;
    PipeEntry* e = (PipeEntry*)u1;
    g_building--;
    if (st == WGPUCreatePipelineAsyncStatus_Success && p)
        e->p = p, e->state = 1;
    else
    {
        e->state = -1;
        plat_atomic_add32(&g_failures, 1);
        if (g_errors_said++ < 40)
            rt_log("[recomp] gfx: a pipeline failed: %.*s\n", (int)(msg.length == WGPU_STRLEN ? strlen(msg.data) : msg.length),
                msg.data ? msg.data : "");
    }
}

static void build_pipeline(const PipeKey* k, PipeEntry* e, const uint32_t* vs, const uint32_t* ps)
{
    WGPUShaderModule m = library(&k->lib, vs, ps);
    if (!m)
    {
        e->state = -1;
        return;
    }
    Layout* l = layout_for(tex_mask(&k->lib.fs));
    WGPUBlendState bs;
    WGPUColorTargetState ct = { 0 };
    ct.format = (WGPUTextureFormat)k->color;
    uint32_t wm = k->pipe.write_mask;
    ct.writeMask = ((wm & 1) ? WGPUColorWriteMask_Red : 0) | ((wm & 2) ? WGPUColorWriteMask_Green : 0) |
        ((wm & 4) ? WGPUColorWriteMask_Blue : 0) | ((wm & 8) ? WGPUColorWriteMask_Alpha : 0);
    if (k->pipe.blend)
    {
        uint32_t sf = k->pipe.src, df = k->pipe.dst;
        if (sf == 12)
            sf = 5, df = 6;
        else if (sf == 13)
            sf = 6, df = 5;
        bs.color.srcFactor = bs.alpha.srcFactor = blend_factor(sf, k->x8);
        bs.color.dstFactor = bs.alpha.dstFactor = blend_factor(df, k->x8);
        if (bs.alpha.srcFactor == WGPUBlendFactor_SrcAlphaSaturated)
            bs.alpha.srcFactor = WGPUBlendFactor_One;
        if (bs.alpha.dstFactor == WGPUBlendFactor_SrcAlphaSaturated)
            bs.alpha.dstFactor = WGPUBlendFactor_One;
        bs.color.operation = bs.alpha.operation = blend_op(k->pipe.op);
        if (bs.color.operation == WGPUBlendOperation_Min || bs.color.operation == WGPUBlendOperation_Max)
            bs.color.srcFactor = bs.color.dstFactor = bs.alpha.srcFactor = bs.alpha.dstFactor = WGPUBlendFactor_One;
        ct.blend = &bs;
    }
    WGPUFragmentState fs = { 0 };
    fs.module = m;
    fs.entryPoint = SV("fs_main");
    fs.targetCount = 1;
    fs.targets = &ct;
    WGPUDepthStencilState ds = { 0 };
    WGPURenderPipelineDescriptor pd = { 0 };
    pd.layout = l->pl;
    pd.vertex.module = m;
    pd.vertex.entryPoint = SV("vs_main");
    static const WGPUPrimitiveTopology TOPO[] = { WGPUPrimitiveTopology_TriangleList, WGPUPrimitiveTopology_PointList,
        WGPUPrimitiveTopology_LineList, WGPUPrimitiveTopology_LineStrip, WGPUPrimitiveTopology_TriangleList,
        WGPUPrimitiveTopology_TriangleStrip, WGPUPrimitiveTopology_TriangleList };
    pd.primitive.topology = TOPO[k->topo < 7 ? k->topo : 0];
    pd.primitive.stripIndexFormat = (WGPUIndexFormat)k->strip_index;
    pd.primitive.frontFace = WGPUFrontFace_CW; /* D3D's front faces are clockwise on screen */
    pd.primitive.cullMode = k->cull == 3 ? WGPUCullMode_Back : k->cull == 2 ? WGPUCullMode_Front : WGPUCullMode_None;
    if (k->ds)
    {
        const GfxDepthKey* dk = &k->depth;
        ds.format = (WGPUTextureFormat)k->ds;
        ds.depthWriteEnabled = dk->zenable && dk->zwrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        ds.depthCompare = dk->zenable ? compare(dk->zfunc) : WGPUCompareFunction_Always;
        WGPUStencilFaceState sf = { WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep,
            WGPUStencilOperation_Keep };
        ds.stencilReadMask = 0xFF, ds.stencilWriteMask = 0;
        if (dk->stencil)
        {
            sf = (WGPUStencilFaceState){ compare(dk->sfunc), stencil_op(dk->sfail), stencil_op(dk->szfail), stencil_op(dk->spass) };
            ds.stencilReadMask = dk->sread, ds.stencilWriteMask = dk->swrite;
        }
        ds.stencilFront = ds.stencilBack = sf;
        if (k->topo >= 4) /* depth bias is for triangles only */
            ds.depthBias = -k->zbias, ds.depthBiasSlopeScale = -(float)k->zbias * 0.5f;
        pd.depthStencil = &ds;
    }
    pd.multisample.count = 1;
    pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g_prof.pipelines++;
    if (g_sync_pipelines)
    {
        e->p = wgpuDeviceCreateRenderPipeline(g_dev, &pd);
        e->state = e->p ? 1 : -1;
        return;
    }
    WGPUCreateRenderPipelineAsyncCallbackInfo cb = { 0 };
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = on_pipeline;
    cb.userdata1 = e;
    g_building++;
    wgpuDeviceCreateRenderPipelineAsync(g_dev, &pd, cb);
}

static WGPURenderPipeline pipeline_for(const PipeKey* k, const GfxDraw* d)
{
    PipeEntry* e = (PipeEntry*)map_get(&g_pipes, k);
    if (!e)
    {
        e = (PipeEntry*)calloc(1, sizeof *e);
        map_put(&g_pipes, k, e);
        build_pipeline(k, e, d->vs_tokens, d->ps_tokens);
    }
    return e->state == 1 ? e->p : NULL;
}

static WGPURenderPipeline pipeline(const GfxDraw* d, GfxTex* depth)
{
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.lib.vs = d->vs, k.lib.fs = d->fs;
    k.lib.vs.pixel = k.lib.vs.shadow = k.lib.vs.water = 0; /* the scene effects' variants are not drawn here yet */
    k.lib.fs.water = 0;
    k.pipe = d->pipe;
    k.color = (uint32_t)g_rt->wf;
    k.x8 = g_rt->fmt == F_X8R8G8B8;
    k.topo = (uint8_t)(d->prim <= 6 ? d->prim : 4);
    if ((d->prim == GFX_TRIANGLESTRIP || d->prim == GFX_LINESTRIP) && (d->ibuf || d->indices))
        k.strip_index = (uint8_t)(d->index_size == 2 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32);
    if (d->prim == GFX_TRIANGLEFAN)
        k.topo = 6; /* drawn as a list (draw_encode) */
    k.cull = d->cull;
    if (depth)
    {
        k.ds = (uint32_t)depth->wf;
        k.depth = d->depth;
        k.depth.pad[0] = k.depth.pad[1] = 0;
        if (!depth->has_stencil)
            k.depth.stencil = k.depth.sfail = k.depth.szfail = k.depth.spass = k.depth.sfunc = k.depth.sread = k.depth.swrite = 0;
        else if (!k.depth.stencil)
            k.depth.sfail = k.depth.szfail = k.depth.spass = k.depth.sfunc = k.depth.sread = k.depth.swrite = 0;
        if (!k.depth.zenable)
            k.depth.zwrite = k.depth.zfunc = 0;
        k.zbias = d->zbias;
    }
    /* the world's lit draws lit per pixel (gfx_msl.c pixel_lit), by the vertex's light meanwhile */
    if (g_fxs.fx != 0.0f && g_fxs.light != 0.0f && d->vs.lighting && !d->vs.rhw && !d->vs.prog && !d->vs.flat)
    {
        k.lib.vs.pixel = g_fxs.light >= 2.0f ? 2 : 1;
        WGPURenderPipeline p = pipeline_for(&k, d);
        if (p)
            return p;
        k.lib.vs.pixel = 0;
    }
    return pipeline_for(&k, d);
}

/* --- bind groups, cached by what they hold ------------------------------------------------------------------- */
typedef struct BgKey
{
    WGPUBindGroupLayout bgl;
    WGPUBuffer ring, sb[GFX_NSTREAMS];
    uint32_t tex[8]; /* GfxTex ids (0: the dummy) */
    WGPUTextureView view[8];
    WGPUSampler samp[8];
} BgKey;

static Map g_bgs = { .ksize = sizeof(BgKey) };
static WGPUBuffer g_dummy_buf;
static GfxTex *g_dummy2d, *g_dummycube;

static void release_bg(void* p) { wgpuBindGroupRelease((WGPUBindGroup)p); }

static WGPUBindGroup bind_group(const BgKey* k, uint32_t mask, const uint32_t* sb_size)
{
    WGPUBindGroup g = (WGPUBindGroup)map_get(&g_bgs, k);
    if (g)
        return g;
    if (g_bgs.n > 8192) /* the textures behind old ones may be gone: start again */
        map_clear(&g_bgs, release_bg);
    WGPUBindGroupEntry e[1 + GFX_NSTREAMS + 16];
    memset(e, 0, sizeof e);
    int n = 0;
    e[n].binding = 0, e[n].buffer = k->ring, e[n++].size = U_SIZE;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        e[n].binding = 1 + (uint32_t)s, e[n].buffer = k->sb[s], e[n++].size = sb_size[s];
    for (int i = 0; i < 8; ++i)
        if ((mask >> (2 * i)) & 3)
        {
            e[n].binding = 8 + (uint32_t)i, e[n++].textureView = k->view[i];
            e[n].binding = 24 + (uint32_t)i, e[n++].sampler = k->samp[i];
        }
    WGPUBindGroupDescriptor bd = { 0 };
    bd.layout = k->bgl;
    bd.entryCount = (size_t)n;
    bd.entries = e;
    g = wgpuDeviceCreateBindGroup(g_dev, &bd);
    map_put(&g_bgs, k, g);
    g_prof.bindgroups++;
    return g;
}

/* --- drawing --------------------------------------------------------------------------------------------- */
static uint32_t vertex_count(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case GFX_POINTLIST: return n;
    case GFX_LINELIST: return n * 2;
    case GFX_LINESTRIP: return n + 1;
    case GFX_TRIANGLELIST: return n * 3;
    case GFX_TRIANGLESTRIP: return n + 2;
    case GFX_TRIANGLEFAN: return n * 3;
    default: return 0;
    }
}

/* D3D's viewport, clamped to the target. 0 when nothing of it is left. */
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
    zmin = zmin < 0 ? 0 : zmin > 1 ? 1 : zmin;
    zmax = zmax < zmin ? zmin : zmax > 1 ? 1 : zmax;
    wgpuRenderPassEncoderSetViewport(g_pass, (float)x, (float)y, (float)vw, (float)vh, zmin, zmax);
    return 1;
}

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
    wgpuRenderPassEncoderSetScissorRect(g_pass, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
}

void gfx_draw(const GfxDraw* d)
{
    if (!g_dev || !d->count || g_init_state != 4)
        return;
    uint64_t t0 = gfx_now_ns();
    uint32_t n = vertex_count(d->prim, d->count);
    if (!n)
        return;
    /* the ring room the draw takes, reserved at once (a submit in between would move what it wrote) */
    uint32_t need = U_STRIDE + 256;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (!d->buf[s] && d->data[s] && d->size[s])
            need += d->size[s] + 256;
    if (d->prim == GFX_TRIANGLEFAN)
        need += n * 4 + 16;
    else if (d->indices && !d->ibuf)
        need += n * d->index_size + 16;
    if (need > RING_SIZE)
        return;
    if (!ring_room(need)) /* submitted: the next pass starts on the same target */
        ;
    if (!begin_pass())
    {
        gfx_prof_skip(GFX_SKIP_NO_TARGET);
        return;
    }
    GfxTex* depth = depth_attachment();
    if (depth && !g_rt_face && !g_rt_level) /* the scene's depth, for the effects */
    {
        g_rt->depth_seen = depth;
        if (d->caster)
            g_rt->depth_world = depth;
    }
    WGPURenderPipeline p = pipeline(d, depth);
    if (!p)
    {
        gfx_prof_skip(GFX_SKIP_PIPELINE);
        return;
    }
    if (!set_viewport(d->vp))
        return;
    /* the uniforms, with each stream's place added to its registers' offsets (the streams are bound whole) */
    GfxU* u = (GfxU*)(g_ring_cpu + ring_put(NULL, U_SIZE, 256));
    uint32_t uoff = (uint32_t)((uint8_t*)u - g_ring_cpu);
    size_t un = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
    if (d->vs.prog)
        un = offsetof(GfxU, psc);
    if (d->fs.prog)
        un = sizeof(GfxU);
    memcpy(u, &d->u, un);
    BgKey bk;
    memset(&bk, 0, sizeof bk);
    uint32_t sb_size[GFX_NSTREAMS];
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        uint32_t at = 0;
        if (d->buf[s])
        {
            bk.sb[s] = d->buf[s]->b, sb_size[s] = d->buf[s]->size, at = d->buf_off[s];
            d->buf[s]->used = g_enc_serial;
        }
        else if (d->data[s] && d->size[s])
        {
            at = ring_put(d->data[s], d->size[s], 16);
            bk.sb[s] = g_ring, sb_size[s] = RING_SIZE;
        }
        else
            bk.sb[s] = g_dummy_buf, sb_size[s] = 256;
        if (at)
            for (int r = 0; r < GFX_NREGS; ++r)
                if (d->vs.el[r].used && d->vs.el[r].stream == s)
                    u->offset[r] += (int32_t)at;
    }
    uint32_t mask = tex_mask(&d->fs);
    Layout* l = layout_for(mask);
    bk.bgl = l->bgl, bk.ring = g_ring;
    for (int i = 0; i < 8; ++i)
    {
        uint32_t want = (mask >> (2 * i)) & 3;
        if (!want)
            continue;
        GfxTex* t = d->tex[i];
        /* none, the wrong kind, depth, or the target being drawn (WebGPU can't read and write it at once) */
        if (!t || t->use == GFX_USE_DEPTH || (want == 2) != (t->type == GFX_TEX_CUBE) || t == g_rt || t == depth)
            t = want == 2 ? g_dummycube : g_dummy2d;
        t->used = g_enc_serial;
        bk.tex[i] = t->id, bk.view[i] = t->view, bk.samp[i] = sampler(&d->samp[i]);
    }
    WGPUBindGroup bg = bind_group(&bk, mask, sb_size);
    WGPURenderPassEncoder rp = g_pass;
    if (p != g_bound)
        wgpuRenderPassEncoderSetPipeline(rp, p), g_bound = p;
    set_scissor(d->scissor);
    if (depth && depth->has_stencil)
        wgpuRenderPassEncoderSetStencilReference(rp, d->depth.stencil ? d->stencil_ref : 0);
    if (bg != g_bound_bg || uoff != g_bound_uoff)
        wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 1, &uoff), g_bound_bg = bg, g_bound_uoff = uoff;
    if (d->prim == GFX_TRIANGLEFAN)
    {
        uint32_t at = ring_put(NULL, n * 4, 16);
        uint32_t* idx = (uint32_t*)(g_ring_cpu + at);
        for (uint32_t i = 0; i < d->count; ++i)
        {
            uint32_t k[3] = { 0, i + 1, i + 2 };
            for (int j = 0; j < 3; ++j)
            {
                if (d->ibuf || d->indices)
                {
                    const void* src = d->indices;
                    idx[3 * i + j] = !src ? 0 : d->index_size == 2 ? ((const uint16_t*)src)[k[j]] : ((const uint32_t*)src)[k[j]];
                }
                else
                    idx[3 * i + j] = d->vertex_start + k[j];
            }
        }
        wgpuRenderPassEncoderSetIndexBuffer(rp, g_ring, WGPUIndexFormat_Uint32, at, n * 4);
        wgpuRenderPassEncoderDrawIndexed(rp, n, 1, 0, 0, 0);
    }
    else if (d->ibuf)
    {
        d->ibuf->used = g_enc_serial;
        uint32_t isz = d->index_size == 2 ? 2 : 4;
        wgpuRenderPassEncoderSetIndexBuffer(rp, d->ibuf->b, isz == 2 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32, d->ibuf_off,
            (uint64_t)n * isz);
        wgpuRenderPassEncoderDrawIndexed(rp, n, 1, 0, 0, 0);
    }
    else if (d->indices)
    {
        uint32_t at = ring_put(d->indices, n * d->index_size, 16);
        wgpuRenderPassEncoderSetIndexBuffer(rp, g_ring, d->index_size == 2 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32, at,
            ((uint64_t)n * d->index_size + 3) & ~3ull);
        wgpuRenderPassEncoderDrawIndexed(rp, n, 1, 0, 0, 0);
    }
    else
        wgpuRenderPassEncoderDraw(rp, n, 1, d->vertex_start, 0);
    g_prof.draws++;
    g_prof.render_ns += gfx_now_ns() - t0;
}

/* --- clears: whole targets as the pass's load, the rest as a quad -------------------------------------------- */
static const char CLEAR_WGSL[] =
    "struct CU { color: vec4f, z: vec4f, };\n"
    "@group(0) @binding(0) var<uniform> cu: CU;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));\n"
    "  return vec4f(p[i], cu.z.x, 1.0);\n"
    "}\n"
    "@fragment fn fs() -> @location(0) vec4f { return cu.color; }\n";

static WGPUShaderModule g_clear_mod;
static WGPUBindGroupLayout g_clear_bgl;
static WGPUPipelineLayout g_clear_pl;
static Map g_clear_pipes = { .ksize = 16 };

static WGPURenderPipeline clear_pipeline(uint32_t color, uint32_t ds, uint32_t flags)
{
    uint32_t key[4] = { color, ds, flags, 0 };
    WGPURenderPipeline p = (WGPURenderPipeline)map_get(&g_clear_pipes, key);
    if (p)
        return p;
    WGPUColorTargetState ct = { 0 };
    ct.format = (WGPUTextureFormat)color;
    ct.writeMask = (flags & 1) ? WGPUColorWriteMask_All : WGPUColorWriteMask_None;
    WGPUFragmentState fs = { 0 };
    fs.module = g_clear_mod, fs.entryPoint = SV("fs"), fs.targetCount = 1, fs.targets = &ct;
    WGPUDepthStencilState dss = { 0 };
    WGPURenderPipelineDescriptor pd = { 0 };
    pd.layout = g_clear_pl;
    pd.vertex.module = g_clear_mod, pd.vertex.entryPoint = SV("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.cullMode = WGPUCullMode_None;
    if (ds)
    {
        dss.format = (WGPUTextureFormat)ds;
        dss.depthWriteEnabled = (flags & 2) ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        dss.depthCompare = WGPUCompareFunction_Always;
        WGPUStencilFaceState sf = { WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep,
            (flags & 4) ? WGPUStencilOperation_Replace : WGPUStencilOperation_Keep };
        dss.stencilFront = dss.stencilBack = sf;
        dss.stencilReadMask = 0xFF, dss.stencilWriteMask = (flags & 4) ? 0xFF : 0;
        pd.depthStencil = &dss;
    }
    pd.multisample.count = 1, pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    p = wgpuDeviceCreateRenderPipeline(g_dev, &pd);
    map_put(&g_clear_pipes, key, p);
    return p;
}

void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    if (!g_dev || !g_rt || g_init_state != 4)
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
    int whole = vp[0] == 0 && vp[1] == 0 && vp[2] >= w && vp[3] >= h;
    for (uint32_t i = 0; whole && i < nrects; ++i)
        whole = rects[4 * i] <= 0 && rects[4 * i + 1] <= 0 && (uint32_t)rects[4 * i + 2] >= w && (uint32_t)rects[4 * i + 3] >= h;
    if (whole && !g_pass) /* the next pass starts cleared */
    {
        g_clear_flags |= flags;
        if (flags & 1)
            g_clear_color = color;
        if (flags & 2)
            g_clear_z = z;
        if (flags & 4)
            g_clear_stencil = stencil;
        return;
    }
    if (!ring_room(U_STRIDE + 256) || !begin_pass())
    {
        if (!begin_pass())
            return;
    }
    float cu[8] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f, z };
    uint32_t uoff = ring_put(cu, sizeof cu, 256);
    WGPUBindGroupEntry be = { 0 };
    be.binding = 0, be.buffer = g_ring, be.size = sizeof cu;
    WGPUBindGroupDescriptor bd = { 0 };
    bd.layout = g_clear_bgl, bd.entryCount = 1, bd.entries = &be;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g_dev, &bd);
    WGPURenderPipeline p = clear_pipeline((uint32_t)g_rt->wf, depth ? (uint32_t)depth->wf : 0, flags);
    WGPURenderPassEncoder rp = g_pass;
    wgpuRenderPassEncoderSetPipeline(rp, p);
    wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 1, &uoff);
    if (depth && depth->has_stencil)
        wgpuRenderPassEncoderSetStencilReference(rp, stencil);
    wgpuRenderPassEncoderSetViewport(rp, 0, 0, (float)w, (float)h, 0, 1);
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
        wgpuRenderPassEncoderSetScissorRect(rp, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
        wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
    }
    wgpuBindGroupRelease(bg);
    g_bound = NULL, g_bound_bg = NULL, g_bound_uoff = ~0u;
}

/* --- copies and reads ----------------------------------------------------------------------------------------- */
void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!g_dev || !src || !dst || src->wf != dst->wf || src->use == GFX_USE_DEPTH || dst->use == GFX_USE_DEPTH)
        return;
    end_pass();
    enc();
    WGPUTexelCopyTextureInfo a = { src->tex, slevel, { sx, sy, sface }, WGPUTextureAspect_All };
    WGPUTexelCopyTextureInfo b = { dst->tex, dlevel, { dx, dy, dface }, WGPUTextureAspect_All };
    WGPUExtent3D ext = { w, h, 1 };
    wgpuCommandEncoderCopyTextureToTexture(g_enc, &a, &b, &ext);
    src->used = dst->used = g_enc_serial;
}

typedef struct Read
{
    WGPUBuffer buf;
    GfxTex* t;
    uint32_t level, bpr, w, h, pitch;
    void* dst;
    GfxqCall* call;
} Read;

/* the texture's texels back into D3D's layout (BGRA8 to the format the game made it with) */
static void on_mapped(WGPUMapAsyncStatus st, WGPUStringView msg, void* u1, void* u2)
{
    (void)msg, (void)u2;
    Read* r = (Read*)u1;
    if (st == WGPUMapAsyncStatus_Success)
    {
        const uint8_t* s = (const uint8_t*)wgpuBufferGetConstMappedRange(r->buf, 0, (size_t)r->bpr * r->h);
        uint8_t* d = (uint8_t*)r->dst;
        uint32_t bpp = d3d_bpp(r->t->fmt);
        for (uint32_t y = 0; s && y < r->h; ++y)
        {
            const uint8_t* sr = s + (size_t)y * r->bpr;
            uint8_t* dr = d + (size_t)y * r->pitch;
            if (bpp == 4 || r->t->texel == 2)
                memcpy(dr, sr, (size_t)r->w * (bpp == 4 ? 4 : 2));
            else
                for (uint32_t x = 0; x < r->w; ++x) /* 16-bit targets: 565 (the only one FFXI reads) */
                {
                    uint32_t b = sr[4 * x] >> 3, g = sr[4 * x + 1] >> 2, rr = sr[4 * x + 2] >> 3;
                    uint16_t v = (uint16_t)(rr << 11 | g << 5 | b);
                    memcpy(dr + 2 * x, &v, 2);
                }
        }
        wgpuBufferUnmap(r->buf);
    }
    wgpuBufferDestroy(r->buf);
    wgpuBufferRelease(r->buf);
    gfxq_call_done(r->call);
    free(r);
}

void gfx_tex_read_then(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch, GfxqCall* call)
{
    if (!g_dev || !t || t->use == GFX_USE_DEPTH || t->block || level >= t->levels)
    {
        gfxq_call_done(call);
        return;
    }
    Read* r = (Read*)calloc(1, sizeof *r);
    r->t = t, r->level = level, r->dst = dst, r->pitch = pitch, r->call = call;
    r->w = level_dim(t->w, level), r->h = level_dim(t->h, level);
    r->bpr = (r->w * t->texel + 255) & ~255u;
    WGPUBufferDescriptor bd = { 0 };
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    bd.size = (uint64_t)r->bpr * r->h;
    r->buf = wgpuDeviceCreateBuffer(g_dev, &bd);
    end_pass();
    enc();
    WGPUTexelCopyTextureInfo src = { t->tex, level, { 0, 0, face }, WGPUTextureAspect_All };
    WGPUTexelCopyBufferInfo out = { { 0, r->bpr, r->h }, r->buf };
    WGPUExtent3D ext = { r->w, r->h, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(g_enc, &src, &out, &ext);
    submit();
    WGPUBufferMapCallbackInfo cb = { 0 };
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = on_mapped;
    cb.userdata1 = r;
    wgpuBufferMapAsync(r->buf, WGPUMapMode_Read, 0, (size_t)r->bpr * r->h, cb);
}

/* the queue always reads through gfx_tex_read_then; these answer only if called in place */
void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)dst, (void)pitch;
}

/* not yet a frame-late copy: the game's lens-flare probe sees nothing (the default occlusion answer is the
 * front end's) */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t)
        return;
    uint32_t lh = level_dim(t->h, level);
    memset(dst, 0, (size_t)pitch * (t->block ? (lh + 3) / 4 : lh));
    (void)face;
}

/* --- present: the back buffer onto the canvas ---------------------------------------------------------------- */
static const char BLIT_WGSL[] =
    "@group(0) @binding(0) var src: texture_2d<f32>;\n"
    "@group(0) @binding(1) var smp: sampler;\n"
    "struct V { @builtin(position) pos: vec4f, @location(0) uv: vec2f, };\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> V {\n"
    "  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));\n"
    "  var o: V;\n  o.pos = vec4f(p[i], 0.0, 1.0);\n  o.uv = vec2f(p[i].x * 0.5 + 0.5, 0.5 - p[i].y * 0.5);\n  return o;\n"
    "}\n"
    "@fragment fn fs(v: V) -> @location(0) vec4f { return vec4f(textureSample(src, smp, v.uv).rgb, 1.0); }\n";

static WGPUShaderModule g_blit_mod;
static WGPUBindGroupLayout g_blit_bgl;
static WGPURenderPipeline g_blit;
static WGPUSampler g_blit_samp;

static WGPUShaderModule module_from(const char* wgsl)
{
    WGPUShaderSourceWGSL w = { 0 };
    w.chain.sType = WGPUSType_ShaderSourceWGSL;
    w.code = SV(wgsl);
    WGPUShaderModuleDescriptor md = { 0 };
    md.nextInChain = &w.chain;
    return wgpuDeviceCreateShaderModule(g_dev, &md);
}

static uint64_t g_serial; /* frames presented */

static void make_statics(void)
{
    fx_config();
    WGPUBufferDescriptor bd = { 0 };
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage | WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
    bd.size = RING_SIZE;
    g_ring = wgpuDeviceCreateBuffer(g_dev, &bd);
    g_ring_cpu = (uint8_t*)malloc(RING_SIZE);
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    bd.size = 256;
    g_dummy_buf = wgpuDeviceCreateBuffer(g_dev, &bd);
    uint32_t white[6] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    g_dummy2d = gfx_tex_create(GFX_TEX_2D, F_A8R8G8B8, 1, 1, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(g_dummy2d, 0, 0, white, 4);
    g_dummycube = gfx_tex_create(GFX_TEX_CUBE, F_A8R8G8B8, 1, 1, 1, GFX_USE_SAMPLE);
    for (uint32_t f = 0; f < 6; ++f)
        gfx_tex_upload(g_dummycube, f, 0, white, 4);

    g_clear_mod = module_from(CLEAR_WGSL);
    WGPUBindGroupLayoutEntry ce = { 0 };
    ce.binding = 0, ce.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    ce.buffer.type = WGPUBufferBindingType_Uniform, ce.buffer.hasDynamicOffset = 1, ce.buffer.minBindingSize = 32;
    WGPUBindGroupLayoutDescriptor ld = { 0 };
    ld.entryCount = 1, ld.entries = &ce;
    g_clear_bgl = wgpuDeviceCreateBindGroupLayout(g_dev, &ld);
    WGPUPipelineLayoutDescriptor pld = { 0 };
    pld.bindGroupLayoutCount = 1, pld.bindGroupLayouts = &g_clear_bgl;
    g_clear_pl = wgpuDeviceCreatePipelineLayout(g_dev, &pld);

    g_blit_mod = module_from(BLIT_WGSL);
    WGPUBindGroupLayoutEntry be[2];
    memset(be, 0, sizeof be);
    be[0].binding = 0, be[0].visibility = WGPUShaderStage_Fragment;
    be[0].texture.sampleType = WGPUTextureSampleType_Float, be[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    be[1].binding = 1, be[1].visibility = WGPUShaderStage_Fragment, be[1].sampler.type = WGPUSamplerBindingType_Filtering;
    ld.entryCount = 2, ld.entries = be;
    g_blit_bgl = wgpuDeviceCreateBindGroupLayout(g_dev, &ld);
    pld.bindGroupLayouts = &g_blit_bgl;
    WGPUPipelineLayout bpl = wgpuDeviceCreatePipelineLayout(g_dev, &pld);
    WGPUColorTargetState ct = { 0 };
    ct.format = g_surface_fmt, ct.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs = { 0 };
    fs.module = g_blit_mod, fs.entryPoint = SV("fs"), fs.targetCount = 1, fs.targets = &ct;
    WGPURenderPipelineDescriptor pd = { 0 };
    pd.layout = bpl;
    pd.vertex.module = g_blit_mod, pd.vertex.entryPoint = SV("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.multisample.count = 1, pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g_blit = wgpuDeviceCreateRenderPipeline(g_dev, &pd);
    WGPUSamplerDescriptor sd = { 0 };
    sd.magFilter = sd.minFilter = WGPUFilterMode_Linear;
    sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_ClampToEdge;
    sd.maxAnisotropy = 1, sd.lodMaxClamp = 32.0f;
    g_blit_samp = wgpuDeviceCreateSampler(g_dev, &sd);
}

/* The page's fps gauge (tools/web/hud.js): frames presented, and the longest gap between two since it last
 * asked (the worst frame), read from the page's thread. */
static volatile uint32_t g_hud_frames;
static volatile uint32_t g_hud_worst_us;
static uint64_t g_hud_last_ns;

EMSCRIPTEN_KEEPALIVE uint32_t web_hud_frames(void) { return g_hud_frames; }
EMSCRIPTEN_KEEPALIVE uint32_t web_hud_worst_us(void) { return __atomic_exchange_n(&g_hud_worst_us, 0, __ATOMIC_SEQ_CST); }

void gfx_present(GfxTex* backbuffer)
{
    if (!g_dev || g_init_state != 4)
        return;
    uint64_t t0 = gfx_now_ns();
    if (g_hud_last_ns)
    {
        uint32_t us = (uint32_t)((t0 - g_hud_last_ns) / 1000u);
        if (us > g_hud_worst_us)
            g_hud_worst_us = us;
    }
    g_hud_last_ns = t0;
    g_hud_frames++;
    g_serial++;
    if (g_serial % 30 == 0)
        fx_reload();
    if (g_clear_flags && g_rt) /* a clear nothing drew after: still a clear */
        begin_pass();
    end_pass();
    if (backbuffer && backbuffer->rtv)
    {
        surface_size(backbuffer->w, backbuffer->h);
        WGPUSurfaceTexture st = { 0 };
        wgpuSurfaceGetCurrentTexture(g_surface, &st);
        if (st.texture && (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
                              st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal))
        {
            enc();
            WGPUTextureView sv = wgpuTextureCreateView(st.texture, NULL);
            WGPUBindGroupEntry e[2] = { { 0 }, { 0 } };
            e[0].binding = 0, e[0].textureView = backbuffer->view;
            e[1].binding = 1, e[1].sampler = g_blit_samp;
            WGPUBindGroupDescriptor bd = { 0 };
            bd.layout = g_blit_bgl, bd.entryCount = 2, bd.entries = e;
            WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g_dev, &bd);
            WGPURenderPassColorAttachment ca = { 0 };
            ca.view = sv;
            ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            ca.loadOp = WGPULoadOp_Clear, ca.storeOp = WGPUStoreOp_Store;
            WGPURenderPassDescriptor pd = { 0 };
            pd.colorAttachmentCount = 1, pd.colorAttachments = &ca;
            WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(g_enc, &pd);
            wgpuRenderPassEncoderSetPipeline(rp, g_blit);
            wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 0, NULL);
            wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(rp);
            wgpuRenderPassEncoderRelease(rp);
            backbuffer->used = g_enc_serial;
            submit();
            wgpuBindGroupRelease(bg);
            wgpuTextureViewRelease(sv);
        }
        if (st.texture)
            wgpuTextureRelease(st.texture);
    }
    submit();
    if (gfx_profiling)
    {
        g_present_thread = plat_thread_id();
        prof_frame(t0);
    }
}

void gfx_resize(uint32_t w, uint32_t h) { surface_size(w, h); }

/* --- the rest of gfx.h --------------------------------------------------------------------------------------- */
void gfx_set_focus(const float* pos) { (void)pos; }
void gfx_set_moghouse(int in) { (void)in; }
int gfx_sun_shadows_shown(void) { return 0; }
float gfx_sun_prime(float* center) { (void)center; return 0.0f; }
/* the passes (gfx_webgpu_fx.h) are written; their driver comes next */
void gfx_scene_done(GfxTex* color, const GfxScene* s) { (void)color, (void)s; }
void gfx_trace_dump(const char* path) { (void)path; }
void gfx_finish(void) { submit(); }
uint32_t gfx_failures(void) { return g_failures; }
void gfx_set_sync_pipelines(int on) { g_sync_pipelines = on; }
