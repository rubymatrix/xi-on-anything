extern "C"
{
#include "../runtime/portable/gfx.h"
}
#include "../runtime/portable/gfx_worker.h"
#include "../runtime/portable/gfx_probe_backend.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
struct GfxBuf
{
    std::vector<unsigned char> data;
};
struct GfxTex
{
    uint32_t width, height, format;
    std::vector<unsigned char> data;
};
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
static std::thread::id mainThread;
static std::mutex gate;
static std::condition_variable changed;
static bool block = false, entered = false, release = false;
static std::vector<int> events;
static unsigned drawExpected = 0, draws = 0, probe = 0, front = 0, skip = 0;
static bool mode = false;
static void event(int n)
{
    events.push_back(n);
}
static void wait_enter()
{
    std::unique_lock<std::mutex> l(gate);
    changed.wait(l, [] { return entered; });
}
static void unblock()
{
    {
        std::lock_guard<std::mutex> l(gate);
        release = true;
    }
    changed.notify_all();
}
extern "C"
{
    gfxprobe::IssueStatus gfx_backend_probe_issue(GfxTex*, uint32_t, uint32_t, const gfxprobe::Request&)
    {
        CHECK(false);
        return gfxprobe::IssueStatus::Failed;
    }
    int gfx_backend_probe_poll(gfxprobe::Completion*, uint32_t)
    {
        CHECK(false);
        return -1;
    }
    bool gfx_backend_probe_exact(GfxTex*, uint32_t, uint32_t, uint64_t, gfxprobe::Pixels&)
    {
        CHECK(false);
        return false;
    }
    bool gfx_backend_probe_shutdown()
    {
        CHECK(false);
        return false;
    }
    int gfx_backend_init(void*, int)
    {
        CHECK(std::this_thread::get_id() == mainThread);
        event(1);
        return 1;
    }
    void gfx_backend_resize(uint32_t w, uint32_t h)
    {
        CHECK(w == 8 && h == 4);
        event(2);
    }
    GfxBuf* gfx_backend_buf_create(uint32_t n)
    {
        event(3);
        return new GfxBuf{std::vector<unsigned char>(n)};
    }
    void gfx_backend_buf_destroy(GfxBuf* b)
    {
        event(4);
        delete b;
    }
    void gfx_backend_buf_upload(GfxBuf* b, const void* d, uint32_t n)
    {
        CHECK(n <= b->data.size());
        std::memcpy(b->data.data(), d, n);
        event(5);
    }
    GfxTex* gfx_backend_tex_create(int, uint32_t fmt, uint32_t w, uint32_t h, uint32_t, int)
    {
        event(6);
        return new GfxTex{w, h, fmt, std::vector<unsigned char>(size_t(w) * h * 4)};
    }
    void gfx_backend_tex_destroy(GfxTex* t)
    {
        event(7);
        delete t;
    }
    void gfx_backend_tex_upload(GfxTex* t, uint32_t, uint32_t, const void* d, uint32_t pitch)
    {
        const auto* p = static_cast<const unsigned char*>(d);
        for (uint32_t r = 0; r < t->height; ++r)
            std::memcpy(t->data.data() + size_t(r) * t->width * 4, p + size_t(r) * pitch, t->width * 4);
        event(8);
    }
    void gfx_backend_tex_upload_rect(GfxTex* t, uint32_t, uint32_t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                     const void* d, uint32_t pitch)
    {
        const auto* p = static_cast<const unsigned char*>(d);
        for (uint32_t r = 0; r < h; ++r)
            std::memcpy(t->data.data() + (size_t(r + y) * t->width + x) * 4, p + size_t(r) * pitch, w * 4);
        event(9);
    }
    void gfx_backend_tex_read(GfxTex* t, uint32_t, uint32_t, void* d, uint32_t)
    {
        std::memcpy(d, t->data.data(), t->data.size());
        event(10);
    }
    void gfx_backend_tex_read_async(GfxTex* t, uint32_t f, uint32_t l, void* d, uint32_t p)
    {
        gfx_backend_tex_read(t, f, l, d, p);
        event(11);
    }
    void gfx_backend_tex_read_async_keyed(GfxTex* t, uint32_t f, uint32_t l, void* d, uint32_t p, uint64_t key,
                                          uint32_t age)
    {
        CHECK(key == 19 && age == 16);
        gfx_backend_tex_read(t, f, l, d, p);
        event(12);
    }
    void gfx_backend_android_probe_read(int a, int b)
    {
        CHECK(a == 1 && b == 1);
        ++probe;
        event(13);
    }
    void gfx_backend_copy(GfxTex* s, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, GfxTex* d, uint32_t,
                          uint32_t, uint32_t, uint32_t)
    {
        d->data = s->data;
        event(14);
    }
    void gfx_backend_set_targets(GfxTex* c, uint32_t f, uint32_t l, GfxTex* z)
    {
        CHECK(c && !z && f == 0 && l == 0);
        event(15);
    }
    void gfx_backend_clear(uint32_t n, const int32_t* r, uint32_t flags, uint32_t color, float z, uint32_t st,
                           const uint32_t vp[6])
    {
        CHECK(n == 1 && r[0] == 1 && r[3] == 4);
        CHECK(flags == 1 && color == 0xff008877 && z == 0.5f && st == 0);
        CHECK(vp[2] == 8 && vp[3] == 4);
        event(16);
    }
    void gfx_backend_draw(const GfxDraw* d)
    {
        if (block)
        {
            std::unique_lock<std::mutex> l(gate);
            entered = true;
            changed.notify_all();
            changed.wait(l, [] { return release; });
            block = false;
            ++draws;
            event(17);
            return;
        }
        CHECK((std::this_thread::get_id() != mainThread) == mode || drawExpected == 2 || drawExpected == 3);
        if (drawExpected == 3)
        {
            CHECK(d->buf[0] && d->ibuf == d->buf[0]);
            CHECK(d->data[0] == nullptr);
            CHECK(d->indices == nullptr || !mode);
            CHECK(d->vs_tokens == nullptr || !mode);
        }
        if (drawExpected == 1)
        {
            CHECK(d->u.params[0] == 3 && d->vp[2] == 8);
            CHECK(static_cast<const unsigned char*>(d->data[0])[1] == 9);
            CHECK(static_cast<const uint16_t*>(d->indices)[4] == 4);
            CHECK(d->vs_tokens[2] == 0xffff && d->ps_tokens[1] == 0xffff);
        }
        ++draws;
        event(17);
    }
    void gfx_backend_scene_done(GfxTex* t, const GfxScene* s)
    {
        CHECK(t && s && s->fog[0] == 2);
        event(18);
    }
    void gfx_backend_fx_set(const char* key, float f)
    {
        CHECK(!std::strcmp(key, "fog") && f == 2);
        event(19);
    }
    int gfx_backend_fx_constant(float*)
    {
        CHECK(false);
        return 0;
    }
    float gfx_backend_fx_get(const char* key)
    {
        CHECK(!std::strcmp(key, "fog"));
        event(20);
        return 2;
    }
    void gfx_backend_trace_dump(const char* path)
    {
        CHECK(!std::strcmp(path, "/test"));
        event(21);
    }
    void gfx_backend_set_focus(const float* p)
    {
        CHECK(p && p[0] == 1 && p[2] == 3);
        event(22);
    }
    void gfx_backend_present(GfxTex* t)
    {
        CHECK(t && std::this_thread::get_id() == mainThread);
        event(23);
    }
    void gfx_backend_finish()
    {
        event(24);
    }
    void gfx_backend_set_sync_pipelines(int on)
    {
        CHECK(on == 1);
        event(25);
    }
    uint32_t gfx_backend_failures()
    {
        event(26);
        return 0;
    }
    void gfx_backend_prof_front(uint64_t n)
    {
        CHECK(n == 5);
        ++front;
        event(27);
    }
    void gfx_backend_prof_skip(int n)
    {
        CHECK(n == GFX_SKIP_RANGE);
        ++skip;
        event(28);
    }
    void gfx_backend_prof_shim(uint64_t n)
    {
        CHECK(n == 6);
        event(29);
    }
}
int main(int argc, char** argv)
{
    mainThread = std::this_thread::get_id();
    mode = argc > 1 && !std::strcmp(argv[1], "on");
    setenv("FFXI_RENDER_WORKER", mode ? "1" : "0", 1);
    setenv("FFXI_RENDER_WORKER_DIAGNOSTICS", "0", 1);
    setenv("FFXI_RENDER_WORKER_MAILBOX", "0", 1);
    setenv("FFXI_RENDER_WORKER_CONST_FX", "0", 1);
    CHECK(gfx_init(nullptr, 0) == 1);
    GfxBuf* b = gfx_buf_create(16);
    GfxTex* t = gfx_tex_create(0, 21, 8, 4, 1, GFX_USE_RT);
    GfxTex* copy = gfx_tex_create(0, 21, 8, 4, 1, GFX_USE_RT);
    unsigned char buffer[16]{7};
    gfx_buf_upload(b, buffer, 16);
    std::memset(buffer, 0, 16);
    if (mode)
    {
        GfxDraw hold{};
        block = true;
        drawExpected = 0;
        gfx_draw(&hold);
        wait_enter();
    }
    gfx_resize(8, 4);
    gfx_set_targets(t, 0, 0, nullptr);
    uint32_t vp[6]{0, 0, 8, 4};
    int32_t rect[4]{1, 2, 3, 4};
    gfx_clear(1, rect, 1, 0xff008877, 0.5f, 0, vp);
    std::memset(rect, 0, sizeof rect);
    GfxDraw d{};
    unsigned char vertices[16]{0, 9};
    uint16_t idx[5]{0, 1, 2, 3, 4};
    uint32_t vs[3]{1, 2, 0xffff}, ps[2]{1, 0xffff};
    d.u.params[0] = 3;
    d.vp[2] = 8;
    d.data[0] = vertices;
    d.size[0] = 16;
    d.prim = GFX_TRIANGLEFAN;
    d.count = 3;
    d.indices = idx;
    d.index_size = 2;
    d.vs.prog = 1;
    d.fs.prog = 2;
    d.vs_tokens = vs;
    d.ps_tokens = ps;
    d.vs_token_count = 3;
    d.ps_token_count = 2;
    // Expected bytes are checked only after the hold packet, whose fields differ.
    if (!mode)
        drawExpected = 1;
    gfx_draw(&d);
    if (mode)
    {
        std::memset(&d, 0, sizeof d);
        std::memset(vertices, 0, sizeof vertices);
        std::memset(idx, 0, sizeof idx);
        std::memset(vs, 0, sizeof vs);
        std::memset(ps, 0, sizeof ps);
    }
    unsigned char texture[128];
    std::memset(texture, 5, sizeof texture);
    gfx_tex_upload(t, 0, 0, texture, 32);
    std::memset(texture, 0, sizeof texture);
    // Rect source has exactly rowbytes on its final row, no pitch tail.
    auto* rectSource = new unsigned char[20];
    std::memset(rectSource, 9, 20);
    gfx_tex_upload_rect(t, 0, 0, 1, 1, 2, 2, rectSource, 12);
    delete[] rectSource;
    GfxScene scene{};
    scene.fog[0] = 2;
    gfx_scene_done(t, &scene);
    std::memset(&scene, 0, sizeof scene);
    char key[4] = "fog", path[6] = "/test";
    float focus[3]{1, 2, 3};
    gfx_fx_set(key, 2);
    gfx_trace_dump(path);
    gfx_set_focus(focus);
    std::memset(key, 0, sizeof key);
    std::memset(path, 0, sizeof path);
    std::memset(focus, 0, sizeof focus);
    gfx_prof_front(5);
    gfx_prof_skip(GFX_SKIP_RANGE);
    gfx_prof_shim(6);
    gfx_android_probe_read(1, 1);
    gfx_set_sync_pipelines(1);
    if (mode)
    {
        drawExpected = 1;
        unblock();
    }
    unsigned char result[128]{};
    gfx_tex_read_async_keyed(t, 0, 0, result, 32, 19, 16);
    CHECK(result[0] == 5 && result[36] == 9 && result[68] == 9);
    CHECK(b->data[0] == 7);
    CHECK(draws == (mode ? 2u : 1u));
    CHECK(probe == 1 && front == 1 && skip == 1);
    CHECK(gfx_fx_get("fog") == 2);
    gfx_copy(t, 0, 0, 0, 0, 8, 4, copy, 0, 0, 0, 0);
    std::memset(result, 0, sizeof result);
    gfx_tex_read(copy, 0, 0, result, 32);
    CHECK(result[0] == 5 && result[36] == 9);
    gfx_tex_read_async(copy, 0, 0, result, 32);
    gfx_present(t);
    CHECK(events.back() == 23);
    gfx_finish();
    CHECK(gfx_failures() == 0);
    std::vector<int> expected{1, 3, 6, 6, 5};
    if (mode)
        expected.push_back(17);
    const int remaining[]{2,  15, 16, 17, 8,  9,  18, 19, 21, 22, 27, 28, 29,
                          13, 25, 10, 12, 20, 14, 10, 10, 11, 23, 24, 26};
    expected.insert(expected.end(), std::begin(remaining), std::end(remaining));
    CHECK(events == expected);
    // Static buffer upload/draw/update stays in FIFO; handles remain valid until
    // destruction, and readback/result calls observe all prior updates.
    drawExpected = 3;
    GfxDraw stat{};
    stat.buf[0] = b;
    stat.ibuf = b;
    stat.indices = reinterpret_cast<void*>(uintptr_t(1));
    stat.vs_tokens = reinterpret_cast<uint32_t*>(uintptr_t(1));
    gfx_draw(&stat);
    unsigned char update[16]{11};
    gfx_buf_upload(b, update, 16);
    std::memset(update, 0, sizeof update);
    gfx_draw(&stat);
    gfx_finish();
    CHECK(b->data[0] == 11);
    // Serialized callers: use no-result profile operations, then verify exact
    // backend count after the result barrier. Backend callbacks never take the
    // wrapper producer gate, so a full queue cannot deadlock the producer.
    std::thread p1([] {
        for (unsigned i = 0; i < 1000; ++i)
            gfx_prof_front(5);
    });
    std::thread p2([] {
        for (unsigned i = 0; i < 1000; ++i)
            gfx_prof_skip(GFX_SKIP_RANGE);
    });
    p1.join();
    p2.join();
    gfx_finish();
    CHECK(front == 1001 && skip == 1001);
    drawExpected = 2;
    GfxDraw large{};
    auto big = std::vector<unsigned char>((32u << 20) + 1);
    large.data[0] = big.data();
    large.size[0] = uint32_t(big.size());
    gfx_draw(&large);
    CHECK(events.back() == 17);
    // Zero token count cannot be guessed; direct fallback executes synchronously.
    drawExpected = 2;
    GfxDraw unknown{};
    unknown.vs.prog = 1;
    unknown.vs_tokens = vs;
    unsigned before = draws;
    gfx_draw(&unknown);
    CHECK(draws == before + 1);
    if (mode)
    {
        // Shutdown must wait for a currently executing draw and all following
        // destruction commands before joining; returning early loses resources
        // or lets the backend outlive the Android entrypoint.
        {
            std::lock_guard<std::mutex> l(gate);
            block = true;
            entered = false;
            release = false;
        }
        GfxDraw pending{};
        gfx_draw(&pending);
        wait_enter();
        const unsigned beforeShutdownDraws = draws;
        const size_t beforeShutdownEvents = events.size();
        gfx_tex_destroy(copy);
        gfx_tex_destroy(t);
        gfx_buf_destroy(b);
        std::atomic<bool> shutdownReturned{false};
        std::thread releaser([&] {
            std::unique_lock<std::mutex> l(gate);
            CHECK(!changed.wait_for(l, std::chrono::milliseconds(40), [&] { return shutdownReturned.load(); }));
            CHECK(draws == beforeShutdownDraws);
            CHECK(events.size() == beforeShutdownEvents);
            release = true;
            l.unlock();
            changed.notify_all();
        });
        gfx_worker_shutdown();
        shutdownReturned = true;
        changed.notify_all();
        releaser.join();
        CHECK(draws == beforeShutdownDraws + 1);
        CHECK(events.size() == beforeShutdownEvents + 4);
        CHECK(events[beforeShutdownEvents] == 17 && events[beforeShutdownEvents + 1] == 7 &&
              events[beforeShutdownEvents + 2] == 7 && events[beforeShutdownEvents + 3] == 4);
        // After join, the unchanged wrapper must execute directly.
        const unsigned beforeDirect = front;
        gfx_prof_front(5);
        CHECK(front == beforeDirect + 1);
    }
    else
    {
        gfx_tex_destroy(copy);
        gfx_tex_destroy(t);
        gfx_buf_destroy(b);
        gfx_worker_shutdown();
        CHECK(events[events.size() - 1] == 4);
    }
    std::printf("worker API %s %u checks passed, %zu backend events\n", mode ? "on" : "off", checks.load(),
                events.size());
}
