#pragma once

#include "dsp/IEffect.h"

#include <array>
#include <span>

// ─────────────────────────────────────────────────────────────────────────────
// PresetLibrary
//
// Compile-time preset table for all effect types.
// Presets are tailored for live saxophone across multiple styles
// (jazz, funk, pop, ambient, rock, electro — not only dub/techno).
//
// Parameter values are research-backed from: Sound on Sound, iZotope,
// Sweetwater, HornFX, BenVesco Mix Recipes, Sax on the Web.
//
// Param indices match the constexpr arrays in each *Effect.cpp file:
//   Reverb      : roomSize, damping, width, mix
//   Delay       : timeMs, feedback, mix
//   Flanger     : rate, depth, feedback, mix
//   Harmonizer  : voice0, voice1, mix
//   EnvFilter   : sensitivity, attack_ms, release_ms, resonance, mix
//   Octaver     : oct1, oct2, dry
//   PitchFork   : semitones, mix
//   Whammy      : expression, toePitch, heelPitch, mix
//   AutoPitch   : strength, refHz
//   Slicer      : rateHz, depth
//   Tuner       : mute
// ─────────────────────────────────────────────────────────────────────────────

struct EffectPreset
{
    const char* name;
    float       params[8];  // up to 8 params; unused slots are 0
};

