/* The Vulkan back end's draw state without the game, offscreen: color write masks, alpha test,
 * blend factors and ops, vertex and index offsets and topologies, and clears of every cube face and
 * mip as render targets. Shares gfx_format_test.c's helpers by including it, its main renamed.
 *
 * usage: tools/build_android.py gfxtest --test gfx_state, or libmain's --android-state-test
 *        (exit status 0 when every check passes) */
#pragma push_macro("main")
#undef main
#define main format_probe_main
#include "gfx_format_test.c"
#undef main
#pragma pop_macro("main")

static void full_color(GfxDraw* d, struct vertex* v, uint32_t color)
{
    ui_color(d);
    rectangle(v, -.5f, 31.5f, color);
    d->data[0] = v;
    d->size[0] = 4 * sizeof *v;
}

static void masks(void)
{
    const uint32_t background = 0xff2d517f, source = 0x918a36ca;
    for (unsigned mask = 0; mask < 16; mask++)
    {
        begin(32, 32);
        GfxDraw d;
        struct vertex v[4];
        full_color(&d, v, source);
        d.pipe.write_mask = mask;
        gfx_draw(&d);
        unsigned byte_mask = ((mask & 1) ? 0x00ff0000 : 0) | ((mask & 2) ? 0x0000ff00 : 0) |
                             ((mask & 4) ? 0x000000ff : 0) | ((mask & 8) ? 0xff000000 : 0);
        uint32_t expected[1024];
        for (unsigned i = 0; i < 1024; i++)
            expected[i] = (background & ~byte_mask) | (source & byte_mask);
        char name[48];
        snprintf(name, sizeof name, "color-mask-%u", mask);
        compare(name, expected, 0);
    }
}

static int alpha_pass(unsigned op, int a, int ref)
{
    switch (op)
    {
    case 1:
        return 0;
    case 2:
        return a < ref;
    case 3:
        return a == ref;
    case 4:
        return a <= ref;
    case 5:
        return a > ref;
    case 6:
        return a != ref;
    case 7:
        return a >= ref;
    default:
        return 1;
    }
}

static void alpha_tests(void)
{
    for (unsigned op = 1; op <= 8; op++)
    {
        begin(32, 32);
        GfxDraw d;
        struct vertex v[4];
        full_color(&d, v, 0x7f639da1);
        d.fs.alpha_func = op;
        d.u.params[1] = 128;
        gfx_draw(&d);
        rectangle(v, 7.5f, 23.5f, 0x80639da1);
        gfx_draw(&d);
        uint32_t expected[1024];
        for (unsigned y = 0; y < 32; y++)
            for (unsigned x = 0; x < 32; x++)
            {
                int middle = x >= 8 && x < 24;
                expected[y * 32 + x] = middle && alpha_pass(op, 128, 128) ? 0x80639da1
                                       : alpha_pass(op, 127, 128)         ? 0x7f639da1
                                                                          : 0xff2d517f;
            }
        char name[48];
        snprintf(name, sizeof name, "alpha-compare-%u-with-equal-boundary", op);
        compare(name, expected, 0);
    }
}

static float factor(unsigned f, unsigned ch, const float* s, const float* t)
{
    switch (f)
    {
    case 1:
        return 0;
    case 2:
        return 1;
    case 3:
        return s[ch];
    case 4:
        return 1 - s[ch];
    case 5:
        return s[3];
    case 6:
        return 1 - s[3];
    case 7:
        return t[3];
    case 8:
        return 1 - t[3];
    case 9:
        return t[ch];
    case 10:
        return 1 - t[ch];
    case 11:
        return ch == 3 ? 1 : fminf(s[3], 1 - t[3]);
    default:
        return 1;
    }
}

static uint32_t blended(unsigned sf, unsigned df, unsigned op, uint32_t source, uint32_t dest)
{
    if (sf == 12)
    {
        sf = 5;
        df = 6;
    }
    else if (sf == 13)
    {
        sf = 6;
        df = 5;
    }
    float s[4], t[4];
    for (unsigned ch = 0; ch < 4; ch++)
    {
        s[ch] = ((source >> (ch * 8)) & 255) / 255.0f;
        t[ch] = ((dest >> (ch * 8)) & 255) / 255.0f;
    }
    uint32_t result = 0;
    for (unsigned ch = 0; ch < 4; ch++)
    {
        float a = s[ch] * factor(sf, ch, s, t), b = t[ch] * factor(df, ch, s, t);
        float value = op == 2   ? a - b
                      : op == 3 ? b - a
                      : op == 4 ? fminf(s[ch], t[ch])
                      : op == 5 ? fmaxf(s[ch], t[ch])
                                : a + b;
        value = fminf(1, fmaxf(0, value));
        result |= (uint32_t)lroundf(255 * value) << (ch * 8);
    }
    return result;
}

static void blend_case(unsigned sf, unsigned df, unsigned op)
{
    begin(32, 32);
    uint32_t vp[] = {0, 0, 32, 32, 0, 0x3f800000}, background = 0x794d9251, source = 0x9b8e376a;
    gfx_clear(0, NULL, 1, background, 1, 0, vp);
    GfxDraw d;
    struct vertex v[4];
    full_color(&d, v, source);
    d.pipe.blend = 1;
    d.pipe.src = sf;
    d.pipe.dst = df;
    d.pipe.op = op;
    gfx_draw(&d);
    uint32_t expected[1024], pixel = blended(sf, df, op, source, background);
    for (unsigned i = 0; i < 1024; i++)
        expected[i] = pixel;
    char name[48];
    snprintf(name, sizeof name, "blend-s%u-d%u-op%u", sf, df, op);
    compare(name, expected, 1);
}

