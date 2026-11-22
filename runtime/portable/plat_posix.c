/* The platform layer on POSIX hosts (R3.1): arm64 macOS first, Linux as a by-product. See plat.h.
 *
 * Waiting on an address: macOS 14.4+ has os_sync_wait_on_address (the supported form of the
 * __ulock calls libc++ uses); Linux has futex. Address space: one PROT_NONE reservation; commit
 * is mprotect, decommit maps fresh zero pages over the range, as Win32's decommit + commit gives
 * zeroed memory back.
 *
 * The browser (Emscripten, wasm32): no address space to reserve. Guest addresses are linear-memory
 * addresses (guest.h without RT_GUEST_WINDOW; gwin.c keeps the host's region below the images), so
 * commit grows the memory to cover the range and decommit zeroes it (wasm memory never shrinks).
 * Waiting is Emscripten's futex (Atomics.wait).
 *
 * Nintendo Switch (libnx, newlib): no mmap and no overcommit. The window is a stretch of the alias
 * region, which libnx leaves alone; commit backs its pages with physical memory there
 * (svcMapPhysicalMemory), decommit hands them back, and the next commit gets fresh zeroed pages.
 * That memory is whatever the C heap leaves, so the heap is capped (__nx_heap_size below). Waiting is
 * the kernel's address arbiter (svcWaitForAddress / svcSignalToAddress). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__SWITCH__)
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#include <sys/sysctl.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/threading.h>
#include "httpfs_web.h"
#elif defined(__SWITCH__)
#include <switch.h>
#else
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#endif

#include "plat.h"
#include "runtime.h"

const char plat_path_sep = '/';

/* The browser: the game install and host64's files are the local server's (httpfs_web.c), read only;
 * everything else (the data folder) is Emscripten's file system. */
#if defined(__EMSCRIPTEN__)
#define HTTP(path) httpfs_owns(path)
#else
#define HTTP(path) 0
typedef struct HttpFile HttpFile;
typedef struct HttpDir HttpDir;
#endif

#if defined(__EMSCRIPTEN__)
void* plat_reserve(size_t size)
{
    (void)size;
    return NULL; /* gwin.c's flat mode never reserves */
}

int plat_commit(void* p, size_t size)
{
    size_t end = (size_t)(uintptr_t)p + size;
    return end <= emscripten_get_heap_size() || emscripten_resize_heap(end);
}

void plat_decommit(void* p, size_t size)
{
    memset(p, 0, size);
}

/* The host's malloc (sbrk) may not grow into the guest's part of memory: linked with
 * -Wl,--wrap=sbrk, every sbrk comes here first. */
void* __real_sbrk(intptr_t increment);
void* __wrap_sbrk(intptr_t increment)
{
    uintptr_t now = (uintptr_t)__real_sbrk(0);
    if (increment > 0 && now + (uintptr_t)increment > PLAT_HOST_TOP)
    {
        errno = ENOMEM;
        return (void*)-1;
    }
    return __real_sbrk(increment);
}
#elif defined(__SWITCH__)
/* Two ways to give the guest its window, chosen at start-up:
 *
 * Physical (the process has a system resource for its page tables, which svcMapPhysicalMemory needs: an
 * application whose NPDM asks for one, as hbloader's does): the C heap is capped at
 * FFXI_SWITCH_HEAP_MB (malloc, the host's buffers, the GPU driver's memory); the window is a stretch of
 * the alias region, which libnx leaves alone; commit maps physical memory there (only the pages not
 * mapped yet, so a partly committed range keeps its contents), decommit unmaps it, and the next commit
 * gets fresh zeroed pages. The guest has what it commits and no more.
 *
 * Heap (no system resource: an applet, or an emulator running a bare .nro): the heap takes all of memory,
 * as libnx's default does, and the window is a block of it, always backed: commit has nothing to do and
 * decommit zeroes the range.
 *
 * libnx's __libnx_initheap is replaced to make that choice. hbloader hands its heap over already sized to
 * all of memory, which is shrunk to the cap here and grown back at exit, so the next homebrew it starts
 * finds the heap it was promised. */
#ifndef FFXI_SWITCH_HEAP_MB
#define FFXI_SWITCH_HEAP_MB 1024
#endif
#define SWITCH_HEAP_CAP ((size_t)FFXI_SWITCH_HEAP_MB << 20)

static int g_physical;          /* commits are svcMapPhysicalMemory */
static size_t g_heap_given;     /* hbloader's heap size, when it was shrunk */
static void* g_window;          /* the reservation (physical mode) */
static size_t g_window_size;

extern char* fake_heap_start;
extern char* fake_heap_end;

