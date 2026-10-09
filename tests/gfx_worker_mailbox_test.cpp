/* Production gfx_worker.cpp against a deterministic CPU-only backend.
 * clang++ -std=c++17 -O1 -g -DFFXI_ANDROID_VULKAN -pthread \
 *   -fsanitize=address,undefined -fno-omit-frame-pointer \
 *   runtime/portable/gfx_worker.cpp tests/gfx_worker_mailbox_test.cpp -o /tmp/xi-worker-mailbox
 * Run fresh processes: off, worker-only, mailbox, mailbox-issue-failed,
 * mailbox-poll-failed, mailbox-exact-failed, const-fx-on, const-fx-off,
 * const-fx-unsupported, const-fx-worker-off
 * This fixture proves wrapper ordering/ownership/policy, not Vulkan or FPS.
 */
extern "C"
{
#include "../runtime/portable/gfx.h"
}
#include "../runtime/portable/gfx_worker.h"
#include "../runtime/portable/gfx_probe_backend.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct GfxBuf
{
    std::vector<uint8_t> data;
};
struct GfxTex
{
    bool live = false;
    uint32_t width = 16, height = 16, format = 21;
    std::array<uint8_t, 1024> data{};
};
using namespace gfxprobe;
static std::atomic<uint64_t> checks{0};
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                                  \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)
static std::thread::id producer;
static std::array<GfxTex, 32> textures;
static GfxTex* target = nullptr;
static std::atomic<unsigned> exactCalls{0}, plainReads{0}, legacyReads{0}, issues{0}, polls{0}, presents{0}, draws{0};
static std::atomic<unsigned> shutdownCalls{0};
static std::atomic<bool> allowPoll{true}, pollFailure{false}, exactSuccess{true};
static bool legacySentinel = false, constantFxSupported = false;
static float fxBackendValue = 17.25f;
static std::atomic<unsigned> constantFxQueries{0}, fxGets{0};
static std::atomic<IssueStatus> issueStatus{IssueStatus::Queued};
static std::mutex pendingMutex;
static std::deque<Completion> pending;
static std::vector<Request> issued;
static uint64_t serial = 1;
struct Hold
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
} hold;
constexpr float BLOCK_DRAW = -713.0f;

