/* The graphics back end on Direct3D 12 (Windows x64): what gfx.h asks for, on one ID3D12Device.
 * The same design as gfx_metal.m, in D3D12's terms:
 *
 *   - Frames: one command list recording at a time, executed at Present (or when the CPU needs a
 *     result, or in chunks mid-frame so the GPU starts early); up to three frames in flight, each
 *     with its own command allocator and upload ring - vertex, index and uniform bytes are copied
 *     into the ring per draw, so the game may rewrite a buffer the moment a draw returns, as D3D
 *     lets it. One fence counts submissions; a resource remembers the submission that last used it.
 *   - Resource states: textures carry their state (whole resource) and transition on use; buffers
 *     decay to COMMON after every submission, so they are tracked per command list.
 *   - Binding: one root signature for everything - the uniforms (b0), the draw's texture and sampler
 *     heap indices as root constants (b1), the vertex streams as raw root SRVs (t0..t3), and the whole
 *     shader-visible heaps as unbounded Texture2D / TextureCube / sampler tables. A texture's SRV lives
 *     in the heap for its lifetime; a draw binds nothing but 16 indices.
 *   - Pipelines come from the generated HLSL (gfx_hlsl.c), compiled with D3DCompile (SM 5.1) and
 *     built off the game's thread, cached by key: shaders, blending, depth-stencil, rasterizer and
 *     topology type (D3D12 bakes all of them into the pipeline). Samplers are cached by key.
 *   - Clears are D3D12's own (with rectangles); the back buffer reaches the window through a flip
 *     swap chain on the SDL window, drawn scaled with the frame-rate overlay on top.
 *   - Textures are default-heap resources filled through the ring with CopyTextureRegion. Formats
 *     D3D12 has no sampled match for (the 16-bit color ones) are widened to BGRA8 on upload, as on
 *     Metal; the swizzled ones (X8R8G8B8, L8, A8L8) are SRV component mappings. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <SDL3/SDL.h>
#if defined(FFXI_UWP)
#include "uwp_bridge.h"
#endif
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cachedir.h"
#include "gfx.h"
#include "gfx_hlsl.h"
#include "gfx_rt_dxil.h"

#define FRAMES 3
#define GFX_PROBES 8 /* reads of one surface per frame that keep their own history */
#define GFX_READBACKS ((FRAMES + 1) * GFX_PROBES)
#define RING_CHUNK (8u << 20)
#define SRV_HEAP 65536u    /* shader-visible CBV/SRV/UAV descriptors: one per texture */
#define SAMPLER_HEAP 2048u /* the most a shader-visible sampler heap may hold */
#define RTV_HEAP 4096u
#define DSV_HEAP 512u
#define SRV_NULL_2D 0 /* heap slots holding null views: an unbound Texture2D / TextureCube */
#define SRV_NULL_CUBE 1

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

/* the root signature's parameters */
enum
{
    ROOT_U,       /* CBV b0: GfxU (the utility functions' own constants too) */
    ROOT_BIND,    /* 16 constants b1: texture heap indices ti[8], sampler heap indices si[8] */
    ROOT_STREAM0, /* raw SRVs t0..t3: the vertex streams */
    ROOT_TEX2D = ROOT_STREAM0 + GFX_NSTREAMS,
    ROOT_TEXCUBE,
    ROOT_SAMPLERS,
    ROOT_CMP, /* the same sampler heap as comparison samplers (space1): the scene effects' shadow maps */
    ROOT_SHADOW, /* CBV b2: drawn from the sun, the camera's clip space to the map (the shadow key) */
    ROOT_COUNT,
};

struct GfxTex
{
    ID3D12Resource* res;
    DXGI_FORMAT dxfmt;
    int type, use, conv;
    uint32_t fmt, w, h, levels; /* D3D's sizes */
    uint32_t rw, rh;            /* the resource's: DXT top levels round up to whole blocks */
    uint32_t faces;
    uint32_t block;       /* bytes per 4x4 block for the compressed formats, else 0 */
    uint32_t texel;       /* bytes per texel in D3D12's layout */
    uint64_t used;        /* the last submission that referenced it */
    int has_stencil, x8;
    D3D12_RESOURCE_STATES state;
    int32_t srv;          /* its slot in the shader-visible heap, or -1 (a depth surface's: its depth, for the scene effects) */
    int32_t* views;       /* RTV (or DSV) slots per subresource, created on first use; -1 until then */
    /* a large color target (FFXI's background) gets a whole mip chain beyond the game's one level: the
     * finished scene is made smaller through it (the scene filter, scene_mips). The game's view (srv)
     * sees level 0 alone; mip_srv the chain, lvl_srv / lvl_rtv one level each (made on first use) */
    uint32_t mips;
    int32_t mip_srv, *lvl_srv, *lvl_rtv;
    uint64_t scene;       /* its mips hold its first level as it is now (the frame they were made); 0 once drawn to */
    uint32_t filled;      /* the levels uploaded so far, a bit each */
    /* a color target's depth (the scene effects'): the depth surface its first level was last drawn
     * with, and the one the scene's casters (its world) were drawn with; cleared when that goes */
    GfxTex *depth_seen, *depth_world;
    /* asynchronous readbacks (gfx_tex_read_async): staging buffers and the submission each was recorded in */
    ID3D12Resource* rb[GFX_READBACKS];
    uint8_t* rb_cpu[GFX_READBACKS];
    uint64_t rb_size[GFX_READBACKS];
    uint64_t rb_serial[GFX_READBACKS];
    uint32_t rb_face[GFX_READBACKS], rb_level[GFX_READBACKS], rb_index[GFX_READBACKS];
    uint64_t rb_frame; /* the frame the reads below were counted in */
    uint32_t rb_count; /* reads of this surface so far in that frame */
};

/* Static buffers live in slabs: one resource carved into equal blocks of a power-of-two size (256 bytes
 * to 256 KB), or, for a larger buffer, a resource of its own. A committed resource per buffer cost
 * 0.7 ms each on an Xbox One X, and a zone load creates over a thousand. Resource states are the
 * slab's: a barrier covers every block in it. */
typedef struct Slab
{
    ID3D12Resource* res;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    D3D12_RESOURCE_STATES state; /* in command list `list`; COMMON in any other (buffers decay) */
    uint32_t list;
    int cls;                     /* the block size's class, or -1: the whole resource is one buffer */
    uint32_t* free_blocks;       /* offsets of the blocks not in use */
    uint32_t nfree;
    struct Slab* next;           /* the class's slabs */
} Slab;

struct GfxBuf
{
    Slab* slab;
    uint32_t off;                  /* in the slab's resource */
    D3D12_GPU_VIRTUAL_ADDRESS gpu; /* the slab's, plus off */
    uint32_t size;
    /* the bytes it was last filled from, which the caller keeps (d3d8.c: the buffer's own memory) and
     * which the sun's shadows read on the CPU - a copy's first vertex, a moving object's copy */
    const uint8_t* cpu;
    uint32_t cpu_size;
    uint64_t up_last, up_prev; /* the frames of its last two uploads (buf_volatile) */
};

/* a static buffer the game keeps rewriting (uploaded in two frames within the last 300): what is drawn
 * from it cannot be drawn again later from it, so the sun's cache keeps a copy instead */
static int buf_volatile(const GfxBuf* b);
static void sun_cache_forget(const GfxBuf* b);
static void sun_cache_forget_tex(const GfxTex* t);
static void casters_clear(void);

typedef struct Chunk
{
    ID3D12Resource* res;
    uint8_t* cpu;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    uint32_t used, size;
} Chunk;

typedef struct Frame
{
    ID3D12CommandAllocator* alloc;
    Chunk* chunks;
    uint32_t nchunks, cur;
    uint64_t fence;   /* the last submission of the frame */
    int timed;        /* its GPU timestamps were resolved (profile) */
} Frame;

static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_queue;
static ID3D12GraphicsCommandList* g_list;
static int g_list_open;
static uint32_t g_list_id;    /* counts command lists recorded (buffer state decay) */
static ID3D12Fence* g_fence;
static HANDLE g_fence_event;
static uint64_t g_fence_value; /* the last submission signalled */
static ID3D12RootSignature* g_root;
static Frame g_frames[FRAMES];
static uint32_t g_frame;       /* index into g_frames */
static uint64_t g_serial = 1;  /* the frame being recorded */
static int g_frame_open;
static uint32_t g_cmd_draws;   /* draws in the command list being recorded */
static uint64_t g_gpu_ns;      /* GPU time of finished frames (profile) */

/* descriptor heaps: shader-visible SRVs and samplers, CPU-only RTVs and DSVs */
typedef struct Heap
{
    ID3D12DescriptorHeap* heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    UINT inc;
    int32_t* free_slots;
    uint32_t nfree;
} Heap;

static Heap g_srv, g_samp, g_rtv, g_dsv;

static GfxTex* g_rt;
static uint32_t g_rt_face, g_rt_level;
static GfxTex** g_rts; /* every color target, for a depth surface going away to be forgotten by them */
static uint32_t g_nrts, g_rts_cap;
static GfxTex* g_ds;
static GfxTex* g_scratch_depth;
static int g_targets_bound;
static ID3D12PipelineState* g_bound_pso;
static int g_bound_topo;
/* what the command list has bound already, so a draw sets only what changed (unbind() forgets it all:
 * a new list, or state set outside draw_encode) */
static int g_bound_valid;
static uint32_t g_bound_bind[16];
static D3D12_GPU_VIRTUAL_ADDRESS g_bound_u, g_bound_stream[GFX_NSTREAMS];
static D3D12_VIEWPORT g_bound_vp;
static D3D12_RECT g_bound_sc;
static uint32_t g_bound_sref;
/* the last draw's uniforms and where they are in this frame's ring: the next draw with the same bytes
 * reuses them (the ring is reset when the frame is reused, so is this) */
static GfxU g_last_u;
static size_t g_last_u_need;
static D3D12_GPU_VIRTUAL_ADDRESS g_last_u_gpu;
static ID3D12Resource* g_dummy;
static ID3D12Resource* g_sync_rb; /* gfx_tex_read's staging buffer */
static uint64_t g_sync_rb_size;

/* the window */
static SDL_Window* g_window;
static IDXGIFactory4* g_factory;
static IDXGISwapChain3* g_swap;
static ID3D12Resource* g_swap_buf[FRAMES];
static D3D12_RESOURCE_STATES g_swap_state[FRAMES];
static int32_t g_swap_rtv[FRAMES];
static uint32_t g_swap_w, g_swap_h, g_want_w, g_want_h;
static int g_vsync, g_tearing;
static ID3D12PipelineState *g_present_pso, *g_overlay_pso;
static uint32_t g_present_samp;
static ID3D12PipelineState* g_present_cas_pso; /* the present, sharpened (g_fxs.sharpen) */

/* The scene effects' settings (fx_config, fx_reload): FFXI_FX_* in the environment, then the settings
 * file while the game runs. fx = 0 is the game as it was. */
typedef struct FxSettings
{
    float fx, ao, radius, grade, sat, contrast, sharpen, filter, aniso, fog, fog_falloff, fog_height, fog_max, fog_sun,
        fog_g, bloom, threshold, rays, rays_decay, rays_length, light, shadow, shadow_length, sun, sun_distance, sun_soft,
        sun_face, sun_min, sun_direct, sun_casters, sun_near, temporal, debug, draw, draw_entities, fps, aa, ao_quality,
        sun_detail, sun_dusk, water, water_refract, water_clarity, water_soft, water_foam, water_foam_width, water_ripple,
        water_scale, water_reflect, water_spec, lod, gameshadows, moghouse, gi, gi_radius, gi_distance, rt;
} FxSettings;
static FxSettings g_fxs = { .fps = 1.0f };

/* where the player stands in the world (gfx_set_focus), and the frame it was given */
static float g_focus[3];
static uint64_t g_focus_serial;

/* the frame-rate overlay (g_fxs.fps): presents counted over half-second windows */
static double g_fps_since;
static uint32_t g_fps_frames;
static char g_fps_text[32] = "-- FPS";

/* FFXI_D3D12_DEBUG=1: the debug layer's messages, printed after each submission */
static ID3D12InfoQueue* g_info;

/* GPU timestamps, two per frame (profile) */
static ID3D12QueryHeap* g_queries;
static ID3D12Resource* g_query_rb;
static uint64_t* g_query_cpu;
static uint64_t g_ts_freq;

/* --- small hash maps (key bytes -> pointer) ------------------------------------------------------------ */
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

static void* map_get(Map* m, const void* key, size_t klen)
{
    MapEnt* e = map_find(m, key, klen, fnv(key, klen));
    return e ? e->obj : NULL;
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
    uint64_t frames, draws, bytes, pipelines, front_ns, draw_ns, sem_ns, drawable_ns, present_ns, last_ns, since_ns;
    uint64_t skips[GFX_NSKIPS];
    uint64_t shim_ns, probe_ns;
} Prof;

static Prof g_prof;
static volatile LONG g_pipes_building;
/* the scene effects in the profile: scenes they ran on, scenes they left as they were (no depth of the
 * scene's size, no perspective, not ready yet) */
static uint32_t g_fx_ran, g_fx_no_depth, g_fx_no_proj, g_fx_not_ready;

uint64_t gfx_now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)freq.QuadPart);
}

void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason) { if (reason >= 0 && reason < GFX_NSKIPS) g_prof.skips[reason]++; }

static DWORD g_present_thread;

void gfx_prof_shim(uint64_t ns)
{
    if (GetCurrentThreadId() == g_present_thread)
        g_prof.shim_ns += ns;
}

static uint32_t g_pixel_fallbacks; /* draws lit per vertex while their per-pixel pipeline builds (the profile) */

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
    double gpu = (double)g_gpu_ns * ms;
    g_gpu_ns = 0;
    if (shims > 0)
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game code %.2f + API calls %.2f (draws %.2f [encode %.2f], probe wait %.2f, "
            "present %.2f [swap chain %.2f], other %.2f) | GPU %.2f ms | %.0f draws, %.0f KB up, %llu new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - shims, shims, front, enc, probe, pres, drw,
            shims - front - probe - pres, gpu, (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0,
            (unsigned long long)g_prof.pipelines);
    else
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game %.2f + d3d %.2f (encode %.2f) + present %.2f (gpu wait %.2f, swap chain %.2f) | "
            "GPU %.2f ms | %.0f draws, %.0f KB up, %llu new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - front - pres, front, enc, pres, sem, drw, gpu,
            (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0, (unsigned long long)g_prof.pipelines);
    static const char* const WHY[GFX_NSKIPS] = { "no shader", "stream>=4", "no position", "no buffer data", "range past buffer",
        "no indices", "pipeline not ready", "no target" };
    int any = 0;
    for (int i = 0; i < GFX_NSKIPS; ++i)
        if (g_prof.skips[i])
            fprintf(stderr, "%s%s %llu", any++ ? ", " : "[gfx]   skipped draws (2 s): ", WHY[i], (unsigned long long)g_prof.skips[i]);
    if (any)
        fprintf(stderr, "\n");
    if (g_pixel_fallbacks)
        fprintf(stderr, "[gfx]   lit per vertex while their per-pixel pipelines build (2 s): %u draws\n", g_pixel_fallbacks);
    g_pixel_fallbacks = 0;
    if (g_fx_ran || g_fx_no_depth || g_fx_no_proj || g_fx_not_ready || g_pipes_building)
        fprintf(stderr, "[gfx]   scene effects (2 s): ran %u, no depth %u, no perspective %u, not ready %u; %ld pipelines building\n",
            g_fx_ran, g_fx_no_depth, g_fx_no_proj, g_fx_not_ready, (long)g_pipes_building);
    g_fx_ran = g_fx_no_depth = g_fx_no_proj = g_fx_not_ready = 0;
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since_ns = now;
}

static volatile LONG g_failures;

uint32_t gfx_failures(void) { return (uint32_t)g_failures; }

/* --- descriptor heaps ------------------------------------------------------------------------------------- */
static int heap_init(Heap* h, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t n, int visible, uint32_t reserved)
{
    D3D12_DESCRIPTOR_HEAP_DESC d = { type, n, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_dev, &d, &IID_ID3D12DescriptorHeap, (void**)&h->heap)))
        return 0;
    // through the vtable: the Windows SDK's C macro takes the out-parameter, mingw-w64's (WIDL) has only
    // an inline wrapper that returns the handle; the vtable entry is the same in both
    h->heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(h->heap, &h->cpu);
    if (visible)
        h->heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(h->heap, &h->gpu);
    h->inc = ID3D12Device_GetDescriptorHandleIncrementSize(g_dev, type);
    h->free_slots = (int32_t*)malloc(sizeof(int32_t) * n);
    h->nfree = 0;
    for (uint32_t i = n; i-- > reserved;) /* low slots first */
        h->free_slots[h->nfree++] = (int32_t)i;
    return 1;
}

static int32_t heap_alloc(Heap* h)
{
    return h->nfree ? h->free_slots[--h->nfree] : -1;
}

static void heap_free(Heap* h, int32_t slot)
{
    if (slot >= 0)
        h->free_slots[h->nfree++] = slot;
}

static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(const Heap* h, int32_t slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c = { h->cpu.ptr + (SIZE_T)slot * h->inc };
    return c;
}

/* --- lifetimes: what the GPU may still read is released once the fence passes it --------------------------- */
typedef struct Dead
{
    IUnknown* obj;
    Heap* heap; /* or a descriptor slot of this heap */
    int32_t slot;
    uint64_t fence;
    Slab* slab; /* or a slab's block, at offset slot */
} Dead;

static Dead* g_dead;
static uint32_t g_ndead, g_cap_dead;

static uint64_t completed(void)
{
    return ID3D12Fence_GetCompletedValue(g_fence);
}

/* the submission the commands being recorded now will belong to */
static uint64_t pending(void)
{
    return g_fence_value + 1;
}

static void defer(IUnknown* obj, Heap* heap, int32_t slot)
{
    if (!obj && (!heap || slot < 0))
        return;
    if (g_ndead == g_cap_dead)
    {
        g_cap_dead = g_cap_dead ? g_cap_dead * 2 : 256;
        g_dead = (Dead*)realloc(g_dead, g_cap_dead * sizeof *g_dead);
    }
    g_dead[g_ndead++] = (Dead){ obj, heap, slot, pending(), NULL };
}

/* a slab's block, back in its free list once the GPU is past what may read it */
static void defer_block(Slab* s, uint32_t off)
{
    if (g_ndead == g_cap_dead)
    {
        g_cap_dead = g_cap_dead ? g_cap_dead * 2 : 256;
        g_dead = (Dead*)realloc(g_dead, g_cap_dead * sizeof *g_dead);
    }
    g_dead[g_ndead++] = (Dead){ NULL, NULL, (int32_t)off, pending(), s };
}

static void collect(void)
{
    uint64_t done = completed();
    uint32_t k = 0;
    for (uint32_t i = 0; i < g_ndead; ++i)
    {
        Dead* d = &g_dead[i];
        if (d->fence <= done)
        {
            if (d->slab)
                d->slab->free_blocks[d->slab->nfree++] = (uint32_t)d->slot;
            else if (d->obj)
                IUnknown_Release(d->obj);
            else
                heap_free(d->heap, d->slot);
        }
        else
            g_dead[k++] = *d;
    }
    g_ndead = k;
}

static void print_messages(void)
{
    UINT64 n = ID3D12InfoQueue_GetNumStoredMessages(g_info);
    for (UINT64 i = 0; i < n; ++i)
    {
        SIZE_T len = 0;
        ID3D12InfoQueue_GetMessage(g_info, i, NULL, &len);
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
        if (m && SUCCEEDED(ID3D12InfoQueue_GetMessage(g_info, i, m, &len)))
            fprintf(stderr, "[d3d12] %s\n", m->pDescription);
        free(m);
    }
    ID3D12InfoQueue_ClearStoredMessages(g_info);
}

static void wait_fence(uint64_t v)
{
    if (completed() >= v)
        return;
    ID3D12Fence_SetEventOnCompletion(g_fence, v, g_fence_event);
    WaitForSingleObject(g_fence_event, INFINITE);
}

/* --- frames, command lists and the upload ring ------------------------------------------------------------ */
static void frame_begin(void)
{
    if (g_frame_open)
        return;
    Frame* f = &g_frames[g_frame];
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    wait_fence(f->fence);
    if (gfx_profiling)
    {
        g_prof.sem_ns += gfx_now_ns() - t0;
        if (f->timed && g_query_cpu && g_ts_freq)
        {
            uint64_t a = g_query_cpu[2 * g_frame], b = g_query_cpu[2 * g_frame + 1];
            if (b > a)
                g_gpu_ns += (uint64_t)((double)(b - a) * 1e9 / (double)g_ts_freq);
        }
    }
    f->timed = 0;
    ID3D12CommandAllocator_Reset(f->alloc);
    for (uint32_t i = 0; i < f->nchunks; ++i)
        f->chunks[i].used = 0;
    f->cur = 0;
    g_last_u_need = 0;
    collect();
    g_frame_open = 1;
}

/* the command list being recorded, opened (and set up) if none is */
static ID3D12GraphicsCommandList* list(void)
{
    frame_begin();
    if (g_list_open)
        return g_list;
    Frame* f = &g_frames[g_frame];
    ID3D12GraphicsCommandList_Reset(g_list, f->alloc, NULL);
    ID3D12DescriptorHeap* heaps[2] = { g_srv.heap, g_samp.heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(g_list, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(g_list, g_root);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_TEX2D, g_srv.gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_TEXCUBE, g_srv.gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_SAMPLERS, g_samp.gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_CMP, g_samp.gpu);
    if (gfx_profiling && g_queries && !f->timed)
    {
        ID3D12GraphicsCommandList_EndQuery(g_list, g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * g_frame);
        f->timed = 1;
    }
    g_list_open = 1;
    g_list_id++;
    g_targets_bound = 0;
    g_bound_pso = NULL;
    g_bound_topo = -1;
    g_bound_valid = 0;
    return g_list;
}

static void unbind(void) { g_bound_valid = 0; }

/* executes what is recorded; the frame stays open (its ring is still in use) */
static void submit(int wait)
{
    if (!g_list_open)
        return;
    ID3D12GraphicsCommandList_Close(g_list);
    ID3D12CommandList* l = (ID3D12CommandList*)g_list;
    ID3D12CommandQueue_ExecuteCommandLists(g_queue, 1, &l);
    ID3D12CommandQueue_Signal(g_queue, g_fence, ++g_fence_value);
    g_frames[g_frame].fence = g_fence_value;
    g_list_open = 0;
    g_cmd_draws = 0;
    if (wait)
        wait_fence(g_fence_value);
    if (g_info)
        print_messages();
}

static ID3D12Resource* make_buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource* r = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void**)&r)))
    {
        fprintf(stderr, "[recomp] gfx: buffer of %llu bytes failed\n", (unsigned long long)size);
        return NULL;
    }
    return r;
}

typedef struct Alloc
{
    uint8_t* cpu;
    ID3D12Resource* res;
    uint64_t off;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
} Alloc;

/* n bytes of this frame's ring (a large request gets an upload buffer of its own) */
static Alloc ring(size_t n, size_t align)
{
    frame_begin();
    Alloc a = { 0 };
    g_prof.bytes += n;
    if (n > RING_CHUNK / 2)
    {
        a.res = make_buffer(n, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!a.res)
            return a;
        ID3D12Resource_Map(a.res, 0, NULL, (void**)&a.cpu);
        a.gpu = ID3D12Resource_GetGPUVirtualAddress(a.res);
        defer((IUnknown*)a.res, NULL, -1); /* released once this frame's work is done */
        return a;
    }
    Frame* f = &g_frames[g_frame];
    for (;; f->cur++)
    {
        if (f->cur == f->nchunks)
        {
            Chunk c = { 0 };
            c.res = make_buffer(RING_CHUNK, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!c.res)
                return a;
            ID3D12Resource_Map(c.res, 0, NULL, (void**)&c.cpu);
            c.gpu = ID3D12Resource_GetGPUVirtualAddress(c.res);
            c.size = RING_CHUNK;
            f->chunks = (Chunk*)realloc(f->chunks, (f->nchunks + 1) * sizeof(Chunk));
            f->chunks[f->nchunks++] = c;
        }
        Chunk* c = &f->chunks[f->cur];
        uint32_t at = (uint32_t)((c->used + align - 1) & ~(align - 1));
        if (at + n <= c->size)
        {
            c->used = at + (uint32_t)n;
            a.cpu = c->cpu + at, a.res = c->res, a.off = at, a.gpu = c->gpu + at;
            return a;
        }
    }
}

static void barrier_sub(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list(), 1, &b);
}

static void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    barrier_sub(r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after);
}

static void tex_state(GfxTex* t, D3D12_RESOURCE_STATES want)
{
    if (t->state != want)
    {
        barrier(t->res, t->state, want);
        t->state = want;
    }
    t->used = pending();
}

#define BUF_READ (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER)

static void buf_state(GfxBuf* b, D3D12_RESOURCE_STATES want)
{
    list();
    Slab* s = b->slab;
    D3D12_RESOURCE_STATES cur = s->list == g_list_id ? s->state : D3D12_RESOURCE_STATE_COMMON;
    if (cur != want)
        barrier(s->res, cur, want);
    s->state = want, s->list = g_list_id;
}

/* --- formats ---------------------------------------------------------------------------------------------- */
#define MAP4(r, g, b, a) D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a)
#define ONE D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1

