#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// SamplerEvent
// Sent from the MIDI thread to the audio thread via LockFreeQueue.
// ─────────────────────────────────────────────────────────────────────────────
struct SamplerEvent
{
    int  slotIndex { -1 };   // 0-7, or -1 = invalid
    bool noteOn    { true };  // true = trigger, false = stop
};

// ─────────────────────────────────────────────────────────────────────────────
// LockFreeQueue<T, Capacity>
//
// Single-Producer Single-Consumer (SPSC) ring buffer.
// - tryPush : called from MIDI / GUI thread (producer)
// - tryPop  : called from audio thread (consumer)
// Capacity MUST be a power of 2 (enforced by static_assert).
// ─────────────────────────────────────────────────────────────────────────────
template <typename T, std::size_t Capacity>
class LockFreeQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "LockFreeQueue: Capacity must be a power of 2");

public:
    LockFreeQueue() noexcept = default;

    // Returns false if the queue is full (event dropped).
    bool tryPush(const T& item) noexcept
    {
        const std::size_t w = writePos_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & kMask;

        if (next == readPos_.load(std::memory_order_acquire))
            return false; // full

        buffer_[w] = item;
        writePos_.store(next, std::memory_order_release);
        return true;
    }

    // Returns false if the queue is empty.
    bool tryPop(T& item) noexcept
    {
        const std::size_t r = readPos_.load(std::memory_order_relaxed);

        if (r == writePos_.load(std::memory_order_acquire))
            return false; // empty

        item = buffer_[r];
        readPos_.store((r + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Approximate size (may be stale by the time the caller uses it).
    std::size_t size() const noexcept
    {
        const std::size_t w = writePos_.load(std::memory_order_relaxed);
        const std::size_t r = readPos_.load(std::memory_order_relaxed);
        return (w - r) & kMask;
    }

    bool empty() const noexcept { return size() == 0; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity>    buffer_{};
    std::atomic<std::size_t>   readPos_  { 0 };
    std::atomic<std::size_t>   writePos_ { 0 };
};

} // namespace dsp
