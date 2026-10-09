/* CPU-only checks of the probe mailbox (gfx_probe_mailbox.h); no renderer.
 * clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
 *   -pthread tests/gfx_probe_mailbox_test.cpp -o /tmp/gfx-probe-mailbox-asan
 * clang++ -std=c++17 -O1 -g -fsanitize=thread -pthread \
 *   tests/gfx_probe_mailbox_test.cpp -o /tmp/gfx-probe-mailbox-tsan
 */
#include "../runtime/portable/gfx_probe_mailbox.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <thread>

using namespace gfxprobe;
static std::atomic<uint64_t> checks{0};
static uint64_t stressHits = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                                  \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)
static Pixels pattern(uint64_t tag)
{
    Pixels pixels;
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = uint8_t((tag >> ((i % 8) * 8)) ^ (i * 137 + i / 7));
    return pixels;
}
static Read first(Mailbox& box, Surface surface, uint64_t frame, uint64_t key = 11)
{
    CHECK(box.begin_frame(frame));
    return box.read(surface, key, 16);
}
static void exact(const Read& read, ExactReason why)
{
    CHECK(!read.cached);
    CHECK(read.reason == why);
    CHECK(read.sourceFrame == 0 && read.sourceSequence == 0);
}
static void cached(const Read& read, uint64_t frame, uint64_t sequence)
{
    CHECK(read.cached && read.reason == ExactReason::None);
    CHECK(read.sourceFrame == frame && read.sourceSequence == sequence);
    CHECK(read.pixels == pattern(sequence));
}

static void basic_policy()
{
    Mailbox box;
    Surface surface = box.create_surface(2, 3);
    CHECK(surface && surface.face == 2 && surface.mip == 3);
    exact(box.read(surface, 11, 16), ExactReason::NoFrame);
    CHECK(!box.begin_frame(0));
    Read one = first(box, surface, 1);
    exact(one, ExactReason::Missing);
    CHECK(one.refresh && one.refresh.surface == surface && one.refresh.issuedFrame == 1);
    Pixels source = pattern(one.refresh.sequence);
    CHECK(box.publish(one.refresh, source) == Publication::Published);
    source.fill(0); // publication owns bytes, not the caller's storage
    exact(box.read(surface, 11, 16), ExactReason::Duplicate);
    CHECK(!box.begin_frame(1) && !box.begin_frame(0));
    Read two = first(box, surface, 2);
    cached(two, 1, one.refresh.sequence);
    Pixels immutable = two.pixels;
    CHECK(box.publish(two.refresh, pattern(two.refresh.sequence)) == Publication::Published);
    CHECK(two.pixels == immutable); // result also owns an untorn snapshot
    CHECK(box.publish(two.refresh, pattern(999)) == Publication::Rejected);
    CHECK(!box.cancel(two.refresh));
    Read three = first(box, surface, 3);
    cached(three, 2, two.refresh.sequence);
    CHECK(box.cancel(three.refresh));
    CHECK(box.publish(three.refresh, pattern(three.refresh.sequence)) == Publication::Rejected);
    Read stale = first(box, surface, 19);
    exact(stale, ExactReason::Stale); // source frame 2 is 17 frames old
    CHECK(stale.refresh && stale.refresh.issuedFrame == 19);
    CHECK(box.cancel(stale.refresh));
    CHECK(box.begin_frame(20));
    exact(box.read(surface, 0, 16), ExactReason::UnknownKey);
    exact(box.read(surface, 11, 0), ExactReason::InvalidAge);
    exact(box.read(surface, 11, 16), ExactReason::Duplicate);
    CHECK(box.begin_frame(21));
    exact(box.read(surface, 11, 65), ExactReason::InvalidAge);
    exact(box.read(surface, 11, 16), ExactReason::Duplicate);
    CHECK(box.begin_frame(22));
    // Age is actual issue frame: repeatedly returning old bytes cannot refresh it.
    Read stillStale = box.read(surface, 11, 16);
    exact(stillStale, ExactReason::Stale);
    CHECK(box.cancel(stillStale.refresh));
}

