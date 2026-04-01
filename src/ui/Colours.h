#pragma once

#include "dsp/IEffect.h"

#include <JuceHeader.h>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// SaxFXColours — Dark palette + per-effect accent colours
//
// Global background/surface colours + one accent colour per effect type.
// Accent colours follow the Nuro Audio Blockchain Ultra visual style:
// vivid, saturated hues on a near-black background.
// ─────────────────────────────────────────────────────────────────────────────
namespace SaxFXColours
{
    // ── Surfaces ──────────────────────────────────────────────────────────────
    inline const juce::Colour background    { 0xFF0A0A0A }; // global bg
    inline const juce::Colour cardBody      { 0xFF131314 }; // rack-unit body / header
    inline const juce::Colour cardBorder    { 0xFF1C1B1C }; // outline
    inline const juce::Colour knobTrack     { 0xFF353436 }; // track background
    inline const juce::Colour textPrimary   { 0xFFE5E2E3 }; // labels / values
    inline const juce::Colour textSecondary { 0xFF6B6E70 }; // hex roughly equivalent to e5e2e3 at 40%

    // ── Per-effect accent colours ───────────────────────────
    inline const juce::Colour harmonizerAccent    { 0xFFA78BFA }; // violet-400
    inline const juce::Colour flangerAccent       { 0xFF22D3EE }; // cyan-400
    inline const juce::Colour reverbAccent        { 0xFF60A5FA }; // blue-400
    inline const juce::Colour delayAccent         { 0xFFFB923C }; // orange-400
    inline const juce::Colour pitchForkAccent     { 0xFF818CF8 }; // indigo-400
    inline const juce::Colour whammyAccent        { 0xFFF87171 }; // red-400
    inline const juce::Colour octaverAccent       { 0xFF4CDFA8 }; // primary
    inline const juce::Colour envFilterAccent     { 0xFFF472B6 }; // pink-400
    inline const juce::Colour slicerAccent        { 0xFFFACC15 }; // yellow-400
    inline const juce::Colour autoPitchAccent     { 0xFF38BDF8 }; // sky-400
    inline const juce::Colour tunerAccent         { 0xFF2DD4BF }; // teal-400
    inline const juce::Colour synthAccent         { 0xFFE879F9 }; // fuchsia-400

    /// AI-managed badge colour (primary brand color)
    inline const juce::Colour aiBadge            { 0xFF4CDFA8 };

    // VU meter gradient stops (bottom → top)
    inline const juce::Colour vuLow  { 0xFF4CDFA8 }; // primary
    inline const juce::Colour vuMid  { 0xFFFFCC00 }; // yellow
    inline const juce::Colour vuHigh { 0xFFFF3333 }; // red

    // ─────────────────────────────────────────────────────────────────────────
    /// Returns the accent colour for a given effect type.
    // ─────────────────────────────────────────────────────────────────────────
    inline juce::Colour forEffectType(::dsp::EffectType t) noexcept
    {
        switch (t)
        {
            case ::dsp::EffectType::Harmonizer:      return harmonizerAccent;
            case ::dsp::EffectType::Flanger:         return flangerAccent;
            case ::dsp::EffectType::Reverb:          return reverbAccent;
            case ::dsp::EffectType::Delay:           return delayAccent;
            case ::dsp::EffectType::PitchFork:       return pitchForkAccent;
            case ::dsp::EffectType::Whammy:          return whammyAccent;
            case ::dsp::EffectType::Octaver:         return octaverAccent;
            case ::dsp::EffectType::EnvelopeFilter:  return envFilterAccent;
            case ::dsp::EffectType::Slicer:          return slicerAccent;
            case ::dsp::EffectType::AutoPitchCorrect: return autoPitchAccent;
            case ::dsp::EffectType::Tuner:           return tunerAccent;
            case ::dsp::EffectType::Synth:           return synthAccent;
            default:                                  return textSecondary;
        }
    }

    // ── Additional surface / glow colours ────────────────────────────────────
    inline const juce::Colour bgInput   { 0xFF0D1117 };  // waveform / input backgrounds
    inline const juce::Colour bgHover   { 0xFF252530 };  // hover state surface
    inline const juce::Colour neonCyan  { 0xFF22D3EE };  // = flangerAccent (cyan-400)
    inline const juce::Colour glowGreen { 0x804CDFA8 };  // aiBadge at 50% alpha
    inline const juce::Colour glowCyan  { 0x8022D3EE };  // neonCyan  at 50% alpha

} // namespace SaxFXColours
} // namespace ui
