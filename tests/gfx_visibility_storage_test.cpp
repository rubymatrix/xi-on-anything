/* Real gfx API regression fixture for the opt-in full-pixel visibility route.
 * Compile -Dmain=xi_visibility_storage_test_main for the Android dispatcher.
 * Reference black/white draws are equivalent six-vertex TRIANGLELIST geometry,
 * outside the production four-vertex-strip recognizer. Candidate draws use
 * synthetic rectangles with the observed D3D8 state shape. Full pixels, padding, mask history,
 * aliases and later generic observations are compared; no timing/game claim.
 * When FFXI_ANDROID_VISIBILITY_STORAGE=1, every supported candidate must raise
 * the production commit counter. Exact fallback alone can never pass that run.
 */
extern "C"
{
#include "gfx.h"
    uint32_t gfx_android_visibility_storage_stats(uint64_t*, uint32_t);
}
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if !defined(FFXI_ANDROID_VULKAN)
extern "C" int main(void)
{
    std::puts("visibility-storage: Android API required");
    return 77;
}
#else
namespace
{
using Pixels = std::vector<uint32_t>;
using Counters = std::array<uint64_t, 5>;
uint64_t checks = 0, errors = 0, cases = 0, shown = 0, proofs = 0, farProofs = 0, materializations = 0, nextKey = 1;
bool required = false;
constexpr uint32_t SEED = 0x3f123456u, SCENE = 0xff2d517fu;
void check(bool ok, const char* why)
{
    ++checks;
    if (ok)
        return;
    ++errors;
    if (shown++ < 24)
        std::fprintf(stderr, "visibility-storage failure: %s\n", why);
}
Counters stats()
{
    Counters c{};
    check(gfx_android_visibility_storage_stats(c.data(), uint32_t(c.size())) == 5, "stats export unavailable");
    return c;
}
uint64_t key()
{
    return UINT64_C(0x1006c72f78000000) + (nextKey++ << 4);
}
void record(const char* name, uint64_t before)
{
    ++cases;
    std::printf("{\"event\":\"visibility-storage-case\",\"name\":\"%s\",\"mismatches\":%llu}\n", name,
                (unsigned long long)(errors - before));
}
void equal(const char* label, const Pixels& a, const Pixels& b)
{
    check(a.size() == b.size(), "pixel vector dimensions");
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    {
        ++checks;
        if (a[i] != b[i])
        {
            ++errors;
            if (shown++ < 24)
                std::fprintf(stderr, "%s pixel %zu got %08x expected %08x\n", label, i, a[i], b[i]);
        }
    }
}
GfxTex* texture(unsigned n = 16, unsigned levels = 1, int type = GFX_TEX_2D, int use = GFX_USE_RT, unsigned fmt = 21)
{
    auto* t = gfx_tex_create(type, fmt, n, n, levels, use);
    if (!t)
    {
        std::fprintf(stderr, "visibility fixture allocation failed\n");
        std::exit(2);
    }
    return t;
}
void target(GfxTex* t, GfxTex* depth = nullptr, unsigned face = 0, unsigned level = 0)
{
    gfx_set_targets(t, face, level, depth);
}
void clear(GfxTex* t, unsigned n, uint32_t color, GfxTex* depth = nullptr, unsigned flags = 1, float z = 1)
{
    target(t, depth);
    uint32_t vp[]{0, 0, n, n, 0, 0x3f800000};
    gfx_clear(0, nullptr, flags, color, z, 0, vp);
}
Pixels read(GfxTex* t, unsigned n = 16, unsigned face = 0, unsigned level = 0, int kind = 0, uint64_t k = 0,
            unsigned age = 16)
{
    const unsigned pitch = n * 4 + 12, body = pitch * n;
    std::vector<uint8_t> b(body + 32, 0xcd);
    Pixels p(size_t(n) * n);
    if (kind == 1)
        gfx_tex_read_async(t, face, level, b.data() + 16, pitch);
    else if (kind == 2)
        gfx_tex_read_async_keyed(t, face, level, b.data() + 16, pitch, k, age);
    else
        gfx_tex_read(t, face, level, b.data() + 16, pitch);
    for (unsigned y = 0; y < n; ++y)
    {
        std::memcpy(p.data() + size_t(y) * n, b.data() + 16 + size_t(y) * pitch, n * 4);
        for (unsigned x = n * 4; x < pitch; ++x)
            check(b[16 + size_t(y) * pitch + x] == 0xcd, "row padding changed");
    }
    for (unsigned i = 0; i < 16; ++i)
    {
        check(b[i] == 0xcd, "leading guard changed");
        check(b[16 + body + i] == 0xcd, "trailing guard changed");
    }
    return p;
}
void advance()
{
    gfx_present(nullptr);
    gfx_finish();
}
struct ColorVertex
{
    float x, y, z, w;
    uint32_t color;
};
struct TexVertex
{
    float x, y, z, w, u, v;
};
static_assert(sizeof(ColorVertex) == 20 && sizeof(TexVertex) == 24, "UP vertex layouts");
GfxDraw base(unsigned n)
{
    GfxDraw d{};
    d.vs.rhw = 1;
    d.vs.el[0] = {1, 0, GFX_FLOAT4, 0};
    d.fs.nstages = 1;
    d.pipe.write_mask = 15;
    d.cull = 1;
    d.fill = 3;
    d.prim = GFX_TRIANGLESTRIP;
    d.count = 2;
    d.vp[2] = d.vp[3] = n;
    d.vp[5] = 0x3f800000;
    d.u.vp[2] = d.u.vp[3] = float(n);
    return d;
}
void solid(unsigned n, float l, float t, float r, float b, float z, uint32_t color, bool strip, bool depthTest = false,
           bool depthWrite = false, bool scissor = false, bool blend = false, unsigned stencilMode = 0)
{
    GfxDraw d = base(n);
    d.vs.el[5] = {1, 0, GFX_D3DCOLOR, 0};
    d.u.stride[0] = 20;
    d.u.offset[5] = 16;
    d.fs.st[0] = {2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2};
    d.depth.zenable = depthTest;
    d.depth.zwrite = depthWrite;
    d.depth.zfunc = 4;
    d.samp[0] = {3, 3, 1, 2, 2, 2, 1, 0, 0, {0, 0, 0}, 0};
    ColorVertex quad[] = {{l, t, z, 1, color}, {r, t, z, 1, color}, {l, b, z, 1, color}, {r, b, z, 1, color}};
    ColorVertex triangles[] = {quad[0], quad[1], quad[2], quad[2], quad[1], quad[3]};
    d.data[0] = strip ? static_cast<void*>(quad) : static_cast<void*>(triangles);
    d.size[0] = strip ? sizeof quad : sizeof triangles;
    if (!strip)
        d.prim = GFX_TRIANGLELIST;
    if (scissor)
    {
        d.scissor[2] = n;
        d.scissor[3] = n;
    }
    if (blend)
    {
        d.pipe.blend = 1;
        d.pipe.src = 5;
        d.pipe.dst = 6;
        d.pipe.op = 1;
    }
    if (stencilMode)
    {
        d.depth.stencil = 1;
        d.depth.sfunc = stencilMode == 1 ? 8 : 3;
        d.depth.sfail = d.depth.szfail = 1;
        d.depth.spass = stencilMode == 1 ? 3 : 1;
        d.depth.sread = 255;
        d.depth.swrite = stencilMode == 1 ? 255 : 0;
        d.stencil_ref = 5;
        if (stencilMode == 1)
            d.pipe.write_mask = 0;
    }
    gfx_draw(&d);
    std::memset(quad, 0xcc, sizeof quad);
    std::memset(triangles, 0xdd, sizeof triangles);
}
void sample(GfxTex* from, unsigned n, float l, float t, float r, float b, float u0, float v0, float u1, float v1)
{
    GfxDraw d = base(n);
    d.vs.el[7] = {1, 0, GFX_FLOAT2, 0};
    d.vs.ntex = 1;
    d.u.stride[0] = 24;
    d.u.offset[7] = 16;
    d.fs.st[0] = {2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2};
    d.tex[0] = from;
    d.samp[0] = {3, 3, 1, 1, 1, 2, 1, 0, 0, {0, 0, 0}, 0};
    TexVertex v[] = {{l, t, 0, 1, u0, v0}, {r, t, 0, 1, u1, v0}, {l, b, 0, 1, u0, v1}, {r, b, 0, 1, u1, v1}};
    d.data[0] = v;
    d.size[0] = sizeof v;
    gfx_draw(&d);
    std::memset(v, 0xaa, sizeof v);
}
Pixels sampled(GfxTex* source, unsigned n = 16)
{
    auto* out = texture(n);
    clear(out, n, 0x71345678);
    target(out);
    sample(source, n, -.5f, -.5f, float(n) - .5f, float(n) - .5f, 0, 0, 1, 1);
    auto p = read(out, n);
    gfx_tex_destroy(out);
    return p;
}
struct Context
{
    bool reference;
    GfxTex *scene, *mask, *depth, *small, *dest;
    explicit Context(bool ref)
        : reference(ref), scene(texture(1024)), mask(texture(1024)),
          depth(texture(1024, 1, GFX_TEX_2D, GFX_USE_DEPTH, 75)), small(texture()), dest(texture())
    {
        clear(mask, 1024, SEED);
        clear(small, 16, 0);
        clear(dest, 16, 0);
    }
    ~Context()
    {
        gfx_tex_destroy(dest);
        gfx_tex_destroy(small);
        gfx_tex_destroy(mask);
        gfx_tex_destroy(scene);
        gfx_tex_destroy(depth);
    }
};
struct Probe
{
    int l, t, r, b;
    float z;
};
// Synthetic screen rectangles/depths, not copied game geometry or assets.
const Probe synthetic[] = {{32, 48, 193, 239, .990f},   {211, 63, 338, 242, .991f},  {74, 321, 217, 493, .992f},
                           {379, 102, 492, 303, .993f}, {603, 415, 740, 596, .994f}, {451, 607, 562, 772, .995f},
                           {751, 681, 916, 878, .996f}};
void scene(Context& c, unsigned variant)
{
    clear(c.scene, 1024, SCENE, c.depth, 7);
    // An independent non-probe scene opens the required retained scene pass and
    // supplies changing occlusion, including odd output rows and columns.
    for (unsigned i = 0; i < 32; ++i)
    {
        const bool near = ((i + variant) % 3) != 0;
        solid(1024, float(i * 32) - .5f, -.5f, float(i * 32 + 31) - .5f, 1023.5f, near ? .985f : .999f,
              0xff304050u + i * 0x010307u, false, true, true);
    }
}
void probe_geometry(Context& c, const Probe& p, bool scissor = false)
{
    target(c.mask, c.depth);
    solid(1024, float(p.l), float(p.t), float(p.r), float(p.b), 0, 0, !c.reference, false, false, scissor);
    solid(1024, float(p.l), float(p.t), float(p.r), float(p.b), p.z, 0x80ffffffu, !c.reference, true, false, scissor);
    target(c.small);
    uint32_t vp[]{0, 0, 16, 16, 0, 0x3f800000};
    gfx_clear(0, nullptr, 1, 0, 1, 0, vp);
    sample(c.mask, 16, 0, 0, 16, 16, float(p.l + 1) / 1024, float(p.t + 1) / 1024, float(p.r) / 1024,
           float(p.b) / 1024);
    target(c.scene, c.depth);
    gfx_copy(c.small, 0, 0, 0, 0, 16, 16, c.dest, 0, 0, 0, 0);
}
Pixels probe(Context& c, const Probe& p, unsigned variant, uint64_t k = 0, unsigned age = 16, bool admit = true,
             bool scissor = false)
{
    if (!k)
        k = key();
    auto before = stats();
    scene(c, variant);
    probe_geometry(c, p, scissor);
    auto pixels = read(c.dest, 16, 0, 0, 2, k, age);
    auto after = stats();
    const uint64_t expected = !c.reference && admit && required ? 1 : 0;
    check(after[1] - before[1] == expected, "per-probe production commit proof");
    if (expected && after[1] - before[1] == 1)
        ++proofs;
    return pixels;
}
void compare_pair(const char* label, Context& r, Context& c, unsigned variant = 0)
{
    equal(label, probe(c, synthetic[0], variant), probe(r, synthetic[0], variant));
}
void fullcopy(GfxTex* from, GfxTex* to)
{
    gfx_copy(from, 0, 0, 0, 0, 16, 16, to, 0, 0, 0, 0);
}
void repeated_readback_command_lifecycle()
{
    const auto before = errors;
    auto* source = texture();
    auto* copy = texture();
    auto* unrelated = texture();
    clear(source, 16, SEED);
    fullcopy(source, copy);
    // The cold keyed read submits the copy and leaves no recording command
    // buffer. Source is already TRANSFER_SRC, so its next transition is a no-op.
    equal("copy-cold-keyed-submit", read(copy, 16, 0, 0, 2, key(), 16), Pixels(256, SEED));
    equal("unchanged-copy-source-first-exact", read(source), Pixels(256, SEED));
    equal("unchanged-copy-source-repeat-exact", read(source), Pixels(256, SEED));
    // A layout no-op must also end an unrelated active render pass before the
    // transfer command. Verify that ending it preserves its pending clear.
    clear(unrelated, 16, SCENE);
    equal("unchanged-source-during-unrelated-pass", read(source), Pixels(256, SEED));
    equal("unrelated-pass-clear-preserved", read(unrelated), Pixels(256, SCENE));
    gfx_tex_destroy(source);
    gfx_tex_destroy(copy);
    gfx_tex_destroy(unrelated);
    record("unchanged-layout-readback-command-lifecycle", before);
}
void synthetic_and_mask()
{
    const auto before = errors;
    Context r(true), c(false);
    for (unsigned i = 0; i < 7; ++i)
    {
        auto expected = probe(r, synthetic[i], i);
        auto got = probe(c, synthetic[i], i);
        equal("synthetic-cold-keyed", got, expected);
        const auto white = std::count(expected.begin(), expected.end(), 0x80ffffffu);
        check(white > 0 && white < 256, "reference must contain mixed visibility, not a blank image");
        for (auto v : expected)
            check(v == 0 || v == 0x80ffffffu, "reference is binary BGRA");
        equal("synthetic-exact-small", read(c.small), read(r.small));
    }
    equal("full1024-mask-preserves-outside-rectangle", read(c.mask, 1024), read(r.mask, 1024));
    equal("late-generic-mask-sampler", sampled(c.mask, 1024), sampled(r.mask, 1024));
    equal("scene-color-preserved", read(c.scene, 1024), read(r.scene, 1024));
    record("synthetic-seven-full-pixels-shared-mask-late-sampler", before);
}
void preserved_depth_stencil()
{
    const auto before = errors;
    Context r(true), c(false);
    for (auto* ctx : {&r, &c})
    {
        const auto s = stats();
        scene(*ctx, 0);
        target(ctx->scene, ctx->depth);
        // Stencil is deliberately nonzero and spatially varying before the
        // transaction, and no later clear can hide a damaged attachment.
        solid(1024, -.5f, -.5f, 511.5f, 1023.5f, .5f, 0, false, false, false, false, false, 1);
        probe_geometry(*ctx, synthetic[0]);
        read(ctx->dest, 16, 0, 0, 2, key(), 16);
        const auto e = stats();
        const uint64_t expected = !ctx->reference && required ? 1 : 0;
        check(e[1] - s[1] == expected, "depth/stencil case must commit");
        if (expected && e[1] - s[1] == 1)
            ++proofs;
        target(ctx->scene, ctx->depth);
        solid(1024, -.5f, -.5f, 1023.5f, 1023.5f, .99f, 0xff5a9317u, false, true, false);
    }
    auto depthR = read(r.scene, 1024);
    equal("postcommit-depth-dependent-draw", read(c.scene, 1024), depthR);
    check(depthR[10 * 1024 + 10] == 0xff5a9317u && depthR[10 * 1024 + 42] != 0xff5a9317u,
          "reference depth sentinel must both pass and fail");
    for (auto* ctx : {&r, &c})
    {
        target(ctx->scene, ctx->depth);
        solid(1024, -.5f, -.5f, 1023.5f, 1023.5f, .5f, 0xffc85731u, false, false, false, false, false, 2);
    }
    auto stencilR = read(r.scene, 1024);
    equal("postcommit-stencil-dependent-draw", read(c.scene, 1024), stencilR);
    check(stencilR[10 * 1024 + 10] == 0xffc85731u && stencilR[10 * 1024 + 700] != 0xffc85731u,
          "reference stencil sentinel must both pass and fail");
    record("preserved-depth-and-stencil-after-fast-commit", before);
}
void far_scene(Context& c)
{
    clear(c.scene, 1024, SCENE, c.depth, 7, 1);
    // Keep depth exactly 1. A wrong clamp of far Z to 1 would pass here and write
    // white, unlike the .985/.999 scene used by the ordinary fixture groups.
    target(c.scene, c.depth);
    solid(1024, -.5f, -.5f, 1023.5f, 1023.5f, .5f, SCENE, false);
    solid(1024, -.5f, -.5f, 511.5f, 1023.5f, .5f, 0, false, false, false, false, false, 1);
}
Pixels far_probe(Context& c, const Probe& p, uint64_t k, bool prepareScene = true)
{
    auto before = stats();
    if (prepareScene)
        far_scene(c);
    probe_geometry(c, p);
    auto pixels = read(c.dest, 16, 0, 0, 2, k, 16);
    auto after = stats();
    const uint64_t expected = !c.reference && required ? 1 : 0;
    check(after[1] - before[1] == expected, "far/boundary probe production commit proof");
    if (expected && after[1] - before[1] == 1)
    {
        ++proofs;
        if (p.z > 1)
            ++farProofs;
    }
    return pixels;
}
void far_clipped_white()
{
    const auto before = errors;
    Context r(true), c(false);
    const Probe far[] = {{238, 847, 250, 863, 1.0030274391174316f}, {51, 847, 66, 863, 1.0030295848846436f}};
    const float adjacent = std::nextafter(1.0f, std::numeric_limits<float>::infinity());
    check(adjacent > 1, "nextafter far boundary must differ from 1");
    for (const auto& syntheticFar : far)
        for (float z : {1.0f, adjacent, syntheticFar.z})
        {
            Probe p = syntheticFar;
            p.z = z;
            auto reference = far_probe(r, p, key());
            auto candidate = far_probe(c, p, key());
            const Pixels expected(256, z == 1 ? 0x80ffffffu : 0u);
            equal("far-boundary-independent-reference", reference, expected);
            equal("far-boundary-candidate", candidate, reference);
            equal("far-boundary-exact-small", read(c.small), expected);
            equal("far-boundary-exact-destination", read(c.dest), expected);
            auto maskR = read(r.mask, 1024);
            auto maskC = read(c.mask, 1024);
            equal("far-full-mask-after-prior-white", maskC, maskR);
            check(maskR[size_t(p.t + 1) * 1024 + p.l + 1] == expected[0],
                  "far mask interior must overwrite old pixels");
            check(maskR[0] == SEED, "far mask outside sentinel must survive");
            equal("far-late-mask-sample", sampled(c.mask, 1024), sampled(r.mask, 1024));
            equal("far-scene-color", read(c.scene, 1024), read(r.scene, 1024));
            auto* alias = texture();
            fullcopy(c.small, alias);
            equal("far-copied-version-sampler", sampled(alias), expected);
            gfx_tex_destroy(alias);
            for (auto* ctx : {&r, &c})
            {
                target(ctx->scene, ctx->depth);
                solid(1024, -.5f, -.5f, 1023.5f, 1023.5f, 1, 0xff5a9317u, false, true, false);
            }
            auto depthR = read(r.scene, 1024);
            equal("far-preserved-depth1", read(c.scene, 1024), depthR);
            check(depthR[10 * 1024 + 10] == 0xff5a9317u && depthR[10 * 1024 + 700] == 0xff5a9317u,
                  "far reference depth 1 must remain passing");
            for (auto* ctx : {&r, &c})
            {
                target(ctx->scene, ctx->depth);
                solid(1024, -.5f, -.5f, 1023.5f, 1023.5f, .5f, 0xffc85731u, false, false, false, false, false, 2);
            }
            auto stencilR = read(r.scene, 1024);
            equal("far-preserved-spatial-stencil", read(c.scene, 1024), stencilR);
            check(stencilR[10 * 1024 + 10] == 0xffc85731u && stencilR[10 * 1024 + 700] == 0xff5a9317u,
                  "far reference stencil must both pass and fail");
        }
    // Warm history is deliberately different from the newly produced far-zero
    // image. Earlier white may be returned, but must never become current pixels.
    const auto rk = key(), ck = key();
    Probe white = far[0];
    white.z = 1;
    equal("far-history-seed-white-reference", far_probe(r, white, rk), Pixels(256, 0x80ffffffu));
    equal("far-history-seed-white-candidate", far_probe(c, white, ck), Pixels(256, 0x80ffffffu));
    advance();
    auto refOld = far_probe(r, far[0], rk);
    auto gotOld = far_probe(c, far[0], ck);
    equal("far-history-earlier-white-reference", refOld, Pixels(256, 0x80ffffffu));
    equal("far-history-earlier-white-candidate", gotOld, refOld);
    equal("far-history-current-source-zero", read(c.small), Pixels(256, 0));
    equal("far-history-current-destination-zero", read(c.dest), Pixels(256, 0));
    advance();
    equal("far-history-completed-zero-reference", far_probe(r, far[0], rk), Pixels(256, 0));
    equal("far-history-completed-zero-candidate", far_probe(c, far[0], ck), Pixels(256, 0));
    // Prime separate real histories, then execute the actual eight-chain tail
    // with no intervening scene draw or synchronous exact observation. Warm
    // candidates must keep committing against the one retained scene pass.
    std::array<uint64_t, 8> rkeys{}, ckeys{};
    for (unsigned i = 0; i < 8; ++i)
    {
        rkeys[i] = key();
        ckeys[i] = key();
        equal("far-synthetic-eight-cold-reference", far_probe(r, far[i % 2], rkeys[i]), Pixels(256, 0));
        equal("far-synthetic-eight-cold-candidate", far_probe(c, far[i % 2], ckeys[i]), Pixels(256, 0));
    }
    advance();
    far_scene(r);
    for (unsigned i = 0; i < 8; ++i)
        equal("far-synthetic-eight-warm-reference", far_probe(r, far[i % 2], rkeys[i], false), Pixels(256, 0));
    far_scene(c);
    for (unsigned i = 0; i < 8; ++i)
        equal("far-synthetic-eight-warm-candidate", far_probe(c, far[i % 2], ckeys[i], false), Pixels(256, 0));
    equal("far-synthetic-tail-full-mask", read(c.mask, 1024), read(r.mask, 1024));
    record("far-clipped-white-depth1-boundary-full-mask-and-history", before);
}
void aliases()
{
    const auto before = errors;
    Context r(true), c(false);
    auto* ra = texture();
    auto* ca = texture();
    auto* rb = texture();
    auto* cb = texture();
    compare_pair("alias-A-probe", r, c, 0);
    fullcopy(r.small, ra);
    fullcopy(c.small, ca);
    compare_pair("alias-B-probe", r, c, 1);
    fullcopy(r.small, rb);
    fullcopy(c.small, cb);
    equal("old-copy-point-A", read(ca), read(ra));
    equal("new-copy-point-B", read(cb), read(rb));
    equal("source-now-B", read(c.small), read(r.small));
    fullcopy(rb, ra);
    fullcopy(cb, ca);
    equal("destination-alias-replaced", sampled(ca), sampled(ra));
    gfx_tex_destroy(ra);
    gfx_tex_destroy(ca);
    gfx_tex_destroy(rb);
    gfx_tex_destroy(cb);
    record("copy-point-source-and-destination-aliases", before);
}
void cpu_mutations()
{
    const auto before = errors;
    Context r(true), c(false);
    auto* siblingR = texture();
    auto* siblingC = texture();
    compare_pair("partial-upload-probe", r, c);
    fullcopy(r.small, siblingR);
    fullcopy(c.small, siblingC);
    const auto originalSibling = read(siblingR);
    uint32_t patch[3][7];
    for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 7; ++x)
            patch[y][x] = 0x51436587u + y * 0x170900u + x * 0x30105u;
    gfx_tex_upload_rect(r.small, 0, 0, 5, 6, 4, 3, patch, 28);
    gfx_tex_upload_rect(c.small, 0, 0, 5, 6, 4, 3, patch, 28);
    equal("partial-CPU-upload-preserves-copied-sibling", read(siblingC), originalSibling);
    equal("partial-upload-exact", read(c.small), read(r.small));
    equal("partial-upload-sampled", sampled(c.small), sampled(r.small));
    compare_pair("full-upload-probe", r, c, 1);
    Pixels fill(256);
    for (unsigned i = 0; i < 256; ++i)
        fill[i] = 0x19765432u + i * 0x030507u;
    gfx_tex_upload(r.small, 0, 0, fill.data(), 64);
    gfx_tex_upload(c.small, 0, 0, fill.data(), 64);
    equal("whole-upload-current-shadow", read(c.small), fill);
    equal("whole-upload-sampled", sampled(c.small), sampled(r.small));
    gfx_tex_destroy(siblingR);
    gfx_tex_destroy(siblingC);
    record("partial-and-whole-CPU-mutation", before);
}
void target_mutations()
{
    const auto before = errors;
    Context r(true), c(false);
    compare_pair("partial-clear-probe", r, c);
    const int32_t rect[]{-3, 5, 13, 21};
    const uint32_t vp[]{4, 3, 9, 10, 0, 0x3f800000};
    for (auto* ctx : {&r, &c})
    {
        target(ctx->small);
        gfx_clear(1, rect, 1, 0x5f927146u, 1, 0, vp);
    }
    equal("partial-clear-viewport", sampled(c.small), sampled(r.small));
    compare_pair("partial-draw-probe", r, c, 2);
    for (auto* ctx : {&r, &c})
    {
        target(ctx->small);
        solid(16, 2.5f, 3.5f, 11.5f, 12.5f, .5f, 0x80933751u, false, false, false, false, true);
    }
    equal("blending-reads-prior-virtual-image", sampled(c.small), sampled(r.small));
    record("partial-clear-and-blended-target-draw", before);
}
void partial_and_self_copy()
{
    const auto before = errors;
    Context r(true), c(false);
    auto* ra = texture();
    auto* ca = texture();
    compare_pair("partial-copy-A", r, c, 0);
    fullcopy(r.small, ra);
    fullcopy(c.small, ca);
    compare_pair("partial-copy-B", r, c, 1);
    gfx_copy(r.small, 0, 0, 2, 3, 7, 8, ra, 0, 0, 6, 5);
    gfx_copy(c.small, 0, 0, 2, 3, 7, 8, ca, 0, 0, 6, 5);
    equal("partial-copy-both-virtual-versions", read(ca), read(ra));
    compare_pair("self-copy-probe", r, c, 2);
    gfx_copy(r.small, 0, 0, 0, 1, 12, 13, r.small, 0, 0, 3, 2);
    gfx_copy(c.small, 0, 0, 0, 1, 12, 13, c.small, 0, 0, 3, 2);
    equal("overlapping-self-copy-snapshot", sampled(c.small), sampled(r.small));
    gfx_tex_destroy(ra);
    gfx_tex_destroy(ca);
    record("partial-copy-and-overlapping-self-copy", before);
}
void ordinary_subresources()
{
    const auto before = errors;
    Context r(true), c(false);
    auto* rm = texture(32, 2);
    auto* cm = texture(32, 2);
    auto* rc = texture(16, 1, GFX_TEX_CUBE);
    auto* cc = texture(16, 1, GFX_TEX_CUBE);
    Pixels seed32(1024, SEED), seed16(256, SEED);
    for (auto* t : {rm, cm})
    {
        gfx_tex_upload(t, 0, 0, seed32.data(), 128);
        gfx_tex_upload(t, 0, 1, seed16.data(), 64);
    }
    for (auto* t : {rc, cc})
        for (unsigned face = 0; face < 6; ++face)
            gfx_tex_upload(t, face, 0, seed16.data(), 64);
    compare_pair("subresource-probe", r, c);
    gfx_copy(r.small, 0, 0, 0, 0, 16, 16, rm, 0, 1, 0, 0);
    gfx_copy(c.small, 0, 0, 0, 0, 16, 16, cm, 0, 1, 0, 0);
    equal("mip-copy-destination", read(cm, 16, 0, 1), read(rm, 16, 0, 1));
    equal("other-mip-preserved", read(cm, 32), seed32);
    gfx_copy(r.small, 0, 0, 0, 0, 16, 16, rc, 3, 0, 0, 0);
    gfx_copy(c.small, 0, 0, 0, 0, 16, 16, cc, 3, 0, 0, 0);
    equal("cube-face-copy", read(cc, 16, 3), read(rc, 16, 3));
    equal("other-cube-face-preserved", read(cc, 16, 2), seed16);
    gfx_copy(r.small, 0, 0, 0, 0, 16, 16, rm, 0, 0, 7, 5);
    gfx_copy(c.small, 0, 0, 0, 0, 16, 16, cm, 0, 0, 7, 5);
    equal("larger-partial-destination", read(cm, 32), read(rm, 32));
    for (auto* t : {rm, cm, rc, cc})
        gfx_tex_destroy(t);
    record("mip-cube-and-larger-copy-fallback", before);
}
void reads_and_sampling()
{
    const auto before = errors;
    Context r(true), c(false);
    for (unsigned mode = 0; mode < 5; ++mode)
    {
        compare_pair("read-mode-probe", r, c, mode);
        const auto expected = read(r.small);
        Pixels got;
        if (mode == 0)
            got = read(c.small, 16, 0, 0, 1);
        else
            got = read(c.small, 16, 0, 0, 2, mode == 1 ? 0 : key(), mode == 2 ? 0 : mode == 3 ? 65 : 16);
        equal("generic-read-current", got, expected);
        equal("generic-sample-current", sampled(c.small), expected);
    }
    compare_pair("duplicate-probe", r, c, 1);
    const uint64_t k = key();
    auto a = read(c.small, 16, 0, 0, 2, k, 16);
    auto b = read(c.small, 16, 0, 0, 2, k, 16);
    equal("same-frame-duplicate-exact", b, a);
    record("exact-async-keyed-and-repeat-sampling", before);
}
void history_and_old_alias()
{
    const auto before = errors;
    Context r(true), c(false);
    auto* oldr = texture();
    auto* oldc = texture();
    const uint64_t rk = key(), ck = key();
    auto a = probe(r, synthetic[0], 0, rk);
    equal("history-cold", probe(c, synthetic[0], 0, ck), a);
    fullcopy(r.small, oldr);
    fullcopy(c.small, oldc);
    advance();
    auto bHistory = probe(r, synthetic[0], 1, rk);
    auto cbHistory = probe(c, synthetic[0], 1, ck);
    equal("warm-history-candidate-reference", cbHistory, bHistory);
    equal("warm-history-must-be-earlier", cbHistory, a);
    auto currentB = read(r.dest);
    check(currentB != a, "history scene variants must differ");
    equal("exact-current-B", read(c.dest), currentB);
    advance();
    auto cHistory = probe(c, synthetic[0], 2, ck);
    auto rHistory = probe(r, synthetic[0], 2, rk);
    equal("fresh-completed-version-proof", cHistory, currentB);
    equal("third-history-reference", cHistory, rHistory);
    for (unsigned i = 0; i < 18; ++i)
        advance();
    fullcopy(oldr, r.dest);
    fullcopy(oldc, c.dest);
    const auto s = stats();
    equal("aged-alias-is-current-exact", read(c.dest, 16, 0, 0, 2, ck, 16), read(r.dest, 16, 0, 0, 2, rk, 16));
    const auto e = stats();
    if (required)
        check(e[3] > s[3], "aged virtual alias must materialize for a fresh real copy");
    equal("aged-source-pixels-never-rejuvenated", read(oldc), a);
    gfx_tex_destroy(oldr);
    gfx_tex_destroy(oldc);
    record("warm-history-freshness-and-aged-copy", before);
}
void unsupported_fallbacks()
{
    const auto before = errors;
    Context r(true), c(false);
    const Probe reject[] = {{244, 175, 245, 347, .99f}, {238, 847, 250, 863, -.01f}};
    for (const auto& p : reject)
        equal("unsupported-geometry-fallback", probe(c, p, 0, 0, 16, false), probe(r, p, 0, 0, 16, false));
    equal("full-scissor-fallback", probe(c, synthetic[0], 1, 0, 16, false, true),
          probe(r, synthetic[0], 1, 0, 16, false, true));
    equal("age0-transaction-fallback", probe(c, synthetic[0], 2, 0, 0, false), probe(r, synthetic[0], 2, 0, 0, false));
    equal("fallback-fullmask", read(c.mask, 1024), read(r.mask, 1024));
    record("unsupported-state-and-cold-exact-fallback", before);
}
void unsupported_shapes_and_zero_key()
{
    const auto before = errors;
    Context r(true), c(false);
    for (auto* ctx : {&r, &c})
    {
        gfx_tex_destroy(ctx->small);
        gfx_tex_destroy(ctx->dest);
        ctx->small = texture(16, 1, GFX_TEX_2D, GFX_USE_RT, 22);
        ctx->dest = texture(16, 1, GFX_TEX_2D, GFX_USE_RT, 22);
    }
    equal("format22-original-fallback", probe(c, synthetic[0], 0, 0, 16, false),
          probe(r, synthetic[0], 0, 0, 16, false));
    auto* small = texture(8);
    clear(small, 8, 0x31867524u);
    const auto s = stats();
    equal("width8-keyed-original-path", read(small, 8, 0, 0, 2, key(), 16), Pixels(64, 0x31867524u));
    check(stats()[1] == s[1], "unsupported width cannot commit");
    gfx_tex_destroy(small);
    record("unsupported-format-and-small-shape", before);
    const auto zeroBefore = errors;
    Context zr(true), zc(false);
    const auto zstats = stats();
    scene(zr, 0);
    probe_geometry(zr, synthetic[0]);
    auto expected = read(zr.dest, 16, 0, 0, 2, 0, 16);
    scene(zc, 0);
    probe_geometry(zc, synthetic[0]);
    equal("zero-key-replay-exact", read(zc.dest, 16, 0, 0, 2, 0, 16), expected);
    check(stats()[1] == zstats[1], "zero key cannot commit recognized transaction");
    record("recognized-transaction-unknown-key-fallback", zeroBefore);
}
void interrupted_and_boundaries()
{
    const auto before = errors;
    Context r(true), c(false);
    for (unsigned boundary = 0; boundary < 4; ++boundary)
    {
        const auto s = stats();
        for (auto* ctx : {&r, &c})
        {
            scene(*ctx, boundary);
            target(ctx->mask, ctx->depth);
            const auto& p = synthetic[0];
            solid(1024, p.l, p.t, p.r, p.b, 0, 0, !ctx->reference);
            solid(1024, p.l, p.t, p.r, p.b, p.z, 0x80ffffffu, !ctx->reference, true);
            if (boundary == 0)
                gfx_finish();
            else if (boundary == 1)
                gfx_resize(1024, 1024);
            else if (boundary == 2)
                gfx_present(nullptr);
            else
            {
                target(ctx->scene, ctx->depth);
                sample(ctx->mask, 1024, -.5f, -.5f, 1023.5f, 1023.5f, 0, 0, 1, 1);
            }
        }
        equal("interrupted-journal-mask", read(c.mask, 1024), read(r.mask, 1024));
        const auto e = stats();
        check(e[1] == s[1], "interrupted transaction cannot commit");
        if (required)
            check(e[2] > s[2], "interrupted journal must replay");
    }
    record("journal-observation-target-finish-resize-present-boundaries", before);
}
void destroy_inflight()
{
    const auto before = errors;
    Context r(true), c(false);
    auto* ra = texture();
    auto* ca = texture();
    const auto rk = key(), ck = key();
    probe(r, synthetic[0], 0, rk);
    probe(c, synthetic[0], 0, ck);
    advance();
    probe(r, synthetic[0], 1, rk);
    probe(c, synthetic[0], 1, ck);
    fullcopy(r.small, ra);
    fullcopy(c.small, ca);
    // Warm keyed reads return earlier pixels, leaving newly recorded production
    // work pending. Destroy the source and retained MRT mask before any finish.
    gfx_tex_destroy(r.small);
    r.small = nullptr;
    gfx_tex_destroy(c.small);
    c.small = nullptr;
    gfx_tex_destroy(r.mask);
    r.mask = nullptr;
    gfx_tex_destroy(c.mask);
    c.mask = nullptr;
    r.small = texture();
    c.small = texture();
    r.mask = texture(1024);
    c.mask = texture(1024);
    clear(r.small, 16, 0x61234567);
    clear(c.small, 16, 0x61234567);
    equal("surviving-copy-after-source-mask-destroy", read(ca), read(ra));
    equal("replacement-cannot-inherit-old-version", read(c.small), Pixels(256, 0x61234567));
    gfx_tex_destroy(ra);
    gfx_tex_destroy(ca);
    record("destroy-inflight-alias-and-handle-reuse", before);
}
}
extern "C" int main(void)
{
    const char* env = std::getenv("FFXI_ANDROID_VISIBILITY_STORAGE");
    required = env && env[0] == '1' && !env[1];
    gfx_set_sync_pipelines(1);
    if (!gfx_init(nullptr, 0))
        return 2;
    const auto first = stats();
    check(bool(first[0]) == required, "actual transform activation differs from requested fixture mode");
    repeated_readback_command_lifecycle();
    synthetic_and_mask();
    preserved_depth_stencil();
    far_clipped_white();
    aliases();
    cpu_mutations();
    target_mutations();
    partial_and_self_copy();
    ordinary_subresources();
    reads_and_sampling();
    history_and_old_alias();
    unsupported_fallbacks();
    unsupported_shapes_and_zero_key();
    interrupted_and_boundaries();
    destroy_inflight();
    gfx_finish();
    const auto final = stats();
    materializations = final[3] - first[3];
    if (required)
    {
        check(farProofs > 0, "no far-clipped fast-path commits observed");
        check(proofs > 0, "no supported fast-path commits observed");
        check(materializations > 0, "no lazy image materialization observed");
    }
    const auto failures = gfx_failures();
    check(failures == 0, "backend pipeline/API failures");
    std::printf(
        "{\"event\":\"visibility-storage-final\",\"cases\":%llu,\"checks\":%llu,\"mismatches\":%llu,\"pipeline_failures\":%u,\"transform_required\":%s,\"committed_proofs\":%llu,\"far_clipped_proofs\":%llu,\"committed\":%llu,\"replayed\":%llu,\"materialized\":%llu,\"pool_misses\":%llu,\"actual_device_loss_tested\":false,\"allocation_failure_injection_tested\":false,\"game_fidelity_claim\":false,\"fps_claim\":false}\n",
        (unsigned long long)cases, (unsigned long long)checks, (unsigned long long)errors, failures,
        required ? "true" : "false", (unsigned long long)proofs, (unsigned long long)farProofs,
        (unsigned long long)(final[1] - first[1]), (unsigned long long)(final[2] - first[2]),
        (unsigned long long)materializations, (unsigned long long)(final[4] - first[4]));
    return errors ? 1 : 0;
}
#endif
