#include "SaxOsLookAndFeel.h"

namespace ui {

SaxOsLookAndFeel::SaxOsLookAndFeel()
{
    // ── Global colours (dark neon base) ──────────────────────────────────────
    setColour(juce::ResizableWindow::backgroundColourId,        juce::Colour(0xFF0A0A0A));
    setColour(juce::Label::textColourId,                        juce::Colour(0xFFE5E2E3));
    setColour(juce::Slider::thumbColourId,                      accent_);
    setColour(juce::Slider::rotarySliderFillColourId,           accent_);
    setColour(juce::TextButton::textColourOffId,                juce::Colour(0xFFE5E2E3));
    setColour(juce::TextButton::buttonColourId,                 juce::Colour(0xFF1C1B1C));
    setColour(juce::TextButton::buttonOnColourId,               juce::Colour(0xFF252525));
    setColour(juce::PopupMenu::backgroundColourId,              juce::Colour(0xFF131314));
    setColour(juce::PopupMenu::textColourId,                    juce::Colour(0xFFE5E2E3));
    setColour(juce::PopupMenu::highlightedBackgroundColourId,   juce::Colour(0xFF353436));
}

// ─────────────────────────────────────────────────────────────────────────────
// Knob — neon circle with gradient ring + pointer dot
// ─────────────────────────────────────────────────────────────────────────────

void SaxOsLookAndFeel::drawRotarySlider(juce::Graphics& g,
                                        int x, int y, int width, int height,
                                        float sliderPos,
                                        float rotaryStartAngle,
                                        float rotaryEndAngle,
                                        juce::Slider& slider)
{
    juce::ignoreUnused(slider);

    const float radius    = juce::jmin(static_cast<float>(width),
                                       static_cast<float>(height)) * 0.42f;
    const float centreX   = static_cast<float>(x) + static_cast<float>(width)  * 0.5f;
    const float centreY   = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const float angle     = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // ── Soft ambient glow behind knob (HTML: blur-xl bg-accent/10) ───────
    g.setColour(accent_.withAlpha(0.08f));
    g.fillEllipse(centreX - radius * 1.5f, centreY - radius * 1.5f,
                  radius * 3.0f, radius * 3.0f);

    // ── Background circle (gradient 145deg matching HTML knob-outer) ─────
    {
        juce::ColourGradient bg(juce::Colour(0xFF353436), centreX - radius, centreY - radius,
                                juce::Colour(0xFF1C1B1C), centreX + radius, centreY + radius, false);
        g.setGradientFill(bg);
        g.fillEllipse(centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f);
    }

    // ── Inset highlight (HTML: inset 0 1px rgba(255,255,255,0.05)) ───────
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.drawEllipse(centreX - radius + 2.f, centreY - radius + 1.f,
                  (radius - 2.f) * 2.0f, (radius - 1.f) * 2.0f, 1.0f);

    // ── Track arc (dark ring matching HTML border-[6px] border-[#1c1b1c]) ─
    {
        juce::Path track;
        track.addCentredArc(centreX, centreY, radius * 0.78f, radius * 0.78f,
                            0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xFF1C1B1C));
        g.strokePath(track, juce::PathStrokeType(radius * 0.12f,
                     juce::PathStrokeType::curved,
                     juce::PathStrokeType::rounded));
    }

    // ── Value arc (accent neon, thicker matching HTML 6px border) ─────────
    {
        juce::Path arc;
        arc.addCentredArc(centreX, centreY, radius * 0.78f, radius * 0.78f,
                          0.0f, rotaryStartAngle, angle, true);

        // Soft glow pass
        g.setColour(accent_.withAlpha(0.18f));
        g.strokePath(arc, juce::PathStrokeType(radius * 0.24f,
                     juce::PathStrokeType::curved,
                     juce::PathStrokeType::rounded));

        // Core arc
        juce::ColourGradient ag(accent_.withAlpha(0.95f), centreX, centreY - radius,
                                accent_.darker(0.4f), centreX, centreY + radius, false);
        g.setGradientFill(ag);
        g.strokePath(arc, juce::PathStrokeType(radius * 0.12f,
                     juce::PathStrokeType::curved,
                     juce::PathStrokeType::rounded));
    }

    // ── Pointer dot (white, at current angle) ────────────────────────────
    {
        const float dotR = radius * 0.12f;
        const float dotX = centreX + std::cos(angle) * (radius * 0.55f);
        const float dotY = centreY + std::sin(angle) * (radius * 0.55f);

        // Glow behind dot
        g.setColour(accent_.withAlpha(0.35f));
        g.fillEllipse(dotX - dotR * 2.0f, dotY - dotR * 2.0f,
                      dotR * 4.0f, dotR * 4.0f);

        g.setColour(juce::Colours::white);
        g.fillEllipse(dotX - dotR, dotY - dotR, dotR * 2.0f, dotR * 2.0f);
    }

