/* A one-frame dependency trace for gfx_vulkan.cpp, as JSON lines: targets, passes, draws, clears,
 * copies, uploads and reads, with resources as small ids. CPU-side metadata only: no GPU commands,
 * waits, pixels or pointer values. Included once GfxTex is complete, outside any namespace.
 *
 * FFXI_ANDROID_PASS_TRACE=<file> names the output. Creating <file>.arm traces the next frame (Present
 * to Present); the .arm file is removed, and <file> is written only once that frame is captured. A
 * traced frame's timing is not representative. */
#include <cstdarg>

namespace
{
struct PassTrace
{
    bool configured = false, active = false, overflow = false;
    uint64_t frame = 0, sequence = 0, draws = 0, passes = 0;
    uint32_t nextId = 1;
    std::string path, data;
    std::unordered_map<const GfxTex*, uint32_t> textures;
    std::unordered_map<const GfxBuf*, uint32_t> buffers;
};
PassTrace passTrace;
constexpr size_t PASS_TRACE_BYTE_CAP = 16u << 20;
constexpr uint64_t PASS_TRACE_EVENT_CAP = 16384;

// One record. Past the size or event cap the trace stops, and its footer says it overflowed.
void trace_line(const char* format, ...)
{
    if (!passTrace.active || passTrace.overflow)
        return;
    char text[4096];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (n < 0 || size_t(n) >= sizeof text || passTrace.data.size() + size_t(n) > PASS_TRACE_BYTE_CAP ||
        passTrace.sequence >= PASS_TRACE_EVENT_CAP)
    {
        passTrace.overflow = true;
        return;
    }
    try
    {
        passTrace.data.append(text, size_t(n));
    }
    catch (...)
    {
        passTrace.overflow = true;
    }
}
uint32_t trace_texture_id(const GfxTex* t)
{
    if (!t)
        return 0;
    auto found = passTrace.textures.find(t);
    if (found != passTrace.textures.end())
        return found->second;
    uint32_t id = passTrace.nextId++;
    try
    {
        passTrace.textures.emplace(t, id);
    }
    catch (...)
    {
        passTrace.overflow = true;
        return 0;
    }
    trace_line(
        "{\"event\":\"texture\",\"id\":%u,\"w\":%u,\"h\":%u,\"levels\":%u,\"faces\":%u,\"format\":%u,\"use\":%d,\"type\":%d}\n",
        id, t->w, t->h, t->levels, t->faces, t->fmt, t->use, t->type);
    return id;
}
uint32_t trace_buffer_id(const GfxBuf* b)
{
    if (!b)
        return 0;
    auto found = passTrace.buffers.find(b);
    if (found != passTrace.buffers.end())
        return found->second;
    uint32_t id = passTrace.nextId++;
    try
    {
        passTrace.buffers.emplace(b, id);
    }
    catch (...)
    {
        passTrace.overflow = true;
        return 0;
    }
    trace_line("{\"event\":\"buffer\",\"id\":%u,\"bytes\":%u}\n", id, b->size);
    return id;
}
void trace_draw_active(const GfxDraw* d)
{
    if (!passTrace.active || !d)
        return;
    uint32_t rt = trace_texture_id(g.rt), ds = trace_texture_id(g.ds), samples[8]{}, bound[8]{}, bufs[4]{};
    // The textures draw() binds: what the draw may read (a pixel shader need not sample them all).
    for (unsigned i = 0; i < 8; ++i)
    {
        bound[i] = trace_texture_id(d->tex[i]);
        if (stage_texture(d, i))
            samples[i] = bound[i];
    }
    for (unsigned i = 0; i < 4; ++i)
        bufs[i] = trace_buffer_id(d->buf[i]);
    uint32_t ib = trace_buffer_id(d->ibuf);
    trace_line(
        "{\"event\":\"draw\",\"seq\":%llu,\"draw\":%llu,\"rt\":%u,\"depth\":%u,\"face\":%u,\"level\":%u,"
        "\"samples\":[%u,%u,%u,%u,%u,%u,%u,%u],\"bound\":[%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"vs\":%u,\"fs\":%u,\"vs_words\":%u,\"fs_words\":%u,\"rhw\":%u,\"projected\":[%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"stages\":%u,\"blend\":[%u,%u,%u,%u],\"write_mask\":%u,\"z\":[%u,%u,%u],"
        "\"stencil\":[%u,%u,%u,%u,%u,%u,%u,%u],\"vp_bits\":[%u,%u,%u,%u,%u,%u],\"scissor\":[%d,%d,%d,%d],"
        "\"prim\":%u,\"count\":%u,\"vertex_start\":%u,\"fill\":%u,\"cull\":%u,\"zbias\":%d,"
        "\"buffers\":[%u,%u,%u,%u],\"buffer_offsets\":[%u,%u,%u,%u],\"inline_bytes\":[%u,%u,%u,%u],\"ibuf\":%u,\"index_size\":%u,\"ibuf_offset\":%u}\n",
        (unsigned long long)++passTrace.sequence, (unsigned long long)++passTrace.draws, rt, ds, g.rtFace, g.rtLevel,
        samples[0], samples[1], samples[2], samples[3], samples[4], samples[5], samples[6], samples[7], bound[0],
        bound[1], bound[2], bound[3], bound[4], bound[5], bound[6], bound[7], d->vs.prog, d->fs.prog, d->vs_token_count,
        d->ps_token_count, d->vs.rhw, d->fs.st[0].projected, d->fs.st[1].projected, d->fs.st[2].projected,
        d->fs.st[3].projected, d->fs.st[4].projected, d->fs.st[5].projected, d->fs.st[6].projected,
        d->fs.st[7].projected, d->fs.nstages, d->pipe.blend, d->pipe.src, d->pipe.dst, d->pipe.op, d->pipe.write_mask,
        d->depth.zenable, d->depth.zwrite, d->depth.zfunc, d->depth.stencil, d->depth.sfail, d->depth.szfail,
        d->depth.spass, d->depth.sfunc, d->depth.sread, d->depth.swrite, d->stencil_ref, d->vp[0], d->vp[1], d->vp[2],
        d->vp[3], d->vp[4], d->vp[5], d->scissor[0], d->scissor[1], d->scissor[2], d->scissor[3], d->prim, d->count,
        d->vertex_start, d->fill, d->cull, d->zbias, bufs[0], bufs[1], bufs[2], bufs[3], d->buf_off[0], d->buf_off[1],
        d->buf_off[2], d->buf_off[3], d->size[0], d->size[1], d->size[2], d->size[3], ib, d->index_size, d->ibuf_off);
}
inline void trace_draw(const GfxDraw* d)
{
    if (passTrace.active)
        trace_draw_active(d);
}
void trace_pass()
{
    if (!passTrace.active)
        return;
    uint32_t rt = trace_texture_id(g.rt), ds = trace_texture_id(g.ds);
    trace_line(
        "{\"event\":\"pass\",\"seq\":%llu,\"pass\":%llu,\"rt\":%u,\"depth\":%u,\"face\":%u,\"level\":%u,\"area\":[%d,%d,%u,%u]}\n",
        (unsigned long long)++passTrace.sequence, (unsigned long long)++passTrace.passes, rt, ds, g.rtFace, g.rtLevel,
        g.passArea.offset.x, g.passArea.offset.y, g.passArea.extent.width, g.passArea.extent.height);
}
// Resource creation and non-resource observations can flush a pass plan without
// showing up as an upload. A boundary identifies those scheduling barriers.
void trace_boundary(const char* reason)
{
    if (!passTrace.active)
        return;
    trace_line("{\"event\":\"boundary\",\"seq\":%llu,\"reason\":\"%s\"}\n", (unsigned long long)++passTrace.sequence,
               reason);
}
void trace_target(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (!passTrace.active)
        return;
    uint32_t from = trace_texture_id(g.rt), fromDepth = trace_texture_id(g.ds), to = trace_texture_id(color),
             toDepth = trace_texture_id(depth);
    trace_line(
        "{\"event\":\"target\",\"seq\":%llu,\"from\":%u,\"from_depth\":%u,\"to\":%u,\"depth\":%u,\"face\":%u,\"level\":%u}\n",
        (unsigned long long)++passTrace.sequence, from, fromDepth, to, toDepth, face, level);
}
void trace_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
                 const uint32_t vp[6])
{
    if (!passTrace.active)
        return;
    uint32_t rt = trace_texture_id(g.rt), ds = trace_texture_id(g.ds), zbits;
    std::memcpy(&zbits, &z, sizeof zbits);
    trace_line(
        "{\"event\":\"clear\",\"seq\":%llu,\"rt\":%u,\"depth\":%u,\"face\":%u,\"level\":%u,\"flags\":%u,\"color\":%u,\"z_bits\":%u,\"stencil\":%u,\"rect_count\":%u,\"vp_bits\":[%u,%u,%u,%u,%u,%u]}\n",
        (unsigned long long)++passTrace.sequence, rt, ds, g.rtFace, g.rtLevel, flags, color, zbits, stencil, nrects,
        vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
    if (nrects && !rects)
    {
        passTrace.overflow = true;
        return;
    }
    for (uint32_t i = 0; i < nrects && !passTrace.overflow; ++i)
        trace_line("{\"event\":\"clear_rect\",\"clear_seq\":%llu,\"index\":%u,\"rect\":[%d,%d,%d,%d]}\n",
                   (unsigned long long)passTrace.sequence, i, rects[4 * i], rects[4 * i + 1], rects[4 * i + 2],
                   rects[4 * i + 3]);
}
void trace_upload(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!passTrace.active)
        return;
    uint32_t id = trace_texture_id(t);
    trace_line("{\"event\":\"upload\",\"seq\":%llu,\"tex\":%u,\"face\":%u,\"level\":%u,\"rect\":[%u,%u,%u,%u]}\n",
               (unsigned long long)++passTrace.sequence, id, face, level, x, y, w, h);
}
void trace_copy(GfxTex* src, uint32_t sf, uint32_t sl, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
                uint32_t df, uint32_t dl, uint32_t dx, uint32_t dy)
{
    if (!passTrace.active)
        return;
    uint32_t from = trace_texture_id(src), to = trace_texture_id(dst);
    trace_line(
        "{\"event\":\"copy\",\"seq\":%llu,\"src\":%u,\"src_face\":%u,\"src_level\":%u,\"src_xy\":[%u,%u],\"dst\":%u,\"dst_face\":%u,\"dst_level\":%u,\"dst_xy\":[%u,%u],\"size\":[%u,%u]}\n",
        (unsigned long long)++passTrace.sequence, from, sf, sl, sx, sy, to, df, dl, dx, dy, w, h);
}
void trace_read(GfxTex* t, uint32_t face, uint32_t level, const char* mode)
{
    if (!passTrace.active)
        return;
    uint32_t id = trace_texture_id(t);
    trace_line("{\"event\":\"read\",\"seq\":%llu,\"tex\":%u,\"face\":%u,\"level\":%u,\"mode\":\"%s\"}\n",
               (unsigned long long)++passTrace.sequence, id, face, level, mode);
}
void trace_buffer_write(GfxBuf* b, uint32_t bytes)
{
    if (!passTrace.active)
        return;
    uint32_t id = trace_buffer_id(b);
    trace_line("{\"event\":\"buffer_write\",\"seq\":%llu,\"buffer\":%u,\"bytes\":%u}\n",
               (unsigned long long)++passTrace.sequence, id, bytes);
}
void trace_texture_destroy(GfxTex* t)
{
    if (!passTrace.active || !t)
        return;
    uint32_t id = trace_texture_id(t);
    trace_line("{\"event\":\"texture_destroy\",\"seq\":%llu,\"tex\":%u}\n", (unsigned long long)++passTrace.sequence,
               id);
    passTrace.textures.erase(t);
}
void trace_buffer_destroy(GfxBuf* b)
{
    if (!passTrace.active || !b)
        return;
    uint32_t id = trace_buffer_id(b);
    trace_line("{\"event\":\"buffer_destroy\",\"seq\":%llu,\"buffer\":%u}\n", (unsigned long long)++passTrace.sequence,
               id);
    passTrace.buffers.erase(b);
}
void trace_present()
{
    if (!passTrace.configured)
    {
        passTrace.configured = true;
        const char* p = std::getenv("FFXI_ANDROID_PASS_TRACE");
        if (p && *p)
            passTrace.path = p;
    }
    if (passTrace.path.empty())
        return;
    if (passTrace.active)
    {
        std::FILE* out = std::fopen(passTrace.path.c_str(), "wb");
        bool ok = false;
        if (out)
        {
            size_t wrote = std::fwrite(passTrace.data.data(), 1, passTrace.data.size(), out);
            int footer = std::fprintf(
                out,
                "{\"event\":\"complete\",\"frame\":%llu,\"events\":%llu,\"draws\":%llu,\"passes\":%llu,\"overflow\":%s}\n",
                (unsigned long long)passTrace.frame, (unsigned long long)passTrace.sequence,
                (unsigned long long)passTrace.draws, (unsigned long long)passTrace.passes,
                passTrace.overflow ? "true" : "false");
            bool outputOk = wrote == passTrace.data.size() && footer > 0 && !std::ferror(out);
            ok = std::fclose(out) == 0 && outputOk;
        }
        std::fprintf(stderr,
                     "[gfx-vulkan] pass trace frame %llu: %llu draws, overflow=%d, output=%s; diagnostic timing only\n",
                     (unsigned long long)passTrace.frame, (unsigned long long)passTrace.draws, passTrace.overflow,
                     ok ? "ok" : "FAILED");
        passTrace.active = false;
        passTrace.data.clear();
        passTrace.textures.clear();
        passTrace.buffers.clear();
    }
    std::string arm = passTrace.path + ".arm";
    std::FILE* request = std::fopen(arm.c_str(), "rb");
    if (!request)
        return;
    std::fclose(request);
    if (std::remove(arm.c_str()) != 0)
        return;
    try
    {
        passTrace.data.reserve(PASS_TRACE_BYTE_CAP);
        passTrace.textures.reserve(2048);
        passTrace.buffers.reserve(2048);
        passTrace.active = true;
        passTrace.overflow = false;
        passTrace.frame = g.frameNumber;
        passTrace.sequence = 0;
        passTrace.draws = 0;
        passTrace.passes = 0;
        passTrace.nextId = 1;
        trace_line(
            "{\"event\":\"metadata\",\"schema\":1,\"frame\":%llu,\"instrumented_fps_excluded\":true,\"gpu_waits_added\":false,\"read_semantics\":\"potential effective descriptor reads\",\"event_cap\":%llu,\"byte_cap\":%zu}\n",
            (unsigned long long)passTrace.frame, (unsigned long long)PASS_TRACE_EVENT_CAP, PASS_TRACE_BYTE_CAP);
        trace_target(g.rt, g.rtFace, g.rtLevel, g.ds);
    }
    catch (...)
    {
        passTrace.active = false;
        std::fprintf(stderr, "[gfx-vulkan] pass trace allocation failed\n");
    }
}
} // namespace
