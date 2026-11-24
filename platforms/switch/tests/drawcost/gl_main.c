/* drawcost on OpenGL 4.3 core: Mesa 26 (nouveau, the game's build), through EGL as gfx_gl.c does, glthread as
 * Mesa sets it (on by default; mesa_glthread=false in drawcost.cfg turns it off).
 *
 * The draw: glBindBufferRange (the uniform block's slice), glBindTexture (unit 0), glBindVertexBuffer (GL 4.3's
 * vertex attribute binding, one VAO), glUseProgram when the program changes, glDrawElements. The sampler is a
 * sampler object bound once; the index buffer is the VAO's. */
#include "common.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "shader_src.h" /* build.py's: dc_vs_src, dc_fs_src from vs.glsl and fs.glsl */

#define GL_FUNCS(X)                                                                                           \
    X(PFNGLGETSTRINGPROC, GetString)                                                                          \
    X(PFNGLGETINTEGERVPROC, GetIntegerv)                                                                      \
    X(PFNGLGETERRORPROC, GetError)                                                                            \
    X(PFNGLGENBUFFERSPROC, GenBuffers)                                                                        \
    X(PFNGLBINDBUFFERPROC, BindBuffer)                                                                        \
    X(PFNGLBUFFERDATAPROC, BufferData)                                                                        \
    X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays)                                                              \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray)                                                              \
    X(PFNGLVERTEXATTRIBFORMATPROC, VertexAttribFormat)                                                        \
    X(PFNGLVERTEXATTRIBBINDINGPROC, VertexAttribBinding)                                                      \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray)                                              \
    X(PFNGLBINDVERTEXBUFFERPROC, BindVertexBuffer)                                                            \
    X(PFNGLGENTEXTURESPROC, GenTextures)                                                                      \
    X(PFNGLBINDTEXTUREPROC, BindTexture)                                                                      \
    X(PFNGLTEXSTORAGE2DPROC, TexStorage2D)                                                                    \
    X(PFNGLTEXSUBIMAGE2DPROC, TexSubImage2D)                                                                  \
    X(PFNGLGENSAMPLERSPROC, GenSamplers)                                                                      \
    X(PFNGLSAMPLERPARAMETERIPROC, SamplerParameteri)                                                          \
    X(PFNGLBINDSAMPLERPROC, BindSampler)                                                                      \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture)                                                                  \
    X(PFNGLCREATESHADERPROC, CreateShader)                                                                    \
    X(PFNGLSHADERSOURCEPROC, ShaderSource)                                                                    \
    X(PFNGLCOMPILESHADERPROC, CompileShader)                                                                  \
    X(PFNGLGETSHADERIVPROC, GetShaderiv)                                                                      \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog)                                                            \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram)                                                                  \
    X(PFNGLATTACHSHADERPROC, AttachShader)                                                                    \
    X(PFNGLLINKPROGRAMPROC, LinkProgram)                                                                      \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv)                                                                    \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog)                                                          \
    X(PFNGLUSEPROGRAMPROC, UseProgram)                                                                        \
    X(PFNGLBINDBUFFERRANGEPROC, BindBufferRange)                                                              \
    X(PFNGLDRAWELEMENTSPROC, DrawElements)                                                                    \
    X(PFNGLVIEWPORTPROC, Viewport)                                                                            \
    X(PFNGLCLEARCOLORPROC, ClearColor)                                                                        \
    X(PFNGLCLEARPROC, Clear)                                                                                  \
    X(PFNGLFINISHPROC, Finish)                                                                                \
    X(PFNGLDISABLEPROC, Disable)                                                                              \
    X(PFNGLENABLEPROC, Enable)                                                                                \
    X(PFNGLDEPTHMASKPROC, DepthMask)                                                                          \
    X(PFNGLDEPTHFUNCPROC, DepthFunc)                                                                          \
    X(PFNGLBLENDFUNCPROC, BlendFunc)                                                                          \
    X(PFNGLCULLFACEPROC, CullFace)                                                                            \
    X(PFNGLTEXBUFFERPROC, TexBuffer)                                                                          \
    X(PFNGLBUFFERSTORAGEPROC, BufferStorage)                                                                  \
    X(PFNGLMAPBUFFERRANGEPROC, MapBufferRange)                                                                \
    X(PFNGLFENCESYNCPROC, FenceSync)                                                                          \
    X(PFNGLCLIENTWAITSYNCPROC, ClientWaitSync)                                                                \
    X(PFNGLDELETESYNCPROC, DeleteSync)                                                                        \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers)                                                              \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer)                                                              \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D)                                                    \
    X(PFNGLGENRENDERBUFFERSPROC, GenRenderbuffers)                                                            \
    X(PFNGLBINDRENDERBUFFERPROC, BindRenderbuffer)                                                            \
    X(PFNGLRENDERBUFFERSTORAGEPROC, RenderbufferStorage)                                                      \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, FramebufferRenderbuffer)                                              \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus)                                                \
    X(PFNGLBLITFRAMEBUFFERPROC, BlitFramebuffer)                                                              \
    X(PFNGLDRAWARRAYSPROC, DrawArrays)                                                                        \
    X(PFNGLDRAWELEMENTSINSTANCEDPROC, DrawElementsInstanced)

