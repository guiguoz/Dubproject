#pragma once

#include "BpmDetector.h"
#include "EffectChain.h"
#include "ExpressionMapper.h"
#include "LockFreeQueue.h"
#include "MasterLimiter.h"
#include "Sampler.h"
#include "YinPitchTracker.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// DspPipeline
//
// Orchestrates: YIN pitch track → harmonizer → flanger → sampler mix → clip.
//
// Thread safety:
//   - enable/disable flags: atomic<bool> with acquire/release
//   - float parameters:     forwarded directly to sub-modules (atomic<float>)
//   - MIDI events:          consumed from midiEventQueue_ (lock-free SPSC)
//   - process() is called from the audio thread only
//   - set*() methods are called from the GUI thread only
// ─────────────────────────────────────────────────────────────────────────────
class DspPipeline
{
  public:
    /// Call from prepareToPlay(). Pre-allocates all internal memory.
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    /// Process a mono block in-place. Realtime-safe.
    void process(float* buffer, int numSamples) noexcept;

    void reset() noexcept;

    // ── Enable / disable (GUI thread) ───────────────────────────────────────
    void setSamplerEnabled(bool enabled) noexcept;
    bool isSamplerEnabled() const noexcept;

    // ── Beat clock (GUI thread) ─────────────────────────────────────────────
    // Forward the master BPM to sampler + tempo-synced effects (Delay).
    void setBpm(float bpm) noexcept
    {
        sampler_.setBpm(bpm);
        effectChain_.setBpm(bpm);
    }

    // ── Effect Chain and other sub-modules ──────────────────────────────────
    EffectChain& getEffectChain() noexcept { return effectChain_; }
    YinPitchTracker& getPitchTracker() noexcept { return pitchTracker_; }
    Sampler& getSampler() noexcept { return sampler_; }
    BpmDetector& getBpmDetector() noexcept { return bpmDetector_; }
    ExpressionMapper& getExpressionMapper() noexcept { return expressionMapper_; }

    /// Queue used by MidiManager to push SamplerEvents (lock-free).
    LockFreeQueue<SamplerEvent, 64>& getMidiEventQueue() noexcept
    {
        return midiEventQueue_;
    }

    /// Latest pitch result (safe to read from GUI thread — atomic copy)
    PitchResult getLastPitch() const noexcept;

    /// Smoothed input RMS level (safe to read from GUI thread).
    float getLastRms() const noexcept
    {
        return rmsLevel_.load(std::memory_order_relaxed);
    }

    // ── Keyboard forced pitch (GUI thread) ──────────────────────────────────
    // When > 0, the piano keyboard panel overrides the YIN-detected pitch for
    // the effect chain (SynthEffect follows it).  0 = use live YIN pitch.
    void setForcedPitch(float hz) noexcept
    {
        forcedPitchHz_.store(hz, std::memory_order_relaxed);
    }
    void clearForcedPitch() noexcept
    {
        forcedPitchHz_.store(0.f, std::memory_order_relaxed);
    }

    // ── Anti-masking (Ducking) ───────────────────────────────────────────────
    // Toggle whether the sampler ducks when the saxophone plays loud.
    void setDuckingEnabled(bool enabled) noexcept
    {
        duckingEnabled_.store(enabled, std::memory_order_relaxed);
    }
    bool isDuckingEnabled() const noexcept { return duckingEnabled_.load(std::memory_order_relaxed); }
    float getCurrentDuckingGain() const noexcept { return currentDuckingGain_.load(std::memory_order_relaxed); }

    // ── Master Limiter (GUI thread) ──────────────────────────────────────────
    void setMasterLimiterEnabled(bool enabled) noexcept { masterLimiter_.setEnabled(enabled); }
    bool isMasterLimiterEnabled() const noexcept { return masterLimiter_.isEnabled(); }

  private:
    YinPitchTracker  pitchTracker_;
    EffectChain      effectChain_;
    Sampler          sampler_;
    BpmDetector      bpmDetector_;
    ExpressionMapper expressionMapper_;
    MasterLimiter    masterLimiter_;

    LockFreeQueue<SamplerEvent, 64> midiEventQueue_;

    std::atomic<bool> samplerEnabled_{true};
    std::atomic<bool> duckingEnabled_{true};
    std::atomic<float> currentDuckingGain_{1.0f};

    std::vector<float> tempBuffer_; // Used for sampler ducking

    // Latest pitch (written by audio thread, read by GUI thread)
    std::atomic<float> lastPitchHz_{0.0f};
    std::atomic<float> lastConfidence_{0.0f};

    // Smoothed RMS: updated per-block on the audio thread, read by GUI
    float              rmsRunning_{ 0.0f };       // audio thread only
    std::atomic<float> rmsLevel_{   0.0f };       // GUI read via getLastRms()
    static constexpr float kRmsAlpha = 0.9995f;  // ~100ms at 44100 Hz

    // Forced pitch from piano keyboard (0 = off)
    std::atomic<float> forcedPitchHz_{ 0.f };
};

} // namespace dsp
