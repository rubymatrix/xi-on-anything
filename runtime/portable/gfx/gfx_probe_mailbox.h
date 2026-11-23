#pragma once

// CPU-only visibility history used by the optional Android renderer worker.
// One producer registers each (native texture allocation, face, mip) lifetime,
// orders mutations and begins producer frames. A worker publishes completed
// copies. No guest pointers, GPU handles, renderer calls or waiting live here.
//
// Admit only the existing 16x16 A8R8G8B8 visibility whitelist. Callers must issue
// a fresh GPU copy at the Request's command-stream position (or do the ordered
// exact fallback), then publish its OWN bytes and original token. Never publish
// the return of the historical keyed-read API under a newer request's token.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace gfxprobe
{
constexpr size_t PIXEL_BYTES = 16 * 16 * 4;
constexpr size_t COPIES_PER_KEY = 4;
constexpr uint32_t MAX_AGE = 64;
using Pixels = std::array<uint8_t, PIXEL_BYTES>;

struct Surface
{
    uint64_t lifetime = 0;
    uint32_t face = 0, mip = 0;
    explicit operator bool() const { return lifetime != 0; }
};
inline bool operator==(Surface a, Surface b)
{
    return a.lifetime == b.lifetime && a.face == b.face && a.mip == b.mip;
}

struct Request
{
    Surface surface;
    uint64_t key = 0, cpuEpoch = 0, issuedFrame = 0, sequence = 0;
    explicit operator bool() const { return sequence != 0; }
};

enum class ExactReason
{
    None,
    NoFrame,
    InvalidSurface,
    UnknownKey,
    KeyCapacity,
    Duplicate,
    InvalidAge,
    CpuCurrent,
    UnresolvedCpuMutation,
    Missing,
    Stale,
    RequestCapacity
};
struct Read
{
    bool cached = false;
    ExactReason reason = ExactReason::Missing;
    Pixels pixels{}; // owned snapshot; valid only when cached is true
    uint64_t sourceFrame = 0, sourceSequence = 0;
    Request refresh; // optional; caller must publish or cancel even on exact fallback
};
enum class Publication
{
    Rejected,
    Published,
    Superseded
};
enum class CpuWrite
{
    Whole,
    Partial
};

class Mailbox
{
public:
    // All table/result storage is allocated here. No call grows a table. Each
    // slot owns one 1024-byte result and four outstanding request identities.
    explicit Mailbox(size_t surfaceCapacity = 16, size_t keyCapacity = 128)
        : surfaces_(surfaceCapacity), keys_(keyCapacity)
    {
        if (!surfaceCapacity || !keyCapacity)
            throw std::invalid_argument("probe mailbox capacity");
    }
    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;

    // Register ONCE per native allocation/subresource, never once per actor or
    // frame. A recreated native pointer must receive a new Surface. Existing
    // producer wrappers are responsible for associating handles with textures.
    Surface create_surface(uint32_t face = 0, uint32_t mip = 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || nextLifetime_ == limit())
            return {};
        for (auto& slot : surfaces_)
            if (!slot.live)
            {
                slot = SurfaceState{};
                slot.live = true;
                slot.id = {nextLifetime_++, face, mip};
                return slot.id;
            }
        return {};
    }
    bool destroy_surface(Surface surface)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = find_surface(surface);
        if (!slot)
            return false;
        erase_keys(surface);
        slot->live = false;
        return true;
    }

    // Frame IDs belong to the producer, not Vulkan's submit/frame-ring counter.
    // Equal/backwards/zero IDs are rejected without changing policy state.
    bool begin_frame(uint64_t frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || !frame || frame <= frame_)
            return false;
        frame_ = frame;
        return true;
    }

    // Direct CPU writes and copies derived from CPU-current/uncertain source
    // bytes invalidate every actor history on this subresource immediately.
    // Partial writes into an already CPU-current image preserve that status.
    // An uncertain partial write stays exact until a new, completed post-write
    // sample resolves it. Normal GPU probe draws/copies MUST NOT call this.
    bool cpu_write(Surface surface, CpuWrite write)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = find_surface(surface);
        if (!slot)
            return false;
        if (slot->epoch == limit())
        {
            erase_keys(surface);
            slot->live = false; // fail closed instead of wrapping identity
            return false;
        }
        ++slot->epoch;
        if (write == CpuWrite::Whole || slot->mode == Mode::CpuCurrent)
            slot->mode = Mode::CpuCurrent;
        else
            slot->mode = Mode::Unresolved;
        for (auto& entry : keys_)
            if (entry.live && entry.surface == surface)
            {
                entry.hasResult = false;
                for (auto& pending : entry.pending)
                    pending = {};
                // Preserve lastReadFrame: CPU write -> GPU write must not reopen
                // a duplicate key's delayed-read eligibility in this same frame.
            }
        return true;
    }

    // Ordinary GPU writes retain completed per-actor history. They invalidate
    // CPU-current bytes but do not resolve an uncertain CPU mutation. In that
    // case the first ordered exact post-mutation sample must seed the mailbox.
    bool gpu_write(Surface surface)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = find_surface(surface);
        if (!slot)
            return false;
        if (slot->mode == Mode::CpuCurrent)
            slot->mode = Mode::Gpu;
        return true;
    }

    Read read(Surface surface, uint64_t key, uint32_t maxAge)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Read result;
        if (!frame_)
        {
            result.reason = ExactReason::NoFrame;
            return result;
        }
        auto* slot = find_surface(surface);
        if (!slot)
        {
            result.reason = ExactReason::InvalidSurface;
            return result;
        }
        if (!key)
        {
            result.reason = ExactReason::UnknownKey;
            return result;
        }
        Entry* entry = find_key(surface, key);
        if (!entry)
            entry = allocate_key(surface, key);
        if (!entry)
        {
            result.reason = ExactReason::KeyCapacity;
            return result;
        }
        if (entry->lastReadFrame == frame_)
        {
            result.reason = ExactReason::Duplicate;
            return result;
        }
        entry->lastReadFrame = frame_;
        entry->retention = maxAge <= MAX_AGE ? maxAge : MAX_AGE;
        // Age0 and unsupported ages consume the marker but issue no delayed
        // request. The caller performs its original exact read.
        if (!maxAge || maxAge > MAX_AGE)
        {
            result.reason = ExactReason::InvalidAge;
            return result;
        }
        if (slot->mode == Mode::CpuCurrent)
            result.reason = ExactReason::CpuCurrent;
        else if (slot->mode == Mode::Unresolved)
            result.reason = ExactReason::UnresolvedCpuMutation;
        else if (!entry->hasResult)
            result.reason = ExactReason::Missing;
        else if (entry->sourceFrame >= frame_ || frame_ - entry->sourceFrame > maxAge)
            result.reason = ExactReason::Stale;
        else
        {
            result.cached = true;
            result.reason = ExactReason::None;
            result.pixels = entry->pixels; // copy while protected against publish/invalidate
            result.sourceFrame = entry->sourceFrame;
            result.sourceSequence = entry->sourceSequence;
        }
        // Even an exact first read may seed a current, post-mutation result for
        // later frames. Publishing this token means the caller obtained actual
        // bytes at THIS command position, never another historical cache hit.
        if (nextSequence_ != limit())
        {
            for (auto& pending : entry->pending)
                if (!pending.sequence)
                {
                    result.refresh = {surface, key, slot->epoch, frame_, nextSequence_++};
                    pending = result.refresh;
                    break;
                }
        }
        if (!result.refresh && !result.cached && result.reason == ExactReason::Missing)
            result.reason = ExactReason::RequestCapacity;
        return result;
    }

    Publication publish(const Request& request, const Pixels& completedPixels)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SurfaceState* surface = nullptr;
        Entry* entry = nullptr;
        Request* pending = find_request(request, surface, entry);
        if (!pending)
            return Publication::Rejected;
        *pending = {};
        if (entry->hasResult &&
            (request.issuedFrame < entry->sourceFrame ||
             (request.issuedFrame == entry->sourceFrame && request.sequence <= entry->sourceSequence)))
            return Publication::Superseded;
        entry->pixels = completedPixels;
        entry->sourceFrame = request.issuedFrame;
        entry->sourceSequence = request.sequence;
        entry->hasResult = true;
        // A current-epoch copy resolves uncertainty; a CPU-current surface
        // remains exact until a subsequent producer-side GPU write.
        if (surface->mode == Mode::Unresolved)
            surface->mode = Mode::Gpu;
        return Publication::Published;
    }
    bool cancel(const Request& request)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SurfaceState* surface = nullptr;
        Entry* entry = nullptr;
        Request* pending = find_request(request, surface, entry);
        if (!pending)
            return false;
        *pending = {};
        return true;
    }

    // A worker failure/shutdown closes this instance permanently. It has no
    // blocking readers; every future read is exact/unavailable, every callback
    // is rejected, and registration cannot resurrect a failed pipeline.
    void stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        for (auto& surface : surfaces_)
            surface.live = false;
        for (auto& entry : keys_)
            entry = Entry{};
    }
    size_t live_keys() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& entry : keys_)
            count += entry.live;
        return count;
    }
    size_t pending_requests() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& entry : keys_)
            if (entry.live)
                for (const auto& pending : entry.pending)
                    count += pending.sequence != 0;
        return count;
    }
    size_t capacity_bytes() const { return surfaces_.size() * sizeof(SurfaceState) + keys_.size() * sizeof(Entry); }

