#include "gfx_pass_plan.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
static unsigned checks;
static void require(bool condition, const char* why)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "pass-plan failure: %s\n", why);
        std::abort();
    }
}
static GfxTex* tex(unsigned n)
{
    return reinterpret_cast<GfxTex*>(uintptr_t(n + 1));
}
static void snapshots()
{
    gfxplan::Queue queue;
    gfxplan::Target target{tex(1), 2, 3, nullptr};
    GfxDraw d{};
    d.prim = GFX_TRIANGLELIST;
    d.count = 2;
    d.index_size = 2;
    std::array<unsigned char, 96> vertices{};
    for (unsigned i = 0; i < vertices.size(); ++i)
        vertices[i] = i;
    uint16_t ix[] = {0, 1, 2, 2, 1, 3};
    uint32_t vs[] = {0xfffe0101, 0x0000ffff}, ps[] = {0xffff0101, 0x0000ffff};
    d.data[0] = vertices.data();
    d.size[0] = vertices.size();
    d.indices = ix;
    d.vs.prog = 123;
    d.fs.prog = 456;
    d.vs_tokens = vs;
    d.ps_tokens = ps;
    d.vs_token_count = d.ps_token_count = 2;
    d.tex[7] = tex(4);
    d.u.params[0] = 27;
    require(queue.try_draw(target, d), "snapshot accepted");
    auto bytes = vertices;
    auto first = queue.commands()[0].draw();
    vertices.fill(0);
    ix[0] = 99;
    vs[0] = ps[0] = 0;
    d.u.params[0] = 0;
    for (unsigned i = 0; i < bytes.size(); ++i)
        require(static_cast<const unsigned char*>(first->data[0])[i] == bytes[i], "UP snapshot");
    require(static_cast<const uint16_t*>(first->indices)[0] == 0, "index snapshot");
    require(first->vs_tokens[0] == 0xfffe0101 && first->ps_tokens[0] == 0xffff0101, "owned shader tokens");
    require(first->u.params[0] == 27, "uniform snapshot");
    for (unsigned i = 0; i < 128; ++i)
        require(queue.try_draw(target, d), "queue growth");
    require(queue.commands()[0].draw() == first, "payload stable after metadata relocation");
    require(static_cast<const unsigned char*>(first->data[0])[95] == 95, "bytes survive growth");
    queue.reset();
    require(queue.empty() && queue.bytes() == 0, "reset clears accounting");
    uint32_t vp[] = {1, 2, 3, 4, 0, 0x3f800000};
    int32_t rects[] = {-2, 7, 15, 20, 2, 3, 8, 9};
    require(queue.try_clear(target, 2, rects, 1, 0xff123456, .25f, 7, vp), "clear snapshot");
    rects[0] = 99;
    vp[0] = 99;
    require(queue.commands()[0].clear()->rects()[0] == -2, "clear rectangles owned");
    require(queue.commands()[0].clear()->vp[0] == 1, "clear viewport owned");
    gfxplan::Queue tiny(sizeof(GfxDraw) - 1);
    require(!tiny.try_draw(target, d) && tiny.empty() && !tiny.bytes(), "budget failure atomic");
    gfxplan::Queue one(1u << 20, 1);
    require(one.try_draw(target, d), "slot admission");
    size_t before = one.bytes();
    require(!one.try_draw(target, d) && one.size() == 1 && one.bytes() == before, "slot failure atomic");
    d.vs_token_count = 0;
    require(!queue.try_draw(target, d) && queue.size() == 1, "unknown token length rejects atomically");
    require(!queue.try_clear(target, 1, nullptr, 1, 0, 0, 0, vp), "invalid rectangles reject");
    require(!queue.try_clear({tex(0), 0, 0, tex(3)}, 0, nullptr, 3, 0, 0, 0, vp), "depth clear never deferred");
}
static void directed_hazards()
{
    gfxplan::Queue q;
    GfxDraw d{};
    d.prim = GFX_TRIANGLELIST;
    d.count = 1;
    d.tex[7] = tex(1);
    require(q.try_draw({tex(2), 0, 0, nullptr}, d), "hazard packet");
    GfxDraw next{};
    require(!q.conflicts({tex(3), 0, 0, tex(4)}, &next), "independent draw can cross");
    next.tex[0] = tex(2);
    require(q.conflicts({tex(3), 0, 0, tex(4)}, &next), "RAW flush");
    next.tex[0] = nullptr;
    require(q.conflicts({tex(1), 0, 0, tex(4)}, &next), "WAR color flush");
    require(q.conflicts({tex(3), 0, 0, tex(1)}, &next), "WAR depth flush");
    require(q.conflicts({tex(2), 5, 9, nullptr}, &next), "WAW across subresources conservatively flush");
    require(q.conflicts({tex(2), 0, 0, nullptr}, nullptr), "clear after queued write flush");
    require(q.conflicts({tex(1), 0, 0, nullptr}, nullptr), "clear after queued read flush");
}
/* Model independent resources with noncommutative destination blending and
 * sampled source values. An incorrect RAW/WAR/WAW rule changes exact outputs.
 * External mutations/observations use the required unconditional drain.
 */
static void randomized_semantics()
{
    std::mt19937 rng(0x70617373);
    uint64_t moved = 0;
    for (unsigned trial = 0; trial < 1000; ++trial)
    {
        std::array<uint32_t, 8> direct{}, planned{};
        gfxplan::Queue q;
        unsigned queuedCount = 0;
        auto execute = [](auto& state, unsigned dst, const GfxDraw& d) {
            uint32_t v = state[dst] * 17u + uint32_t(d.u.params[0]);
            for (unsigned i = 0; i < 8; ++i)
                if (d.tex[i])
                    v += state[uintptr_t(d.tex[i]) - 1] * (i * 2u + 1);
            state[dst] = v;
        };
        auto flush = [&] {
            for (const auto& c : q.commands())
                execute(planned, unsigned(uintptr_t(c.target.color) - 1), *c.draw());
            q.reset();
            queuedCount = 0;
        };
        for (unsigned step = 0; step < 200; ++step)
        {
            unsigned dst = rng() % 8;
            GfxDraw d{};
            d.prim = GFX_TRIANGLELIST;
            d.count = 1;
            d.u.params[0] = float(rng() % 65536);
            for (unsigned i = 0; i < 8; ++i)
                if (rng() % 9 == 0)
                    d.tex[i] = tex(rng() % 8);
            gfxplan::Target target{tex(dst), 0, 0, nullptr};
            execute(direct, dst, d);
            if (rng() % 3 == 0)
            {
                if (!q.try_draw(target, d))
                {
                    flush();
                    require(q.try_draw(target, d), "bounded packet retry");
                }
                ++queuedCount;
            }
            else
            {
                if (q.conflicts(target, &d))
                    flush();
                else
                    moved += queuedCount;
                execute(planned, dst, d);
            }
            if (rng() % 11 == 0)
            {
                flush();
                for (unsigned j = 0; j < 8; ++j)
                    require(direct[j] == planned[j], "observation after drain");
            }
        }
        flush();
        for (unsigned i = 0; i < 8; ++i)
            require(direct[i] == planned[i], "randomized sequential equivalence");
    }
    require(moved > 10000, "random stream actually reorders independent operations");
}
int main()
{
    snapshots();
    directed_hazards();
    randomized_semantics();
    std::printf("pass-plan host: %u checks, zero failures\n", checks);
}
