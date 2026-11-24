/* drawcost on deko3d: one device and queue, one big data block (uniforms, vertices, indices, descriptors, the
 * game mode's ring, command memory), one block for shader code (uam's .dksh from romfs) and one for images, a
 * command buffer recorded each frame into one of two slices of command memory.
 *
 * The simple modes' draw: dkCmdBufBindUniformBuffer (vertex stage, binding 0: the draw's 256-byte slice of the big
 * buffer), dkCmdBufBindTexture (fragment stage, binding 0: image i, sampler 0 from the descriptor sets),
 * dkCmdBufBindVtxBuffer (buffer 0: one of 8 places in the data block), dkCmdBufBindShaders when the program
 * changes, dkCmdBufDrawIndexed.
 *
 * The game mode (common.h): the same frame as gl_main.c's game_frame, in deko3d's terms. Texture buffers are buffer
 * images (DkImageType_Buffer, R32_Uint) over the ring and the static buffers; a texture and its sampler are one
 * handle (dkMakeTextureHandle), so a sampler change is a texture bind here; the uniform block, read by both stages,
 * is bound to each (two calls where GL makes one); the ring's indices are the ring bound once as the index buffer
 * and a first index (GL: an offset). */
#include "common.h"

#include <deko3d.h>
#include <string.h>
#include <switch.h>

#define FB_NUM 3 /* three: with two, a swap interval of 0 still waited for the display (60 fps at most) */
#define CMD_SLICE (4u << 20)
#define INIT_CMD (64u << 10)
#define ALIGN(x, a) (((x) + (a) - 1) & ~((uint32_t)(a) - 1))

/* the image descriptors: textures, then the texture buffers */
#define D_RING DC_NTEX
#define D_STAT (D_RING + 1)
#define D_CVB (D_STAT + DC_G_NSTATIC)
#define D_CUV (D_CVB + DC_CH_NMESH)
#define N_IMGDESC (D_CUV + DC_CH_NMESH)
/* the sampler descriptors: 0 the simple modes' (clamp, nearest), then the game's, then the characters' */
#define S_GAME 1
#define S_CHAR (S_GAME + DC_G_NSAMP)
#define N_SAMPDESC (S_CHAR + DC_CH_NSAMP)

static DkDevice g_dev;
static DkQueue g_q;
static DkMemBlock g_img_mem, g_code_mem, g_data_mem;
static DkImage g_fb[FB_NUM], g_tex[DC_NTEX], g_rt[DC_NRT], g_depth;
static DkImage g_ring_img, g_stat_img[DC_G_NSTATIC], g_cvb_img[DC_CH_NMESH], g_cuv_img[DC_CH_NMESH];
static DkSwapchain g_sc;
static DkShader g_vs[DC_NPROG], g_fs[DC_NPROG], g_gvs[DC_G_NPROG], g_gfs[DC_G_NPROG], g_cvs, g_cfs;
static DkCmdBuf g_cb;
static DkFence g_fence[2];
static uint8_t* g_data_cpu;
static DkGpuAddr g_data_gpu;
/* places in the data block */
static uint32_t o_ubo, o_vb[DC_NVB], o_ib, o_imgdesc, o_sampdesc, o_stage, o_initcmd, o_cmd;
static uint32_t o_ring, o_stat[DC_G_NSTATIC], o_gib[DC_NIB], o_cvb[DC_CH_NMESH], o_cuv[DC_CH_NMESH];
static int g_errors;
static const int g_rt_size[DC_NRT][2] = { { DC_W, DC_H }, { DC_W, DC_H }, { 16, 16 }, { 256, 256 } };

static void debug_cb(void* user, const char* context, DkResult result, const char* message)
{
    (void)user;
    if (++g_errors <= 20)
        dc_log("deko3d: %s: result %d: %s", context ? context : "?", (int)result, message ? message : "");
}