static void identity_and_order()
{
    Mailbox box(4, 8);
    Surface a = box.create_surface(0, 0), b = box.create_surface(1, 0), c = box.create_surface(0, 1);
    CHECK(a && b && c && a.lifetime != b.lifetime && b.lifetime != c.lifetime);
    CHECK(box.begin_frame(1));
    Read ar = box.read(a, 22, 16), br = box.read(b, 22, 16), cr = box.read(c, 22, 16);
    CHECK(ar.refresh && br.refresh && cr.refresh);
    Request forged = ar.refresh;
    forged.surface.face = 1;
    CHECK(box.publish(forged, pattern(forged.sequence)) == Publication::Rejected);
    forged = ar.refresh;
    ++forged.surface.mip;
    CHECK(box.publish(forged, pattern(forged.sequence)) == Publication::Rejected);
    forged = ar.refresh;
    ++forged.key;
    CHECK(box.publish(forged, pattern(forged.sequence)) == Publication::Rejected);
    forged = ar.refresh;
    ++forged.cpuEpoch;
    CHECK(box.publish(forged, pattern(forged.sequence)) == Publication::Rejected);
    forged = ar.refresh;
    ++forged.issuedFrame;
    CHECK(box.publish(forged, pattern(forged.sequence)) == Publication::Rejected);
    forged = ar.refresh;
    ++forged.sequence;
    CHECK(box.publish(forged, pattern(forged.sequence)) == Publication::Rejected);
    CHECK(box.publish(br.refresh, pattern(br.refresh.sequence)) == Publication::Published);
    CHECK(box.publish(cr.refresh, pattern(cr.refresh.sequence)) == Publication::Published);
    CHECK(box.begin_frame(2));
    Read a2 = box.read(a, 22, 16);
    exact(a2, ExactReason::Missing);
    CHECK(box.publish(a2.refresh, pattern(a2.refresh.sequence)) == Publication::Published);
    CHECK(box.publish(ar.refresh, pattern(ar.refresh.sequence)) == Publication::Superseded);
    Read b2 = box.read(b, 22, 16), c2 = box.read(c, 22, 16);
    cached(b2, 1, br.refresh.sequence);
    cached(c2, 1, cr.refresh.sequence);
    CHECK(box.cancel(b2.refresh) && box.cancel(c2.refresh));
    Read a3 = first(box, a, 3, 22);
    cached(a3, 2, a2.refresh.sequence);
    CHECK(box.cancel(a3.refresh));
    CHECK(box.destroy_surface(a));
    CHECK(!box.destroy_surface(a));
    exact(box.read(a, 22, 16), ExactReason::InvalidSurface);
    Surface replacement = box.create_surface(0, 0);
    CHECK(replacement && replacement.lifetime != a.lifetime);
    exact(box.read(a, 22, 16), ExactReason::InvalidSurface);
    Read newRead = box.read(replacement, 22, 16);
    exact(newRead, ExactReason::Missing);
    CHECK(box.publish(ar.refresh, pattern(ar.refresh.sequence)) == Publication::Rejected);
    CHECK(box.cancel(newRead.refresh));
}

static void mutation_policy()
{
    Mailbox box;
    Surface surface = box.create_surface();
    Read seed = first(box, surface, 1);
    CHECK(box.publish(seed.refresh, pattern(seed.refresh.sequence)) == Publication::Published);
    CHECK(box.begin_frame(2));
    CHECK(box.gpu_write(surface)); // GPU scratch overwrite retains per-key history
    Read history = box.read(surface, 11, 16);
    cached(history, 1, seed.refresh.sequence);
    CHECK(box.cpu_write(surface, CpuWrite::Whole));
    CHECK(box.publish(history.refresh, pattern(history.refresh.sequence)) == Publication::Rejected);
    CHECK(box.gpu_write(surface));
    exact(box.read(surface, 11, 16), ExactReason::Duplicate); // invalidation preserves marker
    Read after = first(box, surface, 3);
    exact(after, ExactReason::Missing);
    CHECK(after.refresh.cpuEpoch != history.refresh.cpuEpoch);
    CHECK(box.publish(after.refresh, pattern(after.refresh.sequence)) == Publication::Published);
    CHECK(box.cpu_write(surface, CpuWrite::Whole));
    CHECK(box.begin_frame(4));
    Read cpu = box.read(surface, 11, 16);
    exact(cpu, ExactReason::CpuCurrent);
    CHECK(box.publish(cpu.refresh, pattern(cpu.refresh.sequence)) == Publication::Published);
    CHECK(box.gpu_write(surface));
    exact(box.read(surface, 11, 16), ExactReason::Duplicate);
    Read later = first(box, surface, 5);
    cached(later, 4, cpu.refresh.sequence);
    CHECK(box.cpu_write(surface, CpuWrite::Partial));
    CHECK(box.publish(later.refresh, pattern(later.refresh.sequence)) == Publication::Rejected);
    CHECK(box.gpu_write(surface)); // unknown CPU-derived pixels stay exact
    CHECK(box.begin_frame(6));
    Read partial = box.read(surface, 11, 16);
    exact(partial, ExactReason::UnresolvedCpuMutation);
    CHECK(box.publish(partial.refresh, pattern(partial.refresh.sequence)) == Publication::Published);
    Read resolved = first(box, surface, 7);
    cached(resolved, 6, partial.refresh.sequence);
    CHECK(box.cancel(resolved.refresh));
    CHECK(box.cpu_write(surface, CpuWrite::Whole));
    CHECK(box.cpu_write(surface, CpuWrite::Partial)); // full CPU shadow is still current
    CHECK(box.begin_frame(8));
    Read stillCpu = box.read(surface, 11, 16);
    exact(stillCpu, ExactReason::CpuCurrent);
    CHECK(box.cancel(stillCpu.refresh));
}

