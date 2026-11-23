/* The Vulkan back end's bounded render area, without the game: a pass begun on only part of the
 * target must keep the pixels outside that part, and a pass that grows must keep its color, depth
 * and stencil contents. Each draw sets its own viewport, the area the back end may bound it to.
 *
 * usage: tools/build_android.py gfxtest --test gfx_area, or libmain's --android-area-test
 *        (exit status 0 when every check passes; FFXI_ANDROID_BOUNDED_AREA=1 enables the bounds,
 *        without it every pass covers the whole target) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"

#define W 64u
#define H 64u
#define CLEAR 0xff101820u
static GfxTex *rt, *ds;
static unsigned tests, checks, failures;
static uint32_t pixels[W * H], expected[W * H];
static const uint32_t VP_FULL[6] = {0, 0, W, H, 0, UINT32_C(0x3f800000)};

/* expected: the clear color everywhere */
static void expect_clear(void)
{
    for (unsigned i = 0; i < W * H; ++i)
        expected[i] = CLEAR;
}

/* expected: color over [x0, x1) x [y0, y1) */
static void expect_rect(unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t color)
{
    for (unsigned y = y0; y < y1; ++y)
        for (unsigned x = x0; x < x1; ++x)
            expected[y * W + x] = color;
}

/* the target read back against expected */
static void check(const char* name)
{
    gfx_tex_read(rt, 0, 0, pixels, W * 4);
    unsigned before = failures;
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x)
        {
            uint32_t got = pixels[y * W + x], want = expected[y * W + x];
            ++checks;
            if (got != want && failures++ < 12)
                fprintf(stderr, "area %s pixel %u,%u got %08x want %08x\n", name, x, y, got, want);
        }
    printf("{\"event\":\"area-case\",\"name\":\"%s\",\"checks\":%u,\"mismatches\":%u}\n", name, W * H,
           failures - before);
    ++tests;
}

