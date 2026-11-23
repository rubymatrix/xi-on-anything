/* The letterbox the Android back end presents into: the largest rectangle with the content's aspect,
 * centered on the surface. In the surface's pixels, not SDL window units. */
#pragma once
#include <stdint.h>
typedef struct GfxPresentRect
{
    uint32_t x, y, w, h;
} GfxPresentRect;
static inline GfxPresentRect gfx_present_rect(uint32_t surface_w, uint32_t surface_h, uint32_t content_w,
                                              uint32_t content_h)
{
    GfxPresentRect r = {0, 0, 0, 0};
    if (!surface_w || !surface_h || !content_w || !content_h)
        return r;
    r.w = surface_w;
    r.h = (uint32_t)((uint64_t)surface_w * content_h / content_w);
    if (r.h > surface_h)
    {
        r.h = surface_h;
        r.w = (uint32_t)((uint64_t)surface_h * content_w / content_h);
    }
    if (!r.w)
        r.w = 1;
    if (!r.h)
        r.h = 1;
    r.x = (surface_w - r.w) / 2;
    r.y = (surface_h - r.h) / 2;
    return r;
}