static void bounded_storage()
{
    Mailbox box(1, 2);
    const size_t bytes = box.capacity_bytes();
    Surface a = box.create_surface();
    CHECK(a && !box.create_surface());
    CHECK(box.begin_frame(1));
    Read ar = box.read(a, 1, 1), br = box.read(a, 2, 1);
    CHECK(ar.refresh && br.refresh);
    exact(box.read(a, 3, 1), ExactReason::KeyCapacity);
    CHECK(box.live_keys() == 2 && box.pending_requests() == 2);
    CHECK(box.begin_frame(100)); // unfinished copies cannot be evicted to admit another key
    exact(box.read(a, 3, 1), ExactReason::KeyCapacity);
    CHECK(box.cancel(ar.refresh));
    Read replacement = box.read(a, 3, 1);
    exact(replacement, ExactReason::Missing);
    CHECK(replacement.refresh && box.live_keys() == 2);
    CHECK(box.publish(ar.refresh, pattern(ar.refresh.sequence)) == Publication::Rejected);
    CHECK(box.cancel(br.refresh) && box.cancel(replacement.refresh));
    CHECK(box.capacity_bytes() == bytes);
    CHECK(box.destroy_surface(a));
    Surface b = box.create_surface();
    CHECK(b && b.lifetime != a.lifetime);
    std::array<Request, COPIES_PER_KEY> pending;
    for (size_t i = 0; i < COPIES_PER_KEY; ++i)
    {
        Read r = first(box, b, 101 + i, 1);
        CHECK(r.refresh);
        pending[i] = r.refresh;
    }
    Read full = first(box, b, 105, 1);
    exact(full, ExactReason::RequestCapacity);
    CHECK(!full.refresh && box.pending_requests() == COPIES_PER_KEY);
    CHECK(box.publish(pending[1], pattern(pending[1].sequence)) == Publication::Published);
    Read hasOld = first(box, b, 106, 1);
    cached(hasOld, 102, pending[1].sequence);
    CHECK(hasOld.refresh && box.pending_requests() == COPIES_PER_KEY);
    Read busyButCached = first(box, b, 107, 1);
    cached(busyButCached, 102, pending[1].sequence);
    CHECK(!busyButCached.refresh);
    CHECK(box.capacity_bytes() == bytes);
    box.stop();
    CHECK(box.live_keys() == 0 && box.pending_requests() == 0);
    CHECK(!box.begin_frame(108) && !box.create_surface());
    CHECK(!box.cpu_write(b, CpuWrite::Whole) && !box.gpu_write(b));
    exact(box.read(b, 1, 16), ExactReason::InvalidSurface);
    CHECK(box.publish(pending[0], pattern(pending[0].sequence)) == Publication::Rejected);
    CHECK(!box.cancel(hasOld.refresh));
}

