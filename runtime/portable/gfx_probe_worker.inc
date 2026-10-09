/* Bounded fresh GPU transfers for FFXI_RENDER_WORKER_MAILBOX. Included once after
 * Vulkan helpers, outside the anonymous namespace. Never retains a GfxTex after
 * issue and never publishes historical keyed-read bytes under a new token. */
#if defined(FFXI_ANDROID_VULKAN) && defined(FFXI_RENDER_WORKER_BUILD)
namespace
{
struct WorkerProbeTransfer
{
    Readback transfer;
    gfxprobe::Request request;
    bool pending = false;
};
std::array<WorkerProbeTransfer, gfxprobe::TRANSFER_CAPACITY> workerProbeTransfers;
bool worker_probe_supported(const GfxTex* t, uint32_t face, uint32_t mip)
{
    return t && !face && !mip && t->type == GFX_TEX_2D && t->fmt == 21 && t->w == 16 && t->h == 16 && t->levels == 1 &&
           t->faces == 1 && t->use == GFX_USE_RT && !t->compressed && t->gpuBpp == 4 &&
           t->native == VK_FORMAT_B8G8R8A8_UNORM;
}
}
extern "C" gfxprobe::IssueStatus gfx_backend_probe_issue(GfxTex* t, uint32_t face, uint32_t mip,
                                                         const gfxprobe::Request& request)
{
    using Status = gfxprobe::IssueStatus;
    return guarded_value<Status>(Status::Failed, [&] {
        if (!g.device || !request || !worker_probe_supported(t, face, mip))
            return Status::Unsupported;
        flush_pass_plan();
        trace_read(t, face, mip, "keyed");
        probe_diag_read(t, "keyed", request.key);
        // Producer lifetime poisoning must exclude every CPU-current source.
        if (t->shadowValid[0])
            fail("worker mailbox CPU-current transfer escaped producer poison");
        for (auto& slot : workerProbeTransfers)
            if (!slot.pending)
            {
                queue_readback(t, face, mip, slot.transfer);
                slot.request = request;
                slot.pending = true;
                return Status::Queued;
            }
        return Status::Busy;
    });
}
extern "C" int gfx_backend_probe_poll(gfxprobe::Completion* out, uint32_t capacity)
{
    return guarded_value<int>(-1, [&] {
        if (!out || !capacity || !g.device)
            return 0;
        collect();
        unsigned count = 0;
        for (auto& slot : workerProbeTransfers)
        {
            if (count == capacity)
                break;
            if (!slot.pending || !slot.transfer.serial || slot.transfer.serial > g.completedSerial)
                continue;
            auto& result = out[count++];
            result.request = slot.request;
            result.completedSerial = g.completedSerial;
            // The strict format21/BGRA8 whitelist has byte-identical D3D pixels.
            std::memcpy(result.pixels.data(), slot.transfer.b.cpu, result.pixels.size());
            slot.pending = false;
            slot.request = {};
        }
        return int(count);
    });
}
extern "C" bool gfx_backend_probe_exact(GfxTex* t, uint32_t face, uint32_t mip, uint64_t key, gfxprobe::Pixels& out)
{
    return guarded_value<bool>(false, [&] {
        if (!g.device || !worker_probe_supported(t, face, mip))
            return false;
        flush_pass_plan();
        trace_read(t, face, mip, "keyed");
        probe_diag_read(t, "keyed", key);
        texture_read(t, face, mip, out.data(), 64, false);
        return true;
    });
}
extern "C" bool gfx_backend_probe_shutdown()
{
    return guarded_value<bool>(false, [&] {
        bool allocated = false;
        for (const auto& slot : workerProbeTransfers)
            allocated |= slot.transfer.b.buffer != VK_NULL_HANDLE;
        if (!allocated)
            return true;
        flush_pass_plan();
        submit(true);
        VK_CHECK(vkDeviceWaitIdle(g.device));
        g.completedSerial = g.nextSerial - 1;
        collect();
        for (auto& slot : workerProbeTransfers)
        {
            destroy_buffer(slot.transfer.b);
            slot = {};
        }
        return true;
    });
}
#endif
