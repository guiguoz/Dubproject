#pragma once

#include "IEffect.h"
#include "Harmonizer.h"

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// HarmonizerEffect — IEffect wrapper around the existing Harmonizer module.
//
// Parameters (indices 0-2):
//   0  voice0  Interval for voice 0 in semitones  [-12 .. +12]  default +3
//   1  voice1  Interval for voice 1 in semitones  [-12 .. +12]  default -5
//   2  mix     Dry/wet mix                         [0.0 .. 1.0]  default 0.5
// ─────────────────────────────────────────────────────────────────────────────
class HarmonizerEffect : public IEffect
{
public:
    EffectType type() const noexcept override { return EffectType::Harmonizer; }

    void prepare(double sampleRate, int maxBlockSize) noexcept override;
    void process(float* buf, int numSamples, float pitchHz) noexcept override;
    void reset()  noexcept override;

    int             paramCount()           const noexcept override { return kParamCount; }
    ParamDescriptor paramDescriptor(int i) const noexcept override;
    float           getParam(int i)        const noexcept override;
    void            setParam(int i, float v) noexcept override;

    int         presetCount()             const noexcept override;
    const char* presetName(int index)     const noexcept override;
    void        applyPreset(int index)          noexcept override;

    /// Direct access for the AI ScaleHarmonizer to update intervals.
    Harmonizer& harmonizer() noexcept { return harmonizer_; }

private:
    enum : int { kVoice0 = 0, kVoice1, kMix, kParamCount };

    Harmonizer harmonizer_;

    // Scratch buffer for harmonizer output (pre-allocated in prepare).
    static constexpr int kMaxBlock = 8192;
    std::array<float, kMaxBlock> scratchBuf_{};
};

} // namespace dsp
