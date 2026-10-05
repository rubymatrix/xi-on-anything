/* Watermarks for sizing other targets (the wasm port's gate 0): how deep the host stack gets under
 * translated code, how much guest stack the game uses, and how much of the guest window it commits.
 * Only built in with -DRT_WATERMARK (XI_CFLAGS=-DRT_WATERMARK); the stack probe sits on
 * rt_call_indirect, the hottest call in the game. host64 calls watermark_frame at every Present. */
#include "watermark.h"

#if defined(RT_WATERMARK)
#include <pthread.h>
#include <stdint.h>

#include "gwin.h"
#include "plat.h"

static _Thread_local uintptr_t t_top;   /* this host thread's stack top */
static _Thread_local uint32_t t_esp_hi;  /* the guest esp at this thread's first probe */
static volatile uint32_t g_host_max, g_guest_max, g_host_where, g_guest_where;

static uintptr_t stack_top(void)
{
#if defined(__APPLE__)
    return (uintptr_t)pthread_get_stackaddr_np(pthread_self());
#else
    pthread_attr_t a;
    void* lo = 0;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &a) == 0)
    {
        pthread_attr_getstack(&a, &lo, &size);
        pthread_attr_destroy(&a);
    }
    return (uintptr_t)lo + size;
#endif
}

static int raise_max(volatile uint32_t* max, uint32_t v)
{
    uint32_t old;
    while ((old = *max) < v)
        if (plat_atomic_cas32(max, old, v) == old)
            return 1;
    return 0;
}

void watermark_probe(Guest* g, uint32_t target)
{
    char here;
    if (!t_top)
        t_top = stack_top(), t_esp_hi = g->esp;
    if (raise_max(&g_host_max, (uint32_t)(t_top - (uintptr_t)&here)))
        g_host_where = target;
    if (t_esp_hi > g->esp && raise_max(&g_guest_max, t_esp_hi - g->esp))
        g_guest_where = target;
}

void watermark_frame(void)
{
    static uint64_t last;
    static uint32_t host_said, guest_said;
    static uint64_t peak_said;
    uint64_t now = rt_monotonic_ns();
    if (now - last < 10000000000ull)
        return;
    last = now;
    GwinStats s;
    gwin_stats(&s);
    if (g_host_max == host_said && g_guest_max == guest_said && s.committed_peak == peak_said)
        return;
    host_said = g_host_max, guest_said = g_guest_max, peak_said = s.committed_peak;
    rt_log("[recomp] watermark: host stack %u KB (calling %08x), guest stack %u KB (calling %08x); "
           "guest memory %llu MB committed, peak %llu MB, top %08x\n",
           host_said >> 10, g_host_where, guest_said >> 10, g_guest_where,
           (unsigned long long)(s.committed >> 20), (unsigned long long)(s.committed_peak >> 20), s.top);
}
#endif