private:
    enum class Mode
    {
        Gpu,
        CpuCurrent,
        Unresolved
    };
    struct SurfaceState
    {
        Surface id;
        uint64_t epoch = 1;
        Mode mode = Mode::Gpu;
        bool live = false;
    };
    struct Entry
    {
        Surface surface;
        uint64_t key = 0, lastReadFrame = 0;
        uint32_t retention = 0;
        bool live = false, hasResult = false;
        uint64_t sourceFrame = 0, sourceSequence = 0;
        Pixels pixels{};
        std::array<Request, COPIES_PER_KEY> pending{};
    };
    static constexpr uint64_t limit() { return std::numeric_limits<uint64_t>::max(); }
    SurfaceState* find_surface(Surface id)
    {
        if (stopped_ || !id)
            return nullptr;
        for (auto& slot : surfaces_)
            if (slot.live && slot.id == id)
                return &slot;
        return nullptr;
    }
    Entry* find_key(Surface surface, uint64_t key)
    {
        for (auto& entry : keys_)
            if (entry.live && entry.surface == surface && entry.key == key)
                return &entry;
        return nullptr;
    }
    Entry* allocate_key(Surface surface, uint64_t key)
    {
        Entry* expired = nullptr;
        for (auto& entry : keys_)
        {
            if (!entry.live)
            {
                expired = &entry;
                break;
            }
            bool pending = false;
            for (const auto& request : entry.pending)
                pending |= request.sequence != 0;
            if (!pending && entry.lastReadFrame < frame_ && frame_ - entry.lastReadFrame > entry.retention && !expired)
                expired = &entry;
        }
        if (!expired)
            return nullptr;
        *expired = Entry{};
        expired->live = true;
        expired->surface = surface;
        expired->key = key;
        return expired;
    }
    void erase_keys(Surface surface)
    {
        for (auto& entry : keys_)
            if (entry.live && entry.surface == surface)
                entry = Entry{};
    }
    Request* find_request(const Request& request, SurfaceState*& surface, Entry*& entry)
    {
        if (!request || !(surface = find_surface(request.surface)) || request.cpuEpoch != surface->epoch ||
            !request.issuedFrame || request.issuedFrame > frame_ || !(entry = find_key(request.surface, request.key)))
            return nullptr;
        for (auto& pending : entry->pending)
            if (pending.sequence == request.sequence && pending.issuedFrame == request.issuedFrame &&
                pending.cpuEpoch == request.cpuEpoch && pending.key == request.key &&
                pending.surface == request.surface)
                return &pending;
        return nullptr;
    }
    mutable std::mutex mutex_;
    std::vector<SurfaceState> surfaces_;
    std::vector<Entry> keys_;
    uint64_t frame_ = 0, nextLifetime_ = 1, nextSequence_ = 1;
    bool stopped_ = false;
};
} // namespace gfxprobe
