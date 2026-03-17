#pragma once

#include <array>
#include <cstddef>
#include <cmath>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// RingBuffer<MaxSize>
//
// Fixed-capacity delay line. Single-thread use only (audio callback).
// Supports fractional-sample reads via linear interpolation.
// ─────────────────────────────────────────────────────────────────────────────
template <std::size_t MaxSize>
class RingBuffer
{
public:
    /// Set the active size (must be <= MaxSize). Call before audio starts.
    void setSize(std::size_t size) noexcept
    {
        size_     = (size <= MaxSize) ? size : MaxSize;
        writePos_ = 0;
        buffer_.fill(0.0f);
    }

    /// Clear buffer and reset write position.
    void reset() noexcept
    {
        buffer_.fill(0.0f);
        writePos_ = 0;
    }

    /// Write one sample to the current position and advance.
    void push(float sample) noexcept
    {
        buffer_[writePos_] = sample;
        if (++writePos_ >= size_)
            writePos_ = 0;
    }

    /// Read a sample delayed by `delaySamples` (fractional, linear interpolation).
    /// delaySamples must be in [0, size - 1].
    float read(float delaySamples) const noexcept
    {
        const float  floorDelay = std::floor(delaySamples);
        const float  frac       = delaySamples - floorDelay;
        const int    d0         = static_cast<int>(floorDelay);
        const int    d1         = d0 + 1;

        const float s0 = readAt(d0);
        const float s1 = readAt(d1);
        return s0 + frac * (s1 - s0);
    }

    std::size_t getSize() const noexcept { return size_; }

private:
    float readAt(int delaySamples) const noexcept
    {
        // writePos_ points to the NEXT write position, so the most recent
        // sample is at writePos_ - 1.
        int idx = static_cast<int>(writePos_) - 1 - delaySamples;
        while (idx < 0)
            idx += static_cast<int>(size_);
        return buffer_[static_cast<std::size_t>(idx)];
    }

    std::array<float, MaxSize> buffer_{};
    std::size_t size_     { MaxSize };
    std::size_t writePos_ { 0 };
};

} // namespace dsp
