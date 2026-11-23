/* drawcost on deko3d: one device and queue, one big data block (uniforms, vertices, indices, descriptors,
 * command memory), one block for shader code (uam's .dksh from romfs) and one for images, a command buffer
 * recorded each frame into one of two slices of command memory.
 *
 * The draw: dkCmdBufBindUniformBuffer (vertex stage, binding 0: the draw's 256-byte slice of the big buffer),
 * dkCmdBufBindTexture (fragment stage, binding 0: image i, sampler 0 from the descriptor sets), dkCmdBufBindVtxBuffer
 * (buffer 0: one of 8 places in the data block), dkCmdBufBindShaders when the program changes, dkCmdBufDrawIndexed. */
#include "common.h"

#include <deko3d.h>
#include <string.h>
#include <switch.h>

#define FB_NUM 2
#define CMD_SLICE (4u << 20)
#define INIT_CMD (64u << 10)
#define ALIGN(x, a) (((x) + (a) - 1) & ~((uint32_t)(a) - 1))

static DkDevice g_dev;
static DkQueue g_q;
static DkMemBlock g_img_mem, g_code_mem, g_data_mem;
static DkImage g_fb[FB_NUM], g_tex[DC_NTEX];
static DkSwapchain g_sc;
static DkShader g_vs[DC_NPROG], g_fs[DC_NPROG];
static DkCmdBuf g_cb;
static DkFence g_fence[2];
static uint8_t* g_data_cpu;
static DkGpuAddr g_data_gpu;
/* places in the data block */
static uint32_t o_ubo, o_vb[DC_NVB], o_ib, o_imgdesc, o_sampdesc, o_stage, o_initcmd, o_cmd;
static int g_errors;

static void debug_cb(void* user, const char* context, DkResult result, const char* message)
{
    (void)user;
    if (++g_errors <= 20)
        dc_log("deko3d: %s: result %d: %s", context ? context : "?", (int)result, message ? message : "");
}

static int load_shader(DkShader* sh, const char* path, uint32_t* code_off)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return dc_log("deko3d: no %s", path), 0;
    fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(f);
    rewind(f);
    uint32_t off = *code_off;
    if (off + size + DK_SHADER_CODE_UNUSABLE_SIZE > dkMemBlockGetSize(g_code_mem))
        return fclose(f), dc_log("deko3d: code block full at %s", path), 0;
    fread((uint8_t*)dkMemBlockGetCpuAddr(g_code_mem) + off, size, 1, f);
    fclose(f);
    *code_off = ALIGN(off + size, DK_SHADER_CODE_ALIGNMENT);
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, g_code_mem, off);
    dkShaderInitialize(sh, &m);
    return 1;
}

static DkMemBlock block(uint32_t size, uint32_t flags)
{
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, g_dev, ALIGN(size, DK_MEMBLOCK_ALIGNMENT));
    m.flags = flags;
    return dkMemBlockCreate(&m);
}

static void image_layout(DkImageLayout* l, uint32_t flags, uint32_t w, uint32_t h)
{
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, g_dev);
    m.flags = flags;
    m.format = DkImageFormat_RGBA8_Unorm;
    m.dimensions[0] = w;
    m.dimensions[1] = h;
    dkImageLayoutInitialize(l, &m);
}

