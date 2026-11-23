#pragma once
// Backend-only C++ contract for the opt-in worker mailbox. No guest pointers.
#include "gfx_probe_mailbox.h"
struct GfxTex;
namespace gfxprobe
{
enum class IssueStatus
{
    Queued,
    Busy,
    Unsupported,
    Failed
};
struct Completion
{
    Request request;
    Pixels pixels{};
    uint64_t completedSerial = 0;
};
constexpr unsigned TRANSFER_CAPACITY = 128;
}
extern "C"
{
    gfxprobe::IssueStatus gfx_backend_probe_issue(GfxTex*, uint32_t face, uint32_t mip, const gfxprobe::Request&);
    // Poll returns completed owned snapshots, never submits or waits. -1 is failure.
    int gfx_backend_probe_poll(gfxprobe::Completion*, uint32_t capacity);
    // Current pixels only; true means the owned output was completely initialized.
    bool gfx_backend_probe_exact(GfxTex*, uint32_t face, uint32_t mip, uint64_t key, gfxprobe::Pixels&);
    // Called after queue drain on enabled shutdown. Completes/retire owned transfers.
    bool gfx_backend_probe_shutdown();
}
