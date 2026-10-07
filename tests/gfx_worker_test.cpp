#include "../runtime/portable/gfx_worker_queue.h"
#include "../runtime/portable/gfx_worker_snapshot.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace gfxworker;
static std::atomic<unsigned> checks{0};
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "check %u failed: %s line %d\n", checks.load(), #x, __LINE__);                        \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)
struct Gate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool release = false, entered = false;
};
struct Mock
{
    Gate gate;
    std::vector<uint64_t> seen;
    std::thread::id producer;
    bool blocked = false;
    uint64_t drawSum = 0;
};
static void execute(void* context, uint32_t op, const void* data, size_t bytes)
{
    Mock& m = *static_cast<Mock*>(context);
    if (op == 1)
    {
        std::unique_lock<std::mutex> lock(m.gate.mutex);
        m.gate.entered = true;
        m.gate.changed.notify_all();
        m.gate.changed.wait(lock, [&] { return m.gate.release; });
    }
    else if (op == 2)
    {
        CHECK(bytes == sizeof(uint64_t));
        uint64_t n;
        std::memcpy(&n, data, 8);
        m.seen.push_back(n);
    }
    else if (op == 3)
    {
        const auto& d = *static_cast<const GfxDraw*>(data);
        CHECK(d.u.params[0] == 17);
        CHECK(d.data[0] && static_cast<const uint8_t*>(d.data[0])[0] == 5);
        CHECK(d.indices && static_cast<const uint16_t*>(d.indices)[4] == 4);
        CHECK(d.vs_tokens && d.vs_tokens[3] == 0xffff);
        CHECK(d.ps_tokens && d.ps_tokens[2] == 0xffff);
        m.drawSum = d.count;
    }
    else if (op == 4)
    {
        CHECK(bytes == 12);
        const auto* p = static_cast<const uint8_t*>(data);
        CHECK(p[0] == 3 && p[5] == 8 && p[6] == 11 && p[11] == 16);
    }
    else if (op == 5)
        throw std::runtime_error("expected worker failure");
    else if (op == 6)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        CHECK(bytes >= 8);
        uint64_t n;
        std::memcpy(&n, p, 8);
        for (size_t i = 8; i < bytes; ++i)
            CHECK(p[i] == uint8_t(n));
        m.seen.push_back(n);
    }
}
static void wait_entered(Mock& m)
{
    std::unique_lock<std::mutex> l(m.gate.mutex);
    m.gate.changed.wait(l, [&] { return m.gate.entered; });
}
static void release(Mock& m)
{
    {
        std::lock_guard<std::mutex> l(m.gate.mutex);
        m.gate.release = true;
    }
    m.gate.changed.notify_all();
}
int main()
{
    {
        Mock m;
        Queue q(16384, 8, execute, &m);
        q.put(1, 0, [](void*) {});
        wait_entered(m);
        GfxDraw d{};
        uint8_t stream[12]{5};
        uint16_t index[5]{0, 1, 2, 3, 4};
        uint32_t vs[4]{1, 2, 3, 0xffff}, ps[3]{1, 2, 0xffff};
        d.u.params[0] = 17;
        d.data[0] = stream;
        d.size[0] = 12;
        d.prim = GFX_TRIANGLEFAN;
        d.count = 3;
        d.indices = index;
        d.index_size = 2;
        d.vs.prog = 1;
        d.fs.prog = 2;
        d.vs_tokens = vs;
        d.ps_tokens = ps;
        DrawPlan plan;
        CHECK(draw_plan(d, 4, 3, plan));
        CHECK(q.put(3, plan.bytes, [&](void* p) { fill_draw(p, d, plan); }));
        std::memset(&d, 0, sizeof d);
        std::memset(stream, 0, sizeof stream);
        std::memset(index, 0, sizeof index);
        std::memset(vs, 0, sizeof vs);
        std::memset(ps, 0, sizeof ps);
        TextureInfo ti{23, 3, 2, 1, 1, GFX_USE_SAMPLE};
        UploadPlan upload;
        CHECK(upload_plan(ti, 0, 0, 0, 0, 3, 2, 8, upload));
        CHECK(upload.bytes == 12);
        // Exactly last-row rowBytes, no trailing pitch padding. ASan must catch
        // any rows*pitch source copy overreading this 14-byte allocation.
        auto data = std::unique_ptr<uint8_t[]>(new uint8_t[14]);
        for (unsigned i = 0; i < 14; ++i)
            data[i] = uint8_t(i + 3);
        CHECK(q.put(4, upload.bytes, [&](void* p) { fill_upload(p, data.get(), 8, upload); }));
        data.reset();
        release(m);
        q.drain();
        CHECK(m.drawSum == 3);
        CHECK(draw_plan(GfxDraw{}, 65538, 0, plan));
        GfxDraw invalid{};
        invalid.vs.prog = 1;
        invalid.vs_tokens = vs;
        CHECK(!draw_plan(invalid, 0, 0, plan));
        CHECK(!draw_plan(invalid, 65538, 0, plan));
        invalid = {};
        invalid.prim = GFX_TRIANGLELIST;
        invalid.count = UINT32_MAX;
        invalid.indices = index;
        invalid.index_size = 4;
        CHECK(!draw_plan(invalid, 0, 0, plan));
    }
    {
        Mock m;
        Queue q(128, 3, execute, &m);
        for (uint64_t i = 0; i < 3000; ++i)
            CHECK(q.put(2, 8, [&](void* p) { std::memcpy(p, &i, 8); }));
        q.drain();
        CHECK(m.seen.size() == 3000);
        for (size_t i = 0; i < m.seen.size(); ++i)
            CHECK(m.seen[i] == i);
        auto c = q.counters();
        CHECK(c.commands == 3000);
        CHECK(c.bytes == 24000);
        CHECK(!q.put(2, 129, [](void*) {}));
        q.stop();
        bool threw = false;
        try
        {
            q.put(2, 8, [](void*) {});
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        CHECK(threw);
    }
    {
        Mock m;
        Queue q(128, 2, execute, &m);
        q.put(1, 0, [](void*) {});
        wait_entered(m);
        std::atomic<bool> completed{false};
        std::thread producer([&] {
            for (uint64_t i = 0; i < 100; ++i)
                CHECK(q.put(2, 8, [&](void* p) { std::memcpy(p, &i, 8); }));
            completed = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(!completed);
        release(m);
        producer.join();
        q.drain();
        CHECK(m.seen.size() == 100);
        CHECK(q.counters().backpressure > 0);
    }
    {
        Mock m;
        Queue q(192, 5, execute, &m);
        for (uint64_t i = 0; i < 1000; ++i)
        {
            size_t bytes = 17 + size_t(i % 4) * 17;
            CHECK(q.put(6, bytes, [&](void* p) {
                std::memcpy(p, &i, 8);
                std::memset(static_cast<uint8_t*>(p) + 8, int(uint8_t(i)), bytes - 8);
            }));
        }
        q.drain();
        CHECK(m.seen.size() == 1000);
        for (size_t i = 0; i < m.seen.size(); ++i)
            CHECK(m.seen[i] == i);
        bool threw = false;
        try
        {
            q.put(6, 16, [](void*) { throw std::runtime_error("fill failed before publish"); });
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        CHECK(threw);
        CHECK(q.put(2, 8, [](void* p) {
            uint64_t n = 1000;
            std::memcpy(p, &n, 8);
        }));
        q.drain();
        CHECK(m.seen.back() == 1000);
    }
    {
        Mock m;
        Queue q(128, 2, execute, &m);
        CHECK(q.put(5, 0, [](void*) {}));
        bool threw = false;
        try
        {
            q.drain();
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        CHECK(threw);
        q.stop();
    }
    {
        TextureInfo t{0x31545844, 8, 8, 4, 6, GFX_USE_SAMPLE};
        UploadPlan p;
        CHECK(upload_plan(t, 5, 1, 0, 0, 4, 4, 8, p));
        CHECK(p.rows == 1 && p.rowBytes == 8 && p.bytes == 8);
        CHECK(!upload_plan(t, 6, 0, 0, 0, 4, 4, 8, p));
        CHECK(!upload_plan(t, 0, 4, 0, 0, 1, 1, 8, p));
        CHECK(!upload_plan(t, 0, 0, 1, 0, 4, 4, 8, p));
        t.use = GFX_USE_DEPTH;
        CHECK(!upload_plan(t, 0, 0, 0, 0, 4, 4, 8, p));
    }
    std::printf("worker queue/snapshot %u checks passed\n", checks.load());
}
