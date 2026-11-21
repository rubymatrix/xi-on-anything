/* d3d8's command ring: the D3D calls the game's thread makes, recorded for a d3d8 worker thread that runs them
 * (builds the draws, records them for the render thread: gfx_queue.c), so the game's thread only records.
 * Included once, by d3d8.c.
 *
 * FFXI_D3D_WORKER:
 *   0 (the default)  every call runs as it always has: no records;
 *   inline           the calls d3d8.c defers are recorded and each runs at once, on the calling thread - the
 *                    worker's path without the thread (a test of the records alone);
 *   1                they run on a worker thread; any other d3d8 call waits for it first (thunk_pre_rule).
 *
 * - A record is an op and a body the caller fills (d3dw_rec), then d3dw_done. Everything a record needs is in its
 *   body or in the guest ring (d3dw_guest): the game may change its memory as soon as the call returns.
 * - The guest ring is guest memory, so a deferred call's arguments and what they point at, copied there, are read
 *   by the shim's own code (ARG, ARGP, GUEST_PTR) when the worker runs it. A record says how far into the ring it
 *   reaches (gend): once it has run, the ring is free up to there.
 * - d3dw_sync: everything recorded has run, and the worker is idle (before a call that needs its results, its work
 *   done, or the guest heap).
 *
 * Batches of records go over as gfx_queue.c's do: a ring of batch slots one way, the batches back the other for
 * reuse; plat_wait32/plat_wake_all32 to wait. The worker's side is d3d8.c's d3dw_run(op, body, bytes). */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gwin.h"
#include "plat.h"
#include "runtime.h"

static void d3dw_run(uint32_t op, const void* body, uint32_t bytes); /* d3d8.c */
static void d3dw_after_sync(void);                                  /* d3d8.c: the worker idle, on the game's thread */

typedef struct D3dwRec
{
    uint32_t op, size; /* size: this header and the body, a multiple of 16 */
    uint32_t gend, pad; /* the guest ring's position after this record's part of it; 0 none */
} D3dwRec;

typedef struct D3dwBatch
{
    uint8_t* p;
    size_t n, cap;
    uint32_t recs;
} D3dwBatch;

#define D3DW_ALIGN16(x) (((x) + 15) & ~(size_t)15)
#define D3DW_FLUSH_RECS 256            /* a batch goes to the worker after this many records (64 woke it ~260 times
                                        * a frame in a town: 0.4 ms of the game's thread in the wake calls) */
#define D3DW_FLUSH_BYTES (128u << 10) /* or this many bytes */
#define D3DW_NSLOTS 128u
#define D3DW_GUEST (16u << 20) /* the guest ring (a power of two) */

static struct
{
    int mode; /* -1 not read, 0 off, 1 inline, 2 threaded */
    int threaded;
    uint32_t front_tid, worker_tid;
    D3dwBatch* cur;
    D3dwBatch* volatile slots[D3DW_NSLOTS]; /* game -> worker */
    volatile uint32_t head, tail;
    volatile uint32_t asleep; /* the worker waits for `head` (d3dw_flush wakes it only then) */
    D3dwBatch* volatile free[D3DW_NSLOTS]; /* worker -> game, for reuse */
    volatile uint32_t fhead, ftail;
    uint32_t gbase, gpos;     /* the guest ring and the game's position in it (bytes, all along) */
    volatile uint32_t gcons;  /* ... and how far the worker has run through it */
    uint32_t gend_pending;    /* the record being filled: its end in the guest ring */
    size_t last;              /* ... and where it is in the batch */
    int running;              /* inline: a record being run (one that records another is a bug) */
    uint32_t scratch;                  /* the worker's argument frame for the short records (W_FAST), guest memory */
    uint32_t presents_sent;            /* the game's thread: the Presents handed to the worker */
    volatile uint32_t presents_done;   /* ... and the ones it has run (the game stays at most one frame ahead) */
    uint64_t present_waits, present_wait_ns; /* FFXI_PROFILE: the game's thread waiting for the frame before */
    uint64_t syncs, waits, wait_ns, recs, bytes; /* FFXI_PROFILE */
} g_w = { -1 };

static D3dwBatch* d3dw_batch_get(void)
{
    if (g_w.ftail != g_w.fhead)
    {
        D3dwBatch* b = g_w.free[g_w.ftail % D3DW_NSLOTS];
        plat_atomic_add32(&g_w.ftail, 1);
        b->n = 0, b->recs = 0;
        return b;
    }
    D3dwBatch* b = (D3dwBatch*)calloc(1, sizeof *b);
    if (b)
    {
        b->cap = D3DW_FLUSH_BYTES * 2;
        b->p = (uint8_t*)malloc(b->cap);
    }
    if (!b || !b->p)
    {
        fprintf(stderr, "d3d8 worker: out of memory for a batch\n");
        abort();
    }
    return b;
}

