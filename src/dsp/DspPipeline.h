#pragma once

#include "BpmDetector.h"
#include "EffectChain.h"
#include "ExpressionMapper.h"
#include "KeyboardSynth.h"
#include "LockFreeQueue.h"
#include "MasterLimiter.h"
#include "Sampler.h"
#include "YinPitchTracker.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace dsp
{

struct KeyboardEvent {
    int   note;   // MIDI note 0-127
    float vel;    // 0..1
    bool  on;
};

// ─────────────────────────────────────────────────────────────────────────────
// DspPipeline
//
// Orchestrates: YIN pitch track → effect chain → sampler mix → master limiter.
//
// Pitch stabilization (v2):
//   The raw YIN pitch is gated (confidence + RMS) and smoothed in log-domain
//   before being sent to the effect chain. This eliminates jitter, octave
//   errors, and false pitch during silence/breath noise.
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

    /// Process a stereo block in-place. Realtime-safe.
    /// Both channels should contain the live saxophone input on entry.
    /// The sax effect chain runs on left; right gets a panned copy (+0.2 R).
    /// Sampler uses per-slot pan/Haas settings (set by SmartSamplerEngine).
    void processStereo(float* left, float* right, int numSamples) noexcept;

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

    // Reset the smoothed ducking state (call on preset change / scene switch).
    void resetDucking() noexcept { smoothDuck_ = 1.0f; }

    // ── Master Limiter (GUI thread) ──────────────────────────────────────────
    void setMasterLimiterEnabled(bool enabled) noexcept { masterLimiter_.setEnabled(enabled); }
    bool isMasterLimiterEnabled() const noexcept { return masterLimiter_.isEnabled(); }

    // ── Keyboard synth (GUI thread) ──────────────────────────────────────────
    // Instrument indépendant, mixé dans le bus master après le sampler.
    void keyboardNoteOn (int midiNote, float vel = 1.f) noexcept;
    void keyboardNoteOff(int midiNote)                  noexcept;
    void  setKeyboardParam   (int idx, float value) noexcept { keyboardSynth_.setParam(idx, value); }
    void  setKeyboardGain    (float g)              noexcept { keyboardSynth_.setGain(g); }
    void  applyKeyboardPreset(int idx)              noexcept { keyboardSynth_.applyPreset(idx); }
    float getKeyboardParam   (int idx)        const noexcept { return keyboardSynth_.getParam(idx); }
    float getKeyboardGain    ()               const noexcept { return keyboardSynth_.getGain(); }
    bool  getKeyboardMono    ()               const noexcept { return keyboardSynth_.getMonoMode(); }

  private:
    YinPitchTracker  pitchTracker_;
    EffectChain      effectChain_;
    Sampler          sampler_;
    KeyboardSynth    keyboardSynth_;
    BpmDetector      bpmDetector_;
    ExpressionMapper expressionMapper_;
    MasterLimiter    masterLimiter_;

    LockFreeQueue<SamplerEvent,   64> midiEventQueue_;
    LockFreeQueue<KeyboardEvent,  64> keyboardEventQueue_;

    std::atomic<bool> samplerEnabled_{true};
    std::atomic<bool> duckingEnabled_{false};
    std::atomic<float> currentDuckingGain_{1.0f};

    float smoothDuck_{ 1.0f };  // EMA-smoothed duck gain (audio thread only)

    std::vector<float> tempBuffer_; // Used for sampler ducking (mono path)
    std::vector<float> tempBufL_;   // Stereo path: sampler left  temp
    std::vector<float> tempBufR_;   // Stereo path: sampler right temp

    // Latest pitch (written by audio thread, read by GUI thread)
    // lastPitchHz_ contains the STABLE pitch (after gating + smoothing), not raw YIN.
    std::atomic<float> lastPitchHz_{0.0f};
    std::atomic<float> lastConfidence_{0.0f};

    // Smoothed RMS: updated per-block on the audio thread, read by GUI
    float              rmsRunning_{ 0.0f };       // audio thread only
    std::atomic<float> rmsLevel_{   0.0f };       // GUI read via getLastRms()
    static constexpr float kRmsAlpha = 0.9995f;  // ~100ms at 44100 Hz

    // Forced pitch from piano keyboard (0 = off)
    std::atomic<float> forcedPitchHz_{ 0.f };

    // ── Pitch stabilization (v2) ─────────────────────────────────────────────
    // stablePitch_ is audio-thread-only (like rmsRunning_).
    // The stabilized value is exported to lastPitchHz_ (atomic) for GUI.
    double sampleRate_        { 48000.0 };
    float  stablePitch_       { 0.0f };    ///< audio thread only
    int    unvoicedSamples_   { 0 };       ///< audio thread only — hold timeout counter

    // Tuning constants for pitch gating + smoothing
    static constexpr float kConfidenceGate = 0.82f;    ///< min confidence to accept pitch
    static constexpr float kRmsGate        = 0.02f;    ///< min RMS to consider "voiced"
    static constexpr float kSmoothTimeSec  = 0.05f;    ///< 50ms log-domain EMA
    static constexpr float kHoldTimeoutSec = 0.20f;    ///< 200ms before releasing held pitch
};

} // namespace dsp
