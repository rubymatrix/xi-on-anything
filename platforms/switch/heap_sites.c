/* Built with XI_SWITCH_HEAP_SITES=1, and on with FFXI_HEAP_SITES=1 (run.args): the C heap's live memory by where it
 * was allocated, to find what keeps growing (it found the pool's idle threads and gfx_queue.c's grown batches).
 * newlib's allocators are wrapped at the link (build_switch.py: --wrap=_malloc_r and the rest; malloc, calloc,
 * realloc, memalign and free all end in them, so libdeko3d's and libstdc++'s allocations are counted too). Each
 * allocation is kept in a table by its address with its size and its site: the three return addresses above the
 * allocator (the caller, and the two calls above it). heap_sites_report logs the sites holding the most, and how
 * much each grew since the last report, every 30 s with FFXI_FRAMELOG (host64.c's heap line). Addresses are the
 * .nro's own (its load address taken off): tools/debug symbolizes them with the .elf. Off, the wrappers only pass
 * the calls on. A test aid: not for a normal build's run.args. */
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

void* __real__malloc_r(struct _reent* r, size_t n);
void* __real__calloc_r(struct _reent* r, size_t a, size_t b);
void* __real__realloc_r(struct _reent* r, void* p, size_t n);
void* __real__memalign_r(struct _reent* r, size_t al, size_t n);
void __real__free_r(struct _reent* r, void* p);
void rt_log(const char* fmt, ...);

#define PTRS (1u << 21) /* the live allocations it can follow (32 MB of table, outside the heap) */
#define SITES 8192u
#define DEPTH 3

typedef struct
{
    uintptr_t p; /* 0 empty, 1 removed */
    uint32_t size;
    uint32_t site;
} Live;

typedef struct
{
    uint32_t ra[DEPTH];
    uint64_t bytes, count, was;
} Site;

static Live g_live[PTRS];
static Site g_sites[SITES];
static uint32_t g_nsites;
static uint64_t g_tracked, g_lost; /* bytes followed; allocations not (the table full) */
static int g_on; /* heap_sites_start: run.args' settings are in the environment only once the host starts */
static volatile uint32_t g_lock;
static __thread int t_in; /* inside an allocator already (newlib's own calls between them aren't counted twice) */
static uintptr_t g_base;

static int on(void) { return __atomic_load_n(&g_on, __ATOMIC_RELAXED); }

/* from the host's start: what was allocated before isn't followed (its frees not found, which is harmless) */
void heap_sites_start(void)
{
    const char* e = getenv("FFXI_HEAP_SITES");
    extern char _start[];
    g_base = (uintptr_t)_start; /* (the .nro's offset 0; __start__ is an absolute 0) */
    if (e && *e == '1')
    {
        __atomic_store_n(&g_on, 1, __ATOMIC_RELEASE);
        rt_log("[heapsites] following the C heap's allocations by their site (FFXI_HEAP_SITES=1)\n");
    }
}

static void lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&g_lock, __ATOMIC_RELAXED))
            ;
}
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

static uint32_t rva(uintptr_t a) { return a >= g_base && a - g_base < 0x80000000u ? (uint32_t)(a - g_base) : 0xFFFFFFFFu; }

/* the caller's site: lr (the allocator's caller: malloc and the rest tail-call these), then the frame records */
static uint32_t site_of(uintptr_t lr, uintptr_t fp)
{
    uint32_t ra[DEPTH] = { rva(lr) };
    for (int i = 1; i < DEPTH; ++i)
    {
        if (!fp || (fp & 15))
            break;
        const uintptr_t* f = (const uintptr_t*)fp;
        uintptr_t next = f[0];
        ra[i] = rva(f[1]);
        if (next <= fp)
            break;
        fp = next;
    }
    uint32_t h = (ra[0] * 2654435761u) ^ (ra[1] * 40503u) ^ ra[2];
    for (uint32_t k = 0; k < SITES; ++k)
    {
        Site* s = &g_sites[(h + k) & (SITES - 1)];
        if (!s->ra[0] && !s->ra[1])
        {
            if (g_nsites >= SITES - 64)
                return 0;
            memcpy(s->ra, ra, sizeof ra), g_nsites++;
            return (uint32_t)(s - g_sites);
        }
        if (!memcmp(s->ra, ra, sizeof ra))
            return (uint32_t)(s - g_sites);
    }
    return 0;
}

static void add(void* p, size_t n, uintptr_t lr, uintptr_t fp)
{
    if (!p)
        return;
    lock();
    uint32_t site = site_of(lr, fp);
    uint32_t h = (uint32_t)(((uintptr_t)p >> 4) * 2654435761u) & (PTRS - 1);
    for (uint32_t k = 0; k < 64; ++k)
    {
        Live* e = &g_live[(h + k) & (PTRS - 1)];
        if (e->p <= 1)
        {
            e->p = (uintptr_t)p, e->size = (uint32_t)n, e->site = site;
            g_sites[site].bytes += n, g_sites[site].count++;
            g_tracked += n;
            unlock();
            return;
        }
    }
    g_lost++;
    unlock();
}