static void d3dw_run_batch(D3dwBatch* b)
{
    for (size_t at = 0; at < b->n;)
    {
        const D3dwRec* r = (const D3dwRec*)(b->p + at);
        d3dw_run(r->op, r + 1, r->size - (uint32_t)sizeof *r);
        if (r->gend)
        {
            /* (an atomic: this record's reads of the ring are done before the game's thread may reuse it) */
            plat_atomic_add32(&g_w.gcons, r->gend - g_w.gcons);
            plat_wake_all32(&g_w.gcons);
        }
        at += r->size;
    }
}

static void d3dw_worker(void* arg)
{
    (void)arg;
    g_w.worker_tid = plat_thread_id();
    for (;;)
    {
        uint32_t h = g_w.head;
        if (g_w.tail == h)
        {
            /* (asleep first, then head read again: d3dw_flush moves head, then reads asleep - one of the two sees
             * the other's write, so a batch handed over is never left without a wake) */
            __atomic_store_n(&g_w.asleep, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&g_w.head, __ATOMIC_SEQ_CST) == h)
                plat_wait32(&g_w.head, h);
            __atomic_store_n(&g_w.asleep, 0, __ATOMIC_SEQ_CST);
            continue;
        }
        D3dwBatch* b = g_w.slots[g_w.tail % D3DW_NSLOTS];
        d3dw_run_batch(b);
        /* back for reuse, if the free ring has room: there can be more batches than it holds (all the slots full,
         * one being filled, more made while the worker fell behind) - one put in a full ring overwrote one not yet
         * taken, and the game's thread was then handed the same batch twice (records lost or garbled) */
        if (g_w.fhead - g_w.ftail < D3DW_NSLOTS)
        {
            g_w.free[g_w.fhead % D3DW_NSLOTS] = b;
            plat_atomic_add32(&g_w.fhead, 1);
        }
        else
        {
            free(b->p);
            free(b);
        }
        plat_atomic_add32(&g_w.tail, 1);
        plat_wake_all32(&g_w.tail);
    }
}

/* rt_log's lock with the worker on (it logs too): a waiting one (the worker runs a step below the game's threads, so
 * one of them spinning on its core would never let it finish) */
static volatile uint32_t g_w_log;
static void d3dw_log_lock(void)
{
    while (plat_atomic_cas32(&g_w_log, 0, 1) != 0)
        plat_wait32(&g_w_log, 1);
}
static void d3dw_log_unlock(void)
{
    plat_atomic_cas32(&g_w_log, 1, 0);
    plat_wake_all32(&g_w_log);
}

/* the mode, read at the first d3d8 call (on the game's thread); `can` 0 keeps it off (d3d8.c: what the worker can't
 * do yet is in use) */
static void d3dw_start(int can)
{
    if (g_w.mode >= 0)
        return;
    const char* e = getenv("FFXI_D3D_WORKER");
    g_w.mode = !e || !*e || *e == '0' ? 0 : !strcmp(e, "inline") ? 1 : 2;
    g_w.front_tid = plat_thread_id();
    if (g_w.mode && !can)
    {
        fprintf(stderr, "[d3d8] FFXI_D3D_WORKER: not with the scene renderer on; the calls run as they always have\n");
        g_w.mode = 0;
    }
    if (g_w.mode)
    {
        g_w.gbase = gheap_alloc(D3DW_GUEST, 0);
        uint32_t sc = gheap_alloc(4096 + 64, 0);
        g_w.scratch = sc ? sc + 16 : 0; /* (room below it: its esp is 4 under) */
        if (!g_w.gbase || !g_w.scratch)
        {
            fprintf(stderr, "[d3d8] FFXI_D3D_WORKER: no guest memory for its ring; the calls run as they always have\n");
            g_w.mode = 0;
        }
    }
    if (g_w.mode == 2)
    {
        /* a core of its own (the Switch: core 2 - core 0 is the game's, core 1 the render thread's, and a thread there
         * runs until it waits, so one sharing the render thread's core took turns with it); FFXI_D3D_WORKER_CORE */
        const char* ce = getenv("FFXI_D3D_WORKER_CORE");
        if (plat_thread_start_core(d3dw_worker, NULL, ce && *ce ? atoi(ce) : 2))
            g_w.threaded = 1, rt_set_log_lock(d3dw_log_lock, d3dw_log_unlock);
        else
        {
            fprintf(stderr, "[d3d8] FFXI_D3D_WORKER=1: no worker thread; the calls are recorded and run at once\n");
            g_w.mode = 1;
        }
    }
    fprintf(stderr, "[d3d8] the D3D calls %s\n", g_w.mode == 2 ? "run on a worker thread (FFXI_D3D_WORKER=1)"
                                                 : g_w.mode == 1 ? "are recorded and run at once (FFXI_D3D_WORKER=inline)"
                                                                 : "run as they always have");
}