static int up(void)
{
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debug_cb;
    g_dev = dkDeviceCreate(&dm);
    if (!g_dev)
        return dc_log("deko3d: no device"), 0;

    /* the images: two framebuffers and the 16 textures, in one block */
    DkImageLayout fbl, texl;
    image_layout(&fbl, DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression, DC_W, DC_H);
    image_layout(&texl, 0, DC_TEX_SIZE, DC_TEX_SIZE);
    uint32_t fbsz = ALIGN((uint32_t)dkImageLayoutGetSize(&fbl), dkImageLayoutGetAlignment(&fbl));
    uint32_t texsz = ALIGN((uint32_t)dkImageLayoutGetSize(&texl), dkImageLayoutGetAlignment(&texl));
    uint32_t tex0 = ALIGN(FB_NUM * fbsz, dkImageLayoutGetAlignment(&texl));
    g_img_mem = block(tex0 + DC_NTEX * texsz, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    DkImage const* fbs[FB_NUM];
    for (int i = 0; i < FB_NUM; ++i)
        dkImageInitialize(&g_fb[i], &fbl, g_img_mem, (uint32_t)i * fbsz), fbs[i] = &g_fb[i];
    for (int i = 0; i < DC_NTEX; ++i)
        dkImageInitialize(&g_tex[i], &texl, g_img_mem, tex0 + (uint32_t)i * texsz);
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, g_dev, nwindowGetDefault(), fbs, FB_NUM);
    g_sc = dkSwapchainCreate(&sm);
    dkSwapchainSetSwapInterval(g_sc, dc_cfg.vsync ? 1 : 0);

    /* the shaders */
    g_code_mem = block(128u << 10, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    uint32_t code_off = 0;
    for (int p = 0; p < DC_NPROG; ++p)
    {
        char path[64];
        snprintf(path, sizeof path, "romfs:/shaders/vs%d.dksh", p);
        if (!load_shader(&g_vs[p], path, &code_off))
            return 0;
        snprintf(path, sizeof path, "romfs:/shaders/fs%d.dksh", p);
        if (!load_shader(&g_fs[p], path, &code_off))
            return 0;
    }

    /* the data block */
    uint32_t at = 0;
    o_ubo = at, at += DC_MAX_DRAWS * DC_UBO_STRIDE;
    for (int i = 0; i < DC_NVB; ++i)
        at = ALIGN(at, 256), o_vb[i] = at, at += DC_NVERT * sizeof(DcVertex);
    at = ALIGN(at, 256), o_ib = at, at += sizeof dc_indices;
    at = ALIGN(at, DK_IMAGE_DESCRIPTOR_ALIGNMENT), o_imgdesc = at, at += DC_NTEX * sizeof(DkImageDescriptor);
    at = ALIGN(at, DK_SAMPLER_DESCRIPTOR_ALIGNMENT), o_sampdesc = at, at += sizeof(DkSamplerDescriptor);
    at = ALIGN(at, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT), o_stage = at, at += DC_NTEX * DC_TEX_SIZE * DC_TEX_SIZE * 4;
    at = ALIGN(at, DK_CMDMEM_ALIGNMENT), o_initcmd = at, at += INIT_CMD;
    at = ALIGN(at, DK_CMDMEM_ALIGNMENT), o_cmd = at, at += 2 * CMD_SLICE;
    g_data_mem = block(at, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!g_img_mem || !g_code_mem || !g_data_mem)
        return dc_log("deko3d: no memory"), 0;
    g_data_cpu = (uint8_t*)dkMemBlockGetCpuAddr(g_data_mem);
    g_data_gpu = dkMemBlockGetGpuAddr(g_data_mem);
    dc_log("deko3d: data block %u KB (uniforms %u KB, command memory 2 x %u KB), images %u KB", at >> 10,
           (DC_MAX_DRAWS * DC_UBO_STRIDE) >> 10, CMD_SLICE >> 10, (unsigned)(dkMemBlockGetSize(g_img_mem) >> 10));
    for (int i = 0; i < DC_MAX_DRAWS; ++i)
        dc_uniforms(i, (DcUniforms*)(g_data_cpu + o_ubo + (uint32_t)i * DC_UBO_STRIDE));
    for (int i = 0; i < DC_NVB; ++i)
        dc_mesh(i, (DcVertex*)(g_data_cpu + o_vb[i]));
    memcpy(g_data_cpu + o_ib, dc_indices, sizeof dc_indices);
    for (int i = 0; i < DC_NTEX; ++i)
        dc_texture(i, (uint32_t*)(g_data_cpu + o_stage + (uint32_t)i * DC_TEX_SIZE * DC_TEX_SIZE * 4));

    /* the queue, and the textures' upload and descriptors in a first command list */
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_dev);
    qm.flags = DkQueueFlags_Graphics;
    g_q = dkQueueCreate(&qm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, g_dev);
    g_cb = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(g_cb, g_data_mem, o_initcmd, INIT_CMD);
    DkImageDescriptor idesc[DC_NTEX];
    for (int i = 0; i < DC_NTEX; ++i)
    {
        DkImageView v;
        dkImageViewDefaults(&v, &g_tex[i]);
        DkCopyBuf src = { g_data_gpu + o_stage + (uint32_t)i * DC_TEX_SIZE * DC_TEX_SIZE * 4, 0, 0 };
        DkImageRect r = { 0, 0, 0, DC_TEX_SIZE, DC_TEX_SIZE, 1 };
        dkCmdBufCopyBufferToImage(g_cb, &src, &v, &r, 0);
        dkImageDescriptorInitialize(&idesc[i], &v, false, false);
    }
    DkSampler s;
    dkSamplerDefaults(&s);
    s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_ClampToEdge;
    DkSamplerDescriptor sdesc;
    dkSamplerDescriptorInitialize(&sdesc, &s);
    dkCmdBufPushData(g_cb, g_data_gpu + o_imgdesc, idesc, sizeof idesc);
    dkCmdBufPushData(g_cb, g_data_gpu + o_sampdesc, &sdesc, sizeof sdesc);
    dkCmdBufBindImageDescriptorSet(g_cb, g_data_gpu + o_imgdesc, DC_NTEX);
    dkCmdBufBindSamplerDescriptorSet(g_cb, g_data_gpu + o_sampdesc, 1);
    dkQueueSubmitCommands(g_q, dkCmdBufFinishList(g_cb));
    dkQueueWaitIdle(g_q);
    dkCmdBufClear(g_cb);
    return !g_errors;
}

