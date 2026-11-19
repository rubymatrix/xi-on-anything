/* The render thread. See gfx_queue.h. */
#define GFX_QUEUE_IMPL
#include "gfx_queue.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "runtime.h"
#if defined(__SWITCH__)
#include "gthread.h"
#include "sampler.h"
#include "thunk.h"
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <pthread.h>
#endif

/* The back end can be called only from the render thread: WebGPU in the browser (the page gives it the
 * canvas), and OpenGL, whose context is current there alone (gfx_gl.c). Its device is brought up there,
 * and the calls that answer the game (reads, finish, failures) go over to it (call_wait) rather than
 * running on the game's thread once it has caught up (drain). Elsewhere the back ends may be called from
 * any thread. */
#if defined(__EMSCRIPTEN__) || defined(GFX_GL)
#define GFXQ_OWNED 1
#endif

#if defined(__APPLE__)
/* gfx_metal.m's calls make autoreleased objects; a thread of our own drains them per batch */
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);
#endif

/* --- handles ------------------------------------------------------------------------------------------ */
/* What d3d8.c holds as a GfxTex* or GfxBuf*: the back end's object once the render thread has made it. */
typedef struct QTex
{
    GfxTex* real;
    uint32_t fmt, w, h;
    /* the async read's answers: three copies the render thread fills in turn; pub is the newest + 1 */
    uint8_t* async[3];
    uint32_t async_size, async_key;
    volatile uint32_t async_pub;
} QTex;

typedef struct QBuf
{
    GfxBuf* real;
} QBuf;

static GfxTex* real_tex(GfxTex* t) { return t ? ((QTex*)t)->real : NULL; }
static GfxBuf* real_buf(GfxBuf* b) { return b ? ((QBuf*)b)->real : NULL; }

/* --- formats: how many bytes of the game's memory an upload reads -------------------------------- */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static uint32_t fmt_block(uint32_t f) /* bytes per 4x4 block, or 0 */
{
    if (f == FOURCC('D', 'X', 'T', '1'))
        return 8;
    if (f == FOURCC('D', 'X', 'T', '2') || f == FOURCC('D', 'X', 'T', '3') || f == FOURCC('D', 'X', 'T', '4') ||
        f == FOURCC('D', 'X', 'T', '5'))
        return 16;
    return 0;
}

static uint32_t fmt_bytes(uint32_t f) /* bytes per pixel, uncompressed (d3d8.c's fmt_bytes) */
{
    switch (f)
    {
    case 28: /* A8 */
    case 50: /* L8 */
    case 41: /* P8 */
    case 52: /* A4L4 */
        return 1;
    case 23: /* R5G6B5 */
    case 24: /* X1R5G5B5 */
    case 25: /* A1R5G5B5 */
    case 26: /* A4R4G4B4 */
    case 51: /* A8L8 */
    case 60: /* V8U8 */
    case 80: /* D16 */
    case 73: /* D15S1 */
    case 101: /* INDEX16 */
        return 2;
    default: return 4;
    }
}

/* rows of pitch bytes a w x h rectangle spans, and the bytes of its last row */
static size_t rect_bytes(uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch)
{
    uint32_t blk = fmt_block(fmt);
    uint32_t rows = blk ? (h + 3) / 4 : h, row = blk ? (w + 3) / 4 * blk : w * fmt_bytes(fmt);
    if (!rows)
        return 0;
    return (size_t)pitch * (rows - 1) + row;
}

static uint32_t level_dim(uint32_t d, uint32_t level)
{
    d >>= level;
    return d ? d : 1;
}

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

/* the bytes of GfxU the back ends read for a draw (gfx_metal.m's and gfx_d3d12.c's rule) */
static size_t u_need(const GfxDraw* d)
{
    if (d->fs.prog)
        return sizeof(GfxU);
    if (d->vs.prog)
        return offsetof(GfxU, psc);
    return offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
}

/* Shader tokens outlive the draw: the back ends build pipelines from them later, off the thread.
 * One copy per shader hash, kept for good (a few dozen shaders). */
typedef struct Tokens
{
    uint32_t hash;
    const uint32_t* src;
    uint32_t* copy;
} Tokens;

static Tokens* g_tokens;
static unsigned g_ntokens, g_captokens;

static const uint32_t* intern_tokens(uint32_t hash, const uint32_t* t)
{
    if (!t)
        return NULL;
    for (unsigned i = 0; i < g_ntokens; ++i)
        if (g_tokens[i].hash == hash && g_tokens[i].src == t)
            return g_tokens[i].copy;
    size_t n = 1;
    while (t[n - 1] != 0x0000FFFFu && n < 65536)
        n++;
    if (g_ntokens == g_captokens)
    {
        g_captokens = g_captokens ? g_captokens * 2 : 64;
        g_tokens = (Tokens*)realloc(g_tokens, g_captokens * sizeof *g_tokens);
    }
    uint32_t* c = (uint32_t*)malloc(n * 4);
    memcpy(c, t, n * 4);
    g_tokens[g_ntokens++] = (Tokens){ hash, t, c };
    return c;
}

/* --- batches of recorded calls ------------------------------------------------------------------------ */
enum
{
    OP_RESIZE = 1,
    OP_BUF_CREATE,
    OP_BUF_DESTROY,
    OP_BUF_UPLOAD,
    OP_TEX_CREATE,
    OP_TEX_DESTROY,
    OP_TEX_UPLOAD,
    OP_TEX_UPLOAD_RECT,
    OP_TEX_READ_ASYNC,
    OP_COPY,
    OP_SET_TARGETS,
    OP_CLEAR,
    OP_DRAW,
    OP_SCENE_DONE,
    OP_FX_SET,
    OP_PRESENT,
    OP_SET_SYNC_PIPELINES,
    OP_SET_FOCUS,
    OP_SET_MOGHOUSE,
    OP_CALL,
};

/* a call the game's thread waits on (the browser's way to run something where the back end lives) */
struct GfxqCall
{
    void (*fn)(GfxqCall* c);
    void* p[4];
    uint32_t u[6];
    int result;
    volatile uint32_t done;
};

typedef struct Cmd
{
    uint32_t op, size; /* size: this header and its body, a multiple of 16 */
} Cmd;

typedef struct Batch
{
    uint8_t* p;
    size_t n, cap;
    uint32_t cmds;
} Batch;

#define ALIGN16(x) (((x) + 15) & ~(size_t)15)
#define FLUSH_CMDS 48          /* a batch goes to the render thread after this many calls */
#define FLUSH_BYTES (256u << 10) /* or this many bytes */
#define NSLOTS 64u
#define AHEAD 1u /* presents the game may be ahead of the render thread */