static struct
{
#define X(t, n) t n;
    GL_FUNCS(X)
#undef X
} gl;

static EGLDisplay g_dpy;
static EGLSurface g_surf;
static EGLContext g_ctx;

static int egl_up(void)
{
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, NULL, NULL))
        return dc_log("EGL: no display (0x%x)", eglGetError()), 0;
    if (!eglBindAPI(EGL_OPENGL_API))
        return dc_log("EGL: no desktop OpenGL (0x%x)", eglGetError()), 0;
    static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &ncfg) || !ncfg)
        return dc_log("EGL: no config (0x%x)", eglGetError()), 0;
    NWindow* nw = nwindowGetDefault();
    nwindowSetDimensions(nw, DC_W, DC_H);
    g_surf = eglCreateWindowSurface(g_dpy, cfg, (EGLNativeWindowType)nw, NULL);
    if (g_surf == EGL_NO_SURFACE)
        return dc_log("EGL: no window surface (0x%x)", eglGetError()), 0;
    static const EGLint ctx_attr[] = { EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE };
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx))
        return dc_log("EGL: no OpenGL 4.3 core context (0x%x)", eglGetError()), 0;
#define X(t, n)                                                                                               \
    if (!(gl.n = (t)eglGetProcAddress("gl" #n)))                                                              \
        return dc_log("GL: no gl" #n), 0;
    GL_FUNCS(X)
#undef X
    return 1;
}

static GLuint program(int prog, const char* vs_body, const char* fs_body);

static GLuint shader(GLenum type, int prog, const char* body)
{
    char head[64];
    snprintf(head, sizeof head, "#version 430 core\n#define PROG %d\n", prog);
    const char* src[2] = { head, body };
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 2, src, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = "";
        gl.GetShaderInfoLog(s, sizeof log, NULL, log);
        dc_log("GL: shader %d (%s) not compiled: %s", prog, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        return 0;
    }
    return s;
}

static GLuint program(int prog, const char* vs_body, const char* fs_body)
{
    GLuint vs = shader(GL_VERTEX_SHADER, prog, vs_body), fs = shader(GL_FRAGMENT_SHADER, prog, fs_body);
    if (!vs || !fs)
        return 0;
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs);
    gl.AttachShader(p, fs);
    gl.LinkProgram(p);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = "";
        gl.GetProgramInfoLog(p, sizeof log, NULL, log);
        dc_log("GL: program %d not linked: %s", prog, log);
        return 0;
    }
    return p;
}

static GLuint g_prog[DC_NPROG], g_tex[DC_NTEX], g_vb[DC_NVB], g_ubo, g_ib, g_vao, g_samp;

static int scene_up(void)
{
    for (int p = 0; p < DC_NPROG; ++p)
        if (!(g_prog[p] = program(p, dc_vs_src, dc_fs_src)))
            return 0;
    /* the uniform buffer: every draw's block, written once */
    GLint align = 0;
    gl.GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    dc_log("GL: uniform buffer offset alignment %d (the test's stride %d)", align, DC_UBO_STRIDE);
    if (align > DC_UBO_STRIDE)
        return dc_log("GL: the alignment is above the stride"), 0;
    static DcUniforms u[DC_MAX_DRAWS];
    for (int i = 0; i < DC_MAX_DRAWS; ++i)
        dc_uniforms(i, &u[i]);
    gl.GenBuffers(1, &g_ubo);
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_ubo);
    gl.BufferData(GL_UNIFORM_BUFFER, sizeof u, u, GL_STATIC_DRAW);
    /* the vertex buffers, the index buffer and the VAO */
    gl.GenVertexArrays(1, &g_vao);
    gl.BindVertexArray(g_vao);
    gl.GenBuffers(DC_NVB, g_vb);
    for (int i = 0; i < DC_NVB; ++i)
    {
        DcVertex v[DC_NVERT];
        dc_mesh(i, v);
        gl.BindBuffer(GL_ARRAY_BUFFER, g_vb[i]);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, 0);
    gl.GenBuffers(1, &g_ib);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ib);
    gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof dc_indices, dc_indices, GL_STATIC_DRAW);
    gl.VertexAttribFormat(0, 2, GL_FLOAT, GL_FALSE, 0);
    gl.VertexAttribFormat(1, 2, GL_FLOAT, GL_FALSE, 8);
    gl.VertexAttribBinding(0, 0);
    gl.VertexAttribBinding(1, 0);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.BindVertexBuffer(0, g_vb[0], 0, sizeof(DcVertex));
    /* the textures and the sampler */
    gl.GenTextures(DC_NTEX, g_tex);
    gl.ActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < DC_NTEX; ++i)
    {
        static uint32_t px[DC_TEX_SIZE * DC_TEX_SIZE];
        dc_texture(i, px);
        gl.BindTexture(GL_TEXTURE_2D, g_tex[i]);
        gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, DC_TEX_SIZE, DC_TEX_SIZE);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DC_TEX_SIZE, DC_TEX_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    gl.GenSamplers(1, &g_samp);
    gl.SamplerParameteri(g_samp, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.SamplerParameteri(g_samp, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.SamplerParameteri(g_samp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.SamplerParameteri(g_samp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.BindSampler(0, g_samp);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_BLEND);
    gl.Viewport(0, 0, DC_W, DC_H);
    gl.ClearColor(0.05f, 0.05f, 0.1f, 1.0f);
    GLenum e = gl.GetError();
    if (e)
        return dc_log("GL: error 0x%x setting up", e), 0;
    return 1;
}

/* one frame's draws: what the mode rebinds, per draw; the rest left at draw 0's */
static void draws(int n, int binds)
{
    int cur_prog = -1;
    gl.BindBufferRange(GL_UNIFORM_BUFFER, 0, g_ubo, 0, DC_UBO_STRIDE);
    gl.BindTexture(GL_TEXTURE_2D, g_tex[0]);
    gl.BindVertexBuffer(0, g_vb[0], 0, sizeof(DcVertex));
    gl.UseProgram(g_prog[0]), cur_prog = 0;
    for (int i = 0; i < n; ++i)
    {
        if (binds & DC_BIND_UBO)
            gl.BindBufferRange(GL_UNIFORM_BUFFER, 0, g_ubo, (GLintptr)i * DC_UBO_STRIDE, DC_UBO_STRIDE);
        if (binds & DC_BIND_TEX)
            gl.BindTexture(GL_TEXTURE_2D, g_tex[i % DC_NTEX]);
        if (binds & DC_BIND_VB)
            gl.BindVertexBuffer(0, g_vb[i % DC_NVB], 0, sizeof(DcVertex));
        if (binds & DC_BIND_PROG)
        {
            int p = dc_prog_of(i);
            if (p != cur_prog)
                gl.UseProgram(g_prog[p]), cur_prog = p;
        }
        gl.DrawElements(GL_TRIANGLES, DC_NIDX, GL_UNSIGNED_SHORT, (const void*)0);
    }
}

/* --- the "game" mode (common.h): gfx_gl.c's way of drawing ------------------------------------------------------- */
static GLuint g_gprog[DC_G_NPROG], g_cprog, g_gvao;
static GLuint g_ring, g_ring_tbo;
static uint8_t* g_ring_cpu;
static GLsync g_ring_fence[2];
static uint32_t g_seg, g_seg_used;
static GLuint g_sbuf[DC_G_NSTATIC], g_stbo[DC_G_NSTATIC], g_ibs[DC_NIB];
static GLuint g_cvb[DC_CH_NMESH], g_cvb_tbo[DC_CH_NMESH], g_cuv[DC_CH_NMESH], g_cuv_tbo[DC_CH_NMESH];
static GLuint g_samps[DC_G_NSAMP + DC_CH_NSAMP];
static GLuint g_fbo[DC_NRT], g_rt_tex[DC_NRT], g_depth_rb;
static const int g_rt_size[DC_NRT][2] = { { DC_W, DC_H }, { DC_W, DC_H }, { 16, 16 }, { 256, 256 } };
static uint32_t g_words_list[DC_G_NSTATIC][DC_G_ARRAY_VERTS * 4], g_words_idx[DC_G_NSTATIC][DC_NVERT * 4];
static float g_ublock[DC_G_URANGE / 4], g_cstate[DC_CH_STATE / 4], g_cpal[DC_CH_PAL / 4];

static int game_up(void)
{
    for (int p = 0; p < DC_G_NPROG; ++p)
        if (!(g_gprog[p] = program(p, dc_game_vs_src, dc_game_fs_src)))
            return 0;
    if (!(g_cprog = program(0, dc_char_vs_src, dc_char_fs_src)))
        return 0;
    gl.GenVertexArrays(1, &g_gvao); /* no attributes: the vertices are pulled from texture buffers */
    /* the ring: two frames' segments, persistently mapped and coherent (gfx_gl.c's ring_make), one texture buffer
     * of words over all of it */
    const uint32_t ring_size = 2 * DC_RING_SEG;
    GLint tbs = 0;
    gl.GetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &tbs);
    if ((uint32_t)tbs < ring_size / 4)
        return dc_log("GL: texture buffers of %d words, %u needed", tbs, ring_size / 4), 0;
    gl.GenBuffers(1, &g_ring);
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, g_ring);
    GLbitfield mf = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    gl.BufferStorage(GL_COPY_WRITE_BUFFER, ring_size, NULL, mf);
    g_ring_cpu = (uint8_t*)gl.MapBufferRange(GL_COPY_WRITE_BUFFER, 0, ring_size, mf);
    if (!g_ring_cpu)
        return dc_log("GL: the ring was not mapped (0x%x)", gl.GetError()), 0;
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GenTextures(1, &g_ring_tbo);
    gl.BindTexture(GL_TEXTURE_BUFFER, g_ring_tbo);
    gl.TexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, g_ring);
    /* the static streams (each: the 24-vertex list, then the 9-vertex mesh), the index buffers */
    for (int i = 0; i < DC_G_NSTATIC; ++i)
    {
        dc_game_words(i, 0, g_words_list[i]);
        dc_game_words(i, 1, g_words_idx[i]);
        uint32_t w[DC_G_STATIC_WORDS];
        memcpy(w, g_words_list[i], sizeof g_words_list[i]);
        memcpy(w + DC_G_ARRAY_VERTS * 4, g_words_idx[i], sizeof g_words_idx[i]);
        gl.GenBuffers(1, &g_sbuf[i]);
        gl.BindBuffer(GL_COPY_WRITE_BUFFER, g_sbuf[i]);
        gl.BufferData(GL_COPY_WRITE_BUFFER, sizeof w, w, GL_STATIC_DRAW);
        gl.GenTextures(1, &g_stbo[i]);
        gl.BindTexture(GL_TEXTURE_BUFFER, g_stbo[i]);
        gl.TexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, g_sbuf[i]);
    }
    gl.BindVertexArray(g_gvao);
    for (int i = 0; i < DC_NIB; ++i)
    {
        gl.GenBuffers(1, &g_ibs[i]);
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibs[i]);
        gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof dc_indices, dc_indices, GL_STATIC_DRAW);
    }
    /* the characters' meshes: positions and UVs, two words each a vertex, in buffers of their own */
    for (int m = 0; m < DC_CH_NMESH; ++m)
    {
        DcVertex v[DC_NVERT];
        dc_mesh(m, v);
        float pos[DC_NVERT * 2], uv[DC_NVERT * 2];
        for (int k = 0; k < DC_NVERT; ++k)
            pos[2 * k] = v[k].pos[0], pos[2 * k + 1] = v[k].pos[1], uv[2 * k] = v[k].uv[0], uv[2 * k + 1] = v[k].uv[1];
        GLuint* bufs[2][2] = { { &g_cvb[m], &g_cvb_tbo[m] }, { &g_cuv[m], &g_cuv_tbo[m] } };
        for (int j = 0; j < 2; ++j)
        {
            gl.GenBuffers(1, bufs[j][0]);
            gl.BindBuffer(GL_COPY_WRITE_BUFFER, *bufs[j][0]);
            gl.BufferData(GL_COPY_WRITE_BUFFER, sizeof pos, j ? uv : pos, GL_STATIC_DRAW);
            gl.GenTextures(1, bufs[j][1]);
            gl.BindTexture(GL_TEXTURE_BUFFER, *bufs[j][1]);
            gl.TexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, *bufs[j][0]);
        }
    }
    gl.BindTexture(GL_TEXTURE_BUFFER, 0);
    gl.BindVertexArray(g_vao);
    /* the samplers: filters and wraps in turn */
    gl.GenSamplers(DC_G_NSAMP + DC_CH_NSAMP, g_samps);
    for (int i = 0; i < DC_G_NSAMP + DC_CH_NSAMP; ++i)
    {
        GLint f = i & 1 ? GL_LINEAR : GL_NEAREST, w = i & 2 ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        gl.SamplerParameteri(g_samps[i], GL_TEXTURE_MIN_FILTER, f);
        gl.SamplerParameteri(g_samps[i], GL_TEXTURE_MAG_FILTER, f);
        gl.SamplerParameteri(g_samps[i], GL_TEXTURE_WRAP_S, w);
        gl.SamplerParameteri(g_samps[i], GL_TEXTURE_WRAP_T, i & 4 ? GL_MIRRORED_REPEAT : w);
    }
    /* the targets: the main one with depth, the others without */
    gl.GenFramebuffers(DC_NRT, g_fbo);
    gl.GenTextures(DC_NRT, g_rt_tex);
    gl.GenRenderbuffers(1, &g_depth_rb);
    gl.BindRenderbuffer(GL_RENDERBUFFER, g_depth_rb);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, DC_W, DC_H);
    for (int r = 0; r < DC_NRT; ++r)
    {
        gl.BindTexture(GL_TEXTURE_2D, g_rt_tex[r]);
        gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, g_rt_size[r][0], g_rt_size[r][1]);
        gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo[r]);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rt_tex[r], 0);
        if (r == DC_RT_MAIN)
            gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_depth_rb);
        GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE)
            return dc_log("GL: target %d incomplete (0x%x)", r, st), 0;
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.BindTexture(GL_TEXTURE_2D, 0);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.CullFace(GL_BACK);
    /* the uniform blocks' sources: filled once, the per-draw part patched */
    for (int i = 0; i < DC_G_URANGE / 4; ++i)
        g_ublock[i] = 0.001f * (float)(i % 97);
    for (int i = 0; i < DC_CH_STATE / 4; ++i)
        g_cstate[i] = 0.001f * (float)(i % 89);
    for (int i = 0; i < DC_CH_PAL / 4; ++i)
        g_cpal[i] = 0.6f + 0.004f * (float)(i % 100);
    GLenum e = gl.GetError();
    if (e)
        return dc_log("GL: error 0x%x setting up the game mode", e), 0;
    dc_log("GL: game mode up (ring 2 x %u KB, texture buffers up to %d words)", DC_RING_SEG >> 10, tbs);
    return 1;
}

