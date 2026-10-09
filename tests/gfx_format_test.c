/* The Vulkan back end's texture formats without the game, offscreen: every value of the 16-bit
 * color formats, the other uncompressed formats, DXT1-5 blocks and mips, cube faces, partial uploads
 * and copies (overlapping ones too), texture and buffer lifetimes, the row-vector matrix convention,
 * culling, clears, depth and stencil. Expected pixels come from the D3D format definitions, not
 * from the back end's conversion code. gfx_state_test.c includes this file for its helpers.
 *
 * usage: tools/build_android.py gfxtest --test gfx_format, or libmain's --android-format-test
 *        (exit status 0 when every check passes) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"

static unsigned tests, checks, errors, first_errors;
static GfxTex* target;
static unsigned width, height;
static void identity(float* m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}

/* a texture, or exit 2 when the back end cannot make it */
static GfxTex* new_tex(int type, uint32_t fmt, unsigned w, unsigned h, unsigned levels, int use)
{
    GfxTex* t = gfx_tex_create(type, fmt, w, h, levels, use);
    if (!t)
    {
        fprintf(stderr, "texture format %#x %ux%u failed\n", fmt, w, h);
        exit(2);
    }
    return t;
}

static void base(GfxDraw* d)
{
    memset(d, 0, sizeof *d);
    identity(d->u.wvp);
    identity(d->u.wv);
    identity(d->u.wvit);
    for (int i = 0; i < 8; i++)
        identity(d->u.texm[i]);
    d->u.vp[2] = width;
    d->u.vp[3] = height;
    d->vp[2] = width;
    d->vp[3] = height;
    d->vp[5] = 0x3f800000;
    d->pipe.write_mask = 15;
    d->cull = 1;
    d->depth.zfunc = 4;
    d->u.tfactor[0] = d->u.tfactor[1] = d->u.tfactor[2] = d->u.tfactor[3] = 1;
}

struct vertex
{
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
};

static void sampled(GfxTex* tex, unsigned level, int snorm)
{
    GfxDraw d;
    base(&d);
    d.vs.rhw = 1;
    d.vs.ntex = 1;
    d.vs.el[0] = (GfxElem){1, 0, GFX_FLOAT4, 0};
    d.vs.el[5] = (GfxElem){1, 0, GFX_D3DCOLOR, 0};
    d.vs.el[7] = (GfxElem){1, 0, GFX_FLOAT2, 0};
    d.u.stride[0] = sizeof(struct vertex);
    d.u.offset[5] = 16;
    d.u.offset[7] = 20;
    d.fs.nstages = 1;
    d.fs.st[0] = (GfxStage){2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2};
    d.tex[0] = tex;
    d.samp[0] = (GfxSampler){.addr_u = 3,
                             .addr_v = 3,
                             .addr_w = 3,
                             .mag = 1,
                             .min = 1,
                             .mip = 1,
                             .max_aniso = 1,
                             .max_level = level,
                             .lod_cap = level + 1};
    if (snorm)
    {
        d.fs.st[0].cop = 25;
        d.fs.st[0].ca1 = 2;
        d.fs.st[0].ca2 = 3;
        d.fs.st[0].ca0 = 0;
        d.u.tfactor[0] = d.u.tfactor[1] = d.u.tfactor[2] = .5f;
    }
    uint32_t color = snorm ? 0xff808080 : 0xffffffff;
    struct vertex v[] = {{-.5f, -.5f, .5f, 1, color, 0, 0},
                         {width - .5f, -.5f, .5f, 1, color, 1, 0},
                         {-.5f, height - .5f, .5f, 1, color, 0, 1},
                         {width - .5f, height - .5f, .5f, 1, color, 1, 1}};
    d.data[0] = v;
    d.size[0] = sizeof v;
    d.prim = GFX_TRIANGLESTRIP;
    d.count = 2;
    gfx_draw(&d);
}

static void begin(unsigned w, unsigned h)
{
    if (target)
        gfx_tex_destroy(target);
    width = w;
    height = h;
    target = new_tex(GFX_TEX_2D, 21, w, h, 1, GFX_USE_RT);
    gfx_set_targets(target, 0, 0, NULL);
    uint32_t vp[] = {0, 0, w, h, 0, 0x3f800000};
    gfx_clear(0, NULL, 1, 0xff2d517f, 1, 0, vp);
}

static void compare(const char* name, const uint32_t* expected, unsigned tolerance)
{
    size_t n = (size_t)width * height;
    uint32_t* got = calloc(n, 4);
    gfx_tex_read(target, 0, 0, got, width * 4);
    unsigned before = errors;
    for (size_t i = 0; i < n; i++)
        for (unsigned channel = 0; channel < 4; channel++)
        {
            int a = (got[i] >> (8 * channel)) & 255, b = (expected[i] >> (8 * channel)) & 255;
            checks++;
            if (abs(a - b) > (int)tolerance)
            {
                errors++;
                if (first_errors++ < 8)
                    fprintf(stderr, "%s pixel%zu channel%u got%d expected%d\n", name, i, channel, a, b);
            }
        }
    printf("{\"event\":\"format-case\",\"name\":\"%s\",\"checks\":%zu,\"mismatches\":%u}\n", name, n * 4,
           errors - before);
    free(got);
    tests++;
}

