#pragma once
#include "gfx.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

namespace gfxworker
{
constexpr size_t MAX_SHADER_WORDS = 65537;
inline bool add_bytes(size_t& value, size_t add)
{
    if (add > std::numeric_limits<size_t>::max() - value)
        return false;
    value += add;
    return true;
}
inline bool align_bytes(size_t& value)
{
    if (value > std::numeric_limits<size_t>::max() - 15)
        return false;
    value = (value + 15) & ~size_t(15);
    return true;
}
struct DrawPlan
{
    size_t bytes = sizeof(GfxDraw), stream[4]{}, indices = 0, vs = 0, ps = 0;
    uint32_t vsWords = 0, psWords = 0;
};
inline bool draw_plan(const GfxDraw& d, uint32_t vsWords, uint32_t psWords, DrawPlan& plan)
{
    plan = DrawPlan{};
    // Explicit owned lengths only. Unknown programmed arrays drain/direct;
    // never scan caller memory looking for an END sentinel.
    if ((d.vs.prog && (!d.vs_tokens || !vsWords || vsWords > MAX_SHADER_WORDS)) ||
        (d.fs.prog && (!d.ps_tokens || !psWords || psWords > MAX_SHADER_WORDS)))
        return false;
    for (unsigned i = 0; i < 4; ++i)
        if (!d.buf[i] && d.data[i] && d.size[i])
        {
            if (!align_bytes(plan.bytes))
                return false;
            plan.stream[i] = plan.bytes;
            if (!add_bytes(plan.bytes, d.size[i]))
                return false;
        }
    if (!d.ibuf && d.indices)
    {
        if (d.index_size != 2 && d.index_size != 4)
            return false;
        uint64_t n = 0;
        switch (d.prim)
        {
        case GFX_POINTLIST:
            n = d.count;
            break;
        case GFX_LINELIST:
            n = uint64_t(d.count) * 2;
            break;
        case GFX_LINESTRIP:
            n = uint64_t(d.count) + 1;
            break;
        case GFX_TRIANGLELIST:
            n = uint64_t(d.count) * 3;
            break;
        case GFX_TRIANGLESTRIP:
        case GFX_TRIANGLEFAN:
            n = uint64_t(d.count) + 2;
            break;
        default:
            return false;
        }
        if (n > UINT32_MAX || n > std::numeric_limits<size_t>::max() / d.index_size)
            return false;
        if (!align_bytes(plan.bytes))
            return false;
        plan.indices = plan.bytes;
        if (!add_bytes(plan.bytes, size_t(n) * d.index_size))
            return false;
    }
    if (d.vs.prog)
    {
        if (!align_bytes(plan.bytes))
            return false;
        plan.vs = plan.bytes;
        plan.vsWords = vsWords;
        if (!add_bytes(plan.bytes, size_t(vsWords) * 4))
            return false;
    }
    if (d.fs.prog)
    {
        if (!align_bytes(plan.bytes))
            return false;
        plan.ps = plan.bytes;
        plan.psWords = psWords;
        if (!add_bytes(plan.bytes, size_t(psWords) * 4))
            return false;
    }
    return true;
}
inline void fill_draw(void* destination, const GfxDraw& d, const DrawPlan& plan)
{
    auto* bytes = static_cast<uint8_t*>(destination);
    auto* copy = new (destination) GfxDraw(d);
    for (unsigned i = 0; i < 4; ++i)
    {
        copy->data[i] = nullptr;
        if (plan.stream[i])
        {
            std::memcpy(bytes + plan.stream[i], d.data[i], d.size[i]);
            copy->data[i] = bytes + plan.stream[i];
        }
    }
    copy->indices = nullptr;
    copy->vs_tokens = nullptr;
    copy->ps_tokens = nullptr;
    if (plan.indices)
    {
        // Padding is not source data. Recalculate exact original index count.
        uint64_t n = d.prim == GFX_POINTLIST      ? d.count
                     : d.prim == GFX_LINELIST     ? uint64_t(d.count) * 2
                     : d.prim == GFX_LINESTRIP    ? uint64_t(d.count) + 1
                     : d.prim == GFX_TRIANGLELIST ? uint64_t(d.count) * 3
                                                  : uint64_t(d.count) + 2;
        std::memcpy(bytes + plan.indices, d.indices, size_t(n) * d.index_size);
        copy->indices = bytes + plan.indices;
    }
    if (plan.vs)
    {
        std::memcpy(bytes + plan.vs, d.vs_tokens, size_t(plan.vsWords) * 4);
        copy->vs_tokens = reinterpret_cast<uint32_t*>(bytes + plan.vs);
    }
    if (plan.ps)
    {
        std::memcpy(bytes + plan.ps, d.ps_tokens, size_t(plan.psWords) * 4);
        copy->ps_tokens = reinterpret_cast<uint32_t*>(bytes + plan.ps);
    }
}
struct TextureInfo
{
    uint32_t format, width, height, levels, faces;
    int use;
};
struct UploadPlan
{
    uint32_t rows = 0, rowBytes = 0;
    size_t bytes = 0;
};
inline bool upload_plan(const TextureInfo& t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w,
                        uint32_t h, uint32_t pitch, UploadPlan& p)
{
    if (face >= t.faces || level >= t.levels || level >= 32 || t.use == GFX_USE_DEPTH || !w || !h)
        return false;
    uint32_t mw = t.width >> level, mh = t.height >> level;
    if (!mw)
        mw = 1;
    if (!mh)
        mh = 1;
    if (x > mw || y > mh || w > mw - x || h > mh - y)
        return false;
    auto four = [](char a, char b, char c, char d) {
        return uint32_t(a) | uint32_t(b) << 8 | uint32_t(c) << 16 | uint32_t(d) << 24;
    };
    uint32_t block = t.format == four('D', 'X', 'T', '1') ? 8
                     : (t.format == four('D', 'X', 'T', '2') || t.format == four('D', 'X', 'T', '3') ||
                        t.format == four('D', 'X', 'T', '4') || t.format == four('D', 'X', 'T', '5'))
                         ? 16
                         : 0;
    uint32_t bpp = (t.format == 28 || t.format == 50)                                                           ? 1
                   : ((t.format >= 23 && t.format <= 26) || t.format == 51 || t.format == 60 || t.format == 80) ? 2
                                                                                                                : 4;
    uint64_t row = block ? ((uint64_t(w) + 3) / 4) * block : uint64_t(w) * bpp;
    uint64_t rows = block ? (uint64_t(h) + 3) / 4 : h;
    if (row > UINT32_MAX || pitch < row || (block && ((x | y) & 3)) || rows > std::numeric_limits<size_t>::max() / row)
        return false;
    p = {uint32_t(rows), uint32_t(row), size_t(rows * row)};
    return true;
}
inline void fill_upload(void* dst, const void* src, uint32_t pitch, const UploadPlan& p)
{
    for (uint32_t i = 0; i < p.rows; ++i)
        std::memcpy(static_cast<uint8_t*>(dst) + size_t(i) * p.rowBytes,
                    static_cast<const uint8_t*>(src) + size_t(i) * pitch, p.rowBytes);
}
}