/* what is bound, as gfx_gl.c's st_* keep it: a bind of what is bound already is left out */
static struct
{
    GLuint prog, tex[5][2], samp[5], ubo[3], ebo;
    GLintptr ubo_off[3];
    GLsizeiptr ubo_size[3];
    int unit, rt, blend, zwrite, zfunc, cull;
} S;

static int st_tex(int unit, GLenum target, GLuint name)
{
    int k = target == GL_TEXTURE_BUFFER;
    if (S.tex[unit][k] == name)
        return 0;
    if (S.unit != unit)
        gl.ActiveTexture(GL_TEXTURE0 + (GLenum)unit), S.unit = unit;
    gl.BindTexture(target, name);
    S.tex[unit][k] = name;
    return 1;
}

static int st_samp(int unit, GLuint s)
{
    if (S.samp[unit] == s)
        return 0;
    gl.BindSampler((GLuint)unit, s);
    S.samp[unit] = s;
    return 1;
}

static void st_ubo(int i, GLuint buf, GLintptr off, GLsizeiptr size)
{
    if (S.ubo[i] == buf && S.ubo_off[i] == off && S.ubo_size[i] == size)
        return;
    gl.BindBufferRange(GL_UNIFORM_BUFFER, (GLuint)i, buf, off, size);
    S.ubo[i] = buf, S.ubo_off[i] = off, S.ubo_size[i] = size;
    dc_cnt.ubo++;
}

