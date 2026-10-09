#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace gfxworker
{
// Single ordered producer (the API gate can serialize several callers), one
// consumer. Arena bytes remain live until execute returns, including popped
// commands. No renderer lock or per-command allocation is needed.
class Queue
{
public:
    using Execute = void (*)(void*, uint32_t, const void*, size_t);
    struct Counters
    {
        uint64_t commands = 0, bytes = 0, backpressure = 0, wakes = 0;
    };
    Queue(size_t bytes, size_t slots, Execute execute, void* context)
        : arena_(bytes), slots_(slots), execute_(execute), context_(context)
    {
        if (bytes < 16 || !slots || !execute)
            throw std::invalid_argument("worker queue capacity/callback");
        worker_ = std::thread([this] { run(); });
    }
    ~Queue() { stop(); }
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    size_t capacity() const { return arena_.size(); }
    template <class Fill> bool put(uint32_t op, size_t bytes, Fill&& fill)
    {
        if (bytes > arena_.size())
            return false;
        size_t aligned = (bytes + 15) & ~size_t(15);
        if (!aligned)
            aligned = 16;
        if (aligned > arena_.size())
            return false;
        std::unique_lock<std::mutex> lock(mutex_);
        bool counted = false;
        for (;;)
        {
            rethrow();
            if (stopping_)
                throw std::runtime_error("worker queue stopped");
            size_t padding = write_ + aligned > arena_.size() ? arena_.size() - write_ : 0;
            size_t occupied = padding + aligned;
            if (count_ < slots_.size() && occupied <= arena_.size() - used_)
            {
                size_t offset = padding ? 0 : write_;
                // fill must not throw after publication; on a throw no arena or
                // header accounting has changed, so existing work is intact.
                fill(arena_.data() + offset);
                slots_[push_] = {op, offset, bytes, occupied};
                push_ = (push_ + 1) % slots_.size();
                write_ = (offset + aligned) % arena_.size();
                used_ += occupied;
                ++count_;
                ++counters_.commands;
                counters_.bytes += bytes;
                ready_.notify_one();
                return true;
            }
            if (!counted)
            {
                ++counters_.backpressure;
                counted = true;
            }
            room_.wait(lock);
        }
    }
    void drain()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        room_.wait(lock, [&] { return (!count_ && !active_) || error_; });
        rethrow();
    }
    void stop() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            ready_.notify_all();
            room_.notify_all();
        }
        if (worker_.joinable())
            worker_.join();
    }
    Counters counters()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return counters_;
    }
#if defined(__ANDROID__)
    std::thread::native_handle_type native_handle() { return worker_.native_handle(); }
#endif
private:
    struct Slot
    {
        uint32_t op = 0;
        size_t offset = 0, bytes = 0, occupied = 0;
    };
    void rethrow()
    {
        if (error_)
            std::rethrow_exception(error_);
    }
    void run() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;)
        {
            ready_.wait(lock, [&] { return count_ || stopping_; });
            if (!count_)
                return;
            ++counters_.wakes;
            Slot slot = slots_[pop_];
            active_ = true;
            lock.unlock();
            try
            {
                execute_(context_, slot.op, arena_.data() + slot.offset, slot.bytes);
            }
            catch (...)
            {
                lock.lock();
                error_ = std::current_exception();
                stopping_ = true;
                active_ = false;
                room_.notify_all();
                ready_.notify_all();
                return;
            }
            lock.lock();
            active_ = false;
            used_ -= slot.occupied;
            pop_ = (pop_ + 1) % slots_.size();
            --count_;
            // Reclaim otherwise unusable tail padding when drained. The live
            // command is counted until execution ends, so no producer can reset
            // arena positions while execute reads it.
            if (!count_)
            {
                used_ = 0;
                write_ = 0;
                push_ = 0;
                pop_ = 0;
            }
            room_.notify_all();
        }
    }
    std::vector<uint8_t> arena_;
    std::vector<Slot> slots_;
    Execute execute_;
    void* context_;
    std::mutex mutex_;
    std::condition_variable ready_, room_;
    std::thread worker_;
    size_t push_ = 0, pop_ = 0, count_ = 0, write_ = 0, used_ = 0;
    bool active_ = false, stopping_ = false;
    std::exception_ptr error_;
    Counters counters_;
};
}