// The opposite sides execute on distinct threads; condition variables choose
// both legal orderings without sleeps or depending on scheduler timing.
static void ordered_threads(const std::function<void()>& before, const std::function<void()>& after)
{
    std::mutex mutex;
    std::condition_variable ready;
    bool done = false;
    std::thread second([&] {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return done; });
        lock.unlock();
        after();
    });
    std::thread firstThread([&] {
        before();
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        ready.notify_one();
    });
    firstThread.join();
    second.join();
}
static void adversarial_orderings()
{
    for (bool completionFirst : {false, true})
    {
        Mailbox box;
        Surface s = box.create_surface();
        Read one = first(box, s, 1);
        CHECK(box.begin_frame(2));
        Read observed;
        auto complete = [&] {
            CHECK(box.publish(one.refresh, pattern(one.refresh.sequence)) == Publication::Published);
        };
        auto consume = [&] { observed = box.read(s, 11, 16); };
        if (completionFirst)
            ordered_threads(complete, consume);
        else
            ordered_threads(consume, complete);
        if (completionFirst)
            cached(observed, 1, one.refresh.sequence);
        else
            exact(observed, ExactReason::Missing);
        CHECK(box.cancel(observed.refresh));
        exact(box.read(s, 11, 16), ExactReason::Duplicate);
    }
    for (bool mutationFirst : {false, true})
    {
        Mailbox box;
        Surface s = box.create_surface();
        Read one = first(box, s, 1);
        Publication outcome = Publication::Rejected;
        auto mutate = [&] { CHECK(box.cpu_write(s, CpuWrite::Whole)); };
        auto complete = [&] { outcome = box.publish(one.refresh, pattern(one.refresh.sequence)); };
        if (mutationFirst)
            ordered_threads(mutate, complete);
        else
            ordered_threads(complete, mutate);
        CHECK(outcome == (mutationFirst ? Publication::Rejected : Publication::Published));
        CHECK(box.gpu_write(s));
        Read after = first(box, s, 2);
        exact(after, ExactReason::Missing);
        CHECK(box.cancel(after.refresh));
    }
    for (bool destroyFirst : {false, true})
    {
        Mailbox box;
        Surface s = box.create_surface();
        Read one = first(box, s, 1);
        Publication outcome = Publication::Rejected;
        auto destroy = [&] { CHECK(box.destroy_surface(s)); };
        auto complete = [&] { outcome = box.publish(one.refresh, pattern(one.refresh.sequence)); };
        if (destroyFirst)
            ordered_threads(destroy, complete);
        else
            ordered_threads(complete, destroy);
        CHECK(outcome == (destroyFirst ? Publication::Rejected : Publication::Published));
        Surface newer = box.create_surface();
        CHECK(newer && newer.lifetime != s.lifetime);
        Read after = first(box, newer, 2);
        exact(after, ExactReason::Missing);
        CHECK(box.cancel(after.refresh));
    }
    for (bool cancelFirst : {false, true})
    {
        Mailbox box;
        Surface s = box.create_surface();
        Read one = first(box, s, 1);
        bool canceled = false;
        Publication outcome = Publication::Rejected;
        auto cancel = [&] { canceled = box.cancel(one.refresh); };
        auto complete = [&] { outcome = box.publish(one.refresh, pattern(one.refresh.sequence)); };
        if (cancelFirst)
            ordered_threads(cancel, complete);
        else
            ordered_threads(complete, cancel);
        CHECK(canceled == cancelFirst);
        CHECK(outcome == (cancelFirst ? Publication::Rejected : Publication::Published));
        CHECK(box.pending_requests() == 0);
    }
    for (bool stopFirst : {false, true})
    {
        Mailbox box;
        Surface s = box.create_surface();
        Read one = first(box, s, 1);
        Publication outcome = Publication::Rejected;
        auto stop = [&] { box.stop(); };
        auto complete = [&] { outcome = box.publish(one.refresh, pattern(one.refresh.sequence)); };
        if (stopFirst)
            ordered_threads(stop, complete);
        else
            ordered_threads(complete, stop);
        CHECK(outcome == (stopFirst ? Publication::Rejected : Publication::Published));
        CHECK(!box.begin_frame(2) && !box.create_surface());
        exact(box.read(s, 11, 16), ExactReason::InvalidSurface);
    }
}