static DXGI_FORMAT dx_format(uint32_t fmt, int use, int* conv, uint32_t* block, uint32_t* texel, UINT* mapping)
{
    *conv = CONV_NONE, *block = 0, *texel = 4;
    *mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (use == GFX_USE_DEPTH)
        return fmt == F_D16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
    switch (fmt)
    {
    case F_A8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_X8R8G8B8: *mapping = MAP4(0, 1, 2, ONE); return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_R5G6B5: *conv = CONV_565; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_X1R5G5B5: *conv = CONV_X555; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A1R5G5B5: *conv = CONV_1555; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A4R4G4B4: *conv = CONV_4444; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A8: *texel = 1; return DXGI_FORMAT_A8_UNORM;
    case F_L8: *texel = 1, *mapping = MAP4(0, 0, 0, ONE); return DXGI_FORMAT_R8_UNORM;
    case F_A8L8: *texel = 2, *mapping = MAP4(0, 0, 0, 1); return DXGI_FORMAT_R8G8_UNORM;
    case F_V8U8: *texel = 2; return DXGI_FORMAT_R8G8_SNORM;
    }
    if (fmt == FOURCC('D', 'X', 'T', '1'))
        return *block = 8, DXGI_FORMAT_BC1_UNORM;
    if (fmt == FOURCC('D', 'X', 'T', '2') || fmt == FOURCC('D', 'X', 'T', '3'))
        return *block = 16, DXGI_FORMAT_BC2_UNORM;
    if (fmt == FOURCC('D', 'X', 'T', '4') || fmt == FOURCC('D', 'X', 'T', '5'))
        return *block = 16, DXGI_FORMAT_BC3_UNORM;
    return DXGI_FORMAT_B8G8R8A8_UNORM;
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
#define SLAB_SIZE (2u << 20)
#define SLAB_MIN_SHIFT 8  /* 256-byte blocks, the smallest class */
#define SLAB_CLASSES 11   /* up to 256 KB blocks; larger buffers get a resource each */
static Slab* g_slabs[SLAB_CLASSES];

static Slab* slab_new(uint64_t size, int cls)
{
    Slab* s = (Slab*)calloc(1, sizeof *s);
    s->res = make_buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    if (!s->res)
    {
        free(s);
        return NULL;
    }
    s->gpu = ID3D12Resource_GetGPUVirtualAddress(s->res);
    s->state = D3D12_RESOURCE_STATE_COMMON;
    s->cls = cls;
    if (cls >= 0)
    {
        uint32_t block = 1u << (SLAB_MIN_SHIFT + cls), n = SLAB_SIZE / block;
        s->free_blocks = (uint32_t*)malloc(n * sizeof *s->free_blocks);
        for (uint32_t i = 0; i < n; ++i) /* handed out from offset 0 up */
            s->free_blocks[i] = (n - 1 - i) * block;
        s->nfree = n;
    }
    return s;
}

GfxBuf* gfx_buf_create(uint32_t size)
{
    if (!g_dev || !size)
        return NULL;
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    b->size = size;
    uint32_t need = (size + 15) & ~15u;
    int cls = 0;
    while (cls < SLAB_CLASSES && (1u << (SLAB_MIN_SHIFT + cls)) < need)
        cls++;
    if (cls == SLAB_CLASSES)
    {
        b->slab = slab_new(need, -1);
        b->off = 0;
    }
    else
    {
        Slab* s = g_slabs[cls];
        while (s && !s->nfree)
            s = s->next;
        if (!s && (s = slab_new(SLAB_SIZE, cls)) != NULL)
            s->next = g_slabs[cls], g_slabs[cls] = s;
        b->slab = s;
        if (s)
            b->off = s->free_blocks[--s->nfree];
    }
    if (!b->slab)
    {
        free(b);
        return NULL;
    }
    b->gpu = b->slab->gpu + b->off;
    return b;
}

static int buf_volatile(const GfxBuf* b)
{
    return b->up_prev && b->up_last - b->up_prev <= 300 && g_serial - b->up_last <= 300;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    sun_cache_forget(b);
    if (b->slab->cls < 0)
    {
        defer((IUnknown*)b->slab->res, NULL, -1);
        free(b->slab);
    }
    else
        defer_block(b->slab, b->off);
    free(b);
}

/* in command order: draws recorded before this read the old contents, draws after the new */
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b)
        return;
    if (size > b->size)
        size = b->size;
    if (!size)
        return;
    sun_cache_forget(b); /* new contents: what the cache drew from it is no longer it */
    b->cpu = (const uint8_t*)data, b->cpu_size = size;
    if (b->up_last != g_serial)
        b->up_prev = b->up_last, b->up_last = g_serial;
    Alloc a = ring(size, 16);
    if (!a.cpu)
        return;
    memcpy(a.cpu, data, size);
    buf_state(b, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(list(), b->slab->res, b->off, a.res, a.off, size);
    buf_state(b, BUF_READ);
}

/* --- textures ------------------------------------------------------------------------------------------------ */
static uint32_t subresource(const GfxTex* t, uint32_t face, uint32_t level)
{
    return level + face * t->levels;
}

GfxTex* gfx_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    UINT mapping;
    t->dxfmt = dx_format(fmt, use, &t->conv, &t->block, &t->texel, &mapping);
    t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->levels = levels ? levels : 1;
    if (type == GFX_TEX_CUBE)
        t->h = t->w;
    t->faces = type == GFX_TEX_CUBE ? 6 : 1;
    t->rw = t->w, t->rh = t->h;
    if (t->block) /* D3D12 wants the top level of a block-compressed texture in whole blocks */
        t->rw = (t->rw + 3) & ~3u, t->rh = (t->rh + 3) & ~3u;
    uint32_t most = 1;
    for (uint32_t s = t->rw > t->rh ? t->rw : t->rh; s > 1; s >>= 1)
        most++;
    if (t->levels > most)
        t->levels = most;
    t->has_stencil = t->dxfmt == DXGI_FORMAT_D24_UNORM_S8_UINT;
    t->x8 = fmt == F_X8R8G8B8;
    t->srv = t->mip_srv = -1;
    t->mips = t->levels;
    /* a large color target gets a whole mip chain: the finished scene is made smaller through it
     * rather than one bilinear sample per screen pixel (scene_mips) */
    if (use == GFX_USE_RT && type == GFX_TEX_2D && t->levels == 1 && t->w >= 1024 && t->h >= 1024)
        while ((t->w >> t->mips) || (t->h >> t->mips))
            t->mips++;
    D3D12_RESOURCE_FLAGS flags = use == GFX_USE_RT ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
        : use == GFX_USE_DEPTH ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                               : D3D12_RESOURCE_FLAG_NONE;
    t->state = use == GFX_USE_RT ? D3D12_RESOURCE_STATE_RENDER_TARGET
        : use == GFX_USE_DEPTH  ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                : D3D12_RESOURCE_STATE_COPY_DEST;
    /* depth is typeless: drawn to through its depth-stencil view, read by the scene effects as depth */
    DXGI_FORMAT res_fmt = t->dxfmt, depth_srv = DXGI_FORMAT_UNKNOWN;
    if (use == GFX_USE_DEPTH)
    {
        int d16 = t->dxfmt == DXGI_FORMAT_D16_UNORM;
        res_fmt = d16 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
        depth_srv = d16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    }
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, t->rw, t->rh, (UINT16)t->faces, (UINT16)t->mips, res_fmt,
        { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags };
    if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, t->state, NULL, &IID_ID3D12Resource,
            (void**)&t->res)))
    {
        fprintf(stderr, "[recomp] gfx: texture %ux%u format %u failed\n", w, h, fmt);
        free(t);
        return NULL;
    }
    t->srv = heap_alloc(&g_srv);
    if (t->srv >= 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC v;
        memset(&v, 0, sizeof v);
        v.Format = use == GFX_USE_DEPTH ? depth_srv : t->dxfmt;
        v.Shader4ComponentMapping = use == GFX_USE_DEPTH ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING : mapping;
        if (type == GFX_TEX_CUBE)
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE, v.TextureCube.MipLevels = t->levels;
        else
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MipLevels = t->levels;
        ID3D12Device_CreateShaderResourceView(g_dev, t->res, &v, heap_cpu(&g_srv, t->srv));
        if (t->mips > t->levels && (t->mip_srv = heap_alloc(&g_srv)) >= 0)
        {
            v.Texture2D.MipLevels = t->mips;
            ID3D12Device_CreateShaderResourceView(g_dev, t->res, &v, heap_cpu(&g_srv, t->mip_srv));
        }
    }
    else
        fprintf(stderr, "[recomp] gfx: out of texture descriptors\n");
    if (use != GFX_USE_SAMPLE)
    {
        uint32_t n = t->faces * t->levels;
        t->views = (int32_t*)malloc(sizeof(int32_t) * n);
        for (uint32_t i = 0; i < n; ++i)
            t->views[i] = -1;
    }
    if (use == GFX_USE_RT)
    {
        if (g_nrts == g_rts_cap)
            g_rts_cap = g_rts_cap ? g_rts_cap * 2 : 64, g_rts = (GfxTex**)realloc(g_rts, g_rts_cap * sizeof *g_rts);
        g_rts[g_nrts++] = t;
    }
    return t;
}

static void release_views(GfxTex* t)
{
    if (!t->views)
        return;
    for (uint32_t i = 0; i < t->faces * t->levels; ++i)
        defer(NULL, t->use == GFX_USE_DEPTH ? &g_dsv : &g_rtv, t->views[i]);
    free(t->views);
    t->views = NULL;
}

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (g_rt == t)
        g_rt = NULL, g_targets_bound = 0;
    if (g_ds == t)
        g_ds = NULL, g_targets_bound = 0;
    sun_cache_forget_tex(t);
    defer((IUnknown*)t->res, NULL, -1);
    defer(NULL, &g_srv, t->srv);
    defer(NULL, &g_srv, t->mip_srv);
    for (uint32_t i = 0; t->lvl_srv && i < t->mips; ++i)
        defer(NULL, &g_srv, t->lvl_srv[i]), defer(NULL, &g_rtv, t->lvl_rtv[i]);
    free(t->lvl_srv);
    free(t->lvl_rtv);
    release_views(t);
    for (int i = 0; i < GFX_READBACKS; ++i)
        defer((IUnknown*)t->rb[i], NULL, -1);
    for (uint32_t i = 0; i < g_nrts; ++i)
    {
        if (g_rts[i] == t)
            g_rts[i--] = g_rts[--g_nrts];
        else if (g_rts[i]->depth_seen == t || g_rts[i]->depth_world == t)
        {
            if (g_rts[i]->depth_seen == t)
                g_rts[i]->depth_seen = NULL;
            if (g_rts[i]->depth_world == t)
                g_rts[i]->depth_world = NULL;
        }
    }
    free(t);
}

static void level_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = t->w >> level ? t->w >> level : 1;
    *h = t->h >> level ? t->h >> level : 1;
}

/* the resource's own size of a level (differs from D3D's only for rounded-up DXT) */
static void phys_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = t->rw >> level ? t->rw >> level : 1;
    *h = t->rh >> level ? t->rh >> level : 1;
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels || face >= t->faces || !w || !h || t->use == GFX_USE_DEPTH)
        return;
    uint32_t lw, lh;
    level_size(t, level, &lw, &lh);
    if (x >= lw || y >= lh)
        return;
    if (x + w > lw)
        w = lw - x;
    if (y + h > lh)
        h = lh - y;
    uint32_t rows = t->block ? (h + 3) / 4 : h;
    uint32_t cols = t->block ? (w + 3) / 4 : w; /* blocks or texels per row */
    if (t->block)
    {
        uint32_t pw, ph;
        phys_size(t, level, &pw, &ph);
        uint32_t most_c = (pw + 3) / 4 - x / 4, most_r = (ph + 3) / 4 - y / 4;
        cols = cols < most_c ? cols : most_c;
        rows = rows < most_r ? rows : most_r;
    }
    uint32_t row_bytes = t->block ? cols * t->block : cols * t->texel;
    uint32_t up_pitch = (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    Alloc a = ring((size_t)up_pitch * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t r = 0; r < rows; ++r)
    {
        if (t->conv)
            convert_row(t->conv, s + (size_t)r * pitch, a.cpu + (size_t)r * up_pitch, cols);
        else
            memcpy(a.cpu + (size_t)r * up_pitch, s + (size_t)r * pitch, row_bytes);
    }
    tex_state(t, D3D12_RESOURCE_STATE_COPY_DEST);
    if (level < 32)
        t->filled |= 1u << level;
    D3D12_TEXTURE_COPY_LOCATION dst, from;
    memset(&dst, 0, sizeof dst);
    memset(&from, 0, sizeof from);
    dst.pResource = t->res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = subresource(t, face, level);
    from.pResource = a.res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Offset = a.off;
    from.PlacedFootprint.Footprint.Format = t->dxfmt;
    from.PlacedFootprint.Footprint.Width = t->block ? cols * 4 : cols;
    from.PlacedFootprint.Footprint.Height = t->block ? rows * 4 : rows;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = up_pitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &dst, x, y, 0, &from, NULL);
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    gfx_tex_upload_rect(t, face, level, 0, 0, w, h, src, pitch);
}

/* rows of a level read back in D3D12's layout -> D3D's */
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

static uint32_t readback_row(const GfxTex* t, uint32_t w)
{
    return (w * t->texel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

/* a copy of the level into buf, recorded with the frame's other work */
static void queue_readback(GfxTex* t, uint32_t face, uint32_t level, uint32_t w, uint32_t h, uint32_t row, ID3D12Resource* buf)
{
    tex_state(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst, from;
    memset(&dst, 0, sizeof dst);
    memset(&from, 0, sizeof from);
    from.pResource = t->res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = subresource(t, face, level);
    dst.pResource = buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = t->dxfmt;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = row;
    D3D12_BOX box = { 0, 0, 0, w, h, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &dst, 0, 0, 0, &from, &box);
}

void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || face >= t->faces || t->use == GFX_USE_DEPTH || t->block)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    uint32_t row = readback_row(t, w);
    uint64_t size = (uint64_t)row * h;
    if (size > g_sync_rb_size)
    {
        defer((IUnknown*)g_sync_rb, NULL, -1);
        g_sync_rb = make_buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        g_sync_rb_size = g_sync_rb ? size : 0;
        if (!g_sync_rb)
            return;
    }
    queue_readback(t, face, level, w, h, row, g_sync_rb);
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    submit(1);
    if (gfx_profiling)
        g_prof.probe_ns += gfx_now_ns() - t0;
    uint8_t* p = NULL;
    D3D12_RANGE r = { 0, (SIZE_T)size };
    if (SUCCEEDED(ID3D12Resource_Map(g_sync_rb, 0, &r, (void**)&p)))
    {
        copy_out(t, p, w, h, row, dst, pitch);
        D3D12_RANGE none = { 0, 0 };
        ID3D12Resource_Unmap(g_sync_rb, 0, &none);
    }
}

/* A surface read several times a frame (the game reuses one 16x16 target for more than one
 * probe: copy a region, lock, read; copy another, lock, read) keeps each read's history apart:
 * the k-th read this frame gets the k-th read of the newest frame the GPU has finished, never
 * another probe's pixels. */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || face >= t->faces || t->use == GFX_USE_DEPTH || t->block)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    uint32_t row = readback_row(t, w);
    uint64_t size = (uint64_t)row * h;
    if (t->rb_frame != g_serial)
        t->rb_frame = g_serial, t->rb_count = 0;
    uint32_t index = t->rb_count++;
    if (index >= GFX_PROBES)
    {
        gfx_tex_read(t, face, level, dst, pitch); /* more reads a frame than we keep apart */
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
        copy_out(t, t->rb_cpu[best], w, h, row, dst, pitch);
    /* this read's copy for a later frame: a slot the GPU is done with, not the one just read */
    done = completed();
    int slot = -1;
    for (int i = 0; i < GFX_READBACKS && slot < 0; ++i)
        if (i != best && (!t->rb_serial[i] || t->rb_serial[i] <= done))
            slot = i;
    if (slot < 0)
        return;
    if (!t->rb[slot] || t->rb_size[slot] < size)
    {
        defer((IUnknown*)t->rb[slot], NULL, -1);
        t->rb[slot] = make_buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        t->rb_size[slot] = t->rb[slot] ? size : 0;
        t->rb_cpu[slot] = NULL;
        if (!t->rb[slot])
            return;
        ID3D12Resource_Map(t->rb[slot], 0, NULL, (void**)&t->rb_cpu[slot]); /* readback memory stays mapped */
    }
    queue_readback(t, face, level, w, h, row, t->rb[slot]);
    t->rb_serial[slot] = pending(), t->rb_face[slot] = face, t->rb_level[slot] = level, t->rb_index[slot] = index;
}

void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || src == dst || src->dxfmt != dst->dxfmt || src->conv != dst->conv || !w || !h)
        return;
    if (slevel >= src->levels || dlevel >= dst->levels || sface >= src->faces || dface >= dst->faces)
        return;
    if (src->use == GFX_USE_DEPTH) /* D3D12 copies depth whole subresources only */
    {
        uint32_t sw, sh, dw, dh;
        level_size(src, slevel, &sw, &sh);
        level_size(dst, dlevel, &dw, &dh);
        if (sx || sy || dx || dy || w != sw || h != sh || sw != dw || sh != dh)
            return;
    }
    tex_state(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    tex_state(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    from.pResource = src->res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = subresource(src, sface, slevel);
    to.pResource = dst->res;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = subresource(dst, dface, dlevel);
    D3D12_BOX box = { sx, sy, 0, sx + w, sy + h, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, dx, dy, 0, &from, &box);
    if (!dface && !dlevel)
        dst->scene = 0; /* its mips are behind (scene_mips) */
}

/* --- render targets ---------------------------------------------------------------------------------------------- */
/* Draws recorded since the last submission. A frame goes to the GPU in chunks - executed when the
 * targets change with this many behind it, or mid-scene every SPLIT_DRAWS - so the GPU works while
 * the game builds the rest, and a mid-frame readback (the game's per-frame probe) waits only for
 * the tail. */
#define CHUNK_DRAWS 96
#define SPLIT_DRAWS 320

static D3D12_CPU_DESCRIPTOR_HANDLE target_view(GfxTex* t, uint32_t face, uint32_t level)
{
    uint32_t i = subresource(t, face, level);
    Heap* h = t->use == GFX_USE_DEPTH ? &g_dsv : &g_rtv;
    if (t->views[i] < 0)
    {
        t->views[i] = heap_alloc(h);
        if (t->views[i] < 0)
        {
            fprintf(stderr, "[recomp] gfx: out of %s descriptors\n", t->use == GFX_USE_DEPTH ? "DSV" : "RTV");
            t->views[i] = 0;
        }
        if (t->use == GFX_USE_DEPTH)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC v;
            memset(&v, 0, sizeof v);
            v.Format = t->dxfmt;
            v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipSlice = level;
            ID3D12Device_CreateDepthStencilView(g_dev, t->res, &v, heap_cpu(h, t->views[i]));
        }
        else
        {
            D3D12_RENDER_TARGET_VIEW_DESC v;
            memset(&v, 0, sizeof v);
            v.Format = t->dxfmt;
            if (t->type == GFX_TEX_CUBE)
            {
                v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray.MipSlice = level, v.Texture2DArray.FirstArraySlice = face, v.Texture2DArray.ArraySize = 1;
            }
            else
                v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D, v.Texture2D.MipSlice = level;
            ID3D12Device_CreateRenderTargetView(g_dev, t->res, &v, heap_cpu(h, t->views[i]));
        }
    }
    return heap_cpu(h, t->views[i]);
}

static void color_size(uint32_t* w, uint32_t* h)
{
    level_size(g_rt, g_rt_level, w, h);
}

/* the depth surface for the current color target: the bound one, or a scratch one of the color
 * target's size when they differ (D3D8 lets depth be larger; D3D12 wants them equal) */
static GfxTex* depth_attachment(void)
{
    if (!g_ds || !g_rt)
        return NULL;
    uint32_t w, h;
    color_size(&w, &h);
    if (g_ds->w == w && g_ds->h == h)
        return g_ds;
    if (!g_scratch_depth || g_scratch_depth->w != w || g_scratch_depth->h != h || g_scratch_depth->dxfmt != g_ds->dxfmt)
    {
        gfx_tex_destroy(g_scratch_depth);
        g_scratch_depth = gfx_tex_create(GFX_TEX_2D, g_ds->fmt, w, h, 1, GFX_USE_DEPTH);
    }
    return g_scratch_depth;
}

/* the targets bound on the command list, in their target states */
static int bind_targets(void)
{
    if (!g_rt)
        return 0;
    ID3D12GraphicsCommandList* l = list();
    GfxTex* ds = depth_attachment();
    if (g_rt->state != D3D12_RESOURCE_STATE_RENDER_TARGET || (ds && ds->state != D3D12_RESOURCE_STATE_DEPTH_WRITE))
        g_targets_bound = 0;
    tex_state(g_rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (ds)
        tex_state(ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (!g_rt_face && !g_rt_level)
    {
        g_rt->scene = 0; /* drawn to: its mips are behind */
        if (ds)
            g_rt->depth_seen = ds;
    }
    if (!g_targets_bound)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = target_view(g_rt, g_rt_face, g_rt_level), dsv;
        if (ds)
            dsv = target_view(ds, 0, 0);
        ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, ds ? &dsv : NULL);
        uint32_t w, h;
        color_size(&w, &h);
        D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
        ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
        g_bound_sc = sc;
        g_targets_bound = 1;
    }
    return 1;
}

void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (color == g_rt && face == g_rt_face && level == g_rt_level && depth == g_ds)
        return;
    if (g_cmd_draws >= CHUNK_DRAWS)
        submit(0);
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
    g_targets_bound = 0;
}

/* --- pipelines, built off the game's thread ---------------------------------------------------------------
 * A pipeline for a key seen for the first time is built on the thread pool; the draws that need it
 * are skipped until it is ready (a new effect may miss its first frames; the game never waits for
 * the shader compiler). Every key built is recorded in the pipeline cache file, and at start-up the
 * recorded keys are built again in the background, so a second session has them before it needs
 * them. gfx_set_sync_pipelines(1) (the tests) builds in place and records nothing. */
typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

typedef struct PipeKey
{
    LibKey lib;
    GfxPipeKey pipe;
    GfxDepthKey depth;
    uint32_t color, dsv; /* DXGI_FORMAT */
    int32_t zbias;
    uint8_t x8, cull, fill, topo; /* topo: D3D12_PRIMITIVE_TOPOLOGY_TYPE */
} PipeKey;

#define PIPE_FAILED ((void*)1)
#define PIPE_MAGIC 0x31443344u /* "D3D1" */

typedef struct PipeEntry
{
    void* volatile state; /* NULL while building, PIPE_FAILED, or the ID3D12PipelineState */
} PipeEntry;

typedef struct PipeJob
{
    PipeKey k;
    uint32_t *vs, *ps; /* copies of the shader tokens behind k.lib.vs.prog / fs.prog */
    uint32_t nvs, nps;
    PipeEntry* e;
    int record;
} PipeJob;

/* compiled shaders, shared by the pipelines that differ only in fixed state (the thread pool's) */
typedef struct Shaders
{
    ID3DBlob *vs, *ps;
} Shaders;

static int g_sync_pipelines;
static char g_pipe_cache[1024];
static CRITICAL_SECTION g_pipe_file_lock;
static SRWLOCK g_shader_lock = SRWLOCK_INIT;
static Map g_shaders; /* LibKey -> Shaders*, under g_shader_lock */

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

static ID3DBlob* compile(const char* src, const char* entry, const char* target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, entry, target,
        D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
    {
        InterlockedIncrement(&g_failures);
        fprintf(stderr, "[recomp] gfx: HLSL compile failed (%s): %s\n%s\n", entry, err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?",
            src);
        code = NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

static Shaders* shaders(const LibKey* k, const uint32_t* vs, const uint32_t* ps)
{
    AcquireSRWLockShared(&g_shader_lock);
    Shaders* s = (Shaders*)map_get(&g_shaders, k, sizeof *k);
    ReleaseSRWLockShared(&g_shader_lock);
    if (s)
        return s->vs && s->ps ? s : NULL;
    s = (Shaders*)calloc(1, sizeof *s);
    char* src = gfx_hlsl_generate(&k->vs, &k->fs, vs, ps);
    if (src)
    {
        s->vs = compile(src, "vs_main", "vs_5_1");
        s->ps = s->vs ? compile(src, "fs_main", "ps_5_1") : NULL;
        free(src);
    }
    else
        InterlockedIncrement(&g_failures);
    AcquireSRWLockExclusive(&g_shader_lock);
    Shaders* had = (Shaders*)map_get(&g_shaders, k, sizeof *k);
    if (had) /* another job compiled it meanwhile */
    {
        if (s->vs)
            ID3D10Blob_Release(s->vs);
        if (s->ps)
            ID3D10Blob_Release(s->ps);
        free(s);
        s = had;
    }
    else
        map_put(&g_shaders, k, sizeof *k, s);
    ReleaseSRWLockExclusive(&g_shader_lock);
    return s->vs && s->ps ? s : NULL;
}

static D3D12_BLEND blend_factor(uint32_t f, int x8, int alpha)
{
    switch (f)
    {
    case 1: return D3D12_BLEND_ZERO;
    case 2: return D3D12_BLEND_ONE;
    case 3: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR; /* the alpha factors take no colors */
    case 4: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 5: return D3D12_BLEND_SRC_ALPHA;
    case 6: return D3D12_BLEND_INV_SRC_ALPHA;
    case 7: return x8 ? D3D12_BLEND_ONE : D3D12_BLEND_DEST_ALPHA;
    case 8: return x8 ? D3D12_BLEND_ZERO : D3D12_BLEND_INV_DEST_ALPHA;
    case 9: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 10: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 11: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
    }
}

static D3D12_BLEND_OP blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return D3D12_BLEND_OP_SUBTRACT;
    case 3: return D3D12_BLEND_OP_REV_SUBTRACT;
    case 4: return D3D12_BLEND_OP_MIN;
    case 5: return D3D12_BLEND_OP_MAX;
    default: return D3D12_BLEND_OP_ADD;
    }
}

static D3D12_COMPARISON_FUNC compare(uint32_t f)
{
    switch (f)
    {
    case 1: return D3D12_COMPARISON_FUNC_NEVER;
    case 2: return D3D12_COMPARISON_FUNC_LESS;
    case 3: return D3D12_COMPARISON_FUNC_EQUAL;
    case 4: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 5: return D3D12_COMPARISON_FUNC_GREATER;
    case 6: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 7: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

static D3D12_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return D3D12_STENCIL_OP_ZERO;
    case 3: return D3D12_STENCIL_OP_REPLACE;
    case 4: return D3D12_STENCIL_OP_INCR_SAT;
    case 5: return D3D12_STENCIL_OP_DECR_SAT;
    case 6: return D3D12_STENCIL_OP_INVERT;
    case 7: return D3D12_STENCIL_OP_INCR;
    case 8: return D3D12_STENCIL_OP_DECR;
    default: return D3D12_STENCIL_OP_KEEP;
    }
}

static int alpha_tested(const GfxFsKey* k) { return k->alpha_func && k->alpha_func != 8; }

/* the pipeline for a key, or NULL. Drawn from the sun (the shadow key) it writes depth alone: no
 * color target, and no pixel function unless an alpha test discards */
