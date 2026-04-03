#pragma once

#include "Sampler.h"

#include <atomic>
#include <cmath>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// StepSequencer
//
// 8-track × up-to-32-step sequencer (1/16th-note resolution per step).
// Each track can have an independent step count (4, 8, 16, 32).
// Driven by BPM; triggers Sampler slots at step boundaries.
//
// Thread safety:
//   - setBpm(), setPlaying(), setStep(), setTrackStepCount() → GUI thread
//   - process() → audio thread only
//   - getCurrentStep() → GUI thread, reads atomic
// ─────────────────────────────────────────────────────────────────────────────
class StepSequencer
{
public:
    static constexpr int kTracks        = 8;
    static constexpr int kMaxSteps      = 512;  // up to 32 bars (32 × 16 steps)
    static constexpr int kStepsPerBar   = 16;   // 1/16th-note resolution, 4/4 time
    static constexpr int kSteps         = 16;   // kept for legacy compat
    static constexpr int kMaxBars       = kMaxSteps / kStepsPerBar;  // 32

    // ── Setup ────────────────────────────────────────────────────────────────

    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate;
        phase_      = 0.0;
        stepAtomic_.store(0, std::memory_order_relaxed);
    }

    // ── GUI-thread controls ──────────────────────────────────────────────────

    void setBpm(float bpm) noexcept
    {
        bpm_.store(bpm, std::memory_order_relaxed);
    }

    float getBpm() const noexcept
    {
        return bpm_.load(std::memory_order_relaxed);
    }

    void setPlaying(bool play) noexcept
    {
        if (!play)
        {
            phase_ = 0.0;
            stepAtomic_.store(0, std::memory_order_relaxed);
        }
        playing_.store(play, std::memory_order_relaxed);
    }

    bool isPlaying() const noexcept
    {
        return playing_.load(std::memory_order_relaxed);
    }

    /// Set the active step count for a track (clamped to [1, kMaxSteps]).
    void setTrackStepCount(int track, int count) noexcept
    {
        if (track >= 0 && track < kTracks)
        {
            if (count < 1)         count = 1;
            if (count > kMaxSteps) count = kMaxSteps;
            trackStepCount_[track] = count;
        }
    }

    int getTrackStepCount(int track) const noexcept
    {
        if (track >= 0 && track < kTracks)
            return trackStepCount_[track];
        return kMaxSteps;
    }

    /// Convenience: set pattern length in bars (1–kMaxBars). Each bar = kStepsPerBar steps.
    void setTrackBarCount(int track, int bars) noexcept
    {
        if (bars < 1)      bars = 1;
        if (bars > kMaxBars) bars = kMaxBars;
        setTrackStepCount(track, bars * kStepsPerBar);
    }

    /// Returns the pattern length in bars (rounded up).
    int getTrackBarCount(int track) const noexcept
    {
        const int steps = getTrackStepCount(track);
        return (steps + kStepsPerBar - 1) / kStepsPerBar;
    }

    /// Toggle or set a step. Safe to call from GUI thread while sequencer runs.
    void setStep(int track, int step, bool active) noexcept
    {
        if (track >= 0 && track < kTracks && step >= 0 && step < kMaxSteps)
            steps_[track][step] = active;
    }

    bool getStep(int track, int step) const noexcept
    {
        if (track >= 0 && track < kTracks && step >= 0 && step < kMaxSteps)
            return steps_[track][step];
        return false;
    }

    /// Current playhead step (0 .. max active steps-1). GUI thread reads for animation.
    int getCurrentStep() const noexcept
    {
        return stepAtomic_.load(std::memory_order_relaxed);
    }

    void reset() noexcept
    {
        phase_ = 0.0;
        stepAtomic_.store(0, std::memory_order_relaxed);
        playing_.store(false, std::memory_order_relaxed);
        for (int t = 0; t < kTracks; ++t)
        {
            trackStepCount_[t] = kSteps;  // default 16
            for (int s = 0; s < kMaxSteps; ++s)
                steps_[t][s] = false;
        }
    }

    // ── Audio thread ─────────────────────────────────────────────────────────

    /// Call from getNextAudioBlock() BEFORE dspPipeline_.process().
    /// Each track loops independently on its own step count.
    void process(int numSamples, Sampler& sampler) noexcept
    {
        const float bpm = bpm_.load(std::memory_order_relaxed);
        if (!playing_.load(std::memory_order_relaxed) || bpm <= 0.f)
            return;

        // Each 1/16th note = 0.25 beats
        static constexpr double kStepBeats = 0.25;

        const double phaseBefore = phase_;
        phase_ += static_cast<double>(numSamples)
                  / (sampleRate_ * 60.0 / static_cast<double>(bpm));

        // Global step index (wraps on kMaxSteps to cover all possible track lengths)
        const int globalBefore = static_cast<int>(std::floor(phaseBefore / kStepBeats)) % kMaxSteps;
        const int globalAfter  = static_cast<int>(std::floor(phase_       / kStepBeats)) % kMaxSteps;

        if (globalBefore != globalAfter)
        {
            stepAtomic_.store(globalAfter, std::memory_order_relaxed);

            for (int track = 0; track < kTracks; ++track)
            {
                const int trackSteps = trackStepCount_[track];
                const int trackStep  = globalAfter % trackSteps;

                if (steps_[track][trackStep])
                    sampler.trigger(track);
            }

            // Detect end of scene: when the longest track has completed a full cycle.
            // Signal the GUI thread so it can apply any pending scene change.
            int maxSteps = 1;
            for (int t = 0; t < kTracks; ++t)
                if (trackStepCount_[t] > maxSteps) maxSteps = trackStepCount_[t];
            if (globalAfter % maxSteps == 0)
                sceneEndFlag_.store(true, std::memory_order_release);
        }

        // Wrap phase to avoid double-precision drift
        if (phase_ >= 4096.0)
            phase_ -= 4096.0;
    }

    /// Consumes the scene-end signal (returns true once per cycle boundary).
    /// Call from the GUI thread (e.g. timerCallback) to check for pending scene transitions.
    bool consumeSceneEnd() noexcept
    {
        return sceneEndFlag_.exchange(false, std::memory_order_acq_rel);
    }

private:
    bool   steps_[kTracks][kMaxSteps] {};
    int    trackStepCount_[kTracks]   = { kStepsPerBar, kStepsPerBar, kStepsPerBar, kStepsPerBar,
                                          kStepsPerBar, kStepsPerBar, kStepsPerBar, kStepsPerBar };
    double phase_      = 0.0;    // audio thread only
    double sampleRate_ = 44100.0;

    std::atomic<float> bpm_         { 0.f };
    std::atomic<bool>  playing_     { false };
    std::atomic<int>   stepAtomic_  { 0 };
    std::atomic<bool>  sceneEndFlag_{ false };
};

} // namespace dsp
