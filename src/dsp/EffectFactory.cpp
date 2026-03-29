#include "EffectFactory.h"

#include "AutoPitchCorrect.h"
#include "DelayEffect.h"
#include "EnvelopeFilterEffect.h"
#include "FlangerEffect.h"
#include "HarmonizerEffect.h"
#include "OctaverEffect.h"
#include "PitchForkEffect.h"
#include "SlicerEffect.h"
#include "SynthEffect.h"
#include "TunerEffect.h"
#include "WhammyEffect.h"

// ReverbEffect requires JUCE — included conditionally.
// When building the test binary (no JUCE), Reverb is simply unavailable.
#ifdef JUCE_CORE_H_INCLUDED
  #include "ReverbEffect.h"
  #define HAS_REVERB 1
#else
  #define HAS_REVERB 0
#endif

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// effectTypeName
// ─────────────────────────────────────────────────────────────────────────────
const char* effectTypeName(EffectType t) noexcept
{
    switch (t)
    {
    case EffectType::Flanger:          return "Flanger";
    case EffectType::Harmonizer:       return "Harmonizer";
    case EffectType::Reverb:           return "Reverb";
    case EffectType::PitchFork:        return "PitchFork";
    case EffectType::EnvelopeFilter:   return "EnvelopeFilter";
    case EffectType::Delay:            return "Delay";
    case EffectType::Whammy:           return "Whammy";
    case EffectType::Octaver:          return "Octaver";
    case EffectType::Tuner:            return "Tuner";
    case EffectType::Slicer:           return "Slicer";
    case EffectType::AutoPitchCorrect: return "AutoPitchCorrect";
    case EffectType::Synth:            return "Synth";
    }
    return "Unknown";
}

// ─────────────────────────────────────────────────────────────────────────────
// createEffect — by string name
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<IEffect> createEffect(const std::string& typeName) noexcept
{
    if (typeName == "Flanger")          return std::make_unique<FlangerEffect>();
    if (typeName == "Harmonizer")       return std::make_unique<HarmonizerEffect>();
    if (typeName == "PitchFork")        return std::make_unique<PitchForkEffect>();
    if (typeName == "EnvelopeFilter")   return std::make_unique<EnvelopeFilterEffect>();
    if (typeName == "Delay")            return std::make_unique<DelayEffect>();
    if (typeName == "Whammy")           return std::make_unique<WhammyEffect>();
    if (typeName == "Octaver")          return std::make_unique<OctaverEffect>();
    if (typeName == "Tuner")            return std::make_unique<TunerEffect>();
    if (typeName == "Slicer")           return std::make_unique<SlicerEffect>();
    if (typeName == "AutoPitchCorrect") return std::make_unique<AutoPitchCorrect>();
    if (typeName == "Synth")            return std::make_unique<SynthEffect>();
#if HAS_REVERB
    if (typeName == "Reverb")           return std::make_unique<ReverbEffect>();
#endif
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// createEffect — by enum value
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<IEffect> createEffect(EffectType t) noexcept
{
    return createEffect(std::string(effectTypeName(t)));
}

} // namespace dsp
