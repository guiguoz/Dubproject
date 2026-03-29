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
// ProjectData — full project snapshot (version 4 format)
// ─────────────────────────────────────────────────────────────────────────────
struct ProjectData
{
    int                         version     { 4 };
    std::string                 projectName { "Untitled" };
    float                       bpm         { 120.f };  // v4 — master sequencer BPM
    std::vector<EffectSlotData> effectChain;
    std::array<SampleConfig, 8> samples {};
    std::vector<MidiMapping>    midiMappings;
    MusicContextData            musicContext;  // v3
};

} // namespace project
