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
    X(PFNGLDISABLEPROC, Disable)

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

static GLuint g_prog[DC_NPROG], g_tex[DC_NTEX], g_vb[DC_NVB], g_ubo, g_ib, g_vao, g_samp;

static int scene_up(void)
{
    for (int p = 0; p < DC_NPROG; ++p)
    {
        GLuint vs = shader(GL_VERTEX_SHADER, p, dc_vs_src), fs = shader(GL_FRAGMENT_SHADER, p, dc_fs_src);
        if (!vs || !fs)
            return 0;
        g_prog[p] = gl.CreateProgram();
        gl.AttachShader(g_prog[p], vs);
        gl.AttachShader(g_prog[p], fs);
        gl.LinkProgram(g_prog[p]);
        GLint ok = 0;
        gl.GetProgramiv(g_prog[p], GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = "";
            gl.GetProgramInfoLog(g_prog[p], sizeof log, NULL, log);
            return dc_log("GL: program %d not linked: %s", p, log), 0;
        }
    }
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

int main(int argc, char** argv)
{
    dc_init(argc, argv, "gl", "drawcost-gl.log");
    /* Mesa's disk shader cache beside the log, as the game puts it beside itself (drawcost.cfg may set another) */
    static char cache[600];
    snprintf(cache, sizeof cache, "%sshader-cache", dc_dir());
    mkdir(cache, 0777);
    setenv("MESA_SHADER_CACHE_DIR", cache, 0);
    dc_log("GL: MESA_SHADER_CACHE_DIR=%s", getenv("MESA_SHADER_CACHE_DIR"));
    if (!egl_up() || !scene_up())
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
    while (appletMainLoop())
    {
        int vs_changed;
        if (!dc_input(&vs_changed))
            break;
        if (vs_changed)
            eglSwapInterval(g_dpy, dc_cfg.vsync ? 1 : 0);
        DcFrame f = { 0 };
        f.draws = dc_cfg.draws;
        f.start_ns = dc_now_ns();
        gl.Clear(GL_COLOR_BUFFER_BIT);
        draws(f.draws, dc_modes[dc_cfg.mode].binds);
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