static Batch* g_cur;
static Batch* volatile g_slots[NSLOTS]; /* game -> render */
static volatile uint32_t g_head, g_tail;
static Batch* volatile g_free[NSLOTS]; /* render -> game, for reuse */
static volatile uint32_t g_fhead, g_ftail;
static int g_threaded;
static volatile uint32_t g_presents_done;
static uint32_t g_presents_sent;

/* profile (FFXI_PROFILE): the game thread's waits on the render thread, the render thread's work */
static uint64_t g_wait_pace_ns, g_wait_sync_ns, g_bytes, g_since_ns;
static uint64_t g_game_wait_all; /* every wait of the game's thread for the render thread, all along (adapt_interval) */
static uint64_t g_batched_in, g_batched_out; /* FFXI_PROFILE: draws that went into batches of two or more, and those batches */
#if defined(__SWITCH__)
/* The adaptive cap (adapt_interval): the vblanks a frame is shown for (1: 60 fps, 2: 30), the game thread's to
 * choose; the render thread's time running batches all along, of it the time in its presents, and its last swap */
static volatile uint32_t g_interval_want = 1;
static uint32_t g_interval_have = 1;
static volatile uint64_t g_rt_busy_all, g_rt_present_all;
#endif
static void count_vertex_copy(const void* p, size_t n);
static uint64_t g_op_bytes[32], g_u_bytes, g_idx_bytes; /* FFXI_PROFILE: what was recorded, by command; draws' constants */
static uint32_t g_syncs, g_frames;
static volatile uint64_t g_render_busy_ns;

static Batch* batch_get(void)
{
    if (g_ftail != g_fhead)
    {
        Batch* b = g_free[g_ftail % NSLOTS];
        plat_atomic_add32(&g_ftail, 1);
        b->n = 0, b->cmds = 0;
        return b;
    }
    Batch* b = (Batch*)calloc(1, sizeof *b);
    b->cap = FLUSH_BYTES * 2;
    b->p = (uint8_t*)malloc(b->cap);
    return b;
}

static void batch_flush(void);
static int g_batching; /* a batch's draw being recorded (batch_flush): not a reason to flush it */

/* the targets last recorded (gfxq_set_targets), while only draws have been recorded after them */
static struct
{
    GfxTex *color, *depth;
    uint32_t face, level;
} g_targets;
static int g_targets_valid;

/* room for a command with body bytes after the header; returns the body */
static void* rec(uint32_t op, size_t body)
{
    if (!g_batching)
        batch_flush();
    if (op != OP_DRAW && op != OP_SET_TARGETS)
        g_targets_valid = 0; /* (a present, copy, clear or call may have bound others in the back end) */
    if (!g_cur)
        g_cur = batch_get();
    size_t size = ALIGN16(sizeof(Cmd) + body);
    if (g_cur->n + size > g_cur->cap)
    {
        while (g_cur->n + size > g_cur->cap)
            g_cur->cap *= 2;
        uint8_t* p = (uint8_t*)realloc(g_cur->p, g_cur->cap);
        if (!p)
        {
            fprintf(stderr, "gfx queue: out of memory for a %zu-byte batch\n", g_cur->cap);
            abort();
        }
        g_cur->p = p;
    }
    Cmd* c = (Cmd*)(g_cur->p + g_cur->n);
    c->op = op, c->size = (uint32_t)size;
    g_cur->n += size;
    g_cur->cmds++;
    g_bytes += size;
    g_op_bytes[op < 32 ? op : 0] += size;
    return c + 1;
}

static void run_batch(Batch* b);

static void flush(void)
{
    if (!g_cur || !g_cur->cmds)
        return;
    Batch* b = g_cur;
    g_cur = NULL;
    if (!g_threaded)
    {
        run_batch(b);
        b->n = 0, b->cmds = 0;
        g_cur = b;
        return;
    }
    uint64_t w0 = g_head - g_tail < NSLOTS ? 0 : gfx_now_ns();
    for (;;)
    {
        uint32_t t = g_tail;
        if (g_head - t < NSLOTS)
            break;
        plat_wait32(&g_tail, t);
    }
    if (w0)
        g_game_wait_all += gfx_now_ns() - w0;
    g_slots[g_head % NSLOTS] = b;
    plat_atomic_add32(&g_head, 1);
    plat_wake_all32(&g_head);
}

/* after a recorded call: in place without the thread, else maybe hand the batch over */
static void done(void)
{
    if (!g_threaded || g_cur->cmds >= FLUSH_CMDS || g_cur->n >= FLUSH_BYTES)
        flush();
}

static volatile uint32_t g_cur_op, g_ops_run; /* what the render thread is running (the stall watchdog's) */
static void call_wait(GfxqCall* c);
static void nop_call(GfxqCall* c) { gfxq_call_done(c); }

/* everything recorded so far has run: the back end may be called from this thread (not where it is the
 * render thread's alone, GFXQ_OWNED: there, calls go over with call_wait) */
static void drain(void)
{
#if defined(GFXQ_OWNED)
    if (g_threaded)
    {
        GfxqCall c = { nop_call };
        uint64_t t0 = gfx_now_ns();
        call_wait(&c);
        g_wait_sync_ns += gfx_now_ns() - t0;
        g_syncs++;
        return;
    }
#endif
    flush();
    if (!g_threaded)
        return;
    uint64_t t0 = gfx_now_ns();
    for (;;)
    {
        uint32_t t = g_tail;
        if (t == g_head)
            break;
        plat_wait32(&g_tail, t);
    }
    g_wait_sync_ns += gfx_now_ns() - t0;
    g_game_wait_all += gfx_now_ns() - t0;
    g_syncs++;
}

/* --- the render thread ------------------------------------------------------------------------------- */
#define U_PIECE 64u /* u_delta's pieces of GfxU */

typedef struct DrawRec
{
    uint64_t umask; /* the 64-byte pieces of u (within need) that follow, the changed ones (u_delta) */
    uint32_t need, tail_off, data_off[GFX_NSTREAMS], idx_off;
    /* the draw up to u (u's needed bytes, the rest after u, and the payloads follow). Only those bytes: as a whole
     * GfxDraw it was 4 KB a draw, most of it the constants left unused, 6 MB a frame through the queue in a crowd */
    uint8_t head[offsetof(GfxDraw, u)];
} DrawRec;

