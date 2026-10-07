/* The runtime around the translated geometry parent for tests/geometry_replay_test.py, built into
 * a shared library with it. No game code or input data. */
#include "build.h"
#include "geometry_guest.h"
#include "geometry_test_memory.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
unsigned char* rt_guest_base;
uint32_t rt_reloc_delta, rt_image_lo, rt_image_hi;
volatile uint32_t rt_lock_contended;
static unsigned char mapped[1u << 17];
static int depth;
extern const GuestFn rt_orig_geometry_parent;
void rt_log(const char* fmt, ...)
{
    (void)fmt;
}
int gt_holds(void)
{
    return 1;
}
void gt_noyield(int on)
{
    depth += on ? 1 : -1;
    if (depth < 0)
        abort();
}
int gwin_is_committed(uint32_t a)
{
    unsigned p = a >> 12;
    return (mapped[p >> 3] >> (p & 7)) & 1;
}
void rt_safepoint(void)
{
    if (!depth)
        abort();
}
void rt_call_indirect(Guest* g, uint32_t t)
{
    (void)g;
    (void)t;
    abort();
}
void rt_fatal(Guest* g, uint32_t a, const char* text)
{
    (void)g;
    fprintf(stderr, "unexpected original fallback %x %s\n", a, text);
    abort();
}
int test_init(void)
{
    rt_guest_base = geometry_test_reserve();
    if (!rt_guest_base)
        return 0;
    setenv("FFXI_NATIVE_GEOMETRY", "1", 1);
    geometry_hooks_init();
    return 1;
}
void test_reset_pages(void)
{
    memset(mapped, 0, sizeof mapped);
}
void test_page(uint32_t address, const void* contents)
{
    unsigned p = address >> 12;
    geometry_test_commit(rt_guest_base, address, 4096);
    mapped[p >> 3] |= (unsigned char)(1u << (p & 7));
    memcpy(GUEST_PTR(address), contents, 4096);
}
int test_run(Guest* g)
{
    const GeometryLayout layout = {FFXI_GEOMETRY_INFO, FFXI_GEOMETRY_CALLBACK, FFXI_GEOMETRY_PALETTE,
                                   FFXI_GEOMETRY_COUNTS};
    int accepted = geometry_guest_run(g, &layout, rt_orig_geometry_parent);
    if (depth)
        abort();
    return accepted;
}
void test_scalar(Guest* g)
{
    ++depth;
    rt_orig_geometry_parent(g);
    --depth;
}
void test_destroy(void)
{
    geometry_test_release(rt_guest_base);
    rt_guest_base = NULL;
}