namespace PresetLibrary {

// ── Reverb ───────────────────────────────────────────────────────────────────
// (roomSize, damping, width, mix)
// Research: pre-delay 20-30ms is ideal but not a param → use mix/roomSize proxy
inline constexpr EffectPreset kReverb[] = {
    { "Concert Hall",   { 0.70f, 0.40f, 0.85f, 0.22f } },  // live jazz standard
    { "Small Room",     { 0.30f, 0.55f, 0.65f, 0.15f } },  // intimate jazz club
    { "Cathedral",      { 0.92f, 0.20f, 1.00f, 0.38f } },  // ambient, long tail
    { "Studio Dry",     { 0.18f, 0.65f, 0.60f, 0.08f } },  // nearly dry, presence only
    { "Spring Tank",    { 0.50f, 0.75f, 0.70f, 0.28f } },  // vintage rétro
    { "Stairwell",      { 0.42f, 0.30f, 0.55f, 0.18f } },  // dense short reflections
};

// ── Delay ────────────────────────────────────────────────────────────────────
// (timeMs, feedback, mix)
inline constexpr EffectPreset kDelay[] = {
    { "Slapback",       {  80.0f, 0.05f, 0.20f } },  // 80ms, 1 repeat — jazz percussive
    { "Jazz Echo",      { 350.0f, 0.30f, 0.20f } },  // free echo, not BPM-locked
    { "Dotted 8th",     { 375.0f, 0.40f, 0.25f } },  // ~120 BPM dotted 8th (set BPM-sync in IA mode)
    { "Tape Echo",      { 300.0f, 0.45f, 0.22f } },  // warm, soft repeats
    { "Ambient Long",   { 700.0f, 0.55f, 0.28f } },  // long, dissolving
};

// ── Flanger ──────────────────────────────────────────────────────────────────
// (rate Hz, depth, feedback, mix)
inline constexpr EffectPreset kFlanger[] = {
    { "Slow Jet",       { 0.30f, 0.40f, 0.40f, 0.30f } },  // smooth, natural
    { "Chorus Sax",     { 0.80f, 0.30f, 0.20f, 0.35f } },  // light, thickening
    { "Fast Sweep",     { 2.00f, 0.55f, 0.50f, 0.35f } },  // energetic
    { "Vibrato",        { 1.20f, 0.70f, 0.10f, 0.40f } },  // deep modulation
    { "Metal Jet",      { 3.00f, 0.80f, 0.65f, 0.40f } },  // intense, dramatic
};

// ── Harmonizer ───────────────────────────────────────────────────────────────
// (voice0 semitones, voice1 semitones, mix)
inline constexpr EffectPreset kHarmonizer[] = {
    { "Jazz Tierce",    {  4.0f,  0.0f, 0.45f } },   // major 3rd (+4st)
    { "Quinte",         {  7.0f,  0.0f, 0.45f } },   // perfect 5th (+7st)
    { "Octave",         { 12.0f,  0.0f, 0.45f } },   // octave up
    { "Tierce + Quinte",{  4.0f,  7.0f, 0.38f } },   // 2-voice harmony
    { "Choir",          {  3.0f,  7.0f, 0.42f } },   // +3/+7 (minor 3rd + 5th)
    { "Funk Crunch",    {  3.0f, 10.0f, 0.35f } },   // +3/+10 (minor 3rd + minor 7th)
    { "Unisson Détune", {  0.5f, -0.5f, 0.40f } },   // subtle detune chorus effect
};

// ── EnvelopeFilter ────────────────────────────────────────────────────────────
// (sensitivity, attack_ms, release_ms, resonance, mix)
// Research: Mu-Tron III, MXR Two-Knob — standard funk sax reference
inline constexpr EffectPreset kEnvFilter[] = {
    { "Funky",          { 3.0f,  8.0f,  50.0f, 5.0f, 0.70f } },  // fast attack, tight
    { "Slow Wah",       { 1.5f, 20.0f, 120.0f, 2.5f, 0.65f } },  // smooth, slow
    { "Jazz Warmth",    { 1.0f, 15.0f, 200.0f, 2.0f, 0.55f } },  // subtle LP warmth
    { "Heavy Funk",     { 4.0f,  5.0f,  30.0f, 7.0f, 0.80f } },  // aggressive quack
    { "Synth Quack",    { 5.0f,  3.0f,  25.0f, 8.0f, 0.85f } },  // electro, high Q
};

// ── Octaver ──────────────────────────────────────────────────────────────────
// (oct1 = -1 oct, oct2 = -2 oct, dry)
inline constexpr EffectPreset kOctaver[] = {
    { "Sub Only",       { 0.65f, 0.00f, 0.70f } },  // one sub octave
    { "Organ",          { 0.45f, 0.00f, 0.70f } },  // sub + dry (organ feel)
    { "Bass Sax",       { 0.80f, 0.00f, 0.60f } },  // heavy sub, reduced dry
    { "Full Octave",    { 0.45f, 0.20f, 0.65f } },  // dry + sub + 2-oct sub
    { "Sub Blend",      { 0.40f, 0.00f, 0.80f } },  // subtle sub reinforcement
};

// ── PitchFork ────────────────────────────────────────────────────────────────
// (semitones, mix)
inline constexpr EffectPreset kPitchFork[] = {
    { "Octave Up",      { 12.0f, 0.45f } },
    { "Quinte",         {  7.0f, 0.45f } },
    { "Tierce Basse",   { -3.0f, 0.45f } },
    { "Détune Subtle",  {  0.3f, 0.50f } },  // +30 cents subtle chorus
    { "Détune Thick",   { -0.4f, 0.50f } },  // -40 cents (layer with +30c via 2nd inst)
};

// ── Whammy ───────────────────────────────────────────────────────────────────
// (expression, toePitch, heelPitch, mix)
inline constexpr EffectPreset kWhammy[] = {
    { "Dive Bomb",      { 0.0f, -24.0f,  0.0f, 0.85f } },
    { "Octave Up",      { 0.0f,  12.0f,  0.0f, 0.80f } },
    { "Half Step Up",   { 0.0f,   1.0f,  0.0f, 0.90f } },
    { "Power Shift",    { 0.0f,   5.0f,  0.0f, 0.80f } },  // +5st glide
    { "Subtle Bend",    { 0.0f,   2.0f,  0.0f, 0.75f } },
};

// ── AutoPitchCorrect ─────────────────────────────────────────────────────────
// (strength, refHz)
// Research: retune speed 30-50ms = natural for sax; full strength only in studio
inline constexpr EffectPreset kAutoPitch[] = {
    { "Studio Tight",   { 1.00f, 440.0f } },  // max correction, studio use
    { "Live Natural",   { 0.50f, 440.0f } },  // gentle, preserves vibrato
    { "Chromatic Free", { 0.30f, 440.0f } },  // minimal — safety net only
    { "Baroque 415",    { 0.50f, 415.0f } },  // early music A=415
};

// ── Slicer ───────────────────────────────────────────────────────────────────
// (rateHz, depth)
// rateHz at 120 BPM: 16th=8Hz, 8th=4Hz, quarter=2Hz
inline constexpr EffectPreset kSlicer[] = {
    { "16th Gate",      { 8.0f, 1.00f } },
    { "8th Groove",     { 4.0f, 0.90f } },
    { "Triolet",        { 6.0f, 0.95f } },   // triplet feel
    { "Half-time",      { 2.0f, 0.85f } },
    { "Syncopé",        { 5.0f, 1.00f } },   // syncopated feel
    { "Silent Pause",   { 1.0f, 1.00f } },   // dramatic quarter-note gate
};

// ── Tuner ────────────────────────────────────────────────────────────────────
// (mute)
inline constexpr EffectPreset kTuner[] = {
    { "Mute",           { 1.0f } },
    { "Bypass",         { 0.0f } },
};

// ─────────────────────────────────────────────────────────────────────────────
// forType — returns the preset span for a given effect type
// ─────────────────────────────────────────────────────────────────────────────
inline std::span<const EffectPreset> forType(dsp::EffectType t) noexcept
{
    switch (t)
    {
    case dsp::EffectType::Reverb:           return kReverb;
    case dsp::EffectType::Delay:            return kDelay;
    case dsp::EffectType::Flanger:          return kFlanger;
    case dsp::EffectType::Harmonizer:       return kHarmonizer;
    case dsp::EffectType::EnvelopeFilter:   return kEnvFilter;
    case dsp::EffectType::Octaver:          return kOctaver;
    case dsp::EffectType::PitchFork:        return kPitchFork;
    case dsp::EffectType::Whammy:           return kWhammy;
    case dsp::EffectType::AutoPitchCorrect: return kAutoPitch;
    case dsp::EffectType::Slicer:           return kSlicer;
    case dsp::EffectType::Tuner:            return kTuner;
    case dsp::EffectType::Synth:            return {};  // Synth uses built-in presets
    }
    return {};
}

} // namespace PresetLibrary
