/* Exact independent-target scheduling checks using production gfx entrypoints.
 * Compile main as xi_pass_plan_test_main for the Android fixture dispatcher.
 * Run OFF and ON. These pixel checks do not assert game fidelity or FPS. */
#pragma push_macro("main")
#undef main
#define main pass_plan_unused_format_main
#include "gfx_format_test.c"
#undef main
#pragma pop_macro("main")

static GfxTex *side, *depth, *other;
static const uint32_t full_vp[6] = {0, 0, 32, 32, 0, 0x3f800000};
static void solid(uint32_t color, float z, int ztest)
{
    GfxDraw d;
    struct vertex v[4];
    ui_color(&d);
    d.fill = 3;
    rectangle(v, -.5f, 31.5f, color);
    for (unsigned i = 0; i < 4; ++i)
        v[i].z = z;
    d.data[0] = v;
    d.size[0] = sizeof v;
    d.depth.zenable = d.depth.zwrite = ztest;
    gfx_draw(&d);
    memset(v, 0xcc, sizeof v);
}
static void fresh(void)
{
    if (side)
        gfx_tex_destroy(side);
    if (depth)
        gfx_tex_destroy(depth);
    if (other)
        gfx_tex_destroy(other);
    side = other = depth = NULL;
    begin(32, 32);
    side = gfx_tex_create(GFX_TEX_2D, 21, 32, 32, 1, GFX_USE_RT);
    other = gfx_tex_create(GFX_TEX_2D, 21, 32, 32, 1, GFX_USE_RT);
    depth = gfx_tex_create(GFX_TEX_2D, 75, 32, 32, 1, GFX_USE_DEPTH);
    if (!side || !other || !depth)
        exit(2);
    gfx_set_targets(side, 0, 0, NULL);
    gfx_clear(0, NULL, 1, 0xff101820, 1, 0, full_vp);
    gfx_set_targets(other, 0, 0, NULL);
    gfx_clear(0, NULL, 1, 0xff101820, 1, 0, full_vp);
    gfx_set_targets(target, 0, 0, depth);
    gfx_clear(0, NULL, 7, 0xff2d517f, 1, 0, full_vp);
    solid(0xff2d517f, .8f, 1);
}
static void compare_image(const char* name, GfxTex* image, const uint32_t* expected)
{
    GfxTex* old = target;
    target = image;
    compare(name, expected, 0);
    target = old;
}
static void expected_full(const char* name, GfxTex* image, uint32_t color)
{
    uint32_t expected[1024];
    for (unsigned i = 0; i < 1024; ++i)
        expected[i] = color;
    compare_image(name, image, expected);
}
static void independent_and_depth(void)
{
    fresh();
    uint32_t scene_expected[1024], side_expected[1024];
    for (unsigned i = 0; i < 1024; ++i)
    {
        scene_expected[i] = 0xff2d517f;
        side_expected[i] = 0xff101820;
    }
    for (unsigned stripe = 0; stripe < 16; ++stripe)
    {
        GfxDraw d;
        struct vertex v[4];
        ui_color(&d);
        d.fill = 3;
        uint16_t indices[] = {0, 1, 2, 2, 1, 3};
        d.prim = GFX_TRIANGLELIST;
        d.count = 2;
        d.indices = indices;
        d.index_size = 2;
        uint32_t c = 0xff123456 + stripe * 0x00030405;
        rectangle(v, stripe * 2 - .5f, stripe * 2 + 1.5f, c);
        d.data[0] = v;
        d.size[0] = sizeof v;
        gfx_set_targets(side, 0, 0, NULL);
        gfx_draw(&d);
        memset(v, 0xdd, sizeof v);
        memset(indices, 0xcc, sizeof indices);
        gfx_set_targets(target, 0, 0, depth);
        ui_color(&d);
        d.fill = 3;
        d.depth.zenable = d.depth.zwrite = 1;
        rectangle(v, stripe * 2 - .5f, stripe * 2 + 1.5f, 0xff507030 + stripe * 0x00010203);
        for (unsigned i = 0; i < 4; ++i)
            v[i].z = .25f;
        d.data[0] = v;
        d.size[0] = sizeof v;
        gfx_draw(&d);
        for (unsigned y = 0; y < 32; ++y)
            for (unsigned x = stripe * 2; x < stripe * 2 + 2; ++x)
            {
                side_expected[y * 32 + x] = c;
                scene_expected[y * 32 + x] = 0xff507030 + stripe * 0x00010203;
            }
    }
    /* This depth-rejected draw must not overwrite any scene stripe. */
    solid(0xffff00ff, .75f, 1);
    compare("planner-independent-scene-depth-preserved", scene_expected, 0);
    compare_image("planner-independent-side-UP-and-indices-snapshot", side, side_expected);
}
static void raw(void)
{
    fresh();
    gfx_set_targets(side, 0, 0, NULL);
    solid(0xff6c9a31, .5f, 0);
    gfx_set_targets(target, 0, 0, depth);
    sampled(side, 0, 0);
    expected_full("planner-RAW-samples-latest-pending-side", target, 0xff6c9a31);
}
static void war(void)
{
    fresh();
    gfx_set_targets(side, 0, 0, NULL);
    sampled(target, 0, 0);
    gfx_set_targets(target, 0, 0, depth);
    solid(0xff987624, .5f, 0);
    expected_full("planner-WAR-side-retains-earlier-scene", side, 0xff2d517f);
    expected_full("planner-WAR-scene-rewrite", target, 0xff987624);
}
static void blend_order(void)
{
    fresh();
    gfx_set_targets(side, 0, 0, NULL);
    gfx_clear(0, NULL, 1, 0xff646464, 1, 0, full_vp);
    for (unsigned i = 0; i < 2; ++i)
    {
        GfxDraw d;
        struct vertex v[4];
        ui_color(&d);
        d.fill = 3;
        rectangle(v, -.5f, 31.5f, i ? 0xff323232 : 0xffc8c8c8);
        d.data[0] = v;
        d.size[0] = sizeof v;
        d.pipe.blend = 1;
        d.pipe.src = d.pipe.dst = 2;
        d.pipe.op = 2;
        gfx_set_targets(side, 0, 0, NULL);
        gfx_draw(&d);
        gfx_set_targets(target, 0, 0, depth);
        solid(0xff314159, .5f, 0);
    }
    expected_full("planner-order-sensitive-subtract-blend", side, 0xff000000);
}
static void mutations(void)
{
    fresh();
    uint32_t src[1024];
    for (unsigned i = 0; i < 1024; ++i)
        src[i] = 0xff739a5b;
    gfx_tex_upload(other, 0, 0, src, 128);
    gfx_set_targets(target, 0, 0, depth);
    solid(0xff2d517f, .5f, 0);
    gfx_set_targets(side, 0, 0, NULL);
    sampled(other, 0, 0);
    for (unsigned i = 0; i < 1024; ++i)
        src[i] = 0xffa36291;
    gfx_tex_upload(other, 0, 0, src, 128);
    gfx_set_targets(target, 0, 0, depth);
    solid(0xff314159, .5f, 0);
    expected_full("planner-source-upload-flushes-pending-reader", side, 0xff739a5b);
    gfx_set_targets(side, 0, 0, NULL);
    solid(0xff927346, .5f, 0);
    gfx_copy(side, 0, 0, 0, 0, 32, 32, other, 0, 0, 0, 0);
    expected_full("planner-copy-observes-pending-writer", other, 0xff927346);
    gfx_set_targets(target, 0, 0, depth);
    solid(0xff314159, .5f, 0);
    gfx_set_targets(side, 0, 0, NULL);
    sampled(other, 0, 0);
    gfx_tex_destroy(other);
    other = NULL;
    expected_full("planner-source-destroy-flushes-pending-reader", side, 0xff927346);
}
static void static_buffer_mutation(void)
{
    fresh();
    GfxDraw d;
    struct vertex v[4];
    ui_color(&d);
    d.fill = 3;
    GfxBuf* b = gfx_buf_create(sizeof v);
    if (!b)
        exit(2);
    rectangle(v, -.5f, 15.5f, 0xff8a3b6c);
    gfx_buf_upload(b, v, sizeof v);
    d.buf[0] = b;
    gfx_set_targets(target, 0, 0, depth);
    solid(0xff314159, .5f, 0);
    gfx_set_targets(side, 0, 0, NULL);
    gfx_draw(&d);
    rectangle(v, 15.5f, 31.5f, 0xff3c7d9e);
    gfx_buf_upload(b, v, sizeof v);
    gfx_set_targets(target, 0, 0, depth);
    solid(0xff314159, .5f, 0);
    gfx_set_targets(side, 0, 0, NULL);
    gfx_draw(&d);
    gfx_buf_destroy(b);
    uint32_t expected[1024];
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x)
            expected[y * 32 + x] = x < 16 ? 0xff8a3b6c : 0xff3c7d9e;
    compare_image("planner-static-buffer-upload-and-destroy-barriers", side, expected);
}
static void partial_clear_and_mips(void)
{
    fresh();
    gfx_set_targets(side, 0, 0, NULL);
    int32_t rect[] = {4, 6, 20, 24};
    gfx_clear(1, rect, 1, 0xffd3a257, 1, 0, full_vp);
    memset(rect, 0, sizeof rect);
    gfx_set_targets(target, 0, 0, depth);
    solid(0xff314159, .5f, 0);
    uint32_t expected[1024];
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x)
            expected[y * 32 + x] = x >= 4 && x < 20 && y >= 6 && y < 24 ? 0xffd3a257 : 0xff101820;
    compare_image("planner-partial-clear-rectangles-snapshot", side, expected);
}
int main(void)
{
    gfx_set_sync_pipelines(1);
    if (!gfx_init(NULL, 0))
        return 2;
    independent_and_depth();
    raw();
    war();
    blend_order();
    mutations();
    static_buffer_mutation();
    partial_clear_and_mips();
    gfx_finish();
    gfx_tex_destroy(side);
    gfx_tex_destroy(depth);
    if (other)
        gfx_tex_destroy(other);
    gfx_tex_destroy(target);
    printf(
        "{\"event\":\"pass-plan-final\",\"tests\":%u,\"checked_values\":%u,\"mismatches\":%u,\"pipeline_failures\":%u,\"game_fidelity_claim\":false,\"fps_claim\":false}\n",
        tests, checks, errors, gfx_failures());
    return errors || gfx_failures();
}
