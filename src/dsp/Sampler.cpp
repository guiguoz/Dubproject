#include "Sampler.h"
#include <algorithm>
#include <cassert>

namespace dsp {

void Sampler::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
}

void Sampler::loadSample(int slot, const float* data, int numSamples,
                          double /*fileSampleRate*/) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    if (!data || numSamples <= 0)      return;

    auto& s = slots_[static_cast<std::size_t>(slot)];

    // Mark as unloaded while we modify data (audio thread checks loaded flag).
    s.loaded.store(false, std::memory_order_release);

    s.data.assign(data, data + numSamples);
    s.sampleCount = numSamples;

    // Make data visible to the audio thread before setting the loaded flag.
    s.loaded.store(true, std::memory_order_release);
}

void Sampler::clearSlot(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& s = slots_[static_cast<std::size_t>(slot)];
    s.loaded.store(false, std::memory_order_release);
    stop(slot);
}

void Sampler::trigger(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    playStates_[static_cast<std::size_t>(slot)]
        .triggerPending.store(true, std::memory_order_release);
}

void Sampler::stop(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    playStates_[static_cast<std::size_t>(slot)]
        .stopPending.store(true, std::memory_order_release);
}

void Sampler::setSlotGain(int slot, float gain) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(slot)].gain.store(gain, std::memory_order_relaxed);
}

void Sampler::setSlotLoop(int slot, bool loop) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(slot)].loopEnabled.store(loop, std::memory_order_relaxed);
}

void Sampler::setSlotOneShot(int slot, bool oneShot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(slot)].oneShot.store(oneShot, std::memory_order_relaxed);
}

bool Sampler::isLoaded(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return slots_[static_cast<std::size_t>(slot)].loaded.load(std::memory_order_acquire);
}

bool Sampler::isPlaying(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return playStates_[static_cast<std::size_t>(slot)].playing.load(std::memory_order_acquire);
}

void Sampler::process(float* buffer, int numSamples) noexcept
{
    for (int v = 0; v < kMaxSlots; ++v)
    {
        auto& ps  = playStates_[static_cast<std::size_t>(v)];
        auto& sl  = slots_[static_cast<std::size_t>(v)];

        // Handle stop request first.
        if (ps.stopPending.load(std::memory_order_acquire))
        {
            ps.stopPending.store(false, std::memory_order_relaxed);
            ps.playing.store(false, std::memory_order_relaxed);
            ps.readPos = 0;
        }

        // Handle trigger request.
        if (ps.triggerPending.load(std::memory_order_acquire))
        {
            ps.triggerPending.store(false, std::memory_order_relaxed);
            if (sl.loaded.load(std::memory_order_acquire))
            {
                ps.readPos = 0;
                ps.playing.store(true, std::memory_order_relaxed);
            }
        }

        if (!ps.playing.load(std::memory_order_relaxed))
            continue;

        if (!sl.loaded.load(std::memory_order_acquire))
        {
            ps.playing.store(false, std::memory_order_relaxed);
            continue;
        }

        const float gain      = sl.gain.load(std::memory_order_relaxed);
        const bool  loop      = sl.loopEnabled.load(std::memory_order_relaxed);
        const int   totalSamp = sl.sampleCount;

        for (int i = 0; i < numSamples; ++i)
        {
            if (ps.readPos >= totalSamp)
            {
                if (loop)
                    ps.readPos = 0;
                else
                {
                    ps.playing.store(false, std::memory_order_relaxed);
                    break;
                }
            }

            buffer[i] += gain * sl.data[static_cast<std::size_t>(ps.readPos)];
            ++ps.readPos;
        }
    }
}

void Sampler::reset() noexcept
{
    for (int v = 0; v < kMaxSlots; ++v)
    {
        auto& ps = playStates_[static_cast<std::size_t>(v)];
        ps.playing.store(false, std::memory_order_relaxed);
        ps.triggerPending.store(false, std::memory_order_relaxed);
        ps.stopPending.store(false, std::memory_order_relaxed);
        ps.readPos = 0;
    }
}

} // namespace dsp