static void drop(void* p)
{
    if (!p)
        return;
    lock();
    uint32_t h = (uint32_t)(((uintptr_t)p >> 4) * 2654435761u) & (PTRS - 1);
    for (uint32_t k = 0; k < 64; ++k)
    {
        Live* e = &g_live[(h + k) & (PTRS - 1)];
        if (e->p == (uintptr_t)p)
        {
            g_sites[e->site].bytes -= e->size, g_sites[e->site].count--;
            g_tracked -= e->size;
            e->p = 1;
            break;
        }
        if (!e->p)
            break;
    }
    unlock();
}

/* (malloc and the rest tail-call these with the caller's x29 and x30 back: x30 is the caller, our frame record's
 * first word its frame) */
#define CALLER (uintptr_t) __builtin_return_address(0), *(const uintptr_t*)__builtin_frame_address(0)

__attribute__((noinline, optimize("no-omit-frame-pointer"))) void* __wrap__malloc_r(struct _reent* r, size_t n)
{
    if (t_in || !on())
        return __real__malloc_r(r, n);
    t_in = 1;
    void* p = __real__malloc_r(r, n);
    t_in = 0;
    add(p, n, CALLER);
    return p;
}

__attribute__((noinline, optimize("no-omit-frame-pointer"))) void* __wrap__calloc_r(struct _reent* r, size_t a, size_t b)
{
    if (t_in || !on())
        return __real__calloc_r(r, a, b);
    t_in = 1;
    void* p = __real__calloc_r(r, a, b);
    t_in = 0;
    add(p, a * b, CALLER);
    return p;
}

__attribute__((noinline, optimize("no-omit-frame-pointer"))) void* __wrap__memalign_r(struct _reent* r, size_t al, size_t n)
{
    if (t_in || !on())
        return __real__memalign_r(r, al, n);
    t_in = 1;
    void* p = __real__memalign_r(r, al, n);
    t_in = 0;
    add(p, n, CALLER);
    return p;
}

__attribute__((noinline, optimize("no-omit-frame-pointer"))) void* __wrap__realloc_r(struct _reent* r, void* old, size_t n)
{
    if (t_in || !on())
        return __real__realloc_r(r, old, n);
    t_in = 1;
    void* p = __real__realloc_r(r, old, n);
    t_in = 0;
    if (p || !n)
        drop(old);
    add(p, n, CALLER);
    return p;
}

void __wrap__free_r(struct _reent* r, void* p)
{
    if (!t_in && on())
        drop(p);
    __real__free_r(r, p);
}

/* the sites holding the most, and their growth since the last report (from the game's thread: its stdio) */
void heap_sites_report(void)
{
    if (!on())
        return;
    enum { TOP = 14 };
    uint32_t top[TOP] = { 0 }, grow[TOP] = { 0 };
    int nt = 0, ng = 0;
    lock();
    for (uint32_t i = 0; i < SITES; ++i)
    {
        const Site* s = &g_sites[i];
        if (!s->count && !s->was)
            continue;
        /* (insertion into two small sorted lists: by bytes, by growth) */
        int at = nt < TOP ? nt++ : TOP;
        while (at > 0 && g_sites[top[at - 1]].bytes < s->bytes)
        {
            if (at < TOP)
                top[at] = top[at - 1];
            --at;
        }
        if (at < TOP)
            top[at] = i;
        int64_t d = (int64_t)(s->bytes - s->was);
        at = ng < TOP ? ng++ : TOP;
        while (at > 0 && (int64_t)(g_sites[grow[at - 1]].bytes - g_sites[grow[at - 1]].was) < d)
        {
            if (at < TOP)
                grow[at] = grow[at - 1];
            --at;
        }
        if (at < TOP)
            grow[at] = i;
    }
    Site snap[2 * TOP];
    for (int i = 0; i < nt; ++i)
        snap[i] = g_sites[top[i]];
    for (int i = 0; i < ng; ++i)
        snap[TOP + i] = g_sites[grow[i]];
    for (uint32_t i = 0; i < SITES; ++i)
        g_sites[i].was = g_sites[i].bytes;
    uint64_t tracked = g_tracked, lost = g_lost;
    uint32_t nsites = g_nsites;
    unlock();
    rt_log("[heapsites] %.1f MB followed in %u sites (%llu not followed)\n", tracked / 1048576.0, nsites,
        (unsigned long long)lost);
    for (int i = 0; i < nt; ++i)
        rt_log("[heapsites] top %2d: %8.2f MB in %7llu, +%.2f MB: %08x < %08x < %08x\n", i, snap[i].bytes / 1048576.0,
            (unsigned long long)snap[i].count, (double)(int64_t)(snap[i].bytes - snap[i].was) / 1048576.0, snap[i].ra[0],
            snap[i].ra[1], snap[i].ra[2]);
    for (int i = 0; i < ng; ++i)
    {
        const Site* s = &snap[TOP + i];
        if ((int64_t)(s->bytes - s->was) <= 0)
            break;
        rt_log("[heapsites] grew %2d: +%.2f MB, now %.2f MB in %llu: %08x < %08x < %08x\n", i,
            (double)(int64_t)(s->bytes - s->was) / 1048576.0, s->bytes / 1048576.0, (unsigned long long)s->count, s->ra[0],
            s->ra[1], s->ra[2]);
    }
}