static int d3dw_on_worker(void) { return g_w.threaded && plat_thread_id() == g_w.worker_tid; }

/* the recorded batch to the worker (threaded) */
static void d3dw_flush(void)
{
    D3dwBatch* b = g_w.cur;
    if (!g_w.threaded || !b || !b->recs)
        return;
    g_w.cur = NULL;
    for (;;)
    {
        uint32_t t = g_w.tail;
        if (g_w.head - t < D3DW_NSLOTS)
            break;
        plat_wait32(&g_w.tail, t);
    }
    g_w.slots[g_w.head % D3DW_NSLOTS] = b;
    plat_atomic_add32(&g_w.head, 1);
    if (__atomic_load_n(&g_w.asleep, __ATOMIC_SEQ_CST))
        plat_wake_all32(&g_w.head);
}

/* n bytes of the guest ring, in one piece, for the record about to be made: its guest address */
static uint32_t d3dw_guest(uint32_t n)
{
    n = (uint32_t)D3DW_ALIGN16(n);
    uint32_t off = g_w.gpos % D3DW_GUEST, skip = off + n > D3DW_GUEST ? D3DW_GUEST - off : 0;
    if (n > D3DW_GUEST / 4)
    {
        fprintf(stderr, "d3d8 worker: a %u-byte call\n", n);
        abort();
    }
    for (;;)
    {
        uint32_t c = g_w.gcons;
        if (g_w.gpos + skip + n - c <= D3DW_GUEST)
            break;
        d3dw_flush(); /* (what it waits for may not have gone over yet) */
        plat_wait32(&g_w.gcons, c);
    }
    g_w.gpos += skip;
    uint32_t a = g_w.gbase + g_w.gpos % D3DW_GUEST;
    g_w.gpos += n;
    g_w.gend_pending = g_w.gpos;
    return a;
}

/* room for a record whose body is `bytes`; returns the body, to be filled before d3dw_done */
static void* d3dw_rec(uint32_t op, uint32_t bytes)
{
    if (g_w.running)
    {
        fprintf(stderr, "d3d8 worker: a record recorded while one runs (op %u)\n", op);
        abort();
    }
    if (!g_w.cur)
        g_w.cur = d3dw_batch_get();
    D3dwBatch* b = g_w.cur;
    size_t size = D3DW_ALIGN16(sizeof(D3dwRec) + bytes);
    if (b->n + size > b->cap)
    {
        while (b->n + size > b->cap)
            b->cap *= 2;
        uint8_t* p = (uint8_t*)realloc(b->p, b->cap);
        if (!p)
        {
            fprintf(stderr, "d3d8 worker: out of memory for a %zu-byte batch\n", b->cap);
            abort();
        }
        b->p = p;
    }
    D3dwRec* r = (D3dwRec*)(b->p + b->n);
    r->op = op, r->size = (uint32_t)size, r->gend = 0, r->pad = 0;
    g_w.last = b->n;
    b->n += size;
    b->recs++;
    g_w.recs++, g_w.bytes += size;
    return r + 1;
}

/* after a record's body is filled: inline, it runs now; threaded, the batch may go over */
static void d3dw_done(void)
{
    D3dwBatch* b = g_w.cur;
    if (g_w.gend_pending)
        ((D3dwRec*)(b->p + g_w.last))->gend = g_w.gend_pending, g_w.gend_pending = 0;
    if (!g_w.threaded)
    {
        g_w.running = 1;
        d3dw_run_batch(b);
        g_w.running = 0;
        b->n = 0, b->recs = 0;
        return;
    }
    if (b->recs >= D3DW_FLUSH_RECS || b->n >= D3DW_FLUSH_BYTES)
        d3dw_flush();
}

/* everything recorded has run and the worker is idle; returns 1 if that took a wait */
static int d3dw_sync(void)
{
    /* (any thread but the worker's: the game makes D3D calls from its loader threads too - one at a time, under the
     * guest lock - and theirs need the worker idle as the main thread's do) */
    if (!g_w.threaded || d3dw_on_worker())
        return 0;
    g_w.syncs++;
    d3dw_flush();
    int waited = 0;
    uint64_t t0 = 0;
    for (;;)
    {
        uint32_t t = g_w.tail;
        if (t == g_w.head)
            break;
        if (!waited)
            waited = 1, t0 = rt_monotonic_ns();
        plat_wait32(&g_w.tail, t);
    }
    if (waited)
        g_w.waits++, g_w.wait_ns += rt_monotonic_ns() - t0;
    d3dw_after_sync();
    return waited;
}