static void duplicate_race()
{
    Mailbox box;
    Surface s = box.create_surface();
    Read one = first(box, s, 1);
    CHECK(box.publish(one.refresh, pattern(one.refresh.sequence)) == Publication::Published);
    CHECK(box.begin_frame(2));
    std::array<Read, 8> results;
    std::array<std::thread, 8> threads;
    for (size_t i = 0; i < threads.size(); ++i)
        threads[i] = std::thread([&, i] { results[i] = box.read(s, 11, 16); });
    for (auto& thread : threads)
        thread.join();
    unsigned cacheHits = 0, duplicates = 0;
    for (const auto& result : results)
    {
        if (result.cached)
        {
            ++cacheHits;
            cached(result, 1, one.refresh.sequence);
            CHECK(box.cancel(result.refresh));
        }
        else
        {
            ++duplicates;
            exact(result, ExactReason::Duplicate);
        }
    }
    CHECK(cacheHits == 1 && duplicates == 7);
}

static void concurrent_stress()
{
    Mailbox box(1, 16);
    Surface surface = box.create_surface();
    const size_t memory = box.capacity_bytes();
    std::mutex mutex;
    std::condition_variable work, room;
    std::deque<Request> requests;
    bool stopped = false;
    std::array<std::thread, 4> consumers;
    std::atomic<uint64_t> completed{0};
    for (size_t index = 0; index < consumers.size(); ++index)
    {
        consumers[index] = std::thread([&, index] {
            for (;;)
            {
                Request request;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    work.wait(lock, [&] { return stopped || !requests.empty(); });
                    if (requests.empty())
                        return;
                    // Alternating deque ends introduces out-of-order delivery.
                    if (index & 1)
                    {
                        request = requests.back();
                        requests.pop_back();
                    }
                    else
                    {
                        request = requests.front();
                        requests.pop_front();
                    }
                }
                room.notify_one();
                if (request.sequence & 1)
                    std::this_thread::yield();
                Pixels pixels = pattern(request.sequence);
                Publication status = box.publish(request, pixels);
                CHECK(status == Publication::Published || status == Publication::Superseded ||
                      status == Publication::Rejected);
                pixels.fill(0);
                ++completed;
            }
        });
    }
    uint64_t lastSequence = 0, minimumSequence = 1, issued = 0;
    for (uint64_t frame = 1; frame <= 20000; ++frame)
    {
        CHECK(box.begin_frame(frame));
        if (frame % 211 == 0)
        {
            CHECK(box.destroy_surface(surface));
            Surface next = box.create_surface();
            CHECK(next && next.lifetime != surface.lifetime);
            surface = next;
            minimumSequence = lastSequence + 1;
        }
        else if (frame % 17 == 0)
        {
            CHECK(box.cpu_write(surface, (frame & 1) ? CpuWrite::Whole : CpuWrite::Partial));
            minimumSequence = lastSequence + 1;
        }
        CHECK(box.gpu_write(surface));
        uint64_t key = 1 + frame % 8;
        Read read = box.read(surface, key, 16);
        if (read.cached)
        {
            ++stressHits;
            CHECK(read.sourceFrame < frame && frame - read.sourceFrame <= 16);
            CHECK(read.sourceSequence >= minimumSequence);
            CHECK(read.pixels == pattern(read.sourceSequence));
        }
        if (read.refresh)
        {
            lastSequence = read.refresh.sequence;
            ++issued;
            {
                std::unique_lock<std::mutex> lock(mutex);
                room.wait(lock, [&] { return requests.size() < 64; });
                requests.push_back(read.refresh);
            }
            work.notify_one();
        }
        Read duplicate = box.read(surface, key, 16);
        CHECK(!duplicate.cached && !duplicate.refresh);
        CHECK(duplicate.reason == ExactReason::Duplicate || duplicate.reason == ExactReason::KeyCapacity);
        CHECK(box.live_keys() <= 16 && box.pending_requests() <= 16 * COPIES_PER_KEY);
        CHECK(box.capacity_bytes() == memory);
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopped = true;
    }
    work.notify_all();
    for (auto& thread : consumers)
        thread.join();
    CHECK(completed == issued && box.pending_requests() == 0);
    CHECK(stressHits > 0);
    box.stop();
    CHECK(box.live_keys() == 0);
}

int main()
{
    basic_policy();
    identity_and_order();
    mutation_policy();
    bounded_storage();
    adversarial_orderings();
    duplicate_race();
    concurrent_stress();
    std::printf(
        "probe mailbox: 7 suites, %llu checks, 20000 threaded producer frames, %llu concurrent cache hits, zero failures; no GPU/FPS claim\n",
        static_cast<unsigned long long>(checks.load()), static_cast<unsigned long long>(stressHits));
}