static void st_ebo(GLuint b)
{
    if (S.ebo == b)
        return;
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, b);
    S.ebo = b;
    dc_cnt.ebo++;
}

static void st_prog(GLuint p)
{
    if (S.prog == p)
        return;
    gl.UseProgram(p);
    S.prog = p;
    dc_cnt.progs++;
}

static void set_rt(int r)
{
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo[r]);
    gl.Viewport(0, 0, g_rt_size[r][0], g_rt_size[r][1]);
    S.rt = r;
    dc_cnt.rt++;
}

static void toggle(int what)
{
    switch (what)
    {
    case DC_ST_BLEND:
        (S.blend = !S.blend) ? gl.Enable(GL_BLEND) : gl.Disable(GL_BLEND);
        break;
    case DC_ST_ZWRITE:
        gl.DepthMask((GLboolean)(S.zwrite = !S.zwrite));
        break;
    case DC_ST_ZFUNC:
        gl.DepthFunc((S.zfunc = !S.zfunc) ? GL_ALWAYS : GL_LEQUAL);
        break;
    case DC_ST_CULL:
        (S.cull = !S.cull) ? gl.Enable(GL_CULL_FACE) : gl.Disable(GL_CULL_FACE);
        break;
    }
    dc_cnt.state++;
}

/* a place in this frame's segment of the ring, the data copied in (gfx_gl.c's ring_put) */
static uint32_t ring_put(const void* src, uint32_t size, uint32_t align)
{
    uint32_t at = (g_seg_used + align - 1) & ~(align - 1);
    if (at + size + DC_G_URANGE > DC_RING_SEG) /* (can't happen with the plan's ~3 MB; wraps rather than overruns) */
        at = 0;
    g_seg_used = at + size;
    uint32_t off = g_seg * DC_RING_SEG + at;
    if (src)
        memcpy(g_ring_cpu + off, src, size);
    dc_cnt.ring_bytes += size;
    return off;
}

