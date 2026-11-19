/* d3d8's command ring: the D3D calls the game's thread makes, recorded for a d3d8 worker thread that runs them
 * (builds the draws, records them for the render thread: gfx_queue.c), so the game's thread only records.
 * Included once, by d3d8.c.
 *
 * - A record is an op and a body the caller fills (d3dw_rec), then d3dw_done. Everything a record needs is in its
 *   body: the game may change its memory as soon as the call returns.
 * - FFXI_D3D_WORKER=1 runs the records on the worker; otherwise (the default, and wherever there is no worker)
 *   each runs at once, on the calling thread, in d3dw_done: the same code by the same path, without a thread.
 * - d3dw_sync: everything recorded has run (before a call that needs the worker's results or its work done).
 *
 * The worker's side is d3d8.c's d3dw_run(op, body, bytes). Batches of records go over as gfx_queue.c's do: a
 * ring of batch slots one way, the batches back the other for reuse; plat_wait32/plat_wake_all32 to wait. */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "runtime.h"

static void d3dw_run(uint32_t op, const void* body, uint32_t bytes); /* d3d8.c */

typedef struct D3dwRec
{
    uint32_t op, size; /* size: this header and the body, a multiple of 16 */
} D3dwRec;

typedef struct D3dwBatch
{
    uint8_t* p;
    size_t n, cap;
    uint32_t recs;
} D3dwBatch;

#define D3DW_ALIGN16(x) (((x) + 15) & ~(size_t)15)
#define D3DW_FLUSH_RECS 64             /* a batch goes to the worker after this many records */
#define D3DW_FLUSH_BYTES (128u << 10) /* or this many bytes */
#define D3DW_NSLOTS 128u

static struct
{
    int threaded; /* -1: not started */
    D3dwBatch* cur;
    D3dwBatch* volatile slots[D3DW_NSLOTS]; /* game -> worker */
    volatile uint32_t head, tail;
    D3dwBatch* volatile free[D3DW_NSLOTS]; /* worker -> game, for reuse */
    volatile uint32_t fhead, ftail;
    int running; /* inline: a record being run (one that records another is a bug) */
    uint64_t syncs, sync_ns, recs, bytes; /* FFXI_PROFILE */
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
    b->cap = D3DW_FLUSH_BYTES * 2;
    b->p = (uint8_t*)malloc(b->cap);
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
        at += r->size;
    }
}

static void d3dw_worker(void* arg)
{
    (void)arg;
    for (;;)
    {
        uint32_t h = g_w.head;
        if (g_w.tail == h)
        {
            plat_wait32(&g_w.head, h);
            continue;
        }
        D3dwBatch* b = g_w.slots[g_w.tail % D3DW_NSLOTS];
        d3dw_run_batch(b);
        /* back for reuse (the free ring has room: it holds no more batches than were ever handed over) */
        g_w.free[g_w.fhead % D3DW_NSLOTS] = b;
        plat_atomic_add32(&g_w.fhead, 1);
        plat_atomic_add32(&g_w.tail, 1);
        plat_wake_all32(&g_w.tail);
    }
}

static void d3dw_start(void)
{
    if (g_w.threaded >= 0)
        return;
    const char* e = getenv("FFXI_D3D_WORKER");
    g_w.threaded = 0;
    if (e && *e == '1')
    {
        if (plat_thread_start(d3dw_worker, NULL))
            g_w.threaded = 1;
        else
            fprintf(stderr, "[d3d8] FFXI_D3D_WORKER=1: no worker thread; the calls run on the game's thread\n");
    }
    fprintf(stderr, "[d3d8] the D3D calls run %s\n", g_w.threaded ? "on a worker thread (FFXI_D3D_WORKER=1)"
                                                                   : "on the game's thread");
}

static int d3dw_threaded(void)
{
    if (g_w.threaded < 0)
        d3dw_start();
    return g_w.threaded;
}

/* room for a record whose body is `bytes`; returns the body, to be filled before d3dw_done */
static void* d3dw_rec(uint32_t op, uint32_t bytes)
{
    if (g_w.threaded < 0)
        d3dw_start();
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
    r->op = op, r->size = (uint32_t)size;
    b->n += size;
    b->recs++;
    g_w.recs++, g_w.bytes += size;
    return r + 1;
}

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
    plat_wake_all32(&g_w.head);
}

/* after a record's body is filled: inline, it runs now; threaded, the batch may go over */
static void d3dw_done(void)
{
    D3dwBatch* b = g_w.cur;
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

/* everything recorded has run */
static void d3dw_sync(void)
{
    if (!g_w.threaded)
        return;
    d3dw_flush();
    uint64_t t0 = rt_monotonic_ns();
    for (;;)
    {
        uint32_t t = g_w.tail;
        if (t == g_w.head)
            break;
        plat_wait32(&g_w.tail, t);
    }
    g_w.syncs++, g_w.sync_ns += rt_monotonic_ns() - t0;
}