static void copy_pixels(GfxTex* t, void* destination, uint32_t pitch)
{
    CHECK(t && t->live && destination && pitch >= t->width * 4);
    for (uint32_t y = 0; y < t->height; ++y)
        std::memcpy(static_cast<uint8_t*>(destination) + size_t(y) * pitch, t->data.data() + size_t(y) * t->width * 4,
                    t->width * 4);
}
static void await_hold()
{
    std::unique_lock<std::mutex> lock(hold.mutex);
    CHECK(hold.changed.wait_for(lock, std::chrono::seconds(2), [] { return hold.entered; }));
}
static void release_hold()
{
    {
        std::lock_guard<std::mutex> lock(hold.mutex);
        hold.released = true;
    }
    hold.changed.notify_all();
}
static void block_worker()
{
    {
        std::lock_guard<std::mutex> lock(hold.mutex);
        hold.entered = false;
        hold.released = false;
    }
    GfxDraw d{};
    d.u.params[0] = BLOCK_DRAW;
    gfx_draw(&d);
    await_hold();
}
static void fill_gpu(GfxTex* t, uint8_t value)
{
    gfx_set_targets(t, 0, 0, nullptr);
    uint32_t vp[6]{0, 0, 16, 16, 0, 0x3f800000};
    gfx_clear(0, nullptr, 1, uint32_t(value) * 0x01010101u, 1.0f, 0, vp);
}
static GfxTex* fresh(uint8_t value)
{
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, 16, 16, 1, GFX_USE_RT);
    CHECK(t);
    fill_gpu(t, value);
    gfx_finish();
    return t;
}
static void expect_pixels(const uint8_t* data, uint8_t value, uint32_t pitch = 64)
{
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 64; ++x)
            CHECK(data[y * pitch + x] == value);
}
static unsigned current_reads()
{
    return exactCalls.load() + plainReads.load();
}
static Pixels read(GfxTex* t, uint64_t key, uint32_t age = 16)
{
    Pixels out{};
    gfx_tex_read_async_keyed(t, 0, 0, out.data(), 64, key, age);
    return out;
}
static void seed(GfxTex* t, uint64_t key, uint8_t value)
{
    auto out = read(t, key);
    expect_pixels(out.data(), value);
    gfx_present(t);
}
static void copy_surface(GfxTex* src, GfxTex* dst)
{
    gfx_copy(src, 0, 0, 0, 0, 16, 16, dst, 0, 0, 0, 0);
}
// A watchdog releases the intentionally blocked worker if a regressed warm
// read drains it; the test then fails rather than deadlocking indefinitely.
template <class Action> static void must_return_while_worker_blocked(Action action)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool returned = false, early = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        early = changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return returned; });
        lock.unlock();
        release_hold();
    });
    action();
    {
        std::lock_guard<std::mutex> lock(mutex);
        returned = true;
    }
    changed.notify_one();
    watchdog.join();
    CHECK(early);
}
// Start the exact-path call on its own serialized producer. It must not reach
// the backend or return until the earlier FIFO draw is explicitly released.
template <class Action> static void must_wait_for_worker(Action action)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false, returned = false;
    const unsigned beforeExact = exactCalls.load(), beforePlain = plainReads.load(), beforeFx = fxGets.load();
    std::thread reader([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            started = true;
        }
        changed.notify_one();
        action();
        {
            std::lock_guard<std::mutex> lock(mutex);
            returned = true;
        }
        changed.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(2), [&] { return started; }));
        CHECK(!changed.wait_for(lock, std::chrono::milliseconds(40), [&] { return returned; }));
        CHECK(exactCalls == beforeExact && plainReads == beforePlain && fxGets == beforeFx);
    }
    release_hold();
    reader.join();
    CHECK(returned);
}