static ID3D12PipelineState* build_pipeline(const PipeKey* k, const uint32_t* vs, const uint32_t* ps)
{
    Shaders* s = shaders(&k->lib, vs, ps);
    if (!s)
        return NULL;
    int shadow = k->lib.vs.shadow != 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = g_root;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s->vs), pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(s->vs);
    if (!shadow || alpha_tested(&k->lib.fs) || k->color != DXGI_FORMAT_UNKNOWN) /* (the bounce light's map is in colour) */
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s->ps), pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(s->ps);
    D3D12_RENDER_TARGET_BLEND_DESC* c = &pd.BlendState.RenderTarget[0];
    uint32_t wm = k->pipe.write_mask;
    c->RenderTargetWriteMask = (UINT8)(((wm & 1) ? D3D12_COLOR_WRITE_ENABLE_RED : 0) | ((wm & 2) ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
        ((wm & 4) ? D3D12_COLOR_WRITE_ENABLE_BLUE : 0) | ((wm & 8) ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0));
    c->SrcBlend = c->SrcBlendAlpha = D3D12_BLEND_ONE;
    c->DestBlend = c->DestBlendAlpha = D3D12_BLEND_ZERO;
    c->BlendOp = c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    c->LogicOp = D3D12_LOGIC_OP_NOOP;
    if (k->pipe.blend)
    {
        uint32_t sf = k->pipe.src, df = k->pipe.dst;
        if (sf == 12) /* BOTHSRCALPHA */
            sf = 5, df = 6;
        else if (sf == 13) /* BOTHINVSRCALPHA */
            sf = 6, df = 5;
        c->BlendEnable = TRUE;
        c->SrcBlend = blend_factor(sf, k->x8, 0), c->SrcBlendAlpha = blend_factor(sf, k->x8, 1);
        c->DestBlend = blend_factor(df, k->x8, 0), c->DestBlendAlpha = blend_factor(df, k->x8, 1);
        c->BlendOp = c->BlendOpAlpha = blend_op(k->pipe.op);
    }
    pd.SampleMask = UINT_MAX;
    /* D3D's front faces are clockwise on screen; CULL_CCW (the default) culls the back ones */
    pd.RasterizerState.FillMode = k->fill == 2 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = k->cull == 3 ? D3D12_CULL_MODE_BACK : k->cull == 2 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthBias = -k->zbias;
    pd.RasterizerState.SlopeScaledDepthBias = shadow ? 1.5f : -(float)k->zbias * 0.5f; /* the map: off its slopes */
    pd.RasterizerState.DepthClipEnable = TRUE;
    const GfxDepthKey* dk = &k->depth;
    pd.DepthStencilState.DepthEnable = dk->zenable ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask = dk->zenable && dk->zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = dk->zenable ? compare(dk->zfunc) : D3D12_COMPARISON_FUNC_ALWAYS;
    if (dk->stencil)
    {
        D3D12_DEPTH_STENCILOP_DESC st = { stencil_op(dk->sfail), stencil_op(dk->szfail), stencil_op(dk->spass), compare(dk->sfunc) };
        pd.DepthStencilState.StencilEnable = TRUE;
        pd.DepthStencilState.StencilReadMask = dk->sread;
        pd.DepthStencilState.StencilWriteMask = dk->swrite;
        pd.DepthStencilState.FrontFace = st;
        pd.DepthStencilState.BackFace = st;
    }
    pd.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)k->topo;
    /* the capture (ray tracing's rt_capture): the position streamed out, nothing drawn */
    static const D3D12_SO_DECLARATION_ENTRY so_pos[2] = { { 0, "SV_Position", 0, 0, 4, 0 }, { 0, "TEXCOORD", 0, 0, 4, 0 } };
    static const UINT so_stride = 32; /* the position, then an alpha test's first texture coordinates */
    if (k->lib.vs.shadow == 2)
    {
        pd.StreamOutput.pSODeclaration = so_pos, pd.StreamOutput.NumEntries = k->lib.fs.alpha_func ? 2 : 1;
        pd.StreamOutput.pBufferStrides = &so_stride, pd.StreamOutput.NumStrides = 1;
        pd.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
        pd.PS.pShaderBytecode = NULL, pd.PS.BytecodeLength = 0;
    }
    pd.NumRenderTargets = k->color != DXGI_FORMAT_UNKNOWN ? 1 : 0;
    pd.RTVFormats[0] = (DXGI_FORMAT)k->color;
    pd.DSVFormat = (DXGI_FORMAT)k->dsv;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* p = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(g_dev, &pd, &IID_ID3D12PipelineState, (void**)&p);
    if (FAILED(hr))
    {
        InterlockedIncrement(&g_failures);
        fprintf(stderr, "[recomp] gfx: pipeline failed (%08lx)\n", (unsigned long)hr);
        return NULL;
    }
    return p;
}

/* one record of the cache file: magic, the key, then each shader's tokens (count first) */
static void record_job(const PipeJob* j)
{
    if (!g_pipe_cache[0])
        return;
    uint32_t hdr[2] = { PIPE_MAGIC, (uint32_t)sizeof(PipeKey) };
    EnterCriticalSection(&g_pipe_file_lock);
    FILE* f = fopen(g_pipe_cache, "ab");
    if (f)
    {
        fwrite(hdr, 4, 2, f);
        fwrite(&j->k, sizeof j->k, 1, f);
        fwrite(&j->nvs, 4, 1, f);
        if (j->nvs)
            fwrite(j->vs, 4, j->nvs, f);
        fwrite(&j->nps, 4, 1, f);
        if (j->nps)
            fwrite(j->ps, 4, j->nps, f);
        fclose(f);
    }
    LeaveCriticalSection(&g_pipe_file_lock);
}

static void run_job(PipeJob* j)
{
    ID3D12PipelineState* p = build_pipeline(&j->k, j->vs, j->ps);
    InterlockedExchangePointer(&j->e->state, p ? (void*)p : PIPE_FAILED);
    if (p && j->record)
        record_job(j);
    free(j->vs);
    free(j->ps);
    free(j);
    InterlockedDecrement(&g_pipes_building);
}

static void CALLBACK job_callback(PTP_CALLBACK_INSTANCE inst, void* ctx)
{
    (void)inst;
    run_job((PipeJob*)ctx);
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
    InterlockedIncrement(&g_pipes_building);
    if (g_sync_pipelines || !TrySubmitThreadpoolCallback(job_callback, j, NULL))
        run_job(j);
}

/* at start-up: the keys earlier sessions built, built again in the background */
static void prewarm_pipelines(void)
{
    char path[900];
    if (!cache_dir(path, sizeof path))
        return;
    snprintf(g_pipe_cache, sizeof g_pipe_cache, "%s\\pipelines.d3d12.v1", path);
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

static D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case GFX_LINELIST:
    case GFX_LINESTRIP: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

/* the pipeline for a key, queued for building the first time (NULL until built, or if it failed) */
static ID3D12PipelineState* pipeline_for(const PipeKey* k, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
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
    void* st = InterlockedCompareExchangePointer(&e->state, NULL, NULL);
    return st && st != PIPE_FAILED ? (ID3D12PipelineState*)st : NULL;
}

static ID3D12PipelineState* pipeline(const GfxDraw* d, GfxTex* ds)
{
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.lib.vs = d->vs, k.lib.fs = d->fs, k.pipe = d->pipe;
    k.color = (uint32_t)g_rt->dxfmt;
    k.dsv = ds ? (uint32_t)ds->dxfmt : (uint32_t)DXGI_FORMAT_UNKNOWN;
    if (ds)
    {
        k.depth = d->depth;
        if (!ds->has_stencil)
            k.depth.stencil = 0;
        if (!k.depth.stencil)
            k.depth.sfail = k.depth.szfail = k.depth.spass = k.depth.sfunc = k.depth.sread = k.depth.swrite = 0;
    }
    if (g_fxs.fx != 0.0f && g_fxs.light != 0.0f && d->vs.lighting && !d->vs.rhw && !d->vs.prog && !d->vs.flat)
        k.lib.vs.pixel = g_fxs.light >= 2.0f ? 2 : 1; /* 1: the sun per pixel, the game's torches per vertex */
    k.x8 = (uint8_t)g_rt->x8;
    k.cull = d->cull, k.fill = d->fill == 2 ? 2 : 3;
    k.zbias = ds ? d->zbias : 0;
    k.topo = (uint8_t)topology_type(d->prim);
    /* runs of draws share a pipeline: the last one found is checked before hashing the key (the hash
     * and the map's probe were 3% of the game thread on an Xbox One X) */
    static PipeKey last_key;
    static ID3D12PipelineState* last_state;
    if (last_state && !memcmp(&k, &last_key, sizeof k))
        return last_state;
    ID3D12PipelineState* p = pipeline_for(&k, d->vs_tokens, d->ps_tokens);
    if (!p && k.lib.vs.pixel)
    {
        g_pixel_fallbacks++;
        /* lit per pixel, still building: lit per vertex meanwhile (a new mix of the game's lights
         * would blink the object out while its pipeline builds) */
        k.lib.vs.pixel = 0;
        return pipeline_for(&k, d->vs_tokens, d->ps_tokens);
    }
    if (p)
        last_key = k, last_state = p;
    return p;
}

static D3D12_TEXTURE_ADDRESS_MODE address(uint32_t a)
{
    switch (a)
    {
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

/* the sampler heap slot for a key: made on first use, kept */
static uint32_t sampler_slot(const GfxSampler* k)
{
    void* s = map_get(&g_samplers, k, sizeof *k);
    if (s)
        return (uint32_t)(uintptr_t)s - 1;
    int32_t slot = heap_alloc(&g_samp);
    if (slot < 0)
        return g_present_samp; /* the heap is full: any sampler beats none */
    D3D12_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    D3D12_FILTER_TYPE mn = k->min >= 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mg = k->mag >= 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mp = k->mip >= 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_ENCODE_BASIC_FILTER(mn, mg, mp, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    sd.MaxAnisotropy = 1;
    if ((k->min == 3 || k->mag == 3) && k->max_aniso > 1)
        sd.Filter = D3D12_FILTER_ANISOTROPIC, sd.MaxAnisotropy = k->max_aniso > 16 ? 16 : k->max_aniso;
    sd.AddressU = address(k->addr_u);
    sd.AddressV = address(k->addr_v);
    sd.AddressW = address(k->addr_w);
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.BorderColor[0] = ((k->border >> 16) & 255) / 255.0f;
    sd.BorderColor[1] = ((k->border >> 8) & 255) / 255.0f;
    sd.BorderColor[2] = (k->border & 255) / 255.0f;
    sd.BorderColor[3] = (k->border >> 24) / 255.0f;
    sd.MinLOD = k->max_level;
    sd.MaxLOD = k->mip == 0 ? (float)k->max_level : D3D12_FLOAT32_MAX; /* no mipmapping: that one level */
    if (k->lod_cap && sd.MaxLOD > (float)(k->lod_cap - 1)) /* a texture pack's glyph sheet: not its smallest mips */
        sd.MaxLOD = (float)(k->lod_cap - 1);
    ID3D12Device_CreateSampler(g_dev, &sd, heap_cpu(&g_samp, slot));
    map_put(&g_samplers, k, sizeof *k, (void*)(uintptr_t)(slot + 1));
    return (uint32_t)slot;
}

/* a stage's sampler is usually the one it had on the last draw: that is checked before hashing */
static uint32_t sampler(int stage, const GfxSampler* k)
{
    static GfxSampler last[8];
    static uint32_t last_slot[8];
    static uint8_t known[8];
    if (known[stage] && !memcmp(k, &last[stage], sizeof *k))
        return last_slot[stage];
    last[stage] = *k, last_slot[stage] = sampler_slot(k), known[stage] = 1;
    return last_slot[stage];
}

/* --- drawing --------------------------------------------------------------------------------------------------- */
static void set_viewport(const uint32_t vp[6])
{
    float zmin, zmax;
    memcpy(&zmin, &vp[4], 4);
    memcpy(&zmax, &vp[5], 4);
    uint32_t w, h;
    color_size(&w, &h);
    float x = (float)vp[0], y = (float)vp[1], vw = (float)vp[2], vh = (float)vp[3];
    if (x > w)
        x = (float)w;
    if (y > h)
        y = (float)h;
    if (x + vw > w)
        vw = w - x;
    if (y + vh > h)
        vh = h - y;
    D3D12_VIEWPORT v = { x, y, vw, vh, zmin, zmax };
    if (g_bound_valid && !memcmp(&v, &g_bound_vp, sizeof v))
        return;
    ID3D12GraphicsCommandList_RSSetViewports(g_list, 1, &v);
    g_bound_vp = v;
}

/* GfxDraw.scissor, clamped to the target; none: all of it */
static void set_scissor(const int32_t sc[4])
{
    uint32_t w, h;
    color_size(&w, &h);
    LONG x0 = 0, y0 = 0, x1 = (LONG)w, y1 = (LONG)h;
    if (sc[2] > 0)
    {
        x0 = sc[0] < 0 ? 0 : sc[0];
        y0 = sc[1] < 0 ? 0 : sc[1];
        x1 = sc[0] + sc[2];
        y1 = sc[1] + sc[3];
        if (x1 > (LONG)w)
            x1 = (LONG)w;
        if (y1 > (LONG)h)
            y1 = (LONG)h;
        if (x1 < x0)
            x1 = x0;
        if (y1 < y0)
            y1 = y0;
    }
    D3D12_RECT r = { x0, y0, x1, y1 };
    if (g_bound_valid && !memcmp(&r, &g_bound_sc, sizeof r))
        return;
    ID3D12GraphicsCommandList_RSSetScissorRects(g_list, 1, &r);
    g_bound_sc = r;
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

static D3D12_PRIMITIVE_TOPOLOGY topology(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case GFX_LINELIST: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case GFX_LINESTRIP: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case GFX_TRIANGLESTRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static void set_pso(ID3D12PipelineState* p)
{
    if (p != g_bound_pso)
        ID3D12GraphicsCommandList_SetPipelineState(g_list, p), g_bound_pso = p;
}

static void set_topology(D3D12_PRIMITIVE_TOPOLOGY t)
{
    if ((int)t != g_bound_topo)
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, t), g_bound_topo = (int)t;
}

static void draw_encode(const GfxDraw* d);
static int gen_mips(GfxTex* t);

/* --- the sun's shadow map: the scene's casters, drawn again from the sun (gfx_scene_done) --------------
 * gfx_metal.m's: each opaque draw of the scene (GfxDraw.caster) is recorded as it is encoded - its key
 * and what it was bound with, the ring's parts of which stay until the frame ends - and when the scene
 * is done they are drawn again into a depth map from the sun: the same functions, with the clip-space
 * position they make taken through the camera's inverse into the sun's view (the shadow key,
 * gfx_hlsl_vs_return). The scene effects then look each pixel up in it. */
typedef struct Caster
{
    LibKey lib;
    const uint32_t *vs, *ps;
    GfxBuf* vb[GFX_NSTREAMS]; /* a static buffer the stream reads (its state to set), or NULL */
    D3D12_GPU_VIRTUAL_ADDRESS va[GFX_NSTREAMS], iva, ugpu;
    const uint8_t* vcpu[GFX_NSTREAMS]; /* the stream's bytes from where it is bound, on the CPU (NULL: none) */
    uint32_t vlen[GFX_NSTREAMS];
    GfxBuf* ib;            /* the indices' static buffer, or NULL */
    const uint8_t* icpu;   /* the indices on the CPU */
    GfxTex* tex[8];        /* for an alpha test, and the bounce light's map (in colour) */
    GfxSampler samp[8];
    float wv[16];          /* its world-view matrix (fixed function: is it placed, or a character?) */
    int32_t uidx;          /* its uniforms in g_caster_u (the cache's copy is made from them), or -1 */
    uint32_t n, vstart;
    uint8_t prim;          /* GFX_*: a fan as the list it was drawn as */
    uint8_t itype;         /* 0 no indices, 2 or 4 bytes each */
    uint8_t fixed;         /* every vertex and index from buffers the game keeps (the zone's): cached as drawn */
    uint8_t keep;          /* not fixed, but a placed object (not a character): kept as a copy */
    uint8_t has_pos, has_wpos;
    float clip0[4];        /* its first vertex in the camera's clip space (draw_clip0): where this copy stands */
    float wpos[3];         /* the same in the world, this frame (sun_map) */
} Caster;

static Caster* g_casters;
static uint32_t g_ncasters, g_casters_cap;
static GfxU* g_caster_u; /* this frame's casters' uniforms, when the cache wants them (sun_casters not 1) */
static uint32_t g_ncaster_u, g_caster_u_cap;

static void casters_clear(void) { g_ncasters = 0, g_ncaster_u = 0; }

/* --- a draw's first vertex through its vertex function, on the CPU ---
 * FFXI draws every copy of a zone mesh (all the trees of one kind) from the same buffers, each placed
 * by its vertex shader's constants: the buffers say nothing of where a copy stands. Its first
 * vertex's clip position, through the frame's camera back into the world, does. A small vs.1.x
 * interpreter over the draw's constants and vertex bytes (gfx_metal.m's); fixed function is P * WVP. */
static void vs_src(float out[4], uint32_t t, const float (*r)[4], const float (*v)[4], const float (*c)[4], const float* a0)
{
    uint32_t type = (t >> 28) & 7, num = t & 0x7FF, mod = (t >> 24) & 0xF, sw = (t >> 16) & 0xFF;
    static const float zero[4] = { 0, 0, 0, 0 };
    const float* reg = zero;
    if (type == 0 && num < 12)
        reg = r[num];
    else if (type == 1 && num < GFX_NREGS)
        reg = v[num];
    else if (type == 2)
    {
        int i = (int)num + ((t & 0x2000) ? (int)a0[0] : 0);
        reg = c[i < 0 ? 0 : i >= GFX_NVSC ? GFX_NVSC - 1 : i];
    }
    else if (type == 3)
        reg = a0;
    for (int i = 0; i < 4; ++i)
    {
        float x = reg[(sw >> (2 * i)) & 3];
        switch (mod)
        {
        case 1: x = -x; break;
        case 2: x = x - 0.5f; break;
        case 3: x = 0.5f - x; break;
        case 4: x = (x - 0.5f) * 2.0f; break;
        case 5: x = -(x - 0.5f) * 2.0f; break;
        case 6: x = 1.0f - x; break;
        case 7: x = x * 2.0f; break;
        case 8: x = -x * 2.0f; break;
        }
        out[i] = x;
    }
}

/* the input registers of vertex vi as the declaration maps them */
static int vs_fetch(const GfxDraw* d, int vi, float v[GFX_NREGS][4])
{
    for (int r = 0; r < GFX_NREGS; ++r)
    {
        v[r][0] = v[r][1] = v[r][2] = 0, v[r][3] = 1;
        const GfxElem* e = &d->vs.el[r];
        if (!e->used)
            continue;
        int s = e->stream;
        const uint8_t* base;
        size_t have;
        if (d->buf[s] && d->buf[s]->cpu)
            base = d->buf[s]->cpu + d->buf_off[s], have = d->buf[s]->cpu_size > d->buf_off[s] ? d->buf[s]->cpu_size - d->buf_off[s] : 0;
        else if (!d->buf[s] && d->data[s])
            base = (const uint8_t*)d->data[s], have = d->size[s];
        else
            return 0;
        long at = (long)vi * d->u.stride[s] + d->u.offset[r];
        static const uint8_t SIZE[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
        if (at < 0 || (size_t)at + SIZE[e->type & 7] > have)
            return 0;
        const uint8_t* p = base + at;
        switch (e->type)
        {
        case GFX_FLOAT1: memcpy(v[r], p, 4); break;
        case GFX_FLOAT2: memcpy(v[r], p, 8); break;
        case GFX_FLOAT3: memcpy(v[r], p, 12); break;
        case GFX_FLOAT4: memcpy(v[r], p, 16); break;
        case GFX_D3DCOLOR: v[r][0] = p[2] / 255.0f, v[r][1] = p[1] / 255.0f, v[r][2] = p[0] / 255.0f, v[r][3] = p[3] / 255.0f; break;
        case GFX_UBYTE4: for (int i = 0; i < 4; ++i) v[r][i] = p[i]; break;
        case GFX_SHORT2: { int16_t h[2]; memcpy(h, p, 4); v[r][0] = h[0], v[r][1] = h[1]; break; }
        default: { int16_t h[4]; memcpy(h, p, 8); for (int i = 0; i < 4; ++i) v[r][i] = h[i]; break; }
        }
    }
    return 1;
}

/* clip-space position of the draw's first vertex in out; 0 when it cannot be had */
static int draw_clip0(const GfxDraw* d, float out[4])
{
    if (d->vs.rhw)
        return 0;
    long idx = d->vertex_start;
    if (d->indices)
        idx = d->index_size == 2 ? ((const uint16_t*)d->indices)[0] : (long)((const uint32_t*)d->indices)[0];
    float v[GFX_NREGS][4];
    if (!vs_fetch(d, (int)(idx + d->u.vofs), v))
        return 0;
    if (!d->vs.prog)
    {
        const float* m = d->u.wvp;
        for (int j = 0; j < 4; ++j)
            out[j] = v[0][0] * m[j] + v[0][1] * m[4 + j] + v[0][2] * m[8 + j] + m[12 + j];
        return 1;
    }
    const uint32_t* t = d->vs_tokens;
    if (!t || (t[0] & 0xFFFF0000u) != 0xFFFE0000u)
        return 0;
    float r[12][4], a0[4] = { 0, 0, 0, 0 }, opos[4] = { 0, 0, 0, 1 };
    memset(r, 0, sizeof r);
    const float(*c)[4] = (const float(*)[4])d->u.vsc;
    for (uint32_t i = 1; i < 65536;)
    {
        uint32_t tok = t[i], op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu)
            break;
        if (op == 0xFFFE)
        {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const uint32_t* p = &t[i + 1];
        float s0[4], s1[4], s2[4], res[4] = { 0, 0, 0, 0 };
        int np;
        switch (op)
        {
        case 0: case 81: np = op ? 5 : 0; break;
        case 1: case 6: case 7: case 14: case 15: case 16: case 19: case 78: case 79: np = 2; break;
        case 2: case 3: case 5: case 8: case 9: case 10: case 11: case 12: case 13: case 17: np = 3; break;
        case 20: case 21: case 22: case 23: case 24: np = 3; break;
        case 4: case 18: np = 4; break;
        default: return 0; /* not vs.1.x */
        }
        if (op == 0 || op == 81)
        {
            i += 1 + (uint32_t)np;
            continue;
        }
        if (np >= 2)
            vs_src(s0, p[1], r, v, c, a0);
        if (np >= 3 && !(op >= 20 && op <= 24))
            vs_src(s1, p[2], r, v, c, a0);
        if (np >= 4)
            vs_src(s2, p[3], r, v, c, a0);
        switch (op)
        {
        case 1: memcpy(res, s0, 16); break;
        case 2: for (int k = 0; k < 4; ++k) res[k] = s0[k] + s1[k]; break;
        case 3: for (int k = 0; k < 4; ++k) res[k] = s0[k] - s1[k]; break;
        case 4: for (int k = 0; k < 4; ++k) res[k] = s0[k] * s1[k] + s2[k]; break;
        case 5: for (int k = 0; k < 4; ++k) res[k] = s0[k] * s1[k]; break;
        case 6: res[0] = res[1] = res[2] = res[3] = s0[3] == 0.0f ? INFINITY : 1.0f / s0[3]; break;
        case 7: res[0] = res[1] = res[2] = res[3] = s0[3] == 0.0f ? INFINITY : 1.0f / sqrtf(fabsf(s0[3])); break;
        case 8: res[0] = res[1] = res[2] = res[3] = s0[0] * s1[0] + s0[1] * s1[1] + s0[2] * s1[2]; break;
        case 9: res[0] = res[1] = res[2] = res[3] = s0[0] * s1[0] + s0[1] * s1[1] + s0[2] * s1[2] + s0[3] * s1[3]; break;
        case 10: for (int k = 0; k < 4; ++k) res[k] = fminf(s0[k], s1[k]); break;
        case 11: for (int k = 0; k < 4; ++k) res[k] = fmaxf(s0[k], s1[k]); break;
        case 12: for (int k = 0; k < 4; ++k) res[k] = s0[k] < s1[k] ? 1.0f : 0.0f; break;
        case 13: for (int k = 0; k < 4; ++k) res[k] = s0[k] >= s1[k] ? 1.0f : 0.0f; break;
        case 14: case 78: res[0] = res[1] = res[2] = res[3] = exp2f(s0[3]); break;
        case 15: case 79: res[0] = res[1] = res[2] = res[3] = s0[3] == 0.0f ? -INFINITY : log2f(fabsf(s0[3])); break;
        case 16:
            res[0] = 1.0f, res[1] = fmaxf(s0[0], 0.0f), res[3] = 1.0f;
            res[2] = s0[0] > 0.0f && s0[1] > 0.0f ? powf(s0[1], fminf(fmaxf(s0[3], -127.9961f), 127.9961f)) : 0.0f;
            break;
        case 17: res[0] = 1.0f, res[1] = s0[1] * s1[1], res[2] = s0[2], res[3] = s1[3]; break;
        case 18: for (int k = 0; k < 4; ++k) res[k] = s2[k] + (s1[k] - s2[k]) * s0[k]; break;
        case 19: for (int k = 0; k < 4; ++k) res[k] = s0[k] - floorf(s0[k]); break;
        default:
        {
            /* m4x4, m4x3, m3x4, m3x3, m3x2: dot products against consecutive constant rows */
            int rows = op == 20 ? 4 : op == 21 ? 3 : op == 22 ? 4 : op == 23 ? 3 : 2, three = op >= 22;
            for (int k = 0; k < rows; ++k)
            {
                float row[4];
                vs_src(row, p[2] + (uint32_t)k, r, v, c, a0);
                res[k] = s0[0] * row[0] + s0[1] * row[1] + s0[2] * row[2] + (three ? 0.0f : s0[3] * row[3]);
            }
            break;
        }
        }
        uint32_t dt = p[0], type = (dt >> 28) & 7, num = dt & 0x7FF, mask = (dt >> 16) & 0xF;
        if (op >= 20 && op <= 24)
            mask = op == 20 || op == 22 ? 0xF : op == 24 ? 0x3 : 0x7;
        if (mask == 0)
            mask = 0xF;
        if ((dt >> 20) & 1)
            for (int k = 0; k < 4; ++k)
                res[k] = fminf(fmaxf(res[k], 0.0f), 1.0f);
        float* dst = type == 0 && num < 12 ? r[num] : type == 3 ? a0 : type == 4 && num == 0 ? opos : NULL;
        if (dst)
            for (int k = 0; k < 4; ++k)
                if (mask & (1u << k))
                    dst[k] = res[k];
        i += 1 + (uint32_t)np;
    }
    memcpy(out, opos, 16);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]) && isfinite(out[3]);
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

/* a caster for the draw being encoded: what draw_encode binds it fills in */
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
    c->fixed = d->prim != GFX_TRIANGLEFAN && (!d->indices || d->ibuf);
    c->has_pos = (uint8_t)draw_clip0(d, c->clip0);
    memcpy(c->wv, d->u.wv, 64);
    c->uidx = -1;
    if (g_fxs.sun_casters != 1.0f) /* the cache keeps the zone's: it wants the uniforms */
    {
        if (g_ncaster_u == g_caster_u_cap)
            g_caster_u_cap = g_caster_u_cap ? g_caster_u_cap * 2 : 1024,
            g_caster_u = (GfxU*)realloc(g_caster_u, g_caster_u_cap * sizeof(GfxU));
        g_caster_u[g_ncaster_u] = d->u;
        c->uidx = (int32_t)g_ncaster_u++;
    }
    return c;
}

/* A caster's vertices and indices from a buffer the game is rewriting (uploaded this frame already, or
 * often), copied into the ring as they are now: the game may upload it again before the scene is done
 * (the next tree of a kind, swayed through the same buffer), and the map would then draw this one
 * with that one's vertices - so the upload took it out (sun_cache_forget), and only the last of them
 * cast. The CPU's copy is the guest's own memory, rewritten in place: it can only be taken here. Up
 * to 1 MB a stream; past that it stays the buffer's. No longer the game's buffers, it is not fixed. */
static void caster_snapshot(Caster* c, const GfxDraw* d)
{
    enum { MOST = 1 << 20 };
    long hi = (long)c->vstart + (long)c->n - 1;
    if (c->itype)
    {
        hi = -1;
        if (c->icpu)
            for (uint32_t i = 0; i < c->n; ++i)
            {
                long x = c->itype == 2 ? ((const uint16_t*)c->icpu)[i] : (long)((const uint32_t*)c->icpu)[i];
                hi = x > hi ? x : hi;
            }
    }
    if (hi >= 0)
        hi += d->u.vofs;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        GfxBuf* b = c->vb[s];
        if (!b || !c->vcpu[s] || !(b->up_last == g_serial || buf_volatile(b)))
            continue;
        uint32_t stride = d->u.stride[s];
        size_t need = hi >= 0 && stride ? (size_t)(hi + 1) * stride + 16 : c->vlen[s];
        if (need > c->vlen[s])
            need = c->vlen[s];
        if (!need || need > MOST)
            continue;
        Alloc a = ring(need + 16, 16);
        if (!a.cpu)
            continue;
        memcpy(a.cpu, c->vcpu[s], need);
        c->vb[s] = NULL, c->va[s] = a.gpu, c->vcpu[s] = a.cpu, c->vlen[s] = (uint32_t)need, c->fixed = 0;
    }
    if (c->ib && c->icpu && (c->ib->up_last == g_serial || buf_volatile(c->ib)) && (size_t)c->n * c->itype <= MOST)
    {
        Alloc a = ring((size_t)c->n * c->itype, 16);
        if (a.cpu)
        {
            memcpy(a.cpu, c->icpu, (size_t)c->n * c->itype);
            c->ib = NULL, c->iva = a.gpu, c->icpu = a.cpu, c->fixed = 0;
        }
    }
}

/* The scene filter (gfx_metal.m's): a large render target drawn smaller onto a large target (FFXI's
 * world onto the back buffer) is sampled through its mips, made here from it as it is now - every
 * frame alike, or the sky's fine detail flickers between frames that had them and frames that did not. */
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
        if (!wanted || !t || t == g_rt || t->mip_srv < 0 || t->scene)
            continue;
        if (t->w <= tw && t->h <= th)
            continue; /* not made smaller */
        if (gen_mips(t))
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
    /* a long scene goes to the GPU in pieces */
    if (g_cmd_draws >= SPLIT_DRAWS)
        submit(0);
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws++;
}

static void draw_encode(const GfxDraw* d)
{
    if (!bind_targets())
    {
        gfx_prof_skip(GFX_SKIP_NO_TARGET);
        return;
    }
    GfxTex* ds = depth_attachment();
    ID3D12PipelineState* p = pipeline(d, ds);
    if (!p)
    {
        gfx_prof_skip(GFX_SKIP_PIPELINE); /* still building (or failed) */
        return;
    }
    ID3D12GraphicsCommandList* l = g_list;
    /* textures first: their transitions go before the draw */
    uint32_t bind[16];
    GfxTex* bound_tex[8] = { 0 }; /* for a caster's alpha test */
    GfxSampler bound_samp[8];
    for (int i = 0; i < 8; ++i)
    {
        GfxTex* t = d->tex[i];
        int wanted = d->fs.prog || i < d->fs.nstages ? d->fs.st[i].tex : 0;
        bind[i] = wanted == 2 ? SRV_NULL_CUBE : SRV_NULL_2D;
        bind[8 + i] = g_present_samp;
        if (!wanted)
            continue;
        GfxSampler sk = d->samp[i];
        int32_t srv = t ? t->srv : -1;
        if (t && g_fxs.fx != 0.0f)
        {
            /* the world's solid textures (not the interface's, not the sky's or effects' - which write
             * no depth), where every mip is there: trilinear and anisotropic, so ground and walls at a
             * slant stay sharp and do not swim */
            if (!d->vs.rhw && d->depth.zwrite && !d->fs.st[i].projected && g_fxs.aniso > 1.0f && t->levels > 1 && t->levels < 32 &&
                t->filled == (1u << t->levels) - 1 && sk.min >= 2)
                sk.min = 3, sk.mip = 2, sk.max_aniso = (uint8_t)(g_fxs.aniso > 16.0f ? 16.0f : g_fxs.aniso);
            /* the finished scene made smaller (FFXI's background onto the back buffer): through its
             * mips, anisotropic for a squeeze that differs across and down */
            else if (t->scene && t->mip_srv >= 0)
                sk.min = 3, sk.mag = 2, sk.mip = 2, sk.max_aniso = 16, sk.max_level = 0, srv = t->mip_srv;
        }
        bind[8 + i] = sampler(i, &sk);
        if (!t || srv < 0 || t == g_rt || (wanted == 2) != (t->type == GFX_TEX_CUBE))
            continue; /* unbound, or the target being drawn to: reads as the null view */
        tex_state(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        bind[i] = (uint32_t)srv;
        if (srv == t->srv)
            bound_tex[i] = t, bound_samp[i] = sk;
    }
    if (d->caster && !g_rt_face && !g_rt_level && ds)
        g_rt->depth_world = ds;
    if (g_targets_bound == 0 || g_rt->state != D3D12_RESOURCE_STATE_RENDER_TARGET)
        bind_targets();
    set_pso(p);
    set_topology(topology(d->prim));
    set_viewport(d->vp);
    set_scissor(d->scissor);
    int bound = g_bound_valid;
    if (ds && ds->has_stencil && d->depth.stencil && (!bound || d->stencil_ref != g_bound_sref))
        ID3D12GraphicsCommandList_OMSetStencilRef(l, d->stencil_ref), g_bound_sref = d->stencil_ref;
    if (!bound || memcmp(bind, g_bound_bind, sizeof bind))
    {
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
        memcpy(g_bound_bind, bind, sizeof bind);
    }

    /* the uniforms the draw's functions read: the lights only when lit, the vertex shader's
     * constants only for a vertex shader, the pixel shader's only for a pixel shader (the ring
     * keeps room for the whole struct: that is what the functions are compiled against). The same
     * bytes as the last draw's are read from where those went. */
    size_t need = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
    if (d->vs.prog)
        need = offsetof(GfxU, psc);
    if (d->fs.prog)
        need = sizeof(GfxU);
    D3D12_GPU_VIRTUAL_ADDRESS ugpu = g_last_u_gpu;
    if (need > g_last_u_need || memcmp(&d->u, &g_last_u, need))
    {
        Alloc u = ring(sizeof(GfxU), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (!u.cpu)
            return;
        memcpy(u.cpu, &d->u, need);
        memcpy(&g_last_u, &d->u, need);
        g_last_u_need = need, g_last_u_gpu = ugpu = u.gpu;
    }
    if (!bound || ugpu != g_bound_u)
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, ugpu), g_bound_u = ugpu;
    /* an opaque draw of the scene, recorded for the sun's map as it is bound */
    Caster* rec = d->caster && g_fxs.fx != 0.0f && g_fxs.sun > 0.0f ? caster_new(d) : NULL;
    if (rec)
    {
        rec->ugpu = ugpu;
        memcpy(rec->tex, bound_tex, sizeof bound_tex), memcpy(rec->samp, bound_samp, sizeof bound_samp);
    }
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        D3D12_GPU_VIRTUAL_ADDRESS va = ID3D12Resource_GetGPUVirtualAddress(g_dummy);
        if (d->buf[s])
        {
            buf_state(d->buf[s], BUF_READ);
            va = d->buf[s]->gpu + d->buf_off[s];
            if (rec)
            {
                GfxBuf* b = d->buf[s];
                rec->vb[s] = b;
                rec->vcpu[s] = b->cpu && b->cpu_size > d->buf_off[s] ? b->cpu + d->buf_off[s] : NULL;
                rec->vlen[s] = rec->vcpu[s] ? b->cpu_size - d->buf_off[s] : 0;
                if (buf_volatile(b))
                    rec->fixed = 0;
            }
        }
        else if (d->data[s] && d->size[s])
        {
            Alloc v = ring(d->size[s] + 16, 16); /* the slack: a last element read whole */
            if (!v.cpu)
            {
                if (rec)
                    g_ncasters--;
                return;
            }
            memcpy(v.cpu, d->data[s], d->size[s]);
            va = v.gpu;
            if (rec)
                rec->vcpu[s] = v.cpu, rec->vlen[s] = d->size[s], rec->fixed = 0;
        }
        if (rec)
            rec->va[s] = va;
        if (!bound || va != g_bound_stream[s])
            ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + s, va), g_bound_stream[s] = va;
    }
    g_bound_valid = 1;

    uint32_t n = vertex_count(d->prim, d->count);
    if (rec)
        rec->prim = (uint8_t)(d->prim == GFX_TRIANGLEFAN ? GFX_TRIANGLELIST : d->prim), rec->n = n, rec->vstart = d->vertex_start;
    D3D12_INDEX_BUFFER_VIEW ibv;
    if (d->prim == GFX_TRIANGLEFAN)
    {
        /* no fans in D3D12: a list with the same vertices */
        Alloc a = ring((size_t)n * 4, 16);
        if (!a.cpu)
        {
            if (rec)
                g_ncasters--;
            return;
        }
        if (rec)
            rec->iva = a.gpu, rec->icpu = a.cpu, rec->itype = 4;
        uint32_t* idx = (uint32_t*)a.cpu;
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
        ibv.BufferLocation = a.gpu, ibv.SizeInBytes = n * 4, ibv.Format = DXGI_FORMAT_R32_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(l, n, 1, 0, 0, 0);
    }
    else if (d->ibuf)
    {
        buf_state(d->ibuf, BUF_READ);
        if (rec)
        {
            rec->ib = d->ibuf, rec->iva = d->ibuf->gpu + d->ibuf_off, rec->itype = (uint8_t)(d->index_size == 2 ? 2 : 4);
            rec->icpu = d->ibuf->cpu && (size_t)d->ibuf_off + (size_t)n * rec->itype <= d->ibuf->cpu_size ? d->ibuf->cpu + d->ibuf_off : NULL;
            if (buf_volatile(d->ibuf))
                rec->fixed = 0;
        }
        ibv.BufferLocation = d->ibuf->gpu + d->ibuf_off;
        ibv.SizeInBytes = d->ibuf->size - d->ibuf_off;
        ibv.Format = d->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(l, n, 1, 0, 0, 0);
    }
    else if (d->indices)
    {
        Alloc a = ring((size_t)n * d->index_size, 16);
        if (!a.cpu)
        {
            if (rec)
                g_ncasters--;
            return;
        }
        memcpy(a.cpu, d->indices, (size_t)n * d->index_size);
        if (rec)
            rec->iva = a.gpu, rec->icpu = a.cpu, rec->itype = (uint8_t)(d->index_size == 2 ? 2 : 4);
        ibv.BufferLocation = a.gpu, ibv.SizeInBytes = n * d->index_size;
        ibv.Format = d->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(l, n, 1, 0, 0, 0);
    }
    else
        ID3D12GraphicsCommandList_DrawInstanced(l, n, 1, d->vertex_start, 0);
    if (rec)
        caster_snapshot(rec, d);
    g_cmd_draws++;
}

/* --- clears ---------------------------------------------------------------------------------------------------- */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    if (!g_dev || !g_rt)
        return;
    if (!g_ds)
        flags &= 1;
    if (!flags || !bind_targets())
        return;
    float c[4] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f };
    uint32_t w, h;
    color_size(&w, &h);
    int whole = !nrects && vp[0] == 0 && vp[1] == 0 && vp[2] >= w && vp[3] >= h;
    /* the viewport, intersected with each rectangle */
    D3D12_RECT stack[16], *r = stack;
    uint32_t n = 0;
    if (!whole)
    {
        int32_t vx0 = (int32_t)vp[0], vy0 = (int32_t)vp[1], vx1 = vx0 + (int32_t)vp[2], vy1 = vy0 + (int32_t)vp[3];
        int32_t whole_rect[4] = { vx0, vy0, vx1, vy1 };
        if (!nrects)
            rects = whole_rect, nrects = 1;
        if (nrects > 16)
            r = (D3D12_RECT*)malloc(sizeof(D3D12_RECT) * nrects);
        for (uint32_t i = 0; i < nrects; ++i)
        {
            int32_t x0 = rects[4 * i] > vx0 ? rects[4 * i] : vx0, y0 = rects[4 * i + 1] > vy0 ? rects[4 * i + 1] : vy0;
            int32_t x1 = rects[4 * i + 2] < vx1 ? rects[4 * i + 2] : vx1, y1 = rects[4 * i + 3] < vy1 ? rects[4 * i + 3] : vy1;
            x1 = x1 < (int32_t)w ? x1 : (int32_t)w, y1 = y1 < (int32_t)h ? y1 : (int32_t)h;
            x0 = x0 > 0 ? x0 : 0, y0 = y0 > 0 ? y0 : 0;
            if (x1 > x0 && y1 > y0)
                r[n++] = (D3D12_RECT){ x0, y0, x1, y1 };
        }
        if (!n)
            goto out;
    }
    if (flags & 1)
        ID3D12GraphicsCommandList_ClearRenderTargetView(g_list, target_view(g_rt, g_rt_face, g_rt_level), c, n, whole ? NULL : r);
    GfxTex* ds = depth_attachment();
    if (ds && (flags & 6))
    {
        D3D12_CLEAR_FLAGS f = (D3D12_CLEAR_FLAGS)(((flags & 2) ? D3D12_CLEAR_FLAG_DEPTH : 0) | ((flags & 4) && ds->has_stencil ? D3D12_CLEAR_FLAG_STENCIL : 0));
        if (f)
            ID3D12GraphicsCommandList_ClearDepthStencilView(g_list, target_view(ds, 0, 0), f, z, (UINT8)stencil, n, whole ? NULL : r);
    }
