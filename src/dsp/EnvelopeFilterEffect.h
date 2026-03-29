#pragma once

#include "IEffect.h"

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// EnvelopeFilterEffect — envelope follower driving a 2-pole state-variable
// low-pass filter (pure C++ Simper/cytomic SVF topology).
//
// Parameters (indices 0-4):
//   0  sensitivity  Envelope gain multiplier  [0.1 .. 10.0]  default 1.0
//   1  attack_ms    Attack  time in ms        [1.0 .. 200.0]  default 5.0
//   2  release_ms   Release time in ms        [10  .. 2000]   default 200
//   3  resonance    Filter Q factor           [0.5 .. 10.0]   default 1.0
//   4  mix          Dry/wet mix               [0.0 ..  1.0]   default 0.7
// ─────────────────────────────────────────────────────────────────────────────
class EnvelopeFilterEffect : public IEffect
{
public:
    EffectType type() const noexcept override { return EffectType::EnvelopeFilter; }

    void prepare(double sampleRate, int maxBlockSize) noexcept override;
    void process(float* buf, int numSamples, float pitchHz) noexcept override;
    void reset()  noexcept override;

    int             paramCount()           const noexcept override { return kParamCount; }
    ParamDescriptor paramDescriptor(int i) const noexcept override;
    float           getParam(int i)        const noexcept override;
    void            setParam(int i, float v) noexcept override;

    // ── Presets ─────────────────────────────────────────────────────────────
    int         presetCount()             const noexcept override;
    const char* presetName(int index)     const noexcept override;
    void        applyPreset(int index)          noexcept override;

private:
    enum : int { kSensitivity = 0, kAttack, kRelease, kResonance, kMix, kParamCount };

    double sampleRate_   { 44100.0 };

    // Parameters (written from GUI thread, read from audio thread)
    std::atomic<float> sensitivity_   { 1.0f   };
    std::atomic<float> attackMs_      { 5.0f   };
    std::atomic<float> releaseMs_     { 200.0f };
    std::atomic<float> resonance_     { 1.0f   };
    std::atomic<float> mix_           { 0.7f   };

    // Envelope follower state (audio thread only)
    float envLevel_      { 0.0f };
    float attackCoeff_   { 0.0f };
    float releaseCoeff_  { 0.0f };

    // SVF state (audio thread only)
    float svfIc1_        { 0.0f };
    float svfIc2_        { 0.0f };

    // Recompute filter coefficients from current parameters.
    void updateCoefficients() noexcept;
};

} // namespace dsp
