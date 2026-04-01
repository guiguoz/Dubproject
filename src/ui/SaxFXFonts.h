#pragma once

#include <JuceHeader.h>

namespace ui::SaxFXFonts {

// ─────────────────────────────────────────────────────────────────────────────
// Font family resolvers — prefer modern fonts, fall back to system defaults
// ─────────────────────────────────────────────────────────────────────────────
inline juce::String getMonoFont()
{
    const auto names = juce::Font::findAllTypefaceNames();
    if (names.contains("JetBrains Mono")) return "JetBrains Mono";
    if (names.contains("Consolas"))       return "Consolas";
    if (names.contains("Courier New"))    return "Courier New";
    return juce::Font::getDefaultMonospacedFontName();
}

inline juce::String getSansFont()
{
    const auto names = juce::Font::findAllTypefaceNames();
    if (names.contains("Inter"))      return "Inter";
    if (names.contains("Segoe UI"))   return "Segoe UI";
    if (names.contains("Helvetica"))  return "Helvetica";
    return juce::Font::getDefaultSansSerifFontName();
}

// ─────────────────────────────────────────────────────────────────────────────
// Size scale (8-point modular)
// ⚠ NEVER use < 11.f for visible text, < 13.f for interactive elements
// ─────────────────────────────────────────────────────────────────────────────
constexpr float xxs  =  9.f;   // micro labels (non-interactive only)
constexpr float xs   = 11.f;   // captions, timestamps
constexpr float sm   = 13.f;   // body small — minimum for interactive text
constexpr float md   = 15.f;   // body regular — standard UI
constexpr float lg   = 18.f;   // subheadings
constexpr float xl   = 22.f;   // headings
constexpr float xxl  = 28.f;   // section titles
constexpr float huge = 36.f;   // app title

// ─────────────────────────────────────────────────────────────────────────────
// Font builders
// ─────────────────────────────────────────────────────────────────────────────
inline juce::Font mono(float size = md)
{
    return juce::Font(juce::FontOptions{}.withName(getMonoFont()).withHeight(size));
}

inline juce::Font sans(float size = md)
{
    return juce::Font(juce::FontOptions{}.withName(getSansFont()).withHeight(size));
}

inline juce::Font bold(float size = md)
{
    return juce::Font(juce::FontOptions{}.withName(getSansFont()).withHeight(size).withStyle("Bold"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Named presets — use these throughout the UI for consistency
// ─────────────────────────────────────────────────────────────────────────────
inline juce::Font dataDisplay()              { return mono(md);  }  // BPM, Hz, dB values
inline juce::Font slotName()                 { return sans(sm);  }  // sample file name in slot
inline juce::Font slotMeta()                 { return mono(xs);  }  // BPM/key metadata in slot
inline juce::Font buttonText(float h = 0.f)  { return bold(h > 0.f ? h : md); }
inline juce::Font sectionTitle()             { return bold(lg);  }
inline juce::Font appTitle()                 { return bold(huge);}

} // namespace ui::SaxFXFonts