out:
    if (r != stack)
        free(r);
}

/* --- the window ------------------------------------------------------------------------------------------------ */
static void swap_release(void)
{
    for (int i = 0; i < FRAMES; ++i)
    {
        if (g_swap_buf[i])
            ID3D12Resource_Release(g_swap_buf[i]);
        g_swap_buf[i] = NULL;
    }
}

static void swap_acquire(void)
{
    for (UINT i = 0; i < FRAMES; ++i)
    {
        IDXGISwapChain3_GetBuffer(g_swap, i, &IID_ID3D12Resource, (void**)&g_swap_buf[i]);
        g_swap_state[i] = D3D12_RESOURCE_STATE_PRESENT;
        if (g_swap_rtv[i] < 0)
            g_swap_rtv[i] = heap_alloc(&g_rtv);
        ID3D12Device_CreateRenderTargetView(g_dev, g_swap_buf[i], NULL, heap_cpu(&g_rtv, g_swap_rtv[i]));
    }
}

/* the swap chain at the window's size in pixels (all GPU work finished first: its buffers go) */
static void swap_fit(void)
{
    int pw = 0, ph = 0;
    if (g_window)
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        pw = (int)g_want_w, ph = (int)g_want_h;
    if (pw <= 0 || ph <= 0 || ((uint32_t)pw == g_swap_w && (uint32_t)ph == g_swap_h))
        return;
    submit(1);
    swap_release();
    HRESULT hr = IDXGISwapChain3_ResizeBuffers(g_swap, FRAMES, (UINT)pw, (UINT)ph, DXGI_FORMAT_B8G8R8A8_UNORM,
        g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr))
        fprintf(stderr, "[recomp] gfx: swap chain resize to %dx%d failed (%08lx)\n", pw, ph, (unsigned long)hr);
    else
        g_swap_w = (uint32_t)pw, g_swap_h = (uint32_t)ph;
    swap_acquire();
}

/* --- frames ---------------------------------------------------------------------------------------------------- */
static void frame_end(void)
{
    casters_clear(); /* what they drew from in the ring goes with the frame */
    list();
    if (gfx_profiling && g_queries && g_frames[g_frame].timed)
    {
        ID3D12GraphicsCommandList_EndQuery(g_list, g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * g_frame + 1);
        ID3D12GraphicsCommandList_ResolveQueryData(g_list, g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * g_frame, 2, g_query_rb,
            16ull * g_frame);
    }
    submit(0);
    g_frame_open = 0;
    g_frame = (g_frame + 1) % FRAMES;
    g_serial++;
}

