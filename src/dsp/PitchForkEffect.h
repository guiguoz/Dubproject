#pragma once

#include "IEffect.h"
#include "Harmonizer.h"  // for OlaShifter

#include <array>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// PitchForkEffect — single-voice pitch shifter using OlaShifter.
//
// Parameters (indices 0-1):
//   0  semitones  Shift amount in semitones  [-12 .. +12]  default 0
//   1  mix        Dry/wet mix                [0.0 .. 1.0]  default 0.5
// ─────────────────────────────────────────────────────────────────────────────
class PitchForkEffect : public IEffect
{
public:
    EffectType type() const noexcept override { return EffectType::PitchFork; }

    void prepare(double sampleRate, int maxBlockSize) noexcept override;
    void process(float* buf, int numSamples, float pitchHz) noexcept override;
    void reset()  noexcept override;

    int             paramCount()           const noexcept override { return kParamCount; }
    ParamDescriptor paramDescriptor(int i) const noexcept override;
    float           getParam(int i)        const noexcept override;
    void            setParam(int i, float v) noexcept override;

private:
    enum : int { kSemitones = 0, kMix, kParamCount };

    OlaShifter shifter_;
    std::atomic<float> semitones_ { 0.0f };
    std::atomic<float> mix_       { 0.5f };

    // Scratch buffer for shifted output (pre-allocated in prepare).
    static constexpr int kMaxBlock = 8192;
    std::array<float, kMaxBlock> shiftedBuf_{};
};

} // namespace dsp
