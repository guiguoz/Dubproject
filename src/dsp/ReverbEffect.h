#pragma once

#include "IEffect.h"

#include <memory>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// ReverbEffect — IEffect wrapper around juce::dsp::Reverb.
// Uses pimpl so the header has no JUCE dependency (keeps tests buildable).
//
// Parameters (indices 0-3):
//   0  roomSize   Room size            [0.0 .. 1.0]  default 0.5
//   1  damping    Damping factor       [0.0 .. 1.0]  default 0.5
//   2  width      Stereo width         [0.0 .. 1.0]  default 1.0
//   3  mix        Dry/wet mix          [0.0 .. 1.0]  default 0.33
// ─────────────────────────────────────────────────────────────────────────────
class ReverbEffect : public IEffect
{
public:
    ReverbEffect();
    ~ReverbEffect() override;

    EffectType type() const noexcept override { return EffectType::Reverb; }

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
    enum : int { kRoomSize = 0, kDamping, kWidth, kMix, kParamCount };

    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace dsp