static int load_shader(DkShader* sh, const char* name, uint32_t* code_off)
{
    char path[64];
    snprintf(path, sizeof path, "romfs:/shaders/%s.dksh", name);
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

static void image_layout(DkImageLayout* l, uint32_t flags, DkImageFormat fmt, uint32_t w, uint32_t h)
{
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, g_dev);
    m.flags = flags;
    m.format = fmt;
    m.dimensions[0] = w;
    m.dimensions[1] = h;
    dkImageLayoutInitialize(l, &m);
}

/* a texture buffer: a buffer image of 32-bit words over a place in the data block */
static void buffer_image(DkImage* img, uint32_t offset, uint32_t words)
{
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, g_dev);
    m.type = DkImageType_Buffer;
    m.format = DkImageFormat_R32_Uint;
    m.dimensions[0] = words;
    DkImageLayout l;
    dkImageLayoutInitialize(&l, &m);
    dkImageInitialize(img, &l, g_data_mem, offset);
}

/* images placed one after another in the image block */
typedef struct
{
    DkImageLayout l;
    uint32_t at;
} Placed;

static uint32_t place(Placed* p, uint32_t* at)
{
    *at = ALIGN(*at, dkImageLayoutGetAlignment(&p->l));
    p->at = *at;
    *at += (uint32_t)dkImageLayoutGetSize(&p->l);
    return p->at;
}