/* "60 FPS 16.7ms": the text of the overlay, from the presents of the last half second */
static void fps_tick(void)
{
    double now = (double)gfx_now_ns() * 1e-9;
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

static void draw_overlay(uint32_t w, uint32_t h)
{
    struct
    {
        float rect[4], scale;
        uint32_t n;
        float size[2];
        uint32_t text[32];
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
    u.size[0] = (float)w, u.size[1] = (float)h;
    Alloc a = ring(sizeof u, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return;
    memcpy(a.cpu, &u, sizeof u);
    set_pso(g_overlay_pso);
    set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(g_list, ROOT_U, a.gpu);
    ID3D12GraphicsCommandList_DrawInstanced(g_list, 4, 1, 0, 0);
}

/* --- scene effects' settings ------------------------------------------------------------------------------------
 * gfx_metal.m's, key for key (its FX_SETTINGS has what each does): FFXI_FX=1 and FFXI_FX_<KEY> at start,
 * then while the game runs FFXI_FX_FILE (default fx.txt in the cache folder, cachedir.h), lines of key=value, which
 * Config > Modern writes. */
#define FXS(k, d) { #k, offsetof(FxSettings, k), d }
static const struct
{
    const char* key;
    size_t at;
    float def;
} FX_SETTINGS[] = {
    FXS(fx, 0.0f), FXS(ao, 0.8f), FXS(radius, 1.0f), FXS(grade, 1.0f), FXS(sat, 1.12f), FXS(contrast, 0.2f),
    FXS(sharpen, 0.3f), FXS(filter, 1.0f), FXS(aniso, 16.0f), FXS(fog, 0.004f), FXS(fog_falloff, 0.08f),
    FXS(fog_height, 2.0f), FXS(fog_max, 0.5f), FXS(fog_sun, 0.5f), FXS(fog_g, 0.6f), FXS(bloom, 0.3f),
    FXS(threshold, 0.75f), FXS(rays, 0.6f), FXS(rays_decay, 0.965f), FXS(rays_length, 0.85f), FXS(light, 0.0f),
    FXS(shadow, 0.3f), FXS(shadow_length, 0.6f), FXS(sun, 0.45f), FXS(sun_distance, 100.0f), FXS(sun_soft, 0.0f),
    FXS(sun_face, 0.0f), FXS(sun_min, 0.15f), FXS(sun_direct, 0.5f), FXS(sun_casters, 0.0f), FXS(sun_near, 10.0f),
    FXS(sun_detail, 4096.0f), FXS(ao_quality, 0.0f), FXS(temporal, 0.85f), FXS(debug, 0.0f), FXS(draw, 0.0f),
    FXS(draw_entities, 0.0f), FXS(fps, 1.0f), FXS(aa, 0.0f), FXS(sun_dusk, 1.0f),
    /* gfx_metal.m's water pass (GfxDraw.water): kept, read back and saved here, not drawn yet */
    FXS(water, 1.0f), FXS(water_refract, 0.015f), FXS(water_clarity, 3.0f), FXS(water_soft, 0.15f), FXS(water_foam, 0.6f),
    FXS(water_foam_width, 0.8f), FXS(water_ripple, 0.25f), FXS(water_scale, 0.6f), FXS(water_reflect, 0.6f), FXS(water_spec, 2.0f),
    /* not an effect: host64's level of detail (--lod), live while tuning */
    FXS(lod, 0.0f),
    /* not an effect: the game's own character shadows (d3d8.c game_shadow_hidden, modern.c game_shadows_follow): 0 off while the sun's are on, 1 always, 2 never */
    FXS(gameshadows, 0.0f),
    /* how much of the sun's shadows stay in a Mog House (gfx_set_moghouse), 0 none to 1 all */
    FXS(moghouse, 0.0f),
    /* the sun's light thrown on by what it lights (bounce light): its strength (0 none), how far a lit
     * surface throws it, and how far from the camera it is gathered (the map it is gathered from) */
    FXS(gi, 0.0f), FXS(gi_radius, 6.0f), FXS(gi_distance, 48.0f), /* (off unless asked for: Config > Modern's Bounce Light) */
    /* ray tracing: the world made for rays each frame (rt_capture) where the device can, the bounce light
     * and the occlusion traced through it; 0, the default, none (the bounce light from its map alone, the
     * occlusion from the depth); debug=clay shows it */
    FXS(rt, 0.0f),
};
#undef FXS

/* sun_detail: the near map's texels across, a power of two from 512 to 8192 - or, as it once was, 0 for
 * 4096 and 1 for 8192 (gfx_metal.m's sun_near_size) */
static int sun_near_size(void)
{
    float d = g_fxs.sun_detail;
    if (d < 2.0f)
        return d >= 0.5f ? 8192 : 4096;
    int n = 512;
    while (n < 8192 && (float)n * 1.5f < d)
        n *= 2;
    return n;
}

void gfx_set_focus(const float* world)
{
    if (world && isfinite(world[0]) && isfinite(world[1]) && isfinite(world[2]))
        memcpy(g_focus, world, sizeof g_focus), g_focus_serial = g_serial;
    else
        g_focus_serial = 0;
}

static float* fx_setting(const char* key)
{
    for (size_t i = 0; i < sizeof FX_SETTINGS / sizeof FX_SETTINGS[0]; ++i)
        if (!strcmp(FX_SETTINGS[i].key, key))
            return (float*)((char*)&g_fxs + FX_SETTINGS[i].at);
    return NULL;
}

void gfx_fx_set(const char* key, float v)
{
    float* p = fx_setting(key);
    if (p)
        *p = v;
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

float gfx_fx_get(const char* key)
{
    float* p = fx_setting(key);
    return p ? *p : 0.0f;
}

static char g_fx_file[1024];
static uint64_t g_fx_mtime;
#define FX_NSETTINGS (sizeof FX_SETTINGS / sizeof FX_SETTINGS[0])
static float g_fx_start[FX_NSETTINGS];      /* each setting as the environment and defaults gave it */
static uint8_t g_fx_from_file[FX_NSETTINGS]; /* set by the file's last read */

/* the settings file, when it changed since the last look (from Present, twice a second) */
static void fx_reload(void)
{
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (!g_fx_file[0] || now - last < 500)
        return;
    last = now;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(g_fx_file, GetFileExInfoStandard, &a))
        return;
    uint64_t mtime = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    if (mtime == g_fx_mtime)
        return;
    g_fx_mtime = mtime;
    FILE* f = fopen(g_fx_file, "r");
    if (!f)
        return;
    char line[256], key[64];
    float v;
    uint8_t seen[FX_NSETTINGS] = { 0 };
    while (fgets(line, sizeof line, f))
        if (sscanf(line, " %63[a-z_] = %f", key, &v) == 2)
            for (size_t i = 0; i < FX_NSETTINGS; ++i)
                if (!strcmp(FX_SETTINGS[i].key, key))
                    *(float*)((char*)&g_fxs + FX_SETTINGS[i].at) = v, seen[i] = 1;
    fclose(f);
    /* a line taken out of the file: its key back to what it was at start, not the file's last word */
    for (size_t i = 0; i < FX_NSETTINGS; ++i)
    {
        if (g_fx_from_file[i] && !seen[i])
            *(float*)((char*)&g_fxs + FX_SETTINGS[i].at) = g_fx_start[i];
        g_fx_from_file[i] = seen[i];
    }
    fprintf(stderr, "[recomp] gfx: scene effects %s from %s\n", g_fxs.fx != 0.0f ? "on" : "off", g_fx_file);
}

static void fx_config(void)
{
    for (size_t i = 0; i < sizeof FX_SETTINGS / sizeof FX_SETTINGS[0]; ++i)
    {
        char name[64], *c;
        snprintf(name, sizeof name, i ? "FFXI_FX_%s" : "FFXI_FX", FX_SETTINGS[i].key);
        for (c = name; *c; ++c)
            if (*c >= 'a' && *c <= 'z')
                *c -= 32;
        const char* v = getenv(name);
        *(float*)((char*)&g_fxs + FX_SETTINGS[i].at) = v && *v ? (float)atof(v) : FX_SETTINGS[i].def;
    }
    const char* dbg = getenv("FFXI_FX_DEBUG"); /* also by name */
    if (dbg)
        g_fxs.debug = !strcmp(dbg, "ao") ? 1.0f : !strcmp(dbg, "fog") ? 2.0f : !strcmp(dbg, "bloom") ? 3.0f
            : !strcmp(dbg, "rays") ? 4.0f : !strcmp(dbg, "shadow") ? 5.0f
            : !strcmp(dbg, "gi") ? 6.0f : !strcmp(dbg, "gi_split") ? 7.0f : !strcmp(dbg, "clay") ? 8.0f : (float)atof(dbg);
    for (size_t i = 0; i < FX_NSETTINGS; ++i)
        g_fx_start[i] = *(float*)((char*)&g_fxs + FX_SETTINGS[i].at);
    /* where Config > Modern writes it (host/modern.c fx_file): FFXI_CACHE_DIR's when that is set */
    const char* file = getenv("FFXI_FX_FILE");
    char dir[900];
    if (file && *file)
        snprintf(g_fx_file, sizeof g_fx_file, "%s", file);
    else if (cache_dir(dir, sizeof dir))
        snprintf(g_fx_file, sizeof g_fx_file, "%s\\fx.txt", dir);
}

/* --- scene effects (gfx_scene_done) ----------------------------------------------------------------------------
 * gfx_metal.m's (its comment says what each pass does and why), in D3D12's terms: the passes are
 * gfx_hlsl.c's gfx_hlsl_fx, each a full-screen triangle into a texture of the effects' own (FxTex),
 * bound as a draw is - FxU as the uniforms, the textures' heap slots as the root constants. Their
 * pipelines are built off the game's thread the first time the effects are on (fx_init); until they
 * are ready a scene goes through as it was. The sun's maps are drawn from the scene's casters
 * (sun_map), depth alone, before the occlusion pass reads them. */
typedef struct FxU
{
    float proj[4], zp[4], vp[4], size[4], ao[4], grade[4], hand[4], up[4], sun[4], suncol[4], sunuv[4], fogc[4], fogp[4],
        bloom[4], rays[4], shadow[4], lmat[16], smap[4], smap2[4], reproj[16], hist[4], lmatn[16], smapn[4], smapn2[4], aop[4],
        gimat[16], giinv[16], gi[4], gip[4];
    uint32_t rtp[4]; /* ray tracing's world (gfx_rt.hlsl says what each holds) */
} FxU;

/* a target of the effects': one level (or the occlusion's depth: four), each level's state its own */
typedef struct FxTex
{
    ID3D12Resource* res;
    DXGI_FORMAT fmt;
    uint32_t w, h, levels;
    D3D12_RESOURCE_STATES state[4];
    int32_t srv, lsrv[4], rtv[4]; /* the whole; one level each (lsrv: only with more than one) */
} FxTex;

/* one of the sun's maps: depth, drawn through a depth-stencil view, read as R32F */
typedef struct SunTex
{
    ID3D12Resource* res;
    int32_t dsv, srv;
    int size;
    D3D12_RESOURCE_STATES state;
} SunTex;

#define FX_HALF DXGI_FORMAT_R16G16B16A16_FLOAT
#define FX_Z DXGI_FORMAT_R32_FLOAT
#define FX_COLOR DXGI_FORMAT_B8G8R8A8_UNORM /* the game's color targets (comp, aa and mip draw onto them) */
#define GI_MAP 1024 /* the bounce light's map's texels across */

static struct
{
    volatile LONG state; /* 0 not asked for, 1 building, 2 ready, 3 failed */
    ID3D12PipelineState *ao, *blur, *bright, *down, *gauss, *raymask, *rays, *temporal, *linz, *zmip, *comp, *aa, *mip, *gi, *gitemp;
    uint32_t cmp; /* the comparison sampler's slot (the shadow maps') */
    FxTex lz;     /* the occlusion's depth: view z at its size and three levels below (fx_linz, fx_zmip) */
    FxTex aa_src; /* the scene as it was, for the anti-aliasing pass to read (scene_aa) */
    FxTex src, ao0, ao1, b1a, b1b, b2a, b2b, ra, rb;
    FxTex hist[2];        /* the occlusion and shadows after the temporal pass: this frame's and the one before */
    int hist_at;          /* which of hist[] the frame before wrote */
    uint64_t hist_serial; /* the frame it was written (0: none) */
    float prev_view[16], prev_proj[16], prev_cam[3];
    /* what the fog and rays follow, eased from frame to frame (fx_ease) */
    int eased; /* ease (the last scene was the frame before); else take the new values */
    uint64_t eased_serial;
    float fog_on, fogc[3], up[3], sun[3], suncol[3];
    float sunw[3]; /* toward the sun in the world, as the last lit draw gave it, and the frame it was seen */
    float sun0[3], sunt[3], sunp; /* the sun's glide from its last step (sun0) to the game's (sunt), sunp of the way */
    uint64_t sunw_seen;
    float direct; /* how much of the game's light is the sun's, eased: the shadows fade with it */
    int fogc_set;
    float last_cam[3]; /* where the camera was at the last scene (a jump is a new place) */
    SunTex smap, smapn; /* the sun's maps: far, near */
    /* the bounce light: the casters near the camera as the sun sees them, depth and colour (with three
     * levels below, for the gather's wide reads); the gather at half the occlusion's size; the gather
     * after the temporal pass, this frame's and the one before */
    SunTex gimap;
    FxTex gicol, gi0, gi1, gih[2]; /* (gi1: the traced bounce light's smoothing, rt_giblur) */
    int gih_at;
    uint64_t gih_serial, prev_serial; /* the frames the bounce's history and the camera's (prev_view) were kept */
    /* the shadows' profile (FFXI_PROFILE): frames, frames with their own sun, with a map, the fewest
     * and most casters; casters drawn live, from the cache, skipped (no pipeline yet) */
    uint32_t st_frames, st_own, st_map, st_cmin, st_cmax, st_drawn_this, st_cached, st_live, st_replayed, st_skipped, st_beyond;
    float st_across, st_focus; /* the far map's width; how far ahead of the camera the player stood (-1: not known) */
} g_fx;

static ID3D12PipelineState* own_pipeline(ID3DBlob* v, ID3DBlob* f, DXGI_FORMAT fmt, int blend);

static ID3D12PipelineState* fx_pipeline(ID3DBlob* vs, const char* entry, DXGI_FORMAT fmt)
{
    ID3DBlob* ps = compile(gfx_hlsl_fx, entry, "ps_5_1");
    ID3D12PipelineState* p = own_pipeline(vs, ps, fmt, 0);
    if (ps)
        ID3D10Blob_Release(ps);
    if (!p)
        fprintf(stderr, "[recomp] gfx: scene effect %s failed\n", entry);
    return p;
}

static void fx_build(void)
{
    ID3DBlob* vs = compile(gfx_hlsl_fx, "fx_vs", "vs_5_1");
    if (vs)
    {
        g_fx.ao = fx_pipeline(vs, "fx_ao", FX_HALF);
        g_fx.blur = fx_pipeline(vs, "fx_blur", FX_HALF);
        g_fx.bright = fx_pipeline(vs, "fx_bright", FX_HALF);
        g_fx.down = fx_pipeline(vs, "fx_down", FX_HALF);
        g_fx.gauss = fx_pipeline(vs, "fx_gauss", FX_HALF);
        g_fx.raymask = fx_pipeline(vs, "fx_raymask", FX_HALF);
        g_fx.rays = fx_pipeline(vs, "fx_rays", FX_HALF);
        g_fx.temporal = fx_pipeline(vs, "fx_temporal", FX_HALF);
        g_fx.linz = fx_pipeline(vs, "fx_linz", FX_Z);
        g_fx.zmip = fx_pipeline(vs, "fx_zmip", FX_Z);
        g_fx.comp = fx_pipeline(vs, "fx_comp", FX_COLOR);
        g_fx.aa = fx_pipeline(vs, "fx_fxaa", FX_COLOR);
        g_fx.mip = fx_pipeline(vs, "fx_mip", FX_COLOR);
        g_fx.gi = fx_pipeline(vs, "fx_gi", FX_HALF);
        g_fx.gitemp = fx_pipeline(vs, "fx_gitemp", FX_HALF);
        ID3D10Blob_Release(vs);
    }
    int ok = vs && g_fx.ao && g_fx.blur && g_fx.bright && g_fx.down && g_fx.gauss && g_fx.raymask && g_fx.rays && g_fx.temporal &&
        g_fx.linz && g_fx.zmip && g_fx.comp && g_fx.aa && g_fx.mip && g_fx.gi && g_fx.gitemp;
    fprintf(stderr, ok ? "[recomp] gfx: scene effects ready\n" : "[recomp] gfx: scene effects failed: the scene goes through as it was\n");
    InterlockedExchange(&g_fx.state, ok ? 2 : 3);
}

static void CALLBACK fx_job(PTP_CALLBACK_INSTANCE inst, void* ctx)
{
    (void)inst, (void)ctx;
    fx_build();
}

/* the effects' pipelines ready (asking for them the first time) */
static int fx_init(void)
{
    if (InterlockedCompareExchange(&g_fx.state, 1, 0) == 0)
    {
        /* the comparison sampler: 1 where the point is no deeper than the map */
        int32_t slot = heap_alloc(&g_samp);
        g_fx.cmp = slot >= 0 ? (uint32_t)slot : g_present_samp;
        if (slot >= 0)
        {
            D3D12_SAMPLER_DESC sd;
            memset(&sd, 0, sizeof sd);
            sd.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxAnisotropy = 1;
            sd.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            ID3D12Device_CreateSampler(g_dev, &sd, heap_cpu(&g_samp, slot));
        }
        if (g_sync_pipelines || !TrySubmitThreadpoolCallback(fx_job, NULL, NULL))
            fx_build();
    }
    return g_fx.state == 2;
}

static void fx_tex_free(FxTex* t)
{
    if (!t->res)
        return;
    defer((IUnknown*)t->res, NULL, -1);
    defer(NULL, &g_srv, t->srv);
    for (int i = 0; i < 4; ++i)
        defer(NULL, &g_srv, t->lsrv[i]), defer(NULL, &g_rtv, t->rtv[i]);
    memset(t, 0, sizeof *t);
}

/* a target of this format, size and levels (1, or 4) in *t, made again when any of them changes */
static int fx_tex(FxTex* t, DXGI_FORMAT fmt, uint32_t w, uint32_t h, uint32_t levels)
{
    if (t->res && t->fmt == fmt && t->w == w && t->h == h && t->levels == levels)
        return 1;
    fx_tex_free(t);
    if (!w || !h)
        return 0;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, (UINT16)levels, fmt, { 1, 0 },
        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            NULL, &IID_ID3D12Resource, (void**)&t->res)))
    {
        fprintf(stderr, "[recomp] gfx: scene effect target %ux%u failed\n", w, h);
        t->res = NULL;
        return 0;
    }
    t->fmt = fmt, t->w = w, t->h = h, t->levels = levels;
    t->srv = heap_alloc(&g_srv);
    int ok = t->srv >= 0;
    D3D12_SHADER_RESOURCE_VIEW_DESC v;
    memset(&v, 0, sizeof v);
    v.Format = fmt;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MipLevels = levels;
    if (ok)
        ID3D12Device_CreateShaderResourceView(g_dev, t->res, &v, heap_cpu(&g_srv, t->srv));
    for (uint32_t i = 0; i < 4; ++i)
    {
        t->state[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        t->lsrv[i] = t->rtv[i] = -1;
        if (i >= levels)
            continue;
        D3D12_RENDER_TARGET_VIEW_DESC r;
        memset(&r, 0, sizeof r);
        r.Format = fmt, r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D, r.Texture2D.MipSlice = i;
        if ((t->rtv[i] = heap_alloc(&g_rtv)) >= 0)
            ID3D12Device_CreateRenderTargetView(g_dev, t->res, &r, heap_cpu(&g_rtv, t->rtv[i]));
        ok &= t->rtv[i] >= 0;
        if (levels > 1)
        {
            v.Texture2D.MostDetailedMip = i, v.Texture2D.MipLevels = 1;
            if ((t->lsrv[i] = heap_alloc(&g_srv)) >= 0)
                ID3D12Device_CreateShaderResourceView(g_dev, t->res, &v, heap_cpu(&g_srv, t->lsrv[i]));
            ok &= t->lsrv[i] >= 0;
        }
    }
    if (!ok)
    {
        fprintf(stderr, "[recomp] gfx: out of descriptors for the scene effects\n");
        fx_tex_free(t);
    }
    return ok;
}

/* level (-1: every level) of t into a state */
static void fx_state(FxTex* t, int level, D3D12_RESOURCE_STATES want)
{
    for (uint32_t i = 0; i < t->levels; ++i)
        if ((level < 0 || (uint32_t)level == i) && t->state[i] != want)
        {
            barrier_sub(t->res, i, t->state[i], want);
            t->state[i] = want;
        }
}

/* t (or one level of it) to be read: its heap slot */
static uint32_t fx_in(FxTex* t, int level)
{
    fx_state(t, level, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return (uint32_t)(level < 0 ? t->srv : t->levels > 1 ? t->lsrv[level] : t->srv);
}

/* a level of t to be drawn to: its view */
static D3D12_CPU_DESCRIPTOR_HANDLE fx_out(FxTex* t, int level)
{
    fx_state(t, level, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return heap_cpu(&g_rtv, t->rtv[level]);
}

/* a depth surface to be read: its heap slot */
static uint32_t fx_depth(GfxTex* d)
{
    tex_state(d, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return (uint32_t)d->srv;
}

/* the first level of a color target into t (of its size and format) */
static void fx_copy(GfxTex* from, FxTex* to)
{
    tex_state(from, D3D12_RESOURCE_STATE_COPY_SOURCE);
    fx_state(to, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    memset(&dst, 0, sizeof dst);
    memset(&src, 0, sizeof src);
    dst.pResource = to->res, dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = from->res, src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { 0, 0, 0, from->w, from->h, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &dst, 0, 0, 0, &src, &box);
}

/* one full-screen triangle into rtv over x, y, w, h, reading the textures at heap slots tex[0..n);
 * dx, dy: a blur's direction */
static void fx_pass(D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12PipelineState* p, float x, float y, float w, float h,
    D3D12_GPU_VIRTUAL_ADDRESS u, const uint32_t* tex, int n, int dx, int dy)
{
    uint32_t bind[16];
    for (int i = 0; i < 8; ++i)
        bind[i] = i < n ? tex[i] : SRV_NULL_2D;
    bind[8] = g_present_samp, bind[9] = g_fx.cmp, bind[10] = bind[11] = 0;
    bind[12] = (uint32_t)dx, bind[13] = (uint32_t)dy, bind[14] = bind[15] = 0;
    ID3D12GraphicsCommandList* l = list();
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT v = { x, y, w, h, 0, 1 };
    D3D12_RECT sc = { (LONG)x, (LONG)y, (LONG)ceilf(x + w), (LONG)ceilf(y + h) };
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, u);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
    set_pso(p);
    set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
    g_targets_bound = 0; /* the game's next draw binds its own again */
    unbind();
}

/* u into this frame's ring: its address */
static D3D12_GPU_VIRTUAL_ADDRESS fx_uniforms(const FxU* u)
{
    Alloc a = ring(sizeof *u, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return 0;
    memcpy(a.cpu, u, sizeof *u);
    return a.gpu;
}

/* t's mip chain made from its first level, a level at a time (the scene filter) */
static int gen_mips(GfxTex* t)
{
    if (!fx_init() || t->dxfmt != FX_COLOR || t->type != GFX_TEX_2D)
        return 0;
    if (!t->lvl_srv)
    {
        t->lvl_srv = (int32_t*)malloc(sizeof(int32_t) * t->mips);
        t->lvl_rtv = (int32_t*)malloc(sizeof(int32_t) * t->mips);
        for (uint32_t m = 0; m < t->mips; ++m)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v;
            memset(&v, 0, sizeof v);
            v.Format = t->dxfmt, v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MostDetailedMip = m, v.Texture2D.MipLevels = 1;
            if ((t->lvl_srv[m] = heap_alloc(&g_srv)) >= 0)
                ID3D12Device_CreateShaderResourceView(g_dev, t->res, &v, heap_cpu(&g_srv, t->lvl_srv[m]));
            D3D12_RENDER_TARGET_VIEW_DESC r;
            memset(&r, 0, sizeof r);
            r.Format = t->dxfmt, r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D, r.Texture2D.MipSlice = m;
            if ((t->lvl_rtv[m] = m ? heap_alloc(&g_rtv) : -1) >= 0)
                ID3D12Device_CreateRenderTargetView(g_dev, t->res, &r, heap_cpu(&g_rtv, t->lvl_rtv[m]));
        }
    }
    for (uint32_t m = 0; m < t->mips; ++m)
        if (t->lvl_srv[m] < 0 || (m && t->lvl_rtv[m] < 0))
            return 0;
    FxU none;
    memset(&none, 0, sizeof none);
    D3D12_GPU_VIRTUAL_ADDRESS u = fx_uniforms(&none);
    if (!u)
        return 0;
    tex_state(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (uint32_t m = 1; m < t->mips; ++m)
    {
        uint32_t w, h, src = (uint32_t)t->lvl_srv[m - 1];
        level_size(t, m, &w, &h);
        barrier_sub(t->res, m, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        fx_pass(heap_cpu(&g_rtv, t->lvl_rtv[m]), g_fx.mip, 0, 0, (float)w, (float)h, u, &src, 1, 0, 0);
        barrier_sub(t->res, m, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    return 1;
}

/* a toward b by k; the first time, b */
static void fx_ease(float* a, const float* b, int n, float k)
{
    for (int i = 0; i < n; ++i)
        a[i] = g_fx.eased ? a[i] + (b[i] - a[i]) * k : b[i];
}

static void normalize3(float* v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f)
        v[0] /= l, v[1] /= l, v[2] /= l;
}

/* the inverse of a 4x4 matrix (row-major); 0 when it has none */
static int mat_inverse(float* out, const float* m)
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f)
        return 0;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] / det;
    return 1;
}

static void mat_mul(float* o, const float* a, const float* b)
{
    float t[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, t, sizeof t);
}

void gfx_trace_dump(const char* path) { (void)path; } /* the shadows' frame-by-frame trace: Metal's only */

enum { SUN_MAP = 4096, SUN_CACHE_FRAMES = 60 * 30 };
#define SUN_CACHE_NEAR 40.0f               /* what was seen within this of the camera stays past SUN_CACHE_FRAMES */
#define SUN_COPY_BYTES (48u * 1024 * 1024) /* the copies' vertices and indices, all told */

/* The zone's casters, kept after they leave the view (gfx_metal.m's sun cache): the game draws only
 * what the camera sees, but a low sun throws the shadows of what is behind and beside the camera into
 * it. Each caster drawn from buffers the game keeps is kept with a copy of its uniforms and that
 * frame's camera (clip space back to the world), and drawn into the map from there while it is out of
 * view - for 30 seconds after it was last seen, longer while the camera stays within 40 units of where
 * it was then, and until the camera jumps (a new zone). Copies of one mesh share a key and are told
 * apart by where their first vertex stands (draw_clip0); what the game draws from buffers it rewrites
 * is kept as a copy of its vertices and indices as last drawn. An entry in plain view the game did not
 * draw this frame has gone and is dropped. */
typedef struct CacheKey
{
    D3D12_GPU_VIRTUAL_ADDRESS va[GFX_NSTREAMS], iva;
    uint32_t n, vstart, copy;
    LibKey lib;
} CacheKey;

typedef struct Cached
{
    Caster c;            /* c.vb[] / c.ib: the game's buffers (fixed), or NULL with c.va / c.iva into copy */
    GfxU u;              /* its uniforms when last seen */
    int32_t copy;         /* a copy's vertices and indices: its chunk (g_copy) + 1, 0 none */
    uint8_t* copy_cpu;    /* where they are in it */
    D3D12_GPU_VIRTUAL_ADDRESS copy_gpu;
    uint32_t copy_bytes;  /* the room it holds there */
    float clip_world[16];
    float pos[3], cam[3]; /* where it stood and where the camera was, when last seen */
    uint64_t seen, replayed;
    int32_t next; /* the next entry with its key, -1 none */
    int dead;     /* a buffer or texture it draws from went (the game let go of it: a zone left behind) */
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
        memcpy(k->va, c->va, sizeof k->va);
        k->iva = c->iva;
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

/* The copies live in 4 MB chunks of the upload heap, at most SUN_COPY_BYTES of them, each copy within
 * one; a chunk is filled again from its start once every copy in it is gone and no frame in flight
 * reads them. (A buffer of each copy's own took 64 KB at the least: 48 MB of small meshes was a
 * gigabyte and more.) */
enum { COPY_CHUNK = 4u << 20, COPY_CHUNKS = SUN_COPY_BYTES / COPY_CHUNK };
typedef struct CopyChunk
{
    ID3D12Resource* res;
    uint8_t* cpu;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    uint32_t used, live; /* bytes handed out since it was last empty, and those still held */
    uint64_t emptied;    /* the frame its last copy went */
} CopyChunk;
static CopyChunk g_copy[COPY_CHUNKS];

static void cached_release(Cached* ce)
{
    if (ce->copy)
    {
        CopyChunk* k = &g_copy[ce->copy - 1];
        k->live -= ce->copy_bytes;
        if (!k->live)
            k->emptied = g_serial;
        ce->copy = 0, ce->copy_cpu = NULL, ce->copy_gpu = 0;
    }
    g_copy_bytes -= ce->copy_bytes, ce->copy_bytes = 0;
}

/* a buffer the game rewrote or destroyed: what the cache draws from it goes, and this frame's casters
 * let go of it */
static void sun_cache_forget(const GfxBuf* b)
{
    for (uint32_t i = 0; i < g_ncache; ++i)
    {
        Caster* c = &g_cache[i].c;
        int hit = c->ib == b;
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            hit |= c->vb[s] == b;
        if (hit)
            g_cache[i].dead = 1;
    }
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* c = &g_casters[i];
        int hit = c->ib == b;
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            hit |= c->vb[s] == b;
        if (hit)
            c->n = 0; /* nothing left to draw */
    }
}

/* a texture the game destroyed: what draws with it (an alpha test's) goes */
static void sun_cache_forget_tex(const GfxTex* t)
{
    for (uint32_t i = 0; i < g_ncache; ++i)
        for (int k = 0; k < 8; ++k)
            if (g_cache[i].c.tex[k] == t)
                g_cache[i].dead = 1, g_cache[i].c.tex[k] = NULL;
    for (uint32_t i = 0; i < g_ncasters; ++i)
        for (int k = 0; k < 8; ++k)
            if (g_casters[i].tex[k] == t)
                g_casters[i].n = alpha_tested(&g_casters[i].lib.fs) ? 0 : g_casters[i].n, g_casters[i].tex[k] = NULL;
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

/* row vector p (x, y, z, w) through a row-major matrix */
static void xform4(float* o, const float* p, const float* m)
{
    float t[4];
    for (int j = 0; j < 4; ++j)
        t[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + p[3] * m[12 + j];
    memcpy(o, t, 16);
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

/* room for a copy of `need` bytes (a multiple of 256) in a chunk: its index in *at and its offset in
 * *off; 0 when there is none this frame. With every chunk made and full, the copies of the chunk
 * seen longest ago go (none seen this frame, not `keep`'s), and it is filled again once no frame in
 * flight reads them. */
static int copy_take(uint32_t need, const Cached* keep, int* at, uint32_t* off)
{
    if (!need || need > COPY_CHUNK)
        return 0;
    int empty = -1;
    for (int i = 0; i < COPY_CHUNKS; ++i)
    {
        CopyChunk* k = &g_copy[i];
        if (!k->res)
        {
            if (empty < 0)
                empty = i;
            continue;
        }
        if (!k->live && k->used && k->emptied + FRAMES < g_serial)
            k->used = 0;
        if (k->used + need <= COPY_CHUNK)
        {
            *at = i, *off = k->used;
            k->used += need, k->live += need;
            return 1;
        }
    }
    if (empty >= 0)
    {
        CopyChunk* k = &g_copy[empty];
        k->res = make_buffer(COPY_CHUNK, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!k->res)
            return 0;
        ID3D12Resource_Map(k->res, 0, NULL, (void**)&k->cpu);
        k->gpu = ID3D12Resource_GetGPUVirtualAddress(k->res);
        *at = empty, *off = 0;
        k->used = k->live = need;
        return 1;
    }
    /* all made and full: empty the stalest */
    uint64_t newest[COPY_CHUNKS] = { 0 };
    for (uint32_t i = 0; i < g_ncache; ++i)
    {
        const Cached* ce = &g_cache[i];
        uint64_t seen = ce == keep ? g_serial : ce->seen;
        if (ce->copy && seen > newest[ce->copy - 1])
            newest[ce->copy - 1] = seen;
    }
    int old = -1;
    for (int i = 0; i < COPY_CHUNKS; ++i)
        if (g_copy[i].live && newest[i] != g_serial && (old < 0 || newest[i] < newest[old]))
            old = i;
    if (old >= 0)
        for (uint32_t i = 0; i < g_ncache; ++i)
            if (g_cache[i].copy == old + 1)
                g_cache[i].dead = 1, cached_release(&g_cache[i]);
    return 0;
}

/* the vertices and indices caster c draws, copied into ce's own buffer: only the vertices its indices
 * reach, with the uniforms' vertex offset (ub, the cache's copy) moved to match. 0 if it cannot be. */
static int cache_copy(Cached* ce, const Caster* c, GfxU* ub)
{
    D3D12_GPU_VIRTUAL_ADDRESS dummy = ID3D12Resource_GetGPUVirtualAddress(g_dummy);
    long lo = c->vstart, hi = (long)c->vstart + (long)c->n - 1;
    if (c->itype)
    {
        if (!c->icpu)
            return 0;
        lo = LONG_MAX, hi = -1;
        for (uint32_t i = 0; i < c->n; ++i)
        {
            long x = c->itype == 2 ? ((const uint16_t*)c->icpu)[i] : (long)((const uint32_t*)c->icpu)[i];
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
        if (c->va[s] == dummy || !c->va[s])
            continue;
        if (!c->vcpu[s])
            return 0; /* bytes it reads that the CPU cannot */
        long stride = ub->stride[s], from = lo * stride, n = (hi - lo + 1) * stride;
        if (!stride)
            from = 0, n = 64;
        long have = (long)c->vlen[s] - from;
        if (have <= 0)
            return 0;
        len[s] = (uint32_t)(n < have ? n : have);
        total += (len[s] + 15) & ~15u;
    }
    uint32_t ioff = total, ilen = c->itype ? c->n * c->itype : 0;
    total += (ilen + 15) & ~15u;
    if (!total)
        return 0;
    /* the copy's room again when it fits and no frame in flight reads it (the slack: a last element read whole) */
    uint32_t room = (total + 16 + 255) & ~255u;
    if (!(ce->copy && room <= ce->copy_bytes && ce->replayed + FRAMES < g_serial))
    {
        int at;
        uint32_t at_off;
        if (!copy_take(room, ce, &at, &at_off))
            return 0;
        cached_release(ce);
        ce->copy = at + 1;
        ce->copy_cpu = g_copy[at].cpu + at_off, ce->copy_gpu = g_copy[at].gpu + at_off;
        ce->copy_bytes = room, g_copy_bytes += room;
    }
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (len[s])
            memcpy(ce->copy_cpu + off[s], c->vcpu[s] + (ub->stride[s] ? lo * ub->stride[s] : 0), len[s]);
    if (ilen)
        memcpy(ce->copy_cpu + ioff, c->icpu, ilen);
    /* the copy's own buffers in place of the caster's */
    D3D12_GPU_VIRTUAL_ADDRESS gpu = ce->copy_gpu;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        ce->c.vb[s] = NULL, ce->c.vcpu[s] = NULL;
        ce->c.va[s] = len[s] ? gpu + off[s] : dummy;
    }
    ce->c.ib = NULL, ce->c.icpu = NULL;
    ce->c.iva = ilen ? gpu + ioff : 0;
    ub->vofs -= (int32_t)lo;
    return 1;
}

/* is a world point in plain view of this frame's camera (w >= 1, within 90% of the frustum)? */
static int in_plain_view(const float* p, const float* vp)
{
    float q[4] = { p[0], p[1], p[2], 1.0f }, c[4];
    xform4(c, q, vp);
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
    }
    memcpy(g_cache_cam, cam, 12);
    if (!(g_serial & 255))
        sun_cache_trim(0);
    /* what is not drawn from the zone's own buffers: a placed object (keep: kept as a copy) or a
     * character (not kept: its vertices are in the world already, and it moves). A fixed-function
     * draw whose world matrix is the identity is a character; a vertex shader's draw is the zone's. */
    float invView[16];
    int have_iv = mat_inverse(invView, view);
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* c = &g_casters[i];
        if (c->fixed)
            continue;
        c->keep = (uint8_t)(c->lib.vs.prog != 0);
        if (!c->lib.vs.prog && have_iv)
        {
            float w[16], off = 0.0f;
            mat_mul(w, c->wv, invView);
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
        if (!c->n || c->uidx < 0 || (!c->fixed && (!c->keep || !c->has_pos)))
            continue;
        float pos[3] = { 0, 0, 0 };
        if (c->has_pos)
        {
            float h[4];
            xform4(h, c->clip0, clip_world);
            if (fabsf(h[3]) < 1e-6f)
                continue;
            pos[0] = h[0] / h[3], pos[1] = h[1] / h[3], pos[2] = h[2] / h[3];
        }
        CacheKey k;
        int copy = !c->fixed, fresh;
        cache_key(&k, c, copy);
        Cached* ce = cache_slot(&k, c, pos, &fresh);
        if (fresh || copy)
        {
            /* what it draws with as of this frame (a copy's buffers are its own: cache_copy) */
            Caster was = ce->c;
            ce->c = *c;
            if (!alpha_tested(&c->lib.fs)) /* its textures are the bounce light's, which draws this frame's alone */
                memset(ce->c.tex, 0, sizeof ce->c.tex);
            if (copy && !fresh)
                memcpy(ce->c.va, was.va, sizeof was.va), ce->c.iva = was.iva;
        }
        ce->u = g_caster_u[c->uidx]; /* the uniforms as of this frame */
        if (copy && !cache_copy(ce, c, &ce->u))
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

/* One cascade of the sun's map: an orthographic view along the sun over a sphere around the slice
 * [t0, t1] of what the camera sees, its center snapped to whole texels (the shadows' edges hold still
 * as the camera moves). */
typedef struct SunCascade
{
    float S[16];    /* the world to the map */
    float lmat[16]; /* view space to the map: x, y -1..1, z 0..1 */
    float texel, bias, soft, slope, range, across;
    int size; /* the map's texels across */
} SunCascade;

static void sun_fit(const GfxScene* s, const float* invV, const float* L, float t0, float t1, int size, SunCascade* k)
{
    float hand = s->proj[11] < 0.0f ? -1.0f : 1.0f, p[8][3], c[3] = { 0, 0, 0 };
    for (int i = 0; i < 8; ++i)
    {
        float t = i < 4 ? t0 : t1, z = t * hand, nx = (i & 1) ? 1.0f : -1.0f, ny = (i & 2) ? 1.0f : -1.0f;
        float v[4] = { (nx * t - s->proj[8] * z) / s->proj[0], (ny * t - s->proj[9] * z) / s->proj[5], z, 1.0f };
        for (int j = 0; j < 3; ++j)
            p[i][j] = v[0] * invV[j] + v[1] * invV[4 + j] + v[2] * invV[8 + j] + invV[12 + j], c[j] += p[i][j] / 8.0f;
    }
    float R = 0.0f;
    for (int i = 0; i < 8; ++i)
    {
        float dx = p[i][0] - c[0], dy = p[i][1] - c[1], dz = p[i][2] - c[2];
        R = fmaxf(R, sqrtf(dx * dx + dy * dy + dz * dz));
    }
    R = ceilf(R); /* whole units: the texel's size holds still */
    /* the sun's axes: f the way its light goes, r and u across */
    float f[3] = { -L[0], -L[1], -L[2] }, up[3] = { 0, 1, 0 };
    if (fabsf(f[1]) > 0.9f)
        up[0] = 1, up[1] = 0;
    float r[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
    normalize3(r);
    float u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    float tx = 2.0f * R / (float)size;
    k->size = size;
    float cx = floorf((c[0] * r[0] + c[1] * r[1] + c[2] * r[2]) / tx) * tx;
    float cy = floorf((c[0] * u[0] + c[1] * u[1] + c[2] * u[2]) / tx) * tx;
    float cz = c[0] * f[0] + c[1] * f[1] + c[2] * f[2];
    /* casters stand up to 200 units sunward of the sphere (a cliff over the camera) */
    float back = 200.0f, range = 2.0f * R + back, z0 = cz - R - back;
    float S[16] = {
        r[0] / R, u[0] / R, f[0] / range, 0,
        r[1] / R, u[1] / R, f[1] / range, 0,
        r[2] / R, u[2] / R, f[2] / range, 0,
        -cx / R, -cy / R, -z0 / range, 1,
    };
    memcpy(k->S, S, sizeof S);
    mat_mul(k->lmat, invV, S);
    k->texel = tx, k->bias = 0.03f / range, k->range = range, k->across = 2.0f * R;
    /* the penumbra's radius grows by sun_soft for each unit from the caster: in the map's width (2R
     * across) per unit of its depth (range deep) - never less than the sun's own disc gives (half a
     * degree across: 0.0047 a unit), so a character's shadow stays hard and a cliff's 30 units off
     * no longer ends in its low-polygon outline */
    k->soft = fmaxf(g_fxs.sun_soft, 0.0047f) * range / (2.0f * R);
    k->slope = 2.0f * R / range;
}

/* one of the sun's maps at size x size (made again when the size changes) */
static int sun_target(SunTex* t, int size)
{
    if (t->res && t->size == size)
        return 1;
    if (t->res)
    {
        defer((IUnknown*)t->res, NULL, -1);
        defer(NULL, &g_dsv, t->dsv);
        defer(NULL, &g_srv, t->srv);
        memset(t, 0, sizeof *t);
    }
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, (UINT64)size, (UINT)size, 1, 1, DXGI_FORMAT_R32_TYPELESS,
        { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL };
    D3D12_CLEAR_VALUE cv;
    memset(&cv, 0, sizeof cv);
    cv.Format = DXGI_FORMAT_D32_FLOAT, cv.DepthStencil.Depth = 1.0f;
    if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            &IID_ID3D12Resource, (void**)&t->res)))
    {
        fprintf(stderr, "[recomp] gfx: the sun's %dx%d map failed\n", size, size);
        t->res = NULL;
        return 0;
    }
    t->size = size, t->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    t->dsv = heap_alloc(&g_dsv), t->srv = heap_alloc(&g_srv);
    if (t->dsv < 0 || t->srv < 0)
    {
        defer((IUnknown*)t->res, NULL, -1), defer(NULL, &g_dsv, t->dsv), defer(NULL, &g_srv, t->srv);
        memset(t, 0, sizeof *t);
        return 0;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    memset(&dv, 0, sizeof dv);
    dv.Format = DXGI_FORMAT_D32_FLOAT, dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateDepthStencilView(g_dev, t->res, &dv, heap_cpu(&g_dsv, t->dsv));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    memset(&sv, 0, sizeof sv);
    sv.Format = DXGI_FORMAT_R32_FLOAT, sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, sv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(g_dev, t->res, &sv, heap_cpu(&g_srv, t->srv));
    return 1;
}

static void sun_state(SunTex* t, D3D12_RESOURCE_STATES want)
{
    if (t->state != want)
        barrier(t->res, t->state, want), t->state = want;
}

/* m (64 bytes) into this frame's ring: its address, 0 when the ring has no room */
static D3D12_GPU_VIRTUAL_ADDRESS ring_bytes(const void* m, size_t n)
{
    Alloc a = ring(n, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return 0;
    memcpy(a.cpu, m, n);
    return a.gpu;
}

/* is a world point more than 96 units outside the cascade's sides? What stands there casts nothing
 * into it (and with a long draw distance the cache held thousands of such, drawn into both maps
 * every frame) */
static int beyond_map(const SunCascade* k, const float* p)
{
    float q[4] = { p[0], p[1], p[2], 1.0f }, m[4];
    xform4(m, q, k->S);
    return (fmaxf(fabsf(m[0]), fabsf(m[1])) - 1.0f) * 0.5f * k->across > 96.0f;
}

/* is a world point more than margin units outside the cascade's sides? */
static int beyond_by(const SunCascade* k, const float* p, float margin)
{
    float q[4] = { p[0], p[1], p[2], 1.0f }, m[4];
    xform4(m, q, k->S);
    return (fmaxf(fabsf(m[0]), fabsf(m[1])) - 1.0f) * 0.5f * k->across > margin;
}

/* the casters into one cascade's map: this frame's, and the zone's kept from before (cache). With col,
 * the bounce light's: every caster (whoever casts) in its own colour as well, through its own pixel
 * function, unfogged. How many were drawn; -1 when the map was not even cleared (it still holds an
 * earlier frame's) */
static int sun_draw(SunTex* t, FxTex* col, const float* invP, const float* invV, const SunCascade* k, int cache)
{
    float clip_world[16], M[16];
    mat_mul(clip_world, invP, invV);
    mat_mul(M, clip_world, k->S); /* the camera's clip space -> the map */
    D3D12_GPU_VIRTUAL_ADDRESS mlive = ring_bytes(M, 64), dummy = ID3D12Resource_GetGPUVirtualAddress(g_dummy);
    if (!mlive)
        return -1;
    ID3D12GraphicsCommandList* l = list();
    sun_state(t, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = heap_cpu(&g_dsv, t->dsv);
    ID3D12GraphicsCommandList_ClearDepthStencilView(l, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    if (col)
    {
        static const float black[4] = { 0, 0, 0, 0 };
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = fx_out(col, 0);
        ID3D12GraphicsCommandList_ClearRenderTargetView(l, rtv, black, 0, NULL);
        ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, &dsv);
    }
    else
        ID3D12GraphicsCommandList_OMSetRenderTargets(l, 0, NULL, FALSE, &dsv);
    D3D12_VIEWPORT v = { 0, 0, (float)k->size, (float)k->size, 0, 1 };
    D3D12_RECT sc = { 0, 0, k->size, k->size };
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_SHADOW, mlive);
    int live_m = 1, drawn = 0;
    uint32_t total = g_ncasters + (cache ? g_ncache : 0);
    for (uint32_t i = 0; i < total; ++i)
    {
        const Caster* cs;
        D3D12_GPU_VIRTUAL_ADDRESS ugpu;
        if (i < g_ncasters)
        {
            cs = &g_casters[i];
            if (!cs->n)
                continue;
            /* sun_casters 1: characters alone cast - the zone's shadows are baked into its colours
             * already, and the game tints them for the hour and the weather */
            if (!col && ((g_fxs.sun_casters == 1.0f && (cs->fixed || cs->keep)) || (g_fxs.sun_casters == 2.0f && !cs->fixed && !cs->keep)))
                continue;
            if (cs->has_wpos && (col ? beyond_by(k, cs->wpos, 16.0f) : beyond_map(k, cs->wpos)))
            {
                g_fx.st_beyond++;
                continue;
            }
            if (!live_m)
                ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_SHADOW, mlive), live_m = 1;
            ugpu = cs->ugpu;
        }
        else
        {
            if (g_fxs.sun_casters == 1.0f)
                break;
            /* the zone out of view: as it was drawn when last seen, through that frame's camera */
            Cached* ce = &g_cache[i - g_ncasters];
            if (ce->dead || ce->seen == g_serial || cached_expired(ce) || !ce->c.n)
                continue;
            if (ce->c.has_pos && beyond_map(k, ce->pos))
            {
                g_fx.st_beyond++;
                continue;
            }
            float m[16];
            mat_mul(m, ce->clip_world, k->S);
            D3D12_GPU_VIRTUAL_ADDRESS mg = ring_bytes(m, 64);
            ugpu = ring_bytes(&ce->u, sizeof ce->u);
            if (!mg || !ugpu)
                break;
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_SHADOW, mg), live_m = 0;
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
            GfxVsKey* vk = &pk.lib.vs;
            vk->lighting = vk->normalize = vk->localviewer = vk->specular = 0;
            vk->src_diffuse = vk->src_specular = vk->src_ambient = vk->src_emissive = 0;
            vk->nlights = 0, memset(vk->light_type, 0, sizeof vk->light_type);
            vk->fog_vertex = vk->range_fog = 0, vk->ntex = 0, vk->flat = 0;
            memset(vk->tci, 0, sizeof vk->tci), memset(vk->ttf, 0, sizeof vk->ttf);
            memset(&pk.lib.fs, 0, sizeof pk.lib.fs);
        }
        pk.depth.zenable = 1, pk.depth.zwrite = 1, pk.depth.zfunc = 2; /* LESS */
        pk.color = col ? FX_COLOR : DXGI_FORMAT_UNKNOWN, pk.dsv = DXGI_FORMAT_D32_FLOAT;
        pk.cull = 1, pk.fill = 3; /* both faces: a caster's back faces cast as well */
        pk.topo = (uint8_t)topology_type(cs->prim);
        ID3D12PipelineState* p = pipeline_for(&pk, cs->vs, cs->ps);
        if (!p)
        {
            g_fx.st_skipped++;
            continue;
        }
        if (i < g_ncasters)
            g_fx.st_live++;
        else
            g_fx.st_replayed++;
        set_pso(p);
        set_topology(topology(cs->prim));
        for (int st = 0; st < GFX_NSTREAMS; ++st)
        {
            if (cs->vb[st])
                buf_state(cs->vb[st], BUF_READ);
            ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + st, cs->va[st] ? cs->va[st] : dummy);
        }
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, ugpu);
        if (tex)
        {
            uint32_t bind[16];
            for (int tx = 0; tx < 8; ++tx)
            {
                GfxTex* tt = cs->tex[tx];
                bind[tx] = SRV_NULL_2D, bind[8 + tx] = g_present_samp;
                if (tt && tt->srv >= 0 && tt->type == GFX_TEX_2D)
                {
                    tex_state(tt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    bind[tx] = (uint32_t)tt->srv, bind[8 + tx] = sampler_slot(&cs->samp[tx]);
                }
                else if (tt && tt->srv >= 0 && tt->type == GFX_TEX_CUBE)
                    bind[tx] = SRV_NULL_CUBE;
            }
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
        }
        if (cs->itype)
        {
            if (cs->ib)
                buf_state(cs->ib, BUF_READ);
            D3D12_INDEX_BUFFER_VIEW ibv = { cs->iva, cs->n * cs->itype, cs->itype == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
            ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ibv);
            ID3D12GraphicsCommandList_DrawIndexedInstanced(l, cs->n, 1, 0, 0, 0);
        }
        else
            ID3D12GraphicsCommandList_DrawInstanced(l, cs->n, 1, cs->vstart, 0);
        drawn++;
    }
    g_targets_bound = 0; /* the game's next draw binds its own again */
    unbind();
    return drawn;
}

/* The sun's maps for the scene: a near cascade over the first sun_near units the camera sees, where
 * characters stand and shadows are looked at closely, and a far one out to sun_distance. Fills the
 * effects' uniforms for both; 0 when nothing was drawn. */
static int sun_map(const GfxScene* s, const float* L, FxU* u)
{
    if (!g_ncasters && !g_ncache)
        return 0;
    float invP[16], invV[16];
    if (!mat_inverse(invP, s->proj) || !mat_inverse(invV, s->view))
        return 0;
    float clip_world[16], vp[16];
    mat_mul(clip_world, invP, invV);
    mat_mul(vp, s->view, s->proj);
    sun_cache_update(clip_world, s->view, vp, invV + 12);
    /* this frame's casters where they stand in the world, for sun_draw to leave out what is far
     * outside a map */
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* c = &g_casters[i];
        c->has_wpos = 0;
        if (!c->has_pos)
            continue;
        float h[4];
        xform4(h, c->clip0, clip_world);
        if (fabsf(h[3]) > 1e-6f)
            c->wpos[0] = h[0] / h[3], c->wpos[1] = h[1] / h[3], c->wpos[2] = h[2] / h[3], c->has_wpos = 1;
    }
    /* the near map reaches sun_near past the player, not the camera (gfx_metal.m's): the camera stands
     * some units behind the character, and a reach from it left the far half of the player's own
     * shadow in the coarse map. A player farther off than 40 units is not the one in view (a cutscene) */
    /* sun_near 3 to 80 (0 or less: no near map); past the far map's reach it stops at it */
    float dfar = fmaxf(g_fxs.sun_distance, 4.0f), dnear = g_fxs.sun_near > 0.0f ? fminf(fmaxf(g_fxs.sun_near, 3.0f), 80.0f) : 0.0f;
    float lead = 0.0f;
    g_fx.st_focus = -1.0f;
    if (g_focus_serial && g_focus_serial + 2 >= g_serial)
    {
        float dx = g_focus[0] - invV[12], dy = g_focus[1] - invV[13], dz = g_focus[2] - invV[14];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        lead = d < 40.0f ? d : 0.0f;
        g_fx.st_focus = d;
    }
    float tnear = fminf(dnear + lead, dfar);
    SunCascade far_k, near_k;
    sun_fit(s, invV, L, 0.5f, dfar, SUN_MAP, &far_k);
    if (!sun_target(&g_fx.smap, SUN_MAP))
        return 0;
    int drawn = sun_draw(&g_fx.smap, NULL, invP, invV, &far_k, 1);
    memcpy(u->lmat, far_k.lmat, 64);
    u->smap[1] = far_k.texel, u->smap[2] = far_k.bias, u->smap[3] = far_k.soft;
    u->smap2[0] = far_k.slope, u->smap2[3] = far_k.range;
    u->smapn2[1] = 0.0f;
    int nsize = sun_near_size();
    if (dnear > 0.0f && sun_target(&g_fx.smapn, nsize))
    {
        sun_fit(s, invV, L, 0.5f, tnear, nsize, &near_k);
        if (sun_draw(&g_fx.smapn, NULL, invP, invV, &near_k, 1) >= 0) /* not an earlier frame's map through this one's matrix */
        {
            memcpy(u->lmatn, near_k.lmat, 64);
            u->smapn[0] = near_k.texel, u->smapn[1] = near_k.bias, u->smapn[2] = near_k.soft, u->smapn[3] = near_k.slope;
            u->smapn2[0] = near_k.range, u->smapn2[1] = 1.0f;
        }
    }
    /* the bounce light's map: this frame's casters over the first gi_distance units the camera sees, in
     * colour, 1024 across (light thrown a few units wants no finer), and its levels below */
    u->gi[0] = 0.0f;
    float gd = fminf(fmaxf(g_fxs.gi_distance, 8.0f), dfar);
    if (g_fxs.gi > 0.0f && sun_target(&g_fx.gimap, GI_MAP) && fx_tex(&g_fx.gicol, FX_COLOR, GI_MAP, GI_MAP, 4))
    {
        SunCascade gk;
        float inv[16];
        sun_fit(s, invV, L, 0.5f, gd, GI_MAP, &gk);
        if (sun_draw(&g_fx.gimap, &g_fx.gicol, invP, invV, &gk, 0) > 0 && mat_inverse(inv, gk.lmat))
        {
            FxU mu;
            memset(&mu, 0, sizeof mu);
            D3D12_GPU_VIRTUAL_ADDRESS ua = fx_uniforms(&mu);
            for (int lv = 1; lv < 4 && ua; ++lv)
            {
                uint32_t in = fx_in(&g_fx.gicol, lv - 1);
                fx_pass(fx_out(&g_fx.gicol, lv), g_fx.mip, 0, 0, (float)(GI_MAP >> lv), (float)(GI_MAP >> lv), ua, &in, 1, 0, 0);
            }
            /* the reach in the map's uv; the level read where a texel is about a quarter of it */
            float r = fmaxf(g_fxs.gi_radius, 0.5f);
            memcpy(u->gimat, gk.lmat, 64), memcpy(u->giinv, inv, 64);
            u->gi[0] = 1.0f, u->gi[1] = r / gk.across, u->gi[2] = r;
            u->gi[3] = fminf(fmaxf(log2f(r / (4.0f * gk.texel)), 0.0f), 3.0f);
            u->gip[3] = gd;
        }
    }
    g_fx.st_across = far_k.across;
    g_fx.st_cached = g_ncache;
    return drawn > 0;
}

/* --- ray tracing (rt) -------------------------------------------------------------------------------------------
 * The world for rays, made from the frame's casters as the sun's maps are: each drawn once more through
 * its own vertex function (the capture key, GfxVsKey.shadow 2: the position on into this frame's view
 * space, divided through), its triangles streamed out (stream output) one after another into one buffer,
 * and one acceleration structure built over them all, every frame - a structure over a single instance of
 * it on top. The casters out of view come from the sun's cache, through the camera they were drawn with.
 * Rays are traced inline (ray queries) from the scene effects' passes, gfx_rt.hlsl's, built with dxc at
 * build time (generated/gfx_rt_dxil.h); with no such device, or no dxc at build time, none are. */
#define RT_REACH 150.0f /* the cache's casters further than this from the camera are left out */

static struct
{
    int tried, ok;
    ID3D12Device5* dev;
    ID3D12PipelineState *clay, *gi, *ao, *giblur;
    ID3D12RootSignature* croot; /* the normals' compute passes': eight root constants */
    ID3D12PipelineState *nclear, *nsum, *nresolve;
    ID3D12Resource *so, *count, *blas, *blas2, *tlas, *scratch, *nrm, *table; /* (blas2: the alpha-tested) */
    uint64_t so_size, count_size, blas_size, blas2_size, tlas_size, scratch_size, nrm_size, table_size;
    D3D12_RESOURCE_STATES so_state, count_state, nrm_state;
    int32_t so_srv, tlas_srv; /* the triangles (a structured buffer of float4) and the structure, in the heap */
    int32_t nrm_srv, nrm_uav, table_uav; /* the corners' smooth normals (rt_nresolve), the table that finds them */
    ID3D12Resource* tab; /* the alpha tests' table (gfx_rt.hlsl alpha_holds) */
    uint64_t tab_size;
    D3D12_RESOURCE_STATES tab_state;
    int32_t tab_srv;
    uint32_t nsolid, nalpha; /* this frame's solid triangles, alpha-tested casters */
    /* why the sun's cache gave none (the profile) */
    uint32_t st_c_gone, st_c_seen, st_c_far, st_c_none;
    uint32_t verts;           /* this frame's: three a triangle */
    uint64_t serial;          /* the frame they were made (0: none yet) */
    /* the profile: casters captured, from the cache, skipped (no pipeline yet), triangles */
    uint32_t st_live, st_cached, st_skipped, st_frames;
    uint64_t st_tris;
} g_ray;

static int g_root_heap; /* the root signature reads the heap directly (shader model 6.6's ResourceDescriptorHeap) */

static ID3DBlob* rt_blob(const unsigned char* p, size_t n)
{
    ID3DBlob* b = NULL;
    if (FAILED(D3DCreateBlob(n, &b)))
        return NULL;
    memcpy(ID3D10Blob_GetBufferPointer(b), p, n);
    return b;
}

/* whether rays can be traced here (asking once) */
static int rt_init(void)
{
    if (g_ray.tried)
        return g_ray.ok;
    g_ray.tried = 1;
#ifdef GFX_RT_NONE
    fprintf(stderr, "[recomp] gfx: ray tracing: not built (no dxc at build time)\n");
    return 0;
#else
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5;
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_6 };
    memset(&o5, 0, sizeof o5);
    if (!g_root_heap || FAILED(ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5)) ||
        o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 || FAILED(ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_6 ||
        FAILED(ID3D12Device_QueryInterface(g_dev, &IID_ID3D12Device5, (void**)&g_ray.dev)))
    {
        fprintf(stderr, "[recomp] gfx: ray tracing: not on this device (tier %d, shader model %x, heap %d)\n", (int)o5.RaytracingTier,
            (unsigned)sm.HighestShaderModel, g_root_heap);
        return 0;
    }
    ID3DBlob* vs = rt_blob(RT_rt_vs, sizeof RT_rt_vs);
    const struct { const unsigned char* code; size_t n; ID3D12PipelineState** p; DXGI_FORMAT fmt; } ps[] = {
        { RT_rt_clay, sizeof RT_rt_clay, &g_ray.clay, FX_COLOR }, { RT_rt_gi, sizeof RT_rt_gi, &g_ray.gi, FX_HALF },
        { RT_rt_ao, sizeof RT_rt_ao, &g_ray.ao, FX_HALF }, { RT_rt_giblur, sizeof RT_rt_giblur, &g_ray.giblur, FX_HALF },
    };
    g_ray.ok = 1;
    for (size_t i = 0; i < sizeof ps / sizeof ps[0]; ++i)
    {
        ID3DBlob* f = rt_blob(ps[i].code, ps[i].n);
        *ps[i].p = own_pipeline(vs, f, ps[i].fmt, 0);
        g_ray.ok &= *ps[i].p != NULL;
        if (f)
            ID3D10Blob_Release(f);
    }
    if (vs)
        ID3D10Blob_Release(vs);
    g_ray.so_srv = g_ray.tlas_srv = g_ray.nrm_srv = g_ray.nrm_uav = g_ray.table_uav = g_ray.tab_srv = -1;
    D3D12_ROOT_PARAMETER cp;
    memset(&cp, 0, sizeof cp);
    cp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    cp.Constants.ShaderRegister = 0, cp.Constants.Num32BitValues = 8;
    cp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC crd = { 1, &cp, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED };
    ID3DBlob *cblob = NULL, *cerr = NULL;
    if (SUCCEEDED(D3D12SerializeRootSignature(&crd, D3D_ROOT_SIGNATURE_VERSION_1, &cblob, &cerr)))
        ID3D12Device_CreateRootSignature(g_dev, 0, ID3D10Blob_GetBufferPointer(cblob), ID3D10Blob_GetBufferSize(cblob),
            &IID_ID3D12RootSignature, (void**)&g_ray.croot);
    if (cblob)
        ID3D10Blob_Release(cblob);
    if (cerr)
        ID3D10Blob_Release(cerr);
    const struct { const unsigned char* code; size_t n; ID3D12PipelineState** p; } cs[] = {
        { RT_rt_nclear, sizeof RT_rt_nclear, &g_ray.nclear }, { RT_rt_nsum, sizeof RT_rt_nsum, &g_ray.nsum },
        { RT_rt_nresolve, sizeof RT_rt_nresolve, &g_ray.nresolve },
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
        memset(&cd, 0, sizeof cd);
        cd.pRootSignature = g_ray.croot;
        cd.CS.pShaderBytecode = cs[i].code, cd.CS.BytecodeLength = cs[i].n;
        if (!g_ray.croot || FAILED(ID3D12Device_CreateComputePipelineState(g_dev, &cd, &IID_ID3D12PipelineState, (void**)cs[i].p)))
            *cs[i].p = NULL, g_ray.ok = 0;
    }
    fprintf(stderr, g_ray.ok ? "[recomp] gfx: ray tracing ready\n" : "[recomp] gfx: ray tracing: its pipelines failed\n");
    return g_ray.ok;
#endif
}

/* a buffer of the GPU's own of at least need bytes in *r (made again, half as large again, when it is
 * smaller), in state; uav: the acceleration structures' and scratch's */
static int rt_buffer(ID3D12Resource** r, uint64_t* size, uint64_t need, D3D12_RESOURCE_STATES state, int uav)
{
    if (*r && *size >= need)
        return 0;
    if (*r)
        defer((IUnknown*)*r, NULL, -1), *r = NULL;
    need = (need + need / 2 + 65535) & ~(uint64_t)65535;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, need, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE };
    if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void**)r)))
    {
        fprintf(stderr, "[recomp] gfx: ray tracing: a buffer of %llu bytes failed\n", (unsigned long long)need);
        *r = NULL, *size = 0;
        return -1;
    }
    *size = need;
    return 1;
}