extern "C"
{
    int gfx_backend_init(void*, int)
    {
        CHECK(std::this_thread::get_id() == producer);
        return 1;
    }
    void gfx_backend_resize(uint32_t, uint32_t) {}
    GfxBuf* gfx_backend_buf_create(uint32_t n)
    {
        return new GfxBuf{std::vector<uint8_t>(n)};
    }
    void gfx_backend_buf_destroy(GfxBuf* b)
    {
        delete b;
    }
    void gfx_backend_buf_upload(GfxBuf* b, const void* p, uint32_t n)
    {
        CHECK(b && n <= b->data.size());
        std::memcpy(b->data.data(), p, n);
    }
    GfxTex* gfx_backend_tex_create(int, uint32_t fmt, uint32_t w, uint32_t h, uint32_t, int)
    {
        CHECK(w <= 16 && h <= 16);
        for (auto& t : textures)
            if (!t.live)
            {
                t = GfxTex{};
                t.live = true;
                t.width = w;
                t.height = h;
                t.format = fmt;
                return &t;
            }
        CHECK(false);
        return nullptr;
    }
    void gfx_backend_tex_destroy(GfxTex* t)
    {
        CHECK(t && t->live);
        t->live = false;
        if (target == t)
            target = nullptr;
    }
    void gfx_backend_tex_upload(GfxTex* t, uint32_t, uint32_t, const void* p, uint32_t pitch)
    {
        CHECK(t && t->live && p && pitch >= t->width * 4);
        for (uint32_t y = 0; y < t->height; ++y)
            std::memcpy(t->data.data() + size_t(y) * t->width * 4, static_cast<const uint8_t*>(p) + size_t(y) * pitch,
                        t->width * 4);
    }
    void gfx_backend_tex_upload_rect(GfxTex* t, uint32_t, uint32_t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                     const void* p, uint32_t pitch)
    {
        CHECK(t && t->live && x + w <= t->width && y + h <= t->height && pitch >= w * 4);
        for (uint32_t row = 0; row < h; ++row)
            std::memcpy(t->data.data() + (size_t(y + row) * t->width + x) * 4,
                        static_cast<const uint8_t*>(p) + size_t(row) * pitch, w * 4);
    }
    void gfx_backend_tex_read(GfxTex* t, uint32_t, uint32_t, void* p, uint32_t pitch)
    {
        ++plainReads;
        copy_pixels(t, p, pitch);
    }
    void gfx_backend_tex_read_async(GfxTex* t, uint32_t f, uint32_t l, void* p, uint32_t pitch)
    {
        gfx_backend_tex_read(t, f, l, p, pitch);
    }
    void gfx_backend_tex_read_async_keyed(GfxTex* t, uint32_t, uint32_t, void* p, uint32_t pitch, uint64_t, uint32_t)
    {
        ++legacyReads;
        copy_pixels(t, p, pitch);
        if (legacySentinel)
            for (uint32_t y = 0; y < t->height; ++y)
                std::memset(static_cast<uint8_t*>(p) + y * pitch, 0xe7, t->width * 4);
    }
    void gfx_backend_android_probe_read(int, int) {}
    void gfx_backend_copy(GfxTex* src, uint32_t, uint32_t, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                          GfxTex* dst, uint32_t, uint32_t, uint32_t dx, uint32_t dy)
    {
        CHECK(src && dst && src->live && dst->live && sx + w <= src->width && sy + h <= src->height &&
              dx + w <= dst->width && dy + h <= dst->height);
        auto bytes = src->data; // self-copy is deliberately well-defined in mock
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(dst->data.data() + (size_t(dy + y) * dst->width + dx) * 4,
                        bytes.data() + (size_t(sy + y) * src->width + sx) * 4, w * 4);
    }
    void gfx_backend_set_targets(GfxTex* t, uint32_t, uint32_t, GfxTex*)
    {
        target = t;
    }
    void gfx_backend_clear(uint32_t, const int32_t*, uint32_t flags, uint32_t color, float, uint32_t, const uint32_t*)
    {
        CHECK(target && target->live);
        if (flags & 1)
            for (unsigned i = 0; i < 256; ++i)
                std::memcpy(target->data.data() + 4 * i, &color, 4);
    }
    void gfx_backend_draw(const GfxDraw* d)
    {
        CHECK(d);
        if (d->u.params[0] == BLOCK_DRAW)
        {
            std::unique_lock<std::mutex> lock(hold.mutex);
            hold.entered = true;
            hold.changed.notify_all();
            CHECK(hold.changed.wait_for(lock, std::chrono::seconds(5), [] { return hold.released; }));
        }
        ++draws;
    }
    void gfx_backend_scene_done(GfxTex*, const GfxScene*) {}
    void gfx_backend_fx_set(const char*, float value)
    {
        if (!constantFxSupported)
            fxBackendValue = value;
    }
    float gfx_backend_fx_get(const char*)
    {
        ++fxGets;
        return fxBackendValue;
    }
    int gfx_backend_fx_constant(float* value)
    {
        ++constantFxQueries;
        CHECK(std::this_thread::get_id() == producer);
        if (!constantFxSupported || !value)
            return 0;
        *value = fxBackendValue;
        return 1;
    }
    void gfx_backend_trace_dump(const char*) {}
    void gfx_backend_set_focus(const float*) {}
    void gfx_backend_present(GfxTex* t)
    {
        CHECK(t && t->live && std::this_thread::get_id() == producer);
        ++presents;
    }
    void gfx_backend_finish() {}
    void gfx_backend_set_sync_pipelines(int) {}
    uint32_t gfx_backend_failures()
    {
        return 0;
    }
    void gfx_backend_prof_front(uint64_t) {}
    void gfx_backend_prof_skip(int) {}
    void gfx_backend_prof_shim(uint64_t) {}
    IssueStatus gfx_backend_probe_issue(GfxTex* t, uint32_t face, uint32_t mip, const Request& request)
    {
        ++issues;
        CHECK(t && t->live && t->width == 16 && t->height == 16 && !face && !mip && request);
        IssueStatus status = issueStatus.load();
        if (status != IssueStatus::Queued)
            return status;
        std::lock_guard<std::mutex> lock(pendingMutex);
        issued.push_back(request);
        pending.push_back({request, t->data, serial++});
        return status;
    }
    int gfx_backend_probe_poll(Completion* out, uint32_t cap)
    {
        ++polls;
        if (pollFailure)
            return -1;
        if (!allowPoll)
            return 0;
        std::lock_guard<std::mutex> lock(pendingMutex);
        unsigned n = 0;
        while (n < cap && !pending.empty())
        {
            out[n++] = pending.front();
            pending.pop_front();
        }
        return int(n);
    }
    bool gfx_backend_probe_exact(GfxTex* t, uint32_t face, uint32_t mip, uint64_t, Pixels& out)
    {
        ++exactCalls;
        CHECK(t && t->live && t->width == 16 && t->height == 16 && !face && !mip);
        if (!exactSuccess)
            return false;
        out = t->data;
        return true;
    }
    bool gfx_backend_probe_shutdown()
    {
        ++shutdownCalls;
        std::lock_guard<std::mutex> lock(pendingMutex);
        pending.clear();
        return true;
    }
}