static unsigned expand(unsigned value, unsigned bits)
{
    return bits == 5 ? (value << 3) | (value >> 2) : bits == 6 ? (value << 2) | (value >> 4) : value * 17;
}

static uint32_t expected16(unsigned fmt, unsigned x)
{
    unsigned r, g, b, a;
    if (fmt == 23)
    {
        r = expand((x >> 11) & 31, 5);
        g = expand((x >> 5) & 63, 6);
        b = expand(x & 31, 5);
        a = 255;
    }
    else if (fmt == 26)
    {
        r = expand((x >> 8) & 15, 4);
        g = expand((x >> 4) & 15, 4);
        b = expand(x & 15, 4);
        a = expand((x >> 12) & 15, 4);
    }
    else
    {
        r = expand((x >> 10) & 31, 5);
        g = expand((x >> 5) & 31, 5);
        b = expand(x & 31, 5);
        a = fmt == 24 || x & 0x8000 ? 255 : 0;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

static void sixteen(unsigned fmt)
{
    begin(256, 256);
    uint16_t* src = malloc(65536 * 2);
    uint32_t* expected = malloc(65536 * 4);
    for (unsigned i = 0; i < 65536; i++)
    {
        src[i] = i;
        expected[i] = expected16(fmt, i);
    }
    GfxTex* t = new_tex(GFX_TEX_2D, fmt, 256, 256, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, src, 512);
    sampled(t, 0, 0);
    char name[32];
    snprintf(name, sizeof name, "all65536-format%u", fmt);
    compare(name, expected, 0);
    gfx_tex_destroy(t);
    free(src);
    free(expected);
}

static void scalar(unsigned fmt)
{
    unsigned dim = fmt == 28 || fmt == 50 ? 16 : 256;
    begin(dim, dim);
    unsigned n = dim * dim, bpp = fmt == 28 || fmt == 50 ? 1 : fmt == 51 || fmt == 60 ? 2 : 4;
    unsigned char* src = malloc((size_t)n * bpp);
    uint32_t* expected = malloc((size_t)n * 4);
    for (unsigned i = 0; i < n; i++)
    {
        unsigned lo = i & 255, hi = i >> 8;
        if (bpp == 1)
            src[i] = lo;
        else if (bpp == 2)
        {
            src[2 * i] = lo;
            src[2 * i + 1] = hi;
        }
        else
        {
            uint32_t v = i * 1664525u + 1013904223u;
            memcpy(src + 4 * i, &v, 4);
            expected[i] = fmt == 22 ? (v | 0xff000000) : v;
            continue;
        }
        if (fmt == 28)
            expected[i] = lo << 24;
        else if (fmt == 50)
            expected[i] = 0xff000000 | lo * 0x10101;
        else if (fmt == 51)
            expected[i] = hi << 24 | lo * 0x10101;
        else
        {
            int u = (int8_t)lo, v = (int8_t)hi;
            float r = fmaxf(-1, (float)u / 127) * .5f + 128.0f / 255,
                  g = fmaxf(-1, (float)v / 127) * .5f + 128.0f / 255;
            unsigned rr = (unsigned)lroundf(fminf(1, r) * 255), gg = (unsigned)lroundf(fminf(1, g) * 255);
            expected[i] = 0xff000080 | rr << 16 | gg << 8;
        }
    }
    GfxTex* t = new_tex(GFX_TEX_2D, fmt, dim, dim, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, src, dim * bpp);
    sampled(t, 0, fmt == 60);
    char name[32];
    snprintf(name, sizeof name, "format%u", fmt);
    compare(name, expected, fmt == 60 ? 1 : 0);
    gfx_tex_destroy(t);
    free(src);
    free(expected);
}

static void mips_rect(void)
{
    GfxTex* t = new_tex(GFX_TEX_2D, 21, 16, 16, 5, GFX_USE_SAMPLE);
    for (unsigned l = 0; l < 5; l++)
    {
        unsigned dim = 16 >> l;
        begin(dim, dim);
        uint32_t data[256], expected[256];
        for (unsigned i = 0; i < dim * dim; i++)
            data[i] = expected[i] = 0xff002d91u + l * 0x00220b07;
        gfx_tex_upload(t, 0, l, data, dim * 4);
        sampled(t, l, 0);
        compare("original-mip", expected, 0);
    }
    begin(16, 16);
    uint32_t expected[256], clear[256], patch[12];
    for (unsigned i = 0; i < 256; i++)
        expected[i] = clear[i] = 0xff1f3e7d;
    for (unsigned i = 0; i < 12; i++)
        patch[i] = 0xff90a031u + i;
    gfx_tex_upload(t, 0, 0, clear, 64);
    gfx_tex_upload_rect(t, 0, 0, 5, 7, 4, 3, patch, 16);
    for (unsigned y = 0; y < 3; y++)
        for (unsigned x = 0; x < 4; x++)
            expected[(7 + y) * 16 + 5 + x] = patch[y * 4 + x];
    sampled(t, 0, 0);
    compare("rect-source-origin", expected, 0);
    gfx_tex_destroy(t);
}

static void nonidentity(void)
{
    begin(32, 32);
    GfxDraw d;
    base(&d);
    d.vs.el[0] = (GfxElem){1, 0, GFX_FLOAT3, 0};
    d.vs.el[5] = (GfxElem){1, 0, GFX_D3DCOLOR, 0};
    d.u.stride[0] = 16;
    d.u.offset[5] = 12;
    d.fs.nstages = 1;
    d.fs.st[0] = (GfxStage){2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2};
    d.u.wvp[0] = .5f;
    d.u.wvp[5] = .5f;
    d.u.wvp[12] = .25f;
    d.u.wvp[13] = -.25f;
    struct
    {
        float x, y, z;
        uint32_t c;
    } v[] = {{-1, 1, .5f, 0xff31a275}, {1, 1, .5f, 0xff31a275}, {-1, -1, .5f, 0xff31a275}, {1, -1, .5f, 0xff31a275}};
    d.data[0] = v;
    d.size[0] = sizeof v;
    d.prim = GFX_TRIANGLESTRIP;
    d.count = 2;
    gfx_draw(&d);
    uint32_t expected[1024];
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
            expected[y * 32 + x] = (x >= 12 && x < 28 && y >= 12 && y < 28) ? 0xff31a275 : 0xff2d517f;
    compare("nonidentity-row-vector-matrix", expected, 0);
}

static void ui_color(GfxDraw* d)
{
    base(d);
    d->vs.rhw = 1;
    d->vs.el[0] = (GfxElem){1, 0, GFX_FLOAT4, 0};
    d->vs.el[5] = (GfxElem){1, 0, GFX_D3DCOLOR, 0};
    d->u.stride[0] = sizeof(struct vertex);
    d->u.offset[5] = 16;
    d->fs.nstages = 1;
    d->fs.st[0] = (GfxStage){2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2};
    d->prim = GFX_TRIANGLESTRIP;
    d->count = 2;
}

static void rectangle(struct vertex* v, float x0, float x1, uint32_t color)
{
    v[0] = (struct vertex){x0, -.5f, .5f, 1, color, 0, 0};
    v[1] = (struct vertex){x1, -.5f, .5f, 1, color, 1, 0};
    v[2] = (struct vertex){x0, 31.5f, .5f, 1, color, 0, 1};
    v[3] = (struct vertex){x1, 31.5f, .5f, 1, color, 1, 1};
}

static void lifetime(int which)
{
    begin(32, 32);
    GfxDraw d;
    ui_color(&d);
    struct vertex v[4];
    rectangle(v, -.5f, 15.5f, 0xff123456);
    d.data[0] = v;
    d.size[0] = sizeof v;
    GfxBuf* b = NULL;
    GfxTex* t = NULL;
    if (which == 1)
    {
        b = gfx_buf_create(sizeof v);
        if (!b)
            exit(2);
        gfx_buf_upload(b, v, sizeof v);
        d.buf[0] = b;
        d.data[0] = NULL;
    }
    if (which == 2)
    {
        t = new_tex(GFX_TEX_2D, 21, 1, 1, 1, GFX_USE_SAMPLE);
        uint32_t color = 0xff123456;
        gfx_tex_upload(t, 0, 0, &color, 4);
        d.vs.ntex = 1;
        d.vs.el[7] = (GfxElem){1, 0, GFX_FLOAT2, 0};
        d.u.offset[7] = 20;
        d.fs.st[0] = (GfxStage){2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2};
        d.tex[0] = t;
        d.samp[0] = (GfxSampler){.addr_u = 3, .addr_v = 3, .addr_w = 3, .mag = 1, .min = 1};
    }
    gfx_draw(&d);
    rectangle(v, 15.5f, 31.5f, 0xff9abcde);
    if (b)
        gfx_buf_upload(b, v, sizeof v);
    if (t)
    {
        uint32_t color = 0xff9abcde;
        gfx_tex_upload(t, 0, 0, &color, 4);
    }
    gfx_draw(&d);
    /* destroyed before the readback: the pending draws must keep what they use */
    if (b)
        gfx_buf_destroy(b);
    if (t)
        gfx_tex_destroy(t);
    uint32_t expected[1024];
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
            expected[y * 32 + x] = x < 16 ? 0xff123456 : 0xff9abcde;
    compare(which == 0   ? "dynamic-bytes-copied-at-draw"
            : which == 1 ? "static-buffer-rewrite-and-delete"
                         : "texture-rewrite-and-delete",
            expected, 0);
}

static void culling(unsigned cull, int reversed)
{
    begin(32, 32);
    GfxDraw d;
    ui_color(&d);
    struct vertex v[] = {
        {4, 4, .5f, 1, 0xff37b493, 0, 0}, {28, 4, .5f, 1, 0xff37b493, 0, 0}, {4, 28, .5f, 1, 0xff37b493, 0, 0}};
    if (reversed)
    {
        struct vertex tmp = v[1];
        v[1] = v[2];
        v[2] = tmp;
    }
    d.cull = cull;
    d.data[0] = v;
    d.size[0] = sizeof v;
    d.prim = GFX_TRIANGLELIST;
    d.count = 1;
    gfx_draw(&d);
    uint32_t pixels[1024];
    gfx_tex_read(target, 0, 0, pixels, 128);
    unsigned before = errors;
    int retained = reversed ? cull == 2 : cull == 3;
    uint32_t expected = retained ? 0xff37b493 : 0xff2d517f;
    for (unsigned ch = 0; ch < 4; ch++)
    {
        checks += 2;
        errors += ((pixels[8 * 32 + 8] >> (8 * ch)) & 255) != ((expected >> (8 * ch)) & 255);
        errors += ((pixels[30 * 32 + 30] >> (8 * ch)) & 255) != ((0xff2d517f >> (8 * ch)) & 255);
    }
    printf("{\"event\":\"format-case\",\"name\":\"cull%u-reverse%d\",\"checks\":8,\"mismatches\":%u}\n", cull, reversed,
           errors - before);
    tests++;
}

static void clear_clips(void)
{
    begin(32, 32);
    uint32_t vp[] = {8, 8, 16, 16, 0, 0x3f800000}, expected[1024];
    gfx_clear(0, NULL, 1, 0xff602a93, 1, 0, vp);
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
            expected[y * 32 + x] = x >= 8 && x < 24 && y >= 8 && y < 24 ? 0xff602a93 : 0xff2d517f;
    compare("clear-clipped-to-viewport", expected, 0);
    int32_t rects[] = {-5, 11, 15, 40, 20, -5, 50, 18};
    gfx_clear(2, rects, 1, 0xff6fb80e, 1, 0, vp);
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
            if (x >= 8 && x < 24 && y >= 8 && y < 24 && ((x < 15 && y >= 11) || (x >= 20 && y < 18)))
                expected[y * 32 + x] = 0xff6fb80e;
    compare("clear-rects-clipped-to-viewport-and-target", expected, 0);
}

static void depth_stencil(int larger)
{
    begin(32, 32);
    GfxTex* depth = new_tex(GFX_TEX_2D, 75, larger ? 64 : 32, larger ? 64 : 32, 1, GFX_USE_DEPTH);
    gfx_set_targets(target, 0, 0, depth);
    uint32_t vp[] = {0, 0, 32, 32, 0, 0x3f800000};
    gfx_clear(0, NULL, 3, 0xff2d517f, 1, 0, vp);
    GfxDraw d;
    ui_color(&d);
    struct vertex v[4];
    rectangle(v, -.5f, 31.5f, 0xff123456);
    for (int i = 0; i < 4; i++)
        v[i].z = .75f;
    d.data[0] = v;
    d.size[0] = sizeof v;
    d.depth.zenable = d.depth.zwrite = 1;
    d.depth.zfunc = 4;
    gfx_draw(&d);
    rectangle(v, -.5f, 15.5f, 0xff53cd98);
    for (int i = 0; i < 4; i++)
        v[i].z = .9f;
    gfx_draw(&d);
    rectangle(v, 15.5f, 31.5f, 0xff94b526);
    for (int i = 0; i < 4; i++)
        v[i].z = .25f;
    gfx_draw(&d);
    uint32_t expected[1024];
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
            expected[y * 32 + x] = x < 16 ? 0xff123456 : 0xff94b526;
    compare(larger ? "depth-larger-than-color" : "depth-test-and-write", expected, 0);
    gfx_clear(0, NULL, 7, 0xff2d517f, 1, 0, vp);
    rectangle(v, -.5f, 15.5f, 0xff123456);
    d.depth.zenable = d.depth.zwrite = 0;
    d.depth.stencil = 1;
    d.depth.sfunc = 8;
    d.depth.sfail = d.depth.szfail = 1;
    d.depth.spass = 3;
    d.depth.sread = d.depth.swrite = 255;
    d.stencil_ref = 5;
    gfx_draw(&d);
    rectangle(v, -.5f, 31.5f, 0xff69ac35);
    d.depth.sfunc = 3;
    d.depth.spass = 1;
    d.depth.swrite = 0;
    gfx_draw(&d);
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
            expected[y * 32 + x] = x < 16 ? 0xff69ac35 : 0xff2d517f;
    compare(larger ? "stencil-larger-than-color" : "stencil-with-depth-disabled", expected, 0);
    gfx_tex_destroy(depth);
}

/* the 8x8 target filled from one cube face, sampled along its axis */
static void cube_sampled(GfxTex* t, unsigned face, GfxSampler samp)
{
    static const float axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    struct cube_vertex
    {
        float x, y, z, rhw;
        uint32_t color;
        float u, v, w;
    } v[4];
    GfxDraw d;
    ui_color(&d);
    d.vs.ntex = 1;
    d.vs.el[7] = (GfxElem){1, 0, GFX_FLOAT3, 0};
    d.u.stride[0] = sizeof(struct cube_vertex);
    d.u.offset[7] = 20;
    d.fs.st[0] = (GfxStage){2, 2, 1, 1, 2, 2, 1, 1, 1, 2, 0, 3};
    d.tex[0] = t;
    d.samp[0] = samp;
    for (unsigned i = 0; i < 4; i++)
        v[i] = (struct cube_vertex){i & 1 ? 7.5f : -.5f, i & 2 ? 7.5f : -.5f, .5f,           1,
                                    0xffffffff,          axes[face][0],       axes[face][1], axes[face][2]};
    d.data[0] = v;
    d.size[0] = sizeof v;
    gfx_draw(&d);
}

static void cube_faces(void)
{
    GfxTex* t = new_tex(GFX_TEX_CUBE, 21, 1, 1, 1, GFX_USE_SAMPLE);
    uint32_t colors[] = {0xff28394a, 0xff596a7b, 0xff8c9dae, 0xffbfc0d1, 0xffd2e3f4, 0xff142536};
    for (unsigned face = 0; face < 6; face++)
        gfx_tex_upload(t, face, 0, &colors[face], 4);
    for (unsigned face = 0; face < 6; face++)
    {
        begin(8, 8);
        cube_sampled(t, face, (GfxSampler){.addr_u = 3, .addr_v = 3, .addr_w = 3, .mag = 1, .min = 1});
        uint32_t expected[64];
        for (unsigned i = 0; i < 64; i++)
            expected[i] = colors[face];
        char name[32];
        snprintf(name, sizeof name, "cube-face%u", face);
        compare(name, expected, 0);
    }
    gfx_tex_destroy(t);
}

static void copy_rect(void)
{
    begin(16, 16);
    GfxTex* a = new_tex(GFX_TEX_2D, 21, 16, 16, 1, GFX_USE_SAMPLE);
    GfxTex* b = new_tex(GFX_TEX_2D, 21, 16, 16, 1, GFX_USE_SAMPLE);
    uint32_t source[256], dest[256], expected[256];
    for (unsigned i = 0; i < 256; i++)
    {
        source[i] = 0xff234681u + i * 0x10101;
        dest[i] = expected[i] = 0xff192837;
    }
    gfx_tex_upload(a, 0, 0, source, 64);
    gfx_tex_upload(b, 0, 0, dest, 64);
    gfx_copy(a, 0, 0, 2, 4, 7, 5, b, 0, 0, 6, 8);
    for (unsigned y = 0; y < 5; y++)
        for (unsigned x = 0; x < 7; x++)
            expected[(8 + y) * 16 + 6 + x] = source[(4 + y) * 16 + 2 + x];
    sampled(b, 0, 0);
    gfx_tex_destroy(a);
    gfx_tex_destroy(b);
    compare("gpu-copy-independent-origins", expected, 0);
}

static void dxt_block(unsigned kind, int transparent, int alpha_reverse)
{
    begin(4, 4);
    unsigned char block[16] = {0};
    unsigned char* rgb = block + (kind == 1 ? 0 : 8);
    unsigned c0 = transparent ? 0x001f : 0xf800, c1 = transparent ? 0xf800 : 0x001f;
    rgb[0] = c0;
    rgb[1] = c0 >> 8;
    rgb[2] = c1;
    rgb[3] = c1 >> 8;
    uint32_t indices = 0;
    for (unsigned i = 0; i < 16; i++)
        indices |= (i & 3) << (2 * i);
    memcpy(rgb + 4, &indices, 4);
    unsigned palette[4] = {0xffff0000, 0xff0000ff, 0xffaa0055, 0xff5500aa};
    if (transparent)
    {
        palette[0] = 0xff0000ff;
        palette[1] = 0xffff0000;
        palette[2] = 0xff7f007f;
        palette[3] = 0;
    }
    unsigned alpha[8] = {0};
    if (kind == 2 || kind == 3)
    {
        for (unsigned i = 0; i < 8; i++)
            block[i] = (2 * i) | ((2 * i + 1) << 4);
    }
    if (kind == 4 || kind == 5)
    {
        alpha[0] = alpha_reverse ? 0 : 255;
        alpha[1] = alpha_reverse ? 255 : 0;
        block[0] = alpha[0];
        block[1] = alpha[1];
        if (alpha[0] > alpha[1])
            for (unsigned i = 2; i < 8; i++)
                alpha[i] = ((8 - i) * alpha[0] + (i - 1) * alpha[1]) / 7;
        else
        {
            for (unsigned i = 2; i < 6; i++)
                alpha[i] = ((6 - i) * alpha[0] + (i - 1) * alpha[1]) / 5;
            alpha[6] = 0;
            alpha[7] = 255;
        }
        uint64_t abits = 0;
        for (unsigned i = 0; i < 16; i++)
            abits |= (uint64_t)(i & 7) << (3 * i);
        for (unsigned i = 0; i < 6; i++)
            block[2 + i] = abits >> (8 * i);
    }
    uint32_t fmt = 0x00545844u | ((uint32_t)('0' + kind) << 24);
    GfxTex* t = new_tex(GFX_TEX_2D, fmt, 4, 4, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, block, kind == 1 ? 8 : 16);
    sampled(t, 0, 0);
    uint32_t expected[16];
    for (unsigned i = 0; i < 16; i++)
    {
        expected[i] = palette[i & 3];
        if (kind == 2 || kind == 3)
            expected[i] = (expected[i] & 0xffffff) | (i * 17) << 24;
        if (kind == 4 || kind == 5)
            expected[i] = (expected[i] & 0xffffff) | alpha[i & 7] << 24;
    }
    char name[48];
    snprintf(name, sizeof name, "DXT%u-transparent%d-alphareverse%d", kind, transparent, alpha_reverse);
    compare(name, expected, 1);
    gfx_tex_destroy(t);
}

static void dxt_mips(void)
{
    GfxTex* t = new_tex(GFX_TEX_2D, 0x31545844, 16, 16, 5, GFX_USE_SAMPLE);
    unsigned colors[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0};
    for (unsigned l = 0; l < 5; l++)
    {
        unsigned dim = 16 >> l, blocks = (dim + 3) / 4;
        unsigned char data[128] = {0};
        for (unsigned i = 0; i < blocks * blocks; i++)
        {
            data[8 * i] = colors[l];
            data[8 * i + 1] = colors[l] >> 8;
            data[8 * i + 2] = colors[l];
            data[8 * i + 3] = colors[l] >> 8;
        }
        gfx_tex_upload(t, 0, l, data, blocks * 8);
        begin(dim, dim);
        sampled(t, l, 0);
        uint32_t expected[256];
        for (unsigned i = 0; i < dim * dim; i++)
            expected[i] = expected16(23, colors[l]);
        compare("DXT1-original-mip16-to1", expected, 0);
    }
    gfx_tex_destroy(t);
}

/* a level's bytes read back, as D3D stores them, against expected */
static void bytes_compare(const char* name, const unsigned char* got, const unsigned char* expected, unsigned n)
{
    unsigned before = errors;
    for (unsigned i = 0; i < n; i++)
    {
        checks++;
        if (got[i] != expected[i])
        {
            errors++;
            if (first_errors++ < 8)
                fprintf(stderr, "%s byte%u got%u expected%u\n", name, i, got[i], expected[i]);
        }
    }
    printf("{\"event\":\"format-case\",\"name\":\"%s\",\"checks\":%u,\"mismatches\":%u}\n", name, n, errors - before);
    tests++;
}

/* An overlapping copy within one texture: the bytes read back check D3D's storage, sampling
 * checks what the GPU holds. */
static void self_copy(unsigned fmt)
{
    unsigned kind = fmt >> 24, compressed = (fmt & 0xffffff) == 0x545844;
    unsigned w = compressed ? 12 : 8, h = 8,
             bpp = fmt == 28 || fmt == 50                  ? 1
                   : fmt == 51 || (fmt >= 23 && fmt <= 26) ? 2
                                                           : 4;
    unsigned block = kind == '1' ? 8 : 16, pitch = compressed ? 3 * block : w * bpp, rows = compressed ? 2 : h,
             n = pitch * rows;
    unsigned char src[384] = {0}, expected_bytes[384] = {0}, got[384] = {0};
    uint32_t original[96], expected_pixels[96];
    if (compressed)
    {
        unsigned colors[] = {0xf800, 0x07e0, 0x001f, 0xffe0, 0x07ff, 0xf81f};
        for (unsigned by = 0; by < 2; by++)
            for (unsigned bx = 0; bx < 3; bx++)
            {
                unsigned id = by * 3 + bx;
                unsigned char *out = src + by * pitch + bx * block, *rgb = out + (kind == '1' ? 0 : 8);
                rgb[0] = colors[id];
                rgb[1] = colors[id] >> 8;
                rgb[2] = colors[id];
                rgb[3] = colors[id] >> 8;
                unsigned a = 255;
                if (kind == '2' || kind == '3')
                {
                    for (unsigned j = 0; j < 8; j++)
                        out[j] = (id + 2) | ((id + 2) << 4);
                    a = (id + 2) * 17;
                }
                else if (kind == '4' || kind == '5')
                {
                    a = 31 + id * 37;
                    out[0] = a;
                    out[1] = 255 - a;
                }
                uint32_t color = (expected16(23, colors[id]) & 0xffffff) | a << 24;
                for (unsigned yy = 0; yy < 4; yy++)
                    for (unsigned xx = 0; xx < 4; xx++)
                        original[(by * 4 + yy) * w + bx * 4 + xx] = color;
            }
    }
    else
        for (unsigned i = 0; i < w * h; i++)
        {
            if (bpp == 1)
            {
                src[i] = i * 3 + 11;
                original[i] = fmt == 28 ? (uint32_t)src[i] << 24 : 0xff000000 | src[i] * 0x10101u;
            }
            else if (bpp == 2)
            {
                unsigned value = i * 997u + 0x3591u;
                if (fmt == 24)
                    value |= 0x8000u;
                src[2 * i] = value;
                src[2 * i + 1] = value >> 8;
                original[i] =
                    fmt == 51 ? (uint32_t)src[2 * i + 1] << 24 | src[2 * i] * 0x10101u : expected16(fmt, value & 65535);
            }
            else
            {
                uint32_t value = 0x31527a91u + i * 0x00071923u;
                memcpy(src + 4 * i, &value, 4);
                original[i] = fmt == 22 ? value | 0xff000000u : value;
            }
        }
    memcpy(expected_bytes, src, n);
    memcpy(expected_pixels, original, w * h * 4);
    unsigned sx = 0, sy = 0, cw = compressed ? 8 : 5, ch = compressed ? 8 : 6, dx = compressed ? 4 : 2,
             dy = compressed ? 0 : 1;
    if (compressed)
        for (unsigned y = 0; y < 2; y++)
            memcpy(expected_bytes + y * pitch + block, src + y * pitch, 2 * block);
    else
        for (unsigned y = 0; y < ch; y++)
            memcpy(expected_bytes + (dy + y) * pitch + dx * bpp, src + (sy + y) * pitch + sx * bpp, cw * bpp);
    for (unsigned y = 0; y < ch; y++)
        for (unsigned x = 0; x < cw; x++)
            expected_pixels[(dy + y) * w + dx + x] = original[(sy + y) * w + sx + x];
    GfxTex* t = new_tex(GFX_TEX_2D, fmt, w, h, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, src, pitch);
    gfx_copy(t, 0, 0, sx, sy, cw, ch, t, 0, 0, dx, dy);
    gfx_tex_read(t, 0, 0, got, pitch);
    char name[64];
    snprintf(name, sizeof name, "overlap-selfcopy-fmt%u-original-bytes", fmt);
    bytes_compare(name, got, expected_bytes, n);
    begin(w, h);
    sampled(t, 0, 0);
    snprintf(name, sizeof name, "overlap-selfcopy-fmt%u-rendered", fmt);
    compare(name, expected_pixels, 0);
    gfx_tex_destroy(t);
}

static void recreate_after_sync(void)
{
    begin(32, 32);
    for (unsigned round = 0; round < 12; round++)
    {
        uint32_t color = 0xff284967u + round * 0x00090b0du;
        GfxTex* t = new_tex(GFX_TEX_2D, 21, 1, 1, 1, GFX_USE_SAMPLE);
        gfx_tex_upload(t, 0, 0, &color, 4);
        sampled(t, 0, 0);
        gfx_tex_destroy(t);
        uint32_t expected[1024];
        for (unsigned i = 0; i < 1024; i++)
            expected[i] = color;
        char name[64];
        snprintf(name, sizeof name, "texture-handle-recreate-after-sync-%u", round);
        compare(name, expected, 0);
    }
}

static void fallback_bindings(void)
{
    begin(32, 32);
    GfxDraw d;
    struct vertex v[4];
    ui_color(&d);
    rectangle(v, -.5f, 31.5f, 0xff79b524);
    d.data[0] = v;
    d.size[0] = sizeof v;
    d.tex[7] = target;
    gfx_draw(&d);
    uint32_t expected[1024];
    for (unsigned i = 0; i < 1024; i++)
        expected[i] = 0xff79b524;
    compare("unused-stage-active-RT-binding-does-not-sample", expected, 0);
    begin(32, 32);
    sampled(NULL, 0, 0);
    for (unsigned i = 0; i < 1024; i++)
        expected[i] = 0;
    compare("requested-unbound-texture-samples-zero", expected, 0);
}

/* one DXT<kind> block of a single color (and alpha), its A8R8G8B8 value returned */
static uint32_t solid_block(unsigned kind, unsigned id, unsigned char* out)
{
    static const unsigned colors[] = {0xf800, 0x07e0, 0x001f, 0xffe0, 0x07ff, 0xf81f, 0xffff, 0x8410, 0x4208};
    unsigned block = kind == 1 ? 8 : 16, c = colors[id % 9], a = 255;
    memset(out, 0, block);
    unsigned char* rgb = out + (kind == 1 ? 0 : 8);
    rgb[0] = rgb[2] = c;
    rgb[1] = rgb[3] = c >> 8;
    if (kind == 2 || kind == 3)
    {
        unsigned nibble = 1 + id % 14;
        memset(out, nibble * 17, 8);
        a = nibble * 17;
    }
    else if (kind == 4 || kind == 5)
    {
        a = 19 + (id * 23) % 220;
        out[0] = a;
        out[1] = 255 - a;
    }
    return (expected16(23, c) & 0xffffffu) | (a << 24);
}

/* DXT rectangles: an upload's source pointer names the rectangle's origin, the blocks around it
 * survive, and an overlapping copy reads all its source before writing. */
static void dxt_regions(unsigned kind)
{
    unsigned fmt = 0x00545844u | (('0' + kind) << 24), block = kind == 1 ? 8 : 16, pitch = 3 * block, n = pitch * 3;
    unsigned char original[144] = {0}, expected[144], got[144] = {0}, patch[48] = {0}, snapshot[144];
    uint32_t pixels[144], before[144];
    for (unsigned by = 0; by < 3; by++)
        for (unsigned bx = 0; bx < 3; bx++)
        {
            uint32_t c = solid_block(kind, by * 3 + bx, original + by * pitch + bx * block);
            for (unsigned y = 0; y < 4; y++)
                for (unsigned x = 0; x < 4; x++)
                    pixels[(by * 4 + y) * 12 + bx * 4 + x] = c;
        }
    memcpy(expected, original, n);
    GfxTex* t = new_tex(GFX_TEX_2D, fmt, 12, 12, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, original, pitch);
    for (unsigned bx = 0; bx < 2; bx++)
    {
        uint32_t c = solid_block(kind, 10 + bx, patch + bx * block);
        for (unsigned y = 0; y < 4; y++)
            for (unsigned x = 0; x < 4; x++)
                pixels[(4 + y) * 12 + 4 + bx * 4 + x] = c;
    }
    gfx_tex_upload_rect(t, 0, 0, 4, 4, 8, 4, patch, 3 * block);
    memcpy(expected + pitch + block, patch, 2 * block);
    gfx_tex_read(t, 0, 0, got, pitch);
    char name[96];
    snprintf(name, sizeof name, "DXT%u-padded-rect-source-origin-original", kind);
    bytes_compare(name, got, expected, n);
    begin(12, 12);
    sampled(t, 0, 0);
    snprintf(name, sizeof name, "DXT%u-padded-rect-source-origin-rendered", kind);
    compare(name, pixels, 0);
    memcpy(snapshot, expected, n);
    memcpy(before, pixels, sizeof pixels);
    gfx_copy(t, 0, 0, 4, 0, 8, 8, t, 0, 0, 0, 4);
    for (unsigned y = 0; y < 2; y++)
        memcpy(expected + (y + 1) * pitch, snapshot + y * pitch + block, 2 * block);
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++)
            pixels[(y + 4) * 12 + x] = before[y * 12 + x + 4];
    gfx_tex_read(t, 0, 0, got, pitch);
    snprintf(name, sizeof name, "DXT%u-diagonal-overlap-original", kind);
    bytes_compare(name, got, expected, n);
    begin(12, 12);
    sampled(t, 0, 0);
    snprintf(name, sizeof name, "DXT%u-diagonal-overlap-rendered", kind);
    compare(name, pixels, 0);
    GfxTex* other = new_tex(GFX_TEX_2D, fmt, 12, 12, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(other, 0, 0, original, pitch);
    gfx_copy(t, 0, 0, 4, 4, 8, 8, other, 0, 0, 0, 0);
    memcpy(snapshot, original, n);
    for (unsigned y = 0; y < 2; y++)
        memcpy(snapshot + y * pitch, expected + (y + 1) * pitch + block, 2 * block);
    gfx_tex_read(other, 0, 0, got, pitch);
    snprintf(name, sizeof name, "DXT%u-copy-independent-origins-original", kind);
    bytes_compare(name, got, snapshot, n);
    gfx_tex_destroy(other);
    gfx_tex_destroy(t);
}

static void dxt_cube_mips(unsigned kind)
{
    unsigned fmt = 0x00545844u | (('0' + kind) << 24), block = kind == 1 ? 8 : 16;
    GfxTex* t = new_tex(GFX_TEX_CUBE, fmt, 8, 8, 2, GFX_USE_SAMPLE);
    for (unsigned level = 0; level < 2; level++)
        for (unsigned face = 0; face < 6; face++)
        {
            unsigned dim = 8 >> level, blocks = (dim + 3) / 4, pitch = blocks * block;
            unsigned char data[64] = {0}, got[64] = {0};
            uint32_t color = 0;
            for (unsigned i = 0; i < blocks * blocks; i++)
                color = solid_block(kind, face + level * 6, data + i * block);
            gfx_tex_upload(t, face, level, data, pitch);
            gfx_tex_read(t, face, level, got, pitch);
            char name[96];
            snprintf(name, sizeof name, "DXT%u-cube%u-mip%u-original", kind, face, level);
            bytes_compare(name, got, data, pitch * blocks);
            begin(8, 8);
            cube_sampled(t, face,
                         (GfxSampler){.addr_u = 3,
                                      .addr_v = 3,
                                      .addr_w = 3,
                                      .mag = 1,
                                      .min = 1,
                                      .mip = 1,
                                      .max_level = level,
                                      .lod_cap = level + 1});
            uint32_t expected[64];
            for (unsigned i = 0; i < 64; i++)
                expected[i] = color;
            snprintf(name, sizeof name, "DXT%u-cube%u-mip%u-rendered", kind, face, level);
            compare(name, expected, 0);
        }
    gfx_tex_destroy(t);
}

int main(void)
{
    gfx_set_sync_pipelines(1);
    if (!gfx_init(NULL, 0))
    {
        fprintf(stderr, "offscreen init failed\n");
        return 2;
    }
    for (unsigned f = 23; f <= 26; f++)
        sixteen(f);
    scalar(21);
    scalar(22);
    scalar(28);
    scalar(50);
    scalar(51);
    scalar(60);
    mips_rect();
    nonidentity();
    for (int i = 0; i < 3; i++)
        lifetime(i);
    for (unsigned c = 2; c <= 3; c++)
        for (int r = 0; r < 2; r++)
            culling(c, r);
    clear_clips();
    depth_stencil(0);
    depth_stencil(1);
    cube_faces();
    copy_rect();
    dxt_block(1, 0, 0);
    dxt_block(1, 1, 0);
    dxt_block(2, 0, 0);
    dxt_block(3, 0, 0);
    dxt_block(4, 0, 0);
    dxt_block(4, 0, 1);
    dxt_block(5, 0, 0);
    dxt_block(5, 0, 1);
    dxt_mips();
    for (unsigned k = 1; k <= 5; k++)
        dxt_regions(k);
    dxt_cube_mips(1);
    dxt_cube_mips(3);
    dxt_cube_mips(5);
    for (unsigned f = 21; f <= 26; f++)
        self_copy(f);
    self_copy(28);
    self_copy(50);
    self_copy(51);
    for (unsigned kind = 1; kind <= 5; kind++)
        self_copy(0x00545844u | ((uint32_t)('0' + kind) << 24));
    recreate_after_sync();
    fallback_bindings();
    gfx_finish();
    gfx_tex_destroy(target);
    printf(
        "{\"event\":\"format-final\",\"tests\":%u,\"checked_channels\":%u,\"mismatches\":%u,\"pipeline_failures\":%u,\"game_fidelity_claim\":false,\"fps_claim\":false}\n",
        tests, checks, errors, gfx_failures());
    return errors || gfx_failures();
}