static void rt_uav_barrier(void)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; /* every UAV: the scratch is shared by both builds */
    ID3D12GraphicsCommandList_ResourceBarrier(list(), 1, &b);
}

/* one caster to capture: what it draws, through which matrix (clip space to this frame's view space),
 * and its triangles */
typedef struct RtItem
{
    const Caster* c;
    const GfxU* u; /* a cache entry's own uniforms (NULL: the caster's, ugpu) */
    float m[16];
    uint32_t tris;
    uint8_t alpha; /* traced through its alpha test (rt_alpha) */
} RtItem;

/* traced through its alpha test: an alpha-tested caster whose vertex function gives texture coordinates
 * and whose first stage's texture is there (else it is traced solid) */
static int rt_alpha(const Caster* c)
{
    /* the zone's and its placed objects' (leaves, grass); a character's (hair, a cape's fringe) is traced
     * solid, so the bounce light's and occlusion's rays (gfx_rt.hlsl trace_solid) still meet characters */
    return (c->fixed || c->keep) && alpha_tested(&c->lib.fs) && c->lib.vs.ntex >= 1 && c->tex[0] && c->tex[0]->srv >= 0 && c->tex[0]->type == GFX_TEX_2D;
}

/* a view in the heap at *slot (a new slot, the old one let go once the GPU is past it): a buffer of float4s
 * read (SRV, uav 0) or written (UAV: 1 structured, 2 raw) */
static int rt_view(int32_t* slot, ID3D12Resource* r, uint64_t size, int uav)
{
    defer(NULL, &g_srv, *slot);
    if ((*slot = heap_alloc(&g_srv)) < 0)
        return 0;
    if (!uav)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC v;
        memset(&v, 0, sizeof v);
        v.Format = DXGI_FORMAT_UNKNOWN, v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Buffer.NumElements = (UINT)(size / 16), v.Buffer.StructureByteStride = 16;
        ID3D12Device_CreateShaderResourceView(g_dev, r, &v, heap_cpu(&g_srv, *slot));
        return 1;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC v;
    memset(&v, 0, sizeof v);
    v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    if (uav == 2)
        v.Format = DXGI_FORMAT_R32_TYPELESS, v.Buffer.NumElements = (UINT)(size / 4), v.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    else
        v.Format = DXGI_FORMAT_UNKNOWN, v.Buffer.NumElements = (UINT)(size / 16), v.Buffer.StructureByteStride = 16;
    ID3D12Device_CreateUnorderedAccessView(g_dev, r, NULL, &v, heap_cpu(&g_srv, *slot));
    return 1;
}

/* The world's smooth normals for its verts corners (gfx_rt.hlsl rt_nclear, rt_nsum, rt_nresolve): the
 * table cleared, each triangle's face summed into the places of its corners, each corner given its place's */
