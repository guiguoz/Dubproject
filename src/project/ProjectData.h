#pragma once

#include <array>
#include <string>
#include <vector>

namespace project {

struct SampleConfig
{
    std::string filePath;
    float gain     { 1.0f };
    bool  loop     { false };
    bool  oneShot  { true };
};

struct MidiMapping
{
    int midiNote  { -1 };
    int slotIndex { -1 };
};

struct EffectParams
{
    bool  harmonizerEnabled  { false };
    float harmVoice0Interval { 3.0f };
    float harmVoice1Interval { -5.0f };
    float harmMix            { 0.5f };

    bool  flangerEnabled  { false };
    float flangerRate     { 0.5f };
    float flangerDepth    { 0.7f };
    float flangerFeedback { 0.3f };
    float flangerMix      { 0.5f };
};

struct ProjectData
{
    std::string                           projectName;
    std::array<SampleConfig, 8>           samples;
    std::vector<MidiMapping>              midiMappings;
    EffectParams                          effects;
};

} // namespace project
