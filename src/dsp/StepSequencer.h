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
        sampleRate_      = sampleRate;
        phase_           = 0.0;
        nextFirePhase_   = 0.0;
        nextFireStepIdx_ = 0;
        lastSwingOff_    = 0.0;
        swingCurrent_    = 0.f;
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
            phase_           = -1e-9;
            nextFirePhase_   = 0.0;
            nextFireStepIdx_ = 0;
            lastSwingOff_    = 0.0;
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
        phase_           = 0.0;
        nextFirePhase_   = 0.0;
        nextFireStepIdx_ = 0;
        lastSwingOff_    = 0.0;
        swingCurrent_    = 0.f;
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
        phase_           = -1e-9;
        nextFirePhase_   = 0.0;
        nextFireStepIdx_ = 0;
        lastSwingOff_    = 0.0;
        stepAtomic_.store(0, std::memory_order_relaxed);
    }

    /// Swing global [0..1] : 0=straight, 0.5=swing, 1.0=shuffle.
    /// Thread-safe (atomic write, audio thread reads).
    void setSwing(float amount) noexcept
    {
        swingTarget_.store(juce::jlimit(0.f, 1.f, amount), std::memory_order_relaxed);
    }

    float getSwing() const noexcept { return swingTarget_.load(std::memory_order_relaxed); }

    /// Beat phase at start of last audio block — read from audio thread only.
    /// Used by LooperEngine to detect bar downbeats.
    double getCurrentPhase() const noexcept { return phase_; }

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
        // Immediate flip only when no bar-quantized transition is pending.
        // When transLen > 0, the flip is deferred to step 0 inside the loop below.
        if (pendingTransLen_.load(std::memory_order_relaxed) == 0)
            flipIfPrepared();

        const float bpm = bpm_.load(std::memory_order_relaxed);
        if (!playing_.load(std::memory_order_relaxed) || bpm <= 0.f)
            return;

        static constexpr double kStepBeats = 0.25;

        // Smooth swing toward target (audio thread — no lock needed)
        swingCurrent_ += 0.05f * (swingTarget_.load(std::memory_order_relaxed) - swingCurrent_);

        phase_ += static_cast<double>(numSamples)
                  / (sampleRate_ * 60.0 / static_cast<double>(bpm));

        // Absolute-position firing loop — handles multiple steps per block correctly.
        // nextFirePhase_: absolute beat phase at which the next step fires.
        // nextFireStepIdx_: monotonically increasing step counter (% kMaxSteps for pattern index).
        while (phase_ > nextFirePhase_)
        {
            const int globalStep = nextFireStepIdx_ % kMaxSteps;
            stepAtomic_.store(globalStep, std::memory_order_relaxed);

            const int transLen = pendingTransLen_.load(std::memory_order_relaxed);
            const bool atSceneBoundary = (transLen > 0 && globalStep % transLen == transLen - 1);

            if (atSceneBoundary)
                sceneEndFlag_.store(true, std::memory_order_release);

            // Bar-quantized flip: activate the new step buffer BEFORE triggering step 0.
            // prepareStepBuffer() was called from navigateScene() (message thread) well
            // before this point, so the buffer is ready regardless of timer latency.
            if (transLen > 0 && globalStep % transLen == 0)
            {
                flipIfPrepared();
                pendingTransLen_.store(0, std::memory_order_relaxed);
            }

            // Always trigger regardless of scene boundary: the bass/kick on step 0
            // must play even when the transition flag fires at that same step.
            {
                const StepBuf& active = swapBufs_[activeBuf_.load(std::memory_order_relaxed)];
                for (int track = 0; track < kTracks; ++track)
                {
                    const int trackSteps = active.trackStepCount[track];
                    if (trackSteps <= 0) continue;
                    const int trackStep  = globalStep % trackSteps;
                    if (trackStep == 0)
                        sampler.onTrackStep0(track);
                    if (active.steps[track][trackStep])
                        sampler.trigger(track);
                }
            }

            // Advance to next fire phase.
            // Odd steps are delayed by swingOff; even steps compensate with -lastSwingOff_
            // so total pair duration stays exactly 2 × kStepBeats (no tempo drift).
            ++nextFireStepIdx_;
            if (nextFireStepIdx_ % 2 == 1)  // next step is odd → apply swing
            {
                const double swingOff = static_cast<double>(swingCurrent_) * kStepBeats / 3.0;
                nextFirePhase_ += kStepBeats + swingOff;
                lastSwingOff_   = swingOff;
            }
            else  // next step is even → compensate previous swing delay
            {
                nextFirePhase_ += kStepBeats - lastSwingOff_;
                lastSwingOff_   = 0.0;
            }
        }

        if (phase_ >= 4096.0)
        {
            phase_         -= 4096.0;
            nextFirePhase_ -= 4096.0;
        }
    }

    /// Fige la longueur de scène utilisée pour détecter la fin de cycle.
    void setPendingTransitionLen(int steps) noexcept
    {
        pendingTransLen_.store(steps, std::memory_order_release);
    }

    /// Consumes the scene-end signal (returns true once per cycle boundary).
    bool consumeSceneEnd() noexcept
    {
        return sceneEndFlag_.exchange(false, std::memory_order_acq_rel);
        // pendingTransLen_ cleared by the audio thread at step 0, after the buffer flip.
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

    double phase_           = 0.0;   // audio thread only — beat phase accumulator
    double nextFirePhase_   = 0.0;   // audio thread only — beat phase of next step fire
    int    nextFireStepIdx_ = 0;     // audio thread only — monotonic step counter (for even/odd swing)
    double lastSwingOff_    = 0.0;   // audio thread only — swing offset applied to last odd step
    float  swingCurrent_    = 0.f;   // audio thread only — smoothed swing value

    double sampleRate_ = 44100.0;

    std::atomic<float> bpm_             { 0.f };
    std::atomic<float> swingTarget_     { 0.f };   // GUI writes, audio reads
    std::atomic<bool>  playing_         { false };
    std::atomic<int>   stepAtomic_      { 0 };
    std::atomic<bool>  sceneEndFlag_    { false };
    std::atomic<int>   pendingTransLen_ { 0 };
};

} // namespace dsp