static void indexed(unsigned mode)
{
    begin(32, 32);
    GfxDraw d;
    struct vertex v[8];
    full_color(&d, v + 2, 0xff73ac59);
    memset(v, 0, 2 * sizeof *v);
    memset(v + 6, 0, 2 * sizeof *v);
    d.data[0] = v;
    d.size[0] = sizeof v;
    d.u.vofs = 1;
    d.vertex_start = 1;
    uint16_t ix16[8] = {99, 99, 1, 2, 3, 3, 2, 4}, fan[] = {1, 2, 3, 4};
    uint32_t ix32[6] = {1, 2, 3, 3, 2, 4};
    GfxBuf *vb = NULL, *ib = NULL;
    /* mode 0: full_color's strip from vertex 1; 1, 2: 16- and 32-bit index lists; 3: vertices and
     * indices from buffers at offsets; 4, 5: a fan, without and with indices */
    if (mode == 1 || mode == 2)
    {
        d.prim = GFX_TRIANGLELIST;
        d.count = 2;
        d.indices = mode == 1 ? (void*)(ix16 + 2) : (void*)ix32;
        d.index_size = mode == 1 ? 2 : 4;
    }
    else if (mode == 3)
    {
        vb = gfx_buf_create(sizeof v + 16);
        unsigned char bytes[sizeof v + 16];
        memset(bytes, 0, 16);
        memcpy(bytes + 16, v, sizeof v);
        gfx_buf_upload(vb, bytes, sizeof bytes);
        d.data[0] = NULL;
        d.buf[0] = vb;
        d.buf_off[0] = 16;
        ib = gfx_buf_create(sizeof ix16);
        gfx_buf_upload(ib, ix16, sizeof ix16);
        d.ibuf = ib;
        d.ibuf_off = 4;
        d.index_size = 2;
        d.prim = GFX_TRIANGLELIST;
        d.count = 2;
    }
    else
    {
        /* a fan takes the quad's corners in perimeter order */
        d.prim = GFX_TRIANGLEFAN;
        struct vertex tmp = v[4];
        v[4] = v[5];
        v[5] = tmp;
        if (mode == 5)
        {
            d.indices = fan;
            d.index_size = 2;
        }
    }
    gfx_draw(&d);
    if (vb)
        gfx_buf_destroy(vb);
    if (ib)
        gfx_buf_destroy(ib);
    uint32_t expected[1024];
    for (unsigned i = 0; i < 1024; i++)
        expected[i] = 0xff73ac59;
    char name[48];
    snprintf(name, sizeof name, "vertex-and-index-offset-mode%u", mode);
    compare(name, expected, 0);
}

static void rt_faces_mips(void)
{
    GfxTex* t = new_tex(GFX_TEX_CUBE, 21, 16, 16, 5, GFX_USE_RT);
    for (unsigned face = 0; face < 6; face++)
        for (unsigned level = 0; level < 5; level++)
        {
            unsigned dim = 16 >> level;
            uint32_t vp[] = {0, 0, dim, dim, 0, 0x3f800000};
            gfx_set_targets(t, face, level, NULL);
            gfx_clear(0, NULL, 1, 0xff192c41 + face * 0x00071902 + level * 0x00110207, 1, 0, vp);
        }
    for (unsigned face = 0; face < 6; face++)
        for (unsigned level = 0; level < 5; level++)
        {
            unsigned dim = 16 >> level;
            uint32_t got[256];
            gfx_tex_read(t, face, level, got, dim * 4);
            uint32_t expected = 0xff192c41 + face * 0x00071902 + level * 0x00110207;
            unsigned before = errors;
            for (unsigned i = 0; i < dim * dim; i++)
            {
                checks++;
                if (got[i] != expected)
                {
                    errors++;
                    if (first_errors++ < 8)
                        fprintf(stderr, "cube-RT face%u mip%u pixel%u got%08x expected%08x\n", face, level, i, got[i],
                                expected);
                }
            }
            char name[48];
            snprintf(name, sizeof name, "render-target-cube-face%u-mip%u", face, level);
            printf("{\"event\":\"format-case\",\"name\":\"%s\",\"checks\":%u,\"mismatches\":%u}\n", name, dim * dim,
                   errors - before);
            tests++;
        }
    gfx_tex_destroy(t);
}

int main(void)
{
    gfx_set_sync_pipelines(1);
    if (!gfx_init(NULL, 0))
        return 2;
    masks();
    alpha_tests();
    for (unsigned sf = 1; sf <= 13; sf++)
        blend_case(sf, 9, 1);
    for (unsigned op = 2; op <= 5; op++)
        blend_case(5, 6, op);
    for (unsigned mode = 0; mode < 6; mode++)
        indexed(mode);
    rt_faces_mips();
    gfx_finish();
    if (target)
        gfx_tex_destroy(target);
    printf(
        "{\"event\":\"state-final\",\"tests\":%u,\"checked_values\":%u,\"mismatches\":%u,\"pipeline_failures\":%u,\"game_fidelity_claim\":false,\"fps_claim\":false}\n",
        tests, checks, errors, gfx_failures());
    return errors || gfx_failures();
}
