#pragma once

#include <JuceHeader.h>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// SaxFXColours — Dark palette used throughout the UI
// ─────────────────────────────────────────────────────────────────────────────
namespace SaxFXColours
{
    // ── Surfaces ──────────────────────────────────────────────────────────────
    inline const juce::Colour background    { 0xFF0A0A0A };
    inline const juce::Colour cardBody      { 0xFF131314 };
    inline const juce::Colour cardBorder    { 0xFF1C1B1C };
    inline const juce::Colour knobTrack     { 0xFF353436 };
    inline const juce::Colour textPrimary   { 0xFFE5E2E3 };
    inline const juce::Colour textSecondary { 0xFF6B6E70 };

    // ── AI / branding ─────────────────────────────────────────────────────────
    inline const juce::Colour aiBadge      { 0xFF4CDFA8 };

    // ── VU meter gradient stops (bottom → top) ────────────────────────────────
    inline const juce::Colour vuLow  { 0xFF4CDFA8 };
    inline const juce::Colour vuMid  { 0xFFFFCC00 };
    inline const juce::Colour vuHigh { 0xFFFF3333 };

    // ── Additional surface / glow colours ────────────────────────────────────
    inline const juce::Colour bgInput   { 0xFF0D1117 };
    inline const juce::Colour bgHover   { 0xFF252530 };
    inline const juce::Colour neonCyan  { 0xFF22D3EE };
    inline const juce::Colour glowGreen { 0x804CDFA8 };
    inline const juce::Colour glowCyan  { 0x8022D3EE };

} // namespace SaxFXColours
} // namespace ui
