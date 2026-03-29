#pragma once

#include "Harmonizer.h"
#include "IEffect.h"

#include <array>


namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// OctaverEffect
//
// Dual octave generation (-1 and -2 octaves).
//
// Parameters:
//   0  oct1  Mix of -1 octave      [0.0 .. 1.0]  default 0.7
//   1  oct2  Mix of -2 octave      [0.0 .. 1.0]  default 0.3
//   2  dry   Mix of dry signal     [0.0 .. 1.0]  default 1.0
// ─────────────────────────────────────────────────────────────────────────────
class OctaverEffect : public IEffect
{
  public:
    EffectType type() const noexcept override
    {
        return EffectType::Octaver;
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
        kOct1 = 0,
        kOct2,
        kDry,
        kParamCount
    };

    OlaShifter shifter1_; // -1 octave (-12 semitones)
    OlaShifter shifter2_; // -2 octave (-24 semitones)

    std::atomic<float> oct1_{0.7f};
    std::atomic<float> oct2_{0.3f};
    std::atomic<float> dry_{1.0f};

    static constexpr int kMaxBlock = 8192;
    std::array<float, kMaxBlock> buf1_{};
    std::array<float, kMaxBlock> buf2_{};
};

} // namespace dsp