static void rt_normals(ID3D12GraphicsCommandList* l, uint32_t verts)
{
    uint32_t slots = 1024;
    while (slots < verts * 2u && slots < (1u << 26))
        slots *= 2;
    int ng = rt_buffer(&g_ray.nrm, &g_ray.nrm_size, (uint64_t)verts * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 1);
    int tg = rt_buffer(&g_ray.table, &g_ray.table_size, (uint64_t)slots * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 1);
    if (ng < 0 || tg < 0)
        return;
    if (ng > 0)
        g_ray.nrm_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if ((ng > 0 || g_ray.nrm_srv < 0) && (!rt_view(&g_ray.nrm_srv, g_ray.nrm, g_ray.nrm_size, 0) ||
            !rt_view(&g_ray.nrm_uav, g_ray.nrm, g_ray.nrm_size, 1)))
        return;
    if ((tg > 0 || g_ray.table_uav < 0) && !rt_view(&g_ray.table_uav, g_ray.table, g_ray.table_size, 2))
        return;
    slots = (uint32_t)(g_ray.table_size / 16); /* (made larger than asked: all of it is used) */
    while (slots & (slots - 1))
        slots &= slots - 1;
    if (g_ray.nrm_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        barrier(g_ray.nrm, g_ray.nrm_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_ray.nrm_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    uint32_t k[8] = { (uint32_t)g_ray.so_srv, (uint32_t)g_ray.table_uav, (uint32_t)g_ray.nrm_uav, verts / 3, slots - 1, verts, 0, 0 };
    ID3D12GraphicsCommandList_SetComputeRootSignature(l, g_ray.croot);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(l, 0, 8, k, 0);
    ID3D12GraphicsCommandList_SetPipelineState(l, g_ray.nclear);
    ID3D12GraphicsCommandList_Dispatch(l, (slots + 255) / 256, 1, 1);
    rt_uav_barrier();
    ID3D12GraphicsCommandList_SetPipelineState(l, g_ray.nsum);
    ID3D12GraphicsCommandList_Dispatch(l, (verts / 3 + 255) / 256, 1, 1);
    rt_uav_barrier();
    ID3D12GraphicsCommandList_SetPipelineState(l, g_ray.nresolve);
    ID3D12GraphicsCommandList_Dispatch(l, (verts + 255) / 256, 1, 1);
    barrier(g_ray.nrm, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    g_ray.nrm_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    g_bound_pso = NULL; /* the game's next draw sets its own again */
}

static uint32_t rt_tris(const Caster* c)
{
    if (c->prim == GFX_TRIANGLELIST)
        return c->n / 3;
    if (c->prim == GFX_TRIANGLESTRIP)
        return c->n >= 3 ? c->n - 2 : 0;
    return 0;
}

static ID3D12PipelineState* rt_pipeline(const Caster* c)
{
    PipeKey pk;
    memset(&pk, 0, sizeof pk);
    pk.lib = c->lib;
    GfxVsKey* vk = &pk.lib.vs;
    vk->shadow = 2, vk->pixel = 0;
    /* the position alone, as the sun's depth maps draw it - and an alpha test's texture coordinates */
    int alpha = rt_alpha(c);
    vk->lighting = vk->normalize = vk->localviewer = vk->specular = 0;
    vk->src_diffuse = vk->src_specular = vk->src_ambient = vk->src_emissive = 0;
    vk->nlights = 0, memset(vk->light_type, 0, sizeof vk->light_type);
    vk->fog_vertex = vk->range_fog = 0, vk->flat = 0;
    if (!alpha)
        vk->ntex = 0, memset(vk->tci, 0, sizeof vk->tci), memset(vk->ttf, 0, sizeof vk->ttf);
    memset(&pk.lib.fs, 0, sizeof pk.lib.fs);
    pk.lib.fs.alpha_func = alpha ? c->lib.fs.alpha_func : 0; /* (build_pipeline: streamed out with them) */
    pk.color = DXGI_FORMAT_UNKNOWN, pk.dsv = DXGI_FORMAT_UNKNOWN;
    pk.cull = 1, pk.fill = 3;
    pk.topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    return pipeline_for(&pk, c->vs, c->ps);
}

/* The world for this frame's rays (invP the projection's inverse, view the camera): 1 when it was made */
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
            it->c = &g_casters[i], it->u = NULL;
            memcpy(it->m, invP, 64);
        }
        else
        {
            Cached* ce = &g_cache[i - g_ncasters];
            if (ce->dead || cached_expired(ce) || !ce->c.n)
            {
                g_ray.st_c_gone++;
                continue;
            }
            if (ce->seen == g_serial)
            {
                g_ray.st_c_seen++;
                continue;
            }
            if (ce->c.has_pos)
            {
                float dx = ce->pos[0] - cam[0], dy = ce->pos[1] - cam[1], dz = ce->pos[2] - cam[2];
                if (dx * dx + dy * dy + dz * dz > RT_REACH * RT_REACH)
                {
                    g_ray.st_c_far++;
                    continue;
                }
            }
            else
                g_ray.st_c_none++;
            it->c = &ce->c, it->u = &ce->u;
            mat_mul(it->m, ce->clip_world, view);
        }
        if (!it->c->n || !(it->tris = rt_tris(it->c)))
            continue;
        it->alpha = (uint8_t)rt_alpha(it->c);
        if (!rt_pipeline(it->c))
        {
            g_ray.st_skipped++;
            continue;
        }
        verts += (uint64_t)it->tris * 3;
        n++;
    }
    if (!n || verts > (1u << 26))
        return 0;
    /* the solid first, then the alpha-tested: the structure's two geometries */
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
    /* the buffers: the triangles (and their count, which stream output keeps), the structures */
    int grown = rt_buffer(&g_ray.so, &g_ray.so_size, verts * 32, D3D12_RESOURCE_STATE_STREAM_OUT, 0);
    if (grown < 0 || rt_buffer(&g_ray.count, &g_ray.count_size, 16, D3D12_RESOURCE_STATE_COPY_DEST, 0) < 0)
        return 0;
    if (grown > 0)
        g_ray.so_state = D3D12_RESOURCE_STATE_STREAM_OUT;
    if (!g_ray.count_state)
        g_ray.count_state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (grown > 0 || g_ray.so_srv < 0)
    {
        defer(NULL, &g_srv, g_ray.so_srv);
        if ((g_ray.so_srv = heap_alloc(&g_srv)) < 0)
            return 0;
        D3D12_SHADER_RESOURCE_VIEW_DESC v;
        memset(&v, 0, sizeof v);
        v.Format = DXGI_FORMAT_UNKNOWN, v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Buffer.NumElements = (UINT)(g_ray.so_size / 16), v.Buffer.StructureByteStride = 16;
        ID3D12Device_CreateShaderResourceView(g_dev, g_ray.so, &v, heap_cpu(&g_srv, g_ray.so_srv));
    }
    ID3D12GraphicsCommandList* l = list();
    /* the count from 0 */
    Alloc z = ring(16, 16);
    if (!z.cpu)
        return 0;
    memset(z.cpu, 0, 16);
    if (g_ray.count_state != D3D12_RESOURCE_STATE_COPY_DEST)
        barrier(g_ray.count, g_ray.count_state, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(l, g_ray.count, 0, z.res, z.off, 4);
    barrier(g_ray.count, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    g_ray.count_state = D3D12_RESOURCE_STATE_STREAM_OUT;
    if (g_ray.so_state != D3D12_RESOURCE_STATE_STREAM_OUT)
        barrier(g_ray.so, g_ray.so_state, D3D12_RESOURCE_STATE_STREAM_OUT), g_ray.so_state = D3D12_RESOURCE_STATE_STREAM_OUT;
    D3D12_STREAM_OUTPUT_BUFFER_VIEW sv = { ID3D12Resource_GetGPUVirtualAddress(g_ray.so), g_ray.so_size,
        ID3D12Resource_GetGPUVirtualAddress(g_ray.count) };
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 0, NULL, FALSE, NULL);
    D3D12_VIEWPORT vp = { 0, 0, 1, 1, 0, 1 };
    D3D12_RECT sc = { 0, 0, 1, 1 };
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_SOSetTargets(l, 0, 1, &sv);
    D3D12_GPU_VIRTUAL_ADDRESS dummy = ID3D12Resource_GetGPUVirtualAddress(g_dummy), mlive = 0;
    uint32_t done = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        const RtItem* it = &items[i];
        const Caster* cs = it->c;
        D3D12_GPU_VIRTUAL_ADDRESS m = it->u ? ring_bytes(it->m, 64) : mlive ? mlive : (mlive = ring_bytes(it->m, 64));
        D3D12_GPU_VIRTUAL_ADDRESS ugpu = it->u ? ring_bytes(it->u, sizeof *it->u) : cs->ugpu;
        if (!m || !ugpu)
            break; /* (the ring is full: the rest would not match the count) */
        set_pso(rt_pipeline(cs));
        set_topology(topology(cs->prim));
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_SHADOW, m);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, ugpu);
        for (int st = 0; st < GFX_NSTREAMS; ++st)
        {
            if (cs->vb[st])
                buf_state(cs->vb[st], BUF_READ);
            ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + st, cs->va[st] ? cs->va[st] : dummy);
        }
        if (cs->itype)
        {
            if (cs->ib)
                buf_state(cs->ib, BUF_READ);
            D3D12_INDEX_BUFFER_VIEW ibv = { cs->iva, cs->n * cs->itype, cs->itype == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
            ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ibv);
            ID3D12GraphicsCommandList_DrawIndexedInstanced(l, cs->n, 1, 0, 0, 0);
        }
        else
            ID3D12GraphicsCommandList_DrawInstanced(l, cs->n, 1, cs->vstart, 0);
        done++;
        if (it->u)
            g_ray.st_cached++;
        else
            g_ray.st_live++;
    }
    D3D12_STREAM_OUTPUT_BUFFER_VIEW none;
    memset(&none, 0, sizeof none);
    ID3D12GraphicsCommandList_SOSetTargets(l, 0, 1, &none);
    g_targets_bound = 0;
    unbind();
    uint64_t vsolid = 0, valpha = 0;
    uint32_t nalpha = 0;
    for (uint32_t i = 0; i < done; ++i)
        *(items[i].alpha ? &valpha : &vsolid) += (uint64_t)items[i].tris * 3, nalpha += items[i].alpha;
    verts = vsolid + valpha;
    if (!verts)
        return 0;
    /* the alpha tests' table: each one's first triangle, its texture, its sampler, its test */
    g_ray.nsolid = (uint32_t)(vsolid / 3), g_ray.nalpha = nalpha;
    if (nalpha)
    {
        Alloc t = ring((size_t)nalpha * 16, 16);
        int tg = rt_buffer(&g_ray.tab, &g_ray.tab_size, (uint64_t)nalpha * 16, D3D12_RESOURCE_STATE_COPY_DEST, 0);
        if (!t.cpu || tg < 0)
            return 0;
        if (tg > 0)
            g_ray.tab_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if ((tg > 0 || g_ray.tab_srv < 0) && !rt_view(&g_ray.tab_srv, g_ray.tab, g_ray.tab_size, 0))
            return 0;
        uint32_t* e = (uint32_t*)t.cpu, first = (uint32_t)(vsolid / 3);
        for (uint32_t i = 0; i < done; ++i)
        {
            const RtItem* it = &items[i];
            if (!it->alpha)
                continue;
            const GfxU* gu = it->u ? it->u : it->c->uidx >= 0 ? &g_caster_u[it->c->uidx] : NULL;
            float ref = gu ? gu->params[1] : 128.0f;
            GfxTex* tx = it->c->tex[0];
            tex_state(tx, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            e[0] = first, e[1] = (uint32_t)tx->srv, e[2] = sampler_slot(&it->c->samp[0]);
            e[3] = (uint32_t)it->c->lib.fs.alpha_func << 8 | (uint32_t)fminf(fmaxf(ref, 0.0f), 255.0f);
            e += 4, first += it->tris;
        }
        if (g_ray.tab_state != D3D12_RESOURCE_STATE_COPY_DEST)
            barrier(g_ray.tab, g_ray.tab_state, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyBufferRegion(l, g_ray.tab, 0, t.res, t.off, (UINT64)nalpha * 16);
        barrier(g_ray.tab, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        g_ray.tab_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }
    barrier(g_ray.so, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_ray.so_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    /* a structure over the solid triangles and one over the alpha-tested ones, then the one over both: two
     * instances, masks 1 and 2 - the bounce light's and occlusion's rays (mask 1) skip the leaves and grass
     * whole, where culling their triangles alone still walked their boxes (Lufaise's grass: 9 ms a frame) */
    D3D12_RAYTRACING_GEOMETRY_DESC g[2];
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bl[2], tl;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bp[2], tp;
    ID3D12Resource** blas[2] = { &g_ray.blas, &g_ray.blas2 };
    uint64_t* blas_size[2] = { &g_ray.blas_size, &g_ray.blas2_size };
    memset(g, 0, sizeof g), memset(bl, 0, sizeof bl), memset(bp, 0, sizeof bp), memset(&tl, 0, sizeof tl);
    uint64_t scratch = 0;
    UINT ninst = 0;
    for (int k = 0; k < 2; ++k)
    {
        uint64_t first = k ? vsolid : 0, count = k ? valpha : vsolid;
        if (!count)
            continue;
        g[k].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g[k].Flags = k ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g[k].Triangles.VertexBuffer.StartAddress = ID3D12Resource_GetGPUVirtualAddress(g_ray.so) + first * 32;
        g[k].Triangles.VertexBuffer.StrideInBytes = 32;
        g[k].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT, g[k].Triangles.VertexCount = (UINT)count;
        bl[k].Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bl[k].Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        bl[k].NumDescs = 1, bl[k].DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY, bl[k].pGeometryDescs = &g[k];
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(g_ray.dev, &bl[k], &bp[k]);
        if (bp[k].ScratchDataSizeInBytes > scratch)
            scratch = bp[k].ScratchDataSizeInBytes;
        if (rt_buffer(blas[k], blas_size[k], bp[k].ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 1) < 0)
            return 0;
        ninst++;
    }
    Alloc inst = ring(2 * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
    if (!inst.cpu)
        return 0;
    tl.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tl.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tl.NumDescs = ninst, tl.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY, tl.InstanceDescs = inst.gpu;
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(g_ray.dev, &tl, &tp);
    if (tp.ScratchDataSizeInBytes > scratch)
        scratch = tp.ScratchDataSizeInBytes;
    if (rt_buffer(&g_ray.scratch, &g_ray.scratch_size, scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 1) < 0)
        return 0;
    int tgrown = rt_buffer(&g_ray.tlas, &g_ray.tlas_size, tp.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 1);
    if (tgrown < 0)
        return 0;
    if (tgrown > 0 || g_ray.tlas_srv < 0)
    {
        defer(NULL, &g_srv, g_ray.tlas_srv);
        if ((g_ray.tlas_srv = heap_alloc(&g_srv)) < 0)
            return 0;
        D3D12_SHADER_RESOURCE_VIEW_DESC v;
        memset(&v, 0, sizeof v);
        v.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.RaytracingAccelerationStructure.Location = ID3D12Resource_GetGPUVirtualAddress(g_ray.tlas);
        ID3D12Device_CreateShaderResourceView(g_dev, NULL, &v, heap_cpu(&g_srv, g_ray.tlas_srv));
    }
    /* each instance's InstanceID: 0 the solid, 1 the alpha-tested (gfx_rt.hlsl: their triangles' offset) */
    D3D12_RAYTRACING_INSTANCE_DESC* id = (D3D12_RAYTRACING_INSTANCE_DESC*)inst.cpu;
    memset(id, 0, 2 * sizeof *id);
    UINT ni = 0;
    for (int k = 0; k < 2; ++k)
        if (bl[k].NumDescs)
        {
            id[ni].Transform[0][0] = id[ni].Transform[1][1] = id[ni].Transform[2][2] = 1.0f;
            id[ni].InstanceID = (UINT)k, id[ni].InstanceMask = k ? 2 : 1;
            id[ni].AccelerationStructure = ID3D12Resource_GetGPUVirtualAddress(*blas[k]);
            ni++;
        }
    ID3D12GraphicsCommandList4* l4 = NULL;
    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(l, &IID_ID3D12GraphicsCommandList4, (void**)&l4)))
        return 0;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(g_ray.scratch);
    for (int k = 0; k < 2; ++k)
        if (bl[k].NumDescs)
        {
            bd.DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(*blas[k]);
            bd.Inputs = bl[k];
            ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(l4, &bd, 0, NULL);
            rt_uav_barrier(); /* (the scratch is shared) */
        }
    bd.DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(g_ray.tlas);
    bd.Inputs = tl;
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(l4, &bd, 0, NULL);
    rt_uav_barrier();
    ID3D12GraphicsCommandList4_Release(l4);
    rt_normals(l, (uint32_t)verts);
    barrier(g_ray.so, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    g_ray.so_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    g_ray.verts = (uint32_t)verts, g_ray.serial = g_serial;
    g_ray.st_tris += verts / 3, g_ray.st_frames++;
    return 1;
}

/* The finished scene anti-aliased (g_fxs.aa: FXAA), within its viewport, before the interface goes on */
static void scene_aa(GfxTex* color, const GfxScene* s)
{
    if (g_fxs.aa < 0.5f || color->dxfmt != FX_COLOR || !fx_init())
        return;
    float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > color->w || vy + vh > color->h)
        vx = vy = 0, vw = (float)color->w, vh = (float)color->h;
    if (!fx_tex(&g_fx.aa_src, color->dxfmt, color->w, color->h, 1))
        return;
    FxU u;
    memset(&u, 0, sizeof u);
    u.size[0] = (float)color->w, u.size[1] = (float)color->h;
    D3D12_GPU_VIRTUAL_ADDRESS ua = fx_uniforms(&u);
    if (!ua)
        return;
    fx_copy(color, &g_fx.aa_src);
    uint32_t in = fx_in(&g_fx.aa_src, 0);
    tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
    fx_pass(target_view(color, 0, 0), g_fx.aa, vx, vy, vw, vh, ua, &in, 1, 0, 0);
    color->scene = 0; /* its mips are behind (scene_mips) */
}

static void scene_fx(GfxTex* color, const GfxScene* s)
{
    GfxTex* depth = color->depth_world ? color->depth_world : color->depth_seen;
    if (!depth || depth->srv < 0 || depth->w != color->w || depth->h != color->h || color->dxfmt != FX_COLOR)
    {
        g_fx_no_depth++;
        return;
    }
    if (s->proj[11] == 0.0f)
    {
        g_fx_no_proj++;
        return;
    }
    if (!fx_init())
    {
        g_fx_not_ready++;
        return;
    }
    g_fx_ran++;
    /* the scene's viewport, within the target */
    float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > color->w || vy + vh > color->h)
        vx = vy = 0, vw = (float)color->w, vh = (float)color->h;
    float minz, maxz;
    memcpy(&minz, &s->vp[4], 4);
    memcpy(&maxz, &s->vp[5], 4);
    if (maxz <= minz)
        minz = 0, maxz = 1;
    /* the occlusion at 2048 pixels across or fewer (halved until it is: the background's 4096, 6144,
     * 8192), bloom and rays (soft anyway) at about 1000 */
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
        { (float)color->w, (float)color->h, (float)aw, (float)ah },
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
    int have_inv = mat_inverse(inv, s->view);
    if (have_inv)
    {
        /* a new place (the camera jumped, or a gap: logging in, a zone change, a moghouse): what is
         * eased and the sun held take this one's at once - eased from the last place's, the sun's
         * strength outdoors shaded a room from its ceiling for the seconds it took to fade */
        float dx = inv[12] - g_fx.last_cam[0], dy = inv[13] - g_fx.last_cam[1], dz = inv[14] - g_fx.last_cam[2];
        if (gap || dx * dx + dy * dy + dz * dz > 50.0f * 50.0f)
            g_fx.eased = 0, g_fx.fogc_set = 0, g_fx.sunw_seen = 0;
        memcpy(g_fx.last_cam, inv + 12, 12);
    }
    if (have_inv)
    {
        float sign = inv[5] < 0.0f ? -1.0f : 1.0f;
        up[0] = inv[1] * sign, up[1] = inv[5] * sign, up[2] = inv[9] * sign;
        normalize3(up);
        /* fog where the game fogs its world, fading in and out; its colour eased over about a second */
        float on = s->fog[2] != 0.0f ? 1.0f : 0.0f;
        fx_ease(&g_fx.fog_on, &on, 1, 0.1f);
        if (g_fxs.fog <= 0.0f)
            g_fx.fogc_set = 0; /* no fog: the next one takes its colour at once */
        else if (!g_fx.fogc_set)
            memcpy(g_fx.fogc, s->fogcolor, 12), g_fx.fogc_set = 1;
        else
            for (int j = 0; j < 3; ++j)
                g_fx.fogc[j] += (s->fogcolor[j] - g_fx.fogc[j]) * 0.04f;
        fx_ease(g_fx.up, up, 3, 0.2f);
        normalize3(g_fx.up);
        memcpy(u.up, g_fx.up, 12);
        memcpy(u.fogc, g_fx.fogc, 12);
        u.fogc[3] = g_fxs.fog * g_fx.fog_on * expf(-g_fxs.fog_falloff * g_fxs.fog_height);
        u.fogp[0] = g_fxs.fog_falloff, u.fogp[1] = g_fxs.fog_max, u.fogp[2] = g_fxs.fog_sun;
        u.fogp[3] = fminf(fmaxf(g_fxs.fog_g, 0.0f), 0.95f);
    }
    /* the sun, kept in the world: a frame gives one only when a lit draw is in view, and the shadows
     * hold the last one rather than come and go with what the camera sees */
    float vinv[16];
    int have_v = mat_inverse(vinv, s->view), own = s->sun_dir[3] != 0.0f && have_v;
    if (own)
    {
        float w[3];
        for (int j = 0; j < 3; ++j)
            w[j] = s->sun_dir[0] * vinv[j] + s->sun_dir[1] * vinv[4 + j] + s->sun_dir[2] * vinv[8 + j];
        normalize3(w);
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
            normalize3(g_fx.sunw);
        }
        g_fx.sunw_seen = g_serial;
        fx_ease(g_fx.suncol, s->sun_color, 3, 0.1f);
    }
    int shadows = g_fxs.sun > 0.0f || g_fxs.shadow > 0.0f;
    if (have_v && (own || (shadows && g_fx.sunw_seen && g_fx.sunw_seen + 600 > g_serial)))
    {
        for (int j = 0; j < 3; ++j) /* back into this frame's view */
            g_fx.sun[j] = g_fx.sunw[0] * s->view[j] + g_fx.sunw[1] * s->view[4 + j] + g_fx.sunw[2] * s->view[8 + j];
        normalize3(g_fx.sun);
        memcpy(u.sun, g_fx.sun, 12);
        u.sun[3] = 1.0f;
        memcpy(u.suncol, g_fx.suncol, 12);
        /* shadows while the sun (or moon) is up: fading as it nears the horizon - or, with sun_dusk,
         * kept long at dawn and dusk and fading only in its last three degrees */
        float e = g_fx.sun[0] * g_fx.up[0] + g_fx.sun[1] * g_fx.up[1] + g_fx.sun[2] * g_fx.up[2];
        float day = g_fxs.sun_dusk != 0.0f ? fminf(fmaxf(e / 0.05f, 0.0f), 1.0f) : fminf(fmaxf((e - 0.05f) / 0.15f, 0.0f), 1.0f);
        if (own)
        {
            float dl = 0.2126f * s->sun_color[0] + 0.7152f * s->sun_color[1] + 0.0722f * s->sun_color[2];
            float al = 0.2126f * s->ambient[0] + 0.7152f * s->ambient[1] + 0.0722f * s->ambient[2];
            float r = dl / fmaxf(dl + al, 1e-3f);
            float want = fminf(fmaxf((r - g_fxs.sun_direct) / 0.3f, 0.0f), 1.0f);
            g_fx.direct = g_fx.eased && g_fx.direct >= 0.0f ? g_fx.direct + (want - g_fx.direct) * 0.05f : want;
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
        int cam = g_fx.prev_serial && g_fx.prev_serial + 1 == g_serial && mat_inverse(vinv2, s->view);
        if (cam)
        {
            float dx = vinv2[12] - g_fx.prev_cam[0], dy = vinv2[13] - g_fx.prev_cam[1], dz = vinv2[14] - g_fx.prev_cam[2];
            cam = dx * dx + dy * dy + dz * dz < 25.0f;
        }
        if (cam)
        {
            mat_mul(m, vinv2, g_fx.prev_view);
            mat_mul(u.reproj, m, g_fx.prev_proj);
            u.hist[0] = g_fx.hist_serial && g_fx.hist_serial + 1 == g_serial && g_fx.hist[0].res && g_fx.hist[0].w == aw &&
                g_fx.hist[0].h == ah ? 1.0f : 0.0f;
            /* the bounce light's too, at its own size */
            u.gip[0] = g_fx.gih_serial && g_fx.gih_serial + 1 == g_serial && g_fx.gih[0].res && g_fx.gih[0].w == (aw + 1) / 2 &&
                g_fx.gih[0].h == (ah + 1) / 2 ? 1.0f : 0.0f;
        }
        g_fx.prev_serial = g_serial;
        u.hist[1] = (float)fmod((double)g_serial * 0.6180339887, 1.0); /* the pattern's turn: a golden-ratio step */
        u.hist[2] = fminf(fmaxf(g_fxs.temporal, 0.0f), 0.95f);
        memcpy(g_fx.prev_view, s->view, 64), memcpy(g_fx.prev_proj, s->proj, 64);
        if (mat_inverse(vinv2, s->view))
            memcpy(g_fx.prev_cam, vinv2 + 12, 12);
    }
    u.bloom[0] = g_fxs.threshold, u.bloom[1] = g_fxs.bloom, u.bloom[2] = 0.25f;
    u.rays[0] = u.sunuv[2] > 0.0f ? g_fxs.rays : 0.0f, u.rays[1] = g_fxs.rays_decay, u.rays[2] = g_fxs.rays_length;
    uint32_t b2w = (bw + 1) / 2, b2h = (bh + 1) / 2;
    if (!fx_tex(&g_fx.src, color->dxfmt, color->w, color->h, 1) || !fx_tex(&g_fx.ao0, FX_HALF, aw, ah, 1) ||
        !fx_tex(&g_fx.ao1, FX_HALF, aw, ah, 1) || !fx_tex(&g_fx.b1a, FX_HALF, bw, bh, 1) || !fx_tex(&g_fx.b1b, FX_HALF, bw, bh, 1) ||
        !fx_tex(&g_fx.b2a, FX_HALF, b2w, b2h, 1) || !fx_tex(&g_fx.b2b, FX_HALF, b2w, b2h, 1) || !fx_tex(&g_fx.ra, FX_HALF, bw, bh, 1) ||
        !fx_tex(&g_fx.rb, FX_HALF, bw, bh, 1))
        return;
    int occlusion = u.ao[1] > 0.0f || u.shadow[0] > 0.0f || u.smap[0] > 0.0f;
    int lz = occlusion && fx_tex(&g_fx.lz, FX_Z, aw, ah, 4);
    if (occlusion && !lz)
        u.ao[1] = 0.0f;
    int temporal = occlusion && g_fxs.temporal > 0.0f && fx_tex(&g_fx.hist[0], FX_HALF, aw, ah, 1) && fx_tex(&g_fx.hist[1], FX_HALF, aw, ah, 1);
    /* the world for rays, before the passes that trace them */
    int rt = 0;
    if ((g_fxs.rt > 0.0f || (int)g_fxs.debug == 8) && have_v)
    {
        float invP[16];
        rt = mat_inverse(invP, s->proj) && rt_capture(invP, s->view, vinv + 12);
        if (rt)
            u.rtp[0] = g_ray.nsolid, u.rtp[1] = (uint32_t)(g_ray.tab_srv >= 0 ? g_ray.tab_srv : 0), u.rtp[2] = (uint32_t)g_ray.so_srv,
            u.rtp[3] = g_ray.nalpha;
    }
    uint32_t gw = (aw + 1) / 2, gh = (ah + 1) / 2;
    int gi = u.gi[0] > 0.0f && fx_tex(&g_fx.gi0, FX_HALF, gw, gh, 1) && fx_tex(&g_fx.gih[0], FX_HALF, gw, gh, 1) &&
        fx_tex(&g_fx.gih[1], FX_HALF, gw, gh, 1) && (!rt || fx_tex(&g_fx.gi1, FX_HALF, gw, gh, 1));
    if (!gi)
        u.gi[0] = 0.0f;
    u.gip[1] = fminf(fmaxf(g_fxs.temporal, 0.0f), 0.95f), u.gip[2] = 12.0f; /* the history's share; the gather's samples */
    if (rt) /* traced: rays reach further, two a texel, kept longer over frames (their noise averaged away) */
        u.gi[2] = fmaxf(g_fxs.gi_radius * 2.5f, 4.0f), u.gip[2] = 2.0f, u.gip[1] = u.gip[1] > 0.0f ? fmaxf(u.gip[1], 0.95f) : 0.0f;
    D3D12_GPU_VIRTUAL_ADDRESS ua = fx_uniforms(&u);
    if (!ua)
        return;
    fx_copy(color, &g_fx.src);
    float fw = (float)aw, fh = (float)ah, fbw = (float)bw, fbh = (float)bh;
    uint32_t in[7];
    FxTex* ao_out = NULL;
    if (occlusion)
    {
        if (lz)
        {
            in[0] = fx_depth(depth);
            fx_pass(fx_out(&g_fx.lz, 0), g_fx.linz, 0, 0, fw, fh, ua, in, 1, 0, 0);
            for (int l = 1; l < 4; ++l)
            {
                in[0] = fx_in(&g_fx.lz, l - 1);
                uint32_t lw = aw >> l ? aw >> l : 1, lh = ah >> l ? ah >> l : 1;
                fx_pass(fx_out(&g_fx.lz, l), g_fx.zmip, 0, 0, (float)lw, (float)lh, ua, in, 1, 0, 0);
            }
        }
        in[0] = fx_depth(depth), in[1] = SRV_NULL_2D, in[2] = SRV_NULL_2D, in[3] = lz ? fx_in(&g_fx.lz, -1) : SRV_NULL_2D;
        if (u.smap[0] > 0.0f)
        {
            sun_state(&g_fx.smap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            in[1] = (uint32_t)g_fx.smap.srv;
            if (u.smapn2[1] > 0.0f)
            {
                sun_state(&g_fx.smapn, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                in[2] = (uint32_t)g_fx.smapn.srv;
            }
        }
        fx_pass(fx_out(&g_fx.ao0, 0), g_fx.ao, 0, 0, fw, fh, ua, in, 4, 0, 0);
        /* traced (rt): the occlusion from rays in the world, the sun's as fx_ao found it; ao1 then holds it */
        FxTex *a = &g_fx.ao0, *b = &g_fx.ao1;
        if (rt && u.ao[1] > 0.0f)
        {
            in[0] = fx_in(&g_fx.ao0, 0), in[1] = fx_depth(depth), in[2] = (uint32_t)g_ray.tlas_srv;
            fx_pass(fx_out(&g_fx.ao1, 0), g_ray.ao, 0, 0, fw, fh, ua, in, 3, 0, 0);
            a = &g_fx.ao1, b = &g_fx.ao0;
        }
        in[0] = fx_in(a, 0);
        fx_pass(fx_out(b, 0), g_fx.blur, 0, 0, fw, fh, ua, in, 1, 1, 0);
        in[0] = fx_in(b, 0);
        fx_pass(fx_out(a, 0), g_fx.blur, 0, 0, fw, fh, ua, in, 1, 0, 1);
        ao_out = a;
        if (temporal)
        {
            int to = g_fx.hist_at ^ 1;
            in[0] = fx_in(a, 0), in[1] = fx_in(&g_fx.hist[g_fx.hist_at], 0);
            fx_pass(fx_out(&g_fx.hist[to], 0), g_fx.temporal, 0, 0, fw, fh, ua, in, 2, 0, 0);
            ao_out = &g_fx.hist[to];
            g_fx.hist_at = to, g_fx.hist_serial = g_serial;
        }
    }
    /* the bounce light: gathered from its map at half the occlusion's size, then over frames */
    FxTex* gi_out = NULL;
    if (gi)
    {
        sun_state(&g_fx.gimap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (rt)
        {
            /* traced (rt): rays from each texel into the world, the light they meet as the frame or the
             * sun saw it; smoothed twice (one texel apart, then two) before the temporal pass */
            in[0] = fx_depth(depth), in[1] = (uint32_t)g_ray.tlas_srv, in[2] = (uint32_t)g_ray.so_srv, in[3] = fx_in(&g_fx.src, 0);
            in[4] = ao_out ? fx_in(ao_out, 0) : SRV_NULL_2D, in[5] = (uint32_t)g_fx.gimap.srv, in[6] = fx_in(&g_fx.gicol, -1);
            fx_pass(fx_out(&g_fx.gi0, 0), g_ray.gi, 0, 0, (float)gw, (float)gh, ua, in, 7, 0, 0);
            in[0] = fx_in(&g_fx.gi0, 0);
            fx_pass(fx_out(&g_fx.gi1, 0), g_ray.giblur, 0, 0, (float)gw, (float)gh, ua, in, 1, 1, 1);
            in[0] = fx_in(&g_fx.gi1, 0);
            fx_pass(fx_out(&g_fx.gi0, 0), g_ray.giblur, 0, 0, (float)gw, (float)gh, ua, in, 1, 2, 2);
            in[0] = fx_in(&g_fx.gi0, 0);
            fx_pass(fx_out(&g_fx.gi1, 0), g_ray.giblur, 0, 0, (float)gw, (float)gh, ua, in, 1, 4, 4);
            in[0] = fx_in(&g_fx.gi1, 0);
            fx_pass(fx_out(&g_fx.gi0, 0), g_ray.giblur, 0, 0, (float)gw, (float)gh, ua, in, 1, 1, 1);
        }
        else
        {
            in[0] = fx_depth(depth), in[1] = (uint32_t)g_fx.gimap.srv, in[2] = fx_in(&g_fx.gicol, -1);
            fx_pass(fx_out(&g_fx.gi0, 0), g_fx.gi, 0, 0, (float)gw, (float)gh, ua, in, 3, 0, 0);
        }
        gi_out = &g_fx.gi0;
        if (u.gip[1] > 0.0f)
        {
            int to = g_fx.gih_at ^ 1;
            in[0] = fx_in(&g_fx.gi0, 0), in[1] = fx_in(&g_fx.gih[g_fx.gih_at], 0);
            fx_pass(fx_out(&g_fx.gih[to], 0), g_fx.gitemp, 0, 0, (float)gw, (float)gh, ua, in, 2, 0, 0);
            gi_out = &g_fx.gih[to];
            g_fx.gih_at = to, g_fx.gih_serial = g_serial;
        }
    }
    if (u.bloom[1] > 0.0f)
    {
        float f2w = (float)b2w, f2h = (float)b2h;
        in[0] = fx_in(&g_fx.src, 0), in[1] = ao_out ? fx_in(ao_out, 0) : SRV_NULL_2D; /* shaded as fx_comp shades */
        fx_pass(fx_out(&g_fx.b1a, 0), g_fx.bright, 0, 0, fbw, fbh, ua, in, 2, 0, 0);
        in[0] = fx_in(&g_fx.b1a, 0);
        fx_pass(fx_out(&g_fx.b1b, 0), g_fx.gauss, 0, 0, fbw, fbh, ua, in, 1, 2, 0);
        in[0] = fx_in(&g_fx.b1b, 0);
        fx_pass(fx_out(&g_fx.b1a, 0), g_fx.gauss, 0, 0, fbw, fbh, ua, in, 1, 0, 2);
        in[0] = fx_in(&g_fx.b1a, 0);
        fx_pass(fx_out(&g_fx.b2a, 0), g_fx.down, 0, 0, f2w, f2h, ua, in, 1, 0, 0);
        in[0] = fx_in(&g_fx.b2a, 0);
        fx_pass(fx_out(&g_fx.b2b, 0), g_fx.gauss, 0, 0, f2w, f2h, ua, in, 1, 2, 0);
        in[0] = fx_in(&g_fx.b2b, 0);
        fx_pass(fx_out(&g_fx.b2a, 0), g_fx.gauss, 0, 0, f2w, f2h, ua, in, 1, 0, 2);
    }
    if (u.rays[0] > 0.0f)
    {
        in[0] = fx_in(&g_fx.src, 0), in[1] = fx_depth(depth);
        fx_pass(fx_out(&g_fx.ra, 0), g_fx.raymask, 0, 0, fbw, fbh, ua, in, 2, 0, 0);
        in[0] = fx_in(&g_fx.ra, 0);
        fx_pass(fx_out(&g_fx.rb, 0), g_fx.rays, 0, 0, fbw, fbh, ua, in, 1, 0, 0);
        in[0] = fx_in(&g_fx.rb, 0);
        fx_pass(fx_out(&g_fx.ra, 0), g_fx.gauss, 0, 0, fbw, fbh, ua, in, 1, 1, 0);
        in[0] = fx_in(&g_fx.ra, 0);
        fx_pass(fx_out(&g_fx.rb, 0), g_fx.gauss, 0, 0, fbw, fbh, ua, in, 1, 0, 1);
    }
    in[0] = fx_in(&g_fx.src, 0);
    in[1] = ao_out ? fx_in(ao_out, 0) : SRV_NULL_2D;
    in[2] = fx_depth(depth);
    in[3] = u.bloom[1] > 0.0f ? fx_in(&g_fx.b1a, 0) : SRV_NULL_2D;
    in[4] = u.bloom[1] > 0.0f ? fx_in(&g_fx.b2a, 0) : SRV_NULL_2D;
    in[5] = u.rays[0] > 0.0f ? fx_in(&g_fx.rb, 0) : SRV_NULL_2D;
    in[6] = gi_out ? fx_in(gi_out, 0) : SRV_NULL_2D;
    tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
    fx_pass(target_view(color, 0, 0), g_fx.comp, vx, vy, vw, vh, ua, in, 7, 0, 0);
    if (rt && (int)g_fxs.debug == 8)
    {
        in[0] = fx_depth(depth), in[1] = (uint32_t)g_ray.tlas_srv, in[2] = (uint32_t)g_ray.so_srv, in[3] = (uint32_t)g_ray.nrm_srv;
        tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
        fx_pass(target_view(color, 0, 0), g_ray.clay, vx, vy, vw, vh, ua, in, 4, 0, 0);
    }
    color->scene = 0; /* the effects changed it: its mips are behind (scene_mips) */
}

void gfx_scene_done(GfxTex* color, const GfxScene* s)
{
    if (!g_dev || !color || color->type != GFX_TEX_2D || color->use != GFX_USE_RT)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    if (g_fxs.fx != 0.0f)
    {
        scene_fx(color, s);
        color->scene = 0; /* a new scene, effects or none: its mips are behind (scene_mips) */
        if (gfx_profiling)
        {
            static uint64_t last;
            g_fx.st_frames++;
            g_fx.st_own += s->sun_dir[3] != 0.0f;
            g_fx.st_map += g_fx.st_drawn_this;
            g_fx.st_cmin = g_fx.st_frames == 1 || g_ncasters < g_fx.st_cmin ? g_ncasters : g_fx.st_cmin;
            g_fx.st_cmax = g_ncasters > g_fx.st_cmax ? g_ncasters : g_fx.st_cmax;
            uint64_t now = gfx_now_ns();
            if (now - last > 2000000000ull)
            {
                fprintf(stderr, "[recomp] gfx: shadows: %u frames, %u with the sun's own light, %u with a map; casters %u..%u; "
                    "%u cached; drawn %u live, %u from the cache, %u waiting for pipelines, %u left out (beyond a map); "
                    "map %.0f units across; the player %.1f units from the camera\n",
                    g_fx.st_frames, g_fx.st_own, g_fx.st_map, g_fx.st_cmin, g_fx.st_cmax, g_fx.st_cached, g_fx.st_live,
                    g_fx.st_replayed, g_fx.st_skipped, g_fx.st_beyond, g_fx.st_across, g_fx.st_focus);
                last = now;
                g_fx.st_frames = g_fx.st_own = g_fx.st_map = g_fx.st_cmax = g_fx.st_live = g_fx.st_replayed = g_fx.st_skipped = 0;
                g_fx.st_beyond = 0;
                if (g_ray.st_frames)
                    fprintf(stderr, "[recomp] gfx: ray tracing: %u frames; casters %u live, %u from the cache, %u waiting for "
                        "pipelines; %llu triangles a frame; the cache's left out: %u gone, %u drawn live, %u too far, %u with no place\n",
                        g_ray.st_frames, g_ray.st_live, g_ray.st_cached, g_ray.st_skipped, (unsigned long long)(g_ray.st_tris / g_ray.st_frames),
                        g_ray.st_c_gone, g_ray.st_c_seen, g_ray.st_c_far, g_ray.st_c_none);
                g_ray.st_frames = g_ray.st_live = g_ray.st_cached = g_ray.st_skipped = 0, g_ray.st_tris = 0;
                g_ray.st_c_gone = g_ray.st_c_seen = g_ray.st_c_far = g_ray.st_c_none = 0;
            }
        }
        g_fx.st_drawn_this = 0;
    }
    casters_clear();
    scene_aa(color, s);
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0;
}

void gfx_present(GfxTex* bb)
{
    if (!g_dev)
        return;
    uint64_t present_start = gfx_profiling ? gfx_now_ns() : 0;
    g_present_thread = GetCurrentThreadId();
    int presented = 0;
    if (g_swap && bb && bb->srv >= 0 && g_present_pso)
    {
        swap_fit();
        UINT i = IDXGISwapChain3_GetCurrentBackBufferIndex(g_swap);
        ID3D12GraphicsCommandList* l = list();
        tex_state(bb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(g_swap_buf[i], g_swap_state[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_cpu(&g_rtv, g_swap_rtv[i]);
        ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
        D3D12_VIEWPORT v = { 0, 0, (float)g_swap_w, (float)g_swap_h, 0, 1 };
        D3D12_RECT sc = { 0, 0, (LONG)g_swap_w, (LONG)g_swap_h };
        ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
        ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
        uint32_t bind[16] = { 0 };
        float sharpen = g_fxs.fx != 0.0f ? g_fxs.sharpen : 0.0f;
        bind[0] = (uint32_t)bb->srv, bind[8] = g_present_samp;
        memcpy(&bind[15], &sharpen, 4); /* present_cas_fs's si[1].w */
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
        set_pso(sharpen > 0.0f && g_present_cas_pso ? g_present_cas_pso : g_present_pso);
        set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
        if (g_fxs.fps != 0.0f && g_overlay_pso)
            draw_overlay(g_swap_w, g_swap_h);
        barrier(g_swap_buf[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        g_swap_state[i] = D3D12_RESOURCE_STATE_PRESENT;
        g_targets_bound = 0;
        unbind();
        presented = 1;
    }
    fps_tick();
    fx_reload();
    frame_end();
    if (presented)
    {
        uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
        HRESULT hr = IDXGISwapChain3_Present(g_swap, g_vsync ? 1 : 0, !g_vsync && g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (gfx_profiling)
            g_prof.drawable_ns += gfx_now_ns() - t0;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            static int told;
            if (!told++)
                fprintf(stderr, "[recomp] gfx: device removed (%08lx)\n", (unsigned long)ID3D12Device_GetDeviceRemovedReason(g_dev));
        }
    }
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
    g_want_w = w, g_want_h = h;
}

/* --- start-up -------------------------------------------------------------------------------------------------- */
static ID3D12RootSignature* make_root(void)
{
    D3D12_DESCRIPTOR_RANGE tex2d = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0 };
    D3D12_DESCRIPTOR_RANGE texcube = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, 0 };
    D3D12_DESCRIPTOR_RANGE samps = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 0, 0 };
    D3D12_DESCRIPTOR_RANGE cmps = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 1, 0 };
    D3D12_ROOT_PARAMETER p[ROOT_COUNT];
    memset(p, 0, sizeof p);
    p[ROOT_U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[ROOT_U].Descriptor.ShaderRegister = 0;
    p[ROOT_U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[ROOT_BIND].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[ROOT_BIND].Constants.ShaderRegister = 1;
    p[ROOT_BIND].Constants.Num32BitValues = 16;
    p[ROOT_BIND].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        p[ROOT_STREAM0 + s].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[ROOT_STREAM0 + s].Descriptor.ShaderRegister = (UINT)s;
        p[ROOT_STREAM0 + s].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    p[ROOT_TEX2D].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_TEX2D].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_TEX2D].DescriptorTable.pDescriptorRanges = &tex2d;
    p[ROOT_TEX2D].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[ROOT_TEXCUBE].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_TEXCUBE].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_TEXCUBE].DescriptorTable.pDescriptorRanges = &texcube;
    p[ROOT_TEXCUBE].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[ROOT_SAMPLERS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_SAMPLERS].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_SAMPLERS].DescriptorTable.pDescriptorRanges = &samps;
    p[ROOT_SAMPLERS].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[ROOT_CMP].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_CMP].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_CMP].DescriptorTable.pDescriptorRanges = &cmps;
    p[ROOT_CMP].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[ROOT_SHADOW].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[ROOT_SHADOW].Descriptor.ShaderRegister = 2;
    p[ROOT_SHADOW].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    /* stream output for ray tracing's capture, and the heap read directly (ResourceDescriptorHeap) for its
     * passes - without that, where the device has not got it, the same signature with neither */
    D3D12_ROOT_SIGNATURE_DESC rd = { ROOT_COUNT, p, 0, NULL,
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT |
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED };
    ID3D12RootSignature* root = NULL;
    for (int tries = 0; tries < 2 && !root; ++tries)
    {
        ID3DBlob *blob = NULL, *err = NULL;
        if (tries)
            rd.Flags &= ~(D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
            fprintf(stderr, "[recomp] gfx: root signature: %s\n", err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?");
        else if (FAILED(ID3D12Device_CreateRootSignature(g_dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                     &IID_ID3D12RootSignature, (void**)&root)))
            root = NULL;
        if (blob)
            ID3D10Blob_Release(blob);
        if (err)
            ID3D10Blob_Release(err);
        g_root_heap = root && !tries;
    }
    return root;
}

/* a full-screen pipeline of the device's own: no depth, blended over (blend) or not, onto fmt */
static ID3D12PipelineState* own_pipeline(ID3DBlob* v, ID3DBlob* f, DXGI_FORMAT fmt, int blend)
{
    ID3D12PipelineState* p = NULL;
    if (v && f)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        memset(&pd, 0, sizeof pd);
        pd.pRootSignature = g_root;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(v), pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(v);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(f), pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(f);
        D3D12_RENDER_TARGET_BLEND_DESC* c = &pd.BlendState.RenderTarget[0];
        c->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        c->SrcBlend = c->SrcBlendAlpha = D3D12_BLEND_ONE;
        c->DestBlend = c->DestBlendAlpha = D3D12_BLEND_ZERO;
        c->BlendOp = c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (blend)
            c->BlendEnable = TRUE, c->SrcBlend = D3D12_BLEND_SRC_ALPHA, c->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = fmt;
        pd.SampleDesc.Count = 1;
        HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(g_dev, &pd, &IID_ID3D12PipelineState, (void**)&p);
        if (FAILED(hr))
        {
            InterlockedIncrement(&g_failures);
            fprintf(stderr, "[recomp] gfx: pipeline of the device's own failed (%08lx)\n", (unsigned long)hr);
            p = NULL;
        }
    }
    return p;
}

/* the present or overlay pipeline, onto the swap chain's format */
static ID3D12PipelineState* util_pipeline(const char* vs, const char* ps, int blend)
{
    ID3DBlob* v = compile(gfx_hlsl_util, vs, "vs_5_1");
    ID3DBlob* f = v ? compile(gfx_hlsl_util, ps, "ps_5_1") : NULL;
    ID3D12PipelineState* p = own_pipeline(v, f, DXGI_FORMAT_B8G8R8A8_UNORM, blend);
    if (v)
        ID3D10Blob_Release(v);
    if (f)
        ID3D10Blob_Release(f);
    return p;
}

/* the device on the high-performance adapter, else the default one */
static int make_device(void)
{
    const char* dbg = getenv("FFXI_D3D12_DEBUG");
    if (dbg && dbg[0] == '1')
    {
        ID3D12Debug* d = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&d)))
        {
            ID3D12Debug_EnableDebugLayer(d);
            ID3D12Debug_Release(d);
            fprintf(stderr, "[recomp] gfx: D3D12 debug layer on\n");
        }
    }
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void**)&g_factory)))
        g_factory = NULL;
    IDXGIFactory6* f6 = NULL;
    IDXGIAdapter1* adapter = NULL;
    if (g_factory && SUCCEEDED(IDXGIFactory4_QueryInterface(g_factory, &IID_IDXGIFactory6, (void**)&f6)))
    {
        IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void**)&adapter);
        IDXGIFactory6_Release(f6);
    }
    HRESULT hr = D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g_dev);
    if (FAILED(hr) && adapter)
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g_dev);
    char name[128] = "?";
    if (adapter)
    {
        DXGI_ADAPTER_DESC1 ad;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &ad)))
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, NULL, NULL);
        IDXGIAdapter1_Release(adapter);
    }
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: no Direct3D 12 device (%08lx)\n", (unsigned long)hr);
        g_dev = NULL;
        return 0;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS o;
    memset(&o, 0, sizeof o);
    ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    if (o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
    {
        fprintf(stderr, "[recomp] gfx: %s has resource binding tier %d; the back end needs tier 2\n", name, (int)o.ResourceBindingTier);
        ID3D12Device_Release(g_dev);
        g_dev = NULL;
        return 0;
    }
    fprintf(stderr, "[recomp] gfx: Direct3D 12 on %s\n", name);
    if (dbg && dbg[0] == '1' && FAILED(ID3D12Device_QueryInterface(g_dev, &IID_ID3D12InfoQueue, (void**)&g_info)))
        g_info = NULL; /* the layer's messages, printed after each submission (print_messages) */
    return 1;
}