static const DkVtxAttribState g_attribs[2] = {
    { .bufferId = 0, .isFixed = 0, .offset = 0, .size = DkVtxAttribSize_2x32, .type = DkVtxAttribType_Float, .isBgra = 0 },
    { .bufferId = 0, .isFixed = 0, .offset = 8, .size = DkVtxAttribSize_2x32, .type = DkVtxAttribType_Float, .isBgra = 0 },
};
static const DkVtxBufferState g_vbstate = { sizeof(DcVertex), 0 };

/* the frame's fixed state, then the draws: what the mode rebinds, per draw; the rest left at draw 0's */
static void record(int slot, int n, int binds)
{
    DkCmdBuf cb = g_cb;
    DkImageView rt;
    dkImageViewDefaults(&rt, &g_fb[slot]);
    dkCmdBufBindRenderTarget(cb, &rt, NULL);
    DkViewport vp = { 0.0f, 0.0f, (float)DC_W, (float)DC_H, 0.0f, 1.0f };
    DkScissor sc = { 0, 0, DC_W, DC_H };
    dkCmdBufSetViewports(cb, 0, &vp, 1);
    dkCmdBufSetScissors(cb, 0, &sc, 1);
    dkCmdBufClearColorFloat(cb, 0, DkColorMask_RGBA, 0.05f, 0.05f, 0.1f, 1.0f);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    DkColorState cs;
    dkColorStateDefaults(&cs);
    DkColorWriteState cws;
    dkColorWriteStateDefaults(&cws);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = 0, ds.depthWriteEnable = 0;
    dkCmdBufBindRasterizerState(cb, &rs);
    dkCmdBufBindColorState(cb, &cs);
    dkCmdBufBindColorWriteState(cb, &cws);
    dkCmdBufBindDepthStencilState(cb, &ds);
    dkCmdBufBindVtxAttribState(cb, g_attribs, 2);
    dkCmdBufBindVtxBufferState(cb, &g_vbstate, 1);
    dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint16, g_data_gpu + o_ib);
    dkCmdBufBindImageDescriptorSet(cb, g_data_gpu + o_imgdesc, DC_NTEX);
    dkCmdBufBindSamplerDescriptorSet(cb, g_data_gpu + o_sampdesc, 1);

    const DkGpuAddr ubo = g_data_gpu + o_ubo;
    DkShader const* sh[2] = { &g_vs[0], &g_fs[0] };
    int cur_prog = 0;
    dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 0, ubo, DC_UBO_STRIDE);
    dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle(0, 0));
    dkCmdBufBindVtxBuffer(cb, 0, g_data_gpu + o_vb[0], DC_NVERT * sizeof(DcVertex));
    dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
    for (int i = 0; i < n; ++i)
    {
        if (binds & DC_BIND_UBO)
            dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 0, ubo + (DkGpuAddr)i * DC_UBO_STRIDE, DC_UBO_STRIDE);
        if (binds & DC_BIND_TEX)
            dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle((uint32_t)(i % DC_NTEX), 0));
        if (binds & DC_BIND_VB)
            dkCmdBufBindVtxBuffer(cb, 0, g_data_gpu + o_vb[i % DC_NVB], DC_NVERT * sizeof(DcVertex));
        if (binds & DC_BIND_PROG)
        {
            int p = dc_prog_of(i);
            if (p != cur_prog)
            {
                sh[0] = &g_vs[p], sh[1] = &g_fs[p];
                dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
                cur_prog = p;
            }
        }
        dkCmdBufDrawIndexed(cb, DkPrimitive_Triangles, DC_NIDX, 1, 0, 0, 0);
    }
}