static void identity(float* m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

typedef struct UiVert
{
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} UiVert;

static void defaults(GfxDraw* d, unsigned x, unsigned y, unsigned w, unsigned h)
{
    memset(d, 0, sizeof *d);
    identity(d->u.wvp);
    identity(d->u.wv);
    identity(d->u.wvit);
    for (unsigned i = 0; i < 8; ++i)
        identity(d->u.texm[i]);
    d->u.vp[0] = (float)x;
    d->u.vp[1] = (float)y;
    d->u.vp[2] = (float)w;
    d->u.vp[3] = (float)h;
    d->u.tfactor[0] = d->u.tfactor[1] = d->u.tfactor[2] = d->u.tfactor[3] = 1.0f;
    d->pipe.write_mask = 0xF;
    d->cull = 1;
    d->fill = 3;        /* D3DFILL_SOLID: only solid fills are bounded */
    d->depth.zfunc = 4; /* D3DCMP_LESS */
    d->vp[0] = x;
    d->vp[1] = y;
    d->vp[2] = w;
    d->vp[3] = h;
    d->vp[4] = 0;
    d->vp[5] = UINT32_C(0x3f800000);
    d->vs.rhw = 1;
    d->vs.el[0] = (GfxElem){1, 0, GFX_FLOAT4, 0};
    d->vs.el[5] = (GfxElem){1, 0, GFX_D3DCOLOR, 0};
    d->u.stride[0] = sizeof(UiVert);
    d->u.offset[5] = 16;
    /* diffuse only; no texture stage is required for a solid-color draw */
    d->fs.nstages = 1;
    d->fs.st[0] = (GfxStage){2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2};
    d->prim = GFX_TRIANGLESTRIP;
    d->count = 2;
}

static void quad(UiVert* v, unsigned x, unsigned y, unsigned w, unsigned h, uint32_t color, float z)
{
    v[0] = (UiVert){(float)x - .5f, (float)y - .5f, z, 1, color, 0, 0};
    v[1] = (UiVert){(float)(x + w) - .5f, (float)y - .5f, z, 1, color, 1, 0};
    v[2] = (UiVert){(float)x - .5f, (float)(y + h) - .5f, z, 1, color, 0, 1};
    v[3] = (UiVert){(float)(x + w) - .5f, (float)(y + h) - .5f, z, 1, color, 1, 1};
}

static void draw_quad(unsigned x, unsigned y, unsigned w, unsigned h, uint32_t color, float z, GfxDraw* d, UiVert* v)
{
    quad(v, x, y, w, h, color, z);
    d->data[0] = v;
    d->size[0] = sizeof *v * 4;
    gfx_draw(d);
}

static void fresh(unsigned with_depth)
{
    if (rt)
        gfx_tex_destroy(rt);
    if (ds)
        gfx_tex_destroy(ds);
    rt = gfx_tex_create(GFX_TEX_2D, 21, W, H, 1, GFX_USE_RT);
    ds = with_depth ? gfx_tex_create(GFX_TEX_2D, 75, W, H, 1, GFX_USE_DEPTH) : NULL;
    if (!rt || (with_depth && !ds))
    {
        fprintf(stderr, "area target allocation failed\n");
        exit(2);
    }
    gfx_set_targets(rt, 0, 0, ds);
    gfx_clear(0, NULL, with_depth ? 7 : 1, CLEAR, 1.0f, 0, VP_FULL);
    /* close the full clear's pass, so the first draw begins a bounded one */
    gfx_set_targets(NULL, 0, 0, NULL);
    gfx_set_targets(rt, 0, 0, ds);
}

static void test_exterior_sentinel(void)
{
    const uint32_t inside = 0xffdb7c22u;
    fresh(0);
    GfxDraw d;
    UiVert v[4];
    defaults(&d, 8, 8, 24, 24);
    draw_quad(8, 8, 24, 24, inside, .5f, &d, v);
    expect_clear();
    expect_rect(8, 8, 32, 32, inside);
    check("exterior-color-sentinel");
}

static void test_scissor_sentinel(void)
{
    const uint32_t inside = 0xffd34b9au;
    fresh(0);
    GfxDraw d;
    UiVert v[4];
    defaults(&d, 0, 0, W, H);
    d.scissor[0] = 12;
    d.scissor[1] = 12;
    d.scissor[2] = 16;
    d.scissor[3] = 16;
    draw_quad(0, 0, W, H, inside, .5f, &d, v);
    expect_clear();
    expect_rect(12, 12, 28, 28, inside);
    check("scissor-narrows-area");
}

static void test_growing_draws(void)
{
    const uint32_t left = 0xff32b85eu, right = 0xff4968e8u;
    fresh(0);
    GfxDraw d;
    UiVert v[4];
    defaults(&d, 4, 4, 16, 16);
    draw_quad(4, 4, 16, 16, left, .5f, &d, v);
    /* This draw is outside the first render area. It must end/reopen with an
     * outward union and LOAD the first pass before writing its new rectangle. */
    defaults(&d, 40, 40, 16, 16);
    draw_quad(40, 40, 16, 16, right, .5f, &d, v);
    expect_clear();
    expect_rect(4, 4, 20, 20, left);
    expect_rect(40, 40, 56, 56, right);
    check("growing-draws-same-submission");
}

static void test_depth_stencil_continuity(void)
{
    const uint32_t first = 0xffd13d57u, front = 0xff4ea9e0u;
    fresh(1);
    GfxDraw d;
    UiVert v[4];
    defaults(&d, 8, 8, 48, 48);
    d.depth.zenable = d.depth.zwrite = 1;
    draw_quad(8, 8, 48, 48, first, .5f, &d, v);
    /* Behind the first draw: tests depth contents after an area expansion that
     * is wholly contained in the existing pass. */
    defaults(&d, 16, 16, 32, 32);
    d.depth.zenable = d.depth.zwrite = 1;
    draw_quad(16, 16, 32, 32, 0xff8c4cd2u, .75f, &d, v);
    /* In front, but the viewport extends outside the current area: this forces
     * pass expansion and a depth LOAD. */
    defaults(&d, 0, 0, 32, 32);
    d.depth.zenable = d.depth.zwrite = 1;
    draw_quad(0, 0, 32, 32, front, .25f, &d, v);
    expect_clear();
    expect_rect(8, 8, 56, 56, first);
    expect_rect(0, 0, 32, 32, front);
    check("depth-preservation-after-area-expansion");

    /* Stencil written in the first area and consumed by a larger later draw. */
    fresh(1);
    defaults(&d, 8, 8, 24, 24);
    d.depth.stencil = 1;
    d.depth.sfunc = 8;
    d.depth.sfail = d.depth.szfail = 1;
    d.depth.spass = 3;
    d.depth.sread = d.depth.swrite = 255;
    d.stencil_ref = 5;
    draw_quad(8, 8, 24, 24, first, .5f, &d, v);
    defaults(&d, 0, 0, 32, 32);
    d.depth.stencil = 1;
    d.depth.sfunc = 3;
    d.depth.sfail = d.depth.szfail = d.depth.spass = 1;
    d.depth.sread = 255;
    d.depth.swrite = 0;
    d.stencil_ref = 5;
    draw_quad(0, 0, 32, 32, front, .5f, &d, v);
    expect_clear();
    expect_rect(8, 8, 32, 32, front);
    check("stencil-preservation-after-area-expansion");
}

static void test_partial_clear(void)
{
    const uint32_t inside = 0xffe0b42fu;
    /* the partial clear begins a new LOAD pass whose area is only its rectangle */
    fresh(0);
    uint32_t vp[] = {16, 16, 32, 32, 0, UINT32_C(0x3f800000)};
    gfx_clear(0, NULL, 1, inside, 1.0f, 0, vp);
    expect_clear();
    expect_rect(16, 16, 48, 48, inside);
    check("partial-clear-load-preserves-exterior");
}

static void setup_textured(GfxDraw* d, GfxTex* tex, unsigned x, unsigned y, unsigned w, unsigned h)
{
    defaults(d, x, y, w, h);
    d->vs.el[7] = (GfxElem){1, 0, GFX_FLOAT2, 0};
    d->vs.ntex = 1;
    d->u.offset[7] = 20;
    d->fs.st[0] = (GfxStage){2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2};
    d->tex[0] = tex;
    d->samp[0] = (GfxSampler){3, 3, 3, 1, 1, 1, 1, 1, 0};
}

static void test_copy_sample_write(void)
{
    const uint32_t blue = 0xff2a58d0u, green = 0xff36c27au, yellow = 0xffe4c231u;
    fresh(0);
    GfxTex* sample = gfx_tex_create(GFX_TEX_2D, 21, 1, 1, 1, GFX_USE_SAMPLE);
    GfxTex* copy = gfx_tex_create(GFX_TEX_2D, 21, 8, 8, 1, GFX_USE_RT);
    uint32_t c = blue, patch[64];
    for (unsigned i = 0; i < 64; ++i)
        patch[i] = yellow;
    gfx_tex_upload(sample, 0, 0, &c, 4);
    gfx_tex_upload(copy, 0, 0, patch, 8 * 4);
    GfxDraw d;
    UiVert v[4];
    setup_textured(&d, sample, 24, 24, 16, 16);
    draw_quad(24, 24, 16, 16, 0xffffffffu, .5f, &d, v);
    c = green;
    /* Uploading a sampled image while its draw is pending must close the pass,
     * transition the image, and invalidate any sampled-layout cache. */
    gfx_tex_upload(sample, 0, 0, &c, 4);
    setup_textured(&d, sample, 24, 24, 16, 16);
    draw_quad(24, 24, 16, 16, 0xffffffffu, .5f, &d, v);
    /* A copy after the sample/write chain closes the pass and must not damage
     * the sampled center or the untouched exterior. */
    gfx_copy(copy, 0, 0, 0, 0, 8, 8, rt, 0, 0, 0, 0);
    expect_clear();
    expect_rect(0, 0, 8, 8, yellow);
    expect_rect(24, 24, 40, 40, green);
    check("copy-sample-write-chain");
    gfx_tex_destroy(sample);
    gfx_tex_destroy(copy);
}

static void test_full_fallback(void)
{
    /* A world-space (not RHW) quad with fill left 0, which the back end does
     * not bound: it may take the full area, and must still render correctly
     * with the bounds enabled. */
    const uint32_t inside = 0xffaf4fe4u;
    fresh(0);
    GfxDraw d;
    memset(&d, 0, sizeof d);
    identity(d.u.wvp);
    identity(d.u.wv);
    identity(d.u.wvit);
    d.u.vp[0] = 0;
    d.u.vp[1] = 0;
    d.u.vp[2] = W;
    d.u.vp[3] = H;
    d.vp[0] = 0;
    d.vp[1] = 0;
    d.vp[2] = W;
    d.vp[3] = H;
    d.vp[5] = UINT32_C(0x3f800000);
    d.pipe.write_mask = 0xF;
    d.cull = 1;
    d.depth.zfunc = 4;
    d.fs.nstages = 1;
    d.fs.st[0] = (GfxStage){2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2};
    d.vs.el[0] = (GfxElem){1, 0, GFX_FLOAT3, 0};
    d.vs.el[5] = (GfxElem){1, 0, GFX_D3DCOLOR, 0};
    d.u.stride[0] = 16;
    d.u.offset[5] = 12;
    struct WorldVert
    {
        float x, y, z;
        uint32_t color;
    } v[4] = {{-0.5f, 0.5f, 0.5f, inside},
              {0.5f, 0.5f, 0.5f, inside},
              {-0.5f, -0.5f, 0.5f, inside},
              {0.5f, -0.5f, 0.5f, inside}};
    d.data[0] = v;
    d.size[0] = sizeof v;
    d.prim = GFX_TRIANGLESTRIP;
    d.count = 2;
    gfx_draw(&d);
    expect_clear();
    expect_rect(16, 16, 48, 48, inside);
    check("full-fallback-world-space");
}

int main(void)
{
    if (!gfx_init(NULL, 0))
        return 2;
    test_exterior_sentinel();
    test_scissor_sentinel();
    test_growing_draws();
    test_depth_stencil_continuity();
    test_partial_clear();
    test_copy_sample_write();
    test_full_fallback();
    gfx_finish();
    if (ds)
        gfx_tex_destroy(ds);
    if (rt)
        gfx_tex_destroy(rt);
    printf(
        "{\"event\":\"area-final\",\"tests\":%u,\"checks\":%u,\"mismatches\":%u,\"pipeline_failures\":%u,\"candidate\":\"FFXI_ANDROID_BOUNDED_AREA\",\"game_fidelity_claim\":false,\"fps_claim\":false}\n",
        tests, checks, failures, gfx_failures());
    return failures || gfx_failures();
}