static int game_frame(uint32_t frame)
{
    /* this frame's segment, once the GPU is done with it (two frames back) */
    g_seg = frame & 1, g_seg_used = 0;
    if (g_ring_fence[g_seg])
    {
        gl.ClientWaitSync(g_ring_fence[g_seg], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        gl.DeleteSync(g_ring_fence[g_seg]);
        g_ring_fence[g_seg] = 0;
    }
    /* the binding state is taken as unknown at each frame's start (the other modes change it) */
    memset(&S, 0, sizeof S);
    S.unit = -1;
    gl.BindVertexArray(g_gvao);
    set_rt(DC_RT_MAIN);
    gl.Enable(GL_DEPTH_TEST), gl.DepthFunc(GL_LEQUAL), gl.DepthMask(GL_TRUE), gl.Disable(GL_BLEND), gl.Disable(GL_CULL_FACE);
    S.zwrite = 1;
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (st_samp(1, g_samps[0]))
        dc_cnt.samplers++;
    int draws = 0;
    for (int i = 0; i < DC_G_DRAWS; ++i)
    {
        const DcGameDraw* d = &dc_game[i];
        if (d->rt != S.rt)
        {
            set_rt(d->rt);
            if (d->clear)
                gl.Clear(GL_COLOR_BUFFER_BIT);
        }
        if (d->state)
            toggle(d->state);
        st_prog(g_gprog[d->prog]);
        dc_cnt.stage_tex += (uint32_t)st_tex(0, GL_TEXTURE_2D, g_tex[d->tex0]);
        dc_cnt.samplers += (uint32_t)st_samp(0, g_samps[d->samp0]);
        dc_cnt.stage_tex += (uint32_t)st_tex(1, GL_TEXTURE_2D, g_tex[d->tex1]);
        /* the vertices: a static buffer's, or written into the ring */
        int indexed = d->kind != DC_K_ARRAYS;
        int base;
        GLuint tbo;
        if (d->src == DC_G_NSTATIC)
        {
            int m = i % DC_G_NSTATIC;
            base = (int)(ring_put(indexed ? g_words_idx[m] : g_words_list[m],
                                  indexed ? sizeof g_words_idx[m] : sizeof g_words_list[m], 16) / 4);
            tbo = g_ring_tbo;
        }
        else
            base = indexed ? DC_G_ARRAY_VERTS * 4 : 0, tbo = g_stbo[d->src];
        dc_cnt.stream_tex += (uint32_t)st_tex(2, GL_TEXTURE_BUFFER, tbo);
        /* the uniforms: the block's source patched, then copied into the ring */
        dc_game_uniforms(i, base, g_ublock);
        st_ubo(0, g_ring, ring_put(g_ublock, DC_G_UWRITE, DC_UBO_STRIDE), DC_G_URANGE);
        if (d->kind == DC_K_ARRAYS)
            gl.DrawArrays(GL_TRIANGLES, 0, DC_G_ARRAY_VERTS);
        else if (d->kind == DC_K_RING_IDX)
        {
            uint32_t at = ring_put(dc_indices, sizeof dc_indices, 4);
            st_ebo(g_ring);
            gl.DrawElements(GL_TRIANGLES, DC_NIDX, GL_UNSIGNED_SHORT, (const void*)(uintptr_t)at);
        }
        else
        {
            st_ebo(g_ibs[d->ib]);
            gl.DrawElements(GL_TRIANGLES, DC_NIDX, GL_UNSIGNED_SHORT, (const void*)0);
        }
        ++draws;
    }
    /* the characters (gfx_chars_draw): on the main target, depth on, no blend or culling */
    if (S.rt != DC_RT_MAIN)
        set_rt(DC_RT_MAIN);
    if (!S.zwrite)
        toggle(DC_ST_ZWRITE);
    if (S.zfunc)
        toggle(DC_ST_ZFUNC);
    if (S.blend)
        toggle(DC_ST_BLEND);
    if (S.cull)
        toggle(DC_ST_CULL);
    st_prog(g_cprog);
    for (int c = 0; c < DC_CH_CHARS; ++c)
    {
        g_cstate[0] = -0.9f + (float)(c % 15) * 0.12f, g_cstate[1] = -0.75f - (float)(c / 15) * 0.15f;
        g_cstate[2] = 0.03f, g_cstate[3] = 0.05f;
        st_ubo(1, g_ring, ring_put(g_cstate, DC_CH_STATE, DC_UBO_STRIDE), DC_CH_STATE);
        st_ebo(g_ibs[c % DC_NIB]);
        for (int k = 0; k < DC_CH_ITEMS; ++k)
        {
            int m = (c * DC_CH_ITEMS + k) % DC_CH_NMESH;
            float item[8] = { 1.0f, 0.9f + 0.01f * (float)k, 1.0f, 1.0f, 0, 0, 0, 0 };
            st_ubo(0, g_ring, ring_put(item, DC_CH_ITEM, DC_UBO_STRIDE), DC_CH_ITEM);
            g_cpal[0] = 0.5f + 0.05f * (float)k;
            st_ubo(2, g_ring, ring_put(g_cpal, DC_CH_PAL, DC_UBO_STRIDE), DC_CH_PAL);
            dc_cnt.stream_tex += (uint32_t)st_tex(3, GL_TEXTURE_BUFFER, g_cvb_tbo[m]);
            dc_cnt.stream_tex += (uint32_t)st_tex(4, GL_TEXTURE_BUFFER, g_cuv_tbo[m]);
            dc_cnt.stage_tex += (uint32_t)st_tex(0, GL_TEXTURE_2D, g_tex[m % DC_NTEX]);
            dc_cnt.samplers += (uint32_t)st_samp(0, g_samps[DC_G_NSAMP + c % DC_CH_NSAMP]);
            if (k % 2 == 0)
                gl.DrawElementsInstanced(GL_TRIANGLES, DC_NIDX, GL_UNSIGNED_SHORT, (const void*)0, 2);
            else
                gl.DrawElements(GL_TRIANGLES, DC_NIDX, GL_UNSIGNED_SHORT, (const void*)0);
            ++draws;
        }
    }
    /* to the screen */
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo[DC_RT_MAIN]);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl.BlitFramebuffer(0, 0, DC_W, DC_H, 0, 0, DC_W, DC_H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    g_ring_fence[g_seg] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    dc_cnt.draws = (uint32_t)draws;
    return draws;
}

/* the other modes' state (game_frame changes it) */
static void simple_state(void)
{
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, DC_W, DC_H);
    gl.Disable(GL_DEPTH_TEST), gl.Disable(GL_BLEND), gl.Disable(GL_CULL_FACE);
    gl.BindVertexArray(g_vao);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindSampler(0, g_samp);
}

