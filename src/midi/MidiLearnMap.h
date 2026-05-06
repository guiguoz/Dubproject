#pragma once

namespace midi {

// Enumère les paramètres DSP/UI mappables via MIDI learn.
enum class MappingTarget {
    None = -1,
    MasterMix = 0,    // mainMixSlider_ / outputGain_
    DubDelaySend,     // dubDelaySendSlider_
    DubDelayWet,      // dubDelayWetSlider_
    DubDelayFeedback, // dubDelayFeedbackSlider_
    SerumGain,        // serumUserGain_ (multiplicateur sur le gain rider)
    Slot0Gain, Slot1Gain, Slot2Gain, Slot3Gain,
    Slot4Gain, Slot5Gain, Slot6Gain, Slot7Gain,
    Count
};

static constexpr int kNumTargets = static_cast<int>(MappingTarget::Count);

struct MidiLearnBinding {
    MappingTarget target { MappingTarget::None };
    int   cc  { -1 };   // -1 = non assigné
    float min { 0.f };
    float max { 1.f };
};

inline const char* mappingTargetName(MappingTarget t) noexcept
{
    switch (t)
    {
    case MappingTarget::MasterMix:        return "Master Mix";
    case MappingTarget::DubDelaySend:     return "Dub Send";
    case MappingTarget::DubDelayWet:      return "Dub Wet";
    case MappingTarget::DubDelayFeedback: return "Dub Feedback";
    case MappingTarget::SerumGain:        return "EWI Synth Gain";
    case MappingTarget::Slot0Gain:        return "Slot 1 Gain";
    case MappingTarget::Slot1Gain:        return "Slot 2 Gain";
    case MappingTarget::Slot2Gain:        return "Slot 3 Gain";
    case MappingTarget::Slot3Gain:        return "Slot 4 Gain";
    case MappingTarget::Slot4Gain:        return "Slot 5 Gain";
    case MappingTarget::Slot5Gain:        return "Slot 6 Gain";
    case MappingTarget::Slot6Gain:        return "Slot 7 Gain";
    case MappingTarget::Slot7Gain:        return "Slot 8 Gain";
    default:                              return "";
    }
}

} // namespace midi