static void test_original_paths(bool worker)
{
    GfxTex* t = fresh(7);
    auto value = read(t, 17);
    expect_pixels(value.data(), 7);
    CHECK(legacyReads == 1 && exactCalls == 0 && issues == 0 && polls == 0);
    if (worker)
    {
        block_worker();
        fill_gpu(t, 9);
        must_wait_for_worker([&] {
            auto out = read(t, 17);
            expect_pixels(out.data(), 9);
        });
    }
    gfx_present(t);
    CHECK(presents == 1);
    gfx_tex_destroy(t);
    gfx_finish();
    gfx_worker_shutdown();
    CHECK(shutdownCalls == 0);
}
static void test_warm_cold_and_present()
{
    GfxTex *t = fresh(11), *cold = fresh(31);
    seed(t, 101, 11);
    const unsigned before = exactCalls.load();
    block_worker();
    fill_gpu(t, 22);
    std::array<uint8_t, 16 * 80 + 16> padded;
    padded.fill(0xcd);
    must_return_while_worker_blocked([&] { gfx_tex_read_async_keyed(t, 0, 0, padded.data(), 80, 101, 16); });
    expect_pixels(padded.data(), 11, 80);
    CHECK(exactCalls == before);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 64; x < 80; ++x)
            CHECK(padded[y * 80 + x] == 0xcd);
    for (unsigned i = 16 * 80; i < padded.size(); ++i)
        CHECK(padded[i] == 0xcd);
    padded.fill(0x99); // returned caller storage must not be retained
    gfx_present(t);
    CHECK(issues >= 1);
    auto current = read(t, 101);
    expect_pixels(current.data(), 22);
    // A first read has no history and waits for the exact command-stream value.
    block_worker();
    fill_gpu(cold, 32);
    must_wait_for_worker([&] {
        auto out = read(cold, 102);
        expect_pixels(out.data(), 32);
    });
    // Present itself keeps its barrier and runs the actual backend on producer.
    block_worker();
    const unsigned drawBefore = draws.load(), presentBefore = presents.load();
    std::thread release([&] {
        std::unique_lock<std::mutex> lock(hold.mutex);
        CHECK(!hold.changed.wait_for(lock, std::chrono::milliseconds(40),
                                     [&] { return presents.load() != presentBefore; }));
        CHECK(draws == drawBefore);
        hold.released = true;
        lock.unlock();
        hold.changed.notify_all();
    });
    gfx_present(t);
    release.join();
    CHECK(presents == presentBefore + 1 && draws == drawBefore + 1);
}
static void test_duplicate_age_and_key()
{
    GfxTex* t = fresh(40);
    seed(t, 201, 40);
    auto history = read(t, 201);
    expect_pixels(history.data(), 40);
    fill_gpu(t, 41);
    unsigned before = exactCalls.load();
    auto duplicate = read(t, 201);
    expect_pixels(duplicate.data(), 41);
    CHECK(exactCalls == before + 1);
    gfx_present(t);
    before = exactCalls.load();
    auto exactAge = read(t, 202, 0);
    expect_pixels(exactAge.data(), 41);
    fill_gpu(t, 42);
    auto second = read(t, 202, 16);
    expect_pixels(second.data(), 42);
    CHECK(exactCalls == before + 2);
    auto unknown = read(t, 0);
    expect_pixels(unknown.data(), 42);
    // Invalid ages are exact and still consume this key in the current frame.
    gfx_present(t);
    before = exactCalls.load();
    auto invalidAge = read(t, 204, 65);
    expect_pixels(invalidAge.data(), 42);
    fill_gpu(t, 45);
    auto invalidDuplicate = read(t, 204);
    expect_pixels(invalidDuplicate.data(), 45);
    CHECK(exactCalls == before + 2);
    fill_gpu(t, 42);
    gfx_present(t);
    seed(t, 203, 42);
    for (unsigned n = 0; n < 20; ++n)
        gfx_present(t);
    fill_gpu(t, 43);
    before = exactCalls.load();
    auto stale = read(t, 203);
    expect_pixels(stale.data(), 43);
    CHECK(exactCalls == before + 1);
    // Busy refresh cannot force an otherwise warm read to wait for the worker.
    gfx_present(t);
    issueStatus = IssueStatus::Busy;
    block_worker();
    fill_gpu(t, 44);
    must_return_while_worker_blocked([&] {
        auto out = read(t, 203);
        expect_pixels(out.data(), 43);
    });
    gfx_present(t);
    issueStatus = IssueStatus::Queued;
}
static void test_permanent_poison()
{
    GfxTex *cpu = fresh(50), *partial = fresh(60), *self = fresh(70), *unknownDst = fresh(80), *taintedDst = fresh(90);
    seed(cpu, 301, 50);
    Pixels upload;
    upload.fill(51);
    block_worker();
    gfx_tex_upload(cpu, 0, 0, upload.data(), 64);
    upload.fill(0);
    must_wait_for_worker([&] {
        auto out = read(cpu, 301);
        expect_pixels(out.data(), 51);
    });
    fill_gpu(cpu, 52);
    gfx_present(cpu);
    unsigned before = current_reads(), issueBefore = issues.load();
    auto remainsExact = read(cpu, 301);
    expect_pixels(remainsExact.data(), 52);
    CHECK(current_reads() == before + 1);
    gfx_finish();
    CHECK(issues == issueBefore);
    seed(partial, 302, 60);
    uint32_t pixel = 0x3d3d3d3d;
    gfx_tex_upload_rect(partial, 0, 0, 1, 1, 1, 1, &pixel, 4);
    pixel = 0;
    auto changed = read(partial, 302);
    CHECK(changed[(16 + 1) * 4] == 61 && changed[0] == 60);
    fill_gpu(partial, 62);
    gfx_present(partial);
    before = current_reads();
    auto stillPartial = read(partial, 302);
    expect_pixels(stillPartial.data(), 62);
    CHECK(current_reads() == before + 1);
    seed(self, 303, 70);
    copy_surface(self, self);
    gfx_present(self);
    before = current_reads();
    auto same = read(self, 303);
    expect_pixels(same.data(), 70);
    CHECK(current_reads() == before + 1);
    GfxTex unknown;
    unknown.live = true;
    unknown.data.fill(81);
    seed(unknownDst, 304, 80);
    copy_surface(&unknown, unknownDst);
    gfx_present(unknownDst);
    before = current_reads();
    auto outside = read(unknownDst, 304);
    expect_pixels(outside.data(), 81);
    CHECK(current_reads() == before + 1);
    seed(taintedDst, 305, 90);
    copy_surface(cpu, taintedDst);
    gfx_present(taintedDst);
    before = current_reads();
    auto inherited = read(taintedDst, 305);
    expect_pixels(inherited.data(), 52);
    CHECK(current_reads() == before + 1);
    // GPU-only scratch copies retain actor history and can still bypass drains.
    GfxTex *source = fresh(95), *destination = fresh(96);
    seed(destination, 306, 96);
    block_worker();
    copy_surface(source, destination);
    must_return_while_worker_blocked([&] {
        auto old = read(destination, 306);
        expect_pixels(old.data(), 96);
    });
    gfx_present(destination);
    auto copied = read(destination, 306);
    expect_pixels(copied.data(), 95);
}
static void test_lifetime_and_pending()
{
    GfxTex* t = fresh(100);
    seed(t, 401, 100);
    allowPoll = false;
    fill_gpu(t, 101);
    auto old = read(t, 401);
    expect_pixels(old.data(), 100);
    gfx_finish();
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        CHECK(!pending.empty());
    }
    gfx_tex_destroy(t);
    gfx_finish();
    GfxTex* replacement = fresh(102);
    CHECK(replacement == t);
    // A pending old-lifetime completion must not seed this recycled pointer.
    gfx_present(replacement);
    unsigned before = exactCalls.load();
    auto first = read(replacement, 401);
    expect_pixels(first.data(), 102);
    CHECK(exactCalls == before + 1);
    allowPoll = true;
    gfx_finish();
    gfx_present(replacement);
    auto after = read(replacement, 401);
    expect_pixels(after.data(), 102);
    CHECK(legacyReads == 0);
}
static void test_outside_whitelist()
{
    struct Case
    {
        int type;
        uint32_t format, width, levels, face, mip;
    };
    const Case cases[]{{GFX_TEX_2D, 21, 8, 1, 0, 0},
                       {GFX_TEX_2D, 22, 16, 1, 0, 0},
                       {GFX_TEX_CUBE, 21, 16, 1, 1, 0},
                       {GFX_TEX_2D, 21, 16, 2, 0, 1}};
    for (const auto& c : cases)
    {
        GfxTex* t = gfx_tex_create(c.type, c.format, c.width, 16, c.levels, GFX_USE_RT);
        CHECK(t);
        const unsigned issueBefore = issues.load(), before = current_reads();
        for (unsigned repeat = 0; repeat < 3; ++repeat)
        {
            uint8_t value = uint8_t(130 + repeat);
            fill_gpu(t, value);
            Pixels out{};
            gfx_tex_read_async_keyed(t, c.face, c.mip, out.data(), 64, 700, 16);
            for (unsigned y = 0; y < 16; ++y)
                for (unsigned x = 0; x < c.width * 4; ++x)
                    CHECK(out[y * 64 + x] == value);
            gfx_present(t);
        }
        CHECK(current_reads() == before + 3 && issues == issueBefore && legacyReads == 0);
        gfx_tex_destroy(t);
        gfx_finish();
    }
}
static void test_shared_scratch_reordering()
{
    GfxTex* t = fresh(110);
    seed(t, 501, 110);
    fill_gpu(t, 120);
    seed(t, 502, 120);
    allowPoll = false;
    fill_gpu(t, 111);
    auto a = read(t, 501);
    expect_pixels(a.data(), 110);
    fill_gpu(t, 121);
    auto b = read(t, 502);
    expect_pixels(b.data(), 120);
    gfx_finish();
    gfx_present(t);
    // A later producer frame samples the same actors in reverse order. No GPU
    // completion has been published yet, so both reads keep their own history.
    fill_gpu(t, 122);
    auto b2 = read(t, 502);
    expect_pixels(b2.data(), 120);
    fill_gpu(t, 112);
    auto a2 = read(t, 501);
    expect_pixels(a2.data(), 110);
    gfx_finish();
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        CHECK(pending.size() == 4);
        std::reverse(pending.begin(), pending.end());
    }
    allowPoll = true;
    gfx_finish();
    gfx_present(t);
    // Reversed callbacks must not relabel one actor's bytes or replace a newer
    // sample with its older late completion.
    auto newestA = read(t, 501), newestB = read(t, 502);
    expect_pixels(newestA.data(), 112);
    expect_pixels(newestB.data(), 122);
    gfx_finish();
}
static void cleanup_mailbox()
{
    gfx_finish();
    for (auto& t : textures)
        if (t.live)
            gfx_tex_destroy(&t);
    gfx_finish();
    gfx_worker_shutdown();
    CHECK(shutdownCalls == 1);
    gfx_worker_shutdown();
    CHECK(shutdownCalls == 1);
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        CHECK(pending.empty());
    }
}
static void test_failure(const char* mode)
{
    GfxTex* t = fresh(150);
    seed(t, 601, 150);
    allowPoll = false;
    fill_gpu(t, 151);
    auto historical = read(t, 601);
    expect_pixels(historical.data(), 150);
    gfx_finish();
    gfx_present(t);
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        CHECK(!pending.empty());
    }
    fill_gpu(t, 152);
    if (std::strcmp(mode, "mailbox-issue-failed") == 0)
    {
        issueStatus = IssueStatus::Failed;
        auto beforeFailure = read(t, 601);
        expect_pixels(beforeFailure.data(), 150);
        gfx_finish();
    }
    else if (std::strcmp(mode, "mailbox-poll-failed") == 0)
    {
        pollFailure = true;
        gfx_finish();
    }
    else
    {
        CHECK(std::strcmp(mode, "mailbox-exact-failed") == 0);
        exactSuccess = false;
        Pixels untouched;
        untouched.fill(0xcd);
        gfx_tex_read_async_keyed(t, 0, 0, untouched.data(), 64, 602, 16);
        expect_pixels(untouched.data(), 0xcd); // failed read never invents historical success
    }
    CHECK(gfx_failures() == 1);
    allowPoll = true;
    pollFailure = false;
    exactSuccess = true;
    issueStatus = IssueStatus::Queued;
    const unsigned before = current_reads(), issueBefore = issues.load();
    auto current = read(t, 601);
    expect_pixels(current.data(), 152);
    gfx_present(t);
    fill_gpu(t, 153);
    auto later = read(t, 601);
    expect_pixels(later.data(), 153);
    gfx_finish();
    CHECK(current_reads() == before + 2 && issues == issueBefore && legacyReads == 0);
    cleanup_mailbox();
}
static void test_constant_fx(const char* mode, bool worker)
{
    const bool fast = std::strcmp(mode, "const-fx-on") == 0;
    const bool queryExpected = fast || std::strcmp(mode, "const-fx-unsupported") == 0;
    CHECK(constantFxQueries == (queryExpected ? 1u : 0u));
    CHECK(gfx_init(nullptr, 0) == 1); // repeated init never re-queries immutable capability
    CHECK(constantFxQueries == (queryExpected ? 1u : 0u));
    if (worker)
        block_worker();
    gfx_fx_set("fog", 29.5f);
    if (fast)
    {
        const unsigned calls = fxGets.load();
        must_return_while_worker_blocked([&] {
            CHECK(gfx_fx_get("fog") == 17.25f);
            CHECK(gfx_fx_get("unrecognized") == 17.25f);
            CHECK(gfx_fx_get(nullptr) == 17.25f);
        });
        CHECK(fxGets == calls);
    }
    else if (worker)
    {
        must_wait_for_worker([&] { CHECK(gfx_fx_get("fog") == (constantFxSupported ? 17.25f : 29.5f)); });
        CHECK(fxGets == 1);
    }
    else
    {
        CHECK(gfx_fx_get("fog") == 17.25f && fxGets == 1);
    }
    gfx_finish();
    CHECK(gfx_fx_get("fog") == (constantFxSupported ? 17.25f : 29.5f));
    CHECK(constantFxQueries == (queryExpected ? 1u : 0u)); // no hot-path capability queries
    gfx_worker_shutdown();
    gfx_worker_shutdown();
    const unsigned callsAfterShutdown = fxGets.load();
    CHECK(gfx_fx_get("fog") == (constantFxSupported ? 17.25f : 29.5f));
    CHECK(fxGets == callsAfterShutdown + 1 && constantFxQueries == (queryExpected ? 1u : 0u));
    CHECK(shutdownCalls == 0 && issues == 0 && legacyReads == 0 && exactCalls == 0);
}
int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const bool normalMailbox = std::strcmp(argv[1], "mailbox") == 0;
    const bool failureMode = std::strcmp(argv[1], "mailbox-issue-failed") == 0 ||
                             std::strcmp(argv[1], "mailbox-poll-failed") == 0 ||
                             std::strcmp(argv[1], "mailbox-exact-failed") == 0;
    const bool mailbox = normalMailbox || failureMode;
    const bool constFxMode = std::strcmp(argv[1], "const-fx-on") == 0 || std::strcmp(argv[1], "const-fx-off") == 0 ||
                             std::strcmp(argv[1], "const-fx-unsupported") == 0 ||
                             std::strcmp(argv[1], "const-fx-worker-off") == 0;
    const bool worker = mailbox || std::strcmp(argv[1], "worker-only") == 0 ||
                        (constFxMode && std::strcmp(argv[1], "const-fx-worker-off") != 0);
    CHECK(mailbox || worker || constFxMode || std::strcmp(argv[1], "off") == 0);
    constantFxSupported = constFxMode && std::strcmp(argv[1], "const-fx-unsupported") != 0;
    legacySentinel = mailbox;
    producer = std::this_thread::get_id();
    setenv("FFXI_RENDER_WORKER", worker ? "1" : "0", 1);
    // OFF also exercises the required-worker guard despite mailbox request.
    setenv("FFXI_RENDER_WORKER_MAILBOX", mailbox || (!worker && !constFxMode) ? "1" : "0", 1);
    setenv("FFXI_RENDER_WORKER_CONST_FX", constFxMode && std::strcmp(argv[1], "const-fx-off") != 0 ? "1" : "0", 1);
    setenv("FFXI_RENDER_WORKER_DIAGNOSTICS", "0", 1);
    CHECK(gfx_init(nullptr, 0) == 1);
    if (constFxMode)
        test_constant_fx(argv[1], worker);
    else if (!mailbox)
        test_original_paths(worker);
    else if (failureMode)
        test_failure(argv[1]);
    else
    {
        test_warm_cold_and_present();
        test_duplicate_age_and_key();
        test_permanent_poison();
        test_lifetime_and_pending();
        test_outside_whitelist();
        test_shared_scratch_reordering();
        gfx_finish();
        CHECK(exactCalls > 0 && issues > 0 && polls > 0 && legacyReads == 0);
        cleanup_mailbox();
    }
    std::printf(
        "worker mailbox %s: %llu checks, exact %u plain %u legacy %u issues %u polls %u present %u fxqueries %u fxgets %u; zero failures; no GPU/FPS claim\n",
        argv[1], (unsigned long long)checks.load(), exactCalls.load(), plainReads.load(), legacyReads.load(),
        issues.load(), polls.load(), presents.load(), constantFxQueries.load(), fxGets.load());
}