static int up(void)
{
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debug_cb;
    g_dev = dkDeviceCreate(&dm);
    if (!g_dev)
        return dc_log("deko3d: no device"), 0;

    /* the images: the framebuffers (also the game mode's blit's destination), the textures, the game's targets */
    Placed fb, tex, rt[DC_NRT], depth;
    image_layout(&fb.l, DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine,
                 DkImageFormat_RGBA8_Unorm, DC_W, DC_H);
    image_layout(&tex.l, 0, DkImageFormat_RGBA8_Unorm, DC_TEX_SIZE, DC_TEX_SIZE);
    for (int r = 0; r < DC_NRT; ++r)
        image_layout(&rt[r].l, DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine, DkImageFormat_RGBA8_Unorm,
                     (uint32_t)g_rt_size[r][0], (uint32_t)g_rt_size[r][1]);
    image_layout(&depth.l, DkImageFlags_UsageRender | DkImageFlags_HwCompression, DkImageFormat_Z24S8, DC_W, DC_H);
    uint32_t at = 0, fb_at[FB_NUM], tex_at[DC_NTEX], rt_at[DC_NRT];
    for (int i = 0; i < FB_NUM; ++i)
        fb_at[i] = place(&fb, &at);
    for (int i = 0; i < DC_NTEX; ++i)
        tex_at[i] = place(&tex, &at);
    for (int r = 0; r < DC_NRT; ++r)
        rt_at[r] = place(&rt[r], &at);
    uint32_t depth_at = place(&depth, &at);
    g_img_mem = block(at, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    DkImage const* fbs[FB_NUM];
    for (int i = 0; i < FB_NUM; ++i)
        dkImageInitialize(&g_fb[i], &fb.l, g_img_mem, fb_at[i]), fbs[i] = &g_fb[i];
    for (int i = 0; i < DC_NTEX; ++i)
        dkImageInitialize(&g_tex[i], &tex.l, g_img_mem, tex_at[i]);
    for (int r = 0; r < DC_NRT; ++r)
        dkImageInitialize(&g_rt[r], &rt[r].l, g_img_mem, rt_at[r]);
    dkImageInitialize(&g_depth, &depth.l, g_img_mem, depth_at);
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, g_dev, nwindowGetDefault(), fbs, FB_NUM);
    g_sc = dkSwapchainCreate(&sm);
    dkSwapchainSetSwapInterval(g_sc, dc_cfg.vsync ? 1 : 0);

    /* the shaders */
    g_code_mem = block(256u << 10, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    uint32_t code_off = 0;
    char name[16];
    for (int p = 0; p < DC_NPROG; ++p)
    {
        snprintf(name, sizeof name, "vs%d", p);
        if (!load_shader(&g_vs[p], name, &code_off))
            return 0;
        snprintf(name, sizeof name, "fs%d", p);
        if (!load_shader(&g_fs[p], name, &code_off))
            return 0;
    }
    for (int p = 0; p < DC_G_NPROG; ++p)
    {
        snprintf(name, sizeof name, "gvs%d", p);
        if (!load_shader(&g_gvs[p], name, &code_off))
            return 0;
        snprintf(name, sizeof name, "gfs%d", p);
        if (!load_shader(&g_gfs[p], name, &code_off))
            return 0;
    }
    if (!load_shader(&g_cvs, "cvs0", &code_off) || !load_shader(&g_cfs, "cfs0", &code_off))
        return 0;

    /* the data block */
    at = 0;
    o_ubo = at, at += DC_MAX_DRAWS * DC_UBO_STRIDE;
    for (int i = 0; i < DC_NVB; ++i)
        at = ALIGN(at, 256), o_vb[i] = at, at += DC_NVERT * sizeof(DcVertex);
    at = ALIGN(at, 256), o_ib = at, at += sizeof dc_indices;
    for (int i = 0; i < DC_G_NSTATIC; ++i)
        at = ALIGN(at, 256), o_stat[i] = at, at += DC_G_STATIC_WORDS * 4;
    for (int i = 0; i < DC_NIB; ++i)
        at = ALIGN(at, 256), o_gib[i] = at, at += sizeof dc_indices;
    for (int i = 0; i < DC_CH_NMESH; ++i)
    {
        at = ALIGN(at, 256), o_cvb[i] = at, at += DC_NVERT * 8;
        at = ALIGN(at, 256), o_cuv[i] = at, at += DC_NVERT * 8;
    }
    at = ALIGN(at, DK_IMAGE_DESCRIPTOR_ALIGNMENT), o_imgdesc = at, at += N_IMGDESC * sizeof(DkImageDescriptor);
    at = ALIGN(at, DK_SAMPLER_DESCRIPTOR_ALIGNMENT), o_sampdesc = at, at += N_SAMPDESC * sizeof(DkSamplerDescriptor);
    at = ALIGN(at, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT), o_stage = at, at += DC_NTEX * DC_TEX_SIZE * DC_TEX_SIZE * 4;
    at = ALIGN(at, DK_CMDMEM_ALIGNMENT), o_initcmd = at, at += INIT_CMD;
    at = ALIGN(at, 256), o_cmd = at, at += 2 * CMD_SLICE;
    at = ALIGN(at, 256), o_ring = at, at += 2 * DC_RING_SEG;
    g_data_mem = block(at, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!g_img_mem || !g_code_mem || !g_data_mem)
        return dc_log("deko3d: no memory"), 0;
    g_data_cpu = (uint8_t*)dkMemBlockGetCpuAddr(g_data_mem);
    g_data_gpu = dkMemBlockGetGpuAddr(g_data_mem);
    dc_log("deko3d: data block %u KB (uniforms %u KB, command memory 2 x %u KB, ring 2 x %u KB), images %u KB", at >> 10,
           (DC_MAX_DRAWS * DC_UBO_STRIDE) >> 10, CMD_SLICE >> 10, DC_RING_SEG >> 10,
           (unsigned)(dkMemBlockGetSize(g_img_mem) >> 10));
    for (int i = 0; i < DC_MAX_DRAWS; ++i)
        dc_uniforms(i, (DcUniforms*)(g_data_cpu + o_ubo + (uint32_t)i * DC_UBO_STRIDE));
    for (int i = 0; i < DC_NVB; ++i)
        dc_mesh(i, (DcVertex*)(g_data_cpu + o_vb[i]));
    memcpy(g_data_cpu + o_ib, dc_indices, sizeof dc_indices);
    for (int i = 0; i < DC_G_NSTATIC; ++i)
    {
        dc_game_words(i, 0, (uint32_t*)(g_data_cpu + o_stat[i]));
        dc_game_words(i, 1, (uint32_t*)(g_data_cpu + o_stat[i]) + DC_G_ARRAY_VERTS * 4);
    }
    for (int i = 0; i < DC_NIB; ++i)
        memcpy(g_data_cpu + o_gib[i], dc_indices, sizeof dc_indices);
    for (int m = 0; m < DC_CH_NMESH; ++m)
    {
        DcVertex v[DC_NVERT];
        dc_mesh(m, v);
        float* pos = (float*)(g_data_cpu + o_cvb[m]);
        float* uv = (float*)(g_data_cpu + o_cuv[m]);
        for (int k = 0; k < DC_NVERT; ++k)
            pos[2 * k] = v[k].pos[0], pos[2 * k + 1] = v[k].pos[1], uv[2 * k] = v[k].uv[0], uv[2 * k + 1] = v[k].uv[1];
    }
    for (int i = 0; i < DC_NTEX; ++i)
        dc_texture(i, (uint32_t*)(g_data_cpu + o_stage + (uint32_t)i * DC_TEX_SIZE * DC_TEX_SIZE * 4));
    buffer_image(&g_ring_img, o_ring, 2 * DC_RING_SEG / 4);
    for (int i = 0; i < DC_G_NSTATIC; ++i)
        buffer_image(&g_stat_img[i], o_stat[i], DC_G_STATIC_WORDS);
    for (int m = 0; m < DC_CH_NMESH; ++m)
        buffer_image(&g_cvb_img[m], o_cvb[m], DC_NVERT * 2), buffer_image(&g_cuv_img[m], o_cuv[m], DC_NVERT * 2);

    /* the queue, and the textures' upload and the descriptors in a first command list */
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_dev);
    qm.flags = DkQueueFlags_Graphics;
    g_q = dkQueueCreate(&qm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, g_dev);
    g_cb = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(g_cb, g_data_mem, o_initcmd, INIT_CMD);
    static DkImageDescriptor idesc[N_IMGDESC];
    DkImageView v;
    for (int i = 0; i < DC_NTEX; ++i)
    {
        dkImageViewDefaults(&v, &g_tex[i]);
        DkCopyBuf src = { g_data_gpu + o_stage + (uint32_t)i * DC_TEX_SIZE * DC_TEX_SIZE * 4, 0, 0 };
        DkImageRect r = { 0, 0, 0, DC_TEX_SIZE, DC_TEX_SIZE, 1 };
        dkCmdBufCopyBufferToImage(g_cb, &src, &v, &r, 0);
        dkImageDescriptorInitialize(&idesc[i], &v, false, false);
    }
    dkImageViewDefaults(&v, &g_ring_img), dkImageDescriptorInitialize(&idesc[D_RING], &v, false, false);
    for (int i = 0; i < DC_G_NSTATIC; ++i)
        dkImageViewDefaults(&v, &g_stat_img[i]), dkImageDescriptorInitialize(&idesc[D_STAT + i], &v, false, false);
    for (int m = 0; m < DC_CH_NMESH; ++m)
    {
        dkImageViewDefaults(&v, &g_cvb_img[m]), dkImageDescriptorInitialize(&idesc[D_CVB + m], &v, false, false);
        dkImageViewDefaults(&v, &g_cuv_img[m]), dkImageDescriptorInitialize(&idesc[D_CUV + m], &v, false, false);
    }
    static DkSamplerDescriptor sdesc[N_SAMPDESC];
    for (int i = 0; i < N_SAMPDESC; ++i)
    {
        DkSampler s;
        dkSamplerDefaults(&s);
        if (i == 0)
            s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_ClampToEdge;
        else
        {
            int k = i - S_GAME; /* the GL path's g_samps[k] */
            if (i >= S_CHAR)
                k = DC_G_NSAMP + (i - S_CHAR);
            DkFilter f = k & 1 ? DkFilter_Linear : DkFilter_Nearest;
            DkWrapMode w = k & 2 ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
            s.minFilter = s.magFilter = f;
            s.wrapMode[0] = s.wrapMode[2] = w;
            s.wrapMode[1] = k & 4 ? DkWrapMode_MirroredRepeat : w;
        }
        dkSamplerDescriptorInitialize(&sdesc[i], &s);
    }
    dkCmdBufPushData(g_cb, g_data_gpu + o_imgdesc, idesc, sizeof idesc);
    dkCmdBufPushData(g_cb, g_data_gpu + o_sampdesc, sdesc, sizeof sdesc);
    dkCmdBufBindImageDescriptorSet(g_cb, g_data_gpu + o_imgdesc, N_IMGDESC);
    dkCmdBufBindSamplerDescriptorSet(g_cb, g_data_gpu + o_sampdesc, N_SAMPDESC);
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

static void fixed_state(DkCmdBuf cb)
{
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
    dkCmdBufBindImageDescriptorSet(cb, g_data_gpu + o_imgdesc, N_IMGDESC);
    dkCmdBufBindSamplerDescriptorSet(cb, g_data_gpu + o_sampdesc, N_SAMPDESC);
}

static void viewport(DkCmdBuf cb, int w, int h)
{
    DkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    DkScissor sc = { 0, 0, (uint32_t)w, (uint32_t)h };
    dkCmdBufSetViewports(cb, 0, &vp, 1);
    dkCmdBufSetScissors(cb, 0, &sc, 1);
}

/* the simple modes: the frame's fixed state, then the draws: what the mode rebinds, per draw; the rest left at
 * draw 0's */
static void record(int slot, int n, int binds)
{
    DkCmdBuf cb = g_cb;
    DkImageView rt;
    dkImageViewDefaults(&rt, &g_fb[slot]);
    dkCmdBufBindRenderTarget(cb, &rt, NULL);
    viewport(cb, DC_W, DC_H);
    dkCmdBufClearColorFloat(cb, 0, DkColorMask_RGBA, 0.05f, 0.05f, 0.1f, 1.0f);
    fixed_state(cb);
    dkCmdBufBindVtxAttribState(cb, g_attribs, 2);
    dkCmdBufBindVtxBufferState(cb, &g_vbstate, 1);
    dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint16, g_data_gpu + o_ib);

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

/* --- the game mode ----------------------------------------------------------------------------------------- */
static uint8_t* g_ring_cpu;
static DkGpuAddr g_ring_gpu;
static uint32_t g_seg, g_seg_used;
static uint32_t g_words_list[DC_G_NSTATIC][DC_G_ARRAY_VERTS * 4], g_words_idx[DC_G_NSTATIC][DC_NVERT * 4];
static float g_ublock[DC_G_URANGE / 4], g_cstate[DC_CH_STATE / 4], g_cpal[DC_CH_PAL / 4];

static void game_up(void)
{
    g_ring_cpu = g_data_cpu + o_ring;
    g_ring_gpu = g_data_gpu + o_ring;
    for (int i = 0; i < DC_G_NSTATIC; ++i)
        dc_game_words(i, 0, g_words_list[i]), dc_game_words(i, 1, g_words_idx[i]);
    for (int i = 0; i < DC_G_URANGE / 4; ++i)
        g_ublock[i] = 0.001f * (float)(i % 97);
    for (int i = 0; i < DC_CH_STATE / 4; ++i)
        g_cstate[i] = 0.001f * (float)(i % 89);
    for (int i = 0; i < DC_CH_PAL / 4; ++i)
        g_cpal[i] = 0.6f + 0.004f * (float)(i % 100);
}

/* a place in this frame's segment of the ring (gl_main.c's ring_put): its offset from the ring's start */
static uint32_t ring_put(const void* src, uint32_t size, uint32_t align)
{
    uint32_t at = (g_seg_used + align - 1) & ~(align - 1);
    if (at + size + DC_G_URANGE > DC_RING_SEG)
        at = 0;
    g_seg_used = at + size;
    uint32_t off = g_seg * DC_RING_SEG + at;
    if (src)
        memcpy(g_ring_cpu + off, src, size);
    dc_cnt.ring_bytes += size;
    return off;
}

/* what is bound (gl_main.c's S): a bind of what is bound already is left out */
static struct
{
    int prog, rt;
    int tex[3], samp[3];   /* the fragment stage's slots 0, 1 (game) */
    int vtex[5];           /* the vertex stage's texture buffers in slots 2, 3, 4 */
    DkGpuAddr ubo[3];
    uint32_t ubo_size[3];
    DkGpuAddr ib;
    DkColorState cs;
    DkDepthStencilState ds;
    DkRasterizerState rs;
    int blend, zwrite, zfunc, cull;
} D;

static DkImageView g_rt_view[DC_NRT], g_depth_view;

static void set_rt(DkCmdBuf cb, int r)
{
    dkCmdBufBindRenderTarget(cb, &g_rt_view[r], r == DC_RT_MAIN ? &g_depth_view : NULL);
    viewport(cb, g_rt_size[r][0], g_rt_size[r][1]);
    D.rt = r;
    dc_cnt.rt++;
}

static void toggle(DkCmdBuf cb, int what)
{
    switch (what)
    {
    case DC_ST_BLEND:
        D.blend = !D.blend;
        dkColorStateSetBlendEnable(&D.cs, 0, D.blend != 0);
        dkCmdBufBindColorState(cb, &D.cs);
        break;
    case DC_ST_ZWRITE:
        D.ds.depthWriteEnable = D.zwrite = !D.zwrite;
        dkCmdBufBindDepthStencilState(cb, &D.ds);
        break;
    case DC_ST_ZFUNC:
        D.ds.depthCompareOp = (D.zfunc = !D.zfunc) ? DkCompareOp_Always : DkCompareOp_Lequal;
        dkCmdBufBindDepthStencilState(cb, &D.ds);
        break;
    case DC_ST_CULL:
        D.rs.cullMode = (D.cull = !D.cull) ? DkFace_Back : DkFace_None;
        dkCmdBufBindRasterizerState(cb, &D.rs);
        break;
    }
    dc_cnt.state++;
}

static void st_prog(DkCmdBuf cb, int p, const DkShader* vs, const DkShader* fs)
{
    if (D.prog == p)
        return;
    DkShader const* sh[2] = { vs, fs };
    dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
    D.prog = p;
    dc_cnt.progs++;
}

/* a fragment-stage texture with its sampler: one handle */
static void st_ftex(DkCmdBuf cb, int slot, int tex, int samp)
{
    if (D.tex[slot] == tex && D.samp[slot] == samp)
        return;
    dc_cnt.stage_tex += D.tex[slot] != tex;
    dc_cnt.samplers += D.samp[slot] != samp;
    dkCmdBufBindTexture(cb, DkStage_Fragment, (uint32_t)slot, dkMakeTextureHandle((uint32_t)tex, (uint32_t)samp));
    D.tex[slot] = tex, D.samp[slot] = samp;
}

/* a vertex-stage texture buffer */
static void st_vtex(DkCmdBuf cb, int slot, int img)
{
    if (D.vtex[slot] == img)
        return;
    dkCmdBufBindTexture(cb, DkStage_Vertex, (uint32_t)slot, dkMakeTextureHandle((uint32_t)img, 0));
    D.vtex[slot] = img;
    dc_cnt.stream_tex++;
}

static void st_ubo(DkCmdBuf cb, int i, uint32_t off, uint32_t size, int both)
{
    DkGpuAddr a = g_ring_gpu + off;
    if (D.ubo[i] == a && D.ubo_size[i] == size)
        return;
    dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, (uint32_t)i, a, size);
    if (both)
        dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, (uint32_t)i, a, size);
    D.ubo[i] = a, D.ubo_size[i] = size;
    dc_cnt.ubo++;
}