static void run_draw(const uint8_t* body)
{
    static GfxDraw d; /* the render thread's */
    const DrawRec* r = (const DrawRec*)body;
    memcpy(&d, r->head, sizeof r->head);
    {
        /* u: the pieces that changed since the draw before (u_delta); the render thread's d keeps the rest */
        const uint8_t* src = body + sizeof(DrawRec);
        for (size_t at = 0, i = 0; at < r->need; at += U_PIECE, ++i)
            if (r->umask >> i & 1u)
            {
                size_t len = r->need - at < U_PIECE ? r->need - at : U_PIECE;
                memcpy((uint8_t*)&d.u + at, src, len);
                src += len;
            }
    }
    memcpy(&d.tex, body + r->tail_off, sizeof(GfxDraw) - offsetof(GfxDraw, tex));
    for (int i = 0; i < 8; ++i)
        d.tex[i] = real_tex(d.tex[i]);
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        d.buf[s] = real_buf(d.buf[s]);
        if (r->data_off[s])
            d.data[s] = body + r->data_off[s];
    }
    d.ibuf = real_buf(d.ibuf);
    if (r->idx_off)
        d.indices = body + r->idx_off;
    gfx_draw(&d);
}

typedef struct TexUp
{
    QTex* t;
    uint32_t face, level, x, y, w, h, pitch;
} TexUp;

typedef struct CopyRec
{
    QTex *src, *dst;
    uint32_t sface, slevel, sx, sy, w, h, dface, dlevel, dx, dy;
} CopyRec;

typedef struct ClearRec
{
    uint32_t nrects, flags, color, stencil;
    float z;
    uint32_t vp[6];
} ClearRec;

typedef struct CreateRec
{
    void* h;
    int type, use;
    uint32_t fmt, w, h_, levels, size;
} CreateRec;

static void run_async_read(QTex* t, uint32_t face, uint32_t level, uint32_t pitch)
{
    uint32_t pub = t->async_pub, next = pub % 3; /* pub is 1..3 or 0: write the one after it */
    if (!t->async[next])
        t->async[next] = (uint8_t*)malloc(t->async_size);
    gfx_tex_read_async(t->real, face, level, t->async[next], pitch);
    plat_atomic_cas32(&t->async_pub, pub, next + 1);
}

void gfxq_call_result(GfxqCall* c, int result) { c->result = result; }

void gfxq_call_done(GfxqCall* c)
{
    plat_atomic_add32(&c->done, 1);
    plat_wake_all32(&c->done);
}

static void call_wait(GfxqCall* c)
{
    c->done = 0;
    *(GfxqCall**)rec(OP_CALL, sizeof(void*)) = c;
    flush();
    if (!g_threaded) /* ran in place: a read finishing later can't happen without the page's thread */
        return;
    uint64_t t0 = gfx_now_ns();
    int said = 0;
    while (!c->done)
    {
        plat_wait32_ms(&c->done, 0, 500);
        if (!said && gfx_now_ns() - t0 > 3000000000ull)
            said = 1, rt_log("[recomp] gfx: a call to the render thread has waited 3 s (head %u tail %u, op %u, %u ops run)\n",
                g_head, g_tail, g_cur_op, g_ops_run);
    }
    g_game_wait_all += gfx_now_ns() - t0;
}

static int g_ran_present; /* the batch just run had a Present */

static void run_batch(Batch* b)
{
#if defined(__APPLE__)
    void* pool = objc_autoreleasePoolPush();
#endif
    for (size_t o = 0; o < b->n;)
    {
        const Cmd* c = (const Cmd*)(b->p + o);
        const uint8_t* body = (const uint8_t*)(c + 1);
        o += c->size;
        g_cur_op = c->op, g_ops_run++;
        switch (c->op)
        {
        case OP_RESIZE:
        {
            const uint32_t* a = (const uint32_t*)body;
            gfx_resize(a[0], a[1]);
            break;
        }
        case OP_BUF_CREATE:
        {
            const CreateRec* r = (const CreateRec*)body;
            ((QBuf*)r->h)->real = gfx_buf_create(r->size);
            break;
        }
        case OP_BUF_DESTROY:
        {
            QBuf* q = *(QBuf* const*)body;
            if (q->real)
                gfx_buf_destroy(q->real);
            free(q);
            break;
        }
        case OP_BUF_UPLOAD:
        {
            QBuf* q = *(QBuf* const*)body;
            uint32_t size = *(const uint32_t*)(body + sizeof(void*));
            if (q->real)
                gfx_buf_upload(q->real, body + 16, size);
            break;
        }
        case OP_TEX_CREATE:
        {
            const CreateRec* r = (const CreateRec*)body;
            ((QTex*)r->h)->real = gfx_tex_create(r->type, r->fmt, r->w, r->h_, r->levels, r->use);
            break;
        }
        case OP_TEX_DESTROY:
        {
            QTex* q = *(QTex* const*)body;
            if (q->real)
                gfx_tex_destroy(q->real);
            for (int i = 0; i < 3; ++i)
                free(q->async[i]);
            free(q);
            break;
        }
        case OP_TEX_UPLOAD:
        case OP_TEX_UPLOAD_RECT:
        {
            const TexUp* r = (const TexUp*)body;
            const uint8_t* src = body + ALIGN16(sizeof *r);
            if (!r->t->real)
                break;
            if (c->op == OP_TEX_UPLOAD)
                gfx_tex_upload(r->t->real, r->face, r->level, src, r->pitch);
            else
                gfx_tex_upload_rect(r->t->real, r->face, r->level, r->x, r->y, r->w, r->h, src, r->pitch);
            break;
        }
        case OP_TEX_READ_ASYNC:
        {
            const TexUp* r = (const TexUp*)body;
            if (r->t->real)
                run_async_read(r->t, r->face, r->level, r->pitch);
            break;
        }
        case OP_COPY:
        {
            const CopyRec* r = (const CopyRec*)body;
            gfx_copy(real_tex((GfxTex*)r->src), r->sface, r->slevel, r->sx, r->sy, r->w, r->h, real_tex((GfxTex*)r->dst),
                r->dface, r->dlevel, r->dx, r->dy);
            break;
        }
        case OP_SET_TARGETS:
        {
            GfxTex* const* p = (GfxTex* const*)body;
            const uint32_t* fl = (const uint32_t*)(body + 2 * sizeof(void*));
            gfx_set_targets(real_tex(p[0]), fl[0], fl[1], real_tex(p[1]));
            break;
        }
        case OP_CLEAR:
        {
            const ClearRec* r = (const ClearRec*)body;
            gfx_clear(r->nrects, r->nrects ? (const int32_t*)(body + ALIGN16(sizeof *r)) : NULL, r->flags, r->color, r->z,
                r->stencil, r->vp);
            break;
        }
        case OP_DRAW: run_draw(body); break;
        case OP_SCENE_DONE:
        {
            GfxTex* t = *(GfxTex* const*)body;
            gfx_scene_done(real_tex(t), (const GfxScene*)(body + 16));
            break;
        }
        case OP_FX_SET:
        {
            float v = *(const float*)body;
            gfx_fx_set((const char*)(body + 4), v);
            break;
        }
        case OP_PRESENT:
#if defined(__SWITCH__)
        {
            /* held to 30 or not (adapt_interval), on the window; the present's time is not work */
            uint64_t t = gfx_now_ns();
            if (g_interval_want != g_interval_have)
                g_interval_have = g_interval_want, gfx_nx_swap_interval((int)g_interval_have);
            gfx_present(real_tex(*(GfxTex* const*)body));
            g_rt_present_all += gfx_now_ns() - t;
        }
#else
            gfx_present(real_tex(*(GfxTex* const*)body));
#endif
            g_ran_present = 1;
            plat_atomic_add32(&g_presents_done, 1);
            plat_wake_all32(&g_presents_done);
            break;
        case OP_SET_SYNC_PIPELINES: gfx_set_sync_pipelines(*(const int*)body); break;
        case OP_SET_FOCUS:
        {
            const float* f = (const float*)body;
            gfx_set_focus(f[3] != 0 ? f : NULL);
            break;
        }
        case OP_SET_MOGHOUSE: gfx_set_moghouse(*(const int*)body); break;
        case OP_CALL:
        {
            GfxqCall* call = *(GfxqCall* const*)body;
            call->fn(call);
            break;
        }
        }
    }
#if defined(__APPLE__)
    objc_autoreleasePoolPop(pool);
#endif
}

