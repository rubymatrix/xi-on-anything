#pragma once
/* Dependency guard and owned packets for an opt-in, single-threaded pass plan.
 * This helper never issues Vulkan commands. The backend owns logical/physical
 * target selection and must flush before EVERY external resource mutation,
 * destruction, observation, submit and presentation. No queued resource may
 * outlive that flush. Resources are tracked as whole textures conservatively,
 * so aliases through different cube faces/mips still conflict.
 */
#include "gfx_worker_snapshot.h"
#include <memory>
#include <vector>

namespace gfxplan
{
struct Target
{
    GfxTex* color = nullptr;
    uint32_t face = 0, level = 0;
    GfxTex* depth = nullptr;
};
struct Clear
{
    uint32_t nrects = 0, flags = 0, color = 0, stencil = 0, vp[6]{};
    float z = 0;
    const int32_t* rects() const { return nrects ? reinterpret_cast<const int32_t*>(this + 1) : nullptr; }
};
struct DeletePayload
{
    void operator()(void* p) const noexcept { ::operator delete(p); }
};
struct Command
{
    enum Kind
    {
        DRAW,
        CLEAR
    } kind = DRAW;
    Target target;
    size_t bytes = 0;
    std::unique_ptr<void, DeletePayload> payload;
    const GfxDraw* draw() const { return kind == DRAW ? static_cast<const GfxDraw*>(payload.get()) : nullptr; }
    const Clear* clear() const { return kind == CLEAR ? static_cast<const Clear*>(payload.get()) : nullptr; }
};
class Queue
{
    std::vector<Command> commands_;
    size_t bytes_ = 0, limit_, maxCommands_;
    bool room(size_t bytes) const
    {
        return commands_.size() < maxCommands_ && bytes_ <= limit_ && bytes <= limit_ - bytes_ &&
               sizeof(Command) <= limit_ - bytes_ - bytes;
    }
    bool append(Command&& c) noexcept
    {
        const size_t cost = c.bytes + sizeof(Command);
        try
        {
            commands_.push_back(std::move(c));
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        bytes_ += cost;
        return true;
    }

public:
    explicit Queue(size_t limit = 16u << 20, size_t maxCommands = 512) : limit_(limit), maxCommands_(maxCommands) {}
    bool empty() const { return commands_.empty(); }
    size_t size() const { return commands_.size(); }
    size_t bytes() const { return bytes_; }
    const std::vector<Command>& commands() const { return commands_; }
    void reset()
    {
        commands_.clear();
        bytes_ = 0;
    }
    /* A later immediate operation must not pass an earlier deferred operation
     * if it observes a pending result, overwrites a pending source, or writes
     * the same target (blend/depth/stencil order). Appended commands retain their
     * original order and therefore need no check against one another.
     */
    bool conflicts(Target destination, const GfxDraw* draw) const
    {
        for (const auto& c : commands_)
        {
            GfxTex* written = c.target.color;
            if (written && (written == destination.color || written == destination.depth))
                return true;
            if (draw)
                for (GfxTex* read : draw->tex)
                    if (read && read == written)
                        return true;
            if (const GfxDraw* queued = c.draw())
                for (GfxTex* read : queued->tex)
                    if (read && (read == destination.color || read == destination.depth))
                        return true;
        }
        return false;
    }
    bool try_draw(Target destination, const GfxDraw& draw) noexcept
    {
        if (!destination.color || destination.depth)
            return false;
        gfxworker::DrawPlan plan;
        if (!gfxworker::draw_plan(draw, draw.vs_token_count, draw.ps_token_count, plan) || !room(plan.bytes))
            return false;
        Command c;
        c.kind = Command::DRAW;
        c.target = destination;
        c.bytes = plan.bytes;
        c.payload.reset(::operator new(plan.bytes, std::nothrow));
        if (!c.payload)
            return false;
        gfxworker::fill_draw(c.payload.get(), draw, plan);
        return append(std::move(c));
    }
    bool try_clear(Target destination, uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z,
                   uint32_t stencil, const uint32_t vp[6]) noexcept
    {
        if (!destination.color || destination.depth || !vp || (nrects && !rects))
            return false;
        size_t bytes = sizeof(Clear);
        if (uint64_t(nrects) * 16 > SIZE_MAX || !gfxworker::add_bytes(bytes, size_t(nrects) * 16) || !room(bytes))
            return false;
        Command c;
        c.kind = Command::CLEAR;
        c.target = destination;
        c.bytes = bytes;
        c.payload.reset(::operator new(bytes, std::nothrow));
        if (!c.payload)
            return false;
        auto* out = new (c.payload.get()) Clear{};
        out->nrects = nrects;
        out->flags = flags;
        out->color = color;
        out->z = z;
        out->stencil = stencil;
        std::memcpy(out->vp, vp, sizeof out->vp);
        if (nrects)
            std::memcpy(static_cast<void*>(out + 1), rects, size_t(nrects) * 16);
        return append(std::move(c));
    }
};
}
