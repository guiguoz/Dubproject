#pragma once

#include <array>
#include <string>
#include <vector>

namespace project {

// ─────────────────────────────────────────────────────────────────────────────
// SampleConfig — one sampler slot
// ─────────────────────────────────────────────────────────────────────────────
struct SampleConfig
{
    std::string filePath;
    float gain    { 1.0f };
    bool  loop    { false };
    bool  oneShot { true };
    bool  muted   { false };   // v3
    int   gridDiv { 1 };       // v3 — GridDiv enum cast to int (legacy, unused in v4)
    bool  stepPattern[16] {};  // v4 — 16-step pattern for this slot
};

// ─────────────────────────────────────────────────────────────────────────────
// MidiMapping — one MIDI note → sampler slot binding
// ─────────────────────────────────────────────────────────────────────────────
struct MidiMapping
{
    int midiNote  { -1 };
    int slotIndex { -1 };
};

// ─────────────────────────────────────────────────────────────────────────────
// EffectSlotData — serialised state of one effect in the chain
//
// type      : string name matching dsp::effectTypeName()
// enabled   : IEffect::enabled flag
// params    : values in param-index order (IEffect::getParam(0..n-1))
// aiManaged : true when SmartMixEngine last set the params (shows ◆ badge)
// ─────────────────────────────────────────────────────────────────────────────
struct EffectSlotData
{
    std::string        type;
    bool               enabled   { true };
    std::vector<float> params;
    bool               aiManaged { false };  // v3
};

// ─────────────────────────────────────────────────────────────────────────────
// MusicContextData — master tempo + tonality detected from reference sample
// ─────────────────────────────────────────────────────────────────────────────
struct MusicContextData
{
    float bpm     { 0.f };   // 0 = not set
    int   keyRoot { -1 };    // -1 = unknown; 0=C … 11=B
    bool  isMajor { true };
    int   style   { 0 };     // SmartMixEngine::Style cast to int
};

// ─────────────────────────────────────────────────────────────────────────────
// SlotMixData — AI magic mix result for one sampler slot (v5)
// ─────────────────────────────────────────────────────────────────────────────
struct SlotMixData
{
    float gain    { 1.f };   // runtime gain multiplier
    float pan     { 0.f };   // pan [-1=L … +1=R]
    float width   { 0.f };   // stereo width [0, 1]
    float depth   { 0.f };   // spatial depth [0, 1]
    bool  applied { false }; // true = magic mix was active on this slot
};

// ─────────────────────────────────────────────────────────────────────────────
// SceneSaveData — one scene snapshot (v5)
// ─────────────────────────────────────────────────────────────────────────────
struct SceneSaveData
{
    float                                 bpm           { 120.f };
    std::array<std::string, 8>            filePaths     {};
    std::array<std::array<bool, 512>, 8>  steps         {};   // up to 32 bars × 16 steps
    std::array<float, 8>                  gains         { 1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f };
    std::array<bool, 8>                   mutes         {};
    std::array<int, 8>                    trackBarCounts{ 1,1,1,1,1,1,1,1 };  // v6
    std::array<int, 8>                    trimStart     { 0,0,0,0,0,0,0,0 };  // v7
    std::array<int, 8>                    trimEnd       { -1,-1,-1,-1,-1,-1,-1,-1 };  // v7 — -1 = pas de trim
    bool                                  used          { false };
};

// ─────────────────────────────────────────────────────────────────────────────
// ProjectData — full project snapshot (version 5 format)
// ─────────────────────────────────────────────────────────────────────────────
struct ProjectData
{
    int                         version     { 6 };
    std::string                 projectName { "Untitled" };
    float                       bpm         { 120.f };
    std::vector<EffectSlotData> effectChain;
    std::array<SampleConfig, 8> samples {};
    std::vector<MidiMapping>    midiMappings;
    MusicContextData            musicContext;
    // v5 — new fields
    int                              masterKeyRoot  { 0 };     // 0=C … 11=B
    bool                             masterKeyMajor { true };
    std::array<SlotMixData, 8>       slotMix        {};        // AI mix results
    std::array<SceneSaveData, 8>     scenes         {};        // up to 8 scenes
    int                              currentScene   { 0 };
};

} // namespace project
