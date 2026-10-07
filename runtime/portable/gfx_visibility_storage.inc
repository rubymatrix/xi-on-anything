/* Exact full-pixel visibility storage ownership. Included twice by the backend:
 * first with GFX_VISIBILITY_STORAGE_MODEL_ONLY for these Vulkan-free helpers,
 * later for the native implementation. Tests include the same model section.
 * Full pixels are stored; an image is never rebuilt from an occlusion count. */
#ifndef GFX_VISIBILITY_STORAGE_MODEL_DEFINED
#define GFX_VISIBILITY_STORAGE_MODEL_DEFINED
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
namespace gfxvisibility
{
template <class Payload, size_t Capacity> class Pool
{
public:
    class Version
    {
        friend class Pool;
        size_t slot_ = 0;
        uint64_t generation_ = 0, epoch_ = 0, frame_ = UINT64_MAX, issue_ = 0, lastUse_ = 0;
        bool readyZero_ = false;
        Payload payload_{};

    public:
        size_t slotIndex() const { return slot_; }
        uint64_t slotGeneration() const { return generation_; }
        uint64_t deviceEpoch() const { return epoch_; }
        uint64_t producerFrame() const { return frame_; }
        uint64_t issueSerial() const { return issue_; }
        uint64_t lastUseSerial() const { return lastUse_; }
        const Payload& payload() const { return payload_; }
    };
    using Ref = std::shared_ptr<const Version>;

private:
    std::array<std::shared_ptr<Version>, Capacity> slots_;
    uint64_t epoch_;

public:
    explicit Pool(uint64_t epoch) : epoch_(epoch)
    {
        for (size_t i = 0; i < Capacity; ++i)
        {
            slots_[i] = std::make_shared<Version>();
            slots_[i]->slot_ = i;
            slots_[i]->epoch_ = epoch;
        }
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    static constexpr size_t capacity() { return Capacity; }
    /* Call only at a backend command preamble outside every render pass.
     * The callback records exact zero initialization and required GPU barriers.
     * Ownership and last use, not just producer completion, gate reuse. */
    template <class Prepare> size_t prepare(uint64_t completedSerial, uint64_t commandSerial, Prepare&& callback)
    {
        if (!commandSerial || commandSerial <= completedSerial)
            return 0;
        size_t count = 0;
        for (auto& slot : slots_)
            if (slot.use_count() == 1 && !slot->readyZero_ && slot->lastUse_ <= completedSerial)
            {
                // A callback may fail after recording part of its prezero work.
                // Preserve that GPU lifetime even on false/exception; never recycle early.
                slot->lastUse_ = commandSerial;
                if (callback(slot->slot_, slot->payload_))
                {
                    slot->readyZero_ = true;
                    ++count;
                }
            }
        return count;
    }
    /* No allocation or GPU operations here. A zero-ready slot is consumed once.
     * Rejected transactions release their Ref and wait for the next preamble;
     * no claim is made that an abandoned slot still contains reusable zeros. */
    Ref acquire(uint64_t producerFrame, uint64_t issueSerial) noexcept
    {
        if (!issueSerial || producerFrame == UINT64_MAX || !epoch_)
            return {};
        for (auto& slot : slots_)
            if (slot.use_count() == 1 && slot->readyZero_ && issueSerial >= slot->lastUse_ &&
                slot->generation_ != UINT64_MAX)
            {
                slot->readyZero_ = false;
                ++slot->generation_;
                slot->frame_ = producerFrame;
                slot->issue_ = slot->lastUse_ = issueSerial;
                return slot;
            }
        return {};
    }
    bool owns(const Ref& ref) const noexcept
    {
        return ref && ref->slot_ < Capacity && slots_[ref->slot_].get() == ref.get() && ref->epoch_ == epoch_;
    }
    bool touch(const Ref& ref, uint64_t commandSerial) noexcept
    {
        if (!owns(ref) || !commandSerial || commandSerial < ref->issue_)
            return false;
        auto& slot = slots_[ref->slot_];
        if (commandSerial > slot->lastUse_)
            slot->lastUse_ = commandSerial;
        return true;
    }
    bool idle(uint64_t completedSerial) const noexcept
    {
        for (const auto& slot : slots_)
            if (slot.use_count() != 1 || slot->lastUse_ > completedSerial)
                return false;
        return true;
    }
};
template <class Ref> bool current_producer(const Ref& ref, uint64_t frame) noexcept
{
    return ref && ref->issueSerial() && ref->producerFrame() == frame;
}
template <class Ref> bool completed(const Ref& ref, uint64_t completedSerial) noexcept
{
    return ref && ref->issueSerial() && ref->issueSerial() <= completedSerial;
}
template <class Ref>
bool history_eligible(const Ref& ref, uint64_t frame, uint64_t completedSerial, uint32_t maxAge) noexcept
{
    return ref && maxAge && maxAge <= 64 && completed(ref, completedSerial) && ref->producerFrame() < frame &&
           frame - ref->producerFrame() <= maxAge;
}
} // namespace gfxvisibility
#endif

#ifndef GFX_VISIBILITY_STORAGE_MODEL_ONLY
#if defined(FFXI_ANDROID_VULKAN)
// Embedded from the independently validated standalone storage shaders.
// Preserve EarlyFragmentTests and literal full BGRA writes; no runtime compiler substitution.
static const uint32_t visibilityQuadSpv[] = {
    0x7230203,  0x10300,    0x8000b,    0x76,       0x0,        0x20011,    0x1,        0x6000b,    0x1,
    0x4c534c47, 0x6474732e, 0x3035342e, 0x0,        0x3000e,    0x0,        0x1,        0x8000f,    0x0,
    0x4,        0x6e69616d, 0x0,        0x16,       0x49,       0x64,       0x30003,    0x2,        0x1c2,
    0x40005,    0x4,        0x6e69616d, 0x0,        0x30005,    0x9,        0x74,       0x60005,    0x16,
    0x565f6c67, 0x65747265, 0x646e4978, 0x7865,     0x50005,    0x19,       0x65646e69, 0x6c626178, 0x65,
    0x30005,    0x1c,       0x7870,     0x30005,    0x20,       0x50,       0x50006,    0x20,       0x0,
    0x74636572, 0x0,        0x50006,    0x20,       0x1,        0x61746164, 0x0,        0x50006,    0x20,
    0x2,        0x666e6f63, 0x6769,     0x50006,    0x20,       0x3,        0x72747865, 0x61,       0x30005,
    0x22,       0x70,       0x40005,    0x41,       0x65747865, 0x746e,     0x60005,    0x47,       0x505f6c67,
    0x65567265, 0x78657472, 0x0,        0x60006,    0x47,       0x0,        0x505f6c67, 0x7469736f, 0x6e6f69,
    0x70006,    0x47,       0x1,        0x505f6c67, 0x746e696f, 0x657a6953, 0x0,        0x70006,    0x47,
    0x2,        0x435f6c67, 0x4470696c, 0x61747369, 0x65636e,   0x70006,    0x47,       0x3,        0x435f6c67,
    0x446c6c75, 0x61747369, 0x65636e,   0x30005,    0x49,       0x0,        0x30005,    0x64,       0x7675,
    0x40047,    0x16,       0xb,        0x2a,       0x30047,    0x20,       0x2,        0x50048,    0x20,
    0x0,        0x23,       0x0,        0x50048,    0x20,       0x1,        0x23,       0x10,       0x50048,
    0x20,       0x2,        0x23,       0x20,       0x50048,    0x20,       0x3,        0x23,       0x30,
    0x30047,    0x47,       0x2,        0x50048,    0x47,       0x0,        0xb,        0x0,        0x50048,
    0x47,       0x1,        0xb,        0x1,        0x50048,    0x47,       0x2,        0xb,        0x3,
    0x50048,    0x47,       0x3,        0xb,        0x4,        0x40047,    0x64,       0x1e,       0x0,
    0x20013,    0x2,        0x30021,    0x3,        0x2,        0x30016,    0x6,        0x20,       0x40017,
    0x7,        0x6,        0x2,        0x40020,    0x8,        0x7,        0x7,        0x40015,    0xa,
    0x20,       0x0,        0x4002b,    0xa,        0xb,        0x4,        0x4001c,    0xc,        0x7,
    0xb,        0x4002b,    0x6,        0xd,        0x0,        0x5002c,    0x7,        0xe,        0xd,
    0xd,        0x4002b,    0x6,        0xf,        0x3f800000, 0x5002c,    0x7,        0x10,       0xf,
    0xd,        0x5002c,    0x7,        0x11,       0xd,        0xf,        0x5002c,    0x7,        0x12,
    0xf,        0xf,        0x7002c,    0xc,        0x13,       0xe,        0x10,       0x11,       0x12,
    0x40015,    0x14,       0x20,       0x1,        0x40020,    0x15,       0x1,        0x14,       0x4003b,
    0x15,       0x16,       0x1,        0x40020,    0x18,       0x7,        0xc,        0x40017,    0x1d,
    0x14,       0x4,        0x40017,    0x1e,       0x6,        0x4,        0x40017,    0x1f,       0xa,
    0x4,        0x6001e,    0x20,       0x1d,       0x1e,       0x1d,       0x1f,       0x40020,    0x21,
    0x9,        0x20,       0x4003b,    0x21,       0x22,       0x9,        0x4002b,    0x14,       0x23,
    0x2,        0x4002b,    0xa,        0x24,       0x1,        0x40020,    0x25,       0x9,        0x14,
    0x4002b,    0x14,       0x28,       0x1,        0x20014,    0x29,       0x4002b,    0x6,        0x2f,
    0x41800000, 0x4002b,    0x14,       0x32,       0x0,        0x40017,    0x33,       0x14,       0x2,
    0x40020,    0x34,       0x9,        0x1d,       0x40020,    0x40,       0x7,        0x6,        0x4002b,
    0xa,        0x42,       0x0,        0x4001c,    0x46,       0x6,        0x24,       0x6001e,    0x47,
    0x1e,       0x6,        0x46,       0x46,       0x40020,    0x48,       0x3,        0x47,       0x4003b,
    0x48,       0x49,       0x3,        0x4002b,    0x6,        0x4e,       0x40000000, 0x40020,    0x5d,
    0x9,        0x6,        0x40020,    0x61,       0x3,        0x1e,       0x40020,    0x63,       0x3,
    0x7,        0x4003b,    0x63,       0x64,       0x3,        0x4002b,    0x6,        0x6b,       0x44800000,
    0x50036,    0x2,        0x4,        0x0,        0x3,        0x200f8,    0x5,        0x4003b,    0x8,
    0x9,        0x7,        0x4003b,    0x18,       0x19,       0x7,        0x4003b,    0x8,        0x1c,
    0x7,        0x4003b,    0x8,        0x2b,       0x7,        0x4003b,    0x40,       0x41,       0x7,
    0x4003d,    0x14,       0x17,       0x16,       0x3003e,    0x19,       0x13,       0x50041,    0x8,
    0x1a,       0x19,       0x17,       0x4003d,    0x7,        0x1b,       0x1a,       0x3003e,    0x9,
    0x1b,       0x60041,    0x25,       0x26,       0x22,       0x23,       0x24,       0x4003d,    0x14,
    0x27,       0x26,       0x500aa,    0x29,       0x2a,       0x27,       0x28,       0x300f7,    0x2d,
    0x0,        0x400fa,    0x2a,       0x2c,       0x31,       0x200f8,    0x2c,       0x4003d,    0x7,
    0x2e,       0x9,        0x5008e,    0x7,        0x30,       0x2e,       0x2f,       0x3003e,    0x2b,
    0x30,       0x200f9,    0x2d,       0x200f8,    0x31,       0x50041,    0x34,       0x35,       0x22,
    0x32,       0x4003d,    0x1d,       0x36,       0x35,       0x7004f,    0x33,       0x37,       0x36,
    0x36,       0x0,        0x1,        0x4006f,    0x7,        0x38,       0x37,       0x50041,    0x34,
    0x39,       0x22,       0x32,       0x4003d,    0x1d,       0x3a,       0x39,       0x7004f,    0x33,
    0x3b,       0x3a,       0x3a,       0x2,        0x3,        0x4006f,    0x7,        0x3c,       0x3b,
    0x4003d,    0x7,        0x3d,       0x9,        0x8000c,    0x7,        0x3e,       0x1,        0x2e,
    0x38,       0x3c,       0x3d,       0x3003e,    0x2b,       0x3e,       0x200f9,    0x2d,       0x200f8,
    0x2d,       0x4003d,    0x7,        0x3f,       0x2b,       0x3003e,    0x1c,       0x3f,       0x60041,
    0x25,       0x43,       0x22,       0x23,       0x42,       0x4003d,    0x14,       0x44,       0x43,
    0x4006f,    0x6,        0x45,       0x44,       0x3003e,    0x41,       0x45,       0x50041,    0x40,
    0x4a,       0x1c,       0x42,       0x4003d,    0x6,        0x4b,       0x4a,       0x4003d,    0x6,
    0x4c,       0x41,       0x50088,    0x6,        0x4d,       0x4b,       0x4c,       0x50085,    0x6,
    0x4f,       0x4d,       0x4e,       0x50083,    0x6,        0x50,       0x4f,       0xf,        0x4003d,
    0x6,        0x51,       0x41,       0x50088,    0x6,        0x52,       0xf,        0x51,       0x50081,
    0x6,        0x53,       0x50,       0x52,       0x50041,    0x40,       0x54,       0x1c,       0x24,
    0x4003d,    0x6,        0x55,       0x54,       0x4003d,    0x6,        0x56,       0x41,       0x50088,
    0x6,        0x57,       0x55,       0x56,       0x50085,    0x6,        0x58,       0x57,       0x4e,
    0x50083,    0x6,        0x59,       0xf,        0x58,       0x4003d,    0x6,        0x5a,       0x41,
    0x50088,    0x6,        0x5b,       0xf,        0x5a,       0x50083,    0x6,        0x5c,       0x59,
    0x5b,       0x60041,    0x5d,       0x5e,       0x22,       0x28,       0x42,       0x4003d,    0x6,
    0x5f,       0x5e,       0x70050,    0x1e,       0x60,       0x53,       0x5c,       0x5f,       0xf,
    0x50041,    0x61,       0x62,       0x49,       0x32,       0x3003e,    0x62,       0x60,       0x50041,
    0x34,       0x65,       0x22,       0x32,       0x4003d,    0x1d,       0x66,       0x65,       0x7004f,
    0x33,       0x67,       0x66,       0x66,       0x0,        0x1,        0x4006f,    0x7,        0x68,
    0x67,       0x50050,    0x7,        0x69,       0xf,        0xf,        0x50081,    0x7,        0x6a,
    0x68,       0x69,       0x50050,    0x7,        0x6c,       0x6b,       0x6b,       0x50088,    0x7,
    0x6d,       0x6a,       0x6c,       0x50041,    0x34,       0x6e,       0x22,       0x32,       0x4003d,
    0x1d,       0x6f,       0x6e,       0x7004f,    0x33,       0x70,       0x6f,       0x6f,       0x2,
    0x3,        0x4006f,    0x7,        0x71,       0x70,       0x50050,    0x7,        0x72,       0x6b,
    0x6b,       0x50088,    0x7,        0x73,       0x71,       0x72,       0x4003d,    0x7,        0x74,
    0x9,        0x8000c,    0x7,        0x75,       0x1,        0x2e,       0x6d,       0x73,       0x74,
    0x3003e,    0x64,       0x75,       0x100fd,    0x10038,
};
static const uint32_t visibilitySampleSpv[] = {
    0x7230203,  0x10300,    0x8000b,    0x74,       0x0,        0x20011,    0x1,        0x6000b,    0x1,
    0x4c534c47, 0x6474732e, 0x3035342e, 0x0,        0x3000e,    0x0,        0x1,        0x8000f,    0x0,
    0x4,        0x6e69616d, 0x0,        0x8,        0xb,        0x60,       0x30003,    0x2,        0x1c2,
    0x40005,    0x4,        0x6e69616d, 0x0,        0x60005,    0x8,        0x7074756f, 0x695f7475, 0x7865646e,
    0x0,        0x60005,    0xb,        0x565f6c67, 0x65747265, 0x646e4978, 0x7865,     0x50005,    0x12,
    0x656c6573, 0x64657463, 0x0,        0x40005,    0x1b,       0x72756f73, 0x6563,     0x30005,    0x20,
    0x50,       0x50006,    0x20,       0x0,        0x74636572, 0x0,        0x50006,    0x20,       0x1,
    0x61746164, 0x0,        0x50006,    0x20,       0x2,        0x666e6f63, 0x6769,     0x50006,    0x20,
    0x3,        0x72747865, 0x61,       0x30005,    0x22,       0x70,       0x30005,    0x49,       0x7870,
    0x50005,    0x58,       0x65646e69, 0x6c626178, 0x65,       0x60005,    0x5e,       0x505f6c67, 0x65567265,
    0x78657472, 0x0,        0x60006,    0x5e,       0x0,        0x505f6c67, 0x7469736f, 0x6e6f69,   0x70006,
    0x5e,       0x1,        0x505f6c67, 0x746e696f, 0x657a6953, 0x0,        0x70006,    0x5e,       0x2,
    0x435f6c67, 0x4470696c, 0x61747369, 0x65636e,   0x70006,    0x5e,       0x3,        0x435f6c67, 0x446c6c75,
    0x61747369, 0x65636e,   0x30005,    0x60,       0x0,        0x30047,    0x8,        0xe,        0x40047,
    0x8,        0x1e,       0x0,        0x40047,    0xb,        0xb,        0x2a,       0x30047,    0x20,
    0x2,        0x50048,    0x20,       0x0,        0x23,       0x0,        0x50048,    0x20,       0x1,
    0x23,       0x10,       0x50048,    0x20,       0x2,        0x23,       0x20,       0x50048,    0x20,
    0x3,        0x23,       0x30,       0x30047,    0x5e,       0x2,        0x50048,    0x5e,       0x0,
    0xb,        0x0,        0x50048,    0x5e,       0x1,        0xb,        0x1,        0x50048,    0x5e,
    0x2,        0xb,        0x3,        0x50048,    0x5e,       0x3,        0xb,        0x4,        0x20013,
    0x2,        0x30021,    0x3,        0x2,        0x40015,    0x6,        0x20,       0x0,        0x40020,
    0x7,        0x3,        0x6,        0x4003b,    0x7,        0x8,        0x3,        0x40015,    0x9,
    0x20,       0x1,        0x40020,    0xa,        0x1,        0x9,        0x4003b,    0xa,        0xb,
    0x1,        0x4002b,    0x9,        0xd,        0x6,        0x40017,    0x10,       0x9,        0x2,
    0x40020,    0x11,       0x7,        0x10,       0x4002b,    0x6,        0x14,       0x10,       0x40017,
    0x1c,       0x9,        0x4,        0x30016,    0x1d,       0x20,       0x40017,    0x1e,       0x1d,
    0x4,        0x40017,    0x1f,       0x6,        0x4,        0x6001e,    0x20,       0x1c,       0x1e,
    0x1c,       0x1f,       0x40020,    0x21,       0x9,        0x20,       0x4003b,    0x21,       0x22,
    0x9,        0x4002b,    0x9,        0x23,       0x0,        0x40020,    0x24,       0x9,        0x1c,
    0x4002b,    0x9,        0x28,       0x1,        0x4002b,    0x9,        0x2b,       0x10,       0x4002b,
    0x9,        0x3c,       0x2,        0x4002b,    0x6,        0x3d,       0x2,        0x40020,    0x3e,
    0x9,        0x9,        0x4002b,    0x6,        0x41,       0x0,        0x40020,    0x42,       0x7,
    0x9,        0x40017,    0x47,       0x1d,       0x2,        0x40020,    0x48,       0x7,        0x47,
    0x4002b,    0x6,        0x4c,       0x6,        0x4001c,    0x4d,       0x47,       0x4c,       0x4002b,
    0x1d,       0x4e,       0x0,        0x5002c,    0x47,       0x4f,       0x4e,       0x4e,       0x4002b,
    0x1d,       0x50,       0x3f800000, 0x5002c,    0x47,       0x51,       0x50,       0x4e,       0x5002c,
    0x47,       0x52,       0x4e,       0x50,       0x5002c,    0x47,       0x53,       0x50,       0x50,
    0x9002c,    0x4d,       0x54,       0x4f,       0x51,       0x52,       0x52,       0x51,       0x53,
    0x40020,    0x57,       0x7,        0x4d,       0x4002b,    0x6,        0x5c,       0x1,        0x4001c,
    0x5d,       0x1d,       0x5c,       0x6001e,    0x5e,       0x1e,       0x1d,       0x5d,       0x5d,
    0x40020,    0x5f,       0x3,        0x5e,       0x4003b,    0x5f,       0x60,       0x3,        0x40020,
    0x61,       0x7,        0x1d,       0x4002b,    0x1d,       0x64,       0x44800000, 0x4002b,    0x1d,
    0x66,       0x40000000, 0x40020,    0x6e,       0x9,        0x1d,       0x40020,    0x72,       0x3,
    0x1e,       0x50036,    0x2,        0x4,        0x0,        0x3,        0x200f8,    0x5,        0x4003b,
    0x11,       0x12,       0x7,        0x4003b,    0x11,       0x1b,       0x7,        0x4003b,    0x48,
    0x49,       0x7,        0x4003b,    0x57,       0x58,       0x7,        0x4003d,    0x9,        0xc,
    0xb,        0x50087,    0x9,        0xe,        0xc,        0xd,        0x4007c,    0x6,        0xf,
    0xe,        0x3003e,    0x8,        0xf,        0x4003d,    0x6,        0x13,       0x8,        0x50089,
    0x6,        0x15,       0x13,       0x14,       0x4007c,    0x9,        0x16,       0x15,       0x4003d,
    0x6,        0x17,       0x8,        0x50086,    0x6,        0x18,       0x17,       0x14,       0x4007c,
    0x9,        0x19,       0x18,       0x50050,    0x10,       0x1a,       0x16,       0x19,       0x3003e,
    0x12,       0x1a,       0x50041,    0x24,       0x25,       0x22,       0x23,       0x4003d,    0x1c,
    0x26,       0x25,       0x7004f,    0x10,       0x27,       0x26,       0x26,       0x0,        0x1,
    0x50050,    0x10,       0x29,       0x28,       0x28,       0x50080,    0x10,       0x2a,       0x27,
    0x29,       0x50050,    0x10,       0x2c,       0x2b,       0x2b,       0x50084,    0x10,       0x2d,
    0x2a,       0x2c,       0x50041,    0x24,       0x2e,       0x22,       0x23,       0x4003d,    0x1c,
    0x2f,       0x2e,       0x7004f,    0x10,       0x30,       0x2f,       0x2f,       0x2,        0x3,
    0x50041,    0x24,       0x31,       0x22,       0x23,       0x4003d,    0x1c,       0x32,       0x31,
    0x7004f,    0x10,       0x33,       0x32,       0x32,       0x0,        0x1,        0x50082,    0x10,
    0x34,       0x30,       0x33,       0x50050,    0x10,       0x35,       0x28,       0x28,       0x50082,
    0x10,       0x36,       0x34,       0x35,       0x4003d,    0x10,       0x37,       0x12,       0x50084,
    0x10,       0x38,       0x36,       0x37,       0x50080,    0x10,       0x39,       0x2d,       0x38,
    0x50050,    0x10,       0x3a,       0x2b,       0x2b,       0x50087,    0x10,       0x3b,       0x39,
    0x3a,       0x3003e,    0x1b,       0x3b,       0x60041,    0x3e,       0x3f,       0x22,       0x3c,
    0x3d,       0x4003d,    0x9,        0x40,       0x3f,       0x50041,    0x42,       0x43,       0x1b,
    0x41,       0x4003d,    0x9,        0x44,       0x43,       0x50080,    0x9,        0x45,       0x44,
    0x40,       0x50041,    0x42,       0x46,       0x1b,       0x41,       0x3003e,    0x46,       0x45,
    0x4003d,    0x10,       0x4a,       0x1b,       0x4006f,    0x47,       0x4b,       0x4a,       0x4003d,
    0x9,        0x55,       0xb,        0x5008b,    0x9,        0x56,       0x55,       0xd,        0x3003e,
    0x58,       0x54,       0x50041,    0x48,       0x59,       0x58,       0x56,       0x4003d,    0x47,
    0x5a,       0x59,       0x50081,    0x47,       0x5b,       0x4b,       0x5a,       0x3003e,    0x49,
    0x5b,       0x50041,    0x61,       0x62,       0x49,       0x41,       0x4003d,    0x1d,       0x63,
    0x62,       0x50088,    0x1d,       0x65,       0x63,       0x64,       0x50085,    0x1d,       0x67,
    0x65,       0x66,       0x50083,    0x1d,       0x68,       0x67,       0x50,       0x50041,    0x61,
    0x69,       0x49,       0x5c,       0x4003d,    0x1d,       0x6a,       0x69,       0x50088,    0x1d,
    0x6b,       0x6a,       0x64,       0x50085,    0x1d,       0x6c,       0x6b,       0x66,       0x50083,
    0x1d,       0x6d,       0x50,       0x6c,       0x60041,    0x6e,       0x6f,       0x22,       0x28,
    0x41,       0x4003d,    0x1d,       0x70,       0x6f,       0x70050,    0x1e,       0x71,       0x68,
    0x6d,       0x70,       0x50,       0x50041,    0x72,       0x73,       0x60,       0x23,       0x3003e,
    0x73,       0x71,       0x100fd,    0x10038,
};
static const uint32_t visibilityMaskSpv[] = {
    0x7230203,  0x10300,    0x8000b,    0x20,       0x0,        0x20011,    0x1,     0x6000b, 0x1,        0x4c534c47,
    0x6474732e, 0x3035342e, 0x0,        0x3000e,    0x0,        0x1,        0x6000f, 0x4,     0x4,        0x6e69616d,
    0x0,        0x9,        0x30010,    0x4,        0x7,        0x30003,    0x2,     0x1c2,   0x40005,    0x4,
    0x6e69616d, 0x0,        0x50005,    0x9,        0x6b73616d, 0x6c6f635f, 0x726f,  0x30005, 0xe,        0x50,
    0x50006,    0xe,        0x0,        0x74636572, 0x0,        0x50006,    0xe,     0x1,     0x61746164, 0x0,
    0x50006,    0xe,        0x2,        0x666e6f63, 0x6769,     0x50006,    0xe,     0x3,     0x72747865, 0x61,
    0x30005,    0x10,       0x70,       0x40047,    0x9,        0x1e,       0x1,     0x30047, 0xe,        0x2,
    0x50048,    0xe,        0x0,        0x23,       0x0,        0x50048,    0xe,     0x1,     0x23,       0x10,
    0x50048,    0xe,        0x2,        0x23,       0x20,       0x50048,    0xe,     0x3,     0x23,       0x30,
    0x20013,    0x2,        0x30021,    0x3,        0x2,        0x30016,    0x6,     0x20,    0x40017,    0x7,
    0x6,        0x4,        0x40020,    0x8,        0x3,        0x7,        0x4003b, 0x8,     0x9,        0x3,
    0x40015,    0xa,        0x20,       0x1,        0x40017,    0xb,        0xa,     0x4,     0x40015,    0xc,
    0x20,       0x0,        0x40017,    0xd,        0xc,        0x4,        0x6001e, 0xe,     0xb,        0x7,
    0xb,        0xd,        0x40020,    0xf,        0x9,        0xe,        0x4003b, 0xf,     0x10,       0x9,
    0x4002b,    0xa,        0x11,       0x1,        0x4002b,    0xc,        0x12,    0x1,     0x40020,    0x13,
    0x9,        0x6,        0x4002b,    0x6,        0x16,       0x0,        0x20014, 0x17,    0x7002c,    0x7,
    0x19,       0x16,       0x16,       0x16,       0x16,       0x4002b,    0x6,     0x1a,    0x3f800000, 0x4002b,
    0x6,        0x1b,       0x3f008081, 0x7002c,    0x7,        0x1c,       0x1a,    0x1a,    0x1a,       0x1b,
    0x40017,    0x1d,       0x17,       0x4,        0x50036,    0x2,        0x4,     0x0,     0x3,        0x200f8,
    0x5,        0x60041,    0x13,       0x14,       0x10,       0x11,       0x12,    0x4003d, 0x6,        0x15,
    0x14,       0x500b4,    0x17,       0x18,       0x15,       0x16,       0x70050, 0x1d,    0x1e,       0x18,
    0x18,       0x18,       0x18,       0x600a9,    0x7,        0x1f,       0x1e,    0x19,    0x1c,       0x3003e,
    0x9,        0x1f,       0x100fd,    0x10038,
};
static const uint32_t visibilityStoreSpv[] = {
    0x7230203,  0x10300,    0x8000b,    0x20,       0x0,        0x20011, 0x1,        0x6000b,    0x1,        0x4c534c47,
    0x6474732e, 0x3035342e, 0x0,        0x3000e,    0x0,        0x1,     0x6000f,    0x4,        0x4,        0x6e69616d,
    0x0,        0x1a,       0x30010,    0x4,        0x7,        0x30010, 0x4,        0x9,        0x30003,    0x2,
    0x1c2,      0x40005,    0x4,        0x6e69616d, 0x0,        0x40005, 0x8,        0x65786950, 0x736c,     0x50006,
    0x8,        0x0,        0x64726f77, 0x73,       0x40005,    0xa,     0x65786970, 0x736c,     0x30005,    0x11,
    0x50,       0x50006,    0x11,       0x0,        0x74636572, 0x0,     0x50006,    0x11,       0x1,        0x61746164,
    0x0,        0x50006,    0x11,       0x2,        0x666e6f63, 0x6769,  0x50006,    0x11,       0x3,        0x72747865,
    0x61,       0x30005,    0x13,       0x70,       0x60005,    0x1a,    0x7074756f, 0x695f7475, 0x7865646e, 0x0,
    0x40047,    0x7,        0x6,        0x4,        0x30047,    0x8,     0x2,        0x40048,    0x8,        0x0,
    0x19,       0x50048,    0x8,        0x0,        0x23,       0x0,     0x30047,    0xa,        0x19,       0x40047,
    0xa,        0x21,       0x1,        0x40047,    0xa,        0x22,    0x0,        0x30047,    0x11,       0x2,
    0x50048,    0x11,       0x0,        0x23,       0x0,        0x50048, 0x11,       0x1,        0x23,       0x10,
    0x50048,    0x11,       0x2,        0x23,       0x20,       0x50048, 0x11,       0x3,        0x23,       0x30,
    0x30047,    0x1a,       0xe,        0x40047,    0x1a,       0x1e,    0x0,        0x20013,    0x2,        0x30021,
    0x3,        0x2,        0x40015,    0x6,        0x20,       0x0,     0x3001d,    0x7,        0x6,        0x3001e,
    0x8,        0x7,        0x40020,    0x9,        0xc,        0x8,     0x4003b,    0x9,        0xa,        0xc,
    0x40015,    0xb,        0x20,       0x1,        0x4002b,    0xb,     0xc,        0x0,        0x40017,    0xd,
    0xb,        0x4,        0x30016,    0xe,        0x20,       0x40017, 0xf,        0xe,        0x4,        0x40017,
    0x10,       0x6,        0x4,        0x6001e,    0x11,       0xd,     0xf,        0xd,        0x10,       0x40020,
    0x12,       0x9,        0x11,       0x4003b,    0x12,       0x13,    0x9,        0x4002b,    0xb,        0x14,
    0x3,        0x4002b,    0x6,        0x15,       0x0,        0x40020, 0x16,       0x9,        0x6,        0x40020,
    0x19,       0x1,        0x6,        0x4003b,    0x19,       0x1a,    0x1,        0x4002b,    0x6,        0x1d,
    0x80ffffff, 0x40020,    0x1e,       0xc,        0x6,        0x50036, 0x2,        0x4,        0x0,        0x3,
    0x200f8,    0x5,        0x60041,    0x16,       0x17,       0x13,    0x14,       0x15,       0x4003d,    0x6,
    0x18,       0x17,       0x4003d,    0x6,        0x1b,       0x1a,    0x50080,    0x6,        0x1c,       0x18,
    0x1b,       0x60041,    0x1e,       0x1f,       0xa,        0xc,     0x1c,       0x3003e,    0x1f,       0x1d,
    0x100fd,    0x10038,
};
struct VisibilityPush
{
    int32_t rect[4]{};
    float data[4]{};
    int32_t config[4]{};
    uint32_t extra[4]{};
};
static_assert(sizeof(VisibilityPush) == 64 && offsetof(VisibilityPush, extra) == 48, "validated SPIR-V push layout");
struct VisibilityOwnedDraw
{
    GfxDraw draw{};
    std::array<uint8_t, 96> vertices{};
    void assign(const GfxDraw& source)
    {
        draw = source;
        std::memcpy(vertices.data(), source.data[0], source.size[0]);
        draw.data[0] = vertices.data();
    }
};
struct VisibilityPacket
{
    enum Kind
    {
        Target,
        Black,
        White,
        Clear,
        Downsample,
        Copy
    } kind = Target;
    gfxplan::Target target{};
};
struct VisibilityJournal
{
    unsigned state = 0, count = 0;
    std::array<VisibilityPacket, 16> packets{};
    std::array<VisibilityOwnedDraw, 3> draws;
    gfxplan::Target scene{}, producer{}, small{}, destination{};
    int rect[4]{};
    float z = 0;
    uint64_t serial = 0, frame = 0;
    VkCommandBuffer command = VK_NULL_HANDLE;
    uint32_t nrects = 0, flags = 0, color = 0, stencil = 0, vp[6]{};
    float clearZ = 0;
    int32_t clearRect[4]{};
    void reset()
    {
        state = count = 0;
        scene = {};
        producer = {};
        small = {};
        destination = {};
    }
    bool append(VisibilityPacket::Kind kind, gfxplan::Target target)
    {
        if (count == packets.size())
            return false;
        packets[count++] = {kind, target};
        return true;
    }
};
static_assert(sizeof(VisibilityJournal) < 32768, "owned journal byte bound");
struct VisibilityState
{
    bool active = false, replaying = false, writesPending = false;
    std::thread::id owner;
    std::unique_ptr<VisibilityPool> versions;
    Buffer arena;
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule quad = VK_NULL_HANDLE, sample = VK_NULL_HANDLE, mask = VK_NULL_HANDLE, store = VK_NULL_HANDLE;
    std::unordered_map<VkRenderPass, std::array<VkPipeline, 3>> pipelines;
    VisibilityJournal journal;
    uint64_t committed = 0, replayed = 0, materialized = 0, poolMisses = 0, farCommitted = 0;
} visibility;
bool visibility_env(const char* name)
{
    const char* p = std::getenv(name);
    return p && p[0] == '1' && !p[1];
}
bool visibility_requested()
{
    static const bool requested = [] {
        bool on = visibility_env("FFXI_ANDROID_VISIBILITY_STORAGE");
        const char* diag = std::getenv("FFXI_ANDROID_PROBE_QUERY_DIAG");
        const char* trace = std::getenv("FFXI_ANDROID_PASS_TRACE");
        if (on && (visibility_env("FFXI_RENDER_WORKER") || !visibility_env("FFXI_ASYNC_READBACK") ||
                   visibility_env("FFXI_ANDROID_PASS_PLAN") || visibility_env("FFXI_ANDROID_BOUNDED_AREA") ||
                   (diag && *diag) || (trace && *trace)))
        {
            std::fprintf(
                stderr,
                "[gfx-vulkan] visibility storage declined: worker0, keyed async1, planner/area/trace/query diagnostic off required\n");
            on = false;
        }
        return on;
    }();
    return requested;
}
void visibility_preamble()
{
    if (!visibility.versions)
        return;
    const size_t count =
        visibility.versions->prepare(g.completedSerial, g.nextSerial, [&](size_t slot, VisibilityPayload& p) {
            p.offset = slot * 1024;
            vkCmdFillBuffer(g.cmd, visibility.arena.buffer, p.offset, 1024, 0);
            return true;
        });
    if (count)
    {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
    }
}
void visibility_pass_ended()
{
    if (!visibility.writesPending)
        return;
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    visibility.writesPending = false;
}
void visibility_materialize(GfxTex* t)
{
    if (!t || !t->visibility)
        return;
    auto ref = t->visibility;
    if (!visibility.versions || !visibility.versions->owns(ref))
        fail("foreign visibility image version");
    if (t->faces != 1 || t->levels != 1 || t->w != 16 || t->h != 16 || t->fmt != 21 || t->gpuBpp != 4)
        fail("visibility image changed allocation shape");
    // Raw transition path: invoking transition() recursively would materialize twice.
    end_pass();
    command();
    image_barrier(t->image, t->aspect, 0, 0, t->layouts[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    t->layouts[0] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkBufferImageCopy copy{};
    copy.bufferOffset = ref->payload().offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {16, 16, 1};
    if (!visibility.versions->touch(ref, g.nextSerial))
        fail("visibility materialization serial regressed");
    vkCmdCopyBufferToImage(g.cmd, visibility.arena.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    t->visibility.reset();
    t->shadowValid[0] = false;
    t->sampledReady = false;
    ++visibility.materialized;
}
void visibility_read_bytes(GfxTex* t, uint32_t level, const VisibilityRef& ref, void* dst, uint32_t pitch)
{
    if (!visibility.versions || !visibility.versions->owns(ref) || !gfxvisibility::completed(ref, g.completedSerial))
        fail("unfinished or foreign visibility host read");
    Buffer view = visibility.arena;
    view.cpu = static_cast<uint8_t*>(view.cpu) + ref->payload().offset;
    view.size = 1024;
    readback_bytes(t, level, view, dst, pitch);
}
bool visibility_queue_keyed(GfxTex* t, KeyedReadback& slot)
{
    if (!t->visibility)
        return false;
    auto ref = t->visibility;
    if (!visibility.versions || !visibility.versions->owns(ref))
        fail("foreign visibility history");
    if (!gfxvisibility::current_producer(ref, g.frameNumber))
    {
        visibility_materialize(t);
        return false;
    }
    slot.visibility = ref;
    slot.transfer.serial = 0;
    slot.issuedFrame = ref->producerFrame();
    return true;
}
void visibility_mutation(GfxTex* t)
{
    if (!t)
        return;
    t->visibility.reset();
    // CPU uploads and destruction poison history admission but retain transfer
    // lifetimes; an unfinished legacy destination cannot suddenly be reused.
    if (visibility.versions)
        for (auto& h : t->keyed)
            for (auto& s : h->slots)
            {
                s.visibility.reset();
                s.issuedFrame = UINT64_MAX;
            }
}
void visibility_abort()
{
    auto& j = visibility.journal;
    if (!j.state || visibility.replaying)
        return;
    visibility.replaying = true;
    // Clear admission first: replay never recaptures itself, including exceptions.
    const unsigned count = j.count;
    j.state = 0;
    ++visibility.replayed;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto& p = j.packets[i];
        guarded([&] {
            physical_target(p.target);
            switch (p.kind)
            {
            case VisibilityPacket::Target:
                break;
            case VisibilityPacket::Black:
                draw(&j.draws[0].draw);
                break;
            case VisibilityPacket::White:
                draw(&j.draws[1].draw);
                break;
            case VisibilityPacket::Downsample:
                draw(&j.draws[2].draw);
                break;
            case VisibilityPacket::Clear:
                clear(j.nrects, j.nrects ? j.clearRect : nullptr, j.flags, j.color, j.clearZ, j.stencil, j.vp);
                break;
            case VisibilityPacket::Copy:
                texture_copy(j.small.color, 0, 0, 0, 0, 16, 16, j.destination.color, 0, 0, 0, 0);
                break;
            }
        });
    }
    j.reset();
    visibility.replaying = false;
}
bool visibility_thread()
{
    if (!visibility.active || visibility.replaying)
        return false;
    if (visibility.owner == std::this_thread::get_id())
        return true;
    visibility_abort();
    visibility.active = false;
    std::fprintf(stderr, "[gfx-vulkan] visibility storage disabled after foreign graphics thread\n");
    return false;
}
// Existing strict effective-state predicates are pure; adapt only their target
// lookup while holding the renderer mutex. Never issue Vulkan within this scope.
struct VisibilityGuardTarget
{
    gfxplan::Target saved;
    explicit VisibilityGuardTarget(gfxplan::Target target) : saved{g.rt, g.rtFace, g.rtLevel, g.ds}
    {
        g.rt = target.color;
        g.rtFace = target.face;
        g.rtLevel = target.level;
        g.ds = target.depth;
    }
    ~VisibilityGuardTarget()
    {
        g.rt = saved.color;
        g.rtFace = saved.face;
        g.rtLevel = saved.level;
        g.ds = saved.depth;
    }
};
bool visibility_quad(const GfxDraw* d, gfxplan::Target target, int rect[4], float& z, uint32_t color,
                     bool allowFarClip = false)
{
    VisibilityGuardTarget scoped(target);
    if (!d)
        return false;
    if (probe_diag_quad(d, rect, z, color))
        return true;
    if (!allowFarClip || !probe_diag_common(d, 1024, false))
        return false;
    // Only the white transaction may extend past the far clip plane. With
    // guarded RHW1 and unchanged depthClampEnable=false, equal finite z>1
    // vertices produce no fragments in either original or custom shaders.
    // Reuse every strict predicate through an owned GUARD-ONLY z1 snapshot;
    // original UP bytes and the z emitted by visibility_commit stay untouched.
    std::array<uint8_t, 80> vertices{};
    std::memcpy(vertices.data(), d->data[0], vertices.size());
    float farZ = 0;
    std::memcpy(&farZ, vertices.data() + 8, sizeof farZ);
    if (!std::isfinite(farZ) || farZ <= 1)
        return false;
    const float guardZ = 1;
    for (unsigned i = 0; i < 4; ++i)
    {
        float value = 0;
        std::memcpy(&value, vertices.data() + i * 20 + 8, sizeof value);
        if (value != farZ)
            return false;
        std::memcpy(vertices.data() + i * 20 + 8, &guardZ, sizeof guardZ);
    }
    GfxDraw guardedDraw = *d;
    guardedDraw.data[0] = vertices.data();
    float admittedZ = 0;
    if (!probe_diag_quad(&guardedDraw, rect, admittedZ, color))
        return false;
    z = farZ;
    return true;
}
bool visibility_small(GfxTex* t)
{
    return probe_diag_target(t, 16);
}
bool visibility_scene()
{
    return g.pass && g.rt && g.ds && g.rt->use == GFX_USE_RT && g.rt->native == VK_FORMAT_B8G8R8A8_UNORM &&
           g.rt->w == 1024 && g.rt->h == 1024 && g.rt->faces == 1 && g.rt->levels == 1 && !g.rtFace && !g.rtLevel &&
           g.ds->use == GFX_USE_DEPTH && g.ds->fmt == 75 && g.ds->w == 1024 && g.ds->h == 1024 && g.ds->levels == 1 &&
           g.ds->faces == 1 &&
           (g.ds->native == VK_FORMAT_D24_UNORM_S8_UINT || g.ds->native == VK_FORMAT_D32_SFLOAT_S8_UINT) &&
           !g.passArea.offset.x && !g.passArea.offset.y && g.passArea.extent.width == 1024 &&
           g.passArea.extent.height == 1024;
}
bool visibility_target(gfxplan::Target target)
{
    if (!visibility_thread())
        return false;
    auto& j = visibility.journal;
    if (j.state && !j.append(VisibilityPacket::Target, target))
        visibility_abort();
    return true;
}
bool visibility_draw(const GfxDraw* d)
{
    if (!visibility_thread() || !d)
        return false;
    auto& j = visibility.journal;
    int rect[4];
    float z = 0;
    if (j.state == 1 && logicalTarget.color == j.producer.color && logicalTarget.depth == j.producer.depth &&
        d->depth.zenable == 1 && d->depth.zfunc == 4 && visibility_quad(d, logicalTarget, rect, z, 0x80ffffffu, true) &&
        !std::memcmp(rect, j.rect, sizeof rect))
    {
        if (!j.append(VisibilityPacket::White, logicalTarget))
        {
            visibility_abort();
            return false;
        }
        j.draws[1].assign(*d);
        j.z = z;
        j.state = 2;
        return true;
    }
    if (j.state == 3 && logicalTarget.color == j.small.color && !logicalTarget.depth)
    {
        bool match = false;
        {
            VisibilityGuardTarget scoped(logicalTarget);
            match = probe_diag_common(d, 16, true) && !d->depth.zenable && d->tex[0] == j.producer.color;
        }
        if (match && probe_diag_downsample_vertices(d, j.rect))
        {
            if (!j.append(VisibilityPacket::Downsample, logicalTarget))
            {
                visibility_abort();
                return false;
            }
            j.draws[2].assign(*d);
            j.state = 4;
            return true;
        }
    }
    if (j.state)
    {
        visibility_abort();
        return false;
    }
    if (!passPlan.empty() || !visibility_scene() || logicalTarget.color == g.rt || logicalTarget.depth != g.ds ||
        d->depth.zenable || !visibility_quad(d, logicalTarget, rect, z, 0) || z != 0)
        return false;
    j.reset();
    j.scene = {g.rt, g.rtFace, g.rtLevel, g.ds};
    j.producer = logicalTarget;
    j.serial = g.nextSerial;
    j.frame = g.frameNumber;
    j.command = g.cmd;
    std::copy(rect, rect + 4, j.rect);
    j.draws[0].assign(*d);
    j.append(VisibilityPacket::Black, logicalTarget);
    j.state = 1;
    return true;
}
bool visibility_clear(uint32_t n, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
                      const uint32_t vp[6])
{
    if (!visibility_thread())
        return false;
    auto& j = visibility.journal;
    bool whole = !n || (n == 1 && rects && !rects[0] && !rects[1] && rects[2] == 16 && rects[3] == 16);
    if (j.state == 2 && visibility_small(logicalTarget.color) && !logicalTarget.face && !logicalTarget.level &&
        !logicalTarget.depth && flags == 1 && !color && whole && vp && !vp[0] && !vp[1] && vp[2] == 16 && vp[3] == 16)
    {
        if (!j.append(VisibilityPacket::Clear, logicalTarget))
        {
            visibility_abort();
            return false;
        }
        j.small = logicalTarget;
        j.nrects = n;
        j.flags = flags;
        j.color = color;
        j.clearZ = z;
        j.stencil = stencil;
        std::memcpy(j.vp, vp, sizeof j.vp);
        if (n)
            std::memcpy(j.clearRect, rects, sizeof j.clearRect);
        j.state = 3;
        return true;
    }
    visibility_abort();
    return false;
}
bool visibility_copy(GfxTex* src, uint32_t sf, uint32_t sl, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                     GfxTex* dst, uint32_t df, uint32_t dl, uint32_t dx, uint32_t dy)
{
    if (!visibility_thread())
        return false;
    auto& j = visibility.journal;
    const bool full = src != dst && visibility_small(src) && visibility_small(dst) &&
                      !(sf | sl | sx | sy | df | dl | dx | dy) && w == 16 && h == 16;
    if (j.state == 4 && src == j.small.color && full)
    {
        if (!j.append(VisibilityPacket::Copy, logicalTarget))
        {
            visibility_abort();
            return false;
        }
        j.destination = {dst, 0, 0, nullptr};
        j.state = 5;
        return true;
    }
    if (j.state)
    {
        visibility_abort();
        return false;
    }
    if (full && src->visibility)
    {
        end_pass();
        dst->visibility = src->visibility;
        dst->shadowValid[0] = false;
        dst->sampledReady = false;
        return true;
    }
    return false;
}
VkPipeline visibility_pipeline(VkRenderPass pass, unsigned kind)
{
    auto found = visibility.pipelines.find(pass);
    if (found != visibility.pipelines.end() && found->second[kind])
        return found->second[kind];
    VkPipelineShaderStageCreateInfo sh[2]{};
    sh[0].sType = sh[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    sh[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    sh[0].module = kind == 2 ? visibility.sample : visibility.quad;
    sh[0].pName = "main";
    sh[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    sh[1].module = kind == 2 ? visibility.store : visibility.mask;
    sh[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input.topology = kind == 2 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = kind != 0;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachments[2]{};
    attachments[1].colorWriteMask = kind == 2 ? 0 : 15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 2;
    blend.pAttachments = attachments;
    VkDynamicState states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo c{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    c.stageCount = 2;
    c.pStages = sh;
    c.pVertexInputState = &vertex;
    c.pInputAssemblyState = &input;
    c.pViewportState = &viewport;
    c.pRasterizationState = &raster;
    c.pMultisampleState = &samples;
    c.pDepthStencilState = &depth;
    c.pColorBlendState = &blend;
    c.pDynamicState = &dynamic;
    c.layout = visibility.layout;
    c.renderPass = pass;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g.device, g.pipelineCache, 1, &c, nullptr, &p));
    visibility.pipelines[pass][kind] = p;
    return p;
}
bool visibility_commit(GfxTex* t, uint32_t face, uint32_t level, uint32_t pitch, uint64_t key, uint32_t maxAge)
{
    auto& j = visibility.journal;
    if (j.state != 5 || !t || t != j.destination.color || face || level || pitch < 64 || !key || !maxAge ||
        maxAge > 64 || g.frameNumber != j.frame || g.nextSerial != j.serial || g.cmd != j.command ||
        g.rt != j.scene.color || g.ds != j.scene.depth || !visibility_scene())
        return false;
    KeyedHistory* history = nullptr;
    for (auto& h : t->keyed)
        if (h->key == key && !h->face && !h->level)
        {
            history = h.get();
            break;
        }
    if (history && history->lastReadFrame == g.frameNumber)
        return false;
    // All potentially allocating setup precedes committed candidate writes.
    if (!history)
    {
        if (t->keyed.size() >= KEYED_HISTORIES)
            return false;
        auto h = std::make_unique<KeyedHistory>();
        h->key = key;
        t->keyed.push_back(std::move(h));
    }
    auto version = visibility.versions->acquire(g.frameNumber, g.nextSerial);
    if (!version)
    {
        ++visibility.poolMisses;
        return false;
    }
    auto* mask = j.producer.color;
    VkRenderPass pass = render_pass(g.rt, g.ds, false, false, mask);
    framebuffer_for(g.rt, g.ds, mask, pass, 0, 0, 1024, 1024);
    const VkPipeline black = visibility_pipeline(pass, 0), white = visibility_pipeline(pass, 1),
                     store = visibility_pipeline(pass, 2);
    VisibilityPush p{};
    std::copy(j.rect, j.rect + 4, p.rect);
    p.config[0] = 1024;
    p.extra[0] = uint32_t(version->payload().offset / 4);
    // Opening the compatible pass may still allocate cache keys; no candidate
    // pixels have been written and exact replay remains valid through that step.
    if (g.visibilityMask != mask)
    {
        end_pass();
        transition(mask, 0, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        g.visibilityMask = mask;
    }
    if (!begin_pass())
        fail("visibility commit lost scene target");
    // No fallible setup remains. From here, never replay original probe geometry.
    j.state = 0;
    VkViewport viewport{0, 1024, 1024, -1024, 0, 1};
    VkRect2D scissor{{0, 0}, {1024, 1024}};
    vkCmdSetViewport(g.cmd, 0, 1, &viewport);
    vkCmdSetScissor(g.cmd, 0, 1, &scissor);
    auto emit = [&](VkPipeline pipe, unsigned vertices) {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdPushConstants(g.cmd, visibility.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof p, &p);
        vkCmdDraw(g.cmd, vertices, 1, 0, 0);
    };
    emit(black, 4);
    p.data[0] = j.z;
    p.data[1] = 1;
    emit(white, 4);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, visibility.layout, 0, 1, &visibility.descriptor, 0,
                            nullptr);
    emit(store, 256 * 6);
    visibility.writesPending = true;
    g.prof.draws += 3;
    mask->shadowValid[0] = false;
    mask->sampledReady = false;
    j.small.color->visibility = version;
    j.small.color->shadowValid[0] = false;
    j.small.color->sampledReady = false;
    t->visibility = version;
    t->shadowValid[0] = false;
    t->sampledReady = false;
    g.boundPipeline = VK_NULL_HANDLE;
    g.encodedViewportValid = g.encodedScissorValid = g.encodedStencilValid = g.encodedIndexValid = false;
    if (j.z > 1)
        ++visibility.farCommitted;
    j.reset();
    ++visibility.committed;
    if (visibility.committed == 1)
        std::fprintf(
            stderr,
            "[gfx-vulkan] visibility storage first commit: real mask MRT, full256 BGRA, fresh producer frame/serial retained\n");
    return true;
}
bool visibility_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch, uint64_t key, uint32_t maxAge)
{
    if (!visibility_thread() || !visibility.journal.state || !dst)
        return false;
    try
    {
        if (!visibility_commit(t, face, level, pitch, key, maxAge))
        {
            visibility_abort();
            return false;
        }
    }
    catch (const std::exception& e)
    {
        if (!visibility.journal.state)
            throw; // A committed transaction must not replay.
        visibility_abort();
        std::fprintf(stderr, "[gfx-vulkan] visibility precommit fallback: %s\n", e.what());
        return false;
    }
    texture_read_keyed(t, face, level, dst, pitch, key, maxAge);
    return true;
}
void visibility_stats_log()
{
    if (!visibility_env("FFXI_ANDROID_VISIBILITY_STORAGE_DIAGNOSTICS"))
        return;
    std::fprintf(
        stderr,
        "[gfx-vulkan] visibility-storage diagnostics: active=%d committed=%llu replayed=%llu materialized=%llu pool_misses=%llu far_clipped_committed=%llu; instrumentation FPS excluded\n",
        visibility.active, (unsigned long long)visibility.committed, (unsigned long long)visibility.replayed,
        (unsigned long long)visibility.materialized, (unsigned long long)visibility.poolMisses,
        (unsigned long long)visibility.farCommitted);
}
void visibility_init()
{
    if (!visibility_requested())
        return;
    const auto& limits = g.props.limits;
    if (!g.features.fragmentStoresAndAtomics || !g.features.independentBlend || limits.maxColorAttachments < 2 ||
        limits.maxPushConstantsSize < 64 || limits.maxStorageBufferRange < 512 * 1024 ||
        limits.maxPerStageDescriptorStorageBuffers < 1 || limits.maxDescriptorSetStorageBuffers < 1 ||
        limits.maxFragmentCombinedOutputResources < 3 || limits.maxImageDimension2D < 1024 ||
        limits.maxFramebufferWidth < 1024 || limits.maxFramebufferHeight < 1024)
    {
        std::fprintf(stderr, "[gfx-vulkan] visibility storage unsupported by actual enabled device features/limits\n");
        return;
    }
    visibility.owner = std::this_thread::get_id();
    visibility.arena = make_buffer(512 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkDescriptorSetLayoutBinding binding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                         nullptr};
    VkDescriptorSetLayoutCreateInfo l{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    l.bindingCount = 1;
    l.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(g.device, &l, nullptr, &visibility.descriptors));
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &visibility.descriptors;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(g.device, &pl, nullptr, &visibility.layout));
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    VK_CHECK(vkCreateDescriptorPool(g.device, &dp, nullptr, &visibility.descriptorPool));
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = visibility.descriptorPool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &visibility.descriptors;
    VK_CHECK(vkAllocateDescriptorSets(g.device, &da, &visibility.descriptor));
    VkDescriptorBufferInfo buffer{visibility.arena.buffer, 0, visibility.arena.size};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = visibility.descriptor;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
    auto shader = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule result;
        VK_CHECK(vkCreateShaderModule(g.device, &ci, nullptr, &result));
        return result;
    };
    visibility.quad = shader(visibilityQuadSpv, sizeof visibilityQuadSpv);
    visibility.sample = shader(visibilitySampleSpv, sizeof visibilitySampleSpv);
    visibility.mask = shader(visibilityMaskSpv, sizeof visibilityMaskSpv);
    visibility.store = shader(visibilityStoreSpv, sizeof visibilityStoreSpv);
    visibility.versions = std::make_unique<VisibilityPool>(1);
    visibility.active = true;
    command();
    submit(true);
    std::fprintf(stderr,
                 "[gfx-vulkan] visibility storage=1: preserved mask/full256, worker0, slots512; experimental\n");
}
#else
bool visibility_requested()
{
    return false;
}
void visibility_init() {}
void visibility_preamble() {}
void visibility_pass_ended() {}
void visibility_stats_log() {}
void visibility_materialize(GfxTex*) {}
void visibility_abort() {}
void visibility_mutation(GfxTex*) {}
bool visibility_target(gfxplan::Target)
{
    return false;
}
bool visibility_draw(const GfxDraw*)
{
    return false;
}
bool visibility_clear(uint32_t, const int32_t*, uint32_t, uint32_t, float, uint32_t, const uint32_t[6])
{
    return false;
}
bool visibility_copy(GfxTex*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, GfxTex*, uint32_t, uint32_t,
                     uint32_t, uint32_t)
{
    return false;
}
bool visibility_read(GfxTex*, uint32_t, uint32_t, void*, uint32_t, uint64_t, uint32_t)
{
    return false;
}
#endif
#endif