static void consume(uint32_t t)
{
    {
        Batch* b = g_slots[t % NSLOTS];
        uint64_t t0 = gfx_now_ns();
        run_batch(b);
#if defined(__SWITCH__)
        g_rt_busy_all += gfx_now_ns() - t0;
#endif
        if (gfx_profiling)
            g_render_busy_ns += gfx_now_ns() - t0;
        plat_atomic_add32(&g_tail, 1);
        plat_wake_all32(&g_tail);
        if (g_fhead - g_ftail < NSLOTS)
        {
            g_free[g_fhead % NSLOTS] = b;
            plat_atomic_add32(&g_fhead, 1);
        }
        else
        {
            free(b->p);
            free(b);
        }
    }
}

static void render_thread(void* unused)
{
    (void)unused;
    for (;;)
    {
        uint32_t t = g_tail;
        while (g_head == t)
            plat_wait32(&g_head, t);
        consume(t);
    }
}

int gfxq_pump(void)
{
    g_ran_present = 0;
    while (g_threaded && g_tail != g_head && !g_ran_present)
        consume(g_tail);
    return g_ran_present;
}

#if defined(__EMSCRIPTEN__)
/* The browser's render thread: a worker of its own that returns to its event loop between frames (WebGPU
 * presents, and answers its callbacks, only then), running what the game recorded from its animation
 * frames, a Present at most each. Not the page's main thread: a worker's printf waits for the main
 * thread, which must never wait on anything a worker holds. */
static void web_frame(void)
{
    static int said;
    static uint32_t frames;
    static uint64_t since;
    if (!said)
        said = 1, since = gfx_now_ns(), rt_log("[recomp] gfx: render thread's first frame\n");
    frames++;
    gfx_web_tick();
    if (getenv("XI_QDEBUG") && gfx_now_ns() - since > 2000000000ull)
        rt_log("[recomp] gfx: render thread %u frames in 2 s, head %u tail %u\n", frames, g_head, g_tail), frames = 0,
            since = gfx_now_ns();
    gfxq_pump();
}
EM_JS(int, web_frame_rate, (void), { return typeof requestAnimationFrame == 'function' ? 0 : 60; });

extern void web_give_canvas(uintptr_t thread); /* tools/web/gfx.js */

static void* web_render_thread(void* unused)
{
    (void)unused;
    /* animation frames where the worker has them (a page); else (Node) 60 a second */
    emscripten_set_main_loop(web_frame, web_frame_rate(), 1);
    return NULL;
}
#endif

/* --- the calls --------------------------------------------------------------------------------------- */
static void init_call(GfxqCall* c)
{
#if defined(__EMSCRIPTEN__)
    gfx_init_then(c->p[0], (int)c->u[0], c); /* WebGPU's device comes in callbacks */
#else
    c->result = gfx_init(c->p[0], (int)c->u[0]);
    gfxq_call_done(c);
#endif
}

int gfxq_init(void* sdl_window, int vsync)
{
#if defined(__EMSCRIPTEN__)
    if (!g_threaded)
    {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, web_render_thread, NULL))
            return 0;
        pthread_attr_destroy(&a);
        web_give_canvas((uintptr_t)t); /* the page's canvas, to draw on from there */
        g_threaded = 1;
    }
    GfxqCall c = { init_call, { sdl_window }, { (uint32_t)vsync } };
    call_wait(&c);
    if (c.result)
        rt_log("[recomp] gfx: render thread on, drawing from its animation frames\n");
    return c.result;
#elif defined(GFXQ_OWNED)
    /* the thread first: the device (its context) is made there, and stays there */
    if (!g_threaded)
    {
        const char* e = getenv("FFXI_RENDER_THREAD");
        if (!(e && *e == '0') && plat_thread_start(render_thread, NULL))
        {
            g_threaded = 1;
            rt_log("[recomp] gfx: render thread on, the device's (FFXI_RENDER_THREAD=0 draws on the game's threads)\n");
        }
    }
    GfxqCall c = { init_call, { sdl_window }, { (uint32_t)vsync } };
    call_wait(&c);
    return c.result;
#endif
    drain();
    if (!gfx_init(sdl_window, vsync))
        return 0;
    if (!g_threaded)
    {
        const char* e = getenv("FFXI_RENDER_THREAD");
        if (!(e && *e == '0') && plat_thread_start(render_thread, NULL))
        {
            g_threaded = 1;
            rt_log("[recomp] gfx: render thread on (FFXI_RENDER_THREAD=0 turns it off)\n");
        }
    }
    return 1;
}

void gfxq_resize(uint32_t w, uint32_t h)
{
    uint32_t* a = (uint32_t*)rec(OP_RESIZE, 8);
    a[0] = w, a[1] = h;
    done();
}

GfxBuf* gfxq_buf_create(uint32_t size)
{
    QBuf* q = (QBuf*)calloc(1, sizeof *q);
    CreateRec* r = (CreateRec*)rec(OP_BUF_CREATE, sizeof *r);
    memset(r, 0, sizeof *r);
    r->h = q, r->size = size;
    done();
    return (GfxBuf*)q;
}

void gfxq_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    *(QBuf**)rec(OP_BUF_DESTROY, sizeof(void*)) = (QBuf*)b;
    done();
}

void gfxq_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b)
        return;
    uint8_t* p = (uint8_t*)rec(OP_BUF_UPLOAD, 16 + (size_t)size);
    *(QBuf**)p = (QBuf*)b;
    *(uint32_t*)(p + sizeof(void*)) = size;
    memcpy(p + 16, data, size);
    done();
}

