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
//   - prepareStepBuffer() → GUI thread; flipIfPrepared() → audio thread
//
// Double-buffer pattern swap:
//   GUI calls prepareStepBuffer(buf) to write the next scene into swapBufs_[writeSlot].
//   At the next audio block (or bar boundary), flipIfPrepared() atomically swaps
//   activeBuf_ to the prepared slot. Audio never reads a partially-written buffer.
// ─────────────────────────────────────────────────────────────────────────────
class StepSequencer
{
public:
    static constexpr int kTracks        = 9;
    static constexpr int kMaxSteps      = 512;  // up to 32 bars (32 × 16 steps)
    static constexpr int kStepsPerBar   = 16;   // 1/16th-note resolution, 4/4 time
    static constexpr int kSteps         = 16;   // kept for legacy compat
    static constexpr int kMaxBars       = kMaxSteps / kStepsPerBar;  // 32

    // ── Step buffer (one of two swap slots) ──────────────────────────────────

    struct StepBuf
    {
        bool steps[kTracks][kMaxSteps] {};
        int  trackStepCount[kTracks]   { kStepsPerBar, kStepsPerBar, kStepsPerBar, kStepsPerBar,
                                         kStepsPerBar, kStepsPerBar, kStepsPerBar, kStepsPerBar,
                                         kStepsPerBar };
    };

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
        if (play)
        {
            phase_ = -1e-9;
            stepAtomic_.store(0, std::memory_order_relaxed);
        }
        else
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
    /// Writes to both swap buffers to keep them in sync.
    void setTrackStepCount(int track, int count) noexcept
    {
        if (track < 0 || track >= kTracks) return;
        if (count < 1)         count = 1;
        if (count > kMaxSteps) count = kMaxSteps;
        swapBufs_[0].trackStepCount[track] = count;
        swapBufs_[1].trackStepCount[track] = count;
    }

    int getTrackStepCount(int track) const noexcept
    {
        if (track < 0 || track >= kTracks) return kMaxSteps;
        return swapBufs_[activeBuf_.load(std::memory_order_relaxed)].trackStepCount[track];
    }

    /// Convenience: set pattern length in bars (1–kMaxBars). Each bar = kStepsPerBar steps.
    void setTrackBarCount(int track, int bars) noexcept
    {
        if (bars < 1)        bars = 1;
        if (bars > kMaxBars) bars = kMaxBars;
        setTrackStepCount(track, bars * kStepsPerBar);
    }

    /// Returns the pattern length in bars (rounded up).
    int getTrackBarCount(int track) const noexcept
    {
        const int steps = getTrackStepCount(track);
        return (steps + kStepsPerBar - 1) / kStepsPerBar;
    }

    /// Toggle or set a step. Writes to both swap buffers (live edit, keeps bufs in sync).
    void setStep(int track, int step, bool active) noexcept
    {
        if (track < 0 || track >= kTracks || step < 0 || step >= kMaxSteps) return;
        swapBufs_[0].steps[track][step] = active;
        swapBufs_[1].steps[track][step] = active;
    }

    bool getStep(int track, int step) const noexcept
    {
        if (track < 0 || track >= kTracks || step < 0 || step >= kMaxSteps) return false;
        return swapBufs_[activeBuf_.load(std::memory_order_relaxed)].steps[track][step];
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
        activeBuf_.store(0, std::memory_order_relaxed);
        preparedBuf_.store(-1, std::memory_order_relaxed);
        swapBufs_[0] = StepBuf{};
        swapBufs_[1] = StepBuf{};
    }

    /// Remet la phase à zéro (step 0) sans arrêter la lecture.
    void resetPhase() noexcept
    {
        phase_ = -1e-9;
        stepAtomic_.store(0, std::memory_order_relaxed);
    }

    // ── Double-buffer scene transition ───────────────────────────────────────

