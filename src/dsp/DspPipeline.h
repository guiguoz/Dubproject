#pragma once

#include "BpmDetector.h"
#include "LockFreeQueue.h"
#include "MasterLimiter.h"
#include "PingPongDelay.h"
#include "Sampler.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// DspPipeline
//
// Orchestrates: sampler mix → dub delay bus → master limiter.
//
// Thread safety:
//   - enable/disable flags: atomic<bool> with acquire/release
//   - float parameters:     forwarded directly to sub-modules (atomic<float>)
//   - MIDI events:          consumed from midiEventQueue_ (lock-free SPSC)
//   - process() / processStereo() are called from the audio thread only
//   - set*() methods are called from the GUI thread only
// ─────────────────────────────────────────────────────────────────────────────
class DspPipeline
{
  public:
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    /// Process a mono block in-place. Realtime-safe.
    void process(float* buffer, int numSamples) noexcept;

    /// Process a stereo block in-place. Realtime-safe.
    void processStereo(float* left, float* right, int numSamples) noexcept;

    void reset() noexcept;

    // ── Enable / disable (GUI thread) ───────────────────────────────────────
    void setSamplerEnabled(bool enabled) noexcept;
    bool isSamplerEnabled() const noexcept;

    // ── Beat clock (GUI thread) ─────────────────────────────────────────────
    void setBpm(float bpm) noexcept
    {
        bpm_.store(bpm, std::memory_order_relaxed);
        sampler_.setBpm(bpm);
        dubDelay_.setBpm(bpm);
        pingPongDelay_.setBpm(bpm);
    }

    // ── Sub-module accessors ────────────────────────────────────────────────
    Sampler&        getSampler()        noexcept { return sampler_; }
    BpmDetector&    getBpmDetector()    noexcept { return bpmDetector_; }
    PingPongDelay&  getDubDelay()       noexcept { return dubDelay_; }

    /// Queue used by MidiManager to push SamplerEvents (lock-free).
    LockFreeQueue<SamplerEvent, 64>& getMidiEventQueue() noexcept
    {
        return midiEventQueue_;
    }

    /// Smoothed input RMS level (safe to read from GUI thread).
    float getLastRms() const noexcept
    {
        return rmsLevel_.load(std::memory_order_relaxed);
    }

    // ── Anti-masking (Ducking) ───────────────────────────────────────────────
    void setDuckingEnabled(bool enabled) noexcept
    {
        duckingEnabled_.store(enabled, std::memory_order_relaxed);
    }
    bool  isDuckingEnabled()       const noexcept { return duckingEnabled_.load(std::memory_order_relaxed); }
    float getCurrentDuckingGain()  const noexcept { return currentDuckingGain_.load(std::memory_order_relaxed); }

    void resetDucking() noexcept { smoothDuck_ = 1.0f; }

    void resetAllDelays() noexcept
    {
        dubDelay_.reset();
        pingPongDelay_.reset();
    }

    // ── Master Limiter (GUI thread) ──────────────────────────────────────────
    void setMasterLimiterEnabled(bool enabled) noexcept { masterLimiter_.setEnabled(enabled); }
    bool isMasterLimiterEnabled() const noexcept { return masterLimiter_.isEnabled(); }
    MasterLimiter& getMasterLimiter() noexcept { return masterLimiter_; }

    // ── Visualizer audio feed (GUI thread read) ──────────────────────────────
    // Copies the most recent n samples (≤ kVisBufSize) into dst.
    // Lock-free: audio thread writes relaxed; torn frame at wrap is imperceptible.
    static constexpr int kVisBufSize = 2048;
    void copyVisSamples(float* dst, int n) const noexcept
    {
        const int head = visBufWriteIdx_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            dst[i] = visBuf_[(head - n + i + kVisBufSize * 2) % kVisBufSize];
    }

    // Called from MainComponent (audio thread) before processStereo().
    // Serum buffers are injected into the FX chain (delay sends + direct).
    void setSerumInput(const float* L, const float* R, int n, float gain) noexcept
    {
        serumL_    = L;
        serumR_    = R;
        serumN_    = n;
        serumGain_ = gain;
    }

  private:
    Sampler       sampler_;
    BpmDetector   bpmDetector_;
    MasterLimiter masterLimiter_;
    PingPongDelay dubDelay_;
    PingPongDelay pingPongDelay_;

    LockFreeQueue<SamplerEvent, 64> midiEventQueue_;

    std::atomic<bool>  samplerEnabled_     { true };
    std::atomic<bool>  duckingEnabled_     { false };
    std::atomic<float> currentDuckingGain_ { 1.0f };
    std::atomic<float> bpm_               { 120.f };

    float smoothDuck_ { 1.0f };

    std::vector<float> tempBuffer_;
    std::vector<float> tempBufL_;
    std::vector<float> tempBufR_;
    std::vector<float> tempSendL_;
    std::vector<float> tempSendR_;

    // Smoothed RMS: updated per-block on the audio thread, read by GUI
    float              rmsRunning_ { 0.0f };
    std::atomic<float> rmsLevel_   { 0.0f };
    static constexpr float kRmsAlpha = 0.9995f;

    // FX sidechain: kick peak ducks delay/reverb sends (coeffs computed in prepare())
    float fxDuckGain_     { 1.0f };
    float fxDuckAttCoeff_ { 0.0f };   // ~2 ms attack
    float fxDuckRelCoeff_ { 0.0f };   // ~150 ms release

    // Serum input — set by MainComponent before processStereo(), consumed inside
    const float* serumL_    { nullptr };
    const float* serumR_    { nullptr };
    int          serumN_    { 0 };
    float        serumGain_ { 1.0f };

    // Visualizer ring buffer: written on audio thread (relaxed), read by GUI
    std::array<float, kVisBufSize> visBuf_         {};
    std::atomic<int>               visBufWriteIdx_ { 0 };

    // ── Mono-sub < 120 Hz (PA mono compatibility) ───────────────────────────
    struct MonoSubFilter {
        float alpha_ { 0.9844f };
        float zL_ { 0.f }, zR_ { 0.f }, zMid_ { 0.f };

        void prepare(double sr) noexcept
        {
            alpha_ = std::exp(-2.f * 3.14159265f * 120.f / static_cast<float>(sr));
            zL_ = zR_ = zMid_ = 0.f;
        }
        void process(float& L, float& R) noexcept
        {
            static constexpr float kDenorm = 1e-20f;
            const float c   = 1.f - alpha_;
            const float mid = (L + R) * 0.5f;
            zMid_ = c * mid + alpha_ * zMid_ + kDenorm;
            zL_   = c * L   + alpha_ * zL_   + kDenorm;
            zR_   = c * R   + alpha_ * zR_   + kDenorm;
            L = (L - zL_) + zMid_;
            R = (R - zR_) + zMid_;
        }
        void reset() noexcept { zL_ = zR_ = zMid_ = 0.f; }
    } monoSubFilter_;
};

} // namespace dsp
