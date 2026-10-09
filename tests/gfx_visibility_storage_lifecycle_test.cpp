/* Actual production version pool with an independent byte/ownership oracle.
 * No Vulkan, generic renderer hooks, game fidelity or FPS is tested here.
 * clang++ -std=c++17 -O1 -g -fsanitize=address,undefined \
 *   -fno-omit-frame-pointer tests/gfx_visibility_storage_lifecycle_test.cpp \
 *   -o /tmp/xi-visibility-storage-lifecycle
 * The production route is single-threaded; this is not a concurrency fixture.
 */
#define GFX_VISIBILITY_STORAGE_MODEL_ONLY
#include "../runtime/portable/gfx_visibility_storage.inc"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace
{
uint64_t checks = 0;
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
using Bytes = std::array<uint8_t, 1024>;
struct Payload
{
    size_t address = SIZE_MAX;
};
template <size_t N> using Pool = gfxvisibility::Pool<Payload, N>;
using Ref = Pool<1>::Ref;
static_assert(std::is_const<typename Ref::element_type>::value, "Versions are immutable through public refs");
static_assert(!std::is_copy_constructible<Pool<1>>::value, "A pool cannot share mutable slot ownership by copy");
static_assert(std::is_same<decltype(std::declval<Ref>()->payload()), const Payload&>::value, "Payload access is const");
Bytes pattern(uint64_t tag)
{
    Bytes b{};
    for (size_t i = 0; i < b.size(); ++i)
        b[i] = uint8_t((tag >> (8 * (i % 8))) ^ (i * 137 + i / 7));
    return b;
}
struct FakeGpu
{
    std::vector<Bytes> slots;
    size_t zeroCalls = 0;
    explicit FakeGpu(size_t capacity) : slots(capacity)
    {
        for (auto& s : slots)
            s.fill(0x77);
    }
    bool zero(size_t index, Payload& p)
    {
        CHECK(index < slots.size());
        ++zeroCalls;
        p.address = index;
        slots[index].fill(0);
        return true;
    }
    template <class R> void write(const R& ref, uint64_t tag)
    {
        CHECK(ref && ref->payload().address == ref->slotIndex());
        slots.at(ref->payload().address) = pattern(tag);
    }
    template <class R> const Bytes& bytes(const R& ref) const
    {
        CHECK(ref && ref->payload().address == ref->slotIndex());
        return slots.at(ref->payload().address);
    }
};
template <size_t N> size_t prepare(Pool<N>& p, FakeGpu& gpu, uint64_t completed, uint64_t serial)
{
    return p.prepare(completed, serial, [&](size_t i, Payload& payload) { return gpu.zero(i, payload); });
}

void initial_and_invalid_parameters()
{
    Pool<2> p(17);
    FakeGpu gpu(2);
    CHECK(p.capacity() == 2);
    CHECK(!p.acquire(0, 1));
    CHECK(p.idle(0));
    CHECK(prepare(p, gpu, 0, 0) == 0);
    CHECK(prepare(p, gpu, 4, 4) == 0);
    CHECK(prepare(p, gpu, 4, 3) == 0);
    CHECK(gpu.zeroCalls == 0);
    CHECK(prepare(p, gpu, 0, 10) == 2);
    CHECK(!p.idle(9));
    CHECK(p.idle(10));
    CHECK(!p.acquire(UINT64_MAX, 10));
    CHECK(!p.acquire(1, 0));
    CHECK(!p.acquire(1, 9));
    auto a = p.acquire(0, 10);
    CHECK(a);
    CHECK(a->slotIndex() == 0);
    CHECK(a->slotGeneration() == 1);
    CHECK(a->deviceEpoch() == 17 && a->producerFrame() == 0 && a->issueSerial() == 10 && a->lastUseSerial() == 10);
    CHECK(gpu.bytes(a) == Bytes{});
    CHECK(p.owns(a));
    CHECK(!p.idle(10));
    CHECK(!p.touch(a, 0));
    CHECK(!p.touch(a, 9));
    CHECK(p.touch(a, 10));
    CHECK(p.touch(a, 12));
    CHECK(p.touch(a, 11));
    CHECK(a->lastUseSerial() == 12); // A later caller cannot lower retirement serial.
    auto b = p.acquire(1, 10);
    CHECK(b && b->slotIndex() == 1);
    CHECK(!p.acquire(2, 10));
    CHECK(prepare(p, gpu, 100, 101) == 0);
    a.reset();
    b.reset();
    CHECK(prepare(p, gpu, 10, 11) == 1);
    CHECK(prepare(p, gpu, 11, 12) == 0);
    CHECK(prepare(p, gpu, 12, 13) == 1);
    Pool<0> empty(1);
    CHECK(!empty.acquire(1, 1));
    CHECK(empty.idle(0));
    CHECK(empty.prepare(0, 1, [](size_t, Payload&) {
        std::abort();
        return false;
    }) == 0);
    Pool<1> disabled(0);
    FakeGpu zeroEpoch(1);
    CHECK(prepare(disabled, zeroEpoch, 0, 1) == 1);
    CHECK(!disabled.acquire(0, 1));
}

void aliases_and_materialization_ownership()
{
    Pool<4> p(1);
    FakeGpu gpu(4);
    CHECK(prepare(p, gpu, 0, 1) == 4);
    auto source = p.acquire(20, 1);
    CHECK(source);
    gpu.write(source, 1001);
    auto destinationA = source, destinationB = source, historyA = source;
    const auto oldSlot = source->slotIndex();
    const auto oldGeneration = source->slotGeneration();
    source = p.acquire(20, 1);
    CHECK(source);
    gpu.write(source, 2002);
    auto destinationC = source;
    source = p.acquire(21, 2);
    CHECK(source);
    gpu.write(source, 3003);
    CHECK(gpu.bytes(destinationA) == pattern(1001));
    CHECK(gpu.bytes(destinationB) == pattern(1001));
    CHECK(gpu.bytes(destinationC) == pattern(2002));
    CHECK(gpu.bytes(source) == pattern(3003));
    CHECK(destinationA->slotIndex() == oldSlot && destinationA->slotGeneration() == oldGeneration);
    CHECK(destinationA->producerFrame() == 20 && source->producerFrame() == 21);
    // A renderer materialization callback consumes the exact version, not the source binding.
    // Byte mutation below represents the independent image after the callback; it is not a hook test.
    Bytes materializedA = gpu.bytes(destinationA);
    CHECK(p.touch(destinationA, 9));
    destinationA.reset();
    materializedA[411] ^= 0x42;
    CHECK(gpu.bytes(destinationB) == pattern(1001));
    CHECK(gpu.bytes(historyA) == pattern(1001));
    CHECK(materializedA != gpu.bytes(destinationB));
    source.reset();
    destinationC.reset();
    CHECK(prepare(p, gpu, 2, 3) == 2);
    destinationB.reset();
    CHECK(prepare(p, gpu, 100, 101) == 0); // History still pins the original bytes.
    historyA.reset();
    CHECK(prepare(p, gpu, 8, 9) == 0);
    CHECK(prepare(p, gpu, 9, 10) == 1);
    auto recycled = p.acquire(30, 101);
    CHECK(recycled);
    CHECK(recycled->slotIndex() == oldSlot);
    CHECK(recycled->slotGeneration() == oldGeneration + 1);
    CHECK(gpu.bytes(recycled) == Bytes{});
}

void gpu_lifetime_outlives_all_aliases()
{
    Pool<1> p(3);
    FakeGpu gpu(1);
    CHECK(prepare(p, gpu, 0, 3) == 1);
    auto ref = p.acquire(5, 3);
    CHECK(ref);
    gpu.write(ref, 333);
    auto alias = ref;
    CHECK(p.touch(ref, 8));
    ref.reset();
    CHECK(prepare(p, gpu, 99, 100) == 0);
    CHECK(gpu.bytes(alias) == pattern(333));
    const auto generation = alias->slotGeneration();
    alias.reset();
    CHECK(!p.idle(7));
    CHECK(prepare(p, gpu, 7, 9) == 0);
    CHECK(p.idle(8));
    CHECK(prepare(p, gpu, 8, 9) == 1);
    CHECK(!p.idle(8));
    CHECK(p.idle(9));
    auto next = p.acquire(6, 9);
    CHECK(next && next->slotGeneration() == generation + 1);
    CHECK(gpu.bytes(next) == Bytes{});
}

void failed_or_abandoned_preparation()
{
    Pool<1> p(5);
    FakeGpu gpu(1);
    // Callback may already have emitted GPU work before returning false or throwing.
    CHECK(p.prepare(0, 5, [&](size_t i, Payload& v) {
        gpu.zero(i, v);
        return false;
    }) == 0);
    CHECK(!p.acquire(1, 5));
    CHECK(!p.idle(4));
    CHECK(prepare(p, gpu, 4, 6) == 0);
    bool threw = false;
    try
    {
        p.prepare(5, 7, [&](size_t i, Payload& v) -> bool {
            gpu.zero(i, v);
            throw std::runtime_error("after GPU prezero");
        });
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
    CHECK(!p.acquire(2, 7));
    CHECK(!p.idle(6));
    CHECK(prepare(p, gpu, 6, 8) == 0);
    CHECK(prepare(p, gpu, 7, 8) == 1);
    auto abandoned = p.acquire(3, 8);
    CHECK(abandoned);
    gpu.write(abandoned, 444);
    const auto generation = abandoned->slotGeneration();
    abandoned.reset();
    CHECK(!p.acquire(4, 9));
    CHECK(prepare(p, gpu, 7, 9) == 0);
    CHECK(prepare(p, gpu, 8, 9) == 1);
    auto next = p.acquire(4, 9);
    CHECK(next && next->slotGeneration() == generation + 1 && gpu.bytes(next) == Bytes{});
}

void history_freshness_boundaries()
{
    using namespace gfxvisibility;
    Pool<1> p(1);
    FakeGpu gpu(1);
    CHECK(prepare(p, gpu, 99, 100) == 1);
    auto old = p.acquire(10, 100);
    CHECK(old);
    gpu.write(old, 1010);
    CHECK(!completed(old, 99));
    CHECK(completed(old, 100));
    CHECK(current_producer(old, 10));
    CHECK(!current_producer(old, 11));
    CHECK(!history_eligible(old, 9, 100, 16));
    CHECK(!history_eligible(old, 10, 100, 16));
    CHECK(!history_eligible(old, 11, 99, 16));
    for (uint32_t age = 1; age <= 64; ++age)
    {
        CHECK(history_eligible(old, 10 + age, 100, age));
        CHECK(!history_eligible(old, 11 + age, 100, age));
    }
    CHECK(!history_eligible(old, 11, 100, 0));
    CHECK(!history_eligible(old, 11, 100, 65));
    // A copy in a later frame cannot relabel the production frame or rejuvenate data.
    auto copiedLater = old;
    CHECK(copiedLater->producerFrame() == 10);
    CHECK(!current_producer(copiedLater, 20));
    CHECK(history_eligible(copiedLater, 26, 100, 16));
    CHECK(!history_eligible(copiedLater, 27, 100, 16));
    CHECK(p.touch(copiedLater, 500));
    CHECK(completed(copiedLater, 100));
    CHECK(history_eligible(copiedLater, 26, 100, 16)); // A GPU read extends lifetime, not generation time.
    CHECK(copiedLater->producerFrame() == 10 && copiedLater->issueSerial() == 100);
    old.reset();
    copiedLater.reset();
    CHECK(prepare(p, gpu, 499, 501) == 0);
    CHECK(prepare(p, gpu, 500, 501) == 1);
    auto far = p.acquire(UINT64_MAX - 1, 501);
    CHECK(far);
    CHECK(history_eligible(far, UINT64_MAX, 501, 1));
    CHECK(!history_eligible(far, 0, 501, 64));
    Pool<1>::Ref absent;
    CHECK(!completed(absent, UINT64_MAX));
    CHECK(!current_producer(absent, 0));
    CHECK(!history_eligible(absent, 1, UINT64_MAX, 1));
}

void device_epoch_and_live_owner_reset()
{
    Pool<1>::Ref old;
    FakeGpu gpu(1);
    {
        Pool<1> p(51);
        CHECK(prepare(p, gpu, 0, 1) == 1);
        old = p.acquire(2, 1);
        CHECK(old);
        gpu.write(old, 5151);
        CHECK(!p.idle(UINT64_MAX));
    }
    CHECK(old->deviceEpoch() == 51 && old->producerFrame() == 2);
    CHECK(gpu.bytes(old) == pattern(5151));
    Pool<1> replacement(52);
    FakeGpu replacementGpu(1);
    CHECK(!replacement.owns(old));
    CHECK(!replacement.touch(old, 2));
    CHECK(prepare(replacement, replacementGpu, 0, 1) == 1);
    auto current = replacement.acquire(2, 1);
    CHECK(current && replacement.owns(current));
    CHECK(current->deviceEpoch() != old->deviceEpoch());
    replacementGpu.write(current, 5252);
    CHECK(gpu.bytes(old) == pattern(5151));
    CHECK(replacementGpu.bytes(current) == pattern(5252));
    // Eligibility alone is intentionally not a device-epoch admission. The caller must own-check.
    CHECK(gfxvisibility::history_eligible(old, 3, 1, 16));
    CHECK(!replacement.owns(old));
}

void full_capacity_retirement()
{
    constexpr size_t capacity = 512;
    Pool<capacity> p(9);
    FakeGpu gpu(capacity);
    CHECK(prepare(p, gpu, 0, 1) == capacity);
    std::vector<Pool<capacity>::Ref> refs;
    for (size_t i = 0; i < capacity; ++i)
    {
        auto r = p.acquire(10, 1);
        CHECK(r && r->slotIndex() == i);
        gpu.write(r, 9000 + i);
        refs.push_back(r);
    }
    CHECK(!p.acquire(10, 1));
    CHECK(prepare(p, gpu, 100, 101) == 0);
    for (size_t i = 0; i < capacity; ++i)
        CHECK(gpu.bytes(refs[i]) == pattern(9000 + i));
    for (size_t i = 0; i < capacity; i += 2)
    {
        CHECK(p.touch(refs[i], 4));
        refs[i].reset();
    }
    CHECK(prepare(p, gpu, 3, 5) == 0);
    CHECK(prepare(p, gpu, 4, 5) == capacity / 2);
    for (size_t i = 0; i < capacity; i += 2)
    {
        auto r = p.acquire(11, 5);
        CHECK(r && r->slotIndex() == i && r->slotGeneration() == 2);
        gpu.write(r, 10000 + i);
        refs[i] = r;
    }
    for (size_t i = 0; i < capacity; ++i)
        CHECK(gpu.bytes(refs[i]) == pattern((i % 2 ? 9000 : 10000) + i));
}

uint64_t random64(uint64_t& s)
{
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}
void randomized_alias_oracle()
{
    constexpr size_t capacity = 16, bindingCount = 40;
    Pool<capacity> p(77);
    FakeGpu gpu(capacity);
    struct Binding
    {
        Pool<capacity>::Ref ref;
        uint64_t expected = 0;
    };
    std::array<Binding, bindingCount> bindings{};
    uint64_t rng = 0x2d91a4ba731u, serial = 1, completed = 0, tag = 1;
    for (unsigned step = 0; step < 12000; ++step)
    {
        const uint64_t r = random64(rng);
        const size_t a = (r >> 8) % bindingCount, b = (r >> 20) % bindingCount;
        switch (r % 6)
        {
        case 0: {
            prepare(p, gpu, completed, serial);
            auto ref = p.acquire(step / 12, serial);
            if (ref)
            {
                gpu.write(ref, tag);
                bindings[a] = {ref, tag++};
            }
            break;
        }
        case 1:
            bindings[a] = bindings[b];
            break; // Copy point snapshot, including alias/self-copy.
        case 2:
            bindings[a] = {};
            break; // Mutation/destruction releases only this binding.
        case 3:
            if (bindings[a].ref)
            {
                CHECK(p.touch(bindings[a].ref, serial));
                CHECK(gpu.bytes(bindings[a].ref) == pattern(bindings[a].expected));
            }
            break;
        case 4:
            ++serial;
            if (serial > 3)
                completed = serial - 3;
            break;
        case 5:
            prepare(p, gpu, completed, serial);
            break;
        }
        for (const auto& binding : bindings)
            if (binding.ref)
            {
                CHECK(p.owns(binding.ref));
                CHECK(gpu.bytes(binding.ref) == pattern(binding.expected));
                CHECK(binding.ref->lastUseSerial() >= binding.ref->issueSerial());
            }
    }
    for (auto& binding : bindings)
        binding = {};
    CHECK(p.idle(serial));
}
}
int main()
{
    initial_and_invalid_parameters();
    aliases_and_materialization_ownership();
    gpu_lifetime_outlives_all_aliases();
    failed_or_abandoned_preparation();
    history_freshness_boundaries();
    device_epoch_and_live_owner_reset();
    full_capacity_retirement();
    randomized_alias_oracle();
    std::printf(
        "{\"event\":\"complete\",\"checks\":%llu,\"errors\":0,\"actual_production_helper\":true,\"generic_api_hooks_tested\":false,\"gpu_tested\":false,\"fps_claim\":false}\n",
        static_cast<unsigned long long>(checks));
}