int main(int argc, char** argv)
{
    romfsInit();
    dc_init(argc, argv, "deko3d", "drawcost-dk.log");
    if (!up())
    {
        dc_log("deko3d: setup failed");
        dc_exit();
        romfsExit();
        return 1;
    }
    dc_log("deko3d: up (%d programs from romfs, %d textures, %d vertex buffers)", DC_NPROG, DC_NTEX, DC_NVB);
    uint32_t frame = 0;
    while (appletMainLoop())
    {
        int vs_changed;
        if (!dc_input(&vs_changed))
            break;
        if (vs_changed)
            dkSwapchainSetSwapInterval(g_sc, dc_cfg.vsync ? 1 : 0);
        uint32_t s = frame++ & 1;
        dkFenceWait(&g_fence[s], -1); /* this slice's commands, two frames back (pipelined only) */
        uint64_t ta = dc_now_ns();
        int slot = dkQueueAcquireImage(g_q, g_sc);
        uint64_t acquire_ns = dc_now_ns() - ta;
        DcFrame f = { 0 };
        f.draws = dc_cfg.draws;
        f.start_ns = dc_now_ns();
        dkCmdBufClear(g_cb);
        dkCmdBufAddMemory(g_cb, g_data_mem, o_cmd + s * CMD_SLICE, CMD_SLICE);
        record(slot, f.draws, dc_modes[dc_cfg.mode].binds);
        uint64_t t1 = dc_now_ns();
        f.record_ns = t1 - f.start_ns;
        dkCmdBufSignalFence(g_cb, &g_fence[s], false);
        dkQueueSubmitCommands(g_q, dkCmdBufFinishList(g_cb));
        dkQueueFlush(g_q);
        uint64_t t2 = dc_now_ns();
        f.submit_ns = t2 - f.start_ns;
        if (dc_cfg.sync)
        {
            dkQueueWaitIdle(g_q);
            f.done_ns = dc_now_ns() - f.start_ns;
        }
        uint64_t t3 = dc_now_ns();
        dkQueuePresentImage(g_q, g_sc, slot);
        f.present_ns = dc_now_ns() - t3 + acquire_ns;
        dc_frame(&f);
    }
    dkQueueWaitIdle(g_q);
    dc_exit();
    dkCmdBufDestroy(g_cb);
    dkQueueDestroy(g_q);
    dkSwapchainDestroy(g_sc);
    dkMemBlockDestroy(g_data_mem);
    dkMemBlockDestroy(g_code_mem);
    dkMemBlockDestroy(g_img_mem);
    dkDeviceDestroy(g_dev);
    romfsExit();
    return 0;
}