GfxTex* gfxq_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    QTex* q = (QTex*)calloc(1, sizeof *q);
    q->fmt = d3dfmt, q->w = w, q->h = h;
    CreateRec* r = (CreateRec*)rec(OP_TEX_CREATE, sizeof *r);
    memset(r, 0, sizeof *r);
    r->h = q, r->type = type, r->use = use, r->fmt = d3dfmt, r->w = w, r->h_ = h, r->levels = levels;
    done();
    return (GfxTex*)q;
}

void gfxq_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    *(QTex**)rec(OP_TEX_DESTROY, sizeof(void*)) = (QTex*)t;
    done();
}

static void tex_up(int op, QTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    size_t bytes = rect_bytes(t->fmt, w, h, pitch);
    TexUp* r = (TexUp*)rec((uint32_t)op, ALIGN16(sizeof *r) + bytes);
    *r = (TexUp){ t, face, level, x, y, w, h, pitch };
    memcpy((uint8_t*)r + ALIGN16(sizeof *r), src, bytes);
    done();
}

void gfxq_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (!t)
        return;
    QTex* q = (QTex*)t;
    tex_up(OP_TEX_UPLOAD, q, face, level, 0, 0, level_dim(q->w, level), level_dim(q->h, level), src, pitch);
}

void gfxq_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    if (t)
        tex_up(OP_TEX_UPLOAD_RECT, (QTex*)t, face, level, x, y, w, h, src, pitch);
}

static void read_call(GfxqCall* c)
{
    gfx_tex_read_then(real_tex((GfxTex*)c->p[0]), c->u[0], c->u[1], c->p[1], c->u[2], c);
}

void gfxq_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
#if defined(GFXQ_OWNED)
    GfxqCall c = { read_call, { t, dst }, { face, level, pitch } };
    call_wait(&c);
    return;
#endif
    drain();
    gfx_tex_read(real_tex(t), face, level, dst, pitch);
}

void gfxq_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    QTex* q = (QTex*)t;
    if (!q)
        return;
    uint32_t key = face << 16 | level, lh = level_dim(q->h, level);
    uint32_t size = (uint32_t)((size_t)pitch * (fmt_block(q->fmt) ? (lh + 3) / 4 : lh));
    uint32_t pub = q->async_pub;
    if (!g_threaded || !pub || q->async_key != key || q->async_size != size)
    {
        /* the first read of this level (or no thread): in place, as the back end would */
        for (int i = 0; i < 3; ++i)
            free(q->async[i]), q->async[i] = NULL;
        q->async_key = key, q->async_size = size, q->async_pub = 0;
        gfxq_tex_read(t, face, level, dst, pitch); /* waits: the first answer is a real one */
        q->async[0] = (uint8_t*)malloc(size);
        memcpy(q->async[0], dst, size);
        q->async_pub = 1;
        return;
    }
    memcpy(dst, q->async[pub - 1], size);
    TexUp* r = (TexUp*)rec(OP_TEX_READ_ASYNC, sizeof *r);
    *r = (TexUp){ q, face, level, 0, 0, 0, 0, pitch };
    done();
}

void gfxq_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
    GfxTex* dst, uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    CopyRec* r = (CopyRec*)rec(OP_COPY, sizeof *r);
    *r = (CopyRec){ (QTex*)src, (QTex*)dst, sface, slevel, sx, sy, w, h, dface, dlevel, dx, dy };
    done();
}

void gfxq_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    /* d3d8.c sets the targets before every draw: the same ones again, with only draws recorded since (draws don't
     * change them), need no command - which would also end a batch (above) at every draw */
    if (g_targets_valid && g_targets.color == color && g_targets.depth == depth && g_targets.face == face &&
        g_targets.level == level)
        return;
    uint8_t* p = (uint8_t*)rec(OP_SET_TARGETS, 2 * sizeof(void*) + 8);
    g_targets.color = color, g_targets.depth = depth, g_targets.face = face, g_targets.level = level;
    g_targets_valid = 1;
    ((GfxTex**)p)[0] = color, ((GfxTex**)p)[1] = depth;
    uint32_t* fl = (uint32_t*)(p + 2 * sizeof(void*));
    fl[0] = face, fl[1] = level;
    done();
}

void gfxq_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const uint32_t vp[6])
{
    if (!rects)
        nrects = 0;
    ClearRec* r = (ClearRec*)rec(OP_CLEAR, ALIGN16(sizeof *r) + (size_t)nrects * 16);
    r->nrects = nrects, r->flags = flags, r->color = color, r->z = z, r->stencil = stencil;
    memcpy(r->vp, vp, sizeof r->vp);
    if (nrects)
        memcpy((uint8_t*)r + ALIGN16(sizeof *r), rects, (size_t)nrects * 16);
    done();
}

static void record_draw(const GfxDraw* d);

/* --- batching: consecutive draws from the game's own memory (DrawPrimitiveUP) with the same state -------------
 * FFXI draws its interface and text as hundreds of small quads a frame (600 DrawPrimitiveUP calls at 22,000 a
 * second, 82% of them with the same state as the one before: docs/switch.md). Such a draw is held here instead
 * of recorded: the next one with the same state - all but its vertices and primitive count - adds its vertices
 * to it, strips and fans made a triangle list with 16-bit indices, and the batch is recorded as one draw when
 * anything else is (rec), so nothing is reordered. Only stream 0 in memory, no indices, from vertex 0, a
 * triangle primitive. FFXI_BATCH=0 records every draw as it comes (the default off except on the Switch). */
#define BATCH_VERTS 65535u
static struct
{
    int on;               /* -1 not read yet */
    int active;
    GfxDraw d;            /* the first draw's state */
    size_t need;          /* its uniforms' bytes (u_need) */
    uint8_t* v;           /* the vertices so far */
    size_t v_cap, v_n;    /* bytes */
    uint16_t* idx;
    size_t i_cap, i_n;    /* indices */
    uint32_t verts, stride, draws;
} g_batch = { -1 };

static int batch_on(void)
{
    if (g_batch.on < 0)
    {
        const char* e = getenv("FFXI_BATCH");
#if defined(__SWITCH__)
        g_batch.on = !(e && *e == '0');
#else
        g_batch.on = e && *e == '1';
#endif
    }
    return g_batch.on;
}

