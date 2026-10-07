/* Android-only, default-off one-frame parity observer. Never supplies game pixels.
 * FFXI_ANDROID_PROBE_QUERY_DIAG=/owned/path.jsonl; create path.jsonl.arm once.
 * Query and exact CURRENT image copy share a transaction; historical async game
 * output is only labelled and is never used for the comparison. No added waits.
 * Included after texture_copy so existing allocation/submission helpers are in scope.
 */
#if defined(FFXI_ANDROID_VULKAN)
constexpr unsigned PROBE_DIAG_CAP = 64;
constexpr size_t PROBE_DIAG_BYTES = 1u << 20;
struct ProbeDiagTransaction
{
    uint64_t frame = 0, copyFrame = 0, querySerial = 0, copySerial = 0;
    GfxTex *producer = nullptr, *depth = nullptr, *small = nullptr, *destination = nullptr;
    int rect[4]{};
    float z = 0;
    unsigned state = 0; // 0 query,1 downsample,2 copied,3 skipped,4 compared
};
struct ProbeDiagState
{
    bool configured = false, active = false, used = false, draining = false, overflow = false, black = false,
         smallClear = false;
    std::string path, log;
    uint64_t frame = 0, event = 0, resetSerial = 0;
    unsigned count = 0, matches = 0, mismatches = 0, skipped = 0, observations = 0;
    GfxTex *blackTarget = nullptr, *blackDepth = nullptr, *clearedSmall = nullptr;
    int blackRect[4]{};
    int pending = -1;
    std::array<ProbeDiagTransaction, PROBE_DIAG_CAP> transactions;
    Buffer pixels;
    VkQueryPool pool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    std::unordered_map<std::string, VkPipeline> pipelines;
    std::unordered_map<GfxTex*, uint32_t> textureIds;
    uint32_t nextId = 1;
} probeDiag;
bool probe_diag_requested()
{
    try
    {
        if (!probeDiag.configured)
        {
            probeDiag.configured = true;
            const char* p = std::getenv("FFXI_ANDROID_PROBE_QUERY_DIAG");
            if (p && *p)
                probeDiag.path = p;
        }
    }
    catch (...)
    {
        probeDiag.path.clear();
        std::fprintf(stderr, "[gfx-vulkan] probe query diagnostic path allocation failed; disabled\n");
    }
    return !probeDiag.path.empty();
}
void probe_diag_line(const char* format, ...)
{
    if (probeDiag.overflow)
        return;
    char line[8192];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0 || size_t(n) >= sizeof line || probeDiag.log.size() + size_t(n) > PROBE_DIAG_BYTES)
    {
        probeDiag.overflow = true;
        return;
    }
    // Observer output must never abort an original clear, upload or game read.
    try
    {
        probeDiag.log.append(line, size_t(n));
    }
    catch (...)
    {
        probeDiag.overflow = true;
    }
}
uint32_t probe_diag_id(GfxTex* t)
{
    if (!t)
        return 0;
    auto found = probeDiag.textureIds.find(t);
    if (found != probeDiag.textureIds.end())
        return found->second;
    uint32_t id = probeDiag.nextId++;
    probeDiag.textureIds.emplace(t, id);
    return id;
}
std::string probe_diag_hex(const void* p, size_t n)
{
    const char h[] = "0123456789abcdef";
    const auto* v = static_cast<const uint8_t*>(p);
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        s += h[v[i] >> 4];
        s += h[v[i] & 15];
    }
    return s;
}
void probe_diag_snapshot(const char* role, const GfxDraw* d, unsigned tx)
{
    auto vs = probe_diag_hex(&d->vs, sizeof d->vs), fs = probe_diag_hex(&d->fs, sizeof d->fs),
         pipe = probe_diag_hex(&d->pipe, sizeof d->pipe), depth = probe_diag_hex(&d->depth, sizeof d->depth),
         sampler = probe_diag_hex(&d->samp[0], sizeof d->samp[0]), vertices = probe_diag_hex(d->data[0], d->size[0]);
    probe_diag_line(
        "{\"event\":\"draw_inputs\",\"sequence\":%llu,\"role\":\"%s\",\"transaction\":%u,\"rt\":%u,\"depth_rt\":%u,\"native_color_format\":%u,\"native_depth_format\":%u,\"vs_hex\":\"%s\",\"fs_hex\":\"%s\",\"pipe_hex\":\"%s\",\"depth_hex\":\"%s\",\"sampler0_hex\":\"%s\",\"vertices_hex\":\"%s\",\"u_vp\":[%.9g,%.9g,%.9g,%.9g],\"vofs\":%d,\"stride0\":%d,\"offsets\":[%d,%d,%d]}\n",
        (unsigned long long)probeDiag.event, role, tx, probe_diag_id(g.rt), probe_diag_id(g.ds), unsigned(g.rt->native),
        unsigned(g.ds ? g.ds->native : VK_FORMAT_UNDEFINED), vs.c_str(), fs.c_str(), pipe.c_str(), depth.c_str(),
        sampler.c_str(), vertices.c_str(), double(d->u.vp[0]), double(d->u.vp[1]), double(d->u.vp[2]),
        double(d->u.vp[3]), d->u.vofs, d->u.stride[0], d->u.offset[0], d->u.offset[5], d->u.offset[7]);
}
void probe_diag_skip(const char* reason)
{
    if (probeDiag.pending >= 0)
    {
        auto& tx = probeDiag.transactions[unsigned(probeDiag.pending)];
        tx.state = 3;
        ++probeDiag.skipped;
        probe_diag_line("{\"event\":\"skipped\",\"transaction\":%d,\"reason\":\"%s\"}\n", probeDiag.pending, reason);
    }
    probeDiag.pending = -1;
    probeDiag.black = false;
    probeDiag.smallClear = false;
    probeDiag.clearedSmall = nullptr;
}
void probe_diag_mutation(GfxTex* t, const char* reason)
{
    if (!probeDiag.active)
        return;
    ++probeDiag.event;
    bool relevant = probeDiag.black && (t == probeDiag.blackTarget || t == probeDiag.blackDepth);
    if (probeDiag.pending >= 0)
    {
        const auto& p = probeDiag.transactions[unsigned(probeDiag.pending)];
        relevant |= t == p.producer || t == p.depth || t == p.small;
    }
    if (relevant)
        probe_diag_skip(reason);
    if (!std::strcmp(reason, "destroy"))
    {
        probeDiag.textureIds.erase(t);
        for (unsigned i = 0; i < probeDiag.count; ++i)
            if (probeDiag.transactions[i].destination == t)
                probeDiag.transactions[i].destination = nullptr;
    }
}
bool probe_diag_target(const GfxTex* t, unsigned extent)
{
    return t && t->w == extent && t->h == extent && t->levels == 1 && t->faces == 1 && t->type == GFX_TEX_2D &&
           t->use == GFX_USE_RT && t->fmt == 21 && t->native == VK_FORMAT_B8G8R8A8_UNORM && t->gpuBpp == 4;
}
bool probe_diag_common(const GfxDraw* d, unsigned extent, bool down)
{
    if (!probe_diag_target(g.rt, extent) || g.rtFace || g.rtLevel || d->prim != GFX_TRIANGLESTRIP || d->count != 2 ||
        d->vertex_start || d->indices || d->ibuf || d->vs.prog || d->fs.prog || d->vs_token_count ||
        d->ps_token_count || !d->vs.rhw || d->vs.lighting || d->vs.specular || d->vs.fog_vertex || d->vs.range_fog ||
        d->vs.pixel || d->vs.shadow || d->vs.water || d->fs.fog || d->fs.specular_add || d->fs.water ||
        (d->fs.alpha_func && d->fs.alpha_func != 8) || d->pipe.blend || d->pipe.write_mask != 15 || d->depth.stencil ||
        d->depth.zwrite || d->zbias || d->cull != 1 || d->fill != 3 || d->scissor[2] || d->scissor[3] || d->u.vofs)
        return false;
    const unsigned stride = down ? 24 : 20;
    if (!d->data[0] || d->size[0] != stride * 4 || d->u.stride[0] != int(stride))
        return false;
    for (unsigned s = 0; s < 4; ++s)
        if (d->buf[s] || d->buf_off[s] || (s && (d->data[s] || d->size[s])))
            return false;
    for (unsigned r = 0; r < GFX_NREGS; ++r)
    {
        const auto& e = d->vs.el[r];
        if (!e.used)
            continue;
        if (e.stream)
            return false;
        if (r == GFX_R_POSITION)
        {
            if (e.type != GFX_FLOAT4 || d->u.offset[r] != 0)
                return false;
        }
        else if (r == (down ? GFX_R_TEXCOORD0 : GFX_R_DIFFUSE))
        {
            if (e.type != (down ? GFX_FLOAT2 : GFX_D3DCOLOR) || d->u.offset[r] != 16)
                return false;
        }
        else
            return false;
    }
    if (!d->vs.el[GFX_R_POSITION].used || !d->vs.el[down ? GFX_R_TEXCOORD0 : GFX_R_DIFFUSE].used)
        return false;
    if (d->vp[0] || d->vp[1] || d->vp[2] != extent || d->vp[3] != extent || d->vp[4] || d->vp[5] != 0x3f800000u ||
        d->u.vp[0] != 0 || d->u.vp[1] != 0 || d->u.vp[2] != float(extent) || d->u.vp[3] != float(extent))
        return false;
    if (d->fs.nstages != 1)
        return false;
    const auto& s = d->fs.st[0];
    if (s.cop != 2 || s.aop != 2 || s.ca1 != (down ? 2 : 0) || s.aa1 != (down ? 2 : 0) || s.projected ||
        (s.result != 0 && s.result != 1))
        return false;
    if (down && (s.tex != 1 || s.ncoord != 2 || d->vs.ntex < 1 || d->vs.tci[0] || d->vs.ttf[0] || d->samp[0].mag != 1 ||
                 d->samp[0].min != 1 || d->samp[0].addr_u != 3 || d->samp[0].addr_v != 3))
        return false;
    return true;
}
bool probe_diag_quad(const GfxDraw* d, int rect[4], float& z, uint32_t color)
{
    if (!probe_diag_common(d, 1024, false) || !g.ds || g.ds->w != 1024 || g.ds->h != 1024 || g.ds->fmt != 75 ||
        g.ds->levels != 1 || g.ds->faces != 1)
        return false;
    float v[4][4];
    const auto* b = static_cast<const uint8_t*>(d->data[0]);
    for (unsigned i = 0; i < 4; ++i)
    {
        std::memcpy(v[i], b + i * 20, 16);
        uint32_t c;
        std::memcpy(&c, b + i * 20 + 16, 4);
        if (c != color || v[i][3] != 1 || !std::isfinite(v[i][0]) || !std::isfinite(v[i][1]) || !std::isfinite(v[i][2]))
            return false;
    }
    float bounds[4] = {v[0][0], v[0][1], v[3][0], v[3][1]};
    for (unsigned i = 0; i < 4; ++i)
    {
        if (bounds[i] < 0 || bounds[i] > 1024 || std::floor(bounds[i]) != bounds[i])
            return false;
        rect[i] = int(bounds[i]);
    }
    if (rect[2] - rect[0] < 2 || rect[3] - rect[1] < 2)
        return false;
    const int xs[4] = {rect[0], rect[2], rect[0], rect[2]}, ys[4] = {rect[1], rect[1], rect[3], rect[3]};
    z = v[0][2];
    if (z < 0 || z > 1)
        return false;
    for (unsigned i = 0; i < 4; ++i)
        if (v[i][0] != float(xs[i]) || v[i][1] != float(ys[i]) || v[i][2] != z)
            return false;
    return true;
}
// The 16x16 downsample quad samples exactly the interior of the probe rectangle.
bool probe_diag_downsample_vertices(const GfxDraw* d, const int rect[4])
{
    float v[4][6];
    std::memcpy(v, d->data[0], sizeof v);
    const float xs[4] = {0, 16, 0, 16}, ys[4] = {0, 0, 16, 16};
    const float us[4] = {float(rect[0] + 1) / 1024, float(rect[2]) / 1024, float(rect[0] + 1) / 1024,
                         float(rect[2]) / 1024};
    const float vs[4] = {float(rect[1] + 1) / 1024, float(rect[1] + 1) / 1024, float(rect[3]) / 1024,
                         float(rect[3]) / 1024};
    for (unsigned i = 0; i < 4; ++i)
        if (v[i][0] != xs[i] || v[i][1] != ys[i] || v[i][2] != 0 || v[i][3] != 1 || v[i][4] != us[i] ||
            v[i][5] != vs[i])
            return false;
    return true;
}
bool probe_diag_downsample(const GfxDraw* d, const ProbeDiagTransaction& tx)
{
    if (!probe_diag_common(d, 16, true) || g.ds || d->depth.zenable || d->tex[0] != tx.producer ||
        !probeDiag.smallClear || probeDiag.clearedSmall != g.rt)
        return false;
    return probe_diag_downsample_vertices(d, tx.rect);
}
static const char PROBE_DIAG_SHADER[] = R"HLSL(
[[vk::push_constant]] cbuffer QueryParameters { int4 rect; float4 data; };
float4 query_vs(uint vertex : SV_VertexID) : SV_Position {
    static const float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    uint sampleIndex=vertex/6;int2 selected=int2(sampleIndex%8,sampleIndex/8)*2;
    int2 source=((rect.xy+1)*16+(rect.zw-rect.xy-1)*selected)/16;
    float2 p=float2(source)+corners[vertex%6];
    return float4(p.x/1024.0*2.0-1.0,1.0-p.y/1024.0*2.0,data.x,1.0);
}
float4 query_fs() : SV_Target0 { return float4(1,1,1,1); }
)HLSL";
VkPipeline probe_diag_pipeline()
{
    auto key = bytes_key(&g.activePass, sizeof g.activePass);
    auto found = probeDiag.pipelines.find(key);
    if (found != probeDiag.pipelines.end())
        return found->second;
    VkPipelineShaderStageCreateInfo shaders[2]{};
    shaders[0].sType = shaders[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaders[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaders[0].module = probeDiag.vs;
    shaders[0].pName = "query_vs";
    shaders[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaders[1].module = probeDiag.fs;
    shaders[1].pName = "query_fs";
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multi{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo c{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    c.stageCount = 2;
    c.pStages = shaders;
    c.pVertexInputState = &vertex;
    c.pInputAssemblyState = &input;
    c.pViewportState = &viewport;
    c.pRasterizationState = &raster;
    c.pMultisampleState = &multi;
    c.pDepthStencilState = &depth;
    c.pColorBlendState = &blend;
    c.pDynamicState = &ds;
    c.layout = probeDiag.layout;
    c.renderPass = g.activePass;
    VkPipeline result;
    VK_CHECK(vkCreateGraphicsPipelines(g.device, g.pipelineCache, 1, &c, nullptr, &result));
    probeDiag.pipelines.emplace(key, result);
    return result;
}
void probe_diag_draw(const GfxDraw* d)
{
    if (!probeDiag.active)
        return;
    try
    {
        ++probeDiag.event;
        // Bounded raw inputs also explain strict guard rejection; never treat
        // such an observation as an admitted transaction or query result.
        if (probeDiag.observations < PROBE_DIAG_CAP && g.rt && d->vs.rhw && d->prim == GFX_TRIANGLESTRIP &&
            d->count == 2 && d->data[0] && !d->buf[0] &&
            ((g.rt->w == 1024 && g.rt->h == 1024 && g.ds && d->size[0] == 80) ||
             (g.rt->w == 16 && g.rt->h == 16 && !g.ds && d->size[0] == 96)))
        {
            ++probeDiag.observations;
            probe_diag_snapshot("observed-unadmitted", d, PROBE_DIAG_CAP);
        }
        if (probeDiag.pending >= 0)
        {
            auto& tx = probeDiag.transactions[unsigned(probeDiag.pending)];
            if (tx.state == 0 && probe_diag_downsample(d, tx))
            {
                tx.small = g.rt;
                tx.state = 1;
                probe_diag_snapshot("downsample", d, unsigned(probeDiag.pending));
                return;
            }
            probe_diag_skip("intervening-or-unsupported-draw");
        }
        int rect[4];
        float z;
        if (!d->depth.zenable && probe_diag_quad(d, rect, z, 0) && z == 0)
        {
            probeDiag.black = true;
            probeDiag.blackTarget = g.rt;
            probeDiag.blackDepth = g.ds;
            std::memcpy(probeDiag.blackRect, rect, sizeof rect);
            probe_diag_snapshot("black", d, probeDiag.count);
            return;
        }
        bool candidate = probeDiag.black;
        probeDiag.black = false;
        if (candidate && probeDiag.count >= PROBE_DIAG_CAP)
        {
            probeDiag.overflow = true;
            return;
        }
        if (!candidate || probeDiag.overflow || d->depth.zenable != 1 || d->depth.zfunc != 4 ||
            !probe_diag_quad(d, rect, z, 0x80ffffffu) || g.rt != probeDiag.blackTarget ||
            g.ds != probeDiag.blackDepth || std::memcmp(rect, probeDiag.blackRect, sizeof rect))
            return;
        if (!g.pass || g.passArea.offset.x || g.passArea.offset.y || g.passArea.extent.width != 1024 ||
            g.passArea.extent.height != 1024)
            return;
        unsigned index = probeDiag.count++;
        auto& tx = probeDiag.transactions[index];
        tx.frame = g.frameNumber;
        tx.querySerial = g.nextSerial;
        tx.producer = g.rt;
        tx.depth = g.ds;
        std::copy(rect, rect + 4, tx.rect);
        tx.z = z;
        probeDiag.pending = int(index);
        probeDiag.smallClear = false;
        probe_diag_snapshot("white", d, index);
        struct Params
        {
            int rect[4];
            float data[4];
        } params{};
        std::copy(rect, rect + 4, params.rect);
        params.data[0] = z;
        auto pipeline = probe_diag_pipeline();
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        g.boundPipeline = VK_NULL_HANDLE;
        vkCmdPushConstants(g.cmd, probeDiag.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof params, &params);
        vkCmdBeginQuery(g.cmd, probeDiag.pool, index, VK_QUERY_CONTROL_PRECISE_BIT);
        vkCmdDraw(g.cmd, 64 * 6, 1, 0, 0);
        vkCmdEndQuery(g.cmd, probeDiag.pool, index);
        // A diagnostic pipeline has its own static/dynamic declarations.
        // Re-emit all production dynamic state on the next original draw.
        g.encodedViewportValid = g.encodedScissorValid = g.encodedStencilValid = g.encodedIndexValid = false;
        probe_diag_line(
            "{\"event\":\"query_issued\",\"transaction\":%u,\"issuing_frame\":%llu,\"query_submission_serial\":%llu,\"rect\":[%d,%d,%d,%d],\"z\":%.9g,\"sample_count\":64,\"inside_existing_pass\":true,\"color_write_mask\":0,\"depth_write\":false}\n",
            index, (unsigned long long)tx.frame, (unsigned long long)tx.querySerial, rect[0], rect[1], rect[2], rect[3],
            double(z));
    }
    catch (const std::exception& e)
    {
        g.boundPipeline = VK_NULL_HANDLE;
        probe_diag_skip("diagnostic-exception");
        probeDiag.active = false;
        probeDiag.draining = true;
        probeDiag.overflow = true;
        std::fprintf(stderr, "[gfx-vulkan] probe query diagnostic failed: %s; game readback unchanged\n", e.what());
    }
}
void probe_diag_clear(uint32_t n, const int32_t* rects, uint32_t flags, uint32_t color, const uint32_t vp[6])
{
    if (!probeDiag.active)
        return;
    ++probeDiag.event;
    if (probeDiag.pending >= 0)
    {
        auto& tx = probeDiag.transactions[unsigned(probeDiag.pending)];
        bool whole = !n || (n == 1 && rects && rects[0] == 0 && rects[1] == 0 && rects[2] == 16 && rects[3] == 16);
        if (tx.state == 0 && probe_diag_target(g.rt, 16) && !g.ds && flags == 1 && color == 0 && whole && !vp[0] &&
            !vp[1] && vp[2] == 16 && vp[3] == 16)
        {
            probeDiag.smallClear = true;
            probeDiag.clearedSmall = g.rt;
            return;
        }
        probe_diag_skip("unexpected-clear");
    }
    probeDiag.black = false;
}
void probe_diag_copy(GfxTex* src, uint32_t sf, uint32_t sl, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                     GfxTex* dst, uint32_t df, uint32_t dl, uint32_t dx, uint32_t dy)
{
    if (!probeDiag.active)
        return;
    try
    {
        ++probeDiag.event;
        if (probeDiag.pending < 0)
        {
            probeDiag.black = false;
            return;
        }
        unsigned index = unsigned(probeDiag.pending);
        auto& tx = probeDiag.transactions[index];
        if (tx.state != 1 || src != tx.small || src == dst || sf || sl || sx || sy || df || dl || dx || dy || w != 16 ||
            h != 16 || !probe_diag_target(dst, 16) || src->layouts[0] != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        {
            probe_diag_skip("unexpected-copy");
            return;
        }
        VkBufferImageCopy copy{};
        copy.bufferOffset = index * 1024;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {16, 16, 1};
        vkCmdCopyImageToBuffer(g.cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probeDiag.pixels.buffer, 1,
                               &copy);
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = probeDiag.pixels.buffer;
        b.offset = index * 1024;
        b.size = 1024;
        vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &b, 0,
                             nullptr);
        tx.copyFrame = g.frameNumber;
        tx.copySerial = g.nextSerial;
        tx.destination = dst;
        tx.state = 2;
        probeDiag.pending = -1;
        probeDiag.smallClear = false;
        probe_diag_line(
            "{\"event\":\"current_reference_copy\",\"transaction\":%u,\"copy_frame\":%llu,\"copy_submission_serial\":%llu,\"source\":%u,\"original_destination\":%u,\"diagnostic_buffer_offset\":%u,\"bytes\":1024,\"historical_game_output_used\":false}\n",
            index, (unsigned long long)g.frameNumber, (unsigned long long)tx.copySerial, probe_diag_id(src),
            probe_diag_id(dst), index * 1024);
    }
    catch (const std::exception& e)
    {
        probe_diag_skip("diagnostic-copy-exception");
        probeDiag.active = false;
        probeDiag.draining = true;
        probeDiag.overflow = true;
        std::fprintf(stderr, "[gfx-vulkan] query reference capture failed: %s\n", e.what());
    }
}
void probe_diag_read(GfxTex* t, const char* mode, uint64_t key)
{
    if (!probeDiag.active)
        return;
    for (unsigned i = probeDiag.count; i > 0; --i)
    {
        const auto& tx = probeDiag.transactions[i - 1];
        if (tx.state == 2 && tx.destination == t)
        {
            probe_diag_line(
                "{\"event\":\"original_game_read\",\"transaction\":%u,\"mode\":\"%s\",\"lifetime_key\":\"%016llx\",\"game_output_untouched_by_diagnostic\":true,\"not_used_as_reference\":true}\n",
                i - 1, mode, (unsigned long long)key);
            break;
        }
    }
}
void probe_diag_release()
{
    for (auto p : probeDiag.pipelines)
        vkDestroyPipeline(g.device, p.second, nullptr);
    probeDiag.pipelines.clear();
    if (probeDiag.vs)
        vkDestroyShaderModule(g.device, probeDiag.vs, nullptr);
    if (probeDiag.fs)
        vkDestroyShaderModule(g.device, probeDiag.fs, nullptr);
    if (probeDiag.layout)
        vkDestroyPipelineLayout(g.device, probeDiag.layout, nullptr);
    if (probeDiag.pool)
        vkDestroyQueryPool(g.device, probeDiag.pool, nullptr);
    destroy_buffer(probeDiag.pixels);
    probeDiag.pixels = {};
    probeDiag.vs = probeDiag.fs = VK_NULL_HANDLE;
    probeDiag.layout = VK_NULL_HANDLE;
    probeDiag.pool = VK_NULL_HANDLE;
}
void probe_diag_write(bool unsupported = false)
{
    auto* out = std::fopen(probeDiag.path.c_str(), "wb");
    bool ok = false;
    if (out)
    {
        size_t n = std::fwrite(probeDiag.log.data(), 1, probeDiag.log.size(), out);
        int footer = std::fprintf(
            out,
            "{\"event\":\"complete\",\"capture_frame\":%llu,\"queries\":%u,\"matches\":%u,\"mismatches\":%u,\"skipped\":%u,\"overflow\":%s,\"unsupported\":%s,\"all_diagnostic_submissions_completed\":true,\"completed_serial\":%llu,\"instrumented_fps_excluded\":true,\"game_readback_policy_unchanged\":true}\n",
            (unsigned long long)probeDiag.frame, probeDiag.count, probeDiag.matches, probeDiag.mismatches,
            probeDiag.skipped, probeDiag.overflow ? "true" : "false", unsupported ? "true" : "false",
            (unsigned long long)g.completedSerial);
        bool written = n == probeDiag.log.size() && footer > 0 && !std::ferror(out);
        ok = std::fclose(out) == 0 && written;
    }
    std::fprintf(stderr,
                 "[gfx-vulkan] probe query diagnostic: %u matches/%u mismatches/%u skipped, output=%s; FPS excluded\n",
                 probeDiag.matches, probeDiag.mismatches, probeDiag.skipped, ok ? "ok" : "FAILED");
    probeDiag.log.clear();
    probeDiag.draining = false;
}
void probe_diag_present()
{
    if (!probe_diag_requested() || (!probeDiag.active && !probeDiag.draining && probeDiag.used))
        return;
    try
    {
        if (probeDiag.active)
        {
            probe_diag_skip("capture-frame-ended");
            probeDiag.active = false;
            probeDiag.draining = true;
        }
        if (probeDiag.draining)
        {
            collect();
            uint64_t last = probeDiag.resetSerial;
            for (unsigned i = 0; i < probeDiag.count; ++i)
            {
                const auto& tx = probeDiag.transactions[i];
                last = std::max({last, tx.querySerial, tx.copySerial});
            }
            if (last > g.completedSerial)
                return;
            for (unsigned i = 0; i < probeDiag.count; ++i)
            {
                auto& tx = probeDiag.transactions[i];
                if (tx.state != 2)
                    continue;
                uint64_t count = 0;
                VK_CHECK(vkGetQueryPoolResults(g.device, probeDiag.pool, i, 1, sizeof count, &count, sizeof count,
                                               VK_QUERY_RESULT_64_BIT));
                const auto* bytes = static_cast<const uint8_t*>(probeDiag.pixels.cpu) + i * 1024;
                uint64_t mask = 0;
                unsigned reference = 0, nonbinary = 0;
                for (unsigned y = 0; y < 16; ++y)
                    for (unsigned x = 0; x < 16; ++x)
                    {
                        auto* v = bytes + 4 * (y * 16 + x);
                        bool black = !v[0] && !v[1] && !v[2] && !v[3],
                             white = v[0] == 255 && v[1] == 255 && v[2] == 255 && v[3] == 128;
                        if (!black && !white)
                            ++nonbinary;
                        if (!(x & 1) && !(y & 1) && (v[0] & 128))
                        {
                            ++reference;
                            mask |= 1ull << ((y / 2) * 8 + x / 2);
                        }
                    }
                bool equal =
                    count == reference && !nonbinary && tx.frame == probeDiag.frame && tx.copyFrame == tx.frame;
                if (equal)
                    ++probeDiag.matches;
                else
                    ++probeDiag.mismatches;
                tx.state = 4;
                probe_diag_line(
                    "{\"event\":\"comparison\",\"transaction\":%u,\"issuing_frame\":%llu,\"query_serial\":%llu,\"current_copy_serial\":%llu,\"completed_serial\":%llu,\"query_count\":%llu,\"reference_count\":%u,\"reference_return\":%u,\"reference_mask\":\"%016llx\",\"nonbinary_pixels\":%u,\"equal\":%s}\n",
                    i, (unsigned long long)tx.frame, (unsigned long long)tx.querySerial,
                    (unsigned long long)tx.copySerial, (unsigned long long)g.completedSerial, (unsigned long long)count,
                    reference, reference * 4, (unsigned long long)mask, nonbinary, equal ? "true" : "false");
            }
            probe_diag_release();
            probe_diag_write();
            return;
        }
        std::string arm = probeDiag.path + ".arm";
        auto* request = std::fopen(arm.c_str(), "rb");
        if (!request)
            return;
        std::fclose(request);
        if (std::remove(arm.c_str()))
            return;
        probeDiag.used = true;
        probeDiag.frame = g.frameNumber + 1;
        probeDiag.log.reserve(PROBE_DIAG_BYTES);
        probe_diag_line(
            "{\"event\":\"metadata\",\"schema\":1,\"capture_frame\":%llu,\"transaction_cap\":64,\"byte_cap\":%zu,\"occlusionQueryPrecise_enabled\":%s,\"gpu_waits_added\":false,\"gpu_submissions_added\":false,\"instrumented_fps_excluded\":true,\"reference\":\"current GPU16x16 copy, never historical async output\",\"caller_identity_guard\":\"effective renderer state only; no replacement authorized\"}\n",
            (unsigned long long)probeDiag.frame, PROBE_DIAG_BYTES, g.features.occlusionQueryPrecise ? "true" : "false");
        if (!g.features.occlusionQueryPrecise)
        {
            std::fprintf(
                stderr,
                "[gfx-vulkan] probe query diagnostic unsupported: native occlusionQueryPrecise unavailable or disabled\n");
            probe_diag_line(
                "{\"event\":\"unsupported\",\"reason\":\"actual precise occlusion query feature unavailable or disabled\"}\n");
            probe_diag_write(true);
            return;
        }
        probeDiag.pixels = make_buffer(PROBE_DIAG_CAP * 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_OCCLUSION;
        q.queryCount = PROBE_DIAG_CAP;
        VK_CHECK(vkCreateQueryPool(g.device, &q, nullptr, &probeDiag.pool));
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo l{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        l.pushConstantRangeCount = 1;
        l.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(g.device, &l, nullptr, &probeDiag.layout));
        probeDiag.vs = shader_module(PROBE_DIAG_SHADER, "query_vs", true);
        probeDiag.fs = shader_module(PROBE_DIAG_SHADER, "query_fs", false);
        end_pass();
        command();
        vkCmdResetQueryPool(g.cmd, probeDiag.pool, 0, PROBE_DIAG_CAP);
        probeDiag.resetSerial = g.nextSerial;
        probeDiag.active = true;
        std::fprintf(
            stderr,
            "[gfx-vulkan] probe query diagnostic armed one frame, precise feature enabled, game readback unchanged; FPS excluded\n");
    }
    catch (const std::exception& e)
    {
        probeDiag.active = false;
        probeDiag.draining = true;
        probeDiag.overflow = true;
        probe_diag_skip("diagnostic-present-exception");
        std::fprintf(stderr, "[gfx-vulkan] probe query diagnostic exception: %s\n", e.what());
    }
}
#else
bool probe_diag_requested()
{
    return false;
}
void probe_diag_draw(const GfxDraw*) {}
void probe_diag_clear(uint32_t, const int32_t*, uint32_t, uint32_t, const uint32_t[6]) {}
void probe_diag_copy(GfxTex*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, GfxTex*, uint32_t, uint32_t,
                     uint32_t, uint32_t)
{
}
void probe_diag_mutation(GfxTex*, const char*) {}
void probe_diag_read(GfxTex*, const char*, uint64_t) {}
void probe_diag_present() {}
#endif