#if defined(FFXI_UWP)
/* UWP: no window handle; the swap chain is for composition, and the app shows it in a SwapChainPanel */
static void make_swap_chain(SDL_Window* win)
{
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = pw > 0 ? (UINT)pw : 640, sd.Height = ph > 0 ? (UINT)ph : 480;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FRAMES;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    g_tearing = 0; /* composition swap chains cannot tear */
    IDXGISwapChain1* sc1 = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForComposition(g_factory, (IUnknown*)g_queue, &sd, NULL, &sc1);
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: swap chain failed (%08lx)\n", (unsigned long)hr);
        return;
    }
    uwp_attach_swapchain(sc1);
    IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g_swap);
    IDXGISwapChain1_Release(sc1);
    g_swap_w = sd.Width, g_swap_h = sd.Height;
    for (int i = 0; i < FRAMES; ++i)
        g_swap_rtv[i] = -1;
    swap_acquire();
}
#else
static void make_swap_chain(SDL_Window* win)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd || !g_factory)
    {
        fprintf(stderr, "[recomp] gfx: no window handle for the swap chain: %s\n", SDL_GetError());
        return;
    }
    IDXGIFactory5* f5 = NULL;
    if (SUCCEEDED(IDXGIFactory4_QueryInterface(g_factory, &IID_IDXGIFactory5, (void**)&f5)))
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            g_tearing = allow != FALSE;
        IDXGIFactory5_Release(f5);
    }
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = pw > 0 ? (UINT)pw : 640, sd.Height = ph > 0 ? (UINT)ph : 480;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FRAMES;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* sc1 = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForHwnd(g_factory, (IUnknown*)g_queue, hwnd, &sd, NULL, NULL, &sc1);
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: swap chain failed (%08lx)\n", (unsigned long)hr);
        return;
    }
    IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g_swap);
    IDXGISwapChain1_Release(sc1);
    IDXGIFactory4_MakeWindowAssociation(g_factory, hwnd, DXGI_MWA_NO_ALT_ENTER); /* full screen is the window's (SDL) */
    g_swap_w = sd.Width, g_swap_h = sd.Height;
    for (int i = 0; i < FRAMES; ++i)
        g_swap_rtv[i] = -1;
    swap_acquire();
}
#endif

uint64_t gfx_window_flags(void) { return 0; }

int gfx_init(void* window, int vsync)
{
    if (g_dev)
        return 1;
    if (!make_device())
        return 0;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void**)&g_queue);
    for (int i = 0; i < FRAMES; ++i)
        ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&g_frames[i].alloc);
    ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, NULL, &IID_ID3D12GraphicsCommandList,
        (void**)&g_list);
    ID3D12GraphicsCommandList_Close(g_list);
    ID3D12Device_CreateFence(g_dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&g_fence);
    g_fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&g_pipe_file_lock);
    g_root = make_root();
    if (!g_queue || !g_list || !g_fence || !g_root || !heap_init(&g_srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_HEAP, 1, 2) ||
        !heap_init(&g_samp, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP, 1, 0) ||
        !heap_init(&g_rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_HEAP, 0, 0) ||
        !heap_init(&g_dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_HEAP, 0, 0))
    {
        fprintf(stderr, "[recomp] gfx: Direct3D 12 set-up failed\n");
        ID3D12Device_Release(g_dev);
        g_dev = NULL;
        return 0;
    }
    /* the null views every unbound slot reads, and a sampler for the present */
    D3D12_SHADER_RESOURCE_VIEW_DESC nv;
    memset(&nv, 0, sizeof nv);
    nv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, nv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(g_dev, NULL, &nv, heap_cpu(&g_srv, SRV_NULL_2D));
    nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE, nv.TextureCube.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(g_dev, NULL, &nv, heap_cpu(&g_srv, SRV_NULL_CUBE));
    GfxSampler linear;
    memset(&linear, 0, sizeof linear);
    linear.addr_u = linear.addr_v = linear.addr_w = 3, linear.mag = linear.min = 2;
    g_present_samp = sampler_slot(&linear);
    g_dummy = make_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);

    const char* prof = getenv("FFXI_PROFILE");
    gfx_profiling = prof && prof[0] && prof[0] != '0';
    if (gfx_profiling)
    {
        D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * FRAMES, 0 };
        if (SUCCEEDED(ID3D12Device_CreateQueryHeap(g_dev, &qh, &IID_ID3D12QueryHeap, (void**)&g_queries)))
        {
            g_query_rb = make_buffer(16 * FRAMES, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            if (g_query_rb)
                ID3D12Resource_Map(g_query_rb, 0, NULL, (void**)&g_query_cpu);
            ID3D12CommandQueue_GetTimestampFrequency(g_queue, &g_ts_freq);
        }
    }
    /* FFXI_FPS=0 hides the overlay (the settings file's fps, from Config > Modern, has the last word) */
    fx_config();
    const char* show = getenv("FFXI_FPS");
    if (show && show[0] == '0')
        g_fxs.fps = 0.0f;
    g_present_pso = util_pipeline("present_vs", "present_fs", 0);
    g_present_cas_pso = util_pipeline("present_vs", "present_cas_fs", 0);
    g_overlay_pso = util_pipeline("overlay_vs", "overlay_fs", 1);
    if (!g_sync_pipelines)
        prewarm_pipelines();
    g_vsync = vsync;
    if (window)
    {
        g_window = (SDL_Window*)window;
        make_swap_chain(g_window);
    }
    return 1;
}