/* a draw a batch can take: its vertices in memory in stream 0 alone, no indices, a triangle primitive */
static int batchable(const GfxDraw* d)
{
    if (!d->data[0] || d->buf[0] || d->indices || d->ibuf || d->vertex_start || d->u.stride[0] <= 0)
        return 0;
    if (d->prim != GFX_TRIANGLELIST && d->prim != GFX_TRIANGLESTRIP && d->prim != GFX_TRIANGLEFAN)
        return 0;
    for (int s = 1; s < GFX_NSTREAMS; ++s)
        if (d->data[s] || d->buf[s])
            return 0;
    uint32_t n = vertex_count(d->prim, d->count);
    return n && n <= BATCH_VERTS && (size_t)n * (uint32_t)d->u.stride[0] <= d->size[0];
}

/* the same state as the batch's: keys, the uniforms the back ends read, textures, samplers, viewport, scissor */
static int same_state(const GfxDraw* a, const GfxDraw* b, size_t need)
{
    return !memcmp(a, b, offsetof(GfxDraw, u)) && !memcmp(&a->u, &b->u, need) &&
           !memcmp(&a->tex, &b->tex, offsetof(GfxDraw, data) - offsetof(GfxDraw, tex));
}

static void* grow(void* p, size_t* cap, size_t want)
{
    if (want <= *cap)
        return p;
    size_t c = *cap ? *cap : 4096;
    while (c < want)
        c *= 2;
    void* q = realloc(p, c);
    if (!q)
    {
        fprintf(stderr, "gfx queue: out of memory for a %zu-byte batch\n", c);
        abort();
    }
    *cap = c;
    return q;
}

/* d's vertices and its triangles' indices onto the batch */
static void batch_add(const GfxDraw* d)
{
    uint32_t n = vertex_count(d->prim, d->count), stride = (uint32_t)d->u.stride[0], b = g_batch.verts;
    g_batch.v = (uint8_t*)grow(g_batch.v, &g_batch.v_cap, g_batch.v_n + (size_t)n * stride);
    if (gfx_profiling)
        count_vertex_copy(d->data[0], (size_t)n * stride);
    memcpy(g_batch.v + g_batch.v_n, d->data[0], (size_t)n * stride);
    g_batch.v_n += (size_t)n * stride;
    g_batch.idx = (uint16_t*)grow(g_batch.idx, &g_batch.i_cap, (g_batch.i_n + (size_t)d->count * 3) * 2) ;
    uint16_t* o = g_batch.idx + g_batch.i_n;
    for (uint32_t t = 0; t < d->count; ++t, o += 3)
    {
        if (d->prim == GFX_TRIANGLELIST)
            o[0] = (uint16_t)(b + 3 * t), o[1] = (uint16_t)(b + 3 * t + 1), o[2] = (uint16_t)(b + 3 * t + 2);
        else if (d->prim == GFX_TRIANGLEFAN)
            o[0] = (uint16_t)b, o[1] = (uint16_t)(b + t + 1), o[2] = (uint16_t)(b + t + 2);
        else if (t & 1) /* a strip's odd triangle: its first two swapped, so every one keeps the strip's winding */
            o[0] = (uint16_t)(b + t + 1), o[1] = (uint16_t)(b + t), o[2] = (uint16_t)(b + t + 2);
        else
            o[0] = (uint16_t)(b + t), o[1] = (uint16_t)(b + t + 1), o[2] = (uint16_t)(b + t + 2);
    }
    g_batch.i_n += (size_t)d->count * 3;
    g_batch.verts += n;
    g_batch.draws++;
}

/* the batch as one draw: a single draw as it came, more as a triangle list over the vertices gathered */
static void batch_flush(void)
{
    if (!g_batch.active)
        return;
    g_batch.active = 0;
    GfxDraw d = g_batch.d;
    d.data[0] = g_batch.v; /* the copy, the game's own may have changed */
    d.size[0] = (uint32_t)g_batch.v_n;
    if (g_batch.draws > 1)
    {
        g_batched_in += g_batch.draws, g_batched_out++;
        d.prim = GFX_TRIANGLELIST;
        d.count = (uint32_t)(g_batch.i_n / 3);
        d.data[0] = g_batch.v;
        d.size[0] = (uint32_t)g_batch.v_n;
        d.indices = g_batch.idx;
        d.index_size = 2;
    }
    g_batching = 1;
    record_draw(&d);
    g_batching = 0;
}

void gfxq_draw(const GfxDraw* d)
{
    if (!d->count)
        return;
    if (!batch_on() || !batchable(d))
    {
        batch_flush();
        record_draw(d);
        return;
    }
    uint32_t n = vertex_count(d->prim, d->count);
    if (g_batch.active && (uint32_t)d->u.stride[0] == g_batch.stride && g_batch.verts + n <= BATCH_VERTS &&
        same_state(d, &g_batch.d, g_batch.need))
    {
        batch_add(d);
        return;
    }
    batch_flush();
    /* a new batch, its vertices copied now: they are the game's memory, which it may write again as soon as the
     * call returns (DrawPrimitiveUP's) */
    g_batch.d = *d;
    g_batch.need = u_need(d);
    g_batch.stride = (uint32_t)d->u.stride[0];
    g_batch.v_n = g_batch.i_n = 0, g_batch.verts = 0, g_batch.draws = 0;
    batch_add(d);
    g_batch.active = 1;
}

/* FFXI_PROFILE: the vertex bytes draws copy, and of them those of a range (the same memory, the same size) already
 * copied this frame - what copying each range once a frame would save at most (the game may write it in between) */
static uint64_t g_vtx_bytes, g_vtx_again;
static uint32_t g_vtx_frame = 1;
static struct { const void* p; size_t n; uint32_t frame; } g_vtx_seen[4096];

static void count_vertex_copy(const void* p, size_t n)
{
    uintptr_t k = (uintptr_t)p ^ (n << 7);
    unsigned i = (unsigned)((k ^ (k >> 12) ^ (k >> 24)) & 4095u);
    g_vtx_bytes += n;
    for (unsigned j = 0; j < 4; ++j, i = (i + 1) & 4095u)
    {
        if (g_vtx_seen[i].frame == g_vtx_frame && g_vtx_seen[i].p == p && g_vtx_seen[i].n == n)
        {
            g_vtx_again += n;
            return;
        }
        if (g_vtx_seen[i].frame != g_vtx_frame)
        {
            g_vtx_seen[i].p = p, g_vtx_seen[i].n = n, g_vtx_seen[i].frame = g_vtx_frame;
            return;
        }
    }
}

/* The constants (GfxU, ~2.5 KB a draw: 3 MB a frame in a crowd) mostly repeat from one draw to the next: a draw
 * records only the 64-byte pieces of them (within its need) that differ from what the render thread's draw already
 * has, which g_u_shadow mirrors. The render thread runs the draws in the order recorded, so the two agree. */
typedef char u_pieces_fit[(sizeof(GfxU) + U_PIECE - 1) / U_PIECE <= 64 ? 1 : -1];
static GfxU g_u_shadow;
static int g_u_shadow_ok;

