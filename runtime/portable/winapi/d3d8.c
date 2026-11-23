/* Direct3D 8 for 64-bit hosts (R3.1): the interface FFXiMain draws through, implemented by us.
 *
 * The translated game still speaks D3D8 - Direct3DCreate8, then COM calls on IDirect3DDevice8 and
 * its resources - so this file is the D3D8 front end the Metal renderer (R3.2) sits under.
 * It owns what the game can observe:
 *
 *   - the COM objects (guest memory: a vtable pointer and our object index; every method is a
 *     thunk named "d3d8.dll!IDirect3DDevice8::SetRenderState" and so on, so a method the game
 *     reaches that is not written yet traps with its name),
 *   - resource memory the game locks (textures, surfaces, vertex and index buffers live in the
 *     guest window, allocated on first lock),
 *   - device state with D3D8 semantics - defaults, readback (the game calls GetTransform and
 *     GetViewport millions of times), state-block recording, capture and apply, the references
 *     bound resources hold, the stream/index reset after the ...UP draws.
 *
 * What is drawn goes to the graphics back end (gfx.h: Metal on macOS, R3.2): every texture and
 * render target has a GfxTex beside its guest memory, and Clear, the Draw* calls and Present turn
 * the device state into the back end's clears and draw packets (see "drawing" below).
 *
 * Which copy of a resource is current: for textures the game fills (the managed and system pools)
 * the guest memory is, and a level is uploaded when it is next drawn with after a lock; for render
 * targets and depth the GPU is, and a lock reads the level back (and an unlock writes it again).
 * Vertex and index buffers stay in guest memory; each draw copies the range it uses.
 *
 * The method set is the one the game was measured to use (runtime/specs/FFXiMain.2026-08-22.d3d8-surface.txt); vtable order is the D3D8 ABI, as the R1 proxy
 * (d3d8proxy.cpp) lists it. */
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "d3d8.h"
#include "../gfx.h"
#include "../gfx/gfx_queue.h" /* -DGFX_QUEUE: the calls go through the render queue */
#include "../gthread.h"
#include "../gwin.h"
#include "../cachedir.h"
#include "../plat.h"
#include "thunk.h"
#include "user32.h"
#if defined(FFXI_ANDROID_VULKAN)
#include "build.h" /* FFXI_WRAP_PROBE_CTOR/DTOR, when the Android translation wraps them */
#endif
#include "d3d8_cmd.h" /* the D3D calls recorded for a worker thread (FFXI_D3D_WORKER), or run at once */

/* --- the worker's side (d3d8_cmd.h) -------------------------------------------------------------------------- */
/* the records */
enum
{
    W_CALL = 1, /* a deferred call: its shim, run with its arguments (and what they point at) in the guest ring */
    W_GPU_SKIP, /* d3d8_probe_test's g_gpu_skip, in order with the draws it is for */
    W_BUFDATA,  /* what the game wrote into a vertex or index buffer (Buffer_Unlock) */
    W_HOST,     /* a host function, with bytes of its own (d3d8_host_call) */
    W_FAST,     /* a deferred call whose arguments (and what they point at, a little) are in the record itself */
};
typedef struct WFast
{
    Shim fn;
    uint32_t nwords, nblob; /* its argument words (`this` first), then nblob bytes at 16 past them */
} WFast;
typedef struct WHost
{
    void (*fn)(const void* data);
    uint32_t n, pad;
} WHost; /* (and its n bytes) */
struct WBufData;
static void buf_data(const struct WBufData* w);

typedef struct WCall
{
    Shim fn;
    uint32_t frame; /* the guest address of its arguments, `this` first */
} WCall;

static int g_gpu_skip; /* inside a call whose draws, clears and copies nobody sees (d3d8_probe_test) */
static int g_gpu_skip_seen; /* the call's first draw has been looked at */

/* The worker logs nothing itself (another thread's stdio breaks the game thread's, on the Switch): its lines wait
 * here until the game's thread has it idle (d3dw_after_sync) */
static char g_relay[8192];
static size_t g_relay_n;
static unsigned g_relay_lost;

static void d3d_log(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!d3dw_on_worker())
    {
        (rt_log)("%s", line);
        return;
    }
    size_t n = strlen(line);
    if (g_relay_n + n + 1 > sizeof g_relay)
    {
        g_relay_lost++;
        return;
    }
    memcpy(g_relay + g_relay_n, line, n + 1);
    g_relay_n += n + 1;
}
#define rt_log d3d_log

/* objects whose last reference the worker dropped (an unbind): destroyed on the game's thread, which has the guest
 * heap (d3dw_after_sync) */
static uint32_t g_dead[4096];
static uint32_t g_ndead;

static void obj_destroy_now(uint32_t p);
static int defer_on(void);
/* FFXI_PROFILE: the calls that waited for the worker, by thunk */
static uint32_t g_wait_by[4096];
/* ... and the waits of the shim's own calls to d3dw_sync, by where */
static struct { const char* at; uint32_t n; } g_wait_at[24];
static int d3dw_sync_at(const char* at)
{
    int w = d3dw_sync();
    if (w)
        for (int i = 0; i < 24; ++i)
            if (g_wait_at[i].at == at || !g_wait_at[i].at)
            {
                g_wait_at[i].at = at, g_wait_at[i].n++;
                break;
            }
    return w;
}
/* the transforms on the game's thread, for GetTransform (F_GetTransform): 280 = NXF */
static float g_fxf[280][16];
static uint8_t g_fxf_ok[280];
/* the streams and indices on the game's thread, for the draws' snapshots of dynamic buffers (front_snapshot) */
static struct
{
    uint32_t stream[16], stride[16], ib, base; /* 16 = NSTREAMS */
    uint32_t vs;                               /* the vertex shader or FVF: which of the streams a draw reads */
} g_fs;
/* FFXI_PROFILE: how many of the game's state calls set what it had set already (the game's own view: a state block
 * applied, or Reset, forgets it) - what dropping them on the game's thread would save */
static struct
{
    uint32_t rs[256], tss[8][32], tex[8];
    uint8_t rs_ok[256], tss_ok[8][32], tex_ok[8];
} g_fm;



static void d3dw_after_sync(void)
{
    for (size_t at = 0; at < g_relay_n;)
    {
        (rt_log)("%s", g_relay + at);
        at += strlen(g_relay + at) + 1;
    }
    g_relay_n = 0;
    if (g_relay_lost)
        (rt_log)("[d3d8] the worker's log: %u lines lost\n", g_relay_lost), g_relay_lost = 0;
    uint32_t n = g_ndead;
    g_ndead = 0;
    for (uint32_t i = 0; i < n; ++i)
        obj_destroy_now(g_dead[i]);
}

static void d3dw_run(uint32_t op, const void* body, uint32_t bytes)
{
    (void)bytes;
    switch (op)
    {
    case W_CALL:
    {
        const WCall* c = (const WCall*)body;
        static Guest wg; /* (the worker's, or inline the game thread's: never both) */
        wg.esp = c->frame - 4u;
        c->fn(&wg);
        break;
    }
    case W_GPU_SKIP:
        memcpy(&g_gpu_skip, body, sizeof g_gpu_skip);
        g_gpu_skip_seen = 0;
        break;
    case W_BUFDATA:
        buf_data((const struct WBufData*)body);
        break;
    case W_FAST:
    {
        /* (its arguments into the worker's frame, the pointers already at their places there: the front set them) */
        const WFast* c = (const WFast*)body;
        const uint8_t* w = (const uint8_t*)(c + 1);
        uint32_t wbytes = (uint32_t)D3DW_ALIGN16(4u * c->nwords);
        memcpy(GUEST_PTR(g_w.scratch), w, 4u * c->nwords);
        if (c->nblob)
            memcpy(GUEST_PTR(g_w.scratch + wbytes), w + wbytes, c->nblob);
        static Guest fg;
        fg.esp = g_w.scratch - 4u;
        c->fn(&fg);
        break;
    }
    case W_HOST:
    {
        const WHost* h = (const WHost*)body;
        h->fn(h + 1);
        break;
    }
    default:
        fprintf(stderr, "d3d8 worker: an unknown record %u\n", op);
        abort();
    }
}

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define E_NOINTERFACE 0x80004002u
#define E_OUTOFMEMORY 0x8007000Eu

/* D3DFORMAT */
#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_R5G6B5 23u
#define FMT_X1R5G5B5 24u
#define FMT_A1R5G5B5 25u
#define FMT_A4R4G4B4 26u
#define FMT_A8 28u
#define FMT_L8 50u
#define FMT_A8L8 51u
#define FMT_V8U8 60u
#define FMT_D16 80u
#define FMT_D24S8 75u
#define FMT_D24X8 77u
#define FMT_INDEX16 101u
#define FMT_INDEX32 102u
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')

/* D3DRESOURCETYPE */
#define RT_SURFACE 1u
#define RT_TEXTURE 3u
#define RT_VOLUMETEXTURE 4u
#define RT_CUBETEXTURE 5u
#define RT_VERTEXBUFFER 6u
#define RT_INDEXBUFFER 7u

#define USAGE_RENDERTARGET 1u
#define USAGE_DEPTHSTENCIL 2u

/* D3DCAPS8 (212 bytes) as Windows 11's d3d8 reports it for a HAL device (NVIDIA RTX A4500,
 * captured 2026-09-24): vertex shader 1.1, pixel shader 1.4, 8 texture stages, 16384 textures,
 * 8 lights, 16 streams. Everything here is within what Metal on an M1 provides. */
static const uint32_t CAPS8[53] = {
    0x00000001, 0x00000000, 0x00020000, 0x200a0000, 0x00000420, 0x8000000f, 0x00000001, 0x001bbef0, 0x00000ef2,
    0x00f37191, 0x000000ff, 0x00001fff, 0x00001fff, 0x000000ff, 0x00084208, 0x0001ecc5, 0x03030700, 0x03030300,
    0x03030300, 0x0000003f, 0x0000003f, 0x0000001f, 0x00004000, 0x00004000, 0x00000800, 0x00002000, 0x00004000,
    0x00000010, 0x501502f9, 0xccbebc20, 0xccbebc20, 0x4cbebc20, 0x4cbebc20, 0x00000000, 0x000000ff, 0x00180008,
    0x03feffff, 0x00000008, 0x00000008, 0x0000003b, 0x00000008, 0x00000008, 0x00000004, 0x00000000, 0x46000000,
    0x00ffffff, 0x00ffffff, 0x00000010, 0x000000ff, 0xfffe0101, 0x00000100, 0xffff0104, 0x477fe000,
};

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* FFXI_DRAWLOG (see cap_present) */
static const char* g_cap_path;
static void scene_forget(GfxTex* t);
static FILE* g_cap;
static uint32_t g_cap_esp, g_cap_frame, g_cap_n;
static uint32_t g_cap_at[32], g_cap_nat; /* <path>.at: draws after which the target is dumped */
static void cap_present(void);
static void water_flush(void); /* the PC port's drawing mistakes (fix_water_layers, fix_button_rims) */
static void fixes_frame(void);
static void dis_present(void); /* disintegrate (fix_disintegrate) */
static int g_btn_shadows; /* this frame's button shadows (button_body) */
static uint32_t g_halo_src, g_halo_low; /* this frame's halo silhouette and its copy's surface (halo_quad) */
static float g_halo_depth; /* the halo's model's depth at the copy-down, for the cap (fix_halo_max); 0 none */
static void scene_present(void);

/* --- formats ----------------------------------------------------------------------------------------- */
static uint32_t fmt_block(uint32_t f) /* bytes per 4x4 block, or 0 */
{
    return f == FMT_DXT1 ? 8u : (f == FMT_DXT2 || f == FMT_DXT3 || f == FMT_DXT4 || f == FMT_DXT5) ? 16u : 0u;
}

static uint32_t fmt_bytes(uint32_t f) /* bytes per pixel for the uncompressed formats */
{
    switch (f)
    {
    case FMT_A8:
    case FMT_L8:
    case 41: /* P8 */
    case 52: /* A4L4 */
        return 1;
    case FMT_R5G6B5:
    case FMT_X1R5G5B5:
    case FMT_A1R5G5B5:
    case FMT_A4R4G4B4:
    case FMT_A8L8:
    case FMT_V8U8:
    case FMT_D16:
    case 73: /* D15S1 */
    case FMT_INDEX16:
        return 2;
    default: return 4;
    }
}

static uint32_t fmt_pitch(uint32_t f, uint32_t w)
{
    uint32_t b = fmt_block(f);
    return b ? ((w + 3) / 4 ? (w + 3) / 4 : 1) * b : w * fmt_bytes(f);
}

static uint32_t fmt_size(uint32_t f, uint32_t w, uint32_t h)
{
    return fmt_pitch(f, w) * (fmt_block(f) ? ((h + 3) / 4 ? (h + 3) / 4 : 1) : h);
}

/* --- objects ----------------------------------------------------------------------------------------- */
enum
{
    O_FREE,
    O_D3D,
    O_DEVICE,
    O_TEXTURE,
    O_CUBE,
    O_VB,
    O_IB,
    O_SURFACE,
};

typedef struct Obj
{
    uint8_t kind, implicit; /* implicit: the swap chain's back buffer, the auto depth buffer */
    int32_t refs;
    uint32_t guest;     /* the COM pointer the game holds */
    uint32_t container; /* surfaces of a texture: its COM pointer; their references are the texture's */
    uint32_t format, usage, pool, width, height, levels, size, fvf;
    uint32_t mem; /* guest memory, on first lock */
    uint32_t* subs; /* textures: level surfaces (cubes: face * levels + level) */
    uint32_t nsubs;
    uint32_t lod, priority;
    GfxTex* gpu;         /* textures, render targets, depth surfaces */
    GfxBuf* gbuf;        /* static vertex and index buffers: the GPU's copy */
    uint8_t gbuf_dirty;  /* ... which a lock that writes makes stale */
    uint32_t serial;     /* vertex and index buffers: new at creation and at every lock that writes (range_cached) */
    uint8_t face, level; /* surfaces of a texture: where in it */
    uint8_t dirty;       /* surfaces of a texture: guest memory is newer than the GPU's copy */
    uint8_t gpu_locked;  /* a GPU-owned surface is locked: written back at unlock */
    GfxTex* repl;        /* textures: a texture pack's replacement, drawn with instead of gpu */
    uint32_t repl_pad;   /* ... whose glyph quads are drawn this many texels wider each side */
    uint32_t repl_pad_min; /* ... when they are at least this many texels tall */
    const struct PackEntry* repl_entry; /* ... and its glyph table, if it has one */
    uint8_t repl_mips;   /* ... sampled through all its mipmaps (a texture source's, not a glyph sheet) */
    uint32_t pw, ph;     /* render-target textures drawn at the screen's resolution (native_size): gpu's
                          * size, where the game sees width x height; 0 when they are the same */
    uint8_t water;       /* vertex buffers: made while the game loaded a water model (d3d8_water_loading) */
    uint8_t unread;      /* render-target textures: drawn into since last cleared or sampled (d3d8_probe_test) */
    uint8_t wr_track, wr_known; /* render-target textures composited as a whole: what was drawn is followed (wr) */
    int32_t wr[4];       /* ... its pixels x0, y0, x1, y1 since a clear to alpha 0, when wr_known (fade_scissor) */
    uint32_t drawn;      /* FFXI_PROFILE: the frame (g_ws.frame) a draw last used it in: vertex and index buffers,
                          * textures */
    /* vertex and index buffers with the worker on (FFXI_D3D_WORKER=1): mem is the worker's, fmem the game's - its
     * locks write there, and an unlock sends what it wrote over (W_BUFDATA), in order with the draws */
    uint32_t fmem, lk_off, lk_n; /* (lk_*: what the locks since the last send wrote, as one range) */
    uint8_t lk_write, lk_depth;  /* ... and the locks not yet unlocked: it goes over at the last unlock */
    uint8_t loose;               /* written outside its locks (d3dw_check_buffers found it): sent at the draws */
    /* the occlusion probe's 16x16 surface with the worker on (probe_lock_now): the sky probe's answers, apart from
     * mem - which the game reads at once and the other probes' answers (fully visible) fill */
    uint32_t sky_mem;
} Obj;
static uint32_t g_buf_serial; /* Obj.serial's last */

/* FFXI_PROFILE: what a d3d8 worker thread (draws built off the game thread) would have to wait for - the calls
 * that need the worker's results or its work done, and the locks of what it may still be drawing from (an
 * object drawn from this frame) - and what the locks would copy. Logged every 10 s from Present. */
static struct
{
    int on; /* -1 not read */
    uint32_t frame;
    uint64_t at;
    uint32_t frames, creates, destroys, buf_locks, buf_locks_drawn, tex_locks, tex_locks_drawn, rt_locks, copies,
        resets, draws, probe_locks;
    uint64_t buf_lock_bytes, tex_lock_bytes, unlock_bytes, snap_bytes;
    uint32_t rs_calls, rs_same, tss_calls, tss_same, tex_calls, tex_same; /* same_count */
    uint32_t applies, applies_forget; /* state blocks applied, and of them ones the front forgot its states at */
} g_ws = { -1, 1 };

/* the worker's check of the buffers (d3dw_check_buffers) */
static struct
{
    int every; /* -1 not read */
    uint32_t n;
    uint32_t cur, quiet, made_at; /* the interval now (every, doubled per clean sweep), checks since one found any, objects made then */
    uint64_t nested, checked, differ, differ_bytes, bytes, ns, runs; /* (bytes, ns, runs: what the checks compared, took) */
    uint32_t logged, next; /* next: where the next check starts (an eighth at a time) */
} g_bufchk = { -1 };

/* FFXI_D3D_SNAP=all: every dynamic buffer copied at its draws (as before the buffer check found the loose ones) */
static int snap_all(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("FFXI_D3D_SNAP");
        v = e && !strcmp(e, "all");
    }
    return v;
}

static int ws_on(void)
{
    if (g_ws.on < 0)
    {
        const char* e = getenv("FFXI_PROFILE");
        g_ws.on = e && *e && *e != '0';
    }
    return g_ws.on;
}

static void ws_frame(void)
{
    if (!ws_on())
        return;
    g_ws.frame++, g_ws.frames++;
    uint64_t now = rt_monotonic_ns();
    if (!g_ws.at)
        g_ws.at = now;
    if (now - g_ws.at < 10000000000ull)
        return;
    double f = g_ws.frames ? (double)g_ws.frames : 1.0;
    rt_log("[d3d8] for a worker, per frame: %.0f draws; waits: %.2f creates, %.2f destroys, %.2f render-target locks, "
           "%.2f CopyRects, %u resets; locks: buffers %.1f (%.1f of ones drawn this frame, "
           "%.0f KB), textures %.1f (%.1f drawn this frame, %.0f KB)\n",
        g_ws.draws / f, g_ws.creates / f, g_ws.destroys / f, g_ws.rt_locks / f, g_ws.copies / f, g_ws.resets,
        g_ws.buf_locks / f, g_ws.buf_locks_drawn / f, (double)g_ws.buf_lock_bytes / f / 1024.0, g_ws.tex_locks / f,
        g_ws.tex_locks_drawn / f, (double)g_ws.tex_lock_bytes / f / 1024.0);
    if (g_w.mode > 0)
    {
        char by[1024];
        int o = 0;
        by[0] = 0;
        if (g_w.threaded)
            for (int k = 0; k < 6; ++k)
            {
                uint32_t best = 0, bi = 0;
                for (uint32_t i = 0; i < 4096; ++i)
                    if (g_wait_by[i] > best)
                        best = g_wait_by[i], bi = i;
                if (!best)
                    break;
                const char* nm = thunk_name_at(bi);
                const char* m = nm ? strstr(nm, "::") : NULL;
                o += snprintf(by + o, sizeof by - (size_t)o, " %s %.1f", m ? m + 2 : nm ? nm : "?", best / f);
                g_wait_by[bi] = 0;
            }
        for (int i = 0; i < 24 && g_wait_at[i].at; ++i)
            if (g_wait_at[i].n && o < (int)sizeof by - 1)
                o += snprintf(by + o, sizeof by - (size_t)o, " [%s] %.1f", g_wait_at[i].at, g_wait_at[i].n / f),
                    g_wait_at[i].n = 0;
        memset(g_wait_by, 0, sizeof g_wait_by);
        rt_log("[d3d8] worker: %.0f records/frame (%.0f KB, %.0f KB of buffer writes, %.0f KB of dynamic buffers at "
               "the draws); %.1f probe locks/frame without a wait; %.1f syncs/frame, %.1f of them waited "
               "(%.2f ms/frame); Present waited for the frame before %.2f times/frame (%.2f ms/frame)%s%s\n",
            (double)g_w.recs / f, (double)g_w.bytes / f / 1024.0, (double)g_ws.unlock_bytes / f / 1024.0,
            (double)g_ws.snap_bytes / f / 1024.0, g_ws.probe_locks / f,
            (double)g_w.syncs / f, (double)g_w.waits / f,
            (double)g_w.wait_ns / f * 1e-6, (double)g_w.present_waits / f, (double)g_w.present_wait_ns / f * 1e-6,
            by[0] ? "; waits by call:" : "", by);
        rt_log("[d3d8] worker: per frame, calls that set what was set already (not sent: FFXI_D3D_SKIP): render states %.0f of %.0f, texture "
               "stages %.0f of %.0f, textures %.0f of %.0f; state blocks applied %.0f, %.0f of them too big to follow\n",
            g_ws.rs_same / f, g_ws.rs_calls / f, g_ws.tss_same / f, g_ws.tss_calls / f, g_ws.tex_same / f,
            g_ws.tex_calls / f, g_ws.applies / f, g_ws.applies_forget / f);
        if (g_w.threaded)
            rt_log("[d3d8] worker check: %llu nested buffer locks; %llu buffers checked, %llu written outside a lock "
                   "(%llu KB); %llu checks, %.2f ms and %.0f KB each, every %u frames now\n",
                (unsigned long long)g_bufchk.nested, (unsigned long long)g_bufchk.checked,
                (unsigned long long)g_bufchk.differ, (unsigned long long)(g_bufchk.differ_bytes >> 10),
                (unsigned long long)g_bufchk.runs, g_bufchk.runs ? (double)g_bufchk.ns / g_bufchk.runs * 1e-6 : 0.0,
                g_bufchk.runs ? (double)g_bufchk.bytes / g_bufchk.runs / 1024.0 : 0.0, g_bufchk.cur);
        g_bufchk.nested = g_bufchk.checked = g_bufchk.differ = g_bufchk.differ_bytes = 0;
        g_bufchk.bytes = g_bufchk.ns = g_bufchk.runs = 0;
        g_w.recs = g_w.bytes = g_w.syncs = g_w.waits = g_w.wait_ns = g_w.present_waits = g_w.present_wait_ns = 0;
    }
    uint32_t frame = g_ws.frame;
    memset(&g_ws, 0, sizeof g_ws);
    g_ws.on = 1, g_ws.frame = frame, g_ws.at = now;
}

/* The objects, in pages that never move (a d3d8 worker thread may hold an Obj* while the game's thread makes
 * more): object i is OBJ_AT(i). */
#define OBJ_PAGE 1024u
#define OBJ_PAGES 256u
static Obj* g_obj_pages[OBJ_PAGES];
#define OBJ_AT(i) (&g_obj_pages[(i) / OBJ_PAGE][(i) % OBJ_PAGE])
static uint32_t g_nobjs, g_free_hint = 1;
static uint32_t g_obj_made; /* objects made so far (d3dw_check_buffers: a zone's load makes many) */
static uint32_t g_vtbl[O_SURFACE + 1];

static uint32_t obj_new(int kind)
{
    uint32_t i = g_free_hint;
    while (i < g_nobjs && OBJ_AT(i)->kind != O_FREE)
        i++;
    if (i >= g_nobjs)
    {
        if ((g_nobjs + 2) / OBJ_PAGE >= OBJ_PAGES)
        {
            rt_log("[recomp] d3d8: more than %u objects\n", OBJ_PAGE * OBJ_PAGES);
            abort();
        }
        /* the page of the next one or two (index 0 and the first object, at the start) */
        for (uint32_t k = g_nobjs; k <= g_nobjs + 1; ++k)
            if (!g_obj_pages[k / OBJ_PAGE])
                g_obj_pages[k / OBJ_PAGE] = (Obj*)calloc(OBJ_PAGE, sizeof(Obj));
        if (!g_nobjs)
            OBJ_AT(g_nobjs++)->kind = O_FREE; /* index 0 is never an object */
        i = g_nobjs++;
    }
    g_free_hint = i + 1;
    g_obj_made++;
    Obj* o = OBJ_AT(i);
    memset(o, 0, sizeof *o);
    g_ws.creates++;
    o->kind = (uint8_t)kind;
    o->refs = 1;
    o->guest = gheap_alloc(8, 1);
    wr32(o->guest, g_vtbl[kind]);
    wr32(o->guest + 4, i);
    return o->guest;
}

static Obj* obj(uint32_t p)
{
    if (!p)
        return NULL;
    uint32_t i = rd32(p + 4);
    if (!i || i >= g_nobjs)
        return NULL;
    Obj* o = OBJ_AT(i);
    return o->kind != O_FREE && o->guest == p ? o : NULL;
}

static uint32_t obj_release(uint32_t p);

static void obj_destroy(Obj* o)
{
    g_ws.destroys++;
    for (uint32_t i = 0; i < o->nsubs; ++i)
    {
        Obj* s = obj(o->subs[i]);
        if (s)
        {
            s->container = 0;
            s->refs = 1;
            obj_release(s->guest);
        }
    }
    free(o->subs);
    scene_forget(o->gpu);
    gfx_tex_destroy(o->gpu);
    gfx_buf_destroy(o->gbuf);
    if (o->mem)
        gheap_free(o->mem);
    if (o->fmem)
        gheap_free(o->fmem);
    if (o->sky_mem)
        gheap_free(o->sky_mem);
    uint32_t idx = rd32(o->guest + 4);
    wr32(o->guest, 0);
    gheap_free(o->guest);
    o->kind = O_FREE;
    if (idx < g_free_hint)
        g_free_hint = idx;
}

static uint32_t obj_addref(uint32_t p)
{
    Obj* o = obj(p);
    if (!o)
        return 0;
    if (o->container)
        return obj_addref(o->container);
    /* (atomic: the worker binds and unbinds while the game's thread adds and drops its own references) */
    return plat_atomic_add32((volatile uint32_t*)&o->refs, 1) + 1;
}

static uint32_t obj_release(uint32_t p)
{
    Obj* o = obj(p);
    if (!o)
        return 0;
    if (o->container)
        return obj_release(o->container);
    /* whether this release took the last reference: from the exchange itself, not a second read of refs (the game's
     * thread and the worker may each drop one at once, and both would read 0 after) */
    uint32_t r;
    for (;;)
    {
        r = (uint32_t)o->refs;
        if (!r || plat_atomic_cas32((volatile uint32_t*)&o->refs, r, r - 1) == r)
            break;
    }
    if (r <= 1 && !o->implicit && o->kind != O_DEVICE && o->kind != O_D3D)
    {
        if (d3dw_on_worker())
        {
            /* (its last holder was the device: the game's thread destroys it once the worker is idle) */
            if (g_ndead < sizeof g_dead / sizeof g_dead[0])
                g_dead[g_ndead++] = p;
            return 0;
        }
        /* the game's last reference: a call of its still to run on the worker may bind it again (and the wait may
         * itself destroy it, if the worker dropped it too: obj_destroy_now) */
        d3dw_sync_at("obj_release");
        o = obj(p);
        if (!o)
            return 0;
        if (!o->refs)
            obj_destroy(o);
        return (uint32_t)o->refs;
    }
    return r ? r - 1 : 0;
}

static void obj_destroy_now(uint32_t p)
{
    Obj* o = obj(p);
    if (o && !o->refs && !o->implicit && o->kind != O_DEVICE && o->kind != O_D3D)
        obj_destroy(o);
}

/* a pointer the device or a state block holds: references the new one, releases the old */
static void bind(uint32_t* slot, uint32_t p)
{
    if (*slot == p)
        return;
    if (p)
        obj_addref(p);
    uint32_t old = *slot;
    *slot = p;
    if (old)
        obj_release(old);
}

static uint32_t obj_mem(Obj* o)
{
    if (!o->mem)
        o->mem = gheap_alloc(o->size ? o->size : 16, 1);
    return o->mem;
}

static uint32_t new_surface(uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t pool, uint32_t container)
{
    uint32_t p = obj_new(O_SURFACE);
    Obj* s = obj(p);
    s->format = fmt;
    s->width = w;
    s->height = h;
    s->usage = usage;
    s->pool = pool;
    s->size = fmt_size(fmt, w, h);
    s->container = container;
    if (!container && (usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)))
        s->gpu = gfx_tex_create(GFX_TEX_2D, fmt, w, h, 1, (usage & USAGE_DEPTHSTENCIL) ? GFX_USE_DEPTH : GFX_USE_RT);
    return p;
}

/* the GPU texture behind a surface, and which face and level of it */
static GfxTex* surface_gpu(const Obj* s, uint32_t* face, uint32_t* level)
{
    if (s->container)
    {
        Obj* t = obj(s->container);
        *face = s->face, *level = s->level;
        return t ? t->gpu : NULL;
    }
    *face = *level = 0;
    return s->gpu;
}

/* render targets and depth: the GPU holds the current pixels, not guest memory */
static int gpu_owned(const Obj* s)
{
    uint32_t f, l;
    return (s->usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) && surface_gpu(s, &f, &l);
}

static void mark_dirty(Obj* s)
{
    s->dirty = 1;
    Obj* t = s->container ? obj(s->container) : NULL;
    if (t)
        t->dirty = 1;
}

/* --- water ----------------------------------------------------------------------------------------------
 * FFXI's water is map models (the effect directories mizu, umi1, taki ... of a zone's DAT) that the
 * game places and tints through its effect generators. The host hooks the model loader and brackets
 * each water model's load with d3d8_water_loading(1) and (0); the vertex buffers made in between are
 * the model's, and a draw from one is the game's water (GfxDraw.water) for the back end's water. Per
 * thread: a loader thread making the buffers of other models meanwhile is not caught up in it. */
static RT_TLS int g_water_loading;
static uint32_t g_water_buffers; /* made so far, for the log */

void d3d8_water_loading(int on)
{
    g_water_loading = on;
}

/* --- texture packs -------------------------------------------------------------------------------------
 * d3d8_texture_pack(folder): high-resolution replacements for the game's own textures (the font, say),
 * made by tools/make_texpack.py. Each is <folder>/<hash>_<w>x<h>.dds: the FNV-1a hash of the first
 * level of a w x h texture as the game uploads it, and the replacement, DXT1/3/5 with its mipmaps.
 * The replacement keeps the original's layout at a larger size, so the game's texture coordinates
 * (fractions of the texture) still land on the same art; it is drawn with instead of the original,
 * filtered with its mipmaps, and dropped again if the game writes something else into the texture.
 * Only textures of a size some entry has are hashed. FFXI_TEXLOG=1 hashes and logs every texture
 * instead, to find the one to replace; with FFXI_TEXDUMP=<folder> as well, each level hashed is
 * written there as <hash>.bin, in D3D's layout. */
typedef struct PackEntry
{
    uint64_t hash;
    uint32_t w, h, pad, pad_min; /* _pad<N>[min<M>] in the name: texels each glyph quad drawn with it is
                                  * widened by (only quads at least M texels tall) */
    float* glyphs;               /* <entry>.glyphs beside it: each glyph's ink box (x0 x1 y0 y1, texels) */
    uint32_t nglyphs;
    char* path;
    GfxTex* tex;
    uint8_t tried; /* loaded, or failed to */
} PackEntry;

static PackEntry* g_pack;
static uint32_t g_npack;
static int g_texlog = -1;

void d3d8_texture_pack(const char* dir)
{
    PlatDir* d = plat_dir_open(dir);
    if (!d)
    {
        rt_log("[recomp] textures: cannot read %s\n", dir);
        return;
    }
    uint32_t added = 0;
    for (const char* name; (name = plat_dir_next(d));)
    {
        unsigned long long hash;
        uint32_t w, h;
        char ext[8];
        uint32_t pad = 0, pad_min = 0;
        if (sscanf(name, "%16llx_%ux%u_pad%umin%u.%7s", &hash, &w, &h, &pad, &pad_min, ext) != 6 &&
            (pad_min = 0, sscanf(name, "%16llx_%ux%u_pad%u.%7s", &hash, &w, &h, &pad, ext) != 5) &&
            (pad = 0, sscanf(name, "%16llx_%ux%u.%7s", &hash, &w, &h, ext) != 4))
            continue;
        if (strcmp(ext, "dds") || strlen(name) < 16)
            continue;
        g_pack = (PackEntry*)realloc(g_pack, (g_npack + 1) * sizeof *g_pack);
        size_t n = strlen(dir) + strlen(name) + 2;
        char* path = (char*)malloc(n);
        snprintf(path, n, "%s%c%s", dir, plat_path_sep, name);
        PackEntry e = { hash, w, h, pad, pad_min, NULL, 0, path, NULL, 0 };
        char gp[1100];
        snprintf(gp, sizeof gp, "%.*s.glyphs", (int)(strlen(path) - 4), path);
        FILE* gf = fopen(gp, "r");
        float b[4];
        while (gf && fscanf(gf, "%f %f %f %f", &b[0], &b[1], &b[2], &b[3]) == 4)
        {
            e.glyphs = (float*)realloc(e.glyphs, (e.nglyphs + 1) * 4 * sizeof(float));
            memcpy(e.glyphs + 4 * e.nglyphs++, b, sizeof b);
        }
        if (gf)
            fclose(gf);
        g_pack[g_npack++] = e;
        added++;
    }
    plat_dir_close(d);
    rt_log("[recomp] textures: %u replacement%s from %s\n", added, added == 1 ? "" : "s", dir);
}

/* A DDS file's texture: DXT1/3/5 only, every level in the file. NULL if it is not one. */
static GfxTex* load_dds(const char* path)
{
    size_t size = 0;
    unsigned char* f = plat_read_file(path, &size);
    GfxTex* t = NULL;
    uint32_t fmt = 0, w = 0, h = 0, levels = 0;
    if (f && size >= 128 && !memcmp(f, "DDS ", 4) && (f[80] & 4))
    {
        h = (uint32_t)f[12] | f[13] << 8 | f[14] << 16 | (uint32_t)f[15] << 24;
        w = (uint32_t)f[16] | f[17] << 8 | f[18] << 16 | (uint32_t)f[19] << 24;
        levels = (uint32_t)f[28] | f[29] << 8;
        fmt = FOURCC(f[84], f[85], f[86], f[87]);
    }
    uint32_t blk = fmt_block(fmt);
    if (blk && w && h && w <= 16384 && h <= 16384)
    {
        levels = levels ? levels : 1;
        size_t off = 128, need = 0;
        for (uint32_t l = 0; l < levels; ++l)
            need += (size_t)(((w >> l ? w >> l : 1) + 3) / 4) * (((h >> l ? h >> l : 1) + 3) / 4) * blk;
        if (off + need <= size && (t = gfx_tex_create(GFX_TEX_2D, fmt, w, h, levels, GFX_USE_SAMPLE)))
            for (uint32_t l = 0; l < levels; ++l)
            {
                uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
                gfx_tex_upload(t, 0, l, f + off, (lw + 3) / 4 * blk);
                off += (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * blk;
            }
    }
    if (!t)
        rt_log("[recomp] textures: %s is not a DXT1/3/5 DDS with its levels; not used\n", path);
    free(f);
    return t;
}

/* --- texture sources ----------------------------------------------------------------------------------
 * d3d8_set_texture_source(fn): replacements drawn at run time (host/ui/vicons.c: the status icons from
 * vectors). fn sees the first level of each small texture the game uploads and may return a square
 * A8R8G8B8 image to draw with instead; it is made into a texture with its mipmaps once per distinct
 * upload (by the level's hash), and a texture pack's entry for the same texture wins. */
#define SOURCE_MAX 64 /* textures larger than this are not shown to the source */

typedef struct SourceEntry
{
    uint64_t hash;
    uint32_t w, h, format;
    GfxTex* tex; /* NULL: the source has none for it */
} SourceEntry;

static D3d8TextureSource g_source;
static SourceEntry* g_srcs;
static uint32_t g_nsrcs, g_capsrcs;

void d3d8_set_texture_source(D3d8TextureSource fn)
{
    g_source = fn;
}

/* An A8R8G8B8 image n x n (n a power of two) with its mipmaps, each the average of the four texels
 * above it weighted by their alpha, so transparent texels do not darken the edges. */
static GfxTex* source_texture(uint8_t* px, uint32_t n)
{
    uint32_t levels = 1;
    while ((n >> levels) >= 1 && levels < 16)
        levels++;
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, n, n, levels, GFX_USE_SAMPLE);
    if (!t)
        return NULL;
    uint8_t* cur = px;
    for (uint32_t l = 0, w = n; l < levels; ++l, w >>= 1)
    {
        gfx_tex_upload(t, 0, l, cur, w * 4);
        if (w == 1)
            break;
        uint32_t h = w / 2;
        uint8_t* next = (uint8_t*)malloc((size_t)h * h * 4);
        if (!next)
            break;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < h; ++x)
            {
                uint32_t c[4] = { 0, 0, 0, 0 };
                for (uint32_t k = 0; k < 4; ++k)
                {
                    const uint8_t* q = cur + (((2 * y + (k >> 1)) * w) + 2 * x + (k & 1)) * 4;
                    c[0] += q[0] * q[3], c[1] += q[1] * q[3], c[2] += q[2] * q[3], c[3] += q[3];
                }
                uint8_t* o = next + (y * h + x) * 4;
                for (int ch = 0; ch < 3; ++ch)
                    o[ch] = (uint8_t)(c[3] ? (c[ch] + c[3] / 2) / c[3] : 0);
                o[3] = (uint8_t)((c[3] + 2) / 4);
            }
        if (cur != px)
            free(cur);
        cur = next;
    }
    if (cur != px)
        free(cur);
    return t;
}

/* The replacement for a texture whose first level was just uploaded, or NULL; *pad its entry's. */
static GfxTex* texture_replacement(const Obj* t, const Obj* level0, uint32_t* pad, uint32_t* pad_min, const PackEntry** entry,
    uint8_t* mips)
{
    *pad = *pad_min = 0, *entry = NULL, *mips = 0;
    if (g_texlog < 0)
        g_texlog = getenv("FFXI_TEXLOG") && getenv("FFXI_TEXLOG")[0] == '1';
    int sourced = g_source && t->width <= SOURCE_MAX && t->height <= SOURCE_MAX;
    if (t->kind != O_TEXTURE || (!g_npack && !g_texlog && !sourced))
        return NULL;
    int sized = g_texlog || sourced;
    for (uint32_t i = 0; i < g_npack && !sized; ++i)
        sized = g_pack[i].w == t->width && g_pack[i].h == t->height;
    if (!sized)
        return NULL;
    uint32_t pitch = fmt_pitch(level0->format, level0->width), blk = fmt_block(level0->format);
    size_t n = (size_t)pitch * (blk ? (level0->height + 3) / 4 : level0->height);
    const uint8_t* p = GUEST_PTR(level0->mem);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i)
        hash = (hash ^ p[i]) * 0x100000001b3ull;
    for (uint32_t i = 0; i < g_npack; ++i)
    {
        PackEntry* e = &g_pack[i];
        if (e->hash != hash || e->w != t->width || e->h != t->height)
            continue;
        if (!e->tried)
        {
            e->tried = 1;
            e->tex = load_dds(e->path);
            if (e->tex)
                rt_log("[recomp] textures: %ux%u %016llx replaced by %s\n", t->width, t->height, (unsigned long long)hash, e->path);
        }
        *pad = e->pad, *pad_min = e->pad_min, *entry = e;
        return e->tex;
    }
    for (uint32_t i = 0; sourced && i < g_nsrcs; ++i)
    {
        const SourceEntry* e = &g_srcs[i];
        if (e->hash == hash && e->w == t->width && e->h == t->height && e->format == level0->format)
            return (*mips = e->tex != NULL), e->tex;
    }
    if (sourced)
    {
        uint32_t size = 0;
        uint8_t* px = g_source(level0->width, level0->height, level0->format, p, pitch, &size);
        GfxTex* tex = px && size && !(size & (size - 1)) ? source_texture(px, size) : NULL;
        free(px);
        if (g_nsrcs == g_capsrcs)
        {
            uint32_t cap = g_capsrcs ? g_capsrcs * 2 : 256;
            SourceEntry* grown = (SourceEntry*)realloc(g_srcs, cap * sizeof *g_srcs);
            if (grown)
                g_srcs = grown, g_capsrcs = cap;
        }
        if (g_nsrcs < g_capsrcs)
            g_srcs[g_nsrcs++] = (SourceEntry){ hash, t->width, t->height, level0->format, tex };
        if (tex)
        {
            if (g_texlog)
                rt_log("[recomp] textures: %ux%u %016llx drawn by the texture source at %ux%u\n", t->width, t->height,
                    (unsigned long long)hash, size, size);
            *mips = 1;
            return tex;
        }
    }
    if (g_texlog)
        rt_log("[recomp] textures: %ux%u format %08x hash %016llx texture %08x (no replacement)\n", t->width, t->height,
            t->format, (unsigned long long)hash, t->guest);
    if (g_texlog && getenv("FFXI_TEXDUMP")) /* ... and the level itself, to make a replacement from */
    {
        char dp[512];
        snprintf(dp, sizeof dp, "%s/%016llx.bin", getenv("FFXI_TEXDUMP"), (unsigned long long)hash);
        FILE* df = fopen(dp, "wb");
        if (df)
            fwrite(p, 1, n, df), fclose(df);
    }
    return NULL;
}

/* A texture about to be drawn with: levels written since their last upload go up first. */
static GfxTex* texture_for_draw(uint32_t p, int* kind)
{
    Obj* t = obj(p);
    if (!t || !t->gpu || (t->kind != O_TEXTURE && t->kind != O_CUBE))
        return NULL;
    t->drawn = g_ws.frame;
    if (t->dirty)
    {
        for (uint32_t i = 0; i < t->nsubs; ++i)
        {
            Obj* s = obj(t->subs[i]);
            if (s && s->dirty && s->mem && !gpu_owned(s))
            {
                gfx_tex_upload(t->gpu, s->face, s->level, GUEST_PTR(s->mem), fmt_pitch(s->format, s->width));
                if (i == 0)
                    t->repl = texture_replacement(t, s, &t->repl_pad, &t->repl_pad_min, &t->repl_entry, &t->repl_mips);
            }
            if (s)
                s->dirty = 0;
        }
        t->dirty = 0;
    }
    *kind = t->kind == O_CUBE ? 2 : 1;
    return t->repl ? t->repl : t->gpu;
}

/* --- device state ------------------------------------------------------------------------------------- */
#define NXF 280 /* transforms: D3DTS 0..23, then WORLDMATRIX(0..255) = 256..511 */
#define MAX_LIGHTS 64
#define NSTREAMS 16
#define NVSC 256
#define NPSC 8

typedef struct Light
{
    uint32_t v[26]; /* D3DLIGHT8 */
    uint32_t enabled;
} Light;

typedef struct State
{
    uint32_t rs[256];
    uint32_t tss[8][32];
    uint32_t tex[8];
    float xf[NXF][16];
    uint32_t vp[6];
    uint32_t mat[17];
    Light light[MAX_LIGHTS];
    uint32_t clip[6][4];
    uint32_t stream[NSTREAMS], stride[NSTREAMS];
    uint32_t ib, base_vertex;
    uint32_t vs, ps;
    float vsc[NVSC][4], psc[NPSC][4];
    /* the versions of what texture matrices (xf 16-23), vsc and psc hold: a new number (state_ver) at every write,
     * 0 for not known; build_draw copies them into its draw only when they are new, and passes them on to the
     * queue (GfxDraw.u_ver), which compares and sends them only then */
    uint32_t ver_texm, ver_vsc, ver_psc;
} State;

static uint32_t g_state_ver;
static uint32_t state_ver(void)
{
    if (!++g_state_ver)
        ++g_state_ver;
    return g_state_ver;
}

/* what a state block holds */
typedef struct Mask
{
    uint8_t rs[256], tss[8][32], tex[8], xf[NXF], vp, mat, light[MAX_LIGHTS], lighten[MAX_LIGHTS], clip[6],
        stream[NSTREAMS], ib, vs, ps, vsc[NVSC], psc[NPSC];
} Mask;

typedef struct Block
{
    State s;
    Mask m;
    /* the game's thread, at ApplyStateBlock (block_front_list): what its mask holds that the front follows, made at
     * its first apply (its mask is fixed once it has a token) - render states, texture stage states and textures
     * (kind << 12 | index: 0 rs, 1 tss stage*32+state, 2 tex, 3 transform); `forget` a block with too many of the
     * first three to set one by one: the front then forgets them all */
    uint16_t* fl;
    uint32_t nfl;
    uint8_t fl_ok, forget;
} Block;

/* where each input register comes from: an FVF's, or a vertex shader declaration's */
typedef struct Layout
{
    GfxElem el[GFX_NREGS];
    int32_t offset[GFX_NREGS];
    uint8_t rhw;
} Layout;

typedef struct Shader
{
    uint32_t* decl;
    uint32_t ndecl;
    uint32_t* func;
    uint32_t nfunc;
    Layout lay;    /* vertex shaders */
    uint32_t hash; /* of func: the back end's key */
} Shader;

typedef struct Dev
{
    uint32_t guest, d3d, hwnd, behavior, device_type;
    uint32_t pp[13]; /* D3DPRESENT_PARAMETERS */
    uint32_t backbuffer, depth, rt, ds;
    State cur;
    Block* rec;
    Block** blocks;
    uint32_t nblocks;
    Shader* vs;
    uint32_t nvs;
    Shader* ps;
    uint32_t nps;
    int cursor;
} Dev;

static Dev g_dev; /* the game makes one device */
/* One past the highest transform, light and vertex shader constant ever set, in any state: past
 * them every state holds the defaults, so a state block's apply or capture stops there (a
 * D3DSBT_ALL block is otherwise 280 matrices, 64 lights and 256 constants a copy). Never lowered. */
static int g_xf_top, g_light_top, g_vsc_top;

static void raise_top(int* top, uint32_t n)
{
    if ((int)n > *top)
        *top = (int)n;
}
/* A 16x16 occlusion probe tests a quad at the sky's depth (z 0x3f7ffffe, 1 - 2^-23: the game's
 * probe for the sun and its lens flare) since the last probe was read; see lock_rect. */
static int g_probe_sky;
/* ... as the game's thread sees its draws, with the worker on (defer, probe_lock_now) */
static int g_fprobe_sky;
static int g_probe_wrapped; /* d3d8_probe_test installed: render targets' unread is kept */
static int g_fade_scissor; /* fade_scissor on (d3d8_setup; FFXI_FADE_SCISSOR=0: off) */
static Obj* wr_target(void);
static int g_occlusion; /* d3d8_set_occlusion */
static uint32_t g_d3d;

static int xf_index(uint32_t ts)
{
    return ts < 24 ? (int)ts : ts >= 256 && ts < 512 ? (int)(24 + ts - 256) : -1;
}

static void default_light(Light* l)
{
    memset(l, 0, sizeof *l);
    l->v[0] = 3; /* D3DLIGHT_DIRECTIONAL */
    l->v[1] = l->v[2] = l->v[3] = f2u(1.0f);
    l->v[18] = f2u(1.0f); /* direction (0, 0, 1) */
}

static void state_defaults(State* s, uint32_t w, uint32_t h, int zbuffer)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < NXF; ++i)
        s->xf[i][0] = s->xf[i][5] = s->xf[i][10] = s->xf[i][15] = 1.0f;
    uint32_t* rs = s->rs;
    rs[7] = zbuffer ? 1 : 0;           /* ZENABLE */
    rs[8] = 3;                         /* FILLMODE solid */
    rs[9] = 2;                         /* SHADEMODE gouraud */
    rs[14] = 1;                        /* ZWRITEENABLE */
    rs[16] = 1;                        /* LASTPIXEL */
    rs[19] = 2;                        /* SRCBLEND one */
    rs[20] = 1;                        /* DESTBLEND zero */
    rs[22] = 3;                        /* CULLMODE ccw */
    rs[23] = 4;                        /* ZFUNC lessequal */
    rs[25] = 8;                        /* ALPHAFUNC always */
    rs[37] = f2u(1.0f);                /* FOGEND */
    rs[38] = f2u(1.0f);                /* FOGDENSITY */
    rs[53] = rs[54] = rs[55] = 1;      /* STENCILFAIL/ZFAIL/PASS keep */
    rs[56] = 8;                        /* STENCILFUNC always */
    rs[58] = rs[59] = 0xFFFFFFFFu;     /* STENCILMASK, STENCILWRITEMASK */
    rs[60] = 0xFFFFFFFFu;              /* TEXTUREFACTOR */
    rs[136] = 1;                       /* CLIPPING */
    rs[137] = 1;                       /* LIGHTING */
    rs[141] = 1;                       /* COLORVERTEX */
    rs[142] = 1;                       /* LOCALVIEWER */
    rs[145] = 1;                       /* DIFFUSEMATERIALSOURCE color1 */
    rs[146] = 2;                       /* SPECULARMATERIALSOURCE color2 */
    rs[154] = rs[155] = f2u(1.0f);     /* POINTSIZE, POINTSIZE_MIN */
    rs[158] = f2u(1.0f);               /* POINTSCALE_A */
    rs[162] = 0xFFFFFFFFu;             /* MULTISAMPLEMASK */
    rs[164] = f2u(1.0f);               /* PATCHSEGMENTS */
    rs[166] = f2u(64.0f);              /* POINTSIZE_MAX */
    rs[168] = 0xF;                     /* COLORWRITEENABLE */
    rs[171] = 1;                       /* BLENDOP add */
    rs[172] = 3;                       /* POSITIONORDER cubic */
    rs[173] = 1;                       /* NORMALORDER linear */
    for (uint32_t t = 0; t < 8; ++t)
    {
        uint32_t* v = s->tss[t];
        v[1] = t ? 1 : 4; /* COLOROP: disable / modulate */
        v[2] = 2;         /* COLORARG1 texture */
        v[3] = 1;         /* COLORARG2 current */
        v[4] = t ? 1 : 2; /* ALPHAOP: disable / selectarg1 */
        v[5] = 2;
        v[6] = 1;
        v[11] = t;        /* TEXCOORDINDEX */
        v[13] = v[14] = v[25] = 1; /* ADDRESSU/V/W wrap */
        v[16] = v[17] = 1;         /* MAGFILTER, MINFILTER point */
        v[21] = 1;                 /* MAXANISOTROPY */
        v[26] = v[27] = v[28] = 1; /* COLORARG0, ALPHAARG0, RESULTARG current */
    }
    s->vp[2] = w;
    s->vp[3] = h;
    s->vp[5] = f2u(1.0f);
}

/* The state a setter writes: the block being recorded, or the device. */
static State* target(Mask** m)
{
    if (g_dev.rec)
    {
        *m = &g_dev.rec->m;
        return &g_dev.rec->s;
    }
    *m = NULL;
    return &g_dev.cur;
}

/* to[i] = from[i] where m[i] (0 or 1), eight at a time: a group with none set is skipped, one with
 * all set is one copy. n is a multiple of 8. */
static void copy_masked(uint32_t* to, const uint32_t* from, const uint8_t* m, int n)
{
    for (int i = 0; i < n; i += 8)
    {
        uint64_t group;
        memcpy(&group, m + i, 8);
        if (group == 0x0101010101010101ull)
            memcpy(to + i, from + i, 32);
        else if (group)
            for (int j = i; j < i + 8; ++j)
                if (m[j])
                    to[j] = from[j];
    }
}

/* Copies the masked entries from `from` to `to` (Apply: block -> device; Capture: device -> block). */
static void state_copy(State* to, const State* from, const Mask* m)
{
    copy_masked(to->rs, from->rs, m->rs, 256);
    for (int t = 0; t < 8; ++t)
    {
        copy_masked(to->tss[t], from->tss[t], m->tss[t], 32);
        if (m->tex[t])
            bind(&to->tex[t], from->tex[t]);
    }
    for (int i = 0; i < g_xf_top; ++i)
        if (m->xf[i])
        {
            memcpy(to->xf[i], from->xf[i], 64);
            if (i >= 16 && i < 24)
                to->ver_texm = state_ver();
        }
    if (m->vp)
        memcpy(to->vp, from->vp, sizeof to->vp);
    if (m->mat)
        memcpy(to->mat, from->mat, sizeof to->mat);
    for (int i = 0; i < g_light_top; ++i)
    {
        if (m->light[i])
            memcpy(to->light[i].v, from->light[i].v, sizeof to->light[i].v);
        if (m->lighten[i])
            to->light[i].enabled = from->light[i].enabled;
    }
    for (int i = 0; i < 6; ++i)
        if (m->clip[i])
            memcpy(to->clip[i], from->clip[i], 16);
    for (int i = 0; i < NSTREAMS; ++i)
        if (m->stream[i])
        {
            bind(&to->stream[i], from->stream[i]);
            to->stride[i] = from->stride[i];
        }
    if (m->ib)
    {
        bind(&to->ib, from->ib);
        to->base_vertex = from->base_vertex;
    }
    if (m->vs)
        to->vs = from->vs;
    if (m->ps)
        to->ps = from->ps;
    for (int i = 0; i < g_vsc_top; ++i)
        if (m->vsc[i])
            memcpy(to->vsc[i], from->vsc[i], 16), to->ver_vsc = 0;
    for (int i = 0; i < NPSC; ++i)
        if (m->psc[i])
            memcpy(to->psc[i], from->psc[i], 16), to->ver_psc = 0;
    if (!to->ver_vsc)
        to->ver_vsc = state_ver();
    if (!to->ver_psc)
        to->ver_psc = state_ver();
}

static void block_free(Block* b)
{
    for (int t = 0; t < 8; ++t)
        bind(&b->s.tex[t], 0);
    for (int i = 0; i < NSTREAMS; ++i)
        bind(&b->s.stream[i], 0);
    bind(&b->s.ib, 0);
    free(b->fl);
    free(b);
}

static uint32_t block_token(Block* b)
{
    for (uint32_t i = 0; i < g_dev.nblocks; ++i)
        if (!g_dev.blocks[i])
        {
            g_dev.blocks[i] = b;
            return i + 1;
        }
    g_dev.blocks = (Block**)realloc(g_dev.blocks, (g_dev.nblocks + 1) * sizeof *g_dev.blocks);
    g_dev.blocks[g_dev.nblocks++] = b;
    return g_dev.nblocks;
}

static Block* block_of(uint32_t token)
{
    return token && token <= g_dev.nblocks ? g_dev.blocks[token - 1] : NULL;
}

/* --- IUnknown, shared by every interface ----------------------------------------------------------------- */
static void Unknown_QueryInterface(Guest* g)
{
    wr32(ARG(2), 0);
    RET(E_NOINTERFACE, 3);
}
static void Unknown_AddRef(Guest* g) { RET(obj_addref(ARG(0)), 1); }
static void Unknown_Release(Guest* g) { RET(obj_release(ARG(0)), 1); }

/* --- IDirect3D8 -------------------------------------------------------------------------------------------- */
static void desktop(uint32_t* w, uint32_t* h, uint32_t* hz)
{
    user32_desktop_mode(w, h, hz);
}

static const uint32_t MODES[][2] = { { 640, 480 },  { 800, 600 },   { 1024, 768 },  { 1152, 864 },  { 1280, 720 },
                                     { 1280, 800 }, { 1280, 1024 }, { 1366, 768 },  { 1440, 900 },  { 1600, 900 },
                                     { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 } };

/* the display modes offered: the list up to the desktop's size, then the desktop, in both formats */
static uint32_t modes(uint32_t (*out)[4], uint32_t max)
{
    uint32_t dw, dh, hz, n = 0;
    desktop(&dw, &dh, &hz);
    const uint32_t fmts[2] = { FMT_X8R8G8B8, FMT_R5G6B5 };
    for (int f = 0; f < 2; ++f)
    {
        int have_desktop = 0;
        for (size_t i = 0; i < sizeof MODES / sizeof MODES[0]; ++i)
            if (MODES[i][0] <= dw && MODES[i][1] <= dh && n < max)
            {
                have_desktop |= MODES[i][0] == dw && MODES[i][1] == dh;
                out[n][0] = MODES[i][0], out[n][1] = MODES[i][1], out[n][2] = hz, out[n][3] = fmts[f], n++;
            }
        if (!have_desktop && n < max)
            out[n][0] = dw, out[n][1] = dh, out[n][2] = hz, out[n][3] = fmts[f], n++;
    }
    return n;
}

static void IDirect3D8_GetAdapterCount(Guest* g) { RET(1, 1); }

static void IDirect3D8_GetAdapterIdentifier(Guest* g)
{
    if (ARG(1))
        RET(D3DERR_INVALIDCALL, 4);
    uint32_t p = ARG(3);
    memset(GUEST_PTR(p), 0, 1068);
    strcpy((char*)GUEST_PTR(p), "ffxi-metal");
    strcpy((char*)GUEST_PTR(p + 512), "FFXI Metal");
    wr32(p + 1024, 0x00010000u); /* DriverVersion 1.0.0.0 */
    wr32(p + 1028, 0x00060000u);
    wr32(p + 1032, 0x106B); /* VendorId: Apple */
    wr32(p + 1064, 1);      /* WHQLLevel */
    RET(D3D_OK, 4);
}

static void IDirect3D8_GetAdapterModeCount(Guest* g)
{
    uint32_t m[64][4];
    RET(ARG(1) ? 0 : modes(m, 64), 2);
}

static void IDirect3D8_EnumAdapterModes(Guest* g)
{
    uint32_t m[64][4], n = modes(m, 64), i = ARG(2);
    if (ARG(1) || i >= n)
        RET(D3DERR_INVALIDCALL, 4);
    for (int k = 0; k < 4; ++k)
        wr32(ARG(3) + 4u * (uint32_t)k, m[i][k]);
    RET(D3D_OK, 4);
}

static void write_desktop_mode(uint32_t p)
{
    uint32_t w, h, hz;
    desktop(&w, &h, &hz);
    wr32(p, w);
    wr32(p + 4, h);
    wr32(p + 8, hz);
    wr32(p + 12, FMT_X8R8G8B8);
}

static void IDirect3D8_GetAdapterDisplayMode(Guest* g)
{
    if (ARG(1))
        RET(D3DERR_INVALIDCALL, 3);
    write_desktop_mode(ARG(2));
    RET(D3D_OK, 3);
}

static void IDirect3D8_CheckDeviceType(Guest* g) { RET(ARG(1) ? D3DERR_INVALIDCALL : D3D_OK, 6); }

/* What a Metal device can back. Paletted and 3-3-2 formats are out, as on current D3D9 drivers. */
static int format_ok(uint32_t usage, uint32_t rtype, uint32_t f)
{
    if (rtype == RT_VERTEXBUFFER || rtype == RT_INDEXBUFFER)
        return 1;
    if (rtype == RT_VOLUMETEXTURE)
        return 0;
    if (usage & USAGE_DEPTHSTENCIL)
        return f == FMT_D16 || f == FMT_D24S8 || f == FMT_D24X8;
    if (usage & USAGE_RENDERTARGET)
        return f == FMT_A8R8G8B8 || f == FMT_X8R8G8B8 || f == FMT_R5G6B5 || f == FMT_X1R5G5B5 || f == FMT_A1R5G5B5;
    switch (f)
    {
    case FMT_A8R8G8B8:
    case FMT_X8R8G8B8:
    case FMT_R5G6B5:
    case FMT_X1R5G5B5:
    case FMT_A1R5G5B5:
    case FMT_A4R4G4B4:
    case FMT_A8:
    case FMT_L8:
    case FMT_A8L8:
    case FMT_V8U8:
    case FMT_DXT1:
    case FMT_DXT2:
    case FMT_DXT3:
    case FMT_DXT4:
    case FMT_DXT5:
        return 1;
    case FMT_D16:
    case FMT_D24S8:
    case FMT_D24X8:
        return rtype == RT_SURFACE || rtype == RT_TEXTURE;
    default: return 0;
    }
}

static void IDirect3D8_CheckDeviceFormat(Guest* g)
{
    RET(!ARG(1) && format_ok(ARG(4), ARG(5), ARG(6)) ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}

static void IDirect3D8_CheckDeviceMultiSampleType(Guest* g) { RET(ARG(5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE, 6); }
static void IDirect3D8_CheckDepthStencilMatch(Guest* g) { RET(D3D_OK, 6); }

static void IDirect3D8_GetDeviceCaps(Guest* g)
{
    if (ARG(1))
        RET(D3DERR_INVALIDCALL, 4);
    for (int i = 0; i < 53; ++i)
        wr32(ARG(3) + 4u * (uint32_t)i, CAPS8[i]);
    wr32(ARG(3), ARG(2)); /* DeviceType as asked */
    RET(D3D_OK, 4);
}

static void IDirect3D8_GetAdapterMonitor(Guest* g) { RET(0x00050001u, 2); }

/* CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice) */
static void IDirect3D8_CreateDevice(Guest* g)
{
    memset(g_fxf_ok, 0, sizeof g_fxf_ok);
    memset(&g_fs, 0, sizeof g_fs);
    memset(&g_fm, 0, sizeof g_fm);
    wr32(ARG(6), 0);
    if (ARG(1) || g_dev.guest)
        RET(g_dev.guest ? D3DERR_NOTAVAILABLE : D3DERR_INVALIDCALL, 7);
    Dev* d = &g_dev;
    for (int i = 0; i < 13; ++i)
        d->pp[i] = rd32(ARG(5) + 4u * (uint32_t)i);
    d->hwnd = d->pp[6] ? d->pp[6] : ARG(3);
    d->behavior = ARG(4);
    d->device_type = ARG(2);
    d->d3d = ARG(0);
    if (!d->pp[0] || !d->pp[1])
    {
        uint32_t w = 640, h = 480;
        user32_client_size(d->hwnd, &w, &h);
        if (!d->pp[0])
            d->pp[0] = w;
        if (!d->pp[1])
            d->pp[1] = h;
    }
    if (!d->pp[2])
        d->pp[2] = FMT_X8R8G8B8;
    /* windowed devices ignore the presentation interval and wait for the display; so do we unless
     * a full-screen device asks for IMMEDIATE */
    user32_set_fullscreen(d->hwnd, !d->pp[7]);
    /* FFXI_VSYNC=0: never wait for the display (the Android host sets it, to measure past the refresh rate) */
    int vsync = d->pp[7] || d->pp[12] != 0x80000000u;
    if (getenv("FFXI_VSYNC") && getenv("FFXI_VSYNC")[0] == '0')
        vsync = 0;
    gfx_init(user32_sdl_window(d->hwnd), vsync);
    obj_addref(d->d3d);
    d->guest = obj_new(O_DEVICE);
    d->backbuffer = new_surface(d->pp[2], d->pp[0], d->pp[1], USAGE_RENDERTARGET, 0, 0);
    obj(d->backbuffer)->implicit = 1;
    if (d->pp[8])
    {
        d->depth = new_surface(d->pp[9], d->pp[0], d->pp[1], USAGE_DEPTHSTENCIL, 0, 0);
        obj(d->depth)->implicit = 1;
    }
    bind(&d->rt, d->backbuffer);
    bind(&d->ds, d->depth);
    state_defaults(&d->cur, d->pp[0], d->pp[1], d->pp[8] != 0);
    rt_log("[recomp] d3d8: device %ux%u format %u%s, depth %u, behavior %08x, window %08x\n", d->pp[0], d->pp[1],
        d->pp[2], d->pp[7] ? " windowed" : " fullscreen", d->pp[8] ? d->pp[9] : 0, d->behavior, d->hwnd);
    wr32(ARG(6), d->guest);
    RET(D3D_OK, 7);
}

/* --- IDirect3DDevice8: device -------------------------------------------------------------------------- */
static void IDirect3DDevice8_TestCooperativeLevel(Guest* g) { RET(D3D_OK, 1); }
static void IDirect3DDevice8_GetAvailableTextureMem(Guest* g) { RET(512u << 20, 1); }
static void IDirect3DDevice8_ResourceManagerDiscardBytes(Guest* g) { RET(D3D_OK, 2); }

static void IDirect3DDevice8_GetDirect3D(Guest* g)
{
    obj_addref(g_dev.d3d);
    wr32(ARG(1), g_dev.d3d);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetDeviceCaps(Guest* g)
{
    for (int i = 0; i < 53; ++i)
        wr32(ARG(1) + 4u * (uint32_t)i, CAPS8[i]);
    wr32(ARG(1), g_dev.device_type);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetDisplayMode(Guest* g)
{
    write_desktop_mode(ARG(1));
    if (!g_dev.pp[7]) /* fullscreen: the mode the device set */
    {
        wr32(ARG(1), g_dev.pp[0]);
        wr32(ARG(1) + 4, g_dev.pp[1]);
        wr32(ARG(1) + 12, g_dev.pp[2]);
    }
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetCreationParameters(Guest* g)
{
    wr32(ARG(1), 0);
    wr32(ARG(1) + 4, g_dev.device_type);
    wr32(ARG(1) + 8, g_dev.hwnd);
    wr32(ARG(1) + 12, g_dev.behavior);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetCursorProperties(Guest* g) { RET(D3D_OK, 4); }
static void IDirect3DDevice8_SetCursorPosition(Guest* g) { RET(0, 4); }

static void IDirect3DDevice8_ShowCursor(Guest* g)
{
    int was = g_dev.cursor;
    g_dev.cursor = ARG(1) != 0;
    RET((uint32_t)was, 2);
}

static void IDirect3DDevice8_Reset(Guest* g)
{
    g_ws.resets++;
    memset(g_fxf_ok, 0, sizeof g_fxf_ok);
    memset(&g_fs, 0, sizeof g_fs);
    memset(&g_fm, 0, sizeof g_fm);
    Dev* d = &g_dev;
    for (int i = 0; i < 13; ++i)
        d->pp[i] = rd32(ARG(1) + 4u * (uint32_t)i);
    if (!d->pp[0] || !d->pp[1])
        user32_client_size(d->hwnd, &d->pp[0], &d->pp[1]);
    if (!d->pp[2])
        d->pp[2] = FMT_X8R8G8B8;
    user32_set_fullscreen(d->hwnd, !d->pp[7]);
    Obj* bb = obj(d->backbuffer);
    bb->format = d->pp[2], bb->width = d->pp[0], bb->height = d->pp[1], bb->size = fmt_size(bb->format, bb->width, bb->height);
    if (bb->mem)
        gheap_free(bb->mem), bb->mem = 0;
    gfx_tex_destroy(bb->gpu);
    bb->gpu = gfx_tex_create(GFX_TEX_2D, bb->format, bb->width, bb->height, 1, GFX_USE_RT);
    Obj* z = obj(d->depth);
    if (z)
    {
        z->width = d->pp[0], z->height = d->pp[1], z->size = fmt_size(z->format, z->width, z->height);
        if (z->mem)
            gheap_free(z->mem), z->mem = 0;
        gfx_tex_destroy(z->gpu);
        z->gpu = gfx_tex_create(GFX_TEX_2D, z->format, z->width, z->height, 1, GFX_USE_DEPTH);
    }
    bind(&d->rt, d->backbuffer);
    bind(&d->ds, d->depth);
    for (int t = 0; t < 8; ++t)
        bind(&d->cur.tex[t], 0);
    for (int i = 0; i < NSTREAMS; ++i)
        bind(&d->cur.stream[i], 0);
    bind(&d->cur.ib, 0);
    state_defaults(&d->cur, d->pp[0], d->pp[1], d->pp[8] != 0);
    rt_log("[recomp] d3d8: reset to %ux%u\n", d->pp[0], d->pp[1]);
    RET(D3D_OK, 2);
}

static int async_readback(void)
{
    static int async = -1;
    if (async < 0)
        async = getenv("FFXI_ASYNC_READBACK") && getenv("FFXI_ASYNC_READBACK")[0] == '1';
    return async;
}

/* the occlusion mode FFXI_PROBE forces on the probes' reads (lock_rect), 0 for none */
static int probe_forced(void)
{
    static int forced = -1;
    if (forced < 0)
    {
        int gpu = getenv("FFXI_PROBE") && !strcmp(getenv("FFXI_PROBE"), "gpu");
#if defined(FFXI_ANDROID_VULKAN)
        forced = async_readback() ? 1 : gpu ? 2 : 0;
#else
        forced = gpu ? (async_readback() ? 1 : 2) : 0;
#endif
    }
    return forced;
}

/* The game's occlusion test of a character's screen rectangle (host64.c wraps it where the build is known): it
 * draws the rectangle black and then white where the scene's depth is behind the character, into a 4096x4096
 * scratch target, samples that into 16x16 and reads the copy back to decide whether the name is drawn. Occlusion
 * 0 answers that read fully visible without the GPU (lock_rect), so while it runs its draws, clears and copies
 * are dropped: up to 12 a frame, each two quads of as much as 3,000x3,000 pixels. The game's code still runs,
 * its state and answer as before. Not the sun flare's probe (character 0), whose answer is read. */
void d3d8_probe_test(Guest* g, GuestFn orig, int character)
{
    g_probe_wrapped = 1;
    int skip = character && g_occlusion == 0 && !probe_forced();
    if (defer_on())
    {
        /* (the draws it is for are run later, on the worker: the flag goes with them) */
        memcpy(d3dw_rec(W_GPU_SKIP, sizeof skip), &skip, sizeof skip), d3dw_done();
        orig(g);
        skip = 0;
        memcpy(d3dw_rec(W_GPU_SKIP, sizeof skip), &skip, sizeof skip), d3dw_done();
        return;
    }
    g_gpu_skip = skip;
    g_gpu_skip_seen = 0;
    orig(g);
    g_gpu_skip = 0;
}

void d3d8_probe_wrapped(void) { g_probe_wrapped = 1; }

/* Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion): the frame is done */
static void (*g_present_hook)(void);

void d3d8_set_present_hook(void (*fn)(void)) { g_present_hook = fn; }

/* the scene renderer (host/scene.c): its batch goes out before anything that comes after it draws, clears,
 * changes the targets, copies from them or shows them */
static void (*g_world_flush)(void);
void d3d8_set_world_flush(void (*fn)(void)) { g_world_flush = fn; }
/* the game's thread's: the batch sent before the call after it, with the worker on (d3d8.h) */
static void (*g_front_flush)(void);
static int g_front_pending;
void d3d8_set_world_front_flush(void (*fn)(void)) { g_front_flush = fn; }
int d3d8_world_front(void) { return g_w.threaded && g_front_flush; }
void d3d8_world_front_mark(void) { g_front_pending = 1; }
#define FRONT_FLUSH()                                                                                                 \
    do                                                                                                                \
    {                                                                                                                 \
        if (g_front_pending)                                                                                          \
            g_front_pending = 0, g_front_flush();                                                                     \
    } while (0)
/* the game's own draws inside its zone walk and its character mesh draws (what the scene renderer leaves to the game)
 * don't send the batch out: told apart by where they return to (d3d8_world_ranges: the drawing functions' code).
 * Timing the game's own draws of those for comparison (scene = -1): each run of them goes into a timed pass of its own
 * (gfx_world_time), which the next other piece of work ends */
static uint32_t g_world_range[2][2];
static int g_world_time_on, g_world_timing, g_draw_zone; /* 0 other, 1 the zone's, 2 a character's */
void d3d8_world_ranges(uint32_t zone_lo, uint32_t zone_hi, uint32_t char_lo, uint32_t char_hi, int time)
{
    g_world_range[0][0] = zone_lo, g_world_range[0][1] = zone_hi, g_world_range[1][0] = char_lo, g_world_range[1][1] = char_hi;
    g_world_time_on = time && (zone_lo || char_lo);
}

/* the game's DrawPrimitive and DrawIndexedPrimitive wrappers (1000cb20, 1000cc00 in 2026-09-03) leave their caller's
 * return 0x20 and 0x28 up the stack at the device's call */
static int world_site(Guest* g)
{
    for (int k = 0; k < 2; ++k)
    {
        uint32_t lo = g_world_range[k][0] + RD, hi = g_world_range[k][1] + RD;
        if (!g_world_range[k][0])
            continue;
        uint32_t a = rd32(g->esp + 0x20), b = rd32(g->esp + 0x28);
        if ((a >= lo && a < hi) || (b >= lo && b < hi))
            return k + 1;
    }
    return 0;
}

static void world_draw_begin(Guest* g)
{
    g_draw_zone = world_site(g);
    if (g_world_timing && g_draw_zone != g_world_timing)
        gfx_world_time(0), g_world_timing = 0;
    if (g_draw_zone && g_world_time_on && !g_world_timing)
        gfx_world_time(g_draw_zone), g_world_timing = g_draw_zone;
}
#define WORLD_FLUSH() \
    do \
    { \
        if (g_world_flush && !g_draw_zone) \
            g_world_flush(); \
        if (g_world_timing && !g_draw_zone) \
            gfx_world_time(0), g_world_timing = 0; \
    } while (0)

void d3d8_set_occlusion(int mode)
{
#if defined(FFXI_ANDROID_VULKAN)
    g_occlusion = mode >= 0 && mode <= 2 ? mode : 0;
#else
    g_occlusion = mode == 2 ? 2 : 0; /* Delayed is Android's alone (lock_rect); FFXI_PROBE still reaches it */
#endif
}
int d3d8_occlusion(void) { return g_occlusion; }

void d3d8_screen_size(uint32_t* w, uint32_t* h)
{
    /* (no wait for the worker: pp is set only by CreateDevice and Reset, which wait - asked ~19 times a frame) */
    *w = g_dev.pp[0], *h = g_dev.pp[1];
    if (g_dev.hwnd)
        user32_client_size(g_dev.hwnd, w, h);
}

static void (*g_overlay)(GfxTex* backbuffer, uint32_t w, uint32_t h); /* d3d8_set_overlay */
static void ui_present(void);
static void apply_targets(void);
static void (*g_after_present)(void);
static int native_size(uint32_t w, uint32_t h, uint32_t* pw, uint32_t* ph);

static char g_capture_path[1024];
static uint32_t g_capture_serial, g_capture_w, g_capture_h, g_capture_fmt;
static int g_capture_ok;

void d3d8_capture(const char* path) { snprintf(g_capture_path, sizeof g_capture_path, "%s", path ? path : ""); }

uint32_t d3d8_capture_result(uint32_t* w, uint32_t* h, uint32_t* format, int* ok)
{
    d3dw_sync_at("d3d8_capture_result"); /* (the device's state is the worker's while it runs) */
    *w = g_capture_w, *h = g_capture_h, *format = g_capture_fmt, *ok = g_capture_ok;
    return g_capture_serial;
}

static void capture_frame(Obj* bb)
{
    g_capture_ok = 0;
    if (bb && bb->gpu && !fmt_block(bb->format))
    {
        uint32_t pitch = fmt_pitch(bb->format, bb->width);
        uint8_t* px = (uint8_t*)malloc((size_t)pitch * bb->height);
        FILE* f = px ? fopen(g_capture_path, "wb") : NULL;
        if (f)
        {
            gfx_tex_read(bb->gpu, 0, 0, px, pitch);
            uint32_t head[4] = { 0x31464958u /* "XIF1" */, bb->width, bb->height, bb->format };
            g_capture_ok = fwrite(head, sizeof head, 1, f) == 1 && fwrite(px, (size_t)pitch * bb->height, 1, f) == 1;
            g_capture_ok &= fclose(f) == 0;
            g_capture_w = bb->width, g_capture_h = bb->height, g_capture_fmt = bb->format;
        }
        free(px);
    }
    if (!g_capture_ok)
        rt_log("[recomp] d3d8: frame capture to %s failed\n", g_capture_path);
    g_capture_path[0] = 0;
    g_capture_serial++;
}

static void d3dw_check_buffers(void);

/* Present's d3d8 half: the frame's end for d3d8's own passes, the overlay, and the render queue's present. On the
 * worker with it on (F_Present), after the host's hook has run on the game's thread. */
static void present_back(Guest* g)
{
    WORLD_FLUSH();
    cap_present();
    scene_present();
    ui_present();
    Obj* bb = obj(g_dev.backbuffer);
    if (g_overlay && bb && bb->gpu)
        g_overlay(bb->gpu, bb->width, bb->height);
    if (g_capture_path[0])
        capture_frame(bb);
    gfx_present(bb ? bb->gpu : NULL);
    plat_atomic_add32(&g_w.presents_done, 1);
    plat_wake_all32(&g_w.presents_done);
    RET(D3D_OK, 5);
}

static void IDirect3DDevice8_Present(Guest* g)
{
    d3dw_check_buffers(); /* (the worker idle: Present waits for it) */
    ws_frame();
    WORLD_FLUSH();
    water_flush();
    fixes_frame();
    dis_present();
    g_btn_shadows = 0, g_halo_src = g_halo_low = 0;
    cap_present();
    scene_present();
    ui_present();
    if (g_present_hook)
        g_present_hook();
    Obj* bb = obj(g_dev.backbuffer);
    if (g_overlay && bb && bb->gpu)
        g_overlay(bb->gpu, bb->width, bb->height);
    if (g_capture_path[0])
        capture_frame(bb);
    gfx_present(bb ? bb->gpu : NULL);
    if (g_after_present)
        g_after_present();
    RET(D3D_OK, 5);
}

void d3d8_set_after_present(void (*fn)(void)) { g_after_present = fn; }

/* A host function that calls the render queue each frame (the sun's shadow focus, the weather): on the worker with it
 * on, in order with the frame's draws - on the game's thread its queue call waited for the worker to finish all it
 * had (gfxq_record_hook) - else at once. data is copied. From the game's threads, under the guest lock. */
void d3d8_host_call(void (*fn)(const void* data), const void* data, uint32_t bytes)
{
    if (g_w.threaded && !d3dw_on_worker() && bytes <= (4u << 20)) /* (the characters' batch: ~100 KB in a town) */
    {
        WHost* h = (WHost*)d3dw_rec(W_HOST, (uint32_t)sizeof *h + bytes);
        h->fn = fn, h->n = bytes, h->pad = 0;
        memcpy(h + 1, data, bytes);
        d3dw_done();
        return;
    }
    fn(data);
}

void d3d8_set_overlay(void (*fn)(GfxTex* backbuffer, uint32_t w, uint32_t h)) { g_overlay = fn; }

void d3d8_backbuffer_size(uint32_t* w, uint32_t* h)
{
    d3dw_sync_at("d3d8_backbuffer_size"); /* (the device's state is the worker's while it runs) */
    Obj* bb = obj(g_dev.backbuffer);
    *w = bb ? bb->width : 0;
    *h = bb ? bb->height : 0;
}

uint32_t d3d8_window(void) { return g_dev.hwnd; }

/* the game's thread with the worker on: what it knows of the device's state (g_fxf, g_fm) is the state as the game set
 * it, which the worker reaches only later; what it doesn't know waits for the worker */
static int front_side(void) { return g_w.threaded && !d3dw_on_worker() && !g_dev.rec; }

int d3d8_get_transform(uint32_t ts, float m[16])
{
    int i = xf_index(ts);
    if (front_side() && i >= 0 && g_fxf_ok[i] && g_dev.guest)
    {
        memcpy(m, g_fxf[i], 16 * sizeof(float));
        return 1;
    }
    d3dw_sync_at("d3d8_get_transform"); /* (the device's state is the worker's while it runs) */
    if (!g_dev.guest || i < 0)
        return 0;
    memcpy(m, g_dev.cur.xf[i], 16 * sizeof(float));
    return 1;
}

int d3d8_get_render_state(uint32_t rs, uint32_t* v)
{
    if (front_side() && rs < 256 && g_fm.rs_ok[rs] && g_dev.guest)
    {
        *v = g_fm.rs[rs];
        return 1;
    }
    d3dw_sync_at("d3d8_get_render_state"); /* (the device's state is the worker's while it runs) */
    if (!g_dev.guest || rs >= 256)
        return 0;
    *v = g_dev.cur.rs[rs];
    if (front_side()) /* (the worker idle: known from now on, d3d8_world_sampler_front's) */
        g_fm.rs[rs] = *v, g_fm.rs_ok[rs] = 1;
    return 1;
}

/* stage 0's sampler state as the game's thread knows it (d3d8_world_sampler's, without the texture's part) */
int d3d8_world_sampler_front(GfxSampler* out)
{
    static const uint8_t used[] = { 13, 14, 15, 16, 17, 18, 20, 21, 25 };
    uint32_t t[32] = { 0 };
    int known = front_side();
    for (size_t k = 0; known && k < sizeof used; ++k)
        if (g_fm.tss_ok[0][used[k]])
            t[used[k]] = g_fm.tss[0][used[k]];
        else
            known = 0;
    if (!known)
    {
        d3dw_sync_at("d3d8_world_sampler_front");
        memcpy(t, g_dev.cur.tss[0], sizeof t);
        /* (the worker idle: the device's state is all the game has set - known here from now on, as a set would make
         * it; states never set since the device was made were never known, and waited every time) */
        if (front_side())
            for (size_t k = 0; k < sizeof used; ++k)
                g_fm.tss[0][used[k]] = t[used[k]], g_fm.tss_ok[0][used[k]] = 1;
    }
    *out = (GfxSampler){ (uint8_t)t[13], (uint8_t)t[14], (uint8_t)t[25], (uint8_t)t[16], (uint8_t)t[17], (uint8_t)t[18],
        (uint8_t)t[21], (uint8_t)t[20], 0, { 0 }, t[15] };
    return 1;
}

/* ... and the texture's part, where the texture is known (the worker) */
void d3d8_world_sampler_fix(uint32_t iface, GfxSampler* s)
{
    Obj* to = obj(iface);
    if (!to)
        return;
    if (to->lod > s->max_level)
        s->max_level = (uint8_t)to->lod;
    if (to->repl) /* a texture pack's: as build_draw draws it */
        s->mag = s->min = s->mip = 2, s->max_level = 0, s->lod_cap = to->repl_mips ? 0 : 2;
}

/* an index buffer's size and index size: set only when it is made (never by the worker) */
int d3d8_world_ibuf_info(uint32_t iface, uint32_t* size, uint32_t* index_size)
{
    Obj* b = obj(iface);
    if (!b || b->kind != O_IB)
        return 0;
    *size = b->size;
    *index_size = b->format == FMT_INDEX32 ? 4 : 2;
    return 1;
}

/* The back buffer (and its depth) at a new size, the game's objects kept: its next frame draws into
 * them at once. The render targets drawn at the screen's resolution (native_size) follow it. Between
 * frames (d3d8_set_after_present); the game's own copies of the size are the host's to change. */
int d3d8_resize(uint32_t w, uint32_t h)
{
    d3dw_sync_at("d3d8_resize"); /* (the device's state is the worker's while it runs) */
    Dev* d = &g_dev;
    Obj* bb = obj(d->backbuffer);
    if (!d->guest || !bb || w < 64 || h < 64)
        return 0;
    if (w == d->pp[0] && h == d->pp[1])
        return 1;
    d->pp[0] = w, d->pp[1] = h;
    bb->width = w, bb->height = h, bb->size = fmt_size(bb->format, w, h);
    if (bb->mem)
        gheap_free(bb->mem), bb->mem = 0;
    scene_forget(bb->gpu);
    gfx_tex_destroy(bb->gpu);
    bb->gpu = gfx_tex_create(GFX_TEX_2D, bb->format, w, h, 1, GFX_USE_RT);
    Obj* z = obj(d->depth);
    if (z)
    {
        z->width = w, z->height = h, z->size = fmt_size(z->format, w, h);
        if (z->mem)
            gheap_free(z->mem), z->mem = 0;
        scene_forget(z->gpu);
        gfx_tex_destroy(z->gpu);
        z->gpu = gfx_tex_create(GFX_TEX_2D, z->format, w, h, 1, GFX_USE_DEPTH);
    }
    /* the interface's targets: drawn at the new screen size (their contents are drawn again each frame) */
    for (uint32_t i = 1; i < g_nobjs; ++i)
    {
        Obj* t = OBJ_AT(i);
        if (t->kind != O_TEXTURE || !t->pw)
            continue;
        uint32_t pw = 0, ph = 0;
        if (!native_size(t->width, t->height, &pw, &ph))
            pw = t->width, ph = t->height;
        if (pw == t->pw && ph == t->ph)
            continue;
        scene_forget(t->gpu);
        gfx_tex_destroy(t->gpu);
        t->pw = pw == t->width && ph == t->height ? 0 : pw, t->ph = t->pw ? ph : 0;
        t->gpu = gfx_tex_create(GFX_TEX_2D, t->format, pw, ph, 1, GFX_USE_RT);
    }
    apply_targets();
    rt_log("[recomp] d3d8: back buffer now %ux%u\n", w, h);
    return 1;
}

static void IDirect3DDevice8_GetBackBuffer(Guest* g)
{
    if (ARG(1))
    {
        wr32(ARG(3), 0);
        RET(D3DERR_INVALIDCALL, 4);
    }
    obj_addref(g_dev.backbuffer);
    wr32(ARG(3), g_dev.backbuffer);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_GetRasterStatus(Guest* g)
{
    wr32(ARG(1), 1); /* InVBlank */
    wr32(ARG(1) + 4, 0);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetGammaRamp(Guest* g) { RET(0, 3); }

/* --- resources ------------------------------------------------------------------------------------------ */
static uint32_t chain_levels(uint32_t w, uint32_t h, uint32_t levels)
{
    uint32_t full = 1;
    for (uint32_t m = w > h ? w : h; m > 1; m >>= 1)
        full++;
    return !levels || levels > full ? full : levels;
}

/* FFXI draws its interface into a render-target texture of the menu resolution (the registry's
 * 0037 x 0038: half the window, say) and stretches that over the back buffer, so its text is drawn at
 * half the screen's resolution and doubled. A render-target texture smaller than the back buffer and
 * of its shape is instead given a GPU texture of the back buffer's size: the game still sees its own
 * size (locks, descriptions, XYZRHW coordinates), and its draws land on the larger texture through a
 * viewport scaled to match (scale_to_target), so the interface is drawn at the screen's resolution
 * and the final stretch is one to one. FFXI_UI_NATIVE=0 draws it at the menu resolution again. */
/* w x h is smaller than the screen and of the --ui-aspect box's shape (1280x720 for 16:9, 1px either
 * way): narrower than the screen in a wider window, shorter in a taller one */
static int ui_box_shape(uint32_t w, uint32_t h)
{
    uint32_t bw = g_dev.pp[0], bh = g_dev.pp[1];
    float s = user32_ui_squeeze(g_dev.hwnd), sy = user32_ui_squeeze_y(g_dev.hwnd);
    if ((s >= 1.0f && sy >= 1.0f) || !h || w >= bw || h >= bh)
        return 0;
    double boxw = bw * (double)s, boxh = bh * (double)sy;
    return fabs((double)w * boxh - boxw * h) <= boxh;
}

static int native_size(uint32_t w, uint32_t h, uint32_t* pw, uint32_t* ph)
{
    static int on = -1;
    if (on < 0)
        on = !(getenv("FFXI_UI_NATIVE") && getenv("FFXI_UI_NATIVE")[0] == '0');
    uint32_t bw = g_dev.pp[0], bh = g_dev.pp[1];
    if (!on || !bw || !bh || w >= bw || h >= bh)
        return 0;
    if ((uint64_t)w * bh == (uint64_t)h * bw)
    {
        *pw = bw, *ph = bh;
        return 1;
    }
    /* under --ui-aspect the menu target is of the interface box's shape; it still covers the screen,
     * its draws squeezed toward the middle (ui_squeeze) */
    if (!ui_box_shape(w, h))
        return 0;
    *pw = bw, *ph = bh;
    return 1;
}

/* a surface of a texture native_size enlarged */
static int surface_scaled(const Obj* s)
{
    Obj* t = s->container ? obj(s->container) : NULL;
    return t && t->pw;
}

/* A viewport (x, y, w, h first) or D3DRECTs from the game's pixels to the current target's. */
static void scale_to_target(uint32_t* vp, int32_t* rects, uint32_t nrects)
{
    Obj* rt = obj(g_dev.rt);
    Obj* t = rt && rt->container ? obj(rt->container) : NULL;
    if (!t || !t->pw)
        return;
    double kx = (double)t->pw / t->width, ky = (double)t->ph / t->height;
    if (vp)
        vp[0] = (uint32_t)(vp[0] * kx + 0.5), vp[1] = (uint32_t)(vp[1] * ky + 0.5), vp[2] = (uint32_t)(vp[2] * kx + 0.5),
        vp[3] = (uint32_t)(vp[3] * ky + 0.5);
    for (uint32_t i = 0; i < nrects; ++i)
    {
        int32_t* r = rects + 4 * i;
        r[0] = (int32_t)(r[0] * kx + 0.5), r[1] = (int32_t)(r[1] * ky + 0.5), r[2] = (int32_t)(r[2] * kx + 0.5),
        r[3] = (int32_t)(r[3] * ky + 0.5);
    }
}

static uint32_t new_texture(int kind, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool)
{
    uint32_t p = obj_new(kind);
    Obj* t = obj(p);
    t->width = w, t->height = h, t->usage = usage, t->format = fmt, t->pool = pool;
    uint32_t nl = t->levels = chain_levels(w, h, levels);
    uint32_t faces = kind == O_CUBE ? 6 : 1;
    uint32_t* subs = t->subs = (uint32_t*)calloc(faces * nl, 4);
    t->nsubs = faces * nl;
    /* new_surface may grow the object table: t is not used past here */
    for (uint32_t f = 0; f < faces; ++f)
        for (uint32_t l = 0; l < nl; ++l)
        {
            uint32_t sp = new_surface(fmt, w >> l ? w >> l : 1, h >> l ? h >> l : 1, usage, pool, p);
            subs[f * nl + l] = sp;
            obj(sp)->face = (uint8_t)f, obj(sp)->level = (uint8_t)l;
        }
    uint32_t pw = 0, ph = 0;
    if (kind == O_TEXTURE && (usage & USAGE_RENDERTARGET) && nl == 1 && pool != 2 && native_size(w, h, &pw, &ph))
    {
        obj(p)->pw = pw, obj(p)->ph = ph;
        rt_log("[recomp] d3d8: %ux%u render target drawn at %ux%u\n", w, h, pw, ph);
    }
    if (pool != 2) /* system memory textures are only ever copied from */
        obj(p)->gpu = gfx_tex_create(kind == O_CUBE ? GFX_TEX_CUBE : GFX_TEX_2D, fmt, pw ? pw : w, ph ? ph : h, nl,
            (usage & USAGE_RENDERTARGET) ? GFX_USE_RT : GFX_USE_SAMPLE);
    return p;
}

/* CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture) */
static void IDirect3DDevice8_CreateTexture(Guest* g)
{
    wr32(ARG(7), 0);
    if (!ARG(1) || !ARG(2) || !format_ok(ARG(4), RT_TEXTURE, ARG(5)))
        RET(D3DERR_INVALIDCALL, 8);
    wr32(ARG(7), new_texture(O_TEXTURE, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6)));
    RET(D3D_OK, 8);
}

/* CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture) */
static void IDirect3DDevice8_CreateCubeTexture(Guest* g)
{
    wr32(ARG(6), 0);
    if (!ARG(1) || !format_ok(ARG(3), RT_CUBETEXTURE, ARG(4)))
        RET(D3DERR_INVALIDCALL, 7);
    wr32(ARG(6), new_texture(O_CUBE, ARG(1), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)));
    RET(D3D_OK, 7);
}

static void IDirect3DDevice8_CreateVolumeTexture(Guest* g)
{
    wr32(ARG(8), 0);
    RET(D3DERR_NOTAVAILABLE, 9);
}

#define USAGE_DYNAMIC 0x200u

/* A buffer the game does not declare dynamic is written once and drawn many times: it gets a GPU
 * copy, refreshed at the next draw after a lock that writes. Dynamic ones are copied per draw. */
static void static_buffer_init(Obj* b)
{
    if (!(b->usage & USAGE_DYNAMIC) && b->size)
    {
        b->gbuf = gfx_buf_create(b->size);
        b->gbuf_dirty = 1;
    }
}

static GfxBuf* static_buffer(Obj* b)
{
    if (!b->gbuf || !b->mem)
        return NULL;
    if (b->gbuf_dirty)
    {
        gfx_buf_upload(b->gbuf, GUEST_PTR(b->mem), b->size);
        b->gbuf_dirty = 0;
    }
    return b->gbuf;
}

/* CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer) */
static void IDirect3DDevice8_CreateVertexBuffer(Guest* g)
{
    uint32_t p = obj_new(O_VB);
    Obj* b = obj(p);
    b->size = ARG(1), b->usage = ARG(2), b->fvf = ARG(3), b->pool = ARG(4);
    if (g_water_loading)
    {
        b->water = 1;
        if (!(g_water_buffers++ & 63))
            rt_log("[recomp] d3d8: water: %u vertex buffers of the game's water models so far\n", g_water_buffers);
    }
    static_buffer_init(b);
    wr32(ARG(5), p);
    RET(D3D_OK, 6);
}

/* CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer) */
static void IDirect3DDevice8_CreateIndexBuffer(Guest* g)
{
    uint32_t p = obj_new(O_IB);
    Obj* b = obj(p);
    b->size = ARG(1), b->usage = ARG(2), b->format = ARG(3), b->pool = ARG(4);
    b->serial = ++g_buf_serial;
    static_buffer_init(b);
    wr32(ARG(5), p);
    RET(D3D_OK, 6);
}

/* CreateRenderTarget(Width, Height, Format, MultiSample, Lockable, ppSurface) */
static void IDirect3DDevice8_CreateRenderTarget(Guest* g)
{
    wr32(ARG(6), new_surface(ARG(3), ARG(1), ARG(2), USAGE_RENDERTARGET, 0, 0));
    RET(D3D_OK, 7);
}

/* CreateDepthStencilSurface(Width, Height, Format, MultiSample, ppSurface) */
static void IDirect3DDevice8_CreateDepthStencilSurface(Guest* g)
{
    wr32(ARG(5), new_surface(ARG(3), ARG(1), ARG(2), USAGE_DEPTHSTENCIL, 0, 0));
    RET(D3D_OK, 6);
}

/* CreateImageSurface(Width, Height, Format, ppSurface): system memory */
static void IDirect3DDevice8_CreateImageSurface(Guest* g)
{
    wr32(ARG(4), new_surface(ARG(3), ARG(1), ARG(2), 0, 2, 0));
    RET(D3D_OK, 5);
}

/* CopyRects(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface, pDestPointsArray).
 * Between two surfaces the GPU owns (the back buffer into a render-target texture, say) the copy is
 * a GPU blit. Otherwise it happens in guest memory - a GPU-owned source is read back first - and the
 * destination is uploaded: at once if the GPU owns it, before its next draw if not. */
static void IDirect3DDevice8_CopyRects(Guest* g)
{
    g_ws.copies++;
    WORLD_FLUSH();
    water_flush();
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(4));
    if (!s || !d || s->format != d->format)
        RET(D3DERR_INVALIDCALL, 6);
    if (g_gpu_skip)
        RET(D3D_OK, 6);
    Obj* dc = d->container ? obj(d->container) : NULL;
    if (dc)
        dc->wr_known = 0; /* copied into: what it holds isn't followed (fade_scissor) */
    if (surface_scaled(s) || surface_scaled(d))
    {
        static int said;
        if (!said)
            said = 1, rt_log("[recomp] d3d8: CopyRects with a render target drawn larger is skipped\n");
        RET(D3D_OK, 6);
    }
    uint32_t sface, slevel, dface, dlevel;
    GfxTex* sg = surface_gpu(s, &sface, &slevel);
    GfxTex* dg = surface_gpu(d, &dface, &dlevel);
    int s_gpu = gpu_owned(s), d_gpu = gpu_owned(d);
    uint32_t n = ARG(2) ? ARG(3) : 1, blk = fmt_block(s->format), unit = blk ? 4 : 1, bpp = blk ? blk : fmt_bytes(s->format);
    uint32_t sp = fmt_pitch(s->format, s->width), dp = fmt_pitch(d->format, d->width);
    uint8_t *sm = NULL, *dm = NULL;
    if (!(s_gpu && d_gpu))
    {
        sm = GUEST_PTR(obj_mem(s));
        dm = GUEST_PTR(obj_mem(d));
        if (s_gpu && !(s->usage & USAGE_DEPTHSTENCIL))
            gfx_tex_read(sg, sface, slevel, sm, sp);
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        int32_t l = 0, t = 0, r = (int32_t)s->width, b = (int32_t)s->height;
        if (ARG(2))
        {
            uint32_t rc = ARG(2) + 16 * i;
            l = (int32_t)rd32(rc), t = (int32_t)rd32(rc + 4), r = (int32_t)rd32(rc + 8), b = (int32_t)rd32(rc + 12);
        }
        int32_t x = l, y = t;
        if (ARG(5))
            x = (int32_t)rd32(ARG(5) + 8 * i), y = (int32_t)rd32(ARG(5) + 8 * i + 4);
        if (l < 0 || t < 0 || r > (int32_t)s->width || b > (int32_t)s->height || r <= l || b <= t || x < 0 || y < 0 ||
            x + (r - l) > (int32_t)d->width || y + (b - t) > (int32_t)d->height)
            continue;
        if (s_gpu && d_gpu)
        {
            gfx_copy(sg, sface, slevel, (uint32_t)l, (uint32_t)t, (uint32_t)(r - l), (uint32_t)(b - t), dg, dface, dlevel,
                (uint32_t)x, (uint32_t)y);
            continue;
        }
        uint32_t rows = ((uint32_t)(b - t) + unit - 1) / unit, bytes = ((uint32_t)(r - l) + unit - 1) / unit * bpp;
        uint8_t* drow = dm + ((uint32_t)y / unit) * dp + (uint32_t)x / unit * bpp;
        for (uint32_t row = 0; row < rows; ++row)
            memmove(drow + row * dp, sm + ((uint32_t)t / unit + row) * sp + (uint32_t)l / unit * bpp, bytes);
        if (d_gpu && !(d->usage & USAGE_DEPTHSTENCIL))
            gfx_tex_upload_rect(dg, dface, dlevel, (uint32_t)x, (uint32_t)y, (uint32_t)(r - l), (uint32_t)(b - t), drow, dp);
    }
    if (!d_gpu)
        mark_dirty(d);
    RET(D3D_OK, 6);
}

/* SetRenderTarget(pRenderTarget, pNewZStencil): NULL render target keeps the current one */
static void IDirect3DDevice8_SetRenderTarget(Guest* g)
{
    WORLD_FLUSH();
    water_flush();
    if (ARG(1))
    {
        bind(&g_dev.rt, ARG(1));
        Obj* rt = obj(ARG(1));
        if (rt) /* D3D8 resets the viewport to the new target */
        {
            uint32_t* vp = g_dev.cur.vp;
            vp[0] = vp[1] = 0, vp[2] = rt->width, vp[3] = rt->height, vp[4] = 0, vp[5] = f2u(1.0f);
        }
    }
    bind(&g_dev.ds, ARG(2));
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetRenderTarget(Guest* g)
{
    obj_addref(g_dev.rt);
    wr32(ARG(1), g_dev.rt);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetDepthStencilSurface(Guest* g)
{
    wr32(ARG(1), g_dev.ds);
    if (!g_dev.ds)
        RET(0x88760834u, 2); /* D3DERR_NOTFOUND */
    obj_addref(g_dev.ds);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_BeginScene(Guest* g) { RET(D3D_OK, 1); }
static void IDirect3DDevice8_EndScene(Guest* g) { RET(D3D_OK, 1); }
static void apply_targets(void);

/* Clear(Count, pRects, Flags, Color, Z, Stencil): D3DRECTs are x1, y1, x2, y2 */
static void IDirect3DDevice8_Clear(Guest* g)
{
    WORLD_FLUSH();
    water_flush();
    if (g_gpu_skip)
        RET(D3D_OK, 7);
    Obj* wt = g_fade_scissor && (ARG(3) & 1u) ? wr_target() : NULL;
    if (wt) /* all of it to alpha 0: nothing drawn yet; else what it holds isn't known */
    {
        int all = !ARG(1) && !g_dev.cur.vp[0] && !g_dev.cur.vp[1] && g_dev.cur.vp[2] >= wt->width && g_dev.cur.vp[3] >= wt->height;
        wt->wr_known = all && !(ARG(4) >> 24), wt->wr[0] = wt->wr[1] = wt->wr[2] = wt->wr[3] = 0;
    }
    if (g_probe_wrapped && (ARG(3) & 1u) && !ARG(1)) /* D3DCLEAR_TARGET, all of it */
    {
        Obj* rt = obj(g_dev.rt);
        Obj* t = rt && rt->container ? obj(rt->container) : NULL;
        if (t)
            t->unread = 0;
    }
    apply_targets();
    uint32_t n = ARG(2) ? ARG(1) : 0, vp[6];
    memcpy(vp, g_dev.cur.vp, sizeof vp);
    int32_t few[4 * 16], *rects = n <= 16 ? few : (int32_t*)malloc(16u * n);
    if (n)
        memcpy(rects, ARGP(2), 16u * n);
    scale_to_target(vp, rects, n);
    gfx_clear(n, n ? rects : NULL, ARG(3), ARG(4), u2f(ARG(5)), ARG(6), vp);
    if (rects != few)
        free(rects);
    RET(D3D_OK, 7);
}

/* --- IDirect3DDevice8: state ----------------------------------------------------------------------------- */
static void IDirect3DDevice8_SetTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    memcpy(s->xf[i], ARGP(2), 64);
    if (i >= 16 && i < 24)
        s->ver_texm = state_ver();
    raise_top(&g_xf_top, (uint32_t)i + 1);
    if (m)
        m->xf[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(ARGP(2), g_dev.cur.xf[i], 64);
    RET(D3D_OK, 3);
}

/* MultiplyTransform(State, pMatrix): the state's matrix becomes pMatrix * matrix */
static void IDirect3DDevice8_MultiplyTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    float a[16], r[16];
    memcpy(a, ARGP(2), 64);
    const float* b = g_dev.cur.xf[i];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            r[y * 4 + x] = a[y * 4] * b[x] + a[y * 4 + 1] * b[4 + x] + a[y * 4 + 2] * b[8 + x] + a[y * 4 + 3] * b[12 + x];
    Mask* m;
    State* s = target(&m);
    memcpy(s->xf[i], r, 64);
    if (i >= 16 && i < 24)
        s->ver_texm = state_ver();
    raise_top(&g_xf_top, (uint32_t)i + 1);
    if (m)
        m->xf[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetViewport(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    memcpy(s->vp, ARGP(1), 24);
    if (m)
        m->vp = 1;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetViewport(Guest* g)
{
    memcpy(ARGP(1), g_dev.cur.vp, 24);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetMaterial(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    memcpy(s->mat, ARGP(1), 68);
    if (m)
        m->mat = 1;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetMaterial(Guest* g)
{
    memcpy(ARGP(1), g_dev.cur.mat, 68);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetLight(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    memcpy(s->light[i].v, ARGP(2), 104);
    raise_top(&g_light_top, i + 1);
    if (m)
        m->light[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetLight(Guest* g)
{
    if (ARG(1) >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(ARGP(2), g_dev.cur.light[ARG(1)].v, 104);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_LightEnable(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    if (!s->light[i].v[0]) /* never set: D3D enables a default light */
    {
        default_light(&s->light[i]);
        if (m)
            m->light[i] = 1;
    }
    s->light[i].enabled = ARG(2) != 0;
    raise_top(&g_light_top, i + 1);
    if (m)
        m->lighten[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetLightEnable(Guest* g)
{
    if (ARG(1) >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    wr32(ARG(2), g_dev.cur.light[ARG(1)].enabled);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetClipPlane(Guest* g)
{
    if (ARG(1) >= 6)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    memcpy(s->clip[ARG(1)], ARGP(2), 16);
    if (m)
        m->clip[ARG(1)] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetRenderState(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= 256)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    s->rs[i] = ARG(2);
    if (m)
        m->rs[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetRenderState(Guest* g)
{
    if (ARG(1) >= 256)
        RET(D3DERR_INVALIDCALL, 3);
    wr32(ARG(2), g_dev.cur.rs[ARG(1)]);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_BeginStateBlock(Guest* g)
{
    if (g_dev.rec)
        RET(D3DERR_INVALIDCALL, 1);
    g_dev.rec = (Block*)calloc(1, sizeof(Block));
    RET(D3D_OK, 1);
}

static void IDirect3DDevice8_EndStateBlock(Guest* g)
{
    if (!g_dev.rec)
        RET(D3DERR_INVALIDCALL, 2);
    wr32(ARG(1), block_token(g_dev.rec));
    g_dev.rec = NULL;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_ApplyStateBlock(Guest* g)
{
    Block* b = block_of(ARG(1));
    if (!b || g_dev.rec)
        RET(D3DERR_INVALIDCALL, 2);
    state_copy(&g_dev.cur, &b->s, &b->m);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_CaptureStateBlock(Guest* g)
{
    Block* b = block_of(ARG(1));
    if (!b || g_dev.rec)
        RET(D3DERR_INVALIDCALL, 2);
    state_copy(&b->s, &g_dev.cur, &b->m);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_DeleteStateBlock(Guest* g)
{
    Block* b = block_of(ARG(1));
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    block_free(b);
    g_dev.blocks[ARG(1) - 1] = NULL;
    RET(D3D_OK, 2);
}

/* CreateStateBlock(Type, pToken): D3DSBT_ALL 1, PIXELSTATE 2, VERTEXSTATE 3. The pixel and vertex
 * sets here are the categories, not D3D's exact render-state lists: rendering state that would be
 * captured by one or the other is captured by both. */
static void IDirect3DDevice8_CreateStateBlock(Guest* g)
{
    uint32_t type = ARG(1);
    if (type < 1 || type > 3 || g_dev.rec)
        RET(D3DERR_INVALIDCALL, 3);
    Block* b = (Block*)calloc(1, sizeof(Block));
    /* state_copy stops at the high-water marks (g_xf_top, g_light_top, g_vsc_top): past them the block
     * starts as the device is (identity transforms, lights off, zero constants), so one applied after
     * the game first sets such an entry puts its default back, not zeros */
    memcpy(b->s.xf, g_dev.cur.xf, sizeof b->s.xf);
    memcpy(b->s.light, g_dev.cur.light, sizeof b->s.light);
    memcpy(b->s.vsc, g_dev.cur.vsc, sizeof b->s.vsc);
    b->s.ver_texm = g_dev.cur.ver_texm, b->s.ver_vsc = g_dev.cur.ver_vsc; /* (the same contents) */
    Mask* m = &b->m;
    memset(m->rs, 1, sizeof m->rs);
    memset(m->tss, 1, sizeof m->tss);
    if (type == 1 || type == 2)
    {
        m->ps = 1;
        memset(m->psc, 1, sizeof m->psc);
    }
    if (type == 1 || type == 3)
    {
        m->vs = 1;
        memset(m->vsc, 1, sizeof m->vsc);
        memset(m->light, 1, sizeof m->light);
        memset(m->lighten, 1, sizeof m->lighten);
    }
    if (type == 1)
    {
        memset(m->tex, 1, sizeof m->tex);
        memset(m->xf, 1, sizeof m->xf);
        memset(m->clip, 1, sizeof m->clip);
        memset(m->stream, 1, sizeof m->stream);
        m->vp = m->mat = m->ib = 1;
    }
    state_copy(&b->s, &g_dev.cur, m);
    wr32(ARG(2), block_token(b));
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetTexture(Guest* g)
{
    if (ARG(1) >= 8)
        RET(D3DERR_INVALIDCALL, 3);
    uint32_t t = g_dev.cur.tex[ARG(1)];
    if (t)
        obj_addref(t);
    wr32(ARG(2), t);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetTexture(Guest* g)
{
    if (ARG(1) >= 8)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    bind(&s->tex[ARG(1)], ARG(2));
    if (m)
        m->tex[ARG(1)] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetTextureStageState(Guest* g)
{
    if (ARG(1) >= 8 || ARG(2) >= 32)
        RET(D3DERR_INVALIDCALL, 4);
    wr32(ARG(3), g_dev.cur.tss[ARG(1)][ARG(2)]);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_SetTextureStageState(Guest* g)
{
    if (ARG(1) >= 8 || ARG(2) >= 32)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    s->tss[ARG(1)][ARG(2)] = ARG(3);
    if (m)
        m->tss[ARG(1)][ARG(2)] = 1;
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_ValidateDevice(Guest* g)
{
    wr32(ARG(1), 1);
    RET(D3D_OK, 2);
}

/* Palettes are held, never applied: no paletted format is offered, but the game sets a grey ramp
 * before 8-bit uploads anyway (10281690). */
#define PALETTES 64u
static uint8_t g_palette[PALETTES][1024];
static uint32_t g_cur_palette;

/* SetPaletteEntries(PaletteNumber, pEntries) / GetPaletteEntries(PaletteNumber, pEntries) */
static void IDirect3DDevice8_SetPaletteEntries(Guest* g)
{
    if (!ARG(2))
        RET(D3DERR_INVALIDCALL, 3);
    if (ARG(1) < PALETTES)
        memcpy(g_palette[ARG(1)], ARGP(2), 1024);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetPaletteEntries(Guest* g)
{
    if (!ARG(2) || ARG(1) >= PALETTES)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(ARGP(2), g_palette[ARG(1)], 1024);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetCurrentTexturePalette(Guest* g)
{
    g_cur_palette = ARG(1);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetCurrentTexturePalette(Guest* g)
{
    if (!ARG(1))
        RET(D3DERR_INVALIDCALL, 2);
    wr32(ARG(1), g_cur_palette);
    RET(D3D_OK, 2);
}

/* --- IDirect3DDevice8: drawing ----------------------------------------------------------------------------- */
static void draw(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size, uint32_t up_data,
    uint32_t up_stride);

/* --- the PC port's own drawing mistakes, drawn as the PS2 draws them ----------------------------------------------
 * After portfix (github.com/voetre/portfix, MIT License, Copyright (c) 2026 voetre), which found and measured both.
 * fix_ps2 (Config > Graphics > PS2 Accuracy) turns them all on or off; each also has its own setting, on unless set
 * to 0: fix_water_layers, fix_button_rims, fix_summon_halos, fix_shine, fix_disintegrate. They are read once a frame
 * (fixes_frame), not at every draw.
 *
 * Water layers. Port Bastok's sea is three flat layers drawn in order: an opaque black one, the scrolling ripples added
 * on top (SRCALPHA, ONE) with depth written, then a dark tint on the black layer's geometry, 0.023 under the ripples
 * and tested LESSEQUAL. On the PS2 the tint shows; on the PC its depth fails against the ripples' wherever the
 * difference is a step of the depth buffer or more, so near the camera only the black and faint ripples are left.
 * The ripple draw is held back (the whole device state with it) and drawn after the next DrawPrimitive, the tint,
 * with no depth write: black, tint, ripples on top. Anything else drawn first, a clear, a change of target or
 * Present lets it go as it was meant to be drawn. (portfix also lifts every water layer: water_lift does that here.)
 *
 * Button rims. The title screen's and the config window's buttons are each two quads of one texture: a black copy a
 * few pixels down and right, alpha blended, as a drop shadow, then the body added (SRCALPHA, ONE). Over the shadow the
 * sum is the body's colour; along the body's top and left edges, past the shadow, it adds to what is behind and shows
 * as a light, see-through rim. The PS2's buttons have none, nor does a disabled button (alpha blended). A body drawn
 * added over its own shadow (same texture, size and uv, the shadow just down and right of it) is drawn alpha blended:
 * where the shadow lies under it the result is the same. */
static int g_fix_water = 1, g_fix_buttons = 1, g_fix_halos = 1, g_fix_shine = 1, g_fix_disint = 1;
static float g_halo_max;     /* fix_halo_max: yalms, 0 no cap */
static float g_skin_at[3];   /* the first bone of the part skinned last (host/ps2fix.c), in the world */
static int g_skin_at_set;
static Obj* sub(uint32_t tex, uint32_t face, uint32_t level);

static void fixes_frame(void)
{
    int ps2 = gfx_fx_get("fix_ps2") != 0.0f;
    g_fix_water = ps2 && gfx_fx_get("fix_water_layers") != 0.0f;
    g_fix_buttons = ps2 && gfx_fx_get("fix_button_rims") != 0.0f;
    g_fix_halos = ps2 && gfx_fx_get("fix_summon_halos") != 0.0f;
    g_fix_shine = ps2 && gfx_fx_get("fix_shine") != 0.0f;
    g_fix_disint = ps2 && gfx_fx_get("fix_disintegrate") != 0.0f;
    g_halo_max = gfx_fx_get("fix_halo_max");
}

unsigned d3d8_fixes(void)
{
    return (g_fix_shine ? D3D8_FIX_SHINE : 0u) | (g_fix_disint ? D3D8_FIX_DISINTEGRATE : 0u) |
        (g_fix_halos && g_halo_max > 0.0f ? D3D8_FIX_HALO_MAX : 0u);
}

void d3d8_skinned_at(const float at[3])
{
    memcpy(g_skin_at, at, sizeof g_skin_at);
    g_skin_at_set = 1;
}

/* the held ripple draw: DrawPrimitive's arguments, the scene renderer's site for it, the device state it had */
static struct
{
    int held;
    uint32_t prim, start, count, zone;
    State s;
} g_water;

/* the ripple layer: FVF XYZ|NORMAL|DIFFUSE|TEX1, unlit, added (SRCALPHA, ONE) with depth written, its texture
 * coordinates scrolled (COUNT2), stage 0 MODULATE */
static int water_ripple(const State* s)
{
    const uint32_t* rs = s->rs;
    return s->vs == 0x152u && rs[14] == 1 && rs[27] == 1 && rs[19] == 5 && rs[20] == 2 && rs[137] == 0 &&
        s->tss[0][24] == 2 && s->tss[0][1] == 4;
}

static void water_hold(uint32_t prim, uint32_t start, uint32_t count)
{
    g_water.held = 1, g_water.prim = prim, g_water.start = start, g_water.count = count, g_water.zone = g_draw_zone;
    g_water.s = g_dev.cur;
}

/* the held ripple draw, drawn now in its own state with no depth write; the device's state is then put back */
static void water_flush(void)
{
    if (!g_water.held)
        return;
    g_water.held = 0;
    static State keep;
    keep = g_dev.cur;
    g_dev.cur = g_water.s;
    g_dev.cur.rs[14] = 0; /* ZWRITEENABLE */
    static int said;
    if (!said++)
        rt_log("[recomp] d3d8: water layers: a ripple layer drawn after its tint, as on the PS2 (fix_water_layers)\n");
    uint32_t zone = g_draw_zone;
    g_draw_zone = g_water.zone;
    draw(g_water.prim, g_water.count, g_water.start, 0, 0, 0, 0);
    g_draw_zone = zone;
    g_dev.cur = keep;
}

/* Summon halos (Carbuncle's blue aura, other summons'): the model is drawn into a silhouette texture the scene's size
 * (4096 at a 4096 background; 2048 here), copied down to a 256x256 target with one quad, and the copy drawn back over
 * the scene as full-screen quads: three tinted ghosts alpha blended, nudged ~3 pixels in a pattern that turns each
 * frame, one opaque alpha-tested copy, then the silhouette itself on top. One texel of the copy is 8-16 of the scene's
 * pixels, so the opaque copy sticks out past the model in steps, and the nudges, in the PS2's 640-wide frame's pixels,
 * don't part the ghosts from the model: a thin stepped rim instead of the PS2's soft halo. The quads that read the copy
 * are drawn from the full-size silhouette instead, their nudge scaled by the target's width / 640. Colours, blending,
 * the ghosts and their pattern are the game's. (portfix's optional cap on the halo's reach, /portfix halomax, isn't
 * here.)
 *
 * The cap (fix_halo_max, off unless set, as on the PS2; portfix's /portfix halomax): the nudges are a fixed part of
 * the picture, so against the model they grow with distance - at depth z, x0 / 320 x z / P00 and y0 / 320 x z / P11
 * yalms. Over the cap they shrink to it, their directions kept. The model's depth is taken at the copy-down from the
 * first bone of the part skinned last (host/ps2fix.c), through the view. */

/* 0, or a copy of a halo quad's vertices nudged for the full-size silhouette, which is bound in its place (tex[0]) */
static uint32_t halo_quad(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride)
{
    const State* s = &g_dev.cur;
    if (prim != 5 || count != 2 || stride != 28 || s->vs != 0x144u || !data || !s->tex[0])
        return 0;
    Obj* rt = obj(g_dev.rt);
    if (!rt)
        return 0;
    if (rt->width == 256 && rt->height == 256) /* the copy-down: the silhouette and the copy's surface */
    {
        g_halo_src = s->tex[0], g_halo_low = g_dev.rt;
        const float* v = s->xf[2]; /* D3DTS_VIEW */
        g_halo_depth = g_halo_max > 0.0f && g_skin_at_set
            ? fabsf(g_skin_at[0] * v[2] + g_skin_at[1] * v[6] + g_skin_at[2] * v[10] + v[14]) : 0.0f;
        return 0;
    }
    Obj* low = g_halo_low ? obj(g_halo_low) : NULL;
    Obj* src = g_halo_src ? obj(g_halo_src) : NULL;
    if (!low || !src || low->container != s->tex[0] || src->kind != O_TEXTURE || !rt->width)
        return 0;
    static uint32_t buf;
    if (!buf)
        buf = gheap_alloc(4 * 28, 1);
    static int said;
    if (!said++)
        rt_log("[recomp] d3d8: summon halos: drawn from the %ux%u silhouette (fix_summon_halos)\n", src->width, src->height);
    memcpy(GUEST_PTR(buf), GUEST_PTR(data), 4 * 28);
    float k = (float)rt->width / 640.0f, x0 = u2f(rd32(data)), y0 = u2f(rd32(data + 4));
    const float* P = s->xf[3]; /* D3DTS_PROJECTION */
    if (g_halo_max > 0.0f && g_halo_depth > 0.0f && P[0] > 0.0f && P[5] > 0.0f)
    {
        float sep = hypotf(x0 / 320.0f * g_halo_depth / P[0], y0 / 320.0f * g_halo_depth / P[5]);
        if (sep > g_halo_max)
        {
            static int capped;
            if (!capped++)
                rt_log("[recomp] d3d8: summon halos: reach capped at %.3f yalms, the model %.1f yalms away (fix_halo_max)\n",
                    (double)g_halo_max, (double)g_halo_depth);
            k *= g_halo_max / sep;
        }
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
        uint32_t v = buf + 28u * i;
        wr32(v, f2u(u2f(rd32(v)) + x0 * (k - 1.0f)));
        wr32(v + 4, f2u(u2f(rd32(v + 4)) + y0 * (k - 1.0f)));
    }
    return buf;
}

/* Shine (fix_shine). Two kinds, one draw each on the PC, both added by stage 1 as MODULATEALPHA_ADDCOLOR (CURRENT,
 * TEXTURE): current + current alpha x texture, where stage 0's alpha is the texture's alpha x strength / 2 (TFACTOR's
 * alpha) x 2, the PS2's alpha read on the 0xFF = 1.0 scale instead of its 0x80 = 1.0: half the PS2's shine.
 * - Sphere map (gold plates and the like: texture coordinates CAMERASPACENORMAL | 1): the identity texture matrix
 *   reads (nx, ny), a quarter of the map, its dark corner on a face turned to the camera. Drawn with the PS2's
 *   mapping, u = 0.5 + 0.5 nx, v = 0.5 - 0.5 ny, and twice the strength: stage 1 = texture x 2 x current's alpha into
 *   TEMP, stage 2 = current + TEMP.
 * - The light's highlight on weapons and armour (CAMERASPACEREFLECTIONVECTOR | 1, a cube): the texture matrix turns the
 *   reflection over (diag(1, -1, -1)), so the spot is found looking away from the light; the PS2's sphere-mapped spot
 *   texture pasted on the cube's -Z face has half its spread; and it is white, at half strength. Drawn with the flip
 *   taken out, a cube rebuilt from the game's -Z face with the PS2's spread (radius 0.6 x sin(angle / 2), 0.5 the
 *   texture's edge), and stage 1 = cube x TFACTOR x 4 into TEMP with TFACTOR's colour half light 0's diffuse, stage 2
 *   = current + current alpha x TEMP. TFACTOR's alpha (the strength) and the stages' alpha results are the game's.
 * The shiny batches some weapon files lost (drawn with the plain material) are restored by host/ps2fix.c. */
static uint32_t g_spot_cube, g_spot_from; /* the highlight cube built from the game's (g_spot_from) */

/* 1 sphere-map shine, 2 the light's highlight, 0 neither */
static int shine_kind(const State* s)
{
    const uint32_t* t = s->tss[1];
    if (t[1] != 18 || t[2] != 1 || t[3] != 2) /* MODULATEALPHA_ADDCOLOR (CURRENT, TEXTURE) */
        return 0;
    return t[11] == (0x10000u | 1) ? 1 : t[11] == (0x30000u | 1) ? 2 : 0;
}

/* a direction's texel of the game's spot face, bilinear, the spot's corner colour outside */
static uint32_t spot_sample(const uint32_t* spot, uint32_t n, float u, float v)
{
    float x = u * (float)n - 0.5f, y = v * (float)n - 0.5f, acc[4] = { 0, 0, 0, 0 };
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    for (int k = 0; k < 4; ++k)
    {
        int xi = x0 + (k & 1), yi = y0 + (k >> 1);
        float w = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
        uint32_t c = xi >= 0 && yi >= 0 && xi < (int)n && yi < (int)n ? spot[yi * (int)n + xi] : (spot[0] & 0xFF000000u);
        for (int b = 0; b < 4; ++b)
            acc[b] += w * (float)(c >> (8 * b) & 0xFF);
    }
    uint32_t out = 0;
    for (int b = 0; b < 4; ++b)
        out |= ((uint32_t)lroundf(acc[b]) & 0xFF) << (8 * b);
    return out;
}

/* the highlight cube for the game's (built once a cube), or 0 when it can't be made from it */
static uint32_t spot_cube(uint32_t game)
{
    if (game == g_spot_from && g_spot_cube)
        return g_spot_cube;
    Obj* g = obj(game);
    Obj* face = sub(game, 5, 0); /* D3DCUBEMAP_FACE_NEGATIVE_Z */
    if (!g || g->kind != O_CUBE || !face || !face->mem || (face->format != 21 && face->format != 22) || face->width < 8 ||
        face->width != face->height)
        return 0;
    uint32_t n = face->width, pitch = fmt_pitch(face->format, n);
    uint32_t* spot = (uint32_t*)malloc(4u * n * n);
    if (!spot)
        return 0;
    for (uint32_t y = 0; y < n; ++y)
        memcpy(spot + y * n, (const uint8_t*)GUEST_PTR(face->mem) + y * pitch, 4u * n);
    const uint32_t N = 128;
    if (g_spot_cube)
        obj_release(g_spot_cube), g_spot_cube = 0;
    uint32_t cube = new_texture(O_CUBE, N, N, 1, 0, 21 /* A8R8G8B8 */, 1 /* MANAGED */);
    for (uint32_t f = 0; f < 6; ++f)
    {
        Obj* s = sub(cube, f, 0);
        uint8_t* px = (uint8_t*)GUEST_PTR(obj_mem(s));
        uint32_t sp = fmt_pitch(21, N);
        for (uint32_t y = 0; y < N; ++y)
            for (uint32_t x = 0; x < N; ++x)
            {
                /* this texel's direction, in Direct3D's cube face layout */
                float a = 2.0f * ((float)x + 0.5f) / (float)N - 1.0f, b = 2.0f * ((float)y + 0.5f) / (float)N - 1.0f, d[3];
                switch (f)
                {
                case 0: d[0] = 1, d[1] = -b, d[2] = -a; break;
                case 1: d[0] = -1, d[1] = -b, d[2] = a; break;
                case 2: d[0] = a, d[1] = 1, d[2] = b; break;
                case 3: d[0] = a, d[1] = -1, d[2] = -b; break;
                case 4: d[0] = a, d[1] = -b, d[2] = 1; break;
                default: d[0] = -a, d[1] = -b, d[2] = -1; break;
                }
                float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), c = -d[2] / len; /* cos from the axis (-Z) */
                float r = 0.6f * sqrtf(fmaxf(0.0f, (1.0f - c) * 0.5f)), side = sqrtf(d[0] * d[0] + d[1] * d[1]);
                /* the same way round as the game's face near the axis: face -Z texel (u, v) is direction (-u, -v) */
                float u = side > 1e-6f ? 0.5f - r * d[0] / side : 0.5f, v = side > 1e-6f ? 0.5f - r * d[1] / side : 0.5f;
                uint32_t t = spot_sample(spot, n, u, v);
                memcpy(px + y * sp + x * 4u, &t, 4);
            }
        mark_dirty(s);
    }
    free(spot);
    g_spot_cube = cube, g_spot_from = game;
    return cube;
}

/* 1 when a shine draw was drawn here, as the PS2 draws it */
static int shine_draw(uint32_t prim, uint32_t count, uint32_t indices, uint32_t isize)
{
    State* s = &g_dev.cur;
    int kind = shine_kind(s);
    uint32_t cube = 0;
    if (!kind || (kind == 2 && !(cube = spot_cube(s->tex[1]))))
        return 0;
    uint32_t keep[3][32], tf = s->rs[60], tex1 = s->tex[1];
    float keep_m[16];
    memcpy(keep, s->tss[1], sizeof keep);
    memcpy(keep_m, s->xf[17], sizeof keep_m); /* D3DTS_TEXTURE1 */
    uint32_t* t1 = s->tss[1];
    uint32_t* t2 = s->tss[2];
    if (kind == 1)
    {
        static const float ps2[16] = { 0.5f, 0, 0, 0, 0, -0.5f, 0, 0, 0, 0, 0, 0, 0.5f, 0.5f, 0, 1 };
        memcpy(s->xf[17], ps2, sizeof ps2);
        t1[1] = 5, t1[2] = 2, t1[3] = 1 | 0x20; /* MODULATE2X (TEXTURE, CURRENT | ALPHAREPLICATE) */
        t2[1] = 7, t2[2] = 1, t2[3] = 5;        /* ADD (CURRENT, TEMP) */
    }
    else
    {
        for (int c = 0; c < 4; ++c) /* diag(1, -1, -1) x the matrix: its second and third rows negated */
            s->xf[17][4 + c] = -keep_m[4 + c], s->xf[17][8 + c] = -keep_m[8 + c];
        const Light* l0 = &s->light[0];
        float rgb[3] = { 1, 1, 1 };
        if (l0->enabled)
            for (int i = 0; i < 3; ++i)
                rgb[i] = u2f(l0->v[1 + i]); /* D3DLIGHT8 Diffuse */
        s->rs[60] = tf & 0xFF000000u;
        for (int i = 0; i < 3; ++i)
            s->rs[60] |= (uint32_t)lroundf(fminf(1.0f, fmaxf(0.0f, rgb[i] * 0.5f)) * 255.0f) << (16 - 8 * i);
        s->tex[1] = cube;
        t1[1] = 6, t1[2] = 2, t1[3] = 3; /* MODULATE4X (TEXTURE, TFACTOR) */
        t2[1] = 18, t2[2] = 1, t2[3] = 5; /* MODULATEALPHA_ADDCOLOR (CURRENT, TEMP) */
    }
    t1[28] = 5;          /* RESULTARG TEMP */
    t2[4] = 2, t2[5] = 5; /* ALPHAOP SELECTARG1 (TEMP) */
    s->tss[3][1] = 1, s->tss[3][4] = 1; /* stage 3 off */
    static int said[2];
    if (!said[kind - 1]++)
        rt_log(kind == 1 ? "[recomp] d3d8: shine: sphere-mapped shine drawn as on the PS2 (fix_shine)\n"
                         : "[recomp] d3d8: shine: the light's highlight drawn as on the PS2 (fix_shine)\n");
    draw(prim, count, 0, indices, isize, 0, 0);
    memcpy(s->tss[1], keep, sizeof keep);
    memcpy(s->xf[17], keep_m, sizeof keep_m);
    s->rs[60] = tf, s->tex[1] = tex1;
    return 1;
}

/* Disintegrate (fix_disintegrate). Some effects break a model up: most notably the Shadow Lord's change of form in the
 * Throne Room (effect file 16/98), and Eald'Narche's, Kam'lanaut's and Selh'teus's. The effect sweeps a particle over
 * the body; every frame its op 0x65 marks a sphere around it, and on the PS2 each body triangle with a vertex inside a
 * sphere leaves the body and flies off as a textured piece, with purple smoke and a heat haze where it broke. The PC
 * runs the same particles, but its interpreter sends op 0x65 to the do-nothing case: the body stays whole.
 * host/ps2fix.c records the spheres (d3d8_disint_sphere, the actor named by its bone palette) and, as each part of a
 * character is skinned on the CPU into world space, which palette and vertex buffers it was (d3d8_disint_part). A
 * capturing actor's part draws (DrawIndexedPrimitive from those buffers) are drawn here: the triangles newly inside a
 * sphere are taken as pieces (their positions, uv, texture, and colours lit with the draw's own lights, as the PS2
 * copies the body's lit colours) and the part is drawn from an index list without them. They are drawn one depth bias
 * step nearer from the actor's first sphere on: the second form is the same mesh in the same pose drawn after the
 * first, opaque, and would win every depth tie and cover it at once; on the PS2 the first stays on top and only its
 * holes show the second. The pieces go in before the first interface draw on the target the body was drawn on, with
 * the PS2's values (mt00's 'disg'): 40 ticks, rising 0.012 a tick (plus up to +-0.003), shrinking from (0.8, 0.8, 1)
 * to 0.02, tumbling up to (0.0314, 0.0332, 0.0332) radians a tick, one tumble and rise per 30 Hz frame shared by that
 * frame's pieces; depth tested, no depth write, no blending, MODULATE2X (texture, diffuse). On frames that took
 * triangles, once a 30 Hz frame at one of them, host/ps2fix.c emits the smoke through the game and a haze element
 * is made here (g000's 'dist'): a fan of the scene copied, shifted 0.07 in its own frame, turning 0.0698 radians and
 * rising 0.002 a tick for 30 ticks, its centre's alpha up to 0x78 of 0x80, blended over the scene. An actor with no
 * spheres for 3 seconds gets its body back. Differences from the PS2 (as portfix's): triangles are taken one at a
 * time, not a GS packet (about 4) at a time; the PC's meshes cut the body into other triangles; the colours are the
 * PC's lighting; the haze copies the scene once a frame. */
#define DIS_TICK 0.06f        /* effect ticks a millisecond */
#define DIS_PSFRAME 0.03f     /* PS2 frames a millisecond */
#define DIS_PIECE_LIFE 40.0f  /* ticks */
#define DIS_PIECE_RISE 0.012f /* a tick */
#define DIS_PIECE_DRIFT 0.003f
#define DIS_PIECE_END 0.02f /* its size at the end */
#define DIS_PIECE_COLOUR 0xFF312029u /* an unlit piece: the body's usual lit colour */
#define DIS_HAZE_LIFE 30.0f
#define DIS_HAZE_SHIFT 0.07f
#define DIS_HOLD_MS 3000u
#define DIS_MAX_PIECES 20000u

typedef struct DisPart
{
    uint64_t key;
    uint16_t* tri;  /* 3 a triangle: the game's index values */
    uint8_t* taken; /* a triangle */
    uint32_t ntri, ntaken, kept;
    uint32_t ib, ibsize; /* guest memory: the triangles not taken */
    uint8_t dirty;
} DisPart;

typedef struct DisActor
{
    uint32_t bones; /* its bone palette, as the skinning reads it */
    uint64_t last;  /* ms: its last sphere */
    DisPart* parts;
    uint32_t nparts, cap;
} DisActor;

typedef struct DisPiece
{
    float p[3][3], uv[3][2], spin[3], drift;
    uint32_t tex, c[3];
    uint64_t born;
    uint8_t lit;
} DisPiece;

typedef struct DisSphere
{
    uint32_t bones;
    float c[3], r;
} DisSphere;

static struct
{
    DisSphere fresh[256], cur[256]; /* marked since the last Present; in use this frame */
    uint32_t nfresh, ncur;
    DisActor actors[8];
    uint32_t nactors;
    uint32_t part;  /* the palette of the part skinned last, when it is a capturing actor's; else 0 */
    uint32_t vb[4]; /* its buffers: positions (0, and 1 for the mirrored half), normals (2, 3) */
    DisPiece* pieces;
    uint32_t npieces, cappieces;
    struct
    {
        float p[3];
        uint64_t born;
    } haze[512];
    uint32_t nhaze;
    struct
    {
        uint32_t rt;
        float view[16], proj[16];
    } views[4]; /* the views the capturing bodies were drawn with this frame, by target */
    uint32_t nviews;
    int drawn; /* the pieces, this frame */
    uint32_t hits, last_hits;                /* triangles taken since Present, and in the frame before */
    float hit_at[4][3], last_hit_at[4][3];   /* four of them, picked at random: their centres */
    uint64_t smoke_at;                       /* ms: the smoke last made */
    uint64_t elem_frame;                     /* the 30 Hz frame of elem_spin and elem_drift */
    float elem_spin[3], elem_drift;
    uint32_t haze_tex;   /* a copy of the scene target, for the haze */
    uint32_t vbuf, vcap; /* guest memory: the pieces' and haze's vertices */
    uint32_t rand;
} g_dis = { .rand = 0x9E3779B9u };

static uint64_t dis_ms(void) { return rt_monotonic_ns() / 1000000u; }

static float dis_rand(void) /* 0..1 */
{
    g_dis.rand = g_dis.rand * 1664525u + 1013904223u;
    return (float)(g_dis.rand >> 8) / 16777216.0f;
}

static DisActor* dis_actor(uint32_t bones)
{
    for (uint32_t i = 0; i < g_dis.nactors; ++i)
        if (g_dis.actors[i].bones == bones)
            return &g_dis.actors[i];
    return NULL;
}

static void dis_actor_free(DisActor* a)
{
    for (uint32_t i = 0; i < a->nparts; ++i)
    {
        free(a->parts[i].tri), free(a->parts[i].taken);
        if (a->parts[i].ib)
            gheap_free(a->parts[i].ib);
    }
    free(a->parts);
    memset(a, 0, sizeof *a);
}

/* everything let go: the fix turned off */
static void dis_clear(void)
{
    for (uint32_t i = 0; i < g_dis.nactors; ++i)
        dis_actor_free(&g_dis.actors[i]);
    for (uint32_t i = 0; i < g_dis.npieces; ++i)
        if (g_dis.pieces[i].tex)
            obj_release(g_dis.pieces[i].tex);
    g_dis.nactors = g_dis.npieces = g_dis.nhaze = g_dis.nfresh = g_dis.ncur = 0;
    g_dis.part = 0;
    memset(g_dis.vb, 0, sizeof g_dis.vb);
}

void d3d8_disint_sphere(uint32_t bones, const float c[3], float r)
{
    if (!g_fix_disint || g_dis.nfresh >= 256)
        return;
    DisSphere* s = &g_dis.fresh[g_dis.nfresh++];
    s->bones = bones, s->r = r;
    memcpy(s->c, c, sizeof s->c);
}

void d3d8_disint_part(uint32_t palette, const uint32_t vb[4])
{
    g_dis.part = g_fix_disint && palette && dis_actor(palette) ? palette : 0;
    if (g_dis.part)
        memcpy(g_dis.vb, vb, sizeof g_dis.vb);
    else
        memset(g_dis.vb, 0, sizeof g_dis.vb);
}

int d3d8_disint_capturing(uint32_t palette)
{
    return g_fix_disint && palette && dis_actor(palette) != NULL;
}

int d3d8_disint_smoke(float at[4][3])
{
    if (!g_fix_disint)
        return 0;
    uint64_t now = dis_ms(), gap = now - g_dis.smoke_at;
    uint32_t hits = g_dis.last_hits < 4 ? g_dis.last_hits : 4;
    uint32_t ticks = gap > 150 ? 1u : (uint32_t)((float)gap * DIS_PSFRAME + 0.5f); /* 30 Hz frames gone by */
    ticks = ticks < hits ? ticks : hits;
    for (uint32_t e = 0; e < ticks && g_dis.nhaze < 512; ++e)
    {
        memcpy(g_dis.haze[g_dis.nhaze].p, g_dis.last_hit_at[e], 12);
        g_dis.haze[g_dis.nhaze++].born = now;
    }
    if (ticks)
    {
        g_dis.last_hits = 0;
        g_dis.smoke_at = gap > 150 ? now : g_dis.smoke_at + (uint64_t)((float)ticks / DIS_PSFRAME + 0.5f);
        memcpy(at, g_dis.last_hit_at, 12u * ticks);
    }
    return (int)ticks;
}

/* Present: this frame's spheres are used by the next frame's draws */
static void dis_present(void)
{
    g_dis.drawn = 0, g_dis.nviews = 0, g_dis.part = 0;
    memset(g_dis.vb, 0, sizeof g_dis.vb);
    if (!g_fix_disint)
    {
        if (g_dis.nactors || g_dis.npieces || g_dis.nhaze)
            dis_clear();
        return;
    }
    g_dis.last_hits = g_dis.hits;
    memcpy(g_dis.last_hit_at, g_dis.hit_at, sizeof g_dis.hit_at);
    g_dis.hits = 0;
    memcpy(g_dis.cur, g_dis.fresh, g_dis.nfresh * sizeof g_dis.cur[0]);
    g_dis.ncur = g_dis.nfresh, g_dis.nfresh = 0;
    uint64_t now = dis_ms();
    for (uint32_t i = 0; i < g_dis.ncur; ++i)
    {
        DisActor* a = dis_actor(g_dis.cur[i].bones);
        if (!a && g_dis.nactors < 8)
        {
            a = &g_dis.actors[g_dis.nactors++];
            memset(a, 0, sizeof *a);
            a->bones = g_dis.cur[i].bones;
        }
        if (a)
            a->last = now;
    }
    for (uint32_t i = 0; i < g_dis.nactors;)
        if (now - g_dis.actors[i].last > DIS_HOLD_MS)
        {
            dis_actor_free(&g_dis.actors[i]);
            g_dis.actors[i] = g_dis.actors[--g_dis.nactors];
            memset(&g_dis.actors[g_dis.nactors], 0, sizeof g_dis.actors[0]);
        }
        else
            ++i;
}

/* a body's lit colour at a vertex, as the fixed-function pipeline lights it (D3DLIGHT8, D3DMATERIAL8 as dwords) */
static uint32_t dis_lit(const State* s, const float* p, const float* nin)
{
    float n[3] = { nin[0], nin[1], nin[2] }, l2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    float inv = l2 > 1e-12f ? 1.0f / sqrtf(l2) : 0.0f, amb[3], ma[3], md[3], c[3];
    for (int j = 0; j < 3; ++j)
    {
        n[j] *= inv;
        amb[j] = (float)(s->rs[139] >> (16 - 8 * j) & 0xFF) / 255.0f; /* AMBIENT */
        md[j] = u2f(s->mat[j]), ma[j] = u2f(s->mat[4 + j]);
        c[j] = u2f(s->mat[12 + j]) + ma[j] * amb[j];
    }
    int used = 0;
    for (int i = 0; i < MAX_LIGHTS && used < 8; ++i)
    {
        const Light* L = &s->light[i];
        if (!L->enabled)
            continue;
        ++used;
        const uint32_t* v = L->v;
        float d[3], att = 1.0f;
        if (v[0] == 3) /* directional */
            d[0] = -u2f(v[16]), d[1] = -u2f(v[17]), d[2] = -u2f(v[18]);
        else
        {
            for (int j = 0; j < 3; ++j)
                d[j] = u2f(v[13 + j]) - p[j];
            float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dist > u2f(v[19]))
                continue;
            float q = u2f(v[21]) + u2f(v[22]) * dist + u2f(v[23]) * dist * dist;
            att = q > 1e-6f ? 1.0f / q : 1.0f;
        }
        float ll = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (ll < 1e-12f)
            continue;
        for (int j = 0; j < 3; ++j)
            d[j] /= ll;
        if (v[0] == 2) /* spot */
        {
            float dx = u2f(v[16]), dy = u2f(v[17]), dz = u2f(v[18]), dl = sqrtf(dx * dx + dy * dy + dz * dz);
            float rho = dl > 1e-12f ? -(d[0] * dx + d[1] * dy + d[2] * dz) / dl : 1.0f;
            float cp = cosf(u2f(v[25]) * 0.5f), ct = cosf(u2f(v[24]) * 0.5f);
            if (rho <= cp)
                continue;
            if (rho < ct && ct > cp)
                att *= powf((rho - cp) / (ct - cp), u2f(v[20]));
        }
        float nd = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
        nd = nd > 0.0f ? nd : 0.0f;
        for (int j = 0; j < 3; ++j)
            c[j] += att * (ma[j] * u2f(v[9 + j]) + md[j] * u2f(v[1 + j]) * nd);
    }
    uint32_t out = 0xFF000000u;
    for (int j = 0; j < 3; ++j)
        out |= (uint32_t)(fminf(fmaxf(c[j], 0.0f), 1.0f) * 255.0f + 0.5f) << (16 - 8 * j);
    return out;
}

/* the part draw's triangles (a list or a strip unpacked), read once */
static void dis_read(DisPart* p, uint32_t prim, uint32_t count, uint32_t indices, const Obj* ib)
{
    uint32_t n = prim == 4 ? count * 3 : count + 2;
    if (!indices || indices < ib->mem || indices + 2u * n > ib->mem + ib->size)
        return;
    p->tri = (uint16_t*)malloc(6u * count);
    p->taken = (uint8_t*)calloc(count ? count : 1, 1);
    if (!p->tri || !p->taken)
    {
        free(p->tri), free(p->taken), p->tri = NULL, p->taken = NULL;
        return;
    }
    const uint8_t* idx = (const uint8_t*)GUEST_PTR(indices);
    for (uint32_t i = 0; i < count; ++i)
    {
        uint16_t t[3];
        memcpy(t, idx + 2u * (prim == 4 ? 3 * i : i), 6);
        if (prim == 5 && (i & 1))
        {
            uint16_t x = t[0];
            t[0] = t[1], t[1] = x;
        }
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2])
            continue;
        memcpy(p->tri + 3 * p->ntri++, t, 6);
    }
}

/* a capturing actor's part draw: the triangles inside this frame's spheres taken, the rest drawn; 1 when drawn here */
static int dis_draw(uint32_t prim, uint32_t count, uint32_t indices, uint32_t isize)
{
    State* s = &g_dev.cur;
    if (!g_dis.part || (prim != 4 && prim != 5) || isize != 2)
        return 0;
    uint32_t pos = s->stream[0];
    int half = !pos || s->stride[0] != 12 ? -1 : pos == g_dis.vb[0] ? 0 : pos == g_dis.vb[1] ? 1 : -1;
    Obj* ib = obj(s->ib);
    Obj* pv = obj(pos);
    DisActor* a = dis_actor(g_dis.part);
    if (half < 0 || !ib || !ib->mem || !pv || !a)
        return 0;
    uint32_t base = s->base_vertex, start = (indices - ib->mem) / 2;
    /* keyed by the positions too: a mirrored part (the other arm) is the same index range drawn from the skinner's
     * second output, and on the PS2 each side breaks up on its own */
    uint64_t key = ((uint64_t)s->ib << 32 ^ (uint64_t)start << 12 ^ (uint64_t)count << 1 ^ (uint64_t)(prim == 5)) ^
        (((uint64_t)pos ^ (uint64_t)base << 24) * 0x9E3779B97F4A7C15ull);
    DisPart* p = NULL;
    for (uint32_t i = 0; i < a->nparts && !p; ++i)
        if (a->parts[i].key == key)
            p = &a->parts[i];
    if (!p)
    {
        if (a->nparts == a->cap)
        {
            uint32_t cap = a->cap ? a->cap * 2 : 64;
            DisPart* np = (DisPart*)realloc(a->parts, cap * sizeof *np);
            if (!np)
                return 0;
            a->parts = np, a->cap = cap;
        }
        p = &a->parts[a->nparts++];
        memset(p, 0, sizeof *p);
        p->key = key;
        dis_read(p, prim, count, indices, ib);
    }
    int any = 0;
    for (uint32_t i = 0; i < g_dis.ncur && !any; ++i)
        any = g_dis.cur[i].bones == g_dis.part;
    uint32_t verts = pv->mem ? pv->size / 12 : 0;
    if (any && p->ntaken < p->ntri && verts)
    {
        const uint8_t* pp = (const uint8_t*)GUEST_PTR(pv->mem);
        Obj* uv = obj(s->stream[2]);
        uint32_t uvs = s->stride[2];
        const uint8_t* up = uv && uv->mem && uvs >= 8 ? (const uint8_t*)GUEST_PTR(uv->mem) : NULL;
        /* the normals, skinned with the positions (stream 1); unit length, or it isn't a normal buffer */
        Obj* nv = s->rs[137] && s->stream[1] == g_dis.vb[half + 2] && s->stride[1] == 12 ? obj(s->stream[1]) : NULL;
        const uint8_t* np = nv && nv->mem && nv->size / 12 >= verts ? (const uint8_t*)GUEST_PTR(nv->mem) : NULL;
        if (np)
        {
            float n0[3];
            memcpy(n0, np, 12);
            float l2 = n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2];
            if (l2 < 0.8f || l2 > 1.2f)
                np = NULL;
        }
        uint64_t now = dis_ms();
        for (uint32_t i = 0; i < p->ntri; ++i)
        {
            if (p->taken[i])
                continue;
            float v[3][3];
            uint32_t vi[3];
            int ok = 1, in = 0;
            for (int k = 0; k < 3 && ok; ++k)
            {
                vi[k] = base + p->tri[3 * i + k];
                ok = vi[k] < verts;
                if (ok)
                    memcpy(v[k], pp + 12u * vi[k], 12);
            }
            for (uint32_t j = 0; ok && j < g_dis.ncur && !in; ++j)
            {
                const DisSphere* sp = &g_dis.cur[j];
                for (int k = 0; sp->bones == g_dis.part && k < 3 && !in; ++k)
                {
                    float x = v[k][0] - sp->c[0], y = v[k][1] - sp->c[1], z = v[k][2] - sp->c[2];
                    in = x * x + y * y + z * z < sp->r * sp->r;
                }
            }
            if (!in)
                continue;
            p->taken[i] = 1, ++p->ntaken, p->dirty = 1, ++g_dis.hits;
            uint32_t slot = g_dis.hits <= 4 ? g_dis.hits - 1 : (uint32_t)(dis_rand() * (float)g_dis.hits);
            if (slot < 4)
                for (int j = 0; j < 3; ++j)
                    g_dis.hit_at[slot][j] = (v[0][j] + v[1][j] + v[2][j]) / 3.0f;
            if (g_dis.npieces >= DIS_MAX_PIECES)
                continue;
            if (g_dis.npieces == g_dis.cappieces)
            {
                uint32_t cap = g_dis.cappieces ? g_dis.cappieces * 2 : 1024;
                DisPiece* n = (DisPiece*)realloc(g_dis.pieces, cap * sizeof *n);
                if (!n)
                    continue;
                g_dis.pieces = n, g_dis.cappieces = cap;
            }
            DisPiece* f = &g_dis.pieces[g_dis.npieces++];
            memset(f, 0, sizeof *f);
            for (int k = 0; k < 3; ++k)
            {
                memcpy(f->p[k], v[k], 12);
                if (up && (vi[k] + 1) * uvs <= uv->size)
                    memcpy(f->uv[k], up + vi[k] * uvs, 8);
                if (np)
                    f->c[k] = dis_lit(s, v[k], (const float*)(np + 12u * vi[k]));
            }
            f->lit = np != NULL;
            /* mt00's element: a random tumble up to op 0c's rates and a rise up to op 03's on op 02's, one a 30 Hz frame */
            uint64_t elem = now * 3 / 100;
            if (elem != g_dis.elem_frame)
            {
                static const float spin[3] = { 0.0314f, 0.0332f, 0.0332f };
                g_dis.elem_frame = elem;
                for (int j = 0; j < 3; ++j)
                    g_dis.elem_spin[j] = spin[j] * (dis_rand() * 2.0f - 1.0f);
                g_dis.elem_drift = DIS_PIECE_DRIFT * (dis_rand() * 2.0f - 1.0f);
            }
            memcpy(f->spin, g_dis.elem_spin, sizeof f->spin);
            f->drift = g_dis.elem_drift;
            f->born = now;
            if ((f->tex = s->tex[0]) != 0)
                obj_addref(f->tex);
        }
        static int said;
        if (!said++)
            rt_log("[recomp] d3d8: disintegrate: a model broken into pieces as on the PS2 (fix_disintegrate)\n");
    }
    /* the view this body is drawn with, for the pieces */
    uint32_t vi = 0;
    while (vi < g_dis.nviews && g_dis.views[vi].rt != g_dev.rt)
        ++vi;
    if (vi == g_dis.nviews && vi < 4)
    {
        g_dis.views[vi].rt = g_dev.rt;
        memcpy(g_dis.views[vi].view, s->xf[2], sizeof g_dis.views[vi].view); /* D3DTS_VIEW */
        memcpy(g_dis.views[vi].proj, s->xf[3], sizeof g_dis.views[vi].proj); /* D3DTS_PROJECTION */
        g_dis.nviews = vi + 1;
    }
    uint32_t bias = s->rs[47]; /* ZBIAS: a step nearer */
    s->rs[47] = bias + 1;
    if (!p->ntaken)
        draw(prim, count, 0, indices, isize, 0, 0);
    else
    {
        if (p->dirty)
        {
            p->dirty = 0, p->kept = 0;
            if (!p->ib && p->ntri)
                p->ib = gheap_alloc(6u * p->ntri, 0), p->ibsize = 6u * p->ntri;
            uint16_t* out = p->ib ? (uint16_t*)GUEST_PTR(p->ib) : NULL;
            for (uint32_t i = 0; out && i < p->ntri; ++i)
                if (!p->taken[i])
                    memcpy(out + 3 * p->kept++, p->tri + 3 * i, 6);
        }
        if (p->kept) /* else all gone: nothing to draw */
            draw(4, p->kept, 0, p->ib, 2, 0, 0);
    }
    s->rs[47] = bias;
    return 1;
}

/* room for n vertices of 24 bytes in guest memory */
static uint32_t dis_vbuf(uint32_t n)
{
    if (n * 24u > g_dis.vcap)
    {
        if (g_dis.vbuf)
            gheap_free(g_dis.vbuf);
        g_dis.vcap = n * 24u * 2u;
        g_dis.vbuf = gheap_alloc(g_dis.vcap, 0);
        if (!g_dis.vbuf)
            g_dis.vcap = 0;
    }
    return g_dis.vbuf;
}

static void dis_vtx(uint8_t* o, float x, float y, float z, uint32_t c, float u, float v)
{
    float f[3] = { x, y, z }, t[2] = { u, v };
    memcpy(o, f, 12), memcpy(o + 12, &c, 4), memcpy(o + 16, t, 8);
}

/* the target's pixels as the GPU holds them: a scaled render target's are its drawn size */
static void dis_gpu_size(const Obj* s, uint32_t* w, uint32_t* h)
{
    const Obj* c = s->container ? obj(s->container) : NULL;
    *w = c && c->pw ? c->pw : s->width, *h = c && c->pw ? c->ph : s->height;
}

/* g000's 'dist' haze: a fan of a centre and four corners (a diamond of radius 1, op 0f scaling it to 0.4) facing the
 * camera, turning 0.0698 radians and rising 0.002 a tick for 30 ticks, textured with the scene itself: each vertex
 * reads the picture at its own place shifted by (0.07, 0.07) in the element's frame. The centre's alpha climbs, holds
 * near 0x78 of 0x80 and falls; the corners have none. Depth tested, no depth write, alpha blended. */
static void dis_draw_haze(const float* V, const float* P, uint64_t now)
{
    Obj* rt = obj(g_dev.rt);
    if (!rt)
        return;
    uint32_t sf, sl, gw, gh;
    GfxTex* sg = surface_gpu(rt, &sf, &sl);
    dis_gpu_size(rt, &gw, &gh);
    Obj* ht = g_dis.haze_tex ? obj(g_dis.haze_tex) : NULL;
    if (ht && (ht->width != rt->width || ht->height != rt->height || ht->format != rt->format))
        obj_release(g_dis.haze_tex), g_dis.haze_tex = 0, ht = NULL;
    if (!ht)
    {
        g_dis.haze_tex = new_texture(O_TEXTURE, rt->width, rt->height, 1, USAGE_RENDERTARGET, rt->format, 0);
        ht = obj(g_dis.haze_tex);
    }
    uint32_t hw = ht && ht->pw ? ht->pw : ht ? ht->width : 0, hh = ht && ht->pw ? ht->ph : ht ? ht->height : 0;
    if (!sg || !ht || !ht->gpu || hw != gw || hh != gh)
        return;
    gfx_copy(sg, sf, sl, 0, 0, gw, gh, ht->gpu, 0, 0, 0, 0);
    const uint32_t* vp = g_dev.cur.vp;
    float vx = (float)vp[0], vy = (float)vp[1], vw = (float)vp[2], vh = (float)vp[3];
    static const float rim[5][2] = { { 0, -1 }, { -1, 0 }, { 0, 1 }, { 1, 0 }, { 0, -1 } };
    uint32_t buf = dis_vbuf(12u * g_dis.nhaze), n = 0;
    if (!buf)
        return;
    uint8_t* o = (uint8_t*)GUEST_PTR(buf);
    for (uint32_t i = 0; i < g_dis.nhaze; ++i)
    {
        float t = (float)(now - g_dis.haze[i].born) * DIS_TICK, u = t / DIS_HAZE_LIFE;
        float a = fminf(2.3f * (1.0f - fabsf(2.0f * u - 1.0f)), 1.0f) * 0.94f;
        a = fminf(fmaxf(a, 0.0f), 1.0f);
        float p[3] = { g_dis.haze[i].p[0], g_dis.haze[i].p[1] - 0.002f * t, g_dis.haze[i].p[2] }, c[3]; /* y is down */
        for (int j = 0; j < 3; ++j)
            c[j] = p[0] * V[j] + p[1] * V[4 + j] + p[2] * V[8 + j] + V[12 + j];
        float cs = cosf(0.0698f * t) * 0.4f, sn = sinf(0.0698f * t) * 0.4f, fan[6][6];
        for (int k = 0; k < 6; ++k)
        {
            float lx = k ? rim[k - 1][0] : 0.0f, ly = k ? rim[k - 1][1] : 0.0f, sx = lx + DIS_HAZE_SHIFT, sy = ly + DIS_HAZE_SHIFT;
            float q[3] = { c[0] + lx * cs - ly * sn, c[1] + lx * sn + ly * cs, c[2] };
            float qs[3] = { c[0] + sx * cs - sy * sn, c[1] + sx * sn + sy * cs, c[2] };
            float x = qs[0] * P[0] + qs[1] * P[4] + qs[2] * P[8] + P[12], y = qs[0] * P[1] + qs[1] * P[5] + qs[2] * P[9] + P[13];
            float w = qs[0] * P[3] + qs[1] * P[7] + qs[2] * P[11] + P[15], iw = fabsf(w) > 1e-6f ? 1.0f / w : 0.0f;
            fan[k][0] = q[0], fan[k][1] = q[1], fan[k][2] = q[2];
            fan[k][3] = k ? 0.0f : a;
            fan[k][4] = (vx + (x * iw * 0.5f + 0.5f) * vw) / (float)rt->width;
            fan[k][5] = (vy + (0.5f - y * iw * 0.5f) * vh) / (float)rt->height;
        }
        for (int k = 1; k < 5; ++k)
        {
            const int tri[3] = { 0, k, k + 1 };
            for (int m = 0; m < 3; ++m, ++n)
            {
                const float* f = fan[tri[m]];
                dis_vtx(o + 24u * n, f[0], f[1], f[2], (uint32_t)(f[3] * 255.0f) << 24 | 0xFFFFFFu, f[4], f[5]);
            }
        }
    }
    State* s = &g_dev.cur;
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    memcpy(s->xf[2], id, sizeof id);
    s->rs[27] = 1, s->rs[19] = 5, s->rs[20] = 6; /* ALPHABLENDENABLE, SRCALPHA, INVSRCALPHA */
    s->tss[0][1] = 2, s->tss[0][2] = 2;           /* SELECTARG1 TEXTURE */
    s->tss[0][4] = 3, s->tss[0][6] = 0;           /* alpha SELECTARG2 DIFFUSE */
    s->tss[0][13] = s->tss[0][14] = 3;            /* CLAMP */
    s->tex[0] = g_dis.haze_tex;
    draw(4, n / 3, 0, 0, 0, buf, 24);
}

/* the pieces (and haze), drawn before the first interface draw on a target a capturing body was drawn on */
static void dis_before_draw(void)
{
    if (!g_fix_disint || g_dis.drawn || (!g_dis.npieces && !g_dis.nhaze) || !g_dis.nviews || g_dev.cur.vs != 0x144u)
        return;
    uint32_t vi = 0;
    while (vi < g_dis.nviews && g_dis.views[vi].rt != g_dev.rt)
        ++vi;
    if (vi == g_dis.nviews) /* an offscreen interface-style pass, not the scene */
        return;
    g_dis.drawn = 1;
    uint64_t now = dis_ms();
    uint32_t w = 0;
    for (uint32_t i = 0; i < g_dis.npieces; ++i)
        if ((float)(now - g_dis.pieces[i].born) * DIS_TICK >= DIS_PIECE_LIFE)
        {
            if (g_dis.pieces[i].tex)
                obj_release(g_dis.pieces[i].tex);
        }
        else
            g_dis.pieces[w++] = g_dis.pieces[i];
    g_dis.npieces = w, w = 0;
    for (uint32_t i = 0; i < g_dis.nhaze; ++i)
        if ((float)(now - g_dis.haze[i].born) * DIS_TICK < DIS_HAZE_LIFE)
            g_dis.haze[w++] = g_dis.haze[i];
    g_dis.nhaze = w;
    if (!g_dis.npieces && !g_dis.nhaze)
        return;
    static State keep;
    keep = g_dev.cur;
    State* s = &g_dev.cur;
    const float* V = g_dis.views[vi].view;
    const float* P = g_dis.views[vi].proj;
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    /* the PS2's piece pass: depth tested, no depth write, no blending */
    s->rs[7] = 1, s->rs[14] = 0, s->rs[23] = 4;    /* ZENABLE, ZWRITEENABLE, ZFUNC LESSEQUAL */
    s->rs[8] = 3, s->rs[22] = 1, s->rs[137] = 0;   /* FILLMODE SOLID, CULLMODE NONE, LIGHTING */
    s->rs[28] = 0, s->rs[27] = 0, s->rs[15] = 0;   /* FOGENABLE, ALPHABLENDENABLE, ALPHATESTENABLE */
    s->rs[52] = 0, s->rs[168] = 0xF, s->rs[47] = 0; /* STENCILENABLE, COLORWRITEENABLE, ZBIAS */
    s->rs[151] = 0, s->rs[167] = 0;               /* VERTEXBLEND, INDEXEDVERTEXBLENDENABLE */
    s->ps = 0, s->vs = 0x142;                     /* XYZ | DIFFUSE | TEX1 */
    for (int st = 1; st < 8; ++st)
        s->tex[st] = 0, s->tss[st][1] = 1, s->tss[st][4] = 1;
    uint32_t* t0 = s->tss[0];
    t0[11] = 0, t0[24] = 0;                  /* TEXCOORDINDEX, TEXTURETRANSFORMFLAGS */
    t0[1] = 5, t0[2] = 2, t0[3] = 0;          /* MODULATE2X (TEXTURE, DIFFUSE) */
    t0[4] = 3, t0[6] = 0;                     /* alpha SELECTARG2 DIFFUSE */
    t0[28] = 1;                               /* RESULTARG CURRENT */
    t0[16] = t0[17] = 2, t0[18] = 0;          /* LINEAR, no mipmaps */
    t0[13] = t0[14] = 1;                      /* WRAP */
    memcpy(s->xf[24], id, sizeof id);        /* WORLD */
    memcpy(s->xf[2], V, sizeof id);
    memcpy(s->xf[3], P, sizeof id);
    /* one draw a run of the same texture (they come part by part) */
    uint32_t buf = dis_vbuf(3u * (g_dis.npieces < 4096 ? g_dis.npieces : 4096));
    for (uint32_t i = 0; buf && i < g_dis.npieces;)
    {
        uint32_t tex = g_dis.pieces[i].tex, n = 0;
        uint8_t* o = (uint8_t*)GUEST_PTR(buf);
        for (; i < g_dis.npieces && g_dis.pieces[i].tex == tex && n < 4096; ++i, ++n)
        {
            const DisPiece* f = &g_dis.pieces[i];
            /* y points down, so rising is -y; the element scales by op 0f's (0.8, 0.8, 1) shrinking to 0.02, and turns
             * about z, then y, then x, about its corners summed and scaled by 0.333 */
            float t = (float)(now - f->born) * DIS_TICK, rise = (DIS_PIECE_RISE + f->drift) * t;
            float k = 1.0f - (1.0f - DIS_PIECE_END) * t / DIS_PIECE_LIFE, sc[3] = { 0.8f * k, 0.8f * k, k }, cs[3], sn[3], cen[3];
            for (int j = 0; j < 3; ++j)
            {
                cs[j] = cosf(f->spin[j] * t), sn[j] = sinf(f->spin[j] * t);
                cen[j] = (f->p[0][j] + f->p[1][j] + f->p[2][j]) * 0.333f;
            }
            for (int m = 0; m < 3; ++m)
            {
                float q[3], x, y, z;
                for (int j = 0; j < 3; ++j)
                    q[j] = (f->p[m][j] - cen[j]) * sc[j];
                x = q[0] * cs[2] - q[1] * sn[2], y = q[0] * sn[2] + q[1] * cs[2], q[0] = x, q[1] = y;
                x = q[0] * cs[1] + q[2] * sn[1], z = q[2] * cs[1] - q[0] * sn[1], q[0] = x, q[2] = z;
                y = q[1] * cs[0] - q[2] * sn[0], z = q[1] * sn[0] + q[2] * cs[0], q[1] = y, q[2] = z;
                dis_vtx(o + 24u * (3 * n + (uint32_t)m), cen[0] + q[0], cen[1] + q[1] - rise, cen[2] + q[2],
                    f->lit ? f->c[m] : DIS_PIECE_COLOUR, f->uv[m][0], f->uv[m][1]);
            }
        }
        s->tex[0] = tex;
        draw(4, n, 0, 0, 0, buf, 24);
    }
    if (g_dis.nhaze)
        dis_draw_haze(V, P, now);
    g_dev.cur = keep;
}

/* this frame's button shadows: black quads, alpha blended, for button_body to find a body's own */
typedef struct BtnQuad
{
    uint32_t tex;
    float x0, y0, x1, y1, u0, v0, u1, v1;
} BtnQuad;
static BtnQuad g_btn_shadow[64];

/* 1 for a button's body drawn added over its own shadow: draw it alpha blended instead (portfix's ButtonDraw) */
static int button_body(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride)
{
    const State* s = &g_dev.cur;
    const uint32_t* rs = s->rs;
    if (prim != 5 || count != 2 || stride != 28 || s->vs != 0x144u || !data || !s->tex[0] || rs[27] != 1 || rs[19] != 5 ||
        (rs[20] != 2 && rs[20] != 6))
        return 0;
    BtnQuad q = { s->tex[0], 1e30f, 1e30f, -1e30f, -1e30f, 1e30f, 1e30f, -1e30f, -1e30f };
    int black = 1;
    for (uint32_t i = 0; i < 4; ++i)
    {
        uint32_t v = data + 28u * i, c = rd32(v + 16);
        float x = u2f(rd32(v)), y = u2f(rd32(v + 4)), u = u2f(rd32(v + 20)), t = u2f(rd32(v + 24));
        q.x0 = fminf(q.x0, x), q.x1 = fmaxf(q.x1, x), q.y0 = fminf(q.y0, y), q.y1 = fmaxf(q.y1, y);
        q.u0 = fminf(q.u0, u), q.u1 = fmaxf(q.u1, u), q.v0 = fminf(q.v0, t), q.v1 = fmaxf(q.v1, t);
        black = black && ((c >> 16) & 0xFF) <= 0x10 && ((c >> 8) & 0xFF) <= 0x10 && (c & 0xFF) <= 0x10;
    }
    if (rs[20] == 6) /* INVSRCALPHA: a shadow, if black */
    {
        if (black && g_btn_shadows < (int)(sizeof g_btn_shadow / sizeof g_btn_shadow[0]))
            g_btn_shadow[g_btn_shadows++] = q;
        return 0;
    }
    float w = q.x1 - q.x0, h = q.y1 - q.y0;
    if (!(w > 0.0f) || !(h > 0.0f))
        return 0;
    /* the body may be a pixel or so shorter or narrower than its shadow (the config window's small buttons: 48x15
     * over 48x16), from the same corner of the texture */
    float crop = fmaxf(1.5f, h * 0.1f);
    for (int i = 0; i < g_btn_shadows; ++i)
    {
        const BtnQuad* sh = &g_btn_shadow[i];
        float dx = sh->x0 - q.x0, dy = sh->y0 - q.y0;
        if (sh->tex == q.tex && fabsf(sh->x1 - sh->x0 - w) <= crop && fabsf(sh->y1 - sh->y0 - h) <= crop && dx > 0.0f &&
            dy > 0.0f && dx <= h * 0.25f && dy <= h * 0.25f && fabsf(sh->u0 - q.u0) <= 1e-3f && fabsf(sh->v0 - q.v0) <= 1e-3f &&
            fabsf(sh->u1 - q.u1) <= 0.05f && fabsf(sh->v1 - q.v1) <= 0.05f)
        {
            static int said;
            if (!said++)
                rt_log("[recomp] d3d8: button rims: a button body drawn over its shadow alpha blended (fix_button_rims)\n");
            return 1;
        }
    }
    return 0;
}

/* DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount) */
static void IDirect3DDevice8_DrawPrimitive(Guest* g)
{
    dis_before_draw();
    g_cap_esp = g->esp;
    world_draw_begin(g);
    if (g_fix_water && water_ripple(&g_dev.cur))
        water_flush(), water_hold(ARG(1), ARG(2), ARG(3));
    else
        draw(ARG(1), ARG(3), ARG(2), 0, 0, 0, 0), water_flush(); /* a held ripple layer over this one */
    g_draw_zone = 0;
    RET(D3D_OK, 4);
}

/* DrawIndexedPrimitive(PrimitiveType, minIndex, NumVertices, startIndex, primCount): the vertex range
 * comes from the indices themselves, not minIndex and NumVertices */
static void IDirect3DDevice8_DrawIndexedPrimitive(Guest* g)
{
    water_flush();
    dis_before_draw();
    g_cap_esp = g->esp;
    world_draw_begin(g);
    Obj* ib = obj(g_dev.cur.ib);
    if (ib && ib->mem)
    {
        uint32_t isize = ib->format == FMT_INDEX32 ? 4 : 2;
        uint32_t at = ib->mem + ARG(4) * isize;
        if (!(g_fix_disint && dis_draw(ARG(1), ARG(5), at, isize)) && !(g_fix_shine && shine_draw(ARG(1), ARG(5), at, isize)))
            draw(ARG(1), ARG(5), 0, at, isize, 0, 0);
    }
    else
        gfx_prof_skip(GFX_SKIP_NO_INDICES);
    g_draw_zone = 0;
    RET(D3D_OK, 6);
}

/* DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride).
 * The ...UP draws leave stream 0 (and the indices) unset, as D3D8 does. */
/* A glyph quad from a texture whose pack entry has a glyph table: FFXI draws each glyph of its name
 * and small menu fonts as a fixed-width cell from where the glyph starts in the sheet, so the cell also
 * takes in the edge of the glyph beside it there (shown between letters) and cuts the glyph's own lean
 * and outline. The quad is drawn over the ink box of the glyph that starts in the cell instead: the
 * same texels where they overlap, so it lands where the game put it. */
static uint32_t g_quad_buf[3]; /* fit_quad's, widen_quad's and inset_quad's quads (d3d8_setup) */

static uint32_t fit_quad(uint32_t data, const Obj* t)
{
    const PackEntry* e = (const PackEntry*)t->repl_entry;
    uint32_t buf = g_quad_buf[0]; /* (made at setup: the worker has no guest heap) */
    float x[4], u[4], v[4], ulo = 1e30f, uhi = -1e30f, vlo = 1e30f, vhi = -1e30f, xlo = 1e30f, xhi = -1e30f;
    for (int i = 0; i < 4; ++i)
    {
        x[i] = u2f(rd32(data + 28u * (uint32_t)i)), u[i] = u2f(rd32(data + 28u * (uint32_t)i + 20)) * (float)t->width;
        v[i] = u2f(rd32(data + 28u * (uint32_t)i + 24)) * (float)t->height;
        ulo = u[i] < ulo ? u[i] : ulo, uhi = u[i] > uhi ? u[i] : uhi, vlo = v[i] < vlo ? v[i] : vlo, vhi = v[i] > vhi ? v[i] : vhi;
        xlo = x[i] < xlo ? x[i] : xlo, xhi = x[i] > xhi ? x[i] : xhi;
    }
    if (!(uhi > ulo) || !(xhi > xlo))
        return data;
    const float* best = NULL;
    for (uint32_t k = 0; k < e->nglyphs; ++k)
    {
        const float* g = e->glyphs + 4 * k;
        float cy = (g[2] + g[3]) * 0.5f;
        if (cy < vlo || cy > vhi || g[0] < ulo - 3.0f || g[0] >= uhi)
            continue;
        if (!best || g[0] < best[0])
            best = g;
    }
    if (!best)
        return data;
    float k = (xhi - xlo) / (uhi - ulo);
    memcpy(GUEST_PTR(buf), GUEST_PTR(data), 4 * 28);
    for (int i = 0; i < 4; ++i)
    {
        float nu = u[i] - ulo < uhi - u[i] ? best[0] : best[1];
        wr32(buf + 28u * (uint32_t)i, f2u(x[i] + (nu - u[i]) * k));
        wr32(buf + 28u * (uint32_t)i + 20, f2u(nu / (float)t->width));
    }
    return buf;
}

/* A glyph quad (a strip of four XYZRHW + diffuse + UV vertices) from a texture whose pack entry
 * asks for it, drawn repl_pad texels wider on each side, its UVs moved with it: FFXI cuts each
 * glyph of its italic name font at an upright box, so the lean and the outline at the ends of a
 * name were lost (the neighbours hide it inside the name). A copy: the game's vertices stay. */
static uint32_t widen_quad(uint32_t data, const Obj* t)
{
    uint32_t buf = g_quad_buf[1]; /* (made at setup: the worker has no guest heap) */
    float x[4], u[4], ulo = 1e30f, uhi = -1e30f, xlo = 1e30f, xhi = -1e30f, vlo = 1e30f, vhi = -1e30f;
    for (int i = 0; i < 4; ++i)
    {
        float v = u2f(rd32(data + 28u * (uint32_t)i + 24));
        x[i] = u2f(rd32(data + 28u * (uint32_t)i)), u[i] = u2f(rd32(data + 28u * (uint32_t)i + 20));
        ulo = u[i] < ulo ? u[i] : ulo, uhi = u[i] > uhi ? u[i] : uhi;
        xlo = x[i] < xlo ? x[i] : xlo, xhi = x[i] > xhi ? x[i] : xhi;
        vlo = v < vlo ? v : vlo, vhi = v > vhi ? v : vhi;
    }
    if (!(uhi > ulo) || !(xhi > xlo) || (vhi - vlo) * (float)t->height < (float)t->repl_pad_min)
        return data;
    float du = (float)t->repl_pad / (float)t->width, dx = du * (xhi - xlo) / (uhi - ulo);
    memcpy(GUEST_PTR(buf), GUEST_PTR(data), 4 * 28);
    for (int i = 0; i < 4; ++i)
    {
        int lo = u[i] - ulo < uhi - u[i];
        wr32(buf + 28u * (uint32_t)i, f2u(x[i] + (lo ? -dx : dx)));
        wr32(buf + 28u * (uint32_t)i + 20, f2u(u[i] + (lo ? -du : du)));
    }
    return buf;
}

/* An axis-aligned sprite quad drawn into a render target native_size enlarged: at the menu
 * resolution a quad's edges fall between pixels, but enlarged (2.5x) an edge can fall on a pixel's
 * centre, which then samples the quad's texture exactly at its edge - and a hair past it, wrapping to
 * the far side of the sheet. FFXI draws its wider menu buttons as two strips side by side, the right
 * one starting at u 0 of a sheet whose last column is transparent there: the alpha test dropped that
 * pixel, a black line down the button (the selected one, a pixel off the others, lands on a centre);
 * Config > Modern's pills stretch a body from u 0 of the same sheet. The UVs are pulled in by a sliver of a texel, which point sampling does not otherwise show. A copy:
 * the game's vertices stay. */
static int g_cap_inset; /* the drawlog: what inset_quad did with the draw it notes (-1 not a sprite, 1 inset) */

static uint32_t inset_quad(uint32_t data, const Obj* t)
{
    g_cap_inset = -1;
    Obj* rt = obj(g_dev.rt);
    Obj* rtt = rt && rt->container ? obj(rt->container) : NULL;
    if (!rtt || !rtt->pw || g_dev.cur.tss[0][16] != 1 || !t->width || !t->height)
        return data;
    float ex = (1.0f / 64.0f) / (float)t->width, ey = (1.0f / 64.0f) / (float)t->height;
    float x[4], y[4], u[4], v[4];
    for (int i = 0; i < 4; ++i)
    {
        x[i] = u2f(rd32(data + 28u * (uint32_t)i)), y[i] = u2f(rd32(data + 28u * (uint32_t)i + 4));
        u[i] = u2f(rd32(data + 28u * (uint32_t)i + 20)), v[i] = u2f(rd32(data + 28u * (uint32_t)i + 24));
    }
    /* axis aligned: vertices 0 and 1 share a row, 0 and 2 a column (a strip) */
    if (fabsf(y[0] - y[1]) > 0.01f || fabsf(y[2] - y[3]) > 0.01f || fabsf(x[0] - x[2]) > 0.01f ||
        fabsf(x[1] - x[3]) > 0.01f || fabsf(v[0] - v[1]) > 1e-6f || fabsf(u[0] - u[2]) > 1e-6f)
        return data;
    g_cap_inset = 1;
    uint32_t buf = g_quad_buf[2]; /* (made at setup: the worker has no guest heap) */
    memcpy(GUEST_PTR(buf), GUEST_PTR(data), 4 * 28);
    float uc = (u[0] + u[1]) * 0.5f, vc = (v[0] + v[2]) * 0.5f;
    for (int i = 0; i < 4; ++i)
    {
        wr32(buf + 28u * (uint32_t)i + 20, f2u(u[i] + (u[i] < uc ? ex : u[i] > uc ? -ex : 0.0f)));
        wr32(buf + 28u * (uint32_t)i + 24, f2u(v[i] + (v[i] < vc ? ey : v[i] > vc ? -ey : 0.0f)));
    }
    return buf;
}

static void IDirect3DDevice8_DrawPrimitiveUP(Guest* g)
{
    water_flush();
    dis_before_draw();
    g_cap_esp = g->esp;
    if (!(g_dev.cur.vs & 1) && (g_dev.cur.vs & 0xE) == 4 && ARG(3) && rd32(ARG(3) + 8) == 0x3f7ffffeu)
        g_probe_sky = 1; /* transformed vertices at the sky's depth */
    uint32_t data = ARG(3);
    Obj* t0 = obj(g_dev.cur.tex[0]);
    if (t0 && t0->repl && ARG(1) == 5 && ARG(2) == 2 && ARG(4) == 28 && g_dev.cur.vs == 0x144)
    {
        if (t0->repl_entry && ((const PackEntry*)t0->repl_entry)->nglyphs)
            data = fit_quad(data, t0);
        else if (t0->repl_pad)
            data = widen_quad(data, t0);
    }
    else if (t0 && ARG(1) == 5 && ARG(2) == 2 && ARG(4) == 28 && g_dev.cur.vs == 0x144)
        data = inset_quad(data, t0);
    uint32_t halo = g_fix_halos ? halo_quad(ARG(1), ARG(2), data, ARG(4)) : 0;
    if (halo)
    {
        uint32_t t = g_dev.cur.tex[0];
        g_dev.cur.tex[0] = g_halo_src; /* the full-size silhouette in place of its copy */
        draw(ARG(1), ARG(2), 0, 0, 0, halo, ARG(4));
        g_dev.cur.tex[0] = t;
    }
    else if (g_fix_buttons && button_body(ARG(1), ARG(2), ARG(3), ARG(4)))
    {
        uint32_t dst = g_dev.cur.rs[20];
        g_dev.cur.rs[20] = 6; /* DESTBLEND INVSRCALPHA */
        draw(ARG(1), ARG(2), 0, 0, 0, data, ARG(4));
        g_dev.cur.rs[20] = dst;
    }
    else
        draw(ARG(1), ARG(2), 0, 0, 0, data, ARG(4));
    bind(&g_dev.cur.stream[0], 0);
    g_dev.cur.stride[0] = 0;
    RET(D3D_OK, 5);
}

/* DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData,
 * IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride) */
static void IDirect3DDevice8_DrawIndexedPrimitiveUP(Guest* g)
{
    water_flush();
    dis_before_draw();
    g_cap_esp = g->esp;
    draw(ARG(1), ARG(4), 0, ARG(5), ARG(6) == FMT_INDEX32 ? 4 : 2, ARG(7), ARG(8));
    bind(&g_dev.cur.stream[0], 0);
    g_dev.cur.stride[0] = 0;
    bind(&g_dev.cur.ib, 0);
    RET(D3D_OK, 9);
}

/* --- IDirect3DDevice8: shaders and streams ------------------------------------------------------------------ */
static uint32_t* copy_tokens(uint32_t p, uint32_t end, uint32_t* n)
{
    uint32_t k = 0;
    while (k < 65536 && rd32(p + 4 * k) != end)
        k++;
    k++;
    uint32_t* t = (uint32_t*)malloc(4 * k);
    memcpy(t, GUEST_PTR(p), 4 * k);
    *n = k;
    return t;
}

static uint32_t type_size(uint32_t t)
{
    return t == GFX_FLOAT2 || t == GFX_SHORT4 ? 8 : t == GFX_FLOAT3 ? 12 : t == GFX_FLOAT4 ? 16 : 4;
}

static void add_elem(Layout* l, int reg, uint32_t stream, uint32_t type, uint32_t* off)
{
    if (reg >= GFX_NREGS)
        return;
    l->el[reg] = (GfxElem){ 1, (uint8_t)stream, (uint8_t)type, 0 };
    l->offset[reg] = (int32_t)*off;
    *off += type_size(type);
}

/* an FVF code: everything in stream 0, in D3D's order */
static void fvf_layout(uint32_t fvf, Layout* l)
{
    memset(l, 0, sizeof *l);
    uint32_t off = 0, pos = fvf & 0xE;
    if (pos == 4) /* XYZRHW */
    {
        add_elem(l, GFX_R_POSITION, 0, GFX_FLOAT4, &off);
        l->rhw = 1;
    }
    else if (pos)
    {
        add_elem(l, GFX_R_POSITION, 0, GFX_FLOAT3, &off);
        if (pos >= 6) /* XYZB1..5: blend weights, the last one indices with LASTBETA_UBYTE4 */
        {
            int betas = (int)(pos - 4) / 2, weights = betas - ((fvf & 0x1000) ? 1 : 0);
            if (weights > 4)
                weights = 4;
            if (weights > 0)
                add_elem(l, GFX_R_BLENDWEIGHT, 0, GFX_FLOAT1 + (uint32_t)weights - 1, &off);
            if (fvf & 0x1000)
                add_elem(l, GFX_R_BLENDINDICES, 0, GFX_UBYTE4, &off);
        }
    }
    if (fvf & 0x10)
        add_elem(l, GFX_R_NORMAL, 0, GFX_FLOAT3, &off);
    if (fvf & 0x20)
        add_elem(l, GFX_R_PSIZE, 0, GFX_FLOAT1, &off);
    if (fvf & 0x40)
        add_elem(l, GFX_R_DIFFUSE, 0, GFX_D3DCOLOR, &off);
    if (fvf & 0x80)
        add_elem(l, GFX_R_SPECULAR, 0, GFX_D3DCOLOR, &off);
    uint32_t n = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < n && i < 8; ++i)
    {
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        add_elem(l, GFX_R_TEXCOORD0 + (int)i, 0, f == 0 ? GFX_FLOAT2 : f == 1 ? GFX_FLOAT3 : f == 2 ? GFX_FLOAT4 : GFX_FLOAT1, &off);
    }
}

/* a vertex shader declaration: D3DVSD_STREAM, then D3DVSD_REG / D3DVSD_SKIP per element */
static void decl_layout(const uint32_t* t, uint32_t n, Layout* l)
{
    memset(l, 0, sizeof *l);
    uint32_t stream = 0, off = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t tok = t[i];
        if (tok == 0xFFFFFFFFu)
            break;
        switch (tok >> 29)
        {
        case 1: stream = tok & 0xF, off = 0; break;
        case 2:
            if ((tok >> 28) & 1)
                off += 4 * ((tok >> 16) & 0xF);
            else
                add_elem(l, (int)(tok & 0x1F), stream, (tok >> 16) & 0xF, &off);
            break;
        case 4: i += 4 * ((tok >> 25) & 0xF); break; /* constants: SetVertexShader loads them */
        case 5: i += (tok >> 24) & 0x1F; break;
        default: break;
        }
    }
}

static uint32_t token_hash(const uint32_t* t, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i)
        for (int b = 0; b < 32; b += 8)
            h = (h ^ ((t[i] >> b) & 255)) * 16777619u;
    return h ? h : 1;
}

/* CreateVertexShader(pDeclaration, pFunction, pHandle, Usage). Handles are odd: D3D8 tells a shader
 * from an FVF code (bit 0 always clear) by that bit. */
static void IDirect3DDevice8_CreateVertexShader(Guest* g)
{
    Dev* d = &g_dev;
    uint32_t i = 0;
    while (i < d->nvs && d->vs[i].decl)
        i++;
    if (i == d->nvs)
    {
        d->vs = (Shader*)realloc(d->vs, (d->nvs + 1) * sizeof *d->vs);
        memset(&d->vs[d->nvs++], 0, sizeof(Shader));
    }
    Shader* s = &d->vs[i];
    s->decl = copy_tokens(ARG(1), 0xFFFFFFFFu, &s->ndecl);
    s->func = ARG(2) ? copy_tokens(ARG(2), 0x0000FFFFu, &s->nfunc) : NULL;
    if (!ARG(2))
        s->nfunc = 0;
    decl_layout(s->decl, s->ndecl, &s->lay);
    s->hash = s->func ? token_hash(s->func, s->nfunc) : 0;
    wr32(ARG(3), ((i + 1) << 1) | 1);
    RET(D3D_OK, 5);
}

static Shader* vs_of(uint32_t h)
{
    uint32_t i = (h >> 1) - 1;
    return (h & 1) && i < g_dev.nvs && g_dev.vs[i].decl ? &g_dev.vs[i] : NULL;
}

static void IDirect3DDevice8_SetVertexShader(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    s->vs = ARG(1);
    if (m)
        m->vs = 1;
    /* D3DVSD_CONST in the declaration: constants loaded with the shader */
    Shader* sh = vs_of(ARG(1));
    for (uint32_t i = 0; sh && i < sh->ndecl && sh->decl[i] != 0xFFFFFFFFu; ++i)
    {
        uint32_t tok = sh->decl[i];
        if (tok >> 29 == 4)
        {
            uint32_t count = (tok >> 25) & 0xF, reg = tok & 0x7F;
            for (uint32_t k = 0; k < count && reg + k < NVSC && i + 1 + 4 * k + 3 < sh->ndecl; ++k)
            {
                memcpy(s->vsc[reg + k], &sh->decl[i + 1 + 4 * k], 16);
                s->ver_vsc = state_ver();
                raise_top(&g_vsc_top, reg + k + 1);
                if (m)
                    m->vsc[reg + k] = 1;
            }
            i += 4 * count;
        }
        else if (tok >> 29 == 5)
            i += (tok >> 24) & 0x1F;
    }
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetVertexShader(Guest* g)
{
    wr32(ARG(1), g_dev.cur.vs);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_DeleteVertexShader(Guest* g)
{
    Shader* s = vs_of(ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    free(s->decl);
    free(s->func);
    memset(s, 0, sizeof *s);
    if (g_dev.cur.vs == ARG(1))
        g_dev.cur.vs = 0;
    RET(D3D_OK, 2);
}

/* Set/GetVertexShaderConstant(Register, pConstantData, ConstantCount) */
static void IDirect3DDevice8_SetVertexShaderConstant(Guest* g)
{
    uint32_t r = ARG(1), n = ARG(3);
    if (r >= NVSC || n > NVSC - r)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    memcpy(s->vsc[r], ARGP(2), 16 * n);
    s->ver_vsc = state_ver();
    raise_top(&g_vsc_top, r + n);
    if (m)
        memset(&m->vsc[r], 1, n);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_GetVertexShaderConstant(Guest* g)
{
    uint32_t r = ARG(1), n = ARG(3);
    if (r >= NVSC || n > NVSC - r)
        RET(D3DERR_INVALIDCALL, 4);
    memcpy(ARGP(2), g_dev.cur.vsc[r], 16 * n);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_SetStreamSource(Guest* g)
{
    if (ARG(1) >= NSTREAMS)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    bind(&s->stream[ARG(1)], ARG(2));
    s->stride[ARG(1)] = ARG(3);
    if (m)
        m->stream[ARG(1)] = 1;
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_GetStreamSource(Guest* g)
{
    if (ARG(1) >= NSTREAMS)
        RET(D3DERR_INVALIDCALL, 4);
    uint32_t p = g_dev.cur.stream[ARG(1)];
    if (p)
        obj_addref(p);
    wr32(ARG(2), p);
    wr32(ARG(3), g_dev.cur.stride[ARG(1)]);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_SetIndices(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    bind(&s->ib, ARG(1));
    s->base_vertex = ARG(2);
    if (m)
        m->ib = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetIndices(Guest* g)
{
    uint32_t p = g_dev.cur.ib;
    if (p)
        obj_addref(p);
    wr32(ARG(1), p);
    wr32(ARG(2), g_dev.cur.base_vertex);
    RET(D3D_OK, 3);
}

/* CreatePixelShader(pFunction, pHandle) */
static void IDirect3DDevice8_CreatePixelShader(Guest* g)
{
    Dev* d = &g_dev;
    uint32_t i = 0;
    while (i < d->nps && d->ps[i].func)
        i++;
    if (i == d->nps)
    {
        d->ps = (Shader*)realloc(d->ps, (d->nps + 1) * sizeof *d->ps);
        memset(&d->ps[d->nps++], 0, sizeof(Shader));
    }
    d->ps[i].func = copy_tokens(ARG(1), 0x0000FFFFu, &d->ps[i].nfunc);
    d->ps[i].hash = token_hash(d->ps[i].func, d->ps[i].nfunc);
    wr32(ARG(2), i + 1);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetPixelShader(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    s->ps = ARG(1);
    if (m)
        m->ps = 1;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetPixelShader(Guest* g)
{
    wr32(ARG(1), g_dev.cur.ps);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_DeletePixelShader(Guest* g)
{
    uint32_t i = ARG(1) - 1;
    if (!ARG(1) || i >= g_dev.nps || !g_dev.ps[i].func)
        RET(D3DERR_INVALIDCALL, 2);
    free(g_dev.ps[i].func);
    memset(&g_dev.ps[i], 0, sizeof(Shader));
    if (g_dev.cur.ps == ARG(1))
        g_dev.cur.ps = 0;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetPixelShaderConstant(Guest* g)
{
    uint32_t r = ARG(1), n = ARG(3);
    if (r >= NPSC || n > NPSC - r)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    memcpy(s->psc[r], ARGP(2), 16 * n);
    s->ver_psc = state_ver();
    if (m)
        memset(&m->psc[r], 1, n);
    RET(D3D_OK, 4);
}

/* --- drawing: the device state as the back end's draw packet ------------------------------------------------ */
/* D3D's v * M with row-major matrices: r = a * b */
static void mat_mul(float* r, const float* a, const float* b)
{
    float t[16];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            t[y * 4 + x] = a[y * 4] * b[x] + a[y * 4 + 1] * b[4 + x] + a[y * 4 + 2] * b[8 + x] + a[y * 4 + 3] * b[12 + x];
    memcpy(r, t, sizeof t);
}

/* the normal matrix: the inverse transpose of m's upper 3x3 (D3D lights in camera space) */
static void normal_matrix(float* r, const float* m)
{
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    float s = det != 0.0f ? 1.0f / det : 0.0f;
    memset(r, 0, 64);
    /* inverse transpose = cofactor matrix / det */
    r[0] = A * s, r[1] = B * s, r[2] = C * s;
    r[4] = -(b * i - c * h) * s, r[5] = (a * i - c * g) * s, r[6] = -(a * h - b * g) * s;
    r[8] = (b * f - c * e) * s, r[9] = -(a * f - c * d) * s, r[10] = (a * e - b * d) * s;
    r[15] = 1.0f;
}

static void xform(float* out, const float* v, float w, const float* m) /* (v, w) * m, xyz */
{
    for (int k = 0; k < 3; ++k)
        out[k] = v[0] * m[k] + v[1] * m[4 + k] + v[2] * m[8 + k] + w * m[12 + k];
}

static void color4(float* out, uint32_t c)
{
    out[0] = ((c >> 16) & 255) / 255.0f, out[1] = ((c >> 8) & 255) / 255.0f, out[2] = (c & 255) / 255.0f, out[3] = (c >> 24) / 255.0f;
}

/* vertices a D3D primitive count spans */
static uint32_t prim_vertices(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case 1: return n;
    case 2: return n * 2;
    case 3: return n + 1;
    case 4: return n * 3;
    case 5:
    case 6: return n + 2;
    default: return 0;
    }
}

static void apply_targets(void)
{
    uint32_t cf = 0, cl = 0, df, dl;
    Obj* rt = obj(g_dev.rt);
    Obj* ds = obj(g_dev.ds);
    GfxTex* c = rt ? surface_gpu(rt, &cf, &cl) : NULL;
    GfxTex* z = ds ? surface_gpu(ds, &df, &dl) : NULL;
    gfx_set_targets(c, cf, cl, z);
}

static GfxDraw g_draw; /* one device, one draw at a time */
static struct
{
    int on;
    uint32_t stride, stream[GFX_NREGS], from[GFX_NREGS]; /* the device's stream and offset per element */
    uint8_t* buf;
    size_t cap;
} g_merge;

/* The merged stream for vertices first .. first + count: each element copied from its own stream. */
static int merge_streams(GfxDraw* d, uint32_t first, uint32_t count)
{
    size_t need = (size_t)count * g_merge.stride;
    if (need > g_merge.cap)
    {
        uint8_t* p = (uint8_t*)realloc(g_merge.buf, need);
        if (!p)
            return 0;
        g_merge.buf = p, g_merge.cap = need;
    }
    for (int r = 0; r < GFX_NREGS; ++r)
    {
        if (!d->vs.el[r].used)
            continue;
        uint32_t st = g_merge.stream[r], stride = g_dev.cur.stride[st], n = type_size(d->vs.el[r].type);
        Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return gfx_prof_skip(GFX_SKIP_NO_BUFFER), 0;
        const uint8_t* src = (const uint8_t*)GUEST_PTR(b->mem);
        uint8_t* dst = g_merge.buf + d->u.offset[r];
        for (uint32_t i = 0; i < count; ++i, dst += g_merge.stride)
        {
            uint32_t at = (first + i) * stride + g_merge.from[r];
            if (at + n > b->size)
                memset(dst, 0, n);
            else
                memcpy(dst, src + at, n);
        }
    }
    d->data[0] = g_merge.buf;
    d->size[0] = (uint32_t)need;
    d->u.stride[0] = (int32_t)g_merge.stride;
    return 1;
}

/* The keys and uniforms for the current state; 0 when the state cannot be drawn. */
/* The game's water models lifted water_lift units up their own vertical (y up the model is down the world), in front of
 * the zone surfaces laid at the water's level. Bastok Markets' harbour has untextured zone tiles at y 8.00 and the sea's
 * model at 8.10, ten centimetres under them; the game draws the water with its near plane 0.3% out (D3DTS_PROJECTION
 * [14] -0.1003 for -0.1), which puts it in front only from some 33 units away: nearer, the tiles hid it and the sea
 * was a flat grey wherever the camera came close to it. */
static void water_lift(GfxDraw* d, const State* s)
{
    float lift = gfx_fx_get("water_lift");
    if (lift <= 0.0f)
        return;
    const float* W = s->xf[24];
    float n = sqrtf(W[4] * W[4] + W[5] * W[5] + W[6] * W[6]);
    if (n <= 0.0f)
        return;
    float o[4] = { -lift * W[4] / n, -lift * W[5] / n, -lift * W[6] / n, 0.0f }, vp[16], r[4];
    mat_mul(vp, s->xf[2], s->xf[3]);
    for (int j = 0; j < 4; ++j)
        r[j] = o[0] * vp[j] + o[1] * vp[4 + j] + o[2] * vp[8 + j];
    for (int j = 0; j < 4; ++j)
        d->u.wvp[12 + j] += r[j];
}

static int build_draw(GfxDraw* d)
{
    State* s = &g_dev.cur;
    const uint32_t* rs = s->rs;
    /* the keys and bindings, and the uniforms up to the lights; lights and shader constants are
     * written where they are used (the back end uploads only those) */
    /* the blocks of u kept from the draw before when their versions say they are the same (State.ver_*): what
     * this draw's u holds of them, its version; d is always g_draw */
    static uint32_t have[GFX_NUV];
    if (d != &g_draw)
        memset(have, 0, sizeof have);
    memset(d, 0, offsetof(GfxDraw, u));
    /* (wvp, wv, wvit and texm are written below whatever the draw) */
    memset(d->u.mat_d, 0, offsetof(GfxU, light) - offsetof(GfxU, mat_d));
    memset(d->tex, 0, sizeof(GfxDraw) - offsetof(GfxDraw, tex));

    Layout fvf;
    const Layout* lay;
    Shader* vs = NULL;
    if (s->vs & 1)
    {
        vs = vs_of(s->vs);
        if (!vs)
            return gfx_prof_skip(GFX_SKIP_NO_SHADER), 0;
        lay = &vs->lay;
    }
    else
    {
        fvf_layout(s->vs, &fvf);
        lay = &fvf;
    }
    /* D3D8 has 16 streams, the back ends GFX_NSTREAMS. A layout that reads past them (the character
     * creation models' hair reads streams 0-4) is drawn from one stream that set_streams interleaves
     * from the device's: g_merge holds where each element comes from. */
    g_merge.on = 0;
    for (int r = 0; r < GFX_NREGS; ++r)
        if (lay->el[r].used && lay->el[r].stream >= GFX_NSTREAMS)
            g_merge.on = 1;
    uint32_t merged = 0;
    for (int r = 0; r < GFX_NREGS; ++r)
    {
        d->vs.el[r] = lay->el[r];
        d->u.offset[r] = lay->offset[r];
        if (g_merge.on && lay->el[r].used)
        {
            g_merge.stream[r] = lay->el[r].stream, g_merge.from[r] = lay->offset[r];
            d->vs.el[r].stream = 0;
            d->u.offset[r] = (int32_t)merged;
            merged += (type_size(lay->el[r].type) + 3) & ~3u;
        }
    }
    g_merge.stride = merged;
    if (!lay->el[GFX_R_POSITION].used && !(vs && vs->func))
        return gfx_prof_skip(GFX_SKIP_NO_POSITION), 0;
    d->vs.rhw = lay->rhw;
    if (vs && vs->func)
    {
        d->vs.prog = vs->hash;
        d->vs_tokens = vs->func;
        d->vs_token_count = vs->nfunc;
        if (!s->ver_vsc || have[GFX_UV_VSC] != s->ver_vsc)
        {
            memcpy(d->u.vsc, s->vsc, sizeof d->u.vsc);
            have[GFX_UV_VSC] = s->ver_vsc;
        }
    }
    if (s->ps)
    {
        uint32_t i = s->ps - 1;
        if (i < g_dev.nps && g_dev.ps[i].func)
        {
            d->fs.prog = g_dev.ps[i].hash;
            d->ps_tokens = g_dev.ps[i].func;
            d->ps_token_count = g_dev.ps[i].nfunc;
            if (!s->ver_psc || have[GFX_UV_PSC] != s->ver_psc)
            {
                memcpy(d->u.psc, s->psc, sizeof d->u.psc);
                have[GFX_UV_PSC] = s->ver_psc;
            }
        }
    }

    /* transforms: WORLD is D3DTS_WORLDMATRIX(0), index 24 here */
    /* (the products and the normals' matrix are those of the draw before while world, view and projection are:
     * most draws of a model share them) */
    static struct { float in[3][16], wv[16], wvp[16], wvit[16]; int ok; } xc;
    if (!xc.ok || memcmp(xc.in[0], s->xf[24], 64) || memcmp(xc.in[1], s->xf[2], 64) || memcmp(xc.in[2], s->xf[3], 64))
    {
        memcpy(xc.in[0], s->xf[24], 64), memcpy(xc.in[1], s->xf[2], 64), memcpy(xc.in[2], s->xf[3], 64);
        mat_mul(xc.wv, s->xf[24], s->xf[2]);
        mat_mul(xc.wvp, xc.wv, s->xf[3]);
        normal_matrix(xc.wvit, xc.wv);
        xc.ok = 1;
    }
    memcpy(d->u.wvp, xc.wvp, 64);
    memcpy(d->u.wv, xc.wv, 64);
    memcpy(d->u.wvit, xc.wvit, 64);
    if (!s->ver_texm || have[GFX_UV_TEXM] != s->ver_texm)
    {
        for (int i = 0; i < 8; ++i)
            memcpy(d->u.texm[i], s->xf[16 + i], 64);
        have[GFX_UV_TEXM] = s->ver_texm;
    }
    memcpy(d->u_ver, have, sizeof d->u_ver);

    /* D3DMATERIAL8: Diffuse, Ambient, Specular, Emissive, Power */
    memcpy(d->u.mat_d, &s->mat[0], 16);
    memcpy(d->u.mat_a, &s->mat[4], 16);
    memcpy(d->u.mat_s, &s->mat[8], 16);
    memcpy(d->u.mat_e, &s->mat[12], 16);
    d->u.params[0] = u2f(s->mat[16]);
    d->u.params[1] = (float)(rs[24] & 255); /* ALPHAREF */
    d->u.params[2] = u2f(rs[36]);           /* FOGSTART */
    d->u.params[3] = u2f(rs[37]);           /* FOGEND */
    d->u.params2[0] = u2f(rs[38]);          /* FOGDENSITY */
    color4(d->u.ambient, rs[139]);
    color4(d->u.tfactor, rs[60]);
    color4(d->u.fogcolor, rs[34]);
    for (int i = 0; i < 4; ++i)
        d->u.vp[i] = (float)s->vp[i];
    memcpy(d->vp, s->vp, sizeof d->vp);
    scale_to_target(d->vp, NULL, 0); /* the XYZRHW mapping (u.vp) stays in the game's pixels */

    int ff_vertex = !d->vs.prog && !lay->rhw;
    if (ff_vertex && rs[137]) /* LIGHTING */
    {
        d->vs.lighting = 1;
        d->vs.normalize = rs[143] != 0;
        d->vs.localviewer = rs[142] != 0;
        d->vs.specular = rs[29] != 0;
        if (rs[141]) /* COLORVERTEX: the material sources */
        {
            d->vs.src_diffuse = (uint8_t)rs[145], d->vs.src_specular = (uint8_t)rs[146];
            d->vs.src_ambient = (uint8_t)rs[147], d->vs.src_emissive = (uint8_t)rs[148];
        }
        const float* view = s->xf[2];
        int n = 0;
        for (int i = 0; i < g_light_top && n < GFX_NLIGHTS; ++i)
        {
            const Light* l = &s->light[i];
            uint32_t type = l->v[0];
            if (!l->enabled || type < 1 || type > 3)
                continue;
            /* D3DLIGHT8: Type, Diffuse, Specular, Ambient, Position, Direction, Range, Falloff,
             * Attenuation0..2, Theta, Phi */
            float v[26];
            memcpy(v, l->v, sizeof v);
            GfxLight* gl = &d->u.light[n];
            memcpy(gl->diffuse, &v[1], 16);
            memcpy(gl->specular, &v[5], 16);
            memcpy(gl->ambient, &v[9], 16);
            xform(gl->pos, &v[13], 1.0f, view);
            gl->pos[3] = v[19];
            float dir[3];
            xform(dir, &v[16], 0.0f, view);
            float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            float k = len > 0 ? (type == 3 ? -1.0f : 1.0f) / len : 0.0f; /* directional: toward the light */
            gl->dir[0] = dir[0] * k, gl->dir[1] = dir[1] * k, gl->dir[2] = dir[2] * k, gl->dir[3] = v[20];
            gl->att[0] = v[21], gl->att[1] = v[22], gl->att[2] = v[23];
            gl->spot[0] = cosf(v[24] * 0.5f), gl->spot[1] = cosf(v[25] * 0.5f);
            d->vs.light_type[n++] = (uint8_t)type;
        }
        d->vs.nlights = (uint8_t)n;
    }

    if (rs[28]) /* FOGENABLE: table (pixel) fog, else vertex fog, else the vertices' specular alpha */
    {
        if (rs[35])
            d->fs.fog = (uint8_t)rs[35];
        else
        {
            d->fs.fog = 4;
            if (ff_vertex)
                d->vs.fog_vertex = (uint8_t)rs[140];
        }
        d->vs.range_fog = ff_vertex && rs[48];
    }

    /* the texture stages, up to the first disabled one */
    int nst = 0;
    if (!d->fs.prog)
        while (nst < 8 && s->tss[nst][1] != 1)
            nst++;
    d->fs.nstages = (uint8_t)nst;
    int nused = d->fs.prog ? 4 : nst, repl0 = 0;
    d->vs.ntex = (uint8_t)(d->vs.prog || d->fs.prog ? 8 : nst);
    for (int i = 0; i < nused; ++i)
    {
        const uint32_t* t = s->tss[i];
        GfxStage* st = &d->fs.st[i];
        int kind = 0;
        GfxTex* tex = texture_for_draw(s->tex[i], &kind);
        if (tex)
        {
            d->tex[i] = tex;
            st->tex = (uint8_t)kind;
            Obj* to = obj(s->tex[i]);
            uint32_t maxlevel = t[20] > to->lod ? t[20] : to->lod;
            d->samp[i] = (GfxSampler){ (uint8_t)t[13], (uint8_t)t[14], (uint8_t)t[25], (uint8_t)t[16], (uint8_t)t[17], (uint8_t)t[18],
                (uint8_t)t[21], (uint8_t)maxlevel, 0, { 0 }, t[15] };
            if (to->repl && tex == to->repl) /* drawn smaller than it is: linear, between its mipmaps */
            {
                d->samp[i].mag = d->samp[i].min = d->samp[i].mip = 2, d->samp[i].max_level = 0;
                d->samp[i].lod_cap = to->repl_mips ? 0 : 2; /* a glyph sheet's half-size mip at most: smaller ones blend
                                                             * neighbouring glyphs in */
                if (i == 0)
                    repl0 = 1;
            }
        }
        if (!d->fs.prog)
        {
            st->cop = (uint8_t)t[1], st->ca1 = (uint8_t)t[2], st->ca2 = (uint8_t)t[3], st->ca0 = (uint8_t)t[26];
            st->aop = (uint8_t)t[4], st->aa1 = (uint8_t)t[5], st->aa2 = (uint8_t)t[6], st->aa0 = (uint8_t)t[27];
            st->result = (uint8_t)t[28];
        }
        uint32_t ttf = t[24], count = ttf & 0xFF;
        st->ncoord = (uint8_t)(count ? count : 2);
        st->projected = (ttf & 256) && count > 1;
        if (!d->vs.prog)
        {
            d->vs.tci[i] = (uint8_t)((t[11] & 7) | ((t[11] >> 16) & 0xF) << 4);
            d->vs.ttf[i] = (uint8_t)(count | ((ttf & 256) ? 0x80 : 0));
        }
    }
    d->fs.alpha_func = (uint8_t)(rs[15] ? rs[25] : 0);
    d->fs.specular_add = !d->fs.prog && rs[29];
    d->vs.flat = d->fs.flat = rs[9] == 1; /* SHADEMODE flat */

    d->pipe.blend = rs[27] != 0;
    if (d->pipe.blend)
        d->pipe.src = (uint8_t)rs[19], d->pipe.dst = (uint8_t)rs[20], d->pipe.op = (uint8_t)rs[171];
    /* FFXI draws its text adding (ONE, ONE): its font's outline can only brighten what is behind it.
     * A replacement drawn that way has its colour premultiplied by alpha (make_texpack --additive), so
     * over (ONE, INVSRCALPHA) draws the same fill and lets the outline darken, as the game's other
     * lettering does. */
    if (repl0 && d->pipe.blend && d->pipe.src == 2 && d->pipe.dst == 2 && d->pipe.op == 1)
        d->pipe.dst = 6;
    d->pipe.write_mask = (uint8_t)(rs[168] & 0xF);
    if (g_dev.ds)
    {
        d->depth.zenable = rs[7] != 0, d->depth.zwrite = rs[14] != 0, d->depth.zfunc = (uint8_t)rs[23];
        if (rs[52]) /* STENCILENABLE */
        {
            d->depth.stencil = 1;
            d->depth.sfail = (uint8_t)rs[53], d->depth.szfail = (uint8_t)rs[54], d->depth.spass = (uint8_t)rs[55];
            d->depth.sfunc = (uint8_t)rs[56], d->depth.sread = (uint8_t)rs[58], d->depth.swrite = (uint8_t)rs[59];
            d->stencil_ref = rs[57];
        }
    }
    d->cull = (uint8_t)rs[22];
    d->fill = (uint8_t)rs[8];
    d->zbias = (int32_t)rs[47];
    if (!d->vs.rhw)
    {
        Obj* vb = obj(s->stream[0]);
        d->water = vb && vb->water;
        if (d->water && !d->vs.prog)
            water_lift(d, s);
    }
    return 1;
}

/* Points the draw at vertices first .. first + count of the streams its layout reads: the bound
 * vertex buffers, or the ...UP draw's memory as stream 0. */
static int set_streams(GfxDraw* d, uint32_t first, uint32_t count, uint32_t up_data, uint32_t up_stride)
{
    if (g_merge.on)
        return up_data ? (gfx_prof_skip(GFX_SKIP_STREAM), 0) : merge_streams(d, first, count);
    int used[GFX_NSTREAMS] = { 0 };
    for (int r = 0; r < GFX_NREGS; ++r)
        if (d->vs.el[r].used)
            used[d->vs.el[r].stream] = 1;
    for (int st = 0; st < GFX_NSTREAMS; ++st)
    {
        if (!used[st])
            continue;
        uint32_t stride, base, size;
        if (up_data)
        {
            if (st)
                return gfx_prof_skip(GFX_SKIP_STREAM), 0;
            stride = up_stride, base = up_data, size = 0xFFFFFFFFu;
        }
        else
        {
            Obj* b = obj(g_dev.cur.stream[st]);
            if (!b || !b->mem)
                return gfx_prof_skip(GFX_SKIP_NO_BUFFER), 0;
            b->drawn = g_ws.frame;
            stride = g_dev.cur.stride[st], base = b->mem, size = b->size;
        }
        uint32_t start = first * stride, bytes = stride ? count * stride : 64;
        if (start >= size)
            return gfx_prof_skip(GFX_SKIP_RANGE), 0;
        if (bytes > size - start)
            bytes = size - start;
        GfxBuf* gb = up_data ? NULL : static_buffer(obj(g_dev.cur.stream[st]));
        if (gb && !(start & 3))
        {
            d->buf[st] = gb, d->buf_off[st] = start;
            d->u.stride[st] = (int32_t)stride;
            continue;
        }
        d->data[st] = GUEST_PTR(base + start);
        d->size[st] = bytes;
        d->u.stride[st] = (int32_t)stride;
    }
    return 1;
}

/* index_range of an index buffer's indices, kept while the buffer isn't written (its serial): every indexed draw
 * scanned all its indices for the lowest and highest */
static void index_range(uint32_t indices, uint32_t size, uint32_t n, uint32_t* lo, uint32_t* hi);
static void range_cached(const Obj* ib, uint32_t indices, uint32_t size, uint32_t n, uint32_t* lo, uint32_t* hi)
{
    static struct { uint32_t serial, indices, n, size, lo, hi; } c[512];
    uint32_t k = (indices >> 1) ^ (indices >> 11) ^ (n * 2654435761u);
    unsigned i = (k ^ (k >> 16)) & 511u;
    if (c[i].serial == ib->serial && c[i].indices == indices && c[i].n == n && c[i].size == size)
    {
        *lo = c[i].lo, *hi = c[i].hi;
        return;
    }
    index_range(indices, size, n, lo, hi);
    c[i].serial = ib->serial, c[i].indices = indices, c[i].n = n, c[i].size = size, c[i].lo = *lo, c[i].hi = *hi;
}

static void index_range(uint32_t indices, uint32_t size, uint32_t n, uint32_t* lo, uint32_t* hi)
{
    uint32_t a = 0xFFFFFFFFu, b = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t v = size == 4 ? rd32(indices + 4 * i) : rd16(indices + 2 * i);
        a = v < a ? v : a;
        b = v > b ? v : b;
    }
    *lo = a, *hi = b;
}

/* --ui-aspect (user32_set_ui_aspect): FFXI lays its interface out in a fixed space and stretches
 * it to the back buffer, so a window wider than that space stretches the interface. Its menus,
 * text and cursor are pre-transformed (XYZRHW) draws to the back buffer; each is squeezed toward
 * the middle by mapping the viewport's width onto a centered box of the aspect asked for (the
 * XYZRHW mapping reads d->u.vp). Draws that span the whole width - the 3D scene put on the screen,
 * fades, letterboxing - are left full. With a menu resolution other than the window's, FFXI draws
 * the interface into a render-target texture of that size (the menu target) and puts it on the
 * screen as one full-width quad: the draws into it are squeezed the same way, so its full-width ones
 * (the dimming behind a menu, the lobby's copy of the 3D scene) still cover the screen. The mouse is
 * mapped back in user32.c. */
/* Bars across the whole width (the lobby's help bar): a full-width draw no taller than this share of
 * the height is a bar's body, and the frame drawn over it after (edge lines inset by a corner's
 * width, corner pieces at the ends) goes out to the screen's edges with it. Reset at Present. */
#define UI_BAR_MAX_H 0.15f
#define UI_MAX_BARS 8
static struct
{
    uint32_t rt;
    float y0, y1;
} g_ui_bars[UI_MAX_BARS];
static int g_ui_nbars;

/* Where the interface was drawn, on a coarse grid over the screen: this frame's, and the last
 * finished one's, which user32 asks (user32_ui_hit) to tell the interface from the world under the
 * mouse. g_ui_composited: the menu target is on the screen this frame, so what goes over it after
 * is the game's cursor. */
#define UI_GW 128
#define UI_GH 64
static uint8_t g_ui_grid[2][UI_GH][UI_GW];
static int g_ui_cur, g_ui_composited;

static int ui_hit(float fx, float fy)
{
    int x = (int)(fx * UI_GW), y = (int)(fy * UI_GH);
    return x >= 0 && x < UI_GW && y >= 0 && y < UI_GH && g_ui_grid[g_ui_cur ^ 1][y][x];
}

/* a box of the screen (0..1 across and down) the interface covers */
static void ui_mark(float x0, float x1, float y0, float y1)
{
    int a = (int)(x0 * UI_GW), b = (int)(x1 * UI_GW), c = (int)(y0 * UI_GH), e = (int)(y1 * UI_GH);
    a = a < 0 ? 0 : a, c = c < 0 ? 0 : c, b = b >= UI_GW ? UI_GW - 1 : b, e = e >= UI_GH ? UI_GH - 1 : e;
    for (int y = c; y <= e; ++y)
        for (int x = a; x <= b; ++x)
            g_ui_grid[g_ui_cur][y][x] = 1;
}

static void ui_present(void)
{
    g_ui_nbars = 0;
    g_ui_composited = 0;
    g_ui_cur ^= 1;
    memset(g_ui_grid[g_ui_cur], 0, sizeof g_ui_grid[0]);
    user32_ui_hit = ui_hit;
}

/* The draw's x (axis 0) or y (axis 1) mapped to a + b x (the game's pixels, across the whole
 * target): its viewport - the clip rectangle the game gave it, in the target's pixels - moves and
 * narrows to match, while the XYZRHW mapping (u.vp, the viewport in the game's pixels) stays, so the
 * draw keeps its clip */
static void ui_target_size(float* w, float* h);

static void ui_map_axis(GfxDraw* d, int i, float a, float b)
{
    float x0 = d->u.vp[i], w = d->u.vp[i + 2];
    if (!(w > 0))
        return;
    float k = (float)d->vp[i + 2] / w; /* the target's pixels per game pixel */
    float l = (a + b * x0) * k, r = (a + b * (x0 + w)) * k;
    /* kept within the target: the back ends clip a viewport to it, which would shrink the mapping
     * (a bar's edge line, widened past the screen's edge, stopped short of its corner) */
    float tw, th;
    ui_target_size(&tw, &th);
    float edge = i == 0 ? tw : th;
    l = l < 0 ? 0 : l;
    r = r > edge * k ? edge * k : r;
    d->vp[i] = (uint32_t)(l + 0.5f);
    d->vp[i + 2] = r > l ? (uint32_t)(r - l + 0.5f) : 0;
    /* what rounding the clip took off, given back to the mapping so the draw lands where asked */
    d->u.vp[i] = x0 + ((float)d->vp[i] / k - (a + b * x0)) / b;
    d->u.vp[i + 2] = (float)d->vp[i + 2] / k / b;
}

static void ui_map(GfxDraw* d, float a, float b) { ui_map_axis(d, 0, a, b); }

/* The game's size of the target the draw goes to: the back buffer's, or the menu target's */
static void ui_target_size(float* w, float* h)
{
    Obj* rt = obj(g_dev.rt);
    Obj* t = rt && rt->container ? obj(rt->container) : NULL;
    *w = t ? (float)t->width : (float)g_dev.pp[0];
    *h = t ? (float)t->height : (float)g_dev.pp[1];
}

static void ui_squeeze(GfxDraw* d, uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride, int mark)
{
    float s = user32_ui_squeeze(g_dev.hwnd), sy = user32_ui_squeeze_y(g_dev.hwnd);
    if (s >= 1.0f && sy >= 1.0f)
        return;
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        uint32_t st = d->vs.el[GFX_R_POSITION].stream;
        Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return;
        base = b->mem, stride = g_dev.cur.stride[st], size = b->size;
    }
    float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = (first + i) * stride + (uint32_t)d->u.offset[GFX_R_POSITION];
        if (at > size - 8)
            break;
        float x = u2f(rd32(base + at)), y = u2f(rd32(base + at + 4));
        lo = x < lo ? x : lo;
        hi = x > hi ? x : hi;
        ylo = y < ylo ? y : ylo;
        yhi = y > yhi ? y : yhi;
    }
    if (lo > hi)
        return;
    /* everything is squeezed toward the target's middle, whatever viewport the draw has (the chat
     * log's text and the menus' titles are clipped to their windows); "the whole width" with some
     * slack: the rules screen's backdrop stops 1.5 pixels short */
    float W, h;
    ui_target_size(&W, &h);
    float slack = 1.0f + W / 256.0f;
    if (sy < 1.0f)
    {
        /* a window taller than the interface's shape: squeezed toward the middle up and down. What
         * covers the whole target stays (the 3D scene put on the screen, fades, the dimming behind a
         * menu); a full-width band at the top or bottom (a cutscene's bars, the lobby's help bar)
         * keeps its outer edge on the screen's */
        float yslack = 1.0f + h / 256.0f, a = h * 0.5f * (1.0f - sy), b = sy;
        int wide = lo <= slack && hi >= W - slack, top = ylo <= yslack, bottom = yhi >= h - yslack;
        if (wide && top && bottom)
            return;
        if (wide && (top || bottom) && yhi > ylo)
        {
            float t0 = top ? 0.0f : a + sy * ylo, t1 = bottom ? h : a + sy * yhi;
            b = (t1 - t0) / (yhi - ylo), a = t0 - b * ylo;
        }
        ui_map_axis(d, 1, a, b);
        if (mark)
            ui_mark(lo / W, hi / W, (a + b * ylo) / h, (a + b * yhi) / h);
        return;
    }
    if (lo <= slack && hi >= W - slack)
    {
        if (yhi - ylo <= h * UI_BAR_MAX_H && g_ui_nbars < UI_MAX_BARS)
            g_ui_bars[g_ui_nbars].rt = g_dev.rt, g_ui_bars[g_ui_nbars].y0 = ylo, g_ui_bars[g_ui_nbars].y1 = yhi,
            g_ui_nbars++;
        return;
    }
    /* a piece of a bar's frame: within a bar's band and at an edge (a corner's width, 64 pixels) */
    float edge = 64.0f * W / 1280.0f, a = W * 0.5f * (1.0f - s), b = s;
    for (int i = 0; i < g_ui_nbars; ++i)
    {
        if (g_ui_bars[i].rt != g_dev.rt || ylo < g_ui_bars[i].y0 - 2.0f || yhi > g_ui_bars[i].y1 + 2.0f)
            continue;
        int left = lo <= edge, right = hi >= W - edge;
        if (left && right) /* an edge line: its ends keep their insets, squeezed */
        {
            float l = lo * s, r = W - (W - hi) * s;
            b = (r - l) / (hi - lo), a = l - b * lo;
            break;
        }
        if (left && hi < W * 0.5f) /* the left corner, squeezed toward the left edge */
        {
            a = 0.0f;
            break;
        }
        if (right && lo > W * 0.5f) /* the right corner */
        {
            a = W * (1.0f - s);
            break;
        }
    }
    ui_map(d, a, b);
    if (mark)
        ui_mark((a + b * lo) / W, (a + b * hi) / W, ylo / h, yhi / h);
}

/* FFXI_DRAWLOG=<path>: while <path>.go exists, the next frame's draws are written to <path> (then
 * .go is removed): the game's return addresses on the guest stack, the texture, and the vertices'
 * box. For finding which of the game's code draws what. */
static void cap_present(void)
{
    static int init;
    if (!init)
    {
        init = 1, g_cap_path = getenv("FFXI_DRAWLOG");
        /* without it, draws.txt in the cache folder (still only while draws.txt.go exists) */
        static char def[1024];
        char dir[900];
        if ((!g_cap_path || !*g_cap_path) && cache_dir(dir, sizeof dir))
            snprintf(def, sizeof def, "%s/draws.txt", dir), g_cap_path = def;
    }
    if (!g_cap_path)
        return;
    ++g_cap_frame;
    if (g_cap)
    {
        fclose(g_cap), g_cap = NULL;
        char go[1024];
        snprintf(go, sizeof go, "%s.go", g_cap_path);
        remove(go);
        return;
    }
    if (g_cap_frame % 30)
        return;
    char go[1024];
    snprintf(go, sizeof go, "%s.go", g_cap_path);
    FILE* f = fopen(go, "rb");
    if (!f)
        return;
    fclose(f);
    g_cap = fopen(g_cap_path, "w");
    g_cap_n = 0;
    g_cap_nat = 0;
    char at[1024];
    snprintf(at, sizeof at, "%s.at", g_cap_path);
    FILE* af = fopen(at, "r");
    while (af && g_cap_nat < 32 && fscanf(af, "%u", &g_cap_at[g_cap_nat]) == 1)
        g_cap_nat++;
    if (af)
        fclose(af);
    uint32_t w, h;
    d3d8_screen_size(&w, &h);
    if (g_cap)
        fprintf(g_cap, "frame %u screen %ux%u backbuffer %ux%u image %08x-%08x\n", g_cap_frame, w, h, g_dev.pp[0],
            g_dev.pp[1], rt_image_lo, rt_image_hi);
}

/* the current target as it is after draw n, if <path>.at asks for it: <path>.<n>.bin, a header
 * of width and height, then its pixels as the GPU holds them (BGRA) */
static void cap_dump_target(uint32_t n)
{
    uint32_t k = 0;
    while (k < g_cap_nat && g_cap_at[k] != n)
        k++;
    Obj* rt = obj(g_dev.rt);
    Obj* t = rt && rt->container ? obj(rt->container) : NULL;
    if (k == g_cap_nat || !t || !t->gpu || t->format != 21)
        return;
    uint32_t w = t->pw ? t->pw : t->width, h = t->pw ? t->ph : t->height;
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    gfx_tex_read(t->gpu, 0, 0, px, w * 4);
    char path[1100];
    snprintf(path, sizeof path, "%s.%u.bin", g_cap_path, n);
    FILE* f = fopen(path, "wb");
    if (f)
    {
        fwrite(&w, 4, 1, f), fwrite(&h, 4, 1, f), fwrite(px, 4, (size_t)w * h, f);
        fclose(f);
    }
    free(px);
}

static void cap_draw(GfxDraw* d, uint32_t prim, uint32_t count, uint32_t first, uint32_t n, uint32_t up_data,
    uint32_t up_stride)
{
    FILE* f = g_cap;
    Obj* crt = obj(g_dev.rt);
    fprintf(f, "#%u prim %u count %u n %u vs %x rhw %d rt %s %ux%u %08x vp %u,%u %ux%u", g_cap_n++, prim, count, n, g_dev.cur.vs,
        d->vs.rhw, g_dev.rt == g_dev.backbuffer ? "bb" : "off", crt ? crt->width : 0, crt ? crt->height : 0, g_dev.rt, d->vp[0],
        d->vp[1], d->vp[2], d->vp[3]);
    const uint32_t* crs = g_dev.cur.rs;
    const uint32_t* ct = g_dev.cur.tss[0];
    fprintf(f, " atest %u func %u ref %u blend %u %u/%u op %u tss0 c%u(%x,%x) a%u(%x,%x) tss1 c%u", crs[15], crs[25], crs[24],
        crs[27], crs[19], crs[20], crs[171], ct[1], ct[2], ct[3], ct[4], ct[5], ct[6], g_dev.cur.tss[1][1]);
    Obj* t = obj(g_dev.cur.tex[0]);
    if (t)
        fprintf(f, " tex %08x %ux%u fmt %u filt %u/%u", g_dev.cur.tex[0], t->width, t->height, t->format, ct[16], ct[17]);
    fprintf(f, " up %u inset %d", up_data ? 1u : 0u, g_cap_inset);
    g_cap_inset = 0;
    fprintf(f, " z %u/%u func %u fog %u/%u lit %u caster %u water %u", d->depth.zenable, d->depth.zwrite, d->depth.zfunc, d->fs.fog,
        d->vs.fog_vertex, d->vs.lighting, d->caster, d->water);
    if (!d->vs.rhw)
    {
        const float* P = g_dev.cur.xf[3];
        const float* V = g_dev.cur.xf[2];
        const float* W = g_dev.cur.xf[24];
        fprintf(f, "\n  proj %.4f %.4f %.4f %.4f view t %.2f %.2f %.2f world t %.2f %.2f %.2f", P[0], P[5], P[8], P[9],
            V[12], V[13], V[14], W[12], W[13], W[14]);
    }
    fprintf(f, "\n  stack");
    for (uint32_t a = g_cap_esp, k = 0; a < g_cap_esp + 0x600 && k < 12; a += 4)
    {
        uint32_t v = rd32(a);
        if (v < rt_image_lo + 6 || v >= rt_image_hi)
            continue;
        if (rd8(v - 5) == 0xE8 || rd8(v - 6) == 0xFF || rd8(v - 3) == 0xFF || rd8(v - 2) == 0xFF)
            fprintf(f, " %08x", v), ++k;
    }
    fprintf(f, "\n");
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        uint32_t st = d->vs.el[GFX_R_POSITION].stream;
        Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return;
        base = b->mem, stride = g_dev.cur.stride[st], size = b->size;
    }
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = (first + i) * stride + (uint32_t)d->u.offset[GFX_R_POSITION];
        if (at > size - 12)
            break;
        for (int c = 0; c < 3; ++c)
        {
            float x = u2f(rd32(base + at + 4 * c));
            lo[c] = x < lo[c] ? x : lo[c];
            hi[c] = x > hi[c] ? x : hi[c];
        }
        if (i < 4 && g_dev.cur.vs == 0x144 && stride == 28) /* XYZRHW, diffuse, one texture: with its UV */
            fprintf(f, "  v%u %.2f %.2f %.4f uv %.5f %.5f c %08x\n", i, u2f(rd32(base + at)), u2f(rd32(base + at + 4)),
                u2f(rd32(base + at + 8)), u2f(rd32(base + at + 20)), u2f(rd32(base + at + 24)), rd32(base + at + 16));
        else if (i < 4)
            fprintf(f, "  v%u %.2f %.2f %.4f\n", i, u2f(rd32(base + at)), u2f(rd32(base + at + 4)),
                u2f(rd32(base + at + 8)));
    }
    fprintf(f, "  box %.2f..%.2f  %.2f..%.2f  %.4f..%.4f\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
}

/* --- the scene's end, for the back end's scene effects (gfx_scene_done) ------------------------------
 * The frame's 3D scene is the target that takes depth-tested draws of untransformed vertices and is
 * at least half the back buffer (not the 16x16 probes or smaller effect targets). It is finished
 * when a draw samples it (FFXI draws its background target onto the back buffer as a quad), when
 * transformed vertices go over it on the back buffer (the interface), or at Present. The camera
 * comes from its fixed-function draws: the zone's own shader draws carry theirs in constants. */
static struct
{
    GfxTex* rt;     /* the scene's target */
    /* the world's view: the target that took the most 3D draws the frame before. FFXI draws the world
     * into one 4096 target and copies it into another, where nameplates and effects go on; other
     * targets' scenes get no effects, cast no shadows and give no camera. */
    GfxTex* world;
    int world_done; /* the effects ran on it this frame: once a frame */
    struct { GfxTex* t; uint32_t n; } tally[4]; /* this frame's 3D draws per target */
    uint32_t draws; /* 3D draws to it since the effects last ran */
    int done;       /* the effects ran this frame */
    /* where this frame's camera and sun came from: the draw's number, and for the camera 2 when that
     * draw was fogged (the world), 1 when not; 0 before one is found */
    uint32_t cam_draw, sun_draw, cam_rank;
    GfxScene s;
    /* the profile (FFXI_PROFILE): scenes by how they ended and where the camera came from, 3D draws
     * after the effects ran, and screen-space or undepth-tested draws onto the scene before them
     * (what the effects would shade) */
    uint32_t st_frames, st_why[4], st_cam[3], st_late, st_over, st_over_rhw, st_game_shadows;
    GfxScene wcam;   /* the scene renderer's camera this scene (d3d8_world_scene): the stand-in when the game gave none */
    GfxTex* wcam_rt; /* the target it drew into (NULL: none this scene) */
    /* the trace (scene_trace): what each of the last frames did with the world's scene */
    uint32_t frame, tr_late, tr_draws, tr_targets;
    char tr_why;
    struct { uint32_t frame, draws, late, targets; char why, ran, changed, cam; } trace[1200];
} g_scene;

/* The sun, by vote. FFXI lights each object with lights of its own, some tens of degrees apart, so
 * the first lit draw's light (the old way) turned the shadows as the camera moved. Each lit draw of
 * the world votes for each of its directional lights, in the world: its first light counts 1, the
 * rest 0.5; lights within about 2 degrees are one light; scores fade by 0.99 a frame. Another light
 * becomes the sun only when it has scored 1.5 times the sun's for four frames running. The winner's
 * direction is its latest, so the sun still moves through the day. */
static struct
{
    struct { float dir[3], color[4], score, fresh; } c[8]; /* fresh: its votes in this scene alone */
    int cur, chal, chal_frames; /* the sun, the light overtaking it and for how many frames; -1 none */
    float cam[3];               /* where the last scene's camera stood (a jump is a new place) */
    int have_cam;
    uint32_t idle;              /* frames since a world scene was last drawn (a gap is a new place too) */
} g_sunv = { .cur = -1, .chal = -1 };

/* The sky's sun, or a zone's fixed light. Outdoors the game's sun never stands still: it turns with
 * the clock a little every scene (d2 3.5e-10 and up between scenes, eased faster at each Vana'diel
 * minute), always the same way, in the world's x-y plane (z = 0). A zone with no sky (Ru'Hmet,
 * Garlaige Citadel, Ordelle's Caves, the Necropolis) lights everything from one light that never
 * moves - straight down, or slanted, often with z != 0; its direction changes by rounding alone (d2
 * 1e-13 or less, turning the camera) - and the sun's shadows from it are those of a ceiling over
 * everything: the zone goes black. After a zone change the light counts as fixed until the sun has
 * moved like the sky's for half a second: every scene, by more than rounding and less than about 3
 * degrees (more is another light winning the vote), the same way as the scene before. Then the sun's
 * shadows ease in over a third of a second, not all at once over the scene the zone came in with. */
static struct
{
    float dir[3], step[2]; /* the sun's world direction at the last scene, and its last move */
    int have, moved;
    uint32_t moving;       /* scenes in a row it has moved like the sky's sun */
    float indoors;         /* GfxScene.indoors: 1 at a zone change, easing to 0 once it has */
} g_sky;

static float sun_sky(const float* w)
{
    if (g_sky.have && !g_sky.moved)
    {
        float dx = w[0] - g_sky.dir[0], dy = w[1] - g_sky.dir[1];
        float d2 = dx * dx + dy * dy;
        int sky = d2 > 1e-11f && d2 < 0.05f * 0.05f && fabsf(w[2]) < 1e-3f && fabsf(g_sky.dir[2]) < 1e-3f &&
                  dx * g_sky.step[0] + dy * g_sky.step[1] >= 0.0f;
        g_sky.moving = sky ? g_sky.moving + 1 : 0;
        g_sky.moved = g_sky.moving >= 30;
        g_sky.step[0] = dx, g_sky.step[1] = dy;
    }
    memcpy(g_sky.dir, w, sizeof g_sky.dir);
    if (!g_sky.have)
        g_sky.indoors = 1.0f;
    g_sky.have = 1;
    if (g_sky.moved)
        g_sky.indoors = fmaxf(g_sky.indoors - 0.05f, 0.0f);
    return g_sky.indoors;
}

/* A new place (the camera jumped, or half a second went by with no world drawn: the world after the
 * lobby's character select and the fade in, a zone change, a moghouse): the votes from before are
 * another place's lights - the lobby's, the last zone's sun - and scored over hundreds of frames, the
 * new sun took seconds to outscore them, the shadows falling from the old light and swinging as the
 * new ones overtook it. Only this scene's votes count, and the sun is chosen again. */
static void sun_new_place(const float* view)
{
    float cam[3];
    for (int j = 0; j < 3; ++j) /* the view's translation back through its rotation */
        cam[j] = -(view[12] * view[j * 4] + view[13] * view[j * 4 + 1] + view[14] * view[j * 4 + 2]);
    float dx = cam[0] - g_sunv.cam[0], dy = cam[1] - g_sunv.cam[1], dz = cam[2] - g_sunv.cam[2];
    if (g_sunv.have_cam && g_sunv.idle >= 30)
        memset(&g_sky, 0, sizeof g_sky); /* a zone change: its sun is still to be seen moving */
    if (g_sunv.have_cam && (dx * dx + dy * dy + dz * dz > 50.0f * 50.0f || g_sunv.idle >= 30))
    {
        for (int i = 0; i < 8; ++i)
            g_sunv.c[i].score = g_sunv.c[i].fresh;
        g_sunv.cur = g_sunv.chal = -1;
    }
    memcpy(g_sunv.cam, cam, sizeof cam);
    g_sunv.have_cam = 1;
}

static void sun_vote(const float* w, const float* color, float weight)
{
    int at = -1, low = -1;
    for (int i = 0; i < 8; ++i)
    {
        const float* d = g_sunv.c[i].dir;
        if (g_sunv.c[i].score > 0.0f && d[0] * w[0] + d[1] * w[1] + d[2] * w[2] > 0.9995f)
        {
            at = i;
            break;
        }
        if (i != g_sunv.cur && (low < 0 || g_sunv.c[i].score < g_sunv.c[low].score))
            low = i;
    }
    if (at < 0)
    {
        if (low < 0)
            return;
        at = low, g_sunv.c[at].score = g_sunv.c[at].fresh = 0.0f;
        if (g_sunv.chal == at)
            g_sunv.chal = -1;
    }
    memcpy(g_sunv.c[at].dir, w, 12);
    memcpy(g_sunv.c[at].color, color, 16);
    g_sunv.c[at].score += weight;
    g_sunv.c[at].fresh += weight;
}

/* the frame's sun from the votes so far */
static int sun_pick(void)
{
    int top = -1;
    for (int i = 0; i < 8; ++i)
        if (g_sunv.c[i].score > 0.0f && (top < 0 || g_sunv.c[i].score > g_sunv.c[top].score))
            top = i;
    if (top < 0)
        return g_sunv.cur;
    if (g_sunv.cur < 0 || g_sunv.c[g_sunv.cur].score <= 0.0f)
        g_sunv.cur = top, g_sunv.chal = -1;
    else if (top != g_sunv.cur && g_sunv.c[top].score > 1.5f * g_sunv.c[g_sunv.cur].score)
    {
        g_sunv.chal_frames = g_sunv.chal == top ? g_sunv.chal_frames + 1 : 1;
        g_sunv.chal = top;
        if (g_sunv.chal_frames >= 4)
            g_sunv.cur = top, g_sunv.chal = -1;
    }
    else
        g_sunv.chal = -1;
    return g_sunv.cur;
}

/* a texture going away: the scene no longer points at it */
static void scene_forget(GfxTex* t)
{
    if (!t)
        return;
    if (g_scene.rt == t)
        g_scene.rt = NULL, g_scene.draws = 0, g_scene.done = 0;
    if (g_scene.wcam_rt == t)
        g_scene.wcam_rt = NULL;
    if (g_scene.world == t)
        g_scene.world = NULL;
    for (int i = 0; i < 4; ++i)
        if (g_scene.tally[i].t == t)
            g_scene.tally[i].t = NULL, g_scene.tally[i].n = 0;
}

static void scene_finish(const char* why)
{
    if (g_cap && g_scene.draws && !g_scene.done)
    {
        const float* p = g_scene.s.proj;
        const uint32_t* vp = g_scene.s.vp;
        const float* f = g_scene.s.fogcolor;
        fprintf(g_cap, "scene done (%s) after %u 3D draws, camera %d from draw %u (fogged %d), sun %d from draw %u, "
            "projection %g %g %g %g / %g %g, viewport %u %u %u %u z %g..%g, fog %.2f %.2f %.2f\n",
            why, g_scene.draws, g_scene.cam_rank != 0, g_scene.cam_draw, g_scene.cam_rank == 2, g_scene.s.sun_dir[3] != 0.0f,
            g_scene.sun_draw, p[0], p[5], p[10], p[11], p[14], p[15], vp[0], vp[1], vp[2], vp[3], u2f(vp[4]), u2f(vp[5]),
            f[0], f[1], f[2]);
    }
    if (g_scene.draws && !g_scene.done)
    {
        g_scene.st_why[!strcmp(why, "sampled") || why[0] == 'e' ? 0 : why[0] == 'i' ? 1 : why[0] == 'p' ? 2 : 3]++;
        g_scene.st_cam[g_scene.cam_rank]++;
    }
    /* none from the game's own draws, but the scene renderer drew the world into this target: its camera */
    if (g_scene.draws && !g_scene.done && !g_scene.cam_rank && g_scene.wcam_rt && g_scene.wcam_rt == g_scene.rt)
    {
        GfxScene* sc = &g_scene.s;
        memcpy(sc->proj, g_scene.wcam.proj, 64), memcpy(sc->view, g_scene.wcam.view, 64), memcpy(sc->vp, g_scene.wcam.vp, sizeof sc->vp);
        memcpy(sc->ambient, g_scene.wcam.ambient, 16), memcpy(sc->fogcolor, g_scene.wcam.fogcolor, 16);
        memcpy(sc->fog, g_scene.wcam.fog, sizeof sc->fog);
        g_scene.cam_rank = 1;
    }
    /* a camera from this scene's own draws (cam_rank), not one kept from an earlier scene: first person by day
     * finds none, and the effects ran on the last third-person view for as long as it lasted (#106) */
    if (g_scene.draws && !g_scene.done && g_scene.cam_rank && (!g_scene.world || g_scene.rt == g_scene.world) &&
        !g_scene.world_done)
    {
        /* the sun the votes chose, into this scene's camera space; w = 1 when lit draws voted in this
         * scene (without, the back end holds the last sun) */
        GfxScene* sc = &g_scene.s;
        sun_new_place(sc->view);
        int sun = sun_pick();
        for (int i = 0; i < 8; ++i)
            g_sunv.c[i].fresh = 0.0f;
        sc->sun_dir[3] = 0.0f;
        if (sun >= 0 && g_scene.sun_draw)
        {
            const float* w = g_sunv.c[sun].dir;
            sc->indoors = sun_sky(w);
            for (int j = 0; j < 3; ++j)
                sc->sun_dir[j] = w[0] * sc->view[j] + w[1] * sc->view[4 + j] + w[2] * sc->view[8 + j];
            float l = sqrtf(sc->sun_dir[0] * sc->sun_dir[0] + sc->sun_dir[1] * sc->sun_dir[1] + sc->sun_dir[2] * sc->sun_dir[2]);
            if (l > 0.0f)
            {
                sc->sun_dir[0] /= l, sc->sun_dir[1] /= l, sc->sun_dir[2] /= l, sc->sun_dir[3] = 1.0f;
                memcpy(sc->sun_color, g_sunv.c[sun].color, 16);
            }
        }
        gfx_scene_done(g_scene.rt, &g_scene.s);
        g_scene.world_done = 1;
        g_scene.tr_why = why[0] == 's' && why[1] == 'w' ? 'w' : why[0], g_scene.tr_draws = g_scene.draws;
    }
    g_scene.draws = 0;
    g_scene.done = 1;
}

static void scene_note(GfxDraw* d)
{
    d->caster = 0;
    uint32_t face, level;
    Obj* rt = obj(g_dev.rt);
    GfxTex* c = rt ? surface_gpu(rt, &face, &level) : NULL;
    if (!c)
        return;
    int large = (uint64_t)rt->width * rt->height * 2 >= (uint64_t)g_dev.pp[0] * g_dev.pp[1];
    if (g_scene.draws && !g_scene.done)
    {
        int sampled = 0;
        for (int i = 0; i < 8; ++i)
            sampled |= d->tex[i] == g_scene.rt;
        /* the scene drawn somewhere as large as the screen (copied into the target the interface goes
         * on, or onto the back buffer): not the sun flare's occlusion probe, which copies it into
         * 16x16 - several times a frame, while the world is still being drawn */
        if (sampled && large)
            scene_finish("sampled");
        /* the interface over the scene - on the back buffer, or into the scene's own target: FFXI draws
         * nameplates and floating text there, blended, before it draws the scene onto the back buffer;
         * the effects go under them, not over. (Not the probe's own screen-space quads there, which
         * are opaque - and textured, some frames.) */
        else if (d->vs.rhw && c == g_scene.rt && (g_dev.rt == g_dev.backbuffer || d->pipe.blend))
            scene_finish("interface");
        /* the effects over the world - flames, glows, spells: blended, writing no depth, and unfogged
         * (the zone's own blended decals among its draws are fogged). The effects go under them: fog
         * read from the depth behind a flame took it for the wall there, and fogged it more or less as
         * it flickered. Not the sky's layers drawn after the world - its clouds, the sun, the moon -
         * which are blended and unfogged too, but drawn round the camera (a view with no translation):
         * ended at them, the effects ran before the sun was drawn and the god rays had none to start
         * from. */
        else if (!d->vs.rhw && c == g_scene.rt && d->depth.zenable && !d->depth.zwrite && d->pipe.blend &&
            !d->fs.fog && !d->vs.fog_vertex &&
            (g_dev.cur.xf[2][12] != 0.0f || g_dev.cur.xf[2][13] != 0.0f || g_dev.cur.xf[2][14] != 0.0f))
            scene_finish("effects");
    }
    if (c == g_scene.rt && g_scene.draws && !g_scene.done && (d->vs.rhw || !d->depth.zenable))
        g_scene.st_over++, g_scene.st_over_rhw += d->vs.rhw;
    if (d->vs.rhw || !d->depth.zenable || !obj(g_dev.ds) || face || level ||
        (uint64_t)rt->width * rt->height * 2 < (uint64_t)g_dev.pp[0] * g_dev.pp[1])
        return;
    for (int i = 0; i < 4; ++i)
        if (g_scene.tally[i].t == c || !g_scene.tally[i].t || i == 3)
        {
            if (g_scene.tally[i].t != c)
                g_scene.tally[i].t = c, g_scene.tally[i].n = 0;
            g_scene.tally[i].n++;
            break;
        }
    /* 3D into other targets - the game draws characters into one partway through the world, then
     * goes on with the world - is not the world's scene: it neither ends nor starts one */
    if (g_scene.world && c != g_scene.world)
        return;
    if (c != g_scene.rt) /* a new scene: the one before is done; the new one's camera and sun are its own */
    {
        scene_finish("switch");
        g_scene.rt = c, g_scene.draws = 0, g_scene.done = 0, g_scene.cam_rank = 0, g_scene.sun_draw = 0, g_scene.wcam_rt = NULL;
    }
    if (g_scene.done) /* 3D again after the effects ran: drawn as it is */
    {
        g_scene.st_late++, g_scene.tr_late++;
        return;
    }
    g_scene.draws++;
    /* what casts the sun's shadow (the back end's shadow map): the scene's opaque, depth-writing
     * draws - alpha-tested ones too (leaves, fences) - of the world, which is fogged or drawn by the
     * zone's shaders; not the sky, the sun and its flare (unfogged, with views of their own) */
    d->caster = d->depth.zwrite && (!d->pipe.blend || (d->fs.alpha_func && d->fs.alpha_func != 8)) &&
        (d->vs.prog || d->fs.fog || d->vs.fog_vertex) && !g_scene.world_done;
    const State* s = &g_dev.cur;
    GfxScene* sc = &g_scene.s;
    /* the camera and fog from the frame's first depth-writing fixed-function draw, a fogged one
     * (the world) over any other: the sun, its flare and the sky come later with their own views
     * and no fog, and taking theirs made the effects' fog come and go with the camera's angle */
    uint32_t rank = d->fs.fog ? 2u : 1u;
    /* a view with no translation (a sky drawn around the eye, as first person at night draws first) is no
     * camera: taking it put the effects' camera at the zone's origin, its sun read back upside down (#106) */
    const float* vt = s->xf[2] + 12;
    if (!d->vs.prog && s->xf[3][11] != 0.0f && d->depth.zwrite && rank > g_scene.cam_rank &&
        (vt[0] != 0.0f || vt[1] != 0.0f || vt[2] != 0.0f))
    {
        g_scene.cam_rank = rank, g_scene.cam_draw = g_scene.draws;
        memcpy(sc->proj, s->xf[3], 64);
        memcpy(sc->view, s->xf[2], 64);
        memcpy(sc->vp, d->vp, sizeof sc->vp);
        memcpy(sc->ambient, d->u.ambient, 16);
        memcpy(sc->fogcolor, d->u.fogcolor, 16);
        sc->fog[0] = d->u.params[2], sc->fog[1] = d->u.params[3], sc->fog[2] = d->fs.fog ? 1.0f : 0.0f;
    }
    if (d->vs.nlights && d->depth.zwrite)
    {
        /* each directional light's vote (sun_vote), back into the world through the view's rotation */
        const float* v = s->xf[2];
        for (int k = 0; k < d->vs.nlights; ++k)
        {
            if (d->vs.light_type[k] != 3)
                continue;
            const float* c = d->u.light[k].dir;
            float w[3];
            for (int i = 0; i < 3; ++i)
                w[i] = c[0] * v[i * 4] + c[1] * v[i * 4 + 1] + c[2] * v[i * 4 + 2];
            float l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            if (l <= 0.0f)
                continue;
            w[0] /= l, w[1] /= l, w[2] /= l;
            sun_vote(w, d->u.light[k].diffuse, k == 0 ? 1.0f : 0.5f);
            if (!g_scene.sun_draw)
                g_scene.sun_draw = g_scene.draws;
        }
    }
}

/* Present: the scene ends here if nothing ended it before; the next frame starts over */
/* The trace: while trace.go exists in the cache folder (cachedir.h; checked every 30 frames), the last 1200
 * frames' scenes go to trace.txt beside it and the effects' own to trace_gfx.txt; then .go is
 * removed. For a flicker the eye catches: what the frame before and after it did. */
static void scene_trace(GfxTex* world_before)
{
    uint32_t targets = 0;
    for (int i = 0; i < 4; ++i)
        targets += g_scene.tally[i].t != NULL;
    __typeof__(g_scene.trace[0])* t = &g_scene.trace[g_scene.frame % 1200];
    t->frame = g_scene.frame, t->draws = g_scene.tr_draws, t->late = g_scene.tr_late, t->targets = targets;
    t->why = g_scene.world_done ? g_scene.tr_why : '-', t->ran = (char)g_scene.world_done;
    t->changed = (char)(world_before != g_scene.world), t->cam = (char)g_scene.cam_rank;
    g_scene.tr_late = g_scene.tr_draws = 0, g_scene.tr_why = 0;
    g_scene.frame++;
    static char dir[900];
    if (!dir[0])
        cache_dir(dir, sizeof dir);
    if (!dir[0] || g_scene.frame % 30)
        return;
    char go[1024], out[1024];
    snprintf(go, sizeof go, "%s/trace.go", dir);
    struct stat st;
    if (stat(go, &st))
        return;
    remove(go);
    snprintf(out, sizeof out, "%s/trace.txt", dir);
    FILE* f = fopen(out, "w");
    if (f)
    {
        fprintf(f, "frame effects ended(s sampled, e effects, i interface, p present, w switch, - none) world-draws late-3D targets "
                   "world-changed camera-rank\n");
        uint32_t n = g_scene.frame < 1200 ? g_scene.frame : 1200;
        for (uint32_t k = g_scene.frame - n; k < g_scene.frame; ++k)
        {
            __typeof__(g_scene.trace[0])* e = &g_scene.trace[k % 1200];
            fprintf(f, "%u %d %c %u %u %u %d %d\n", e->frame, e->ran, e->why ? e->why : '?', e->draws, e->late, e->targets,
                e->changed, e->cam);
        }
        fclose(f);
    }
    snprintf(out, sizeof out, "%s/trace_gfx.txt", dir);
    gfx_trace_dump(out);
}

static void scene_present(void)
{
    scene_finish("present");
    GfxTex* world_before = g_scene.world;
    /* next frame's world: this frame's busiest target */
    uint32_t best = 0;
    for (int i = 0; i < 4; ++i)
        if (g_scene.tally[i].t && g_scene.tally[i].n > best)
            best = g_scene.tally[i].n, g_scene.world = g_scene.tally[i].t;
    scene_trace(world_before);
    g_sunv.idle = g_scene.world_done ? 0 : g_sunv.idle + 1; /* frames with no world drawn (a fade, a loading screen) */
    g_scene.world_done = 0;
    for (int i = 0; i < 8; ++i)
        g_sunv.c[i].score *= 0.99f;
    for (int i = 0; i < 4; ++i)
        g_scene.tally[i].t = NULL, g_scene.tally[i].n = 0;
    if (gfx_profiling && ++g_scene.st_frames == 120)
    {
        rt_log("[recomp] d3d8: scenes (120 frames, the last on %s): ended sampled %u, interface %u, present %u, switch %u; camera fogged %u, "
            "unfogged %u, none %u; 3D after the effects %u; over the scene before them %u (%u screen-space); the game's own shadows left out %u\n",
            g_scene.rt == g_scene.world ? "the world's view" : "another target", g_scene.st_why[0], g_scene.st_why[1], g_scene.st_why[2],
            g_scene.st_why[3], g_scene.st_cam[2], g_scene.st_cam[1], g_scene.st_cam[0],
            g_scene.st_late, g_scene.st_over, g_scene.st_over_rhw, g_scene.st_game_shadows);
        memset(g_scene.st_why, 0, sizeof g_scene.st_why), memset(g_scene.st_cam, 0, sizeof g_scene.st_cam);
        g_scene.st_frames = g_scene.st_late = g_scene.st_over = g_scene.st_over_rhw = g_scene.st_game_shadows = 0;
    }
    g_scene.done = 0;
    g_scene.cam_draw = g_scene.sun_draw = g_scene.cam_rank = 0;
    g_scene.wcam_rt = NULL;
}

static void draw_packet(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size,
    uint32_t up_data, uint32_t up_stride, uint32_t n);

/* A draw: from `start` (non-indexed), through guest indices, or from ...UP memory. */
static void draw(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size, uint32_t up_data,
    uint32_t up_stride)
{
    g_ws.draws++;
    WORLD_FLUSH();
    uint32_t n = prim_vertices(prim, count);
    if (!n)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    draw_packet(prim, count, start, indices, index_size, up_data, up_stride, n);
    if (gfx_profiling)
        gfx_prof_front(gfx_now_ns() - t0);
}

/* The game's cursor: a small screen-space draw (its sprite, 18 pixels in a 1280-wide menu) from the
 * cursor's 64x64 sheet whose box holds a point the game was lately given the mouse at (the latest
 * first: it may draw a frame behind). The sheet matters: with the mouse outside the interface box
 * that point is raw, and a menu glyph whose unsqueezed place lies under it would otherwise be taken
 * for the cursor and drawn there. Returns 0 for no cursor, 1 for one given raw (drawn where it is),
 * 2 for one given unsqueezed (squeezed like the interface). */
static int ui_is_cursor(GfxDraw* d, uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride)
{
    Obj* t = obj(g_dev.cur.tex[0]);
    if (!t || t->width > 64 || t->height > 64)
        return 0;
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        uint32_t st = d->vs.el[GFX_R_POSITION].stream;
        Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return 0;
        base = b->mem, stride = g_dev.cur.stride[st], size = b->size;
    }
    float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f, W, H, fx, fy;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = (first + i) * stride + (uint32_t)d->u.offset[GFX_R_POSITION];
        if (at > size - 8)
            return 0;
        float x = u2f(rd32(base + at)), y = u2f(rd32(base + at + 4));
        lo = x < lo ? x : lo, hi = x > hi ? x : hi, ylo = y < ylo ? y : ylo, yhi = y > yhi ? y : yhi;
    }
    ui_target_size(&W, &H);
    float most = 48.0f * W / 1280.0f;
    if (hi - lo > most || yhi - ylo > most)
        return 0;
    for (int i = 0; i < USER32_MOUSE_GIVEN; ++i)
    {
        int raw = user32_mouse_given(i, &fx, &fy);
        if (raw < 0)
            break;
        float mx = fx * W, my = fy * H;
        if (mx >= lo - 2.0f && mx <= hi + 2.0f && my >= ylo - 2.0f && my <= yhi + 2.0f)
            return raw ? 1 : 2;
    }
    return 0;
}

/* The target arrow: the game puts it over its target's place on the screen, a sprite of the cursor's
 * 64x64 sheet (cursor frames, the menus' hand, the arrow) drawn turned a quarter - the only sprite of
 * the interface drawn so (the lobby, menus and zones checked): the texture's u runs down the screen.
 * It stays where the game put it, over the world. */
static int ui_is_world_pointer(uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride)
{
    Obj* t = obj(g_dev.cur.tex[0]);
    if (!t || t->width != 64 || t->height != 64 || n != 4 || g_dev.cur.vs != 0x144)
        return 0;
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        Obj* b = obj(g_dev.cur.stream[0]);
        if (!b || !b->mem)
            return 0;
        base = b->mem, stride = g_dev.cur.stride[0], size = b->size;
    }
    uint32_t a = first * stride, c = a + stride; /* XYZRHW, diffuse, then u v at 20 */
    if (stride < 28 || c + 28 > size)
        return 0;
    float x0 = u2f(rd32(base + a)), y0 = u2f(rd32(base + a + 4)), u0 = u2f(rd32(base + a + 20));
    float x1 = u2f(rd32(base + c)), y1 = u2f(rd32(base + c + 4)), u1 = u2f(rd32(base + c + 20));
    return fabsf(x1 - x0) < 0.5f && fabsf(y1 - y0) >= 1.0f && fabsf(u1 - u0) > 1e-4f;
}

/* A surface of FFXI's menu target: a render-target texture smaller than the screen of the interface
 * box's shape under --ui-aspect (1280x720 for 16:9), which the game stretches over the whole screen */
static int menu_target(const Obj* o)
{
    const Obj* t = o && o->container ? obj(o->container) : NULL;
    return t && t->kind == O_TEXTURE && (t->usage & USAGE_RENDERTARGET) && ui_box_shape(t->width, t->height);
}

/* FFXI's own character shadow: the character drawn into a render target of the game's, then that
 * texture projected onto the ground around it - a blended 3D draw sampling, through projected
 * coordinates, a texture the game renders into. gameshadows (a scene-effect setting): 0 auto, off
 * whenever the sun's own shadows are on and characters cast them (the two would double) - on, not
 * drawn: by night, in a Mog House or while the effects' pipelines build, the blob must not come
 * back for a frame; 1 always the game's; 2 never. */
static int game_shadow_hidden(const GfxDraw* d)
{
    if (d->vs.rhw || !d->pipe.blend || d->fs.prog)
        return 0;
    float mode = gfx_fx_get("gameshadows");
    if (mode == 1.0f)
        return 0;
    if (mode != 2.0f && !gfx_sun_shadows_on())
        return 0;
    for (int i = 0; i < d->fs.nstages && i < 8; ++i)
    {
        const Obj* t = obj(g_dev.cur.tex[i]);
        if (d->fs.st[i].tex == 1 && d->fs.st[i].projected && t && t->kind == O_TEXTURE && (t->usage & USAGE_RENDERTARGET))
            return 1;
    }
    return 0;
}

/* FFXI_GPU_TIMES: the draw's return address, and the next value up the guest stack that follows a call (as
 * FFXI_DRAWLOG's stack does), for the back end's GPU time per call site. The game draws through wrappers, so the
 * first is one of a few; the second is mostly the wrapper's caller, sometimes a stale local of the wrapper's. */
static uint64_t draw_site(void)
{
    uint32_t ra = rd32(g_cap_esp), up = 0;
    for (uint32_t a = g_cap_esp + 4; a < g_cap_esp + 0x400; a += 4)
    {
        uint32_t v = rd32(a);
        if (v >= rt_image_lo + 6 && v < rt_image_hi &&
            (rd8(v - 5) == 0xE8 || rd8(v - 6) == 0xFF || rd8(v - 3) == 0xFF || rd8(v - 2) == 0xFF))
        {
            up = v;
            break;
        }
    }
    return (uint64_t)up << 32 | ra;
}

/* The pixels a 3D draw of fixed-function vertices first .. first + n can touch in its target, as x0, y0, x1, y1,
 * a pixel round its projected vertices; 0 when that isn't known (a vertex shader, transformed or merged vertices, a
 * vertex at or behind the camera). The vertices are the bound stream's, or a DrawPrimitiveUP's own (up_data). */
static int draw_rect(const GfxDraw* d, uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride, int32_t r[4])
{
    if (d->vs.prog || d->vs.rhw || g_merge.on || !d->vs.el[GFX_R_POSITION].used)
        return 0;
    uint32_t base, stride, off = (uint32_t)d->u.offset[GFX_R_POSITION];
    uint64_t size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0x100000000ull - up_data;
    else
    {
        uint32_t st = d->vs.el[GFX_R_POSITION].stream;
        Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return 0;
        base = b->mem, stride = g_dev.cur.stride[st], size = b->size;
    }
    if (!stride || (uint64_t)(first + n) * stride > size)
        return 0;
    const float* m = d->u.wvp;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = base + (first + i) * stride + off;
        float x = u2f(rd32(at)), y = u2f(rd32(at + 4)), z = u2f(rd32(at + 8));
        float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
        if (!(cw > 1e-4f))
            return 0;
        float cx = (x * m[0] + y * m[4] + z * m[8] + m[12]) / cw, cy = (x * m[1] + y * m[5] + z * m[9] + m[13]) / cw;
        x0 = cx < x0 ? cx : x0, x1 = cx > x1 ? cx : x1, y0 = cy < y0 ? cy : y0, y1 = cy > y1 ? cy : y1;
    }
    if (!(x1 >= x0) || !(y1 >= y0) || x0 < -1e6f || x1 > 1e6f || y0 < -1e6f || y1 > 1e6f)
        return 0;
    float vx = (float)d->vp[0], vy = (float)d->vp[1], vw = (float)d->vp[2], vh = (float)d->vp[3];
    r[0] = (int32_t)floorf(vx + (x0 * 0.5f + 0.5f) * vw) - 2, r[2] = (int32_t)ceilf(vx + (x1 * 0.5f + 0.5f) * vw) + 2;
    r[1] = (int32_t)floorf(vy + (0.5f - y1 * 0.5f) * vh) - 2, r[3] = (int32_t)ceilf(vy + (0.5f - y0 * 0.5f) * vh) + 2;
    return 1;
}

/* A screen-space quad that composites a render-target texture T over the whole target, pixel for pixel (point
 * filtered, its UVs 0..1 on a target T's size) and keeps only what has alpha above 0 from T's alpha: FFXI draws a
 * fading character into a cleared scratch target, then lays the scratch over the scene this way. Where nothing
 * was drawn into T since it was cleared to alpha 0, the test discards every pixel: the quad is scissored to what
 * was (T's wr), or skipped. Returns 0 to skip the draw. */
static int fade_scissor(GfxDraw* d, uint32_t prim, uint32_t count, uint32_t up_data, uint32_t up_stride)
{
    const uint32_t* rs = g_dev.cur.rs;
    const uint32_t* t0 = g_dev.cur.tss[0];
    Obj* tex = obj(g_dev.cur.tex[0]);
    Obj* rt = obj(g_dev.rt);
    if (!up_data || up_stride != 28 || g_dev.cur.vs != 0x144 || prim != 5 || count != 2 || !tex || !rt ||
        tex->kind != O_TEXTURE || !(tex->usage & USAGE_RENDERTARGET) || tex->pw || rt->pw || d->fs.prog ||
        (g_scene.rt && tex->gpu == g_scene.rt)) /* not the scene's own target: the effects write it too */
        return 1;
    /* alpha test GREATER ref (or GREATEREQUAL ref > 0), the alpha a product with T's (MODULATE, 2X, 4X) or T's own */
    uint32_t aop = t0[4], a1 = t0[5], a2 = t0[6];
    int from_t = ((aop == 4 || aop == 5 || aop == 6) && (a1 == 2 || a2 == 2)) || (aop == 2 && a1 == 2) || (aop == 3 && a2 == 2);
    if (!rs[15] || !((rs[25] == 5 && (int32_t)rs[24] >= 0) || (rs[25] == 7 && (int32_t)rs[24] > 0)) || !from_t ||
        g_dev.cur.tss[1][4] != 1 || t0[16] != 1 || t0[17] != 1 || tex->width != rt->width || tex->height != rt->height)
        return 1;
    static const float corner[4][4] = { { 0, 0, 0, 0 }, { 1, 0, 1, 0 }, { 0, 1, 0, 1 }, { 1, 1, 1, 1 } };
    for (int i = 0; i < 4; ++i)
    {
        uint32_t v = up_data + 28u * i;
        if (u2f(rd32(v)) != corner[i][0] * rt->width || u2f(rd32(v + 4)) != corner[i][1] * rt->height ||
            u2f(rd32(v + 20)) != corner[i][2] || u2f(rd32(v + 24)) != corner[i][3])
            return 1;
    }
    if (!tex->wr_track)
    {
        tex->wr_track = 1, tex->wr_known = 0; /* followed from its next clear */
        return 1;
    }
    if (!tex->wr_known)
        return 1;
    int32_t x0 = tex->wr[0] < 0 ? 0 : tex->wr[0], y0 = tex->wr[1] < 0 ? 0 : tex->wr[1];
    int32_t x1 = tex->wr[2] > (int32_t)rt->width ? (int32_t)rt->width : tex->wr[2];
    int32_t y1 = tex->wr[3] > (int32_t)rt->height ? (int32_t)rt->height : tex->wr[3];
    if (x1 <= x0 || y1 <= y0)
        return 0;
    d->scissor[0] = x0, d->scissor[1] = y0, d->scissor[2] = x1 - x0, d->scissor[3] = y1 - y0;
    return 1;
}

/* the current target's texture, when its drawn pixels are followed (fade_scissor) */
static Obj* wr_target(void)
{
    Obj* rt = obj(g_dev.rt);
    Obj* t = rt && rt->container ? obj(rt->container) : NULL;
    return t && t->wr_track ? t : NULL;
}

/* d3d8_probe_draws (d3d8.h): the state at the next draws, logged (the game's thread only: with the worker on its draws
 * run there, and a thread's stdio other than the main one's breaks it on the Switch) */
int d3d8_probe_draws;
static void probe_draw_log(uint32_t prim, uint32_t n)
{
    const uint32_t* rs = g_dev.cur.rs;
    Obj* rt = obj(g_dev.rt);
    Obj* rtt = rt && rt->container ? obj(rt->container) : NULL;
    rt_log("[d3d8] probe draw: prim %u n %u; target %s %ux%u usage %#x; z %u zwrite %u zfunc %u; blend %u %u>%u; "
           "atest %u ref %u func %u; lighting %u fog %u cull %u shade %u; tfactor %08x; colorwrite %#x\n",
        prim, n, rtt ? "texture" : rt && rt->implicit ? "back buffer" : "surface", rt ? rt->width : 0, rt ? rt->height : 0,
        rtt ? rtt->usage : 0, rs[7], rs[14], rs[23], rs[27], rs[19], rs[20], rs[15], rs[24], rs[25], rs[137], rs[28],
        rs[22], rs[9], rs[60], rs[168]);
    for (int s = 0; s < 3; ++s)
    {
        const uint32_t* t = g_dev.cur.tss[s];
        Obj* tx = obj(g_dev.cur.tex[s]);
        rt_log("[d3d8] probe draw:   stage %d: color %u(%u,%u) alpha %u(%u,%u) texcoord %#x transform %u; texture %s %ux%u "
               "usage %#x\n", s, t[1], t[2], t[3], t[4], t[5], t[6], t[11], t[24],
            !tx ? "none" : tx->kind == O_CUBE ? "cube" : "2d", tx ? tx->width : 0, tx ? tx->height : 0, tx ? tx->usage : 0);
    }
}

static void draw_packet(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size,
    uint32_t up_data, uint32_t up_stride, uint32_t n)
{
    if (d3d8_probe_draws > 0 && !d3dw_on_worker())
        --d3d8_probe_draws, probe_draw_log(prim, count);
    if (g_gpu_skip)
    {
        /* what the call draws over must not be waiting to be used: the scratch target is also where a fading
         * character is drawn before it is composited (cleared at the start, not after), so a probe between the
         * two would have been composited with it - drawn then as the game draws it */
        Obj* rt = obj(g_dev.rt);
        Obj* t = rt && rt->container ? obj(rt->container) : NULL;
        if (!g_gpu_skip_seen && t && t->unread)
            g_gpu_skip = 0;
        g_gpu_skip_seen = 1;
        if (g_gpu_skip)
            return;
    }
    static int sites = -1;
    if (sites < 0)
        sites = getenv("FFXI_GPU_TIMES") && *getenv("FFXI_GPU_TIMES");
    GfxDraw* d = &g_draw;
    if (!build_draw(d))
        return;
    if (sites)
        d->site = draw_site();
    if (game_shadow_hidden(d))
    {
        g_scene.st_game_shadows++;
        return;
    }
    uint32_t first = start, nverts = n;
    if (indices)
    {
        uint32_t lo, hi;
        Obj* rib = up_data ? NULL : obj(g_dev.cur.ib);
        if (rib && rib->mem && indices >= rib->mem && indices < rib->mem + rib->size)
            range_cached(rib, indices, index_size, n, &lo, &hi);
        else
            index_range(indices, index_size, n, &lo, &hi);
        first = lo + (up_data ? 0 : g_dev.cur.base_vertex);
        nverts = hi - lo + 1;
        d->u.vofs = -(int32_t)lo;
        d->indices = GUEST_PTR(indices);
        d->index_size = index_size;
        Obj* ib = up_data ? NULL : obj(g_dev.cur.ib);
        if (ib)
            ib->drawn = g_ws.frame;
        GfxBuf* gb = ib && indices >= ib->mem && indices < ib->mem + ib->size ? static_buffer(ib) : NULL;
        /* the index buffer at an offset: Metal's must be a multiple of 4, the other back ends' of the index's size
         * (16-bit indices at 2 mod 4 were copied per draw: on the Switch, ~6 MB a frame in a crowd) */
#if defined(__APPLE__)
        uint32_t ib_align = 4;
#else
        uint32_t ib_align = index_size;
#endif
        if (gb && !((indices - ib->mem) & (ib_align - 1)) && prim != 6 /* fans are rebuilt from the indices */)
            d->ibuf = gb, d->ibuf_off = indices - ib->mem;
    }
    if (!set_streams(d, first, nverts, up_data, up_stride))
        return;
    scene_note(d);
    if (g_cap)
        cap_draw(d, prim, count, first, nverts, up_data, up_stride);
    if (d->vs.rhw && (g_dev.rt == g_dev.backbuffer || menu_target(obj(g_dev.rt))))
    {
        /* the game's cursor is drawn where the mouse was given to it: squeezed when that was
         * unsqueezed (over the interface), as it is when it was given as it is (over the world) */
        int cursor = ui_is_cursor(d, first, nverts, up_data, up_stride);
        if (!cursor && ui_is_world_pointer(first, nverts, up_data, up_stride))
            ; /* the target arrow: where the game put it */
        else if (cursor != 1)
            ui_squeeze(d, first, nverts, up_data, up_stride, !cursor); /* the cursor never marks the interface */
        Obj* t0 = obj(g_dev.cur.tex[0]);
        if (g_dev.rt == g_dev.backbuffer && t0 && t0->kind == O_TEXTURE && (t0->usage & USAGE_RENDERTARGET) &&
            ui_box_shape(t0->width, t0->height))
            g_ui_composited = 1;
        if (g_cap)
            fprintf(g_cap, "  ui vp %.1f %.1f %.1f %.1f clip %u %u %u %u\n", d->u.vp[0], d->u.vp[1], d->u.vp[2], d->u.vp[3],
                d->vp[0], d->vp[1], d->vp[2], d->vp[3]);
    }
    d->prim = prim, d->count = count;
    if (g_fade_scissor && d->vs.rhw && !fade_scissor(d, prim, count, up_data, up_stride))
        return;
    Obj* wt = g_fade_scissor ? wr_target() : NULL;
    if (wt && wt->wr_known)
    {
        int32_t r[4];
        if (draw_rect(d, first, nverts, up_data, up_stride, r))
        {
            if (wt->wr[2] <= wt->wr[0] || wt->wr[3] <= wt->wr[1])
                memcpy(wt->wr, r, sizeof r);
            else
                wt->wr[0] = r[0] < wt->wr[0] ? r[0] : wt->wr[0], wt->wr[1] = r[1] < wt->wr[1] ? r[1] : wt->wr[1],
                wt->wr[2] = r[2] > wt->wr[2] ? r[2] : wt->wr[2], wt->wr[3] = r[3] > wt->wr[3] ? r[3] : wt->wr[3];
        }
        else
            wt->wr_known = 0;
    }
    apply_targets();
    gfx_draw(d);
    if (g_probe_wrapped)
    {
        for (int i = 0; i < 8; ++i)
        {
            Obj* t = (d->fs.prog || i < d->fs.nstages) && d->fs.st[i].tex ? obj(g_dev.cur.tex[i]) : NULL;
            if (t && (t->usage & USAGE_RENDERTARGET))
                t->unread = 0;
        }
        Obj* rt = obj(g_dev.rt);
        Obj* t = rt && rt->container ? obj(rt->container) : NULL;
        if (t)
            t->unread = 1;
    }
    if (g_cap)
        cap_dump_target(g_cap_n - 1);
}

/* --- resources: IDirect3DResource8, textures, buffers, surfaces ------------------------------------------ */
static void Resource_GetDevice(Guest* g)
{
    obj_addref(g_dev.guest);
    wr32(ARG(1), g_dev.guest);
    RET(D3D_OK, 2);
}

static void Resource_SetPriority(Guest* g)
{
    Obj* o = obj(ARG(0));
    uint32_t old = o ? o->priority : 0;
    if (o)
        o->priority = ARG(1);
    RET(old, 2);
}

static void Resource_GetPriority(Guest* g)
{
    Obj* o = obj(ARG(0));
    RET(o ? o->priority : 0, 1);
}

static void Resource_PreLoad(Guest* g) { RET(0, 1); }

static void Resource_GetType(Guest* g)
{
    Obj* o = obj(ARG(0));
    static const uint32_t types[] = { 0, 0, 0, RT_TEXTURE, RT_CUBETEXTURE, RT_VERTEXBUFFER, RT_INDEXBUFFER, RT_SURFACE };
    RET(o ? types[o->kind] : 0, 1);
}

static void Texture_SetLOD(Guest* g)
{
    Obj* o = obj(ARG(0));
    uint32_t old = o ? o->lod : 0;
    if (o && o->pool == 1) /* managed textures only */
        o->lod = ARG(1) < o->levels ? ARG(1) : o->levels - 1;
    RET(old, 2);
}

static void Texture_GetLOD(Guest* g)
{
    Obj* o = obj(ARG(0));
    RET(o ? o->lod : 0, 1);
}

static void Texture_GetLevelCount(Guest* g)
{
    Obj* o = obj(ARG(0));
    RET(o ? o->levels : 0, 1);
}

static void write_surface_desc(uint32_t p, const Obj* s)
{
    wr32(p, s->format);
    wr32(p + 4, RT_SURFACE);
    wr32(p + 8, s->usage);
    wr32(p + 12, s->pool);
    wr32(p + 16, s->size);
    wr32(p + 20, 0); /* MultiSampleType */
    wr32(p + 24, s->width);
    wr32(p + 28, s->height);
}

static Obj* sub(uint32_t tex, uint32_t face, uint32_t level)
{
    Obj* t = obj(tex);
    if (!t || level >= t->levels || face >= (t->kind == O_CUBE ? 6u : 1u))
        return NULL;
    return obj(t->subs[face * t->levels + level]);
}

static void Texture_GetLevelDesc(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    write_surface_desc(ARG(2), s);
    RET(D3D_OK, 3);
}

static void Texture_GetSurfaceLevel(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    wr32(ARG(2), s ? s->guest : 0);
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    obj_addref(s->guest);
    RET(D3D_OK, 3);
}

#define LOCK_READONLY 0x10u

#if defined(FFXI_ANDROID_VULKAN)
/* The lifetimes of the game's occlusion probe objects, for gfx_tex_read_async_keyed: when the
 * Android translation wraps the probe's constructor and destructor (FFXI_WRAP_PROBE_*), each
 * construction gets a new generation, so a probe at a reused address is a different key. The
 * wrappers still run the game's own bodies. Only the guest's (serialized) D3D calls touch this. */
#define ANDROID_PROBE_LIVES 4096u
typedef struct AndroidProbeLife
{
    uint32_t object, generation;
    uint8_t state; /* 0 empty, 1 live, 2 destroyed */
} AndroidProbeLife;
static AndroidProbeLife g_android_probe_lives[ANDROID_PROBE_LIVES];
static int g_android_probe_hooks;
static int android_probe_mapped(uint32_t address, uint32_t size)
{
    if (!address || !size || address > UINT32_MAX - (size - 1))
        return 0;
    uint32_t last = (address + size - 1) & ~0xfffu;
    for (uint32_t page = address & ~0xfffu;; page += 0x1000u)
    {
        if (!gwin_is_committed(page))
            return 0;
        if (page == last)
            return 1;
    }
}
/* the object's entry (open addressing), or with create a free one for it */
static AndroidProbeLife* android_probe_life(uint32_t object, int create)
{
    uint32_t start = ((object >> 2) * 2654435761u) & (ANDROID_PROBE_LIVES - 1);
    AndroidProbeLife* vacant = NULL;
    for (uint32_t n = 0; n < ANDROID_PROBE_LIVES; ++n)
    {
        AndroidProbeLife* entry = &g_android_probe_lives[(start + n) & (ANDROID_PROBE_LIVES - 1)];
        if (entry->state && entry->object == object)
            return entry;
        if (entry->state != 1 && !vacant)
            vacant = entry;
        if (!entry->state)
            break;
    }
    return create ? vacant : NULL;
}
#if defined(FFXI_WRAP_PROBE_CTOR) && defined(FFXI_WRAP_PROBE_DTOR)
static uint32_t g_android_probe_generation;
static void android_probe_created(uint32_t object)
{
    if (!object)
        return;
    if (g_android_probe_generation == UINT32_MAX)
    {
        AndroidProbeLife* old = android_probe_life(object, 0);
        if (old)
        {
            old->generation = 0;
            old->state = 2;
        }
        /* out of generations: rather than wrap, and let a reused address match an earlier
         * lifetime, every read is exact for the rest of the process */
        g_android_probe_hooks = 0;
        return;
    }
    AndroidProbeLife* entry = android_probe_life(object, 1);
    if (!entry)
        return;
    entry->object = object;
    entry->generation = ++g_android_probe_generation;
    entry->state = 1;
}
static void android_probe_destroyed(uint32_t object)
{
    AndroidProbeLife* entry = android_probe_life(object, 0);
    if (entry)
    {
        entry->generation = 0;
        entry->state = 2;
    }
}
extern GuestFn rt_wrap_probe_ctor, rt_wrap_probe_dtor;
extern const GuestFn rt_orig_probe_ctor, rt_orig_probe_dtor;
static void android_wrap_probe_ctor(Guest* guest)
{
    uint32_t object = guest->ecx;
    rt_orig_probe_ctor(guest);
    if (android_probe_mapped(object, 8))
        android_probe_created(object);
}
static void android_wrap_probe_dtor(Guest* guest)
{
    android_probe_destroyed(guest->ecx);
    rt_orig_probe_dtor(guest);
}
/* FNV-1a of guest bytes */
static uint64_t android_probe_hash(uint32_t address, unsigned bytes)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < bytes; ++i)
        hash = (hash ^ rd8(address + i)) * UINT64_C(1099511628211);
    return hash;
}
#endif
static void android_probe_hooks_init(void)
{
#if defined(FFXI_WRAP_PROBE_CTOR) && defined(FFXI_WRAP_PROBE_DTOR)
    uint32_t c = FFXI_WRAP_PROBE_CTOR + RD, d = FFXI_WRAP_PROBE_DTOR + RD;
    if (FFXI_WRAP_PROBE_CTOR == 0x1006c070u && FFXI_WRAP_PROBE_DTOR == 0x1006c080u && !rt_wrap_probe_ctor &&
        !rt_wrap_probe_dtor && rt_orig_probe_ctor && rt_orig_probe_dtor && android_probe_mapped(c, 12) &&
        android_probe_mapped(d, 14) && android_probe_hash(c, 12) == UINT64_C(3548464250665196336) &&
        android_probe_hash(d, 14) == UINT64_C(6811020891934928636))
    {
        rt_wrap_probe_ctor = android_wrap_probe_ctor;
        rt_wrap_probe_dtor = android_wrap_probe_dtor;
        g_android_probe_hooks = 1;
        rt_log("[recomp] d3d8: Android probe lifetime hooks admitted ctor=%08x dtor=%08x; delayed reads up to 16 frames old\n", c,
               d);
    }
    else
        rt_log("[recomp] d3d8: Android probe lifetime anchors differ; all reads stay synchronous\n");
#else
    rt_log("[recomp] d3d8: Android probe lifetime wrappers missing; all reads stay synchronous\n");
#endif
}
/* The key of a read-only lock of a live probe's 16x16 target from the game's one call (returning to
 * 0x1006c72f, with the RECT at 0x1032a17c and the probe in ebp, its surface at +4); 0 for any other. */
static uint64_t android_surface_probe_key(Guest* guest, const Obj* surface, uint32_t rect, uint32_t flags)
{
    if (!g_android_probe_hooks || flags != LOCK_READONLY || surface->width != 16 || surface->height != 16 ||
        surface->format != FMT_A8R8G8B8 || !(surface->usage & USAGE_RENDERTARGET) ||
        !android_probe_mapped(guest->esp, 4) || rd32(guest->esp) != 0x1006c72fu + RD ||
        !android_probe_mapped(guest->ebp, 8) || rd32(guest->ebp + 4) != surface->guest || rect != 0x1032a17cu + RD ||
        !android_probe_mapped(rect, 16) || rd32(rect) != 0 || rd32(rect + 4) != 0 || rd32(rect + 8) != 16 ||
        rd32(rect + 12) != 16)
        return 0;
    AndroidProbeLife* life = android_probe_life(guest->ebp, 0);
    if (!life || life->state != 1 || !life->generation)
        return 0;
    /* the probe's generation and address: unique while the process runs */
    return ((uint64_t)life->generation << 32) | guest->ebp;
}
#define ANDROID_PROBE_ARGUMENT(value) , value
#else
#define ANDROID_PROBE_ARGUMENT(value)
#endif

/* D3DLOCKED_RECT for a surface and an optional RECT. A surface the GPU owns is read back first
 * and written again at unlock; one the game owns is uploaded before its next draw. */
static void lock_rect(Obj* s, uint32_t locked, uint32_t rect, uint32_t flags ANDROID_PROBE_ARGUMENT(uint64_t probe_key))
{
    uint32_t pitch = fmt_pitch(s->format, s->width), bits = obj_mem(s);
    if (gpu_owned(s))
        g_ws.rt_locks++;
    else
    {
        Obj* tc = s->container ? obj(s->container) : NULL;
        g_ws.tex_locks++;
        if (tc && tc->drawn == g_ws.frame)
            g_ws.tex_locks_drawn++;
        if (!(flags & LOCK_READONLY))
            g_ws.tex_lock_bytes += s->size;
    }
    if (gpu_owned(s) && surface_scaled(s))
    {
        /* its pixels are not the game's size: not read back (FFXI never reads its menu target) */
        static int said;
        if (!said)
            said = 1, rt_log("[recomp] d3d8: a lock of a %ux%u render target drawn larger reads nothing\n", s->width, s->height);
    }
    else if (gpu_owned(s))
    {
        uint32_t face, level;
        GfxTex* g = surface_gpu(s, &face, &level);
        if (!s->gpu_locked && !(s->usage & USAGE_DEPTHSTENCIL))
        {
            /* The game's 16x16 occlusion probe (FFXiMain 0x1006c8c0: CopyRects of a 16x16 target, a
             * read-only lock, bit 7 of the blue byte counted over 8x8 samples: 0 hidden .. 256 fully
             * visible) decides what is drawn from these pixels. The occlusion setting (d3d8_set_occlusion)
             * says how it is answered:
             * - 0, fully visible, with no wait for the GPU. That draws what the probe would have hidden,
             *   which the depth test hides anyway. Not so the sky probe: the sun's lens flare draws with
             *   no depth test, so it is the probe that hides it behind walls. It reads a frame late, for
             *   free; the flare writes no depth, so the late answer does not feed back into the next.
             * - 1, the newest copy the GPU has finished, a few frames late: per probe where its lifetime
             *   is known (Android's probe wraps; other probes read exactly there), else the target's newest,
             *   which makes characters flicker, so only Android's menu offers it.
             * - 2, exactly: the CPU waits for the scene so far (7-8 ms a frame at a 4096x4096 background).
             * FFXI_PROBE=gpu reads exactly; FFXI_ASYNC_READBACK=1 makes that late (Android: always late),
             * and on desktop also reads other small read-only locks late. */
            int async = async_readback(), forced = probe_forced();
            int probe = (flags & LOCK_READONLY) && s->width == 16 && s->height == 16, sky = probe && g_probe_sky;
            int mode = forced ? forced : g_occlusion;
            if (probe)
                g_probe_sky = 0;
            /* (the worker running a probe's lock the game's thread answered: the sky's late read goes to its own memory,
             * where the game reads it - probe_lock_now) */
            if (sky && s->sky_mem && d3dw_on_worker())
                bits = s->sky_mem;
#if defined(FFXI_ANDROID_VULKAN)
            if (probe)
                gfx_android_probe_read(probe_key != 0, mode == 1);
            if (probe && mode == 1) /* a probe whose lifetime is not known reads exactly */
                probe_key ? gfx_tex_read_async_keyed(g, face, level, GUEST_PTR(bits), pitch, probe_key, 16)
                          : gfx_tex_read(g, face, level, GUEST_PTR(bits), pitch);
            else
#endif
            if (probe && mode == 0 && !sky)
                memset(GUEST_PTR(bits), 0xFF, (size_t)pitch * s->height);
            else if (probe && (mode == 1 || (mode == 0 && sky)))
                gfx_tex_read_async(g, face, level, GUEST_PTR(bits), pitch);
#if !defined(FFXI_ANDROID_VULKAN)
            else if (async && (flags & LOCK_READONLY) && s->width * s->height <= 128 * 128)
                gfx_tex_read_async(g, face, level, GUEST_PTR(bits), pitch);
#endif
            else
                gfx_tex_read(g, face, level, GUEST_PTR(bits), pitch);
        }
        if (!(flags & LOCK_READONLY) && !(s->usage & USAGE_DEPTHSTENCIL))
            s->gpu_locked = 1;
        Obj* sc = !(flags & LOCK_READONLY) && s->container ? obj(s->container) : NULL;
        if (sc)
            sc->wr_known = 0; /* written by the CPU: what it holds isn't followed (fade_scissor) */
    }
    else if (!(flags & LOCK_READONLY))
        mark_dirty(s);
    if (rect)
    {
        uint32_t l = rd32(rect), t = rd32(rect + 4), blk = fmt_block(s->format);
        bits += blk ? (t / 4) * pitch + (l / 4) * blk : t * pitch + l * fmt_bytes(s->format);
    }
    wr32(locked, pitch);
    wr32(locked + 4, bits);
}

static void unlock_rect(Obj* s)
{
    if (!s->gpu_locked)
        return;
    uint32_t face, level;
    GfxTex* g = surface_gpu(s, &face, &level);
    gfx_tex_upload(g, face, level, GUEST_PTR(s->mem), fmt_pitch(s->format, s->width));
    s->gpu_locked = 0;
}

/* LockRect(Level, pLockedRect, pRect, Flags) */
static void Texture_LockRect(Guest* g)
{
    WORLD_FLUSH();
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 5);
    lock_rect(s, ARG(2), ARG(3), ARG(4) ANDROID_PROBE_ARGUMENT(0));
    RET(D3D_OK, 5);
}

static void Texture_UnlockRect(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    unlock_rect(s);
    RET(D3D_OK, 2);
}
static void Texture_AddDirtyRect(Guest* g) { RET(D3D_OK, 2); }

static void Cube_GetLevelDesc(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    write_surface_desc(ARG(2), s);
    RET(D3D_OK, 3);
}

/* GetCubeMapSurface(FaceType, Level, ppCubeMapSurface) */
static void Cube_GetCubeMapSurface(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    wr32(ARG(3), s ? s->guest : 0);
    if (!s)
        RET(D3DERR_INVALIDCALL, 4);
    obj_addref(s->guest);
    RET(D3D_OK, 4);
}

/* LockRect(FaceType, Level, pLockedRect, pRect, Flags) */
static void Cube_LockRect(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    if (!s)
        RET(D3DERR_INVALIDCALL, 6);
    lock_rect(s, ARG(3), ARG(4), ARG(5) ANDROID_PROBE_ARGUMENT(0));
    RET(D3D_OK, 6);
}

static void Cube_UnlockRect(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    unlock_rect(s);
    RET(D3D_OK, 3);
}
static void Cube_AddDirtyRect(Guest* g) { RET(D3D_OK, 3); }

/* The worker's check of the buffers (FFXI_D3D_CHECK=N, every N frames; 0 off; 30 by default with the worker on): at a
 * Present, with the worker idle, each buffer's two copies are the same unless the game wrote into its copy outside a
 * lock (with a pointer it kept from one) - which the worker never hears of. Each one found is logged and mended
 * (the game's copy over the worker's). */

static void d3dw_check_buffers(void)
{
    if (!g_w.threaded)
        return;
    if (g_bufchk.every < 0)
    {
        const char* e = getenv("FFXI_D3D_CHECK");
        g_bufchk.every = e && *e ? atoi(e) : 30;
        g_bufchk.cur = g_bufchk.every > 0 ? (uint32_t)g_bufchk.every : 0;
        g_bufchk.made_at = g_obj_made;
    }
    if (g_bufchk.every <= 0)
        return;
    /* each check waits for the worker and compares an eighth of the buffers (a ~2 ms stall in a town): while the
     * sweeps find nothing the interval doubles after each whole one, to 16 times FFXI_D3D_CHECK, and goes back to
     * it when one finds a buffer or many objects have come since (a zone's load, where they were found before) */
    if (++g_bufchk.n < g_bufchk.cur)
        return;
    g_bufchk.n = 0;
    if (g_obj_made - g_bufchk.made_at > 64) /* (made since the last check) */
        g_bufchk.cur = (uint32_t)g_bufchk.every, g_bufchk.quiet = 0;
    g_bufchk.made_at = g_obj_made;
    if (++g_bufchk.quiet % 8 == 0 && g_bufchk.cur < 16u * (uint32_t)g_bufchk.every)
        g_bufchk.cur *= 2;
    d3dw_sync_at("the buffer check"); /* (both copies compared with the worker idle; Present doesn't wait) */
    uint64_t t0 = rt_monotonic_ns();
    int found = 0;
    /* an eighth of the objects each time, in turn (all of them at once compared ~22 MB in a town: a 13 ms hitch) */
    uint32_t n = g_nobjs > 1 ? (g_nobjs - 1 + 7) / 8 : 0;
    for (uint32_t k = 0; k < n; ++k)
    {
        if (g_bufchk.next < 1 || g_bufchk.next >= g_nobjs)
            g_bufchk.next = 1;
        uint32_t i = g_bufchk.next++;
        Obj* b = OBJ_AT(i);
        /* (dynamic ones too: only the loose ones - found here - are sent at their draws, front_snapshot; and those
         * all copied at the draws with FFXI_D3D_SNAP=all) */
        if ((b->kind != O_VB && b->kind != O_IB) || !b->fmem || !b->mem || b->lk_depth || b->loose ||
            (snap_all() && (b->usage & USAGE_DYNAMIC)))
            continue;
        g_bufchk.checked++, g_bufchk.bytes += b->size;
        const uint8_t* f = GUEST_PTR(b->fmem);
        uint8_t* m = GUEST_PTR(b->mem);
        if (!memcmp(f, m, b->size))
            continue;
        uint32_t lo = 0, hi = b->size;
        while (lo < hi && f[lo] == m[lo])
            lo++;
        while (hi > lo && f[hi - 1] == m[hi - 1])
            hi--;
        g_bufchk.differ++, g_bufchk.differ_bytes += hi - lo;
        if (g_bufchk.logged < 16)
            g_bufchk.logged++,
                rt_log("[d3d8] worker check: %s %08x (%u bytes, usage %x, pool %u, fvf %x) was written outside a lock: "
                       "bytes %u-%u differ; mended\n",
                    b->kind == O_VB ? "vertex buffer" : "index buffer", b->guest, b->size, b->usage, b->pool, b->fvf, lo,
                    hi);
        memcpy(m + lo, f + lo, hi - lo);
        b->gbuf_dirty = 1, b->serial = ++g_buf_serial;
        b->loose = 1, found = 1; /* (from now on its draws send what they use: front_snapshot) */
    }
    g_bufchk.ns += rt_monotonic_ns() - t0, g_bufchk.runs++;
    /* while it finds some, it looks again at the next frame (a zone's load writes a few at a time) */
    if (found)
        g_bufchk.n = g_bufchk.cur = (uint32_t)g_bufchk.every, g_bufchk.quiet = 0;
}

/* Lock(OffsetToLock, SizeToLock, ppbData, Flags) */
static void Buffer_Lock(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b || ARG(1) > b->size)
        RET(D3DERR_INVALIDCALL, 5);
    g_ws.buf_locks++;
    if (b->drawn == g_ws.frame)
        g_ws.buf_locks_drawn++;
    if (b->gbuf && !(ARG(4) & LOCK_READONLY)) /* a static buffer's: what an upload at unlock would copy */
        g_ws.buf_lock_bytes += ARG(2) ? ARG(2) : b->size - ARG(1);
    if (g_w.threaded)
    {
        /* the game's copy (fmem): its writes go over at the unlock; the worker's (mem) is made here too - the
         * worker has no guest heap */
        obj_mem(b);
        if (!b->fmem)
        {
            b->fmem = gheap_alloc(b->size ? b->size : 16, 1);
            memcpy(GUEST_PTR(b->fmem), GUEST_PTR(b->mem), b->size); /* (as the worker's has it, at its first lock) */
        }
        uint32_t off = ARG(1), n = ARG(2) ? ARG(2) : b->size - off;
        if (n > b->size - off)
            n = b->size - off;
        if (!(ARG(4) & LOCK_READONLY) && n)
        {
            if (!b->lk_write)
                b->lk_off = off, b->lk_n = n, b->lk_write = 1;
            else
            {
                /* (a lock inside another, or one again before the send: both ranges go over) */
                uint32_t lo = off < b->lk_off ? off : b->lk_off;
                uint32_t hi = off + n > b->lk_off + b->lk_n ? off + n : b->lk_off + b->lk_n;
                b->lk_off = lo, b->lk_n = hi - lo;
            }
        }
        if (b->lk_depth)
            g_bufchk.nested++;
        if (b->lk_depth < 255)
            b->lk_depth++;
        wr32(ARG(3), b->fmem + off);
        RET(D3D_OK, 5);
    }
    if (!(ARG(4) & LOCK_READONLY))
        b->gbuf_dirty = 1, b->serial = ++g_buf_serial;
    wr32(ARG(3), obj_mem(b) + ARG(1));
    RET(D3D_OK, 5);
}

/* the worker: what the game wrote into a buffer (Buffer_Unlock), into the worker's copy */
typedef struct WBufData
{
    uint32_t buf, off, n, data; /* the buffer's COM pointer; where; how much; the bytes, in the guest ring */
} WBufData;

static void buf_data(const WBufData* w)
{
    Obj* b = obj(w->buf);
    if (!b || !b->mem || w->off > b->size || w->n > b->size - w->off)
        return;
    memcpy(GUEST_PTR(b->mem + w->off), GUEST_PTR(w->data), w->n);
    b->gbuf_dirty = 1, b->serial = ++g_buf_serial;
}

static void Buffer_Unlock(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (b && b->lk_depth)
        b->lk_depth--;
    if (b && g_w.threaded && !b->lk_depth && b->lk_write && b->lk_n && b->fmem)
    {
        uint32_t data = d3dw_guest(b->lk_n);
        memcpy(GUEST_PTR(data), GUEST_PTR(b->fmem + b->lk_off), b->lk_n);
        WBufData* w = (WBufData*)d3dw_rec(W_BUFDATA, sizeof *w);
        w->buf = ARG(0), w->off = b->lk_off, w->n = b->lk_n, w->data = data;
        d3dw_done();
        g_ws.unlock_bytes += b->lk_n;
        b->lk_write = 0, b->lk_n = 0;
    }
    RET(b ? D3D_OK : D3DERR_INVALIDCALL, 1);
}

static void VertexBuffer_GetDesc(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    uint32_t v[6] = { 100 /* D3DFMT_VERTEXDATA */, RT_VERTEXBUFFER, b->usage, b->pool, b->size, b->fvf };
    memcpy(ARGP(1), v, sizeof v);
    RET(D3D_OK, 2);
}

static void IndexBuffer_GetDesc(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    uint32_t v[5] = { b->format, RT_INDEXBUFFER, b->usage, b->pool, b->size };
    memcpy(ARGP(1), v, sizeof v);
    RET(D3D_OK, 2);
}

static void Surface_GetContainer(Guest* g)
{
    Obj* s = obj(ARG(0));
    uint32_t c = s && s->container ? s->container : g_dev.guest;
    obj_addref(c);
    wr32(ARG(2), c);
    RET(D3D_OK, 3);
}

static void Surface_GetDesc(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    write_surface_desc(ARG(1), s);
    RET(D3D_OK, 2);
}

/* LockRect(pLockedRect, pRect, Flags) */
static void Surface_LockRect(Guest* g)
{
    WORLD_FLUSH();
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 4);
    lock_rect(s, ARG(1), ARG(2), ARG(3) ANDROID_PROBE_ARGUMENT(android_surface_probe_key(g, s, ARG(2), ARG(3))));
    RET(D3D_OK, 4);
}

static void Surface_UnlockRect(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 1);
    unlock_rect(s);
    RET(D3D_OK, 1);
}

/* --- entry point and registration ---------------------------------------------------------------------------- */
/* Direct3DCreate8(SDKVersion) */
static void sh_Direct3DCreate8(Guest* g)
{
    if (!g_d3d)
        g_d3d = obj_new(O_D3D);
    else
        obj_addref(g_d3d);
    RET(g_d3d, 1);
}

/* DebugSetMute(BOOL): D3DX silences the debug runtime; cdecl (the caller pops its argument) */
static void sh_DebugSetMute(Guest* g) { RETC(0); }

/* ValidateVertexShader(pShader, pDeclaration, pCaps, ReturnErrors, ppErrors) and
 * ValidatePixelShader(pShader, pCaps, ReturnErrors, ppErrors), stdcall: what D3DX's shader
 * assembler asks of d3d8.dll after assembling (fetched by GetProcAddress, 0x102ed02c in build
 * 2026-09-03). As Wine: the version token decides (vs.1.0/1.1, ps.1.0-1.4), and there are never
 * error strings. The game's shaders are vs.1.1/ps.1.1, which pass. */
#define E_FAIL 0x80004005u
static void sh_ValidateVertexShader(Guest* g)
{
    uint32_t v = ARG(0) ? rd32(ARG(0)) : 0;
    if (ARG(4))
        wr32(ARG(4), 0);
    RET(v == 0xFFFE0100u || v == 0xFFFE0101u ? D3D_OK : E_FAIL, 5);
}

static void sh_ValidatePixelShader(Guest* g)
{
    uint32_t v = ARG(0) ? rd32(ARG(0)) : 0;
    if (ARG(3))
        wr32(ARG(3), 0);
    RET(v >= 0xFFFF0100u && v <= 0xFFFF0104u ? D3D_OK : E_FAIL, 4);
}

/* --- the calls the worker runs (FFXI_D3D_WORKER): their fronts, on the game's thread ------------------------- */
/* Each front copies its call's argument words (`this` first) and what its pointer arguments point at into the guest
 * ring, the pointers set to the copies, records the call, and returns D3D_OK at once - the worker then runs the
 * shim itself on them (d3dw_run). These never fail here but on arguments the game doesn't pass. Every other d3d8
 * call waits for the worker first (d3dw_pre). */
#if defined(GFX_QUEUE)
/* a render-queue call from the game's thread (a host overlay, an icon's texture) while the worker may be recording
 * draws: it waits for the worker, so one thread records at a time (gfxq_record_hook) */
static void d3dw_gfx_hook(void) { d3dw_sync_at("the render queue from the game's thread"); }
#endif

static int defer_on(void)
{
    if (g_w.mode < 0)
    {
        /* (the scene renderer's flush reads the game's memory where it draws: with the worker, it is sent from the game's
         * thread instead, where there is a front flush - and the worker's draws then send none) */
        d3dw_start(!g_world_flush || g_front_flush);
        if (g_w.threaded && g_front_flush)
            g_world_flush = NULL;
#if defined(GFX_QUEUE)
        if (g_w.threaded)
            gfxq_record_hook = d3dw_gfx_hook;
#else
        if (g_w.threaded)
        {
            /* (no render queue: the back end itself isn't for two threads) */
            fprintf(stderr, "[d3d8] FFXI_D3D_WORKER=1 needs the render queue (-DGFX_QUEUE)\n");
            abort();
        }
#endif
    }
    return g_w.mode;
}

/* A render state, texture stage state or texture set to what it is already (as the game's thread has followed them):
 * 1 when the call does nothing and needn't go to the worker. Those three shims only store the value (or, recording a
 * state block, mark it there: never skipped then); only a state block applied, Reset and CreateDevice set them
 * otherwise, and the first two forget what is known here (the third starts from nothing). A bound texture holds a
 * reference, so the same pointer is the same texture. FFXI_D3D_SKIP=0 sends them all. */
static int front_same(Shim fn, Guest* g)
{
    static int skip = -1;
    if (skip < 0)
    {
        const char* e = getenv("FFXI_D3D_SKIP");
        skip = !(e && *e == '0');
    }
    if (fn == IDirect3DDevice8_ApplyStateBlock) /* (F_ApplyStateBlock follows what it sets) */
        return 0;
    if (g_dev.rec)
        return 0;
    uint32_t* v = NULL;
    uint8_t* ok = NULL;
    uint32_t want = 0, *same = NULL;
    if (fn == IDirect3DDevice8_SetRenderState && ARG(1) < 256)
        v = &g_fm.rs[ARG(1)], ok = &g_fm.rs_ok[ARG(1)], want = ARG(2), g_ws.rs_calls++, same = &g_ws.rs_same;
    else if (fn == IDirect3DDevice8_SetTextureStageState && ARG(1) < 8 && ARG(2) < 32)
        v = &g_fm.tss[ARG(1)][ARG(2)], ok = &g_fm.tss_ok[ARG(1)][ARG(2)], want = ARG(3), g_ws.tss_calls++,
        same = &g_ws.tss_same;
    else if (fn == IDirect3DDevice8_SetTexture && ARG(1) < 8)
        v = &g_fm.tex[ARG(1)], ok = &g_fm.tex_ok[ARG(1)], want = ARG(2), g_ws.tex_calls++, same = &g_ws.tex_same;
    else
        return 0;
    if (*ok && *v == want)
    {
        (*same)++;
        return skip;
    }
    *v = want, *ok = 1;
    return 0;
}

static void defer(Guest* g, Shim fn, uint32_t nwords, const uint32_t* barg, const uint32_t* bbytes, unsigned nblobs)
{
    /* the sky's occlusion probe, as IDirect3DDevice8_DrawPrimitiveUP sees it (transformed vertices at the sky's depth),
     * for probe_lock_now */
    if (fn == IDirect3DDevice8_DrawPrimitiveUP && !(g_fs.vs & 1) && (g_fs.vs & 0xE) == 4 && ARG(3) &&
        rd32(ARG(3) + 8) == 0x3f7ffffeu)
        g_fprobe_sky = 1;
    /* the scene renderer's batch before the work after it (WORLD_FLUSH's calls; a character's draw isn't: world_site on
     * the game's own stack) */
    if (g_front_pending)
    {
        int draw = fn == IDirect3DDevice8_DrawPrimitive || fn == IDirect3DDevice8_DrawIndexedPrimitive ||
            fn == IDirect3DDevice8_DrawPrimitiveUP || fn == IDirect3DDevice8_DrawIndexedPrimitiveUP;
        if ((draw && world_site(g) == 0) || fn == IDirect3DDevice8_Clear || fn == IDirect3DDevice8_SetRenderTarget ||
            fn == IDirect3DDevice8_CopyRects || fn == present_back || fn == Surface_LockRect || fn == Texture_LockRect)
            FRONT_FLUSH();
    }
    if (front_same(fn, g))
    {
        g->eax = D3D_OK;
        g->esp += 4u + 4u * nwords;
        return;
    }
    uint32_t total = (uint32_t)D3DW_ALIGN16(4u * nwords), blob = 0;
    for (unsigned i = 0; i < nblobs; ++i)
        if (ARG(barg[i]) && bbytes[i])
            blob += (uint32_t)D3DW_ALIGN16(bbytes[i]);
    total += blob;
    /* A short call - its arguments and a little they point at (a matrix, a light, rectangles) - goes in its record
     * (W_FAST): no room in the guest ring, no guest writes here. Not the UP draws: their vertices are long, and a draw's
     * may be held past its record (merged with the next draws). */
    if (total <= 1024 && fn != IDirect3DDevice8_DrawPrimitiveUP && fn != IDirect3DDevice8_DrawIndexedPrimitiveUP)
    {
        uint32_t wbytes = (uint32_t)D3DW_ALIGN16(4u * nwords);
        WFast* c = (WFast*)d3dw_rec(W_FAST, (uint32_t)sizeof *c + wbytes + blob);
        c->fn = fn, c->nwords = nwords, c->nblob = blob;
        uint32_t* w = (uint32_t*)(c + 1);
        for (uint32_t i = 0; i < nwords; ++i)
            w[i] = ARG(i);
        uint8_t* b = (uint8_t*)w + wbytes;
        uint32_t at = g_w.scratch + wbytes;
        for (unsigned i = 0; i < nblobs; ++i)
            if (ARG(barg[i]) && bbytes[i])
            {
                memcpy(b, ARGP(barg[i]), bbytes[i]);
                w[barg[i]] = at;
                b += D3DW_ALIGN16(bbytes[i]), at += (uint32_t)D3DW_ALIGN16(bbytes[i]);
            }
        d3dw_done();
        g->eax = D3D_OK;
        g->esp += 4u + 4u * nwords;
        return;
    }
    uint32_t a = d3dw_guest(total);
    for (uint32_t i = 0; i < nwords; ++i)
        wr32(a + 4u * i, ARG(i));
    uint32_t at = a + (uint32_t)D3DW_ALIGN16(4u * nwords);
    for (unsigned i = 0; i < nblobs; ++i)
        if (ARG(barg[i]) && bbytes[i])
        {
            memcpy(GUEST_PTR(at), ARGP(barg[i]), bbytes[i]);
            wr32(a + 4u * barg[i], at);
            at += (uint32_t)D3DW_ALIGN16(bbytes[i]);
        }
    WCall* c = (WCall*)d3dw_rec(W_CALL, sizeof *c);
    c->fn = fn, c->frame = a;
    d3dw_done();
    /* (the UP draws leave stream 0, and the indexed one the indices, unset: g_fs follows) */
    if (fn == IDirect3DDevice8_DrawPrimitiveUP || fn == IDirect3DDevice8_DrawIndexedPrimitiveUP)
    {
        g_fs.stream[0] = 0, g_fs.stride[0] = 0;
        if (fn == IDirect3DDevice8_DrawIndexedPrimitiveUP)
            g_fs.ib = 0;
    }
    g->eax = D3D_OK;
    g->esp += 4u + 4u * nwords;
}

/* a front: method m, n argument words, its pointer arguments' sizes (arg, bytes), 0-2 of them */
#define FRONT(m, n)                                                                                                  \
    static void F_##m(Guest* g)                                                                                      \
    {                                                                                                                \
        if (!defer_on())                                                                                             \
        {                                                                                                            \
            IDirect3DDevice8_##m(g);                                                                                 \
            return;                                                                                                  \
        }                                                                                                            \
        defer(g, IDirect3DDevice8_##m, n, NULL, NULL, 0);                                                            \
    }
#define FRONT1(m, n, a0, b0)                                                                                         \
    static void F_##m(Guest* g)                                                                                      \
    {                                                                                                                \
        if (!defer_on())                                                                                             \
        {                                                                                                            \
            IDirect3DDevice8_##m(g);                                                                                 \
            return;                                                                                                  \
        }                                                                                                            \
        uint32_t ba[1] = { a0 }, bb[1] = { b0 };                                                                     \
        defer(g, IDirect3DDevice8_##m, n, ba, bb, 1);                                                                \
    }
#define FRONT2(m, n, a0, b0, a1, b1)                                                                                 \
    static void F_##m(Guest* g)                                                                                      \
    {                                                                                                                \
        if (!defer_on())                                                                                             \
        {                                                                                                            \
            IDirect3DDevice8_##m(g);                                                                                 \
            return;                                                                                                  \
        }                                                                                                            \
        uint32_t ba[2] = { a0, a1 }, bb[2] = { b0, b1 };                                                             \
        defer(g, IDirect3DDevice8_##m, n, ba, bb, 2);                                                                \
    }
#define CAP(x, most) ((x) < (most) ? (x) : (most))

FRONT(SetRenderState, 3)
FRONT(SetTextureStageState, 4)
FRONT(SetTexture, 3)
/* The transforms as the game set them, kept on its thread too (fxf), so GetTransform - ~100 a frame - needn't wait
 * for the worker: a transform is known here once set (not inside a state block's recording) and unknown again after
 * a state block that holds it is applied, or the device is made or reset */

static void F_SetTransform(Guest* g)
{
    if (!defer_on())
    {
        IDirect3DDevice8_SetTransform(g);
        return;
    }
    int i = xf_index(ARG(1));
    if (i >= 0 && !g_dev.rec)
        memcpy(g_fxf[i], ARGP(2), 64), g_fxf_ok[i] = 1;
    uint32_t ba[1] = { 2 }, bb[1] = { 64 };
    defer(g, IDirect3DDevice8_SetTransform, 3, ba, bb, 1);
}

static void F_MultiplyTransform(Guest* g)
{
    if (!defer_on())
    {
        IDirect3DDevice8_MultiplyTransform(g);
        return;
    }
    int i = xf_index(ARG(1));
    if (i >= 0 && !g_dev.rec)
    {
        if (g_fxf_ok[i])
        {
            /* as IDirect3DDevice8_MultiplyTransform: pMatrix * the matrix */
            float a[16], r[16];
            memcpy(a, ARGP(2), 64);
            const float* b = g_fxf[i];
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    r[y * 4 + x] = a[y * 4] * b[x] + a[y * 4 + 1] * b[4 + x] + a[y * 4 + 2] * b[8 + x] + a[y * 4 + 3] * b[12 + x];
            memcpy(g_fxf[i], r, 64);
        }
    }
    uint32_t ba[1] = { 2 }, bb[1] = { 64 };
    defer(g, IDirect3DDevice8_MultiplyTransform, 3, ba, bb, 1);
}

/* Block.fl: made at a block's first apply. Most of the game's blocks set a few states; one with more than 96 render
 * states, texture stage states and textures (a CreateStateBlock one sets all 520) costs more to follow one by one
 * than it saves - the front forgets them instead, as it did at every apply before. */
static void block_front_list(Block* b)
{
    uint32_t n = 0, nxf = 0;
    for (int i = 0; i < 256; ++i)
        n += b->m.rs[i] != 0;
    for (int t = 0; t < 8; ++t)
    {
        n += b->m.tex[t] != 0;
        for (int s = 0; s < 32; ++s)
            n += b->m.tss[t][s] != 0;
    }
    for (int i = 0; i < NXF; ++i)
        nxf += b->m.xf[i] != 0;
    b->forget = n > 96;
    if (b->forget)
        n = 0;
    b->fl = n + nxf ? (uint16_t*)malloc((n + nxf) * sizeof *b->fl) : NULL;
    if (n + nxf && !b->fl)
    {
        fprintf(stderr, "d3d8: out of memory for a state block's list\n");
        abort();
    }
    uint32_t k = 0;
    if (n)
    {
        for (int i = 0; i < 256; ++i)
            if (b->m.rs[i])
                b->fl[k++] = (uint16_t)i;
        for (int t = 0; t < 8; ++t)
            for (int s = 0; s < 32; ++s)
                if (b->m.tss[t][s])
                    b->fl[k++] = (uint16_t)(1u << 12 | (uint32_t)(t * 32 + s));
        for (int t = 0; t < 8; ++t)
            if (b->m.tex[t])
                b->fl[k++] = (uint16_t)(2u << 12 | (uint32_t)t);
    }
    for (int i = 0; i < NXF; ++i)
        if (b->m.xf[i])
            b->fl[k++] = (uint16_t)(3u << 12 | (uint32_t)i);
    b->nfl = k;
    b->fl_ok = 1;
}

static void F_ApplyStateBlock(Guest* g)
{
    if (!defer_on())
    {
        IDirect3DDevice8_ApplyStateBlock(g);
        return;
    }
    Block* b = block_of(ARG(1));
    if (b && !g_dev.rec)
    {
        /* (a block is the game's thread's to read: only made, recorded into and captured with the worker idle) */
        if (!b->fl_ok)
            block_front_list(b);
        g_ws.applies++;
        if (b->forget)
            memset(&g_fm, 0, sizeof g_fm), g_ws.applies_forget++;
        /* what it sets, as the worker's ApplyStateBlock will (state_copy): known on the game's thread from here */
        for (uint32_t k = 0; k < b->nfl; ++k)
        {
            uint32_t e = b->fl[k], i = e & 0xFFFu;
            switch (e >> 12)
            {
            case 0:
                g_fm.rs[i] = b->s.rs[i], g_fm.rs_ok[i] = 1;
                break;
            case 1:
                g_fm.tss[i >> 5][i & 31] = b->s.tss[i >> 5][i & 31], g_fm.tss_ok[i >> 5][i & 31] = 1;
                break;
            case 2:
                g_fm.tex[i] = b->s.tex[i], g_fm.tex_ok[i] = 1;
                break;
            default:
                g_fxf_ok[i] = 0;
                break;
            }
        }
        for (int i = 0; i < NSTREAMS; ++i)
            if (b->m.stream[i])
                g_fs.stream[i] = b->s.stream[i], g_fs.stride[i] = b->s.stride[i];
        if (b->m.ib)
            g_fs.ib = b->s.ib, g_fs.base = b->s.base_vertex;
        if (b->m.vs)
            g_fs.vs = b->s.vs;
    }
    defer(g, IDirect3DDevice8_ApplyStateBlock, 2, NULL, NULL, 0);
}

static void F_GetTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (g_w.threaded && i >= 0 && g_fxf_ok[i])
    {
        memcpy(ARGP(2), g_fxf[i], 64);
        RET(D3D_OK, 3);
    }
    d3dw_sync_at("F_GetTransform");
    IDirect3DDevice8_GetTransform(g);
    if (g_w.threaded && i >= 0 && !g_dev.rec)
        memcpy(g_fxf[i], g_dev.cur.xf[i], 64), g_fxf_ok[i] = 1;
}

/* (pRenderTarget, pNewZStencil): ~85 a frame in a town; the game's thread reads the targets only in calls that wait */
FRONT(SetRenderTarget, 3)

/* CopyRects(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface, pDestPointsArray): a copy between two
 * GPU surfaces runs on the worker; one with a surface in memory waits (it may make the surface's memory, read the
 * GPU back) */
static void F_CopyRects(Guest* g)
{
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(4));
    if (!defer_on() || !g_w.threaded || !s || !d || !gpu_owned(s) || !gpu_owned(d))
    {
        if (g_w.threaded)
            FRONT_FLUSH();
        d3dw_sync_at("F_CopyRects");
        IDirect3DDevice8_CopyRects(g);
        return;
    }
    uint32_t n = ARG(2) ? CAP(ARG(3), 4096u) : 0;
    uint32_t ba[2] = { 2, 5 }, bb[2] = { 16u * n, 8u * n };
    defer(g, IDirect3DDevice8_CopyRects, 6, ba, bb, 2);
}

/* Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion) with the worker on: the game's thread doesn't
 * wait for the frame to be drawn, only for the one before it to have been presented (at most one frame ahead: the
 * worker draws frame N while the game makes N+1). The host's hook (frame pacing, the host's own per-frame work, which
 * runs the game's code) and the after-present (a resize) stay on the game's thread; d3d8's half, the render queue's
 * present among it, runs on the worker in order with the draws (present_back). */
static void F_Present(Guest* g)
{
    if (!defer_on() || !g_w.threaded)
    {
        IDirect3DDevice8_Present(g);
        return;
    }
    if (g_w.presents_done != g_w.presents_sent)
    {
        uint64_t t0 = rt_monotonic_ns();
        d3dw_flush();
        for (;;)
        {
            uint32_t d = g_w.presents_done;
            if (d == g_w.presents_sent)
                break;
            plat_wait32(&g_w.presents_done, d);
        }
        g_w.present_waits++, g_w.present_wait_ns += rt_monotonic_ns() - t0;
    }
    d3dw_check_buffers(); /* (every FFXI_D3D_CHECK frames: it waits for the worker itself) */
    ws_frame();
    if (g_present_hook)
        g_present_hook();
    g_w.presents_sent++;
    defer(g, present_back, 5, NULL, NULL, 0);
    d3dw_flush(); /* (the frame goes over now, not when the next one's records fill a batch) */
    if (g_after_present)
        g_after_present();
}

/* LockRect of the game's occlusion probe (lock_rect: a 16x16 render target, read only) at occlusion setting 0: its
 * answer is "fully visible", or for the sky's probe a late read - neither needs the worker's results now, so it doesn't
 * wait for it (8 a frame in a town, each a wait for everything recorded). The game is given the surface's memory at
 * once; the worker runs the lock in order with the draws (the sky's flag is theirs) and writes the answer there. A
 * probe's memory, new, says fully visible until then. Any other lock waits as every call does. */
static int probe_lock_now(Obj* s, uint32_t locked, uint32_t rect, uint32_t flags)
{
#if defined(FFXI_ANDROID_VULKAN)
    (void)s, (void)locked, (void)rect, (void)flags;
    return 0;
#else
    /* (the sky probe as the game's thread saw its draw: lock_rect's g_probe_sky, consumed by any probe's lock) */
    int probe = s && (flags & LOCK_READONLY) && s->width == 16 && s->height == 16, sky = probe && g_fprobe_sky;
    if (probe)
        g_fprobe_sky = 0;
    if (!s || !(flags & LOCK_READONLY) || s->width != 16 || s->height != 16 || !gpu_owned(s) || surface_scaled(s) ||
        (s->usage & USAGE_DEPTHSTENCIL) || g_occlusion != 0 || probe_forced() || !locked)
        return 0;
    /* What the game reads now. The others' answer is fully visible: written here, at once. The sky probe's is a late
     * read, which the worker writes when it runs the lock - into memory of the sky's own (sky_mem): in mem, the other
     * probes' answers, written in between, were what the sun's flare read (it showed through walls), and the sky's
     * late answer what the others read (characters went missing where the sun was hidden). */
    uint32_t pitch = fmt_pitch(s->format, s->width), size = pitch * s->height, bits;
    if (sky)
    {
        if (!s->sky_mem && (s->sky_mem = gheap_alloc(size, 1)) != 0)
            memset(GUEST_PTR(s->sky_mem), 0xFF, size); /* (none read yet: visible, as the first late read is) */
        bits = s->sky_mem;
    }
    else if ((bits = obj_mem(s)) != 0)
        memset(GUEST_PTR(bits), 0xFF, size);
    if (!bits)
        return 0;
    if (rect)
    {
        uint32_t l = rd32(rect), t = rd32(rect + 4), blk = fmt_block(s->format);
        bits += blk ? (t / 4) * pitch + (l / 4) * blk : t * pitch + l * fmt_bytes(s->format);
    }
    wr32(locked, pitch);
    wr32(locked + 4, bits);
    g_ws.probe_locks++;
    return 1;
#endif
}

/* (pLockedRect, pRect, Flags) */
static void F_SurfaceLockRect(Guest* g)
{
    if (!defer_on() || !g_w.threaded)
    {
        Surface_LockRect(g);
        return;
    }
    if (!probe_lock_now(obj(ARG(0)), ARG(1), ARG(2), ARG(3)))
    {
        FRONT_FLUSH();
        d3dw_sync_at("LockRect");
        Surface_LockRect(g);
        return;
    }
    uint32_t ba[2] = { 1, 2 }, bb[2] = { 8, 16 }; /* (the worker's D3DLOCKED_RECT and RECT: copies in the ring) */
    defer(g, Surface_LockRect, 4, ba, bb, 2);
}

/* (Level, pLockedRect, pRect, Flags) */
static void F_TextureLockRect(Guest* g)
{
    if (!defer_on() || !g_w.threaded)
    {
        Texture_LockRect(g);
        return;
    }
    if (!probe_lock_now(sub(ARG(0), 0, ARG(1)), ARG(2), ARG(3), ARG(4)))
    {
        FRONT_FLUSH();
        d3dw_sync_at("LockRect");
        Texture_LockRect(g);
        return;
    }
    uint32_t ba[2] = { 2, 3 }, bb[2] = { 8, 16 };
    defer(g, Texture_LockRect, 5, ba, bb, 2);
}

/* UnlockRect: run by the worker, in order (a GPU surface's is the upload of what the game wrote; a read-only lock's
 * does nothing) */
static void F_SurfaceUnlockRect(Guest* g)
{
    if (!defer_on() || !g_w.threaded)
    {
        Surface_UnlockRect(g);
        return;
    }
    defer(g, Surface_UnlockRect, 1, NULL, NULL, 0);
}

static void F_TextureUnlockRect(Guest* g)
{
    if (!defer_on() || !g_w.threaded)
    {
        Texture_UnlockRect(g);
        return;
    }
    defer(g, Texture_UnlockRect, 2, NULL, NULL, 0);
}

/* A dynamic buffer's part a draw uses, as the game's copy has it now, to the worker's before the draw. The game writes
 * some of its dynamic buffers outside a lock (through the pointer a lock gave it, after the unlock - its effects'),
 * which an unlock's send misses; D3D draws what a buffer holds at the draw. */
static void front_snapshot(uint32_t p, uint32_t off, uint32_t bytes)
{
    Obj* b = obj(p);
    if (!b || !b->fmem || !(b->loose || (snap_all() && (b->usage & USAGE_DYNAMIC))) || off >= b->size || !bytes)
        return;
    if (bytes > b->size - off)
        bytes = b->size - off;
    if (bytes > D3DW_GUEST / 8)
        return; /* (not a draw's part: whatever the unlocks sent stands) */
    uint32_t data = d3dw_guest(bytes);
    memcpy(GUEST_PTR(data), GUEST_PTR(b->fmem + off), bytes);
    WBufData* w = (WBufData*)d3dw_rec(W_BUFDATA, sizeof *w);
    w->buf = p, w->off = off, w->n = bytes, w->data = data;
    d3dw_done();
    g_ws.snap_bytes += bytes;
}

/* (only the streams the vertex shader's declaration reads - an FVF reads stream 0 alone - as set_streams does: a
 * stream left set from an earlier draw isn't this one's. Shaders are made and deleted with the worker idle, so the
 * game's thread may read them.) */
static void front_snapshot_streams(uint32_t first, uint32_t n)
{
    uint32_t mask = 1;
    const Shader* sh = vs_of(g_fs.vs);
    if (sh)
    {
        mask = 0;
        for (int r = 0; r < GFX_NREGS; ++r)
            if (sh->lay.el[r].used && sh->lay.el[r].stream < NSTREAMS)
                mask |= 1u << sh->lay.el[r].stream;
    }
    for (int i = 0; i < NSTREAMS; ++i)
        if ((mask >> i & 1) && g_fs.stream[i] && g_fs.stride[i])
            front_snapshot(g_fs.stream[i], first * g_fs.stride[i], n * g_fs.stride[i]);
}

/* (PrimitiveType, StartVertex, PrimitiveCount): the vertices are the worker's copies of the buffers */
static void F_DrawPrimitive(Guest* g)
{
    if (!defer_on())
    {
        IDirect3DDevice8_DrawPrimitive(g);
        return;
    }
    if (g_w.threaded)
        front_snapshot_streams(ARG(2), prim_vertices(ARG(1), ARG(3)));
    defer(g, IDirect3DDevice8_DrawPrimitive, 4, NULL, NULL, 0);
}

/* (PrimitiveType, MinIndex, NumVertices, StartIndex, PrimitiveCount) */
static void F_DrawIndexedPrimitive(Guest* g)
{
    if (!defer_on())
    {
        IDirect3DDevice8_DrawIndexedPrimitive(g);
        return;
    }
    if (g_w.threaded)
    {
        front_snapshot_streams(g_fs.base + ARG(2), ARG(3));
        Obj* ib = obj(g_fs.ib);
        if (ib)
        {
            uint32_t isize = ib->format == FMT_INDEX32 ? 4u : 2u;
            front_snapshot(g_fs.ib, ARG(4) * isize, prim_vertices(ARG(1), ARG(5)) * isize);
        }
    }
    defer(g, IDirect3DDevice8_DrawIndexedPrimitive, 6, NULL, NULL, 0);
}

/* (StreamNumber, pStreamData, Stride), (pIndexData, BaseVertexIndex): followed on the game's thread too (g_fs) */
static void F_SetStreamSource(Guest* g)
{
    if (!defer_on())
    {
        IDirect3DDevice8_SetStreamSource(g);
        return;
    }
    if (!g_dev.rec && ARG(1) < NSTREAMS)
        g_fs.stream[ARG(1)] = ARG(2), g_fs.stride[ARG(1)] = ARG(3);
    defer(g, IDirect3DDevice8_SetStreamSource, 4, NULL, NULL, 0);
}

static void F_SetIndices(Guest* g)
{
    if (!defer_on())
    {
        IDirect3DDevice8_SetIndices(g);
        return;
    }
    if (!g_dev.rec)
        g_fs.ib = ARG(1), g_fs.base = ARG(2);
    defer(g, IDirect3DDevice8_SetIndices, 3, NULL, NULL, 0);
}
FRONT1(SetViewport, 2, 1, 24)
FRONT1(SetMaterial, 2, 1, 68)
FRONT1(SetLight, 3, 2, 104)
FRONT(LightEnable, 3)
FRONT1(SetClipPlane, 3, 2, 16)
/* (Handle): followed on the game's thread too (g_fs.vs) */
static void F_SetVertexShader(Guest* g)
{
    if (!defer_on())
    {
        IDirect3DDevice8_SetVertexShader(g);
        return;
    }
    if (!g_dev.rec)
        g_fs.vs = ARG(1);
    defer(g, IDirect3DDevice8_SetVertexShader, 2, NULL, NULL, 0);
}
FRONT1(SetVertexShaderConstant, 4, 2, 16u * CAP(ARG(3), NVSC))
FRONT(SetPixelShader, 2)
FRONT1(SetPixelShaderConstant, 4, 2, 16u * CAP(ARG(3), NPSC))
FRONT(BeginScene, 1)
FRONT(EndScene, 1)
FRONT1(Clear, 7, 2, 16u * CAP(ARG(1), 4096u))
/* (PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride) */
FRONT1(DrawPrimitiveUP, 5, 3, CAP(prim_vertices(ARG(1), ARG(2)) * ARG(4), D3DW_GUEST / 8))
/* (PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat,
 *  pVertexStreamZeroData, VertexStreamZeroStride): the indices, and the vertices up to the last they may name */
FRONT2(DrawIndexedPrimitiveUP, 9, 5, CAP(prim_vertices(ARG(1), ARG(4)) * (ARG(6) == FMT_INDEX32 ? 4u : 2u), D3DW_GUEST / 8),
    7, CAP((ARG(2) + ARG(3)) * ARG(8), D3DW_GUEST / 8))

/* the calls that don't wait for the worker: those it runs */
static int d3dw_exempt(const char* name)
{
    static const char* const run[] = { "SetRenderState", "SetTextureStageState", "SetTexture", "SetTransform",
        "MultiplyTransform", "SetViewport", "SetMaterial", "SetLight", "LightEnable", "SetClipPlane", "SetStreamSource",
        "SetIndices", "SetVertexShader", "SetVertexShaderConstant", "SetPixelShader", "SetPixelShaderConstant",
        "ApplyStateBlock", "BeginScene", "EndScene", "Clear", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP",
        "DrawPrimitive", "DrawIndexedPrimitive", "GetTransform", "SetRenderTarget", "CopyRects",
        "TestCooperativeLevel" /* (always D3D_OK) */, "Present" /* (waits for the frame before itself: F_Present) */,
        NULL };
    /* the buffers' locks (the game's own copy, sent over at the unlock), and references (atomic; a last Release
     * waits itself, obj_release) */
    if (!strcmp(name, "IDirect3DVertexBuffer8::Lock") || !strcmp(name, "IDirect3DVertexBuffer8::Unlock") ||
        !strcmp(name, "IDirect3DIndexBuffer8::Lock") || !strcmp(name, "IDirect3DIndexBuffer8::Unlock") ||
        !strcmp(name, "IDirect3DTexture8::GetSurfaceLevel") ||
        /* (their fronts wait themselves but for the occlusion probe's, and send the unlocks: F_SurfaceLockRect) */
        !strcmp(name, "IDirect3DSurface8::LockRect") || !strcmp(name, "IDirect3DSurface8::UnlockRect") ||
        !strcmp(name, "IDirect3DTexture8::LockRect") || !strcmp(name, "IDirect3DTexture8::UnlockRect"))
        return 1;
    const char* r = strstr(name, "::");
    if (r && (!strcmp(r, "::AddRef") || !strcmp(r, "::Release")))
        return 1;
    /* what an object is (format, size, usage, pool, levels, fvf): set only by the calls that make or reset it, which
     * wait; the worker never writes it */
    if (r && (!strcmp(r, "::GetDesc") || !strcmp(r, "::GetLevelDesc") || !strcmp(r, "::GetLevelCount") ||
              !strcmp(r, "::GetLOD")))
        return 1;
    const char* m = strstr(name, "IDirect3DDevice8::");
    if (!m)
        return 0;
    m += 18;
    for (int i = 0; run[i]; ++i)
        if (!strcmp(m, run[i]))
            return 1;
    return 0;
}

static void d3dw_pre(uint32_t thunk)
{
    if (g_w.threaded)
        FRONT_FLUSH(); /* (a call that waits: the scene renderer's batch goes first, as WORLD_FLUSH would send it) */
    if (g_w.threaded && d3dw_sync() && thunk < 4096)
        g_wait_by[thunk]++;
}

#define D(i, m) { "d3d8.dll", #i "::" #m, i##_##m }
#define W(i, m) { "d3d8.dll", #i "::" #m, F_##m }
#define U(i, m, f) { "d3d8.dll", #i "::" #m, f }
#define UNKNOWN(i) U(i, QueryInterface, Unknown_QueryInterface), U(i, AddRef, Unknown_AddRef), U(i, Release, Unknown_Release)
#define RESOURCE(i)                                                                                                   \
    UNKNOWN(i), U(i, GetDevice, Resource_GetDevice), U(i, SetPriority, Resource_SetPriority),                         \
        U(i, GetPriority, Resource_GetPriority), U(i, PreLoad, Resource_PreLoad), U(i, GetType, Resource_GetType)

static const ShimDef D3D8[] = {
    { "d3d8.dll", "Direct3DCreate8", sh_Direct3DCreate8 },
    { "d3d8.dll", "DebugSetMute", sh_DebugSetMute },
    { "d3d8.dll", "ValidateVertexShader", sh_ValidateVertexShader },
    { "d3d8.dll", "ValidatePixelShader", sh_ValidatePixelShader },
    UNKNOWN(IDirect3D8),
    D(IDirect3D8, GetAdapterCount),
    D(IDirect3D8, GetAdapterIdentifier),
    D(IDirect3D8, GetAdapterModeCount),
    D(IDirect3D8, EnumAdapterModes),
    D(IDirect3D8, GetAdapterDisplayMode),
    D(IDirect3D8, CheckDeviceType),
    D(IDirect3D8, CheckDeviceFormat),
    D(IDirect3D8, CheckDeviceMultiSampleType),
    D(IDirect3D8, CheckDepthStencilMatch),
    D(IDirect3D8, GetDeviceCaps),
    D(IDirect3D8, GetAdapterMonitor),
    D(IDirect3D8, CreateDevice),

    UNKNOWN(IDirect3DDevice8),
    D(IDirect3DDevice8, TestCooperativeLevel),
    D(IDirect3DDevice8, GetAvailableTextureMem),
    D(IDirect3DDevice8, ResourceManagerDiscardBytes),
    D(IDirect3DDevice8, GetDirect3D),
    D(IDirect3DDevice8, GetDeviceCaps),
    D(IDirect3DDevice8, GetDisplayMode),
    D(IDirect3DDevice8, GetCreationParameters),
    D(IDirect3DDevice8, SetCursorProperties),
    D(IDirect3DDevice8, SetCursorPosition),
    D(IDirect3DDevice8, ShowCursor),
    D(IDirect3DDevice8, Reset),
    W(IDirect3DDevice8, Present),
    D(IDirect3DDevice8, GetBackBuffer),
    D(IDirect3DDevice8, GetRasterStatus),
    D(IDirect3DDevice8, SetGammaRamp),
    D(IDirect3DDevice8, CreateTexture),
    D(IDirect3DDevice8, CreateVolumeTexture),
    D(IDirect3DDevice8, CreateCubeTexture),
    D(IDirect3DDevice8, CreateVertexBuffer),
    D(IDirect3DDevice8, CreateIndexBuffer),
    D(IDirect3DDevice8, CreateRenderTarget),
    D(IDirect3DDevice8, CreateDepthStencilSurface),
    D(IDirect3DDevice8, CreateImageSurface),
    W(IDirect3DDevice8, CopyRects),
    W(IDirect3DDevice8, SetRenderTarget),
    D(IDirect3DDevice8, GetRenderTarget),
    D(IDirect3DDevice8, GetDepthStencilSurface),
    W(IDirect3DDevice8, BeginScene),
    W(IDirect3DDevice8, EndScene),
    W(IDirect3DDevice8, Clear),
    W(IDirect3DDevice8, SetTransform),
    W(IDirect3DDevice8, GetTransform),
    W(IDirect3DDevice8, MultiplyTransform),
    W(IDirect3DDevice8, SetViewport),
    D(IDirect3DDevice8, GetViewport),
    W(IDirect3DDevice8, SetMaterial),
    D(IDirect3DDevice8, GetMaterial),
    W(IDirect3DDevice8, SetLight),
    D(IDirect3DDevice8, GetLight),
    W(IDirect3DDevice8, LightEnable),
    D(IDirect3DDevice8, GetLightEnable),
    W(IDirect3DDevice8, SetClipPlane),
    W(IDirect3DDevice8, SetRenderState),
    D(IDirect3DDevice8, GetRenderState),
    D(IDirect3DDevice8, BeginStateBlock),
    D(IDirect3DDevice8, EndStateBlock),
    W(IDirect3DDevice8, ApplyStateBlock),
    D(IDirect3DDevice8, CaptureStateBlock),
    D(IDirect3DDevice8, DeleteStateBlock),
    D(IDirect3DDevice8, CreateStateBlock),
    D(IDirect3DDevice8, GetTexture),
    W(IDirect3DDevice8, SetTexture),
    D(IDirect3DDevice8, GetTextureStageState),
    W(IDirect3DDevice8, SetTextureStageState),
    D(IDirect3DDevice8, ValidateDevice),
    D(IDirect3DDevice8, SetPaletteEntries),
    D(IDirect3DDevice8, GetPaletteEntries),
    D(IDirect3DDevice8, SetCurrentTexturePalette),
    D(IDirect3DDevice8, GetCurrentTexturePalette),
    W(IDirect3DDevice8, DrawPrimitive),
    W(IDirect3DDevice8, DrawIndexedPrimitive),
    W(IDirect3DDevice8, DrawPrimitiveUP),
    W(IDirect3DDevice8, DrawIndexedPrimitiveUP),
    D(IDirect3DDevice8, CreateVertexShader),
    W(IDirect3DDevice8, SetVertexShader),
    D(IDirect3DDevice8, GetVertexShader),
    D(IDirect3DDevice8, DeleteVertexShader),
    W(IDirect3DDevice8, SetVertexShaderConstant),
    D(IDirect3DDevice8, GetVertexShaderConstant),
    W(IDirect3DDevice8, SetStreamSource),
    D(IDirect3DDevice8, GetStreamSource),
    W(IDirect3DDevice8, SetIndices),
    D(IDirect3DDevice8, GetIndices),
    D(IDirect3DDevice8, CreatePixelShader),
    W(IDirect3DDevice8, SetPixelShader),
    D(IDirect3DDevice8, GetPixelShader),
    D(IDirect3DDevice8, DeletePixelShader),
    W(IDirect3DDevice8, SetPixelShaderConstant),

    RESOURCE(IDirect3DTexture8),
    U(IDirect3DTexture8, SetLOD, Texture_SetLOD),
    U(IDirect3DTexture8, GetLOD, Texture_GetLOD),
    U(IDirect3DTexture8, GetLevelCount, Texture_GetLevelCount),
    U(IDirect3DTexture8, GetLevelDesc, Texture_GetLevelDesc),
    U(IDirect3DTexture8, GetSurfaceLevel, Texture_GetSurfaceLevel),
    U(IDirect3DTexture8, LockRect, F_TextureLockRect),
    U(IDirect3DTexture8, UnlockRect, F_TextureUnlockRect),
    U(IDirect3DTexture8, AddDirtyRect, Texture_AddDirtyRect),

    RESOURCE(IDirect3DCubeTexture8),
    U(IDirect3DCubeTexture8, SetLOD, Texture_SetLOD),
    U(IDirect3DCubeTexture8, GetLOD, Texture_GetLOD),
    U(IDirect3DCubeTexture8, GetLevelCount, Texture_GetLevelCount),
    U(IDirect3DCubeTexture8, GetLevelDesc, Cube_GetLevelDesc),
    U(IDirect3DCubeTexture8, GetCubeMapSurface, Cube_GetCubeMapSurface),
    U(IDirect3DCubeTexture8, LockRect, Cube_LockRect),
    U(IDirect3DCubeTexture8, UnlockRect, Cube_UnlockRect),
    U(IDirect3DCubeTexture8, AddDirtyRect, Cube_AddDirtyRect),

    RESOURCE(IDirect3DVertexBuffer8),
    U(IDirect3DVertexBuffer8, Lock, Buffer_Lock),
    U(IDirect3DVertexBuffer8, Unlock, Buffer_Unlock),
    U(IDirect3DVertexBuffer8, GetDesc, VertexBuffer_GetDesc),

    RESOURCE(IDirect3DIndexBuffer8),
    U(IDirect3DIndexBuffer8, Lock, Buffer_Lock),
    U(IDirect3DIndexBuffer8, Unlock, Buffer_Unlock),
    U(IDirect3DIndexBuffer8, GetDesc, IndexBuffer_GetDesc),

    UNKNOWN(IDirect3DSurface8),
    U(IDirect3DSurface8, GetDevice, Resource_GetDevice),
    U(IDirect3DSurface8, GetContainer, Surface_GetContainer),
    U(IDirect3DSurface8, GetDesc, Surface_GetDesc),
    U(IDirect3DSurface8, LockRect, F_SurfaceLockRect),
    U(IDirect3DSurface8, UnlockRect, F_SurfaceUnlockRect),
    { NULL, NULL, NULL },
};

/* vtable order: the D3D8 ABI (d3d8proxy.cpp's tables) */
#define RESOURCE8_NAMES "QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData", \
    "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType"
static const char* const kD3D8[] = { "QueryInterface", "AddRef", "Release", "RegisterSoftwareDevice", "GetAdapterCount",
    "GetAdapterIdentifier", "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode", "CheckDeviceType",
    "CheckDeviceFormat", "CheckDeviceMultiSampleType", "CheckDepthStencilMatch", "GetDeviceCaps", "GetAdapterMonitor",
    "CreateDevice", NULL };
static const char* const kDevice[] = { "QueryInterface", "AddRef", "Release", "TestCooperativeLevel",
    "GetAvailableTextureMem", "ResourceManagerDiscardBytes", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode",
    "GetCreationParameters", "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain",
    "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetGammaRamp", "GetGammaRamp", "CreateTexture",
    "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget",
    "CreateDepthStencilSurface", "CreateImageSurface", "CopyRects", "UpdateTexture", "GetFrontBuffer",
    "SetRenderTarget", "GetRenderTarget", "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform",
    "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight",
    "GetLight", "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState",
    "BeginStateBlock", "EndStateBlock", "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock",
    "CreateStateBlock", "SetClipStatus", "GetClipStatus", "GetTexture", "SetTexture", "GetTextureStageState",
    "SetTextureStageState", "ValidateDevice", "GetInfo", "SetPaletteEntries", "GetPaletteEntries",
    "SetCurrentTexturePalette", "GetCurrentTexturePalette", "DrawPrimitive", "DrawIndexedPrimitive",
    "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexShader", "SetVertexShader",
    "GetVertexShader", "DeleteVertexShader", "SetVertexShaderConstant", "GetVertexShaderConstant",
    "GetVertexShaderDeclaration", "GetVertexShaderFunction", "SetStreamSource", "GetStreamSource", "SetIndices",
    "GetIndices", "CreatePixelShader", "SetPixelShader", "GetPixelShader", "DeletePixelShader",
    "SetPixelShaderConstant", "GetPixelShaderConstant", "GetPixelShaderFunction", "DrawRectPatch", "DrawTriPatch",
    "DeletePatch", NULL };
static const char* const kTexture[] = { RESOURCE8_NAMES, "SetLOD", "GetLOD", "GetLevelCount", "GetLevelDesc",
    "GetSurfaceLevel", "LockRect", "UnlockRect", "AddDirtyRect", NULL };
static const char* const kCube[] = { RESOURCE8_NAMES, "SetLOD", "GetLOD", "GetLevelCount", "GetLevelDesc",
    "GetCubeMapSurface", "LockRect", "UnlockRect", "AddDirtyRect", NULL };
static const char* const kVB[] = { RESOURCE8_NAMES, "Lock", "Unlock", "GetDesc", NULL };
static const char* const kIB[] = { RESOURCE8_NAMES, "Lock", "Unlock", "GetDesc", NULL };
static const char* const kSurface[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData",
    "GetPrivateData", "FreePrivateData", "GetContainer", "GetDesc", "LockRect", "UnlockRect", NULL };

static uint32_t make_vtbl(const char* iface, const char* const* names)
{
    uint32_t n = 0;
    while (names[n])
        n++;
    uint32_t v = gheap_alloc(4 * n, 1);
    char full[96];
    for (uint32_t i = 0; i < n; ++i)
    {
        snprintf(full, sizeof full, "%s::%s", iface, names[i]);
        wr32(v + 4 * i, thunk_for("d3d8.dll", full));
    }
    return v;
}

void d3d8_init(void)
{
    thunk_register(D3D8);
    /* FFXI_D3D_WORKER=1: every d3d8 call the worker doesn't run waits for it first */
    const char* w = getenv("FFXI_D3D_WORKER");
    if (w && *w == '1')
        thunk_pre_rule("d3d8.dll", d3dw_pre, d3dw_exempt);
}

/* the vtables are guest memory: built once the guest heap is up (from the host, before GameStart) */
void d3d8_setup(void)
{
#if defined(FFXI_ANDROID_VULKAN)
    /* after the images are mapped, before either DLL's DllMain */
    android_probe_hooks_init();
#endif
    g_fade_scissor = !(getenv("FFXI_FADE_SCISSOR") && getenv("FFXI_FADE_SCISSOR")[0] == '0');
    for (int i = 0; i < 3; ++i)
        g_quad_buf[i] = gheap_alloc(4 * 28, 1);
    g_vtbl[O_D3D] = make_vtbl("IDirect3D8", kD3D8);
    g_vtbl[O_DEVICE] = make_vtbl("IDirect3DDevice8", kDevice);
    g_vtbl[O_TEXTURE] = make_vtbl("IDirect3DTexture8", kTexture);
    g_vtbl[O_CUBE] = make_vtbl("IDirect3DCubeTexture8", kCube);
    g_vtbl[O_VB] = make_vtbl("IDirect3DVertexBuffer8", kVB);
    g_vtbl[O_IB] = make_vtbl("IDirect3DIndexBuffer8", kIB);
    g_vtbl[O_SURFACE] = make_vtbl("IDirect3DSurface8", kSurface);
}

/* --- the scene renderer's view of the device (host/scene.c) ------------------------------------------------- */
GfxTex* d3d8_world_texture(uint32_t iface)
{
    d3dw_sync_at("d3d8_world_texture"); /* (the device's state is the worker's while it runs) */
    int kind = 0;
    GfxTex* t = texture_for_draw(iface, &kind);
    return kind == 1 ? t : NULL;
}

GfxTex* d3d8_world_texture_any(uint32_t iface)
{
    d3dw_sync_at("d3d8_world_texture_any"); /* (the device's state is the worker's while it runs) */
    int kind = 0;
    return texture_for_draw(iface, &kind);
}

/* stage 0's sampler as a draw now would have it with this texture (build_draw's) */
int d3d8_world_sampler(uint32_t iface, GfxSampler* out)
{
    d3dw_sync_at("d3d8_world_sampler"); /* (the device's state is the worker's while it runs) */
    Obj* to = obj(iface);
    if (!to)
        return 0;
    const uint32_t* t = g_dev.cur.tss[0];
    uint32_t maxlevel = t[20] > to->lod ? t[20] : to->lod;
    *out = (GfxSampler){ (uint8_t)t[13], (uint8_t)t[14], (uint8_t)t[25], (uint8_t)t[16], (uint8_t)t[17], (uint8_t)t[18], (uint8_t)t[21],
        (uint8_t)maxlevel, 0, { 0 }, t[15] };
    if (to->repl) /* a texture pack's: as build_draw draws it */
        out->mag = out->min = out->mip = 2, out->max_level = 0, out->lod_cap = to->repl_mips ? 0 : 2;
    return 1;
}

GfxBuf* d3d8_world_vbuf(uint32_t iface, uint32_t* size)
{
    d3dw_sync_at("d3d8_world_vbuf"); /* (the device's state is the worker's while it runs) */
    Obj* b = obj(iface);
    if (!b || b->kind != O_VB)
        return NULL;
    if (size)
        *size = b->size;
    return static_buffer(b);
}

uint32_t d3d8_world_vmem(uint32_t iface, uint32_t* size)
{
    /* (the game's thread with the worker on: the game's own copy, as it wrote it; the worker's until its first lock) */
    int front = g_w.threaded && !d3dw_on_worker();
    if (!front)
        d3dw_sync_at("d3d8_world_vmem"); /* (the device's state is the worker's while it runs) */
    Obj* b = obj(iface);
    uint32_t mem = b ? (front && b->fmem ? b->fmem : b->mem) : 0;
    if (!b || (b->kind != O_VB && b->kind != O_IB) || !mem)
        return 0;
    if (size)
        *size = b->size;
    return mem;
}

GfxBuf* d3d8_world_ibuf(uint32_t iface, uint32_t* size, uint32_t* index_size)
{
    d3dw_sync_at("d3d8_world_ibuf"); /* (the device's state is the worker's while it runs) */
    Obj* b = obj(iface);
    if (!b || b->kind != O_IB)
        return NULL;
    if (size)
        *size = b->size;
    if (index_size)
        *index_size = b->format == FMT_INDEX32 ? 4 : 2;
    return static_buffer(b);
}

/* The scene renderer drew the world into the current target with this camera (scene.c flush): the scene goes on (or
 * begins) for the effects, and this camera is the scene's when the game's own draws gave none (scene_finish). With the
 * zone and characters drawn by it, the game's own 3D draws are what is left over (a few fogged objects), and none are in
 * view when the camera looks up at the sky: the scene never began, and the effects went off with the camera's angle.
 * Only a stand-in: taken over the game's camera whenever this drew first, the effects' camera and fog changed with the
 * order of the frame's draws, from frame to frame - a strobe. */
void d3d8_world_scene(const float view[16], const float proj[16], const uint32_t vp[6])
{
    d3dw_sync_at("d3d8_world_scene"); /* (the device's state is the worker's while it runs) */
    uint32_t face, level;
    Obj* rt = obj(g_dev.rt);
    GfxTex* c = rt ? surface_gpu(rt, &face, &level) : NULL;
    if (!c || face || level || !obj(g_dev.ds) || (uint64_t)rt->width * rt->height * 2 < (uint64_t)g_dev.pp[0] * g_dev.pp[1] ||
        proj[11] == 0.0f)
        return;
    for (int i = 0; i < 4; ++i) /* (counted as scene_note counts the game's: the world's target is the most drawn to) */
        if (g_scene.tally[i].t == c || !g_scene.tally[i].t || i == 3)
        {
            if (g_scene.tally[i].t != c)
                g_scene.tally[i].t = c, g_scene.tally[i].n = 0;
            g_scene.tally[i].n++;
            break;
        }
    if (g_scene.world && c != g_scene.world)
        return;
    if (c != g_scene.rt)
    {
        scene_finish("switch");
        g_scene.rt = c, g_scene.draws = 0, g_scene.done = 0, g_scene.cam_rank = 0, g_scene.sun_draw = 0, g_scene.wcam_rt = NULL;
    }
    /* the scene ended before the world was drawn - an interface draw into the target came first, with no camera from
     * the game's draws yet - so the effects didn't run this frame: the scene goes on with the world, and they run
     * where it ends next (a frame without them, now and then, while the camera turns) */
    if (g_scene.done && !g_scene.world_done)
        g_scene.done = 0;
    if (g_scene.done)
        return;
    g_scene.draws++;
    const uint32_t* rs = g_dev.cur.rs;
    GfxScene* w = &g_scene.wcam;
    memcpy(w->proj, proj, 64);
    memcpy(w->view, view, 64);
    memcpy(w->vp, vp, sizeof w->vp);
    color4(w->ambient, rs[139]);
    color4(w->fogcolor, rs[34]);
    w->fog[0] = u2f(rs[36]), w->fog[1] = u2f(rs[37]), w->fog[2] = rs[28] ? 1.0f : 0.0f;
    g_scene.wcam_rt = c;
}

int d3d8_world_begin(uint32_t vp[6], uint32_t game_vp[6])
{
    d3dw_sync_at("d3d8_world_begin"); /* (the device's state is the worker's while it runs) */
    if (!g_dev.rt)
        return 0;
    Obj* wt = g_fade_scissor ? wr_target() : NULL;
    if (wt)
        wt->wr_known = 0; /* the scene renderer's draws into it aren't followed: its composite covers everything (fade_scissor) */
    apply_targets();
    memcpy(game_vp, g_dev.cur.vp, 6 * sizeof *game_vp);
    memcpy(vp, g_dev.cur.vp, 6 * sizeof *vp);
    scale_to_target(vp, NULL, 0);
    return 1;
}
