#pragma once

#include "Harmonizer.h" // for OlaShifter
#include "IEffect.h"

#include <array>
#include <atomic>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// AutoPitchCorrect
//
// Quantises the detected pitch to the nearest semitone (chromatic correction).
// Uses OlaShifter to apply the correction shift, mixed with the dry signal.
//
// Parameters:
//   0  strength  Correction amount  [0.0 .. 1.0]  default 1.0
//   1  refHz     Reference A4 freq  [415 .. 466]   default 440.0
// ─────────────────────────────────────────────────────────────────────────────
class AutoPitchCorrect : public IEffect
{
  public:
    EffectType type() const noexcept override
    {
        return EffectType::AutoPitchCorrect;
    }

    void prepare(double sampleRate, int maxBlockSize) noexcept override;
    void process(float* buf, int numSamples, float pitchHz) noexcept override;
    void reset() noexcept override;

    int             paramCount()           const noexcept override { return kParamCount; }
    ParamDescriptor paramDescriptor(int i) const noexcept override;
    float           getParam(int i)        const noexcept override;
    void            setParam(int i, float v) noexcept override;

  private:
    enum : int { kStrength = 0, kRefHz, kParamCount };

    OlaShifter         shifter_;
    std::atomic<float> strength_{ 1.0f  };
    std::atomic<float> refHz_   { 440.0f};

    static constexpr int kMaxBlock = 8192;
    std::array<float, kMaxBlock> shifted_{};
};

} // namespace dsp