    // ── Outer ring ───────────────────────────────────────────────────────
    g.setColour(juce::Colour(0xFF353436));
    g.drawEllipse(centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f, 1.5f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Button — dark body + neon hover/press + rounded corners
// ─────────────────────────────────────────────────────────────────────────────

void SaxOsLookAndFeel::drawButtonBackground(juce::Graphics& g,
                                             juce::Button& button,
                                             const juce::Colour& backgroundColour,
                                             bool isHighlighted,
                                             bool isButtonDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(1.5f);
    const float cr = 6.0f; // corner radius — was 2.0f, now more modern

    // ── Ambient glow (always on, very subtle) ────────────────────────────
    g.setColour(accent_.withAlpha(0.04f));
    g.fillRoundedRectangle(bounds.expanded(4.f), cr + 3.f);

    // ── Hover / pressed glow (multi-pass) ────────────────────────────────
    if (isButtonDown)
    {
        g.setColour(accent_.withAlpha(0.30f));
        g.fillRoundedRectangle(bounds.expanded(4.f), cr + 3.f);
        g.setColour(accent_.withAlpha(0.45f));
        g.fillRoundedRectangle(bounds.expanded(2.f), cr + 1.f);
    }
    else if (isHighlighted)
    {
        g.setColour(accent_.withAlpha(0.10f));
        g.fillRoundedRectangle(bounds.expanded(4.f), cr + 3.f);
        g.setColour(accent_.withAlpha(0.20f));
        g.fillRoundedRectangle(bounds.expanded(2.f), cr + 1.f);
        g.setColour(accent_.withAlpha(0.30f));
        g.fillRoundedRectangle(bounds.expanded(1.f), cr);
    }

    // ── Dark body ────────────────────────────────────────────────────────
    if (isButtonDown)
        g.setColour(backgroundColour.interpolatedWith(accent_.withAlpha(0.12f), 0.5f));
    else
        g.setColour(backgroundColour);
    g.fillRoundedRectangle(bounds, cr);

    // ── Neon border ───────────────────────────────────────────────────────
    if (isButtonDown)
        g.setColour(accent_.withAlpha(0.90f));
    else if (isHighlighted)
        g.setColour(accent_.withAlpha(0.70f));
    else
        g.setColour(accent_.withAlpha(0.25f));

    g.drawRoundedRectangle(bounds, cr,
                           isButtonDown ? 2.0f : (isHighlighted ? 1.5f : 1.0f));
}

void SaxOsLookAndFeel::drawButtonText(juce::Graphics& g,
                                       juce::TextButton& button,
                                       bool isHighlighted,
                                       bool isButtonDown)
{
    juce::ignoreUnused(isHighlighted);
    auto bounds = button.getLocalBounds();
    auto text   = button.getButtonText();

    g.setColour(isButtonDown ? accent_.brighter(0.2f)
                             : button.findColour(juce::TextButton::textColourOffId));
    const float fh = std::max(9.f, static_cast<float>(bounds.getHeight()) * 0.42f);
    g.setFont(juce::Font(juce::FontOptions{}.withHeight(fh).withStyle("Bold")));
    g.drawFittedText(text, bounds, juce::Justification::centred, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Toggle button — round checkbox, neon-green ON
// ─────────────────────────────────────────────────────────────────────────────

void SaxOsLookAndFeel::drawToggleButton(juce::Graphics& g,
                                         juce::ToggleButton& button,
                                         bool isMouseOverButton,
                                         bool isButtonDown)
{
    juce::ignoreUnused(isMouseOverButton, isButtonDown);

    const bool isOn = button.getToggleState();
    const float size = juce::jmin(static_cast<float>(button.getHeight()),
                                  static_cast<float>(button.getWidth())) * 0.55f;
    const float cx = static_cast<float>(button.getWidth())  * 0.5f;
    const float cy = static_cast<float>(button.getHeight()) * 0.5f;
    const float r  = size * 0.5f;

    // Track ring
    g.setColour(juce::Colour(0xFF353436));
    g.drawEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f, 2.0f);

    if (isOn)
    {
        // Neon fill circle
        g.setColour(accent_);
        g.fillEllipse(cx - r * 0.65f, cy - r * 0.65f, r * 1.3f, r * 1.3f);

        // Glow
        g.setColour(accent_.withAlpha(0.3f));
        g.fillEllipse(cx - r * 1.2f, cy - r * 1.2f, r * 2.4f, r * 2.4f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Popup menus — dark, rounded, neon tick
// ─────────────────────────────────────────────────────────────────────────────

void SaxOsLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    auto bounds = juce::Rectangle<float>(0, 0, static_cast<float>(width),
                                                static_cast<float>(height));
    g.setColour(juce::Colour(0xFF131314));
    g.fillRoundedRectangle(bounds.reduced(1.0f), 4.0f);
    g.setColour(juce::Colour(0xFF353436));
    g.drawRoundedRectangle(bounds.reduced(1.0f), 4.0f, 1.0f);
}

void SaxOsLookAndFeel::drawPopupMenuItem(juce::Graphics& g,
                                          const juce::Rectangle<int>& area,
                                          bool isSeparator,
                                          bool isActive,
                                          bool isHighlighted,
                                          bool isTicked,
                                          bool hasSubMenu,
                                          const juce::String& text,
                                          const juce::String& shortcutKeyText,
                                          const juce::Drawable* icon,
                                          const juce::Colour* textColour)
{
    juce::ignoreUnused(hasSubMenu, shortcutKeyText, icon);

    if (isSeparator)
    {
        g.setColour(juce::Colour(0xFF353436));
        g.fillRect(area.reduced(6, 1).withY(area.getCentreY()).withHeight(1));
        return;
    }

    if (isHighlighted && isActive)
    {
        g.setColour(juce::Colour(0xFF252525));
        g.fillRect(area);
    }

    if (isTicked)
    {
        g.setColour(accent_);
        g.fillRoundedRectangle(static_cast<float>(area.getX()) + 4.0f,
                               static_cast<float>(area.getCentreY()) - 4.0f,
                               8.0f, 8.0f, 2.0f);
    }

    g.setColour(textColour ? *textColour
                : (isActive ? juce::Colour(0xFFE5E2E3) : juce::Colour(0xFF666666)));
    g.setFont(juce::Font(juce::FontOptions{}.withHeight(13.0f)));
    g.drawFittedText(text, area.withX(20).withTrimmedRight(10),
                     juce::Justification::centredLeft, 1);
}

} // namespace ui
