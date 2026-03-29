#pragma once

#include "Harmonizer.h" // for OlaShifter
#include "IEffect.h"

#include <array>


namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// WhammyEffect — variable pitch shifter driven by an expression pedal.
//
// Parameters:
//   0  expression  Pedal position        [0.0 .. 1.0]   default 0.0
//   1  toePitch    Max shift (semitones) [-24.0..24.0]  default 12.0
//   2  heelPitch   Min shift (semitones) [-24.0..24.0]  default 0.0
//   3  mix         Dry/wet mix           [0.0 .. 1.0]   default 1.0
// ─────────────────────────────────────────────────────────────────────────────
class WhammyEffect : public IEffect
{
  public:
    EffectType type() const noexcept override
    {
        return EffectType::Whammy;
    }

    void prepare(double sampleRate, int maxBlockSize) noexcept override;
    void process(float* buf, int numSamples, float pitchHz) noexcept override;
    void reset() noexcept override;

    int paramCount() const noexcept override
    {
        return kParamCount;
    }
    ParamDescriptor paramDescriptor(int i) const noexcept override;
    float getParam(int i) const noexcept override;
    void setParam(int i, float v) noexcept override;

  private:
    enum : int
    {
        kExpression = 0,
        kToePitch,
        kHeelPitch,
        kMix,
        kParamCount
    };

    OlaShifter shifter_;
    std::atomic<float> expression_{0.0f};
    std::atomic<float> toePitch_{12.0f};
    std::atomic<float> heelPitch_{0.0f};
    std::atomic<float> mix_{1.0f};

    static constexpr int kMaxBlock = 8192;
    std::array<float, kMaxBlock> shiftedBuf_{};
};

} // namespace dsp
