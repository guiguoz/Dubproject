#pragma once

#include "IEffect.h"
#include "RingBuffer.h"

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// DelayEffect
//
// Parameters:
//   0  time      Delay time in ms          [1.0 .. 2000.0]  default 500.0
//   1  feedback  Delay feedback            [0.0 .. 0.95]    default 0.3
//   2  mix       Dry/wet mix               [0.0 .. 1.0]     default 0.5
//   3  division  Tempo sync note division  [0 .. 7]         default 3 (1/4)
//                  0=1/32  1=1/16  2=1/8  3=1/4  4=1/2  5=1bar  6=3/16(dot8)  7=3/8(dot4)
//
// When tempo sync is active (syncBpm > 0), the delay time is computed from
// BPM and the selected note division, overriding the manual time knob.
// ─────────────────────────────────────────────────────────────────────────────
class DelayEffect : public IEffect
{
  public:
    EffectType type() const noexcept override { return EffectType::Delay; }

    void prepare(double sampleRate, int maxBlockSize) noexcept override;
    void process(float* buf, int numSamples, float pitchHz) noexcept override;
    void reset() noexcept override;

    int             paramCount() const noexcept override { return kParamCount; }
    ParamDescriptor paramDescriptor(int i) const noexcept override;
    float           getParam(int i) const noexcept override;
    void            setParam(int i, float v) noexcept override;

    // ── Tempo sync ──────────────────────────────────────────────────────────
    /// Set the BPM for tempo-synced delay. 0 = sync off (use manual time).
    void  setSyncBpm(float bpm) noexcept { syncBpm_.store(bpm, std::memory_order_relaxed); }
    float getSyncBpm() const noexcept    { return syncBpm_.load(std::memory_order_relaxed); }
    bool  isSynced()   const noexcept    { return getSyncBpm() > 0.f; }

    /// Convert a division index to delay time in ms at a given BPM.
    static float divisionToMs(int div, float bpm) noexcept;

    // ── Presets ──────────────────────────────────────────────────────────────
    int         presetCount()             const noexcept override;
    const char* presetName(int index)     const noexcept override;
    void        applyPreset(int index)          noexcept override;

  private:
    enum : int
    {
        kTime = 0,
        kFeedback,
        kMix,
        kDivision,
        kParamCount
    };

    static constexpr std::size_t kMaxDelayBuffer = 192000; // ~4s at 48kHz
    RingBuffer<kMaxDelayBuffer> delayBuffer_;
    double sampleRate_ { 44100.0 };

    std::atomic<float> time_     { 500.0f };
    std::atomic<float> feedback_ { 0.3f };
    std::atomic<float> mix_      { 0.5f };
    std::atomic<float> division_ { 3.0f };  // 1/4 note

    // Tempo sync BPM (0 = disabled)
    std::atomic<float> syncBpm_  { 0.0f };
};

} // namespace dsp