static size_t u_delta(const GfxDraw* d, size_t need, uint64_t* mask)
{
    uint64_t m = 0;
    size_t bytes = 0;
    const uint8_t *a = (const uint8_t*)&d->u, *b = (const uint8_t*)&g_u_shadow;
    for (size_t at = 0, i = 0; at < need; at += U_PIECE, ++i)
    {
        size_t len = need - at < U_PIECE ? need - at : U_PIECE;
        int differs;
        if (len == U_PIECE)
        {
            /* a whole piece as eight words, inline (memcmp's call per piece was 1.7% of the game thread) */
            uint64_t x = 0;
            for (unsigned k = 0; k < U_PIECE / 8u; ++k)
            {
                uint64_t p, q;
                memcpy(&p, a + at + 8u * k, 8), memcpy(&q, b + at + 8u * k, 8);
                x |= p ^ q;
            }
            differs = x != 0;
        }
        else
            differs = memcmp(a + at, b + at, len) != 0;
        if (!g_u_shadow_ok || differs)
            m |= 1ull << i, bytes += len;
    }
    *mask = m;
    return bytes;
}

static void record_draw(const GfxDraw* d)
{
    size_t need = u_need(d), tail = sizeof(GfxDraw) - offsetof(GfxDraw, tex);
    size_t data_bytes[GFX_NSTREAMS] = { 0 }, idx_bytes = 0;
    uint64_t umask;
    size_t usent = u_delta(d, need, &umask);
    size_t off = ALIGN16(sizeof(DrawRec) + usent);
    size_t tail_off = off;
    off = ALIGN16(off + tail);
    size_t total = off;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (!d->buf[s] && d->data[s] && d->size[s])
            data_bytes[s] = d->size[s], total = ALIGN16(total + d->size[s]);
    if (d->indices && (!d->ibuf || d->prim == GFX_TRIANGLEFAN))
    {
        uint32_t n = d->prim == GFX_TRIANGLEFAN ? d->count + 2 : vertex_count(d->prim, d->count);
        idx_bytes = (size_t)n * d->index_size;
        total = ALIGN16(total + idx_bytes);
    }

    uint8_t* body = (uint8_t*)rec(OP_DRAW, total);
    DrawRec* r = (DrawRec*)body;
    memcpy(r->head, d, sizeof r->head);
    const uint32_t* vt = intern_tokens(d->vs.prog, d->vs_tokens);
    const uint32_t* pt = intern_tokens(d->fs.prog, d->ps_tokens);
    memcpy(r->head + offsetof(GfxDraw, vs_tokens), &vt, sizeof vt);
    memcpy(r->head + offsetof(GfxDraw, ps_tokens), &pt, sizeof pt);
    r->need = (uint32_t)need, r->tail_off = (uint32_t)tail_off;
    g_u_bytes += usent + tail;
    r->umask = umask;
    {
        uint8_t* dst = body + sizeof(DrawRec);
        for (size_t at = 0, i = 0; at < need; at += U_PIECE, ++i)
            if (umask >> i & 1u)
            {
                size_t len = need - at < U_PIECE ? need - at : U_PIECE;
                memcpy(dst, (const uint8_t*)&d->u + at, len);
                memcpy((uint8_t*)&g_u_shadow + at, (const uint8_t*)&d->u + at, len);
                dst += len;
            }
        g_u_shadow_ok = 1;
    }
    memcpy(body + tail_off, &d->tex, tail);
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        r->data_off[s] = 0;
        if (data_bytes[s])
        {
            if (gfx_profiling && !g_batching) /* (a batch's vertices were counted as they went in) */
                count_vertex_copy(d->data[s], data_bytes[s]);
            memcpy(body + off, d->data[s], data_bytes[s]);
            r->data_off[s] = (uint32_t)off;
            off = ALIGN16(off + data_bytes[s]);
        }
    }
    r->idx_off = 0;
    if (idx_bytes)
    {
        memcpy(body + off, d->indices, idx_bytes);
        g_idx_bytes += idx_bytes;
        r->idx_off = (uint32_t)off;
    }
    done();
}

void gfxq_scene_done(GfxTex* color, const GfxScene* s)
{
    uint8_t* p = (uint8_t*)rec(OP_SCENE_DONE, 16 + sizeof *s);
    *(GfxTex**)p = color;
    memcpy(p + 16, s, sizeof *s);
    done();
}

void gfxq_fx_set(const char* key, float v)
{
    size_t n = strlen(key) + 1;
    uint8_t* p = (uint8_t*)rec(OP_FX_SET, 4 + n);
    memcpy(p, &v, 4);
    memcpy(p + 4, key, n);
    done();
}

static void profile_line(void)
{
    uint64_t now = gfx_now_ns();
    g_frames++;
    g_vtx_frame++;
    if (!g_since_ns)
        g_since_ns = now;
    if (now - g_since_ns < 2000000000ull)
        return;
    double ms = 1e-6 / g_frames;
    fprintf(stderr,
        "[gfx]   render thread: busy %.2f ms/frame; the game waited %.2f ms/frame for it (%.2f pacing, %.2f in %u syncs), "
        "%.0f KB recorded/frame (vertices %.0f KB, %.0f KB of them a range copied before that frame); "
        "%.0f draws/frame batched into %.0f; KB/frame: draws %.0f (constants and state %.0f, indices %.0f), buffer uploads %.0f, "
        "texture uploads %.0f\n",
        (double)g_render_busy_ns * ms, (double)(g_wait_pace_ns + g_wait_sync_ns) * ms, (double)g_wait_pace_ns * ms,
        (double)g_wait_sync_ns * ms, g_syncs, (double)g_bytes / g_frames / 1024.0,
        (double)g_vtx_bytes / g_frames / 1024.0, (double)g_vtx_again / g_frames / 1024.0, (double)g_batched_in / g_frames,
        (double)g_batched_out / g_frames, (double)g_op_bytes[OP_DRAW] / g_frames / 1024.0,
        (double)g_u_bytes / g_frames / 1024.0, (double)g_idx_bytes / g_frames / 1024.0,
        (double)g_op_bytes[OP_BUF_UPLOAD] / g_frames / 1024.0,
        (double)(g_op_bytes[OP_TEX_UPLOAD] + g_op_bytes[OP_TEX_UPLOAD_RECT]) / g_frames / 1024.0);
    g_batched_in = g_batched_out = 0;
    memset(g_op_bytes, 0, sizeof g_op_bytes), g_u_bytes = g_idx_bytes = 0;
    g_vtx_bytes = g_vtx_again = 0;
    g_render_busy_ns = g_wait_pace_ns = g_wait_sync_ns = g_bytes = 0;
    g_syncs = g_frames = 0;
    g_since_ns = now;
}