    /// GUI thread: prépare le prochain buffer de patterns sans toucher au buffer actif.
    /// La copie complète (512 steps × 9 tracks) est écrite dans le slot inactif,
    /// puis signalée via preparedBuf_ (release). flipIfPrepared() finalisera le swap.
    void prepareStepBuffer(const StepBuf& buf) noexcept
    {
        const int writeSlot = 1 - activeBuf_.load(std::memory_order_relaxed);
        swapBufs_[writeSlot] = buf;
        preparedBuf_.store(writeSlot, std::memory_order_release);
    }

    /// Audio thread: swap atomique vers le buffer préparé.
    /// Appelé au début de process() — garantit que le nouveau buffer est actif
    /// avant que les triggers de ce bloc soient calculés.
    void flipIfPrepared() noexcept
    {
        const int p = preparedBuf_.exchange(-1, std::memory_order_acq_rel);
        if (p >= 0)
            activeBuf_.store(p, std::memory_order_relaxed);
    }

    // ── Audio thread ─────────────────────────────────────────────────────────

    /// Call from getNextAudioBlock() BEFORE dspPipeline_.process().
    /// Each track loops independently on its own step count.
    void process(int numSamples, Sampler& sampler) noexcept
    {
        flipIfPrepared();

        const float bpm = bpm_.load(std::memory_order_relaxed);
        if (!playing_.load(std::memory_order_relaxed) || bpm <= 0.f)
            return;

        static constexpr double kStepBeats = 0.25;

        const double phaseBefore = phase_;
        phase_ += static_cast<double>(numSamples)
                  / (sampleRate_ * 60.0 / static_cast<double>(bpm));

        const int globalBefore = static_cast<int>(std::floor(phaseBefore / kStepBeats)) % kMaxSteps;
        const int globalAfter  = static_cast<int>(std::floor(phase_       / kStepBeats)) % kMaxSteps;

        if (globalBefore != globalAfter)
        {
            stepAtomic_.store(globalAfter, std::memory_order_relaxed);

            const int transLen = pendingTransLen_.load(std::memory_order_relaxed);
            const bool atSceneBoundary = (transLen > 0 && globalAfter % transLen == 0);

            if (atSceneBoundary)
            {
                sceneEndFlag_.store(true, std::memory_order_release);
            }
            else
            {
                const StepBuf& active = swapBufs_[activeBuf_.load(std::memory_order_relaxed)];
                for (int track = 0; track < kTracks; ++track)
                {
                    const int trackSteps = active.trackStepCount[track];
                    const int trackStep  = globalAfter % trackSteps;
                    if (active.steps[track][trackStep])
                        sampler.trigger(track);
                }
            }
        }

        if (phase_ >= 4096.0)
            phase_ -= 4096.0;
    }

    /// Fige la longueur de scène utilisée pour détecter la fin de cycle.
    void setPendingTransitionLen(int steps) noexcept
    {
        pendingTransLen_.store(steps, std::memory_order_release);
    }

    /// Consumes the scene-end signal (returns true once per cycle boundary).
    bool consumeSceneEnd() noexcept
    {
        const bool fired = sceneEndFlag_.exchange(false, std::memory_order_acq_rel);
        if (fired) pendingTransLen_.store(0, std::memory_order_relaxed);
        return fired;
    }

    /// Returns true if a quantized scene transition is already armed.
    bool hasPendingTransition() const noexcept
    {
        return pendingTransLen_.load(std::memory_order_relaxed) > 0;
    }

private:
    StepBuf          swapBufs_[2];
    std::atomic<int> activeBuf_   { 0 };   // audio thread swap target; GUI reads for getStep/getCount
    std::atomic<int> preparedBuf_ { -1 };  // -1 = nothing prepared; ≥0 = slot ready to flip

    double phase_      = 0.0;    // audio thread only
    double sampleRate_ = 44100.0;

    std::atomic<float> bpm_             { 0.f };
    std::atomic<bool>  playing_         { false };
    std::atomic<int>   stepAtomic_      { 0 };
    std::atomic<bool>  sceneEndFlag_    { false };
    std::atomic<int>   pendingTransLen_ { 0 };
};

} // namespace dsp
