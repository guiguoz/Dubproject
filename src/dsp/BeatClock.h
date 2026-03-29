#pragma once

#include <atomic>
#include <cmath>

namespace dsp {

// Quantization grid divisions (in beats relative to a 4/4 bar).
enum class GridDiv { Eighth, Quarter, HalfBar, Bar };

// ─────────────────────────────────────────────────────────────────────────────
// BeatClock
//
// A lightweight master beat clock driven by an external BPM value.
// Phase is accumulated in the audio thread; BPM is set from the GUI thread
// via an atomic.
//
// Usage (audio thread):
//   const double before = clock.advance(numSamples);
//   if (BeatClock::crossedBoundary(before, clock.getPhase(), GridDiv::Quarter))
//       // do something on the beat
//
// Thread safety:
//   - setBpm()  → GUI thread only
//   - advance() → audio thread only
//   - getPhase()→ audio thread only
// ─────────────────────────────────────────────────────────────────────────────
class BeatClock
{
public:
    void prepare(double sampleRate) noexcept { sampleRate_ = sampleRate; }

    // GUI thread — set master tempo (0 = clock stopped)
    void setBpm(float bpm) noexcept { bpm_.store(bpm, std::memory_order_relaxed); }
    float getBpm() const noexcept   { return bpm_.load(std::memory_order_relaxed); }
    bool  isRunning() const noexcept { return getBpm() > 0.f; }

    // Audio thread — advance phase by numSamples.
    // Returns phase BEFORE the advance (useful for boundary detection).
    double advance(int numSamples) noexcept
    {
        const double before = phase_;
        const float  bpm    = bpm_.load(std::memory_order_relaxed);
        if (bpm > 0.f)
            phase_ += static_cast<double>(numSamples)
                      / (sampleRate_ * 60.0 / static_cast<double>(bpm));
        return before;
    }

    // Audio thread
    double getPhase() const noexcept { return phase_; }

    // Pure helper — did phase cross a GridDiv boundary during [phaseBefore, phaseAfter)?
    static bool crossedBoundary(double phaseBefore, double phaseAfter, GridDiv div) noexcept
    {
        const double len = divLengthBeats(div);
        return std::floor(phaseAfter / len) > std::floor(phaseBefore / len);
    }

    static constexpr double divLengthBeats(GridDiv div) noexcept
    {
        switch (div)
        {
            case GridDiv::Eighth:   return 0.5;
            case GridDiv::Quarter:  return 1.0;
            case GridDiv::HalfBar:  return 2.0;
            case GridDiv::Bar:      return 4.0;
        }
        return 1.0;
    }

private:
    double             sampleRate_ = 44100.0;
    std::atomic<float> bpm_        { 0.f };
    double             phase_      = 0.0;  // beats; audio thread only
};

} // namespace dsp