#if defined(__SWITCH__)
/* FFXI_FPS_ADAPT (on unless 0): uncapped, a frame over 16.7 ms waits for the next vblank, so in a crowd frames took
 * 33 ms, then 50, then 33: 20 to 30 fps, never even. Below 40 fps - the frames of 30 at a time (each counted to 50 ms
 * at most: a zone's loading is one long frame) averaging over 25 ms, twice running - each frame is shown for two
 * vblanks instead (the render thread's OP_PRESENT, on the window): an even 30. Uncapped again is tried after a while
 * at 30, when a frame's work looks like it fits 22 ms (the game's time between presents less its waits for the
 * render thread, and the render thread's running its batches less its presents). That estimate is hopeful at 30 (in
 * the field it read 1 to 2 ms where uncapped ran at 44 to 57 fps), so a try that falls below 40 again within 10 s
 * waits twice as long for the next one (4 s, 8, ... 64); one that holds starts the wait over. */
static void adapt_interval(uint64_t enter, uint64_t leave)
{
    static int on = -1;
    static uint64_t last_enter, last_leave, last_wait, frames, work, rt_last, since, wait_ns = 4000000000ull;
    static uint32_t n, over;
    if (on < 0)
    {
        const char* e = getenv("FFXI_FPS_ADAPT");
        on = !(e && *e == '0');
        rt_last = g_rt_busy_all - g_rt_present_all;
        since = enter;
    }
    if (!on)
        return;
    if (last_enter)
    {
        uint64_t f = enter - last_enter;
        frames += f < 50000000ull ? f : 50000000ull;
        uint64_t w = enter - last_leave, waited = g_game_wait_all - last_wait;
        w = w > waited ? w - waited : 0;
        work += w < 50000000ull ? w : 50000000ull;
    }
    last_enter = enter, last_leave = leave, last_wait = g_game_wait_all;
    if (++n < 30)
        return;
    uint64_t rt_now = g_rt_busy_all - g_rt_present_all, rt = rt_now > rt_last ? rt_now - rt_last : 0; /* (read mid-batch) */
    rt_last = rt_now;
    double fms = (double)frames / n * 1e-6, gms = (double)work / n * 1e-6, rms = (double)rt / n * 1e-6;
    double worse = gms > rms ? gms : rms;
    n = 0, frames = 0, work = 0;
    if (g_interval_want == 1)
    {
        over = fms > 25.0 ? over + 1 : 0;
        if (over >= 2)
        {
            /* a try at uncapped that didn't hold: the next waits twice as long */
            wait_ns = leave - since < 10000000000ull ? (wait_ns < 64000000000ull ? wait_ns * 2 : wait_ns) : 4000000000ull;
            g_interval_want = 2, over = 0, since = leave;
            rt_log("[switch] frame rate: 30 fps (frames of %.1f ms; uncapped again tried in %.0f s at the soonest)\n", fms,
                (double)wait_ns * 1e-9);
        }
    }
    else if (leave - since >= wait_ns && worse < 22.0)
    {
        g_interval_want = 1, over = 0, since = leave;
        rt_log("[switch] frame rate: uncapped again (a frame's work looks like %.1f ms: the game %.1f, the render thread "
               "%.1f)\n", worse, gms, rms);
    }
}
#endif

void gfxq_present(GfxTex* backbuffer)
{
#if defined(__SWITCH__)
    uint64_t enter = gfx_now_ns();
#endif
    *(GfxTex**)rec(OP_PRESENT, sizeof(void*)) = backbuffer;
    flush();
    if (!g_threaded)
        return;
    g_presents_sent++;
    uint64_t t0 = gfx_now_ns();
    int said = 0;
    for (;;)
    {
        uint32_t d = g_presents_done;
        if (g_presents_sent - d <= AHEAD)
            break;
        plat_wait32_ms(&g_presents_done, d, 500);
        if (!said && gfx_now_ns() - t0 > 3000000000ull)
            said = 1, rt_log("[recomp] gfx: Present has waited 3 s for the render thread (head %u tail %u)\n", g_head, g_tail);
    }
    g_wait_pace_ns += gfx_now_ns() - t0;
    g_game_wait_all += gfx_now_ns() - t0;
#if defined(__SWITCH__)
    adapt_interval(enter, gfx_now_ns());
    {
        /* the frame that ended: a long one logged with what it did (thunk.c), and the sampler's frame marks */
        static uint64_t last, rt_was, lock_was;
        uint64_t now = gfx_now_ns(), lock = gt_wait_ns();
        if (last && gfx_profiling)
            thunk_frame_end(sampler_frame, now - last, g_game_wait_all - rt_was, lock - lock_was);
        last = now, rt_was = g_game_wait_all, lock_was = lock;
        sampler_frame++;
        static uint64_t read_said;
        if (gfx_profiling && now - read_said > 10000000000ull)
        {
            char line[640];
            if (read_said && plat_read_report(line, sizeof line))
                rt_log("[switch] %s (10 s)\n", line);
            read_said = now;
        }
    }
#endif
    if (gfx_profiling)
        profile_line();
}

static void finish_call(GfxqCall* c)
{
    gfx_finish();
    gfxq_call_done(c);
}

void gfxq_finish(void)
{
#if defined(GFXQ_OWNED)
    GfxqCall c = { finish_call };
    call_wait(&c);
    return;
#endif
    drain();
    gfx_finish();
}

void gfxq_set_sync_pipelines(int on)
{
    *(int*)rec(OP_SET_SYNC_PIPELINES, sizeof(int)) = on;
    done();
}

static void failures_call(GfxqCall* c)
{
    c->result = (int)gfx_failures();
    gfxq_call_done(c);
}

uint32_t gfxq_failures(void)
{
#if defined(GFXQ_OWNED)
    GfxqCall c = { failures_call };
    call_wait(&c);
    return (uint32_t)c.result;
#endif
    drain();
    return gfx_failures();
}

void gfxq_set_focus(const float* pos)
{
    float* f = (float*)rec(OP_SET_FOCUS, 16);
    f[0] = pos ? pos[0] : 0, f[1] = pos ? pos[1] : 0, f[2] = pos ? pos[2] : 0, f[3] = pos ? 1.0f : 0.0f;
    done();
}

void gfxq_set_moghouse(int in)
{
    *(int*)rec(OP_SET_MOGHOUSE, sizeof(int)) = in;
    done();
}

static void trace_call(GfxqCall* c)
{
    gfx_trace_dump((const char*)c->p[0]);
    gfxq_call_done(c);
}

void gfxq_trace_dump(const char* path)
{
    GfxqCall c = { trace_call, { (void*)path } };
    call_wait(&c);
}