void __libnx_initheap(void)
{
    uint64_t sysres = 0, total = 0, used = 0;
    svcGetInfo(&sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
    g_physical = sysres != 0;
    void* addr = NULL;
    size_t size = 0;
    if (envHasHeapOverride())
    {
        addr = envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
        if (g_physical && size > SWITCH_HEAP_CAP && R_SUCCEEDED(svcSetHeapSize(&addr, SWITCH_HEAP_CAP)))
        {
            g_heap_given = size;
            size = SWITCH_HEAP_CAP;
        }
    }
    else
    {
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        if (total > used + 0x200000)
            size = (size_t)(total - used - 0x200000) & ~(size_t)0x1FFFFF;
        if (g_physical && size > SWITCH_HEAP_CAP)
            size = SWITCH_HEAP_CAP;
        if (!size)
            size = 0x2000000 * 16;
        if (R_FAILED(svcSetHeapSize(&addr, size)))
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
    fake_heap_start = (char*)addr;
    fake_heap_end = (char*)addr + size;
}

/* at exit: the guest's physical memory back, then hbloader's heap at the size it handed over */
__attribute__((destructor)) static void switch_memory_exit(void)
{
    if (g_physical && g_window)
        svcUnmapPhysicalMemory(g_window, g_window_size);
    if (g_heap_given)
    {
        void* addr = NULL;
        svcSetHeapSize(&addr, g_heap_given);
    }
}

int plat_physical_memory(void)
{
    return g_physical;
}

void* plat_reserve(size_t size)
{
    size = (size + 0x1FFFFF) & ~(size_t)0x1FFFFF;
    if (!g_physical)
    {
        /* the top of the heap, before malloc reaches it (newlib's malloc takes no 2 GB block): the heap's
         * end comes down below it, so sbrk never hands it out */
        uintptr_t end = (uintptr_t)fake_heap_end & ~(uintptr_t)0x1FFFFF;
        uintptr_t brk = (uintptr_t)sbrk(0);
        if (end < size || end - size < brk + (64u << 20)) /* keep the C heap at least 64 MB more */
            return NULL;
        char* p = (char*)(end - size);
        fake_heap_end = p;
        memset(p, 0, size); /* every commit is zero until written */
        return p;
    }
    static uint64_t next; /* the first address of the alias region not handed out yet */
    uint64_t base = 0, len = 0;
    if (R_FAILED(svcGetInfo(&base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
        R_FAILED(svcGetInfo(&len, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0)))
        return NULL;
    void* p = NULL;
    virtmemLock();
    if (!next)
        next = base;
    uint64_t at = (next + 0x1FFFFF) & ~(uint64_t)0x1FFFFF;
    if (at + size <= base + len)
    {
        virtmemAddReservation((void*)(uintptr_t)at, size); /* nothing else libnx finds lands here */
        next = at + size;
        p = (void*)(uintptr_t)at;
        g_window = p;
        g_window_size = size;
    }
    virtmemUnlock();
    return p;
}

int plat_commit(void* p, size_t size)
{
    if (!g_physical)
        return 1;
    Result rc = svcMapPhysicalMemory(p, size);
    if (R_FAILED(rc))
    {
        char line[128];
        snprintf(line, sizeof line, "plat_commit: svcMapPhysicalMemory(%p, 0x%zx) failed: 0x%x\n", p, size, rc);
        plat_debug(line);
        return 0;
    }
    return 1;
}

void plat_decommit(void* p, size_t size)
{
    if (g_physical)
        svcUnmapPhysicalMemory(p, size);
    else
        memset(p, 0, size);
}
#else
void* plat_reserve(size_t size)
{
    void* p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int plat_commit(void* p, size_t size)
{
    return mprotect(p, size, PROT_READ | PROT_WRITE) == 0;
}

void plat_decommit(void* p, size_t size)
{
    mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
}
#endif

/* --- threads ------------------------------------------------------------------------------------- */
uint32_t plat_thread_id(void)
{
#if defined(__APPLE__)
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (uint32_t)id;
#elif defined(__EMSCRIPTEN__)
    return (uint32_t)(uintptr_t)pthread_self();
#elif defined(__SWITCH__)
    static RT_TLS uint32_t t_id;
    if (!t_id)
    {
        uint64_t id = 0;
        svcGetThreadId(&id, CUR_THREAD_HANDLE);
        t_id = (uint32_t)id;
    }
    return t_id;
#else
    /* once per thread: gettid is a system call, and the critical sections (kobj.c) and the profile's
     * per-shim timing (thunk.c) ask on every call - with FFXI_PROFILE, about 1.3 million times a second
     * in a crowd */
    static RT_TLS uint32_t t_id;
    if (!t_id)
        t_id = (uint32_t)syscall(SYS_gettid);
    return t_id;
#endif
}

typedef struct ThreadStart
{
    void (*fn)(void*);
    void* arg;
} ThreadStart;

static void* thread_entry(void* p)
{
    ThreadStart s = *(ThreadStart*)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

#if defined(__SWITCH__)
/* Threads that have finished their job wait here for the next one. FFXiMain starts a thread for every
 * sound it plays (a hit, a footstep), so a thread per start that ends and is dropped would run out: a
 * Horizon thread holds its 8 MB stack and its place in the process's thread limit until its handle is
 * closed and the thread joined, and nobody joins a detached one. On the console 73 sounds in, CreateThread
 * failed and every sound after it was lost. */
typedef struct NxWorker
{
    Thread t;
    struct NxWorker* next;
    void (*fn)(void*);
    void* arg;
    s32 prio;
    int core;
    CondVar cv;
    volatile uint32_t started;
} NxWorker;
static Mutex g_pool_lock;
static NxWorker* g_idle;
static uint32_t g_workers, g_idle_n;

/* the jobs the threads were given, in order (g_pool_lock): plat_thread_list, for the main thread to print */
#define NX_TLOG 48
static struct
{
    void (*fn)(void*);
    int core, prio;
} g_tlog[NX_TLOG];
static uint32_t g_tlog_n;
static void tlog(void (*fn)(void*), int core, int prio)
{
    if (g_tlog_n < NX_TLOG)
        g_tlog[g_tlog_n].fn = fn, g_tlog[g_tlog_n].core = core, g_tlog[g_tlog_n].prio = prio;
    __atomic_store_n(&g_tlog_n, g_tlog_n + 1, __ATOMIC_RELEASE);
}
uint32_t plat_thread_list(char* out, size_t n)
{
    extern void _start(void); /* the .nro's base: the jobs as offsets, for aarch64-none-elf-nm of the .elf */
    mutexLock(&g_pool_lock);
    uint32_t count = g_tlog_n;
    size_t at = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < count && i < NX_TLOG && at + 1 < n; ++i)
    {
        int w = snprintf(out + at, n - at, "%s0x%lx core %d prio 0x%x", i ? "; " : "",
            (unsigned long)((uintptr_t)g_tlog[i].fn - (uintptr_t)_start), g_tlog[i].core, g_tlog[i].prio);
        if (w < 0)
            break;
        at += (size_t)w;
    }
    mutexUnlock(&g_pool_lock);
    return count;
}

static void worker_main(void* p)
{
    NxWorker* w = (NxWorker*)p;
    __atomic_store_n(&w->started, 1, __ATOMIC_SEQ_CST);
    svcSignalToAddress((void*)&w->started, SignalType_Signal, 0, -1);
    for (;;)
    {
        w->fn(w->arg);
        svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)w->prio); /* a job may have raised it (sdl_nx's audio) */
        mutexLock(&g_pool_lock);
        w->fn = NULL;
        w->next = g_idle;
        g_idle = w;
        g_idle_n++;
        while (!w->fn)
            condvarWait(&w->cv, &g_pool_lock);
        mutexUnlock(&g_pool_lock);
    }
}

void plat_thread_counts(uint32_t* made, uint32_t* idle)
{
    mutexLock(&g_pool_lock);
    *made = g_workers, *idle = g_idle_n;
    mutexUnlock(&g_pool_lock);
}
#endif

#if defined(__SWITCH__)
static int nx_thread_new(void (*fn)(void*), void* arg, int core, int pooled);
#endif
static int plat_thread_start_posix(void (*fn)(void*), void* arg);

/* a thread that never ends, on a core of its own choosing where the platform lets it (the Switch: 0-2) */
int plat_thread_start_core(void (*fn)(void*), void* arg, int core)
{
#if defined(__SWITCH__)
    mutexLock(&g_pool_lock);
    return nx_thread_new(fn, arg, core, 0);
#else
    (void)core;
    return plat_thread_start(fn, arg);
#endif
}

int plat_thread_start(void (*fn)(void*), void* arg)
{
#if defined(__SWITCH__)
    mutexLock(&g_pool_lock);
    NxWorker* w = g_idle;
    if (w)
    {
        g_idle = w->next;
        g_idle_n--;
        w->fn = fn, w->arg = arg;
        tlog(fn, w->core, w->prio);
        condvarWakeOne(&w->cv);
        mutexUnlock(&g_pool_lock);
        return 1;
    }
    return nx_thread_new(fn, arg, -1, 1);
#else
    return plat_thread_start_posix(fn, arg);
#endif
}

#if defined(__SWITCH__)
/* (called with g_pool_lock held; unlocks it) core -1: the pool's turn */
static int nx_thread_new(void (*fn)(void*), void* arg, int core_want, int pooled)
{
    NxWorker* w;
    /* Horizon doesn't share a core between threads of one priority: a thread runs until it waits. The
     * application's main thread (the game's) has core 0; the threads made here take cores 1 and 2 in
     * turn - the first, the render thread (gfx_queue.c), core 1 - at the main thread's priority. libnx's
     * pthread_create can't choose a core. A reused thread keeps its core. */
    int core = core_want >= 0 ? core_want : 1 + (int)(g_workers % 2);
    s32 prio = 0x2C;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* one of its own core's (plat_thread_start_core: the d3d8 worker) a step below the game's threads: on a core it
     * shares with them it would otherwise run until it waits, while one of them with a ticket for the guest lock
     * couldn't run - and the game's main thread waited behind that ticket (8% of its time) */
    if (!pooled && prio < 0x3F)
        prio += 1;
    /* the stack page-aligned and whole pages (libnx carves the TLS and reent out of its top) */
    const size_t stack_sz = 8u << 20; /* translated frames are large */
    void* stack = aligned_alloc(0x1000, stack_sz);
    w = (NxWorker*)calloc(1, sizeof *w);
    if (!stack || !w)
    {
        free(stack);
        free(w);
        mutexUnlock(&g_pool_lock);
        return 0;
    }
    w->fn = fn, w->arg = arg, w->prio = prio, w->core = core;
    condvarInit(&w->cv);
    if (R_FAILED(threadCreate(&w->t, worker_main, w, stack, stack_sz, prio, core)) || R_FAILED(threadStart(&w->t)))
    {
        free(stack);
        free(w);
        mutexUnlock(&g_pool_lock);
        return 0;
    }
    if (pooled)
        g_workers++;
    tlog(fn, core, prio);
    mutexUnlock(&g_pool_lock);
    /* until it has started (as libnx's pthread_create waits) */
    while (!__atomic_load_n(&w->started, __ATOMIC_SEQ_CST))
        svcWaitForAddress((void*)&w->started, ArbitrationType_WaitIfEqual, 0, 1000000000ll);
    return 1; /* its Thread and stack are kept for the next start */
}
#endif

static int plat_thread_start_posix(void (*fn)(void*), void* arg)
{
    ThreadStart* s = (ThreadStart*)malloc(sizeof *s);
    if (!s)
        return 0;
    s->fn = fn;
    s->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
#if defined(__EMSCRIPTEN__)
    /* only the C shadow stack (locals whose address is taken): wasm keeps its own call stack, and
     * gate 0 measured 20 KB of native stack at most */
    pthread_attr_setstacksize(&a, 1u << 20);
#else
    pthread_attr_setstacksize(&a, 8u << 20); /* translated frames are large; Windows gives 1 MB+ and grows */
#endif
    pthread_t t;
    int r = pthread_create(&t, &a, thread_entry, s);
    pthread_attr_destroy(&a);
    if (r)
    {
        free(s);
        return 0;
    }
    return 1;
}

void plat_sleep_us(uint32_t us)
{
    struct timespec t = { (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
    {
    }
}

void plat_sleep_ms(uint32_t ms)
{
    if (ms == ~0u)
    {
        for (;;)
#if defined(__SWITCH__)
            svcSleepThread(INT64_MAX);
#else
            pause();
#endif
    }
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
    {
    }
}

void plat_yield(void)
{
    sched_yield();
}

/* --- waiting on an address ------------------------------------------------------------------------ */
void plat_wait32(volatile uint32_t* addr, uint32_t expected)
{
#if defined(__APPLE__)
    os_sync_wait_on_address((void*)addr, expected, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
#elif defined(__EMSCRIPTEN__)
    emscripten_futex_wait(addr, expected, INFINITY);
#elif defined(__SWITCH__)
    svcWaitForAddress((void*)addr, ArbitrationType_WaitIfEqual, (int32_t)expected, -1);
#else
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
#endif
}

void plat_wait32_ms(volatile uint32_t* addr, uint32_t expected, uint32_t ms)
{
    if (ms == ~0u)
    {
        plat_wait32(addr, expected);
        return;
    }
#if defined(__APPLE__)
    os_sync_wait_on_address_with_timeout((void*)addr, expected, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME,
        (uint64_t)ms * 1000000u);
#elif defined(__EMSCRIPTEN__)
    emscripten_futex_wait(addr, expected, (double)ms);
#elif defined(__SWITCH__)
    svcWaitForAddress((void*)addr, ArbitrationType_WaitIfEqual, (int32_t)expected, (int64_t)ms * 1000000);
#else
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, &t, NULL, 0);
#endif
}

void plat_wake_all32(volatile uint32_t* addr)
{
#if defined(__APPLE__)
    os_sync_wake_by_address_all((void*)addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
#elif defined(__EMSCRIPTEN__)
    emscripten_futex_wake(addr, 0x7FFFFFFF);
#elif defined(__SWITCH__)
    svcSignalToAddress((void*)addr, SignalType_Signal, 0, -1); /* -1: every waiter */
#else
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, NULL, NULL, 0);
#endif
}

/* --- atomics and time ------------------------------------------------------------------------------- */
uint32_t plat_atomic_add32(volatile uint32_t* p, uint32_t v)
{
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

uint32_t plat_atomic_cas32(volatile uint32_t* p, uint32_t expected, uint32_t desired)
{
    __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected; /* the old value either way */
}

uint64_t rt_monotonic_ns(void)
{
#if defined(__EMSCRIPTEN__)
    /* performance.now() straight: Emscripten's clock_gettime goes through JS with 64-bit (BigInt)
     * arguments and was most of the game thread's busy time (the guest lock's quantum, the game's
     * timers, the profile's per-call timing all read this) */
    return (uint64_t)(emscripten_get_now() * 1e6);
#elif defined(__SWITCH__)
    /* the CPU's counter (19.2 MHz) straight: newlib's clock_gettime around it was 4% of the game thread's
     * time, the game reading its timers through this thousands of times a frame (docs/switch.md) */
    return armTicksToNs(armGetSystemTick());
#endif
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

uint64_t plat_wall_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

unsigned char* plat_read_file(const char* path, size_t* size)
{
#if defined(__EMSCRIPTEN__)
    if (HTTP(path))
        return httpfs_read_all(path, size);
#endif
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        *size = (size_t)n;
    return buf;
}

void plat_debug(const char* text)
{
    fputs(text, stderr);
}

/* --- files ---------------------------------------------------------------------------------------- */
static RT_TLS PlatError t_error;

PlatError plat_error(void)
{
    return t_error;
}

/* errno to PlatError; ENOENT is "file" or "path" depending on whether the parent exists */
static void set_error(const char* path)
{
    switch (errno)
    {
    case ENOENT:
    {
        t_error = PLAT_NOT_FOUND;
        if (path)
        {
            char parent[1400];
            snprintf(parent, sizeof parent, "%s", path);
            char* s = strrchr(parent, '/');
            struct stat st;
            if (s && s != parent)
            {
                *s = 0;
                if (stat(parent, &st) != 0 || !S_ISDIR(st.st_mode))
                    t_error = PLAT_PATH_NOT_FOUND;
            }
        }
        break;
    }
    case ENOTDIR:
    case ENAMETOOLONG: t_error = PLAT_PATH_NOT_FOUND; break;
    case EACCES:
    case EPERM:
    case EROFS:
    case EISDIR: t_error = PLAT_ACCESS; break;
    case EEXIST: t_error = PLAT_EXISTS; break;
    case ENOTEMPTY: t_error = PLAT_NOT_EMPTY; break;
    default: t_error = PLAT_FAILED; break;
    }
}

struct PlatFile
{
    int fd;
    HttpFile* h; /* the browser's served files */
    uint32_t path_hash; /* (the Switch's read profile) */
    uint64_t path64;    /* PLAT_CACHED on the Switch: read through the block cache (bc_read), by this */
};

#if defined(__SWITCH__)
/* FFXI_PROFILE: what the game reads, on any thread (atomics; the game thread reports it, plat_read_report): reads,
 * bytes and time by size, and of them those of a file, offset and size read before (what a cache would save) */
static int g_read_prof = -1;
static uint64_t g_rd_n[4], g_rd_bytes[4], g_rd_ns[4], g_rd_again_n, g_rd_again_bytes, g_rd_again_ns;
static uint64_t g_rd_seen[1u << 16];

static uint32_t path_hash(const char* p)
{
    uint32_t h = 2166136261u;
    for (; *p; ++p)
        h = (h ^ (uint8_t)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p)) * 16777619u;
    return h;
}

static void read_note(const PlatFile* f, int64_t off, int64_t got, uint64_t ns)
{
    if (got <= 0)
        return;
    int b = got < 4096 ? 0 : got < 65536 ? 1 : got < 1048576 ? 2 : 3;
    __atomic_fetch_add(&g_rd_n[b], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_rd_bytes[b], (uint64_t)got, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_rd_ns[b], ns, __ATOMIC_RELAXED);
    uint64_t key = ((uint64_t)f->path_hash << 32) ^ ((uint64_t)off * 0x9E3779B97F4A7C15ull) ^ (uint64_t)got;
    key |= 1;
    uint64_t* slot = &g_rd_seen[(key ^ (key >> 29)) & 0xFFFFu];
    if (__atomic_load_n(slot, __ATOMIC_RELAXED) == key)
    {
        __atomic_fetch_add(&g_rd_again_n, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_rd_again_bytes, (uint64_t)got, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_rd_again_ns, ns, __ATOMIC_RELAXED);
    }
    else
        __atomic_store_n(slot, key, __ATOMIC_RELAXED);
}

int plat_read_report(char* out, size_t n)
{
    if (g_read_prof <= 0)
        return 0;
    static const char* names[4] = { "under 4 KB", "4-64 KB", "64 KB-1 MB", "1 MB and over" };
    int o = snprintf(out, n, "reads:");
    for (int b = 0; b < 4; ++b)
    {
        uint64_t c = __atomic_exchange_n(&g_rd_n[b], 0, __ATOMIC_RELAXED);
        uint64_t by = __atomic_exchange_n(&g_rd_bytes[b], 0, __ATOMIC_RELAXED);
        uint64_t ns = __atomic_exchange_n(&g_rd_ns[b], 0, __ATOMIC_RELAXED);
        if (c && o < (int)n)
            o += snprintf(out + o, n - (size_t)o, " %s %llu (%.1f MB, %.0f ms, %.2f ms each);", names[b],
                (unsigned long long)c, (double)by / 1048576.0, (double)ns * 1e-6, (double)ns * 1e-6 / (double)c);
    }
    uint64_t an = __atomic_exchange_n(&g_rd_again_n, 0, __ATOMIC_RELAXED);
    uint64_t ab = __atomic_exchange_n(&g_rd_again_bytes, 0, __ATOMIC_RELAXED);
    uint64_t ans = __atomic_exchange_n(&g_rd_again_ns, 0, __ATOMIC_RELAXED);
    if (o < (int)n)
        snprintf(out + o, n - (size_t)o, " read before: %llu (%.1f MB, %.0f ms)", (unsigned long long)an,
            (double)ab / 1048576.0, (double)ans * 1e-6);
    return 1;
}
#endif

#if defined(__SWITCH__)
/* The game's DATs through a block cache (PLAT_CACHED: k32_io.c's read-only opens of them). On the Switch's card a read
 * costs ~1 ms however small (a request to the file system service), and in a crowd the game reads 100-400 times in
 * 10 s, under 64 KB mostly and 40-60% of them a file, offset and size read before. Reads under 256 KB go through 64 KB
 * blocks, the last 32 MB of them kept (4-way sets, least recently used out); bigger ones go to the card as they are.
 * The DATs are never written while the game runs (k32_io.c opens them so only read-only), so a block stays good. */
#define BC_BLOCK 65536u
#define BC_WAYS 4u
#define BC_SETS 128u /* 512 blocks: 32 MB */
#define BC_DIRECT (256u * 1024u)

typedef struct BcBlock
{
    uint64_t path, block; /* path: the file's FNV-1a 64 (case folded); 0 empty */
    uint32_t len;         /* < BC_BLOCK only for a file's last */
    uint64_t used;
    uint8_t* data;
} BcBlock;

static BcBlock g_bc[BC_SETS][BC_WAYS];
static pthread_mutex_t g_bc_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_bc_tick;

static uint64_t path_hash64(const char* p)
{
    uint64_t h = 1469598103934665603ull;
    for (; *p; ++p)
        h = (h ^ (uint8_t)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p)) * 1099511628211ull;
    return h ? h : 1;
}

/* a block's bytes into out (at most len), from the cache or the card; its length, or -1 */
static int64_t bc_block(PlatFile* f, uint64_t block, uint8_t* out)
{
    unsigned set = (unsigned)((f->path64 ^ (block * 0x9E3779B97F4A7C15ull)) >> 7) % BC_SETS;
    pthread_mutex_lock(&g_bc_mu);
    for (unsigned w = 0; w < BC_WAYS; ++w)
    {
        BcBlock* b = &g_bc[set][w];
        if (b->path == f->path64 && b->block == block && b->data)
        {
            b->used = ++g_bc_tick;
            uint32_t len = b->len;
            memcpy(out, b->data, len);
            pthread_mutex_unlock(&g_bc_mu);
            return len;
        }
    }
    pthread_mutex_unlock(&g_bc_mu);
    /* a miss: from the card, outside the cache's lock */
    ssize_t r;
    if (lseek(f->fd, (off_t)(block * BC_BLOCK), SEEK_SET) < 0)
        return -1;
    do
        r = read(f->fd, out, BC_BLOCK);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        return -1;
    pthread_mutex_lock(&g_bc_mu);
    BcBlock* victim = &g_bc[set][0];
    for (unsigned w = 0; w < BC_WAYS; ++w)
    {
        BcBlock* b = &g_bc[set][w];
        if (b->path == f->path64 && b->block == block)
        {
            victim = b; /* (another thread read it meanwhile) */
            break;
        }
        if (!b->data || b->used < victim->used)
            victim = b;
    }
    if (!victim->data)
        victim->data = (uint8_t*)malloc(BC_BLOCK);
    if (victim->data)
    {
        memcpy(victim->data, out, (size_t)r);
        victim->path = f->path64, victim->block = block, victim->len = (uint32_t)r, victim->used = ++g_bc_tick;
    }
    pthread_mutex_unlock(&g_bc_mu);
    return r;
}

/* a cached file's read at its position: through the blocks, the position then moved past what was read */
static int64_t bc_read(PlatFile* f, void* buf, uint32_t n)
{
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0)
        return -1;
    if (n >= BC_DIRECT)
    {
        ssize_t r;
        do
            r = read(f->fd, buf, n);
        while (r < 0 && errno == EINTR);
        return r;
    }
    static __thread uint8_t* tmp;
    if (!tmp && !(tmp = (uint8_t*)malloc(BC_BLOCK)))
        return -1;
    uint64_t done = 0;
    while (done < n)
    {
        uint64_t at = (uint64_t)pos + done, block = at / BC_BLOCK, in = at % BC_BLOCK;
        int64_t len = bc_block(f, block, tmp);
        if (len < 0)
        {
            if (!done)
                return -1;
            break;
        }
        if ((uint64_t)len <= in)
            break; /* the end of the file */
        uint64_t take = (uint64_t)len - in;
        if (take > n - done)
            take = n - done;
        memcpy((uint8_t*)buf + done, tmp + in, (size_t)take);
        done += take;
        if ((uint64_t)len < BC_BLOCK)
            break;
    }
    lseek(f->fd, pos + (off_t)done, SEEK_SET);
    return (int64_t)done;
}
#endif

PlatFile* plat_file_open(const char* path, int flags)
{
#if defined(__EMSCRIPTEN__)
    if (HTTP(path))
    {
        int64_t size;
        int dir;
        if (!httpfs_stat(path, &size, &dir))
        {
            t_error = (flags & PLAT_CREATE) ? PLAT_ACCESS : PLAT_NOT_FOUND;
            return NULL;
        }
        HttpFile* h = (flags & (PLAT_WRITE | PLAT_TRUNCATE | PLAT_EXCL)) || dir ? NULL : httpfs_open(path);
        if (!h)
        {
            t_error = PLAT_ACCESS;
            return NULL;
        }
        PlatFile* f = (PlatFile*)calloc(1, sizeof *f);
        f->fd = -1, f->h = h;
        t_error = PLAT_OK;
        return f;
    }
#endif
    int rw = (flags & PLAT_READ) && (flags & PLAT_WRITE) ? O_RDWR : (flags & PLAT_WRITE) ? O_WRONLY : O_RDONLY;
    int o = rw | ((flags & PLAT_CREATE) ? O_CREAT : 0) | ((flags & PLAT_EXCL) ? O_CREAT | O_EXCL : 0) |
        ((flags & PLAT_TRUNCATE) ? O_TRUNC : 0);
    int fd = open(path, o | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        set_error(path);
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) /* CreateFile does not open directories */
    {
        close(fd);
        t_error = PLAT_ACCESS;
        return NULL;
    }
    PlatFile* f = (PlatFile*)calloc(1, sizeof *f);
    f->fd = fd;
#if defined(__SWITCH__)
    if (g_read_prof < 0)
    {
        const char* e = getenv("FFXI_PROFILE");
        g_read_prof = e && *e && *e != '0';
    }
    if (g_read_prof)
        f->path_hash = path_hash(path);
    if ((flags & PLAT_CACHED) && !(flags & PLAT_WRITE))
        f->path64 = path_hash64(path);
#endif
    t_error = PLAT_OK;
    return f;
}

int64_t plat_file_read(PlatFile* f, void* buf, uint32_t n)
{
#if defined(__EMSCRIPTEN__)
    if (f->h)
    {
        int64_t r = httpfs_read(f->h, buf, n);
        if (r < 0)
            t_error = PLAT_FAILED;
        return r;
    }
#endif
    ssize_t r;
#if defined(__SWITCH__)
    int64_t off = g_read_prof > 0 ? (int64_t)lseek(f->fd, 0, SEEK_CUR) : 0;
    uint64_t t0 = g_read_prof > 0 ? rt_monotonic_ns() : 0;
#endif
#if defined(__SWITCH__)
    if (f->path64)
        r = (ssize_t)bc_read(f, buf, n);
    else
#endif
        do
            r = read(f->fd, buf, n);
        while (r < 0 && errno == EINTR);
    if (r < 0)
        set_error(NULL);
#if defined(__SWITCH__)
    if (g_read_prof > 0)
        read_note(f, off, r, rt_monotonic_ns() - t0);
#endif
    return r;
}

int64_t plat_file_write(PlatFile* f, const void* buf, uint32_t n)
{
    if (f->h)
    {
        t_error = PLAT_ACCESS;
        return -1;
    }
    ssize_t r;
    do
        r = write(f->fd, buf, n);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        set_error(NULL);
    return r;
}

int64_t plat_file_seek(PlatFile* f, int64_t offset, int whence)
{
#if defined(__EMSCRIPTEN__)
    if (f->h)
        return httpfs_seek(f->h, offset, whence);
#endif
    off_t r = lseek(f->fd, (off_t)offset, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
    if (r < 0)
        set_error(NULL);
    return r;
}

int64_t plat_file_size(PlatFile* f)
{
#if defined(__EMSCRIPTEN__)
    if (f->h)
        return httpfs_size(f->h);
#endif
    struct stat st;
    return fstat(f->fd, &st) == 0 ? (int64_t)st.st_size : -1;
}

int plat_file_truncate(PlatFile* f)
{
    if (f->h)
        return 0;
    off_t at = lseek(f->fd, 0, SEEK_CUR);
    return at >= 0 && ftruncate(f->fd, at) == 0;
}

int plat_file_flush(PlatFile* f)
{
    if (f->h)
        return 1;
    return fsync(f->fd) == 0;
}

void plat_file_close(PlatFile* f)
{
#if defined(__EMSCRIPTEN__)
    if (f->h)
        httpfs_close(f->h);
    else
#endif
        close(f->fd);
    free(f);
}

static uint64_t ts_ms(struct timespec t) { return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }

int plat_stat(const char* path, PlatStat* st)
{
#if defined(__EMSCRIPTEN__)
    if (HTTP(path))
    {
        int64_t size;
        int dir;
        if (!httpfs_stat(path, &size, &dir))
        {
            t_error = PLAT_NOT_FOUND;
            return 0;
        }
        memset(st, 0, sizeof *st);
        st->is_dir = dir, st->readonly = 1, st->size = (uint64_t)size;
        return 1;
    }
#endif
    struct stat s;
    if (stat(path, &s) != 0)
    {
        set_error(path);
        return 0;
    }
    st->is_dir = S_ISDIR(s.st_mode);
    st->readonly = access(path, W_OK) != 0;
    st->size = (uint64_t)s.st_size;
#if defined(__APPLE__)
    st->ctime_ms = ts_ms(s.st_birthtimespec);
    st->atime_ms = ts_ms(s.st_atimespec);
    st->mtime_ms = ts_ms(s.st_mtimespec);
#else
    st->ctime_ms = ts_ms(s.st_ctim);
    st->atime_ms = ts_ms(s.st_atim);
    st->mtime_ms = ts_ms(s.st_mtim);
#endif
    return 1;
}

int plat_mkdir(const char* path)
{
    if (HTTP(path))
    {
        t_error = PLAT_ACCESS;
        return 0;
    }
    if (mkdir(path, 0755) == 0)
        return 1;
    set_error(path);
    return 0;
}

int plat_rmdir(const char* path)
{
    if (HTTP(path))
    {
        t_error = PLAT_ACCESS;
        return 0;
    }
    if (rmdir(path) == 0)
        return 1;
    set_error(path);
    return 0;
}

int plat_unlink(const char* path)
{
    if (HTTP(path))
    {
        t_error = PLAT_ACCESS;
        return 0;
    }
    if (unlink(path) == 0)
        return 1;
    set_error(path);
    return 0;
}

int plat_rename(const char* from, const char* to)
{
    if (HTTP(from) || HTTP(to))
    {
        t_error = PLAT_ACCESS;
        return 0;
    }
    if (rename(from, to) == 0)
        return 1;
    set_error(from);
    return 0;
}

struct PlatDir
{
    DIR* d;
    HttpDir* h;
};

PlatDir* plat_dir_open(const char* path)
{
#if defined(__EMSCRIPTEN__)
    if (HTTP(path))
    {
        HttpDir* h = httpfs_dir_open(path);
        if (!h)
        {
            t_error = PLAT_PATH_NOT_FOUND;
            return NULL;
        }
        PlatDir* p = (PlatDir*)calloc(1, sizeof *p);
        p->h = h;
        return p;
    }
#endif
    DIR* d = opendir(path);
    if (!d)
    {
        set_error(path);
        return NULL;
    }
    PlatDir* p = (PlatDir*)calloc(1, sizeof *p);
    p->d = d;
    return p;
}

const char* plat_dir_next(PlatDir* d)
{
#if defined(__EMSCRIPTEN__)
    if (d->h)
        return httpfs_dir_next(d->h);
#endif
    for (struct dirent* e; (e = readdir(d->d));)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            return e->d_name;
    return NULL;
}

void plat_dir_close(PlatDir* d)
{
#if defined(__EMSCRIPTEN__)
    if (d->h)
        httpfs_dir_close(d->h);
    else
#endif
        closedir(d->d);
    free(d);
}

/* --- time and memory ------------------------------------------------------------------------------- */
void plat_split_time(uint64_t ms, int local, PlatTime* t)
{
    time_t s = (time_t)(ms / 1000);
    struct tm tm;
    if (local)
        localtime_r(&s, &tm);
    else
        gmtime_r(&s, &tm);
    t->year = tm.tm_year + 1900;
    t->month = tm.tm_mon + 1;
    t->day_of_week = tm.tm_wday;
    t->day = tm.tm_mday;
    t->hour = tm.tm_hour;
    t->minute = tm.tm_min;
    t->second = tm.tm_sec;
    t->ms = (int)(ms % 1000);
}

int32_t plat_utc_bias_minutes(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
#if defined(__SWITCH__)
    /* newlib's struct tm has no tm_gmtoff: the UTC time read back as local time is now - offset */
    struct tm utc;
    gmtime_r(&now, &utc);
    utc.tm_isdst = tm.tm_isdst;
    return (int32_t)((mktime(&utc) - now) / 60);
#else
    return (int32_t)(-tm.tm_gmtoff / 60); /* Win32: UTC = local + bias */
#endif
}

void plat_memory(uint64_t* total, uint64_t* avail)
{
#if defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    *total = mem;
    *avail = mem / 2; /* the guest only uses this for a "memory load" figure */
#elif defined(__EMSCRIPTEN__)
    *total = 4ull << 30; /* wasm32's whole memory */
    *avail = *total - emscripten_get_heap_size();
#elif defined(__SWITCH__)
    uint64_t used = 0;
    *total = 0;
    svcGetInfo(total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    *avail = *total > used ? *total - used : 0;
#else
    struct sysinfo si;
    sysinfo(&si);
    *total = (uint64_t)si.totalram * si.mem_unit;
    *avail = (uint64_t)si.freeram * si.mem_unit;
#endif
}