static void st_ib(DkCmdBuf cb, DkGpuAddr a)
{
    if (D.ib == a)
        return;
    dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint16, a);
    D.ib = a;
    dc_cnt.ebo++;
}

static int game_record(int slot, uint32_t s)
{
    DkCmdBuf cb = g_cb;
    g_seg = s, g_seg_used = 0; /* (this slice's fence was waited for: the GPU is done with its segment) */
    memset(&D, 0xFF, sizeof D);
    for (int r = 0; r < DC_NRT; ++r)
        dkImageViewDefaults(&g_rt_view[r], &g_rt[r]);
    dkImageViewDefaults(&g_depth_view, &g_depth);
    fixed_state(cb);
    dkRasterizerStateDefaults(&D.rs), D.rs.cullMode = DkFace_None, D.cull = 0;
    dkColorStateDefaults(&D.cs), D.blend = 0;
    dkDepthStencilStateDefaults(&D.ds);
    D.ds.depthTestEnable = 1, D.ds.depthWriteEnable = 1, D.ds.depthCompareOp = DkCompareOp_Lequal;
    D.zwrite = 1, D.zfunc = 0;
    dkCmdBufBindRasterizerState(cb, &D.rs);
    dkCmdBufBindColorState(cb, &D.cs);
    dkCmdBufBindDepthStencilState(cb, &D.ds);
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
    dkCmdBufBindBlendState(cb, 0, &bs);
    dkCmdBufBindVtxAttribState(cb, NULL, 0);
    dkCmdBufBindVtxBufferState(cb, NULL, 0);
    set_rt(cb, DC_RT_MAIN);
    dkCmdBufClearColorFloat(cb, 0, DkColorMask_RGBA, 0.05f, 0.05f, 0.1f, 1.0f);
    dkCmdBufClearDepthStencil(cb, true, 1.0f, 0xFF, 0);
    int draws = 0;
    for (int i = 0; i < DC_G_DRAWS; ++i)
    {
        const DcGameDraw* d = &dc_game[i];
        if (d->rt != D.rt)
        {
            set_rt(cb, d->rt);
            if (d->clear)
                dkCmdBufClearColorFloat(cb, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
        }
        if (d->state)
            toggle(cb, d->state);
        st_prog(cb, d->prog, &g_gvs[d->prog], &g_gfs[d->prog]);
        st_ftex(cb, 0, d->tex0, S_GAME + d->samp0);
        st_ftex(cb, 1, d->tex1, S_GAME + 0);
        int indexed = d->kind != DC_K_ARRAYS;
        int base, img;
        if (d->src == DC_G_NSTATIC)
        {
            int m = i % DC_G_NSTATIC;
            base = (int)(ring_put(indexed ? g_words_idx[m] : g_words_list[m],
                                  indexed ? sizeof g_words_idx[m] : sizeof g_words_list[m], 16) / 4);
            img = D_RING;
        }
        else
            base = indexed ? DC_G_ARRAY_VERTS * 4 : 0, img = D_STAT + d->src;
        st_vtex(cb, 2, img);
        dc_game_uniforms(i, base, g_ublock);
        st_ubo(cb, 0, ring_put(g_ublock, DC_G_UWRITE, DC_UBO_STRIDE), DC_G_URANGE, 1);
        if (d->kind == DC_K_ARRAYS)
            dkCmdBufDraw(cb, DkPrimitive_Triangles, DC_G_ARRAY_VERTS, 1, 0, 0);
        else if (d->kind == DC_K_RING_IDX)
        {
            uint32_t at = ring_put(dc_indices, sizeof dc_indices, 4);
            st_ib(cb, g_ring_gpu);
            dkCmdBufDrawIndexed(cb, DkPrimitive_Triangles, DC_NIDX, 1, at / 2, 0, 0);
        }
        else
        {
            st_ib(cb, g_data_gpu + o_gib[d->ib]);
            dkCmdBufDrawIndexed(cb, DkPrimitive_Triangles, DC_NIDX, 1, 0, 0, 0);
        }
        ++draws;
    }
    /* the characters */
    if (D.rt != DC_RT_MAIN)
        set_rt(cb, DC_RT_MAIN);
    if (!D.zwrite)
        toggle(cb, DC_ST_ZWRITE);
    if (D.zfunc)
        toggle(cb, DC_ST_ZFUNC);
    if (D.blend)
        toggle(cb, DC_ST_BLEND);
    if (D.cull)
        toggle(cb, DC_ST_CULL);
    st_prog(cb, 100, &g_cvs, &g_cfs);
    for (int c = 0; c < DC_CH_CHARS; ++c)
    {
        g_cstate[0] = -0.9f + (float)(c % 15) * 0.12f, g_cstate[1] = -0.75f - (float)(c / 15) * 0.15f;
        g_cstate[2] = 0.03f, g_cstate[3] = 0.05f;
        st_ubo(cb, 1, ring_put(g_cstate, DC_CH_STATE, DC_UBO_STRIDE), DC_CH_STATE, 0);
        st_ib(cb, g_data_gpu + o_gib[c % DC_NIB]);
        for (int k = 0; k < DC_CH_ITEMS; ++k)
        {
            int m = (c * DC_CH_ITEMS + k) % DC_CH_NMESH;
            float item[8] = { 1.0f, 0.9f + 0.01f * (float)k, 1.0f, 1.0f, 0, 0, 0, 0 };
            st_ubo(cb, 0, ring_put(item, DC_CH_ITEM, DC_UBO_STRIDE), DC_CH_ITEM, 0);
            g_cpal[0] = 0.5f + 0.05f * (float)k;
            st_ubo(cb, 2, ring_put(g_cpal, DC_CH_PAL, DC_UBO_STRIDE), DC_CH_PAL, 0);
            st_vtex(cb, 3, D_CVB + m);
            st_vtex(cb, 4, D_CUV + m);
            st_ftex(cb, 0, m % DC_NTEX, S_CHAR + c % DC_CH_NSAMP);
            dkCmdBufDrawIndexed(cb, DkPrimitive_Triangles, DC_NIDX, k % 2 == 0 ? 2 : 1, 0, 0, 0);
            ++draws;
        }
    }
    /* to the screen */
    dkCmdBufBarrier(cb, DkBarrier_Fragments, 0);
    DkImageView dst;
    dkImageViewDefaults(&dst, &g_fb[slot]);
    DkImageRect r = { 0, 0, 0, DC_W, DC_H, 1 };
    dkCmdBufBlitImage(cb, &g_rt_view[DC_RT_MAIN], &r, &dst, &r, 0, 0);
    dc_cnt.draws = (uint32_t)draws;
    return draws;
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
    game_up();
    dc_log("deko3d: up (%d + %d + 1 programs from romfs, %d textures, %d vertex buffers, %d framebuffers)", DC_NPROG,
           DC_G_NPROG, DC_NTEX, DC_NVB, FB_NUM);
    uint32_t frame = 0;
    while (appletMainLoop())
    {
        int vs_changed;
        if (!dc_input(&vs_changed))
            break;
        if (vs_changed)
            dkSwapchainSetSwapInterval(g_sc, dc_cfg.vsync ? 1 : 0);
        uint32_t s = frame++ & 1;
        dkFenceWait(&g_fence[s], -1); /* this slice's commands and ring segment, two frames back (pipelined only) */
        uint64_t ta = dc_now_ns();
        int slot = dkQueueAcquireImage(g_q, g_sc);
        uint64_t acquire_ns = dc_now_ns() - ta;
        DcFrame f = { 0 };
        int binds = dc_modes[dc_cfg.mode].binds;
        f.start_ns = dc_now_ns();
        dkCmdBufClear(g_cb);
        dkCmdBufAddMemory(g_cb, g_data_mem, o_cmd + s * CMD_SLICE, CMD_SLICE);
        if (binds == DC_GAME)
            f.draws = game_record(slot, s);
        else
        {
            f.draws = dc_cfg.draws;
            record(slot, f.draws, binds);
        }
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