int main(int argc, char** argv)
{
    dc_init(argc, argv, "gl", "drawcost-gl.log");
    /* Mesa's disk shader cache beside the log, as the game puts it beside itself (drawcost.cfg may set another) */
    static char cache[600];
    snprintf(cache, sizeof cache, "%sshader-cache", dc_dir());
    mkdir(cache, 0777);
    setenv("MESA_SHADER_CACHE_DIR", cache, 0);
    dc_log("GL: MESA_SHADER_CACHE_DIR=%s", getenv("MESA_SHADER_CACHE_DIR"));
    if (!egl_up() || !scene_up() || !game_up())
    {
        dc_exit();
        return 1;
    }
    dc_log("GL: %s | %s | %s", (const char*)gl.GetString(GL_VENDOR), (const char*)gl.GetString(GL_RENDERER),
           (const char*)gl.GetString(GL_VERSION));
    const char* gt = getenv("mesa_glthread");
    dc_log("GL: mesa_glthread %s (Mesa's threads are listed as thr<n> below as they are made)", gt ? gt : "not set (Mesa's default)");
    eglSwapInterval(g_dpy, dc_cfg.vsync ? 1 : 0);
    dc_log_threads();
    uint32_t frame = 0;
    int was_game = 0;
    while (appletMainLoop())
    {
        int vs_changed;
        if (!dc_input(&vs_changed))
            break;
        if (vs_changed)
            eglSwapInterval(g_dpy, dc_cfg.vsync ? 1 : 0);
        DcFrame f = { 0 };
        int binds = dc_modes[dc_cfg.mode].binds;
        if (binds != DC_GAME && was_game)
            simple_state();
        was_game = binds == DC_GAME;
        f.start_ns = dc_now_ns();
        if (binds == DC_GAME)
            f.draws = game_frame(frame++);
        else
        {
            f.draws = dc_cfg.draws;
            gl.Clear(GL_COLOR_BUFFER_BIT);
            draws(f.draws, binds);
        }
        uint64_t t1 = dc_now_ns();
        f.record_ns = f.submit_ns = t1 - f.start_ns;
        if (dc_cfg.sync)
        {
            gl.Finish();
            f.done_ns = dc_now_ns() - f.start_ns;
        }
        uint64_t t2 = dc_now_ns();
        eglSwapBuffers(g_dpy, g_surf);
        f.present_ns = dc_now_ns() - t2;
        dc_frame(&f);
    }
    dc_exit();
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g_dpy, g_ctx);
    eglDestroySurface(g_dpy, g_surf);
    eglTerminate(g_dpy);
    return 0;
}
