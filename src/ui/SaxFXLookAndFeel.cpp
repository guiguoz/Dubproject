#include "SaxFXLookAndFeel.h"
#include "Colours.h"

#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// 7-Segment LED renderer
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// bit0=top(a), bit1=topR(b), bit2=botR(c), bit3=bot(d),
// bit4=botL(e), bit5=topL(f), bit6=mid(g)
static const uint8_t kSegs[11] = {
    0b0111111, // 0
    0b0000110, // 1
    0b1011011, // 2
    0b1001111, // 3
    0b1100110, // 4
    0b1101101, // 5
    0b1111101, // 6
    0b0000111, // 7
    0b1111111, // 8
    0b1101111, // 9
    0b1000000, // '-' (index 10)
};

static void drawSegDigit(juce::Graphics& g,
                         int d, float dx, float dy, float dw, float dh,
                         juce::Colour lit, juce::Colour dim)
{
    if (d < 0 || d > 10) return;
    const uint8_t mask = kSegs[static_cast<size_t>(d)];
    const float sw  = dh * 0.14f;
    const float gap = dh * 0.03f;
    const float r   = sw * 0.45f;
    const float hh  = dh * 0.5f;
    const float hw  = dw - sw - gap * 2.f;
    const float vh  = hh - sw - gap * 2.f;

    const juce::Rectangle<float> segs[7] = {
        { dx + sw*0.5f + gap, dy,                hw, sw }, // a top
        { dx + dw - sw,       dy + sw*0.5f+gap,  sw, vh }, // b topR
        { dx + dw - sw,       dy + hh + gap,      sw, vh }, // c botR
        { dx + sw*0.5f + gap, dy + dh - sw,       hw, sw }, // d bot
        { dx,                 dy + hh + gap,       sw, vh }, // e botL
        { dx,                 dy + sw*0.5f + gap,  sw, vh }, // f topL
        { dx + sw*0.5f + gap, dy + hh - sw*0.5f,  hw, sw }, // g mid
    };

    for (int i = 0; i < 7; ++i)
    {
        const bool on = (mask >> i) & 1;
        if (on)
        {
            // outer glow
            g.setColour(lit.withAlpha(0.22f));
            g.fillRoundedRectangle(segs[i].expanded(sw * 0.55f), r + sw * 0.5f);
            // bright core
            g.setColour(lit);
            g.fillRoundedRectangle(segs[i], r);
        }
        else
        {
            g.setColour(dim);
            g.fillRoundedRectangle(segs[i], r);
        }
    }
}

// Draw LED string centred at (cx, cy)
static void drawSegString(juce::Graphics& g,
                          const juce::String& s,
                          float cx, float cy, float digitH,
                          juce::Colour lit, juce::Colour dim)
{
    const float dw    = digitH * 0.62f;
    const float sp    = dw    * 0.13f;
    const float dotSz = digitH * 0.12f;

    // Measure total width
    float tw = 0.f;
    for (int i = 0; i < s.length(); ++i)
    {
        const juce::juce_wchar c = s[i];
        tw += (c == '.') ? dotSz + sp : dw + sp;
    }
    tw -= sp;

    float x = cx - tw * 0.5f;
    const float y = cy - digitH * 0.5f;

    for (int i = 0; i < s.length(); ++i)
    {
        const juce::juce_wchar c = s[i];
        if (c >= '0' && c <= '9')
        {
            drawSegDigit(g, (int)(c - '0'), x, y, dw, digitH, lit, dim);
            x += dw + sp;
        }
        else if (c == '-')
        {
            drawSegDigit(g, 10, x, y, dw, digitH, lit, dim);
            x += dw + sp;
        }
        else if (c == '.')
        {
            const float dotY = y + digitH - dotSz * 1.4f;
            g.setColour(lit.withAlpha(0.28f));
            g.fillEllipse(x - dotSz*0.4f, dotY - dotSz*0.4f, dotSz*1.8f, dotSz*1.8f);
            g.setColour(lit);
            g.fillEllipse(x, dotY, dotSz, dotSz);
            x += dotSz + sp;
        }
        else
        {
            x += dw * 0.5f;
        }
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────

namespace ui {

SaxFXLookAndFeel::SaxFXLookAndFeel()
{
    using namespace SaxFXColours;

    setColour(juce::ResizableWindow::backgroundColourId,         background);

    // Sliders
    setColour(juce::Slider::backgroundColourId,                  knobTrack);
    setColour(juce::Slider::trackColourId,                       accent_);
    setColour(juce::Slider::thumbColourId,                       textPrimary);
    setColour(juce::Slider::textBoxTextColourId,                  textPrimary);
    setColour(juce::Slider::textBoxBackgroundColourId,            cardBody);
    setColour(juce::Slider::textBoxOutlineColourId,               cardBorder);

    // Labels
    setColour(juce::Label::textColourId,                         textPrimary);
    setColour(juce::Label::backgroundColourId,                   juce::Colours::transparentBlack);

    // Buttons
    setColour(juce::TextButton::buttonColourId,                  cardBody);
    setColour(juce::TextButton::buttonOnColourId,                accent_);
    setColour(juce::TextButton::textColourOffId,                  textSecondary);
    setColour(juce::TextButton::textColourOnId,                   textPrimary);

    // Toggle buttons
    setColour(juce::ToggleButton::textColourId,                  textPrimary);
    setColour(juce::ToggleButton::tickColourId,                  accent_);
    setColour(juce::ToggleButton::tickDisabledColourId,           textSecondary);

    // Popup menus
    setColour(juce::PopupMenu::backgroundColourId,               cardBody);
    setColour(juce::PopupMenu::textColourId,                     textPrimary);
    setColour(juce::PopupMenu::headerTextColourId,               textSecondary);
    setColour(juce::PopupMenu::highlightedBackgroundColourId,    cardBorder);
    setColour(juce::PopupMenu::highlightedTextColourId,          textPrimary);

    // Combo boxes
    setColour(juce::ComboBox::backgroundColourId,                cardBody);
    setColour(juce::ComboBox::textColourId,                      textPrimary);
    setColour(juce::ComboBox::outlineColourId,                   cardBorder);
    setColour(juce::ComboBox::buttonColourId,                    cardBody);
    setColour(juce::ComboBox::arrowColourId,                     textSecondary);

    // Group components
    setColour(juce::GroupComponent::textColourId,                textSecondary);
    setColour(juce::GroupComponent::outlineColourId,             cardBorder);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawRotarySlider — rétro industriel + affichage LED 7 segments
// ─────────────────────────────────────────────────────────────────────────────
void SaxFXLookAndFeel::drawRotarySlider(
    juce::Graphics& g,
    int x, int y, int width, int height,
    float sliderPosProportional,
    float rotaryStartAngle,
    float rotaryEndAngle,
    juce::Slider& slider)
{
    using namespace SaxFXColours;
    using juce::MathConstants;

    const float radius  = juce::jmin(width, height) * 0.5f - 4.0f;
    const float centreX = static_cast<float>(x) + static_cast<float>(width)  * 0.5f;
    const float centreY = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const float rx      = centreX - radius;
    const float ry      = centreY - radius;
    const float rw      = radius * 2.0f;
    const float angle   = rotaryStartAngle
                        + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    const float trackW  = juce::jmax(3.0f, radius * 0.14f);

    // ── Soft ambient glow behind knob (HTML: blur-xl bg-accent/10) ─────────
    g.setColour(accent_.withAlpha(0.06f));
    g.fillEllipse(rx - radius * 0.5f, ry - radius * 0.5f,
                  rw + radius, rw + radius);

    // ── Outer glow halo (accent, before everything) ────────────────────────
    g.setColour(accent_.withAlpha(0.10f));
    g.fillEllipse(rx - 6.f, ry - 6.f, rw + 12.f, rw + 12.f);

    // ── Metal body (145deg gradient matching HTML knob-outer) ──────────────
    {
        juce::ColourGradient metal(
            juce::Colour(0xFF353436), rx, ry,
            juce::Colour(0xFF1C1B1C), rx + rw, ry + rw, false);
        g.setGradientFill(metal);
        g.fillEllipse(rx, ry, rw, rw);
    }

    // ── Knurled edge ring ──────────────────────────────────────────────────
    g.setColour(juce::Colour(0xFF444444));
    g.drawEllipse(rx + 1.f, ry + 1.f, rw - 2.f, rw - 2.f, 3.0f);
    g.setColour(juce::Colour(0xFF222222));
    g.drawEllipse(rx + 3.f, ry + 3.f, rw - 6.f, rw - 6.f, 1.0f);

    // ── Inset highlight (HTML: inset 0 1px rgba(255,255,255,0.05)) ─────────
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.drawEllipse(rx + 4.f, ry + 3.f, rw - 8.f, rw - 7.f, 1.0f);

    // ── Track arc (dark ghost) ─────────────────────────────────────────────
    {
        juce::Path track;
        track.addArc(rx + trackW, ry + trackW,
                     rw - trackW*2.f, rw - trackW*2.f,
                     rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xFF1A1A1A));
        g.strokePath(track, juce::PathStrokeType(trackW,
                                                 juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

    // ── Value arc (3-pass neon glow) ───────────────────────────────────────
    if (angle > rotaryStartAngle)
    {
        const float ar = rx + trackW;
        const float aw = rw - trackW * 2.f;
        juce::Path val;
        val.addArc(ar, ry + trackW, aw, aw, rotaryStartAngle, angle, true);

        // Pass 1: wide soft glow
        g.setColour(accent_.withAlpha(0.12f));
        g.strokePath(val, juce::PathStrokeType(trackW * 4.0f,
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
        // Pass 2: medium glow
        g.setColour(accent_.withAlpha(0.30f));
        g.strokePath(val, juce::PathStrokeType(trackW * 2.0f,
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
        // Pass 3: bright core
        g.setColour(accent_.brighter(0.30f));
        g.strokePath(val, juce::PathStrokeType(trackW,
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    // ── Pointer dot ────────────────────────────────────────────────────────
    {
        const float pr   = radius * 0.62f;
        const float sinA = std::sin(angle);
        const float cosA = std::cos(angle);
        const float px   = centreX + pr * sinA;
        const float py   = centreY - pr * cosA;

        g.setColour(accent_.withAlpha(0.5f));
        g.fillEllipse(px - 4.0f, py - 4.0f, 8.0f, 8.0f);
        g.setColour(juce::Colours::white);
        g.fillEllipse(px - 2.5f, py - 2.5f, 5.0f, 5.0f);
    }

    // ── LCD panel + 7-segment value ────────────────────────────────────────
    if (radius > 16.0f)
    {
        const float panW = radius * 0.88f;
        const float panH = radius * 0.44f;
        const float panX = centreX - panW * 0.5f;
        const float panY = centreY - panH * 0.5f;

        // LCD background
        g.setColour(juce::Colour(0xFF050908));
        g.fillRoundedRectangle(panX, panY, panW, panH, 3.0f);
        g.setColour(accent_.withAlpha(0.20f));
        g.drawRoundedRectangle(panX, panY, panW, panH, 3.0f, 1.0f);

        // Format value
        const double val = slider.getValue();
        juce::String valStr;
        const double absVal = std::abs(val);
        if (absVal >= 10000.0)
            valStr = juce::String(val / 1000.0, 0) + "k";
        else if (absVal >= 1000.0)
            valStr = juce::String(val / 1000.0, 1);
        else if (absVal >= 100.0)
            valStr = juce::String(static_cast<int>(std::round(val)));
        else if (absVal >= 10.0)
            valStr = juce::String(val, 1);
        else
            valStr = juce::String(val, 2);

        const juce::Colour ledLit = accent_.brighter(0.55f);
        const juce::Colour ledDim = juce::Colour(0xFF080E08);
        drawSegString(g, valStr, centreX, centreY, panH * 0.72f, ledLit, ledDim);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawButtonBackground
// ─────────────────────────────────────────────────────────────────────────────
void SaxFXLookAndFeel::drawButtonBackground(
    juce::Graphics& g,
    juce::Button& button,
    const juce::Colour& /*backgroundColour*/,
    bool isHighlighted,
    bool isButtonDown)
{
    using namespace SaxFXColours;

    const auto  bounds   = button.getLocalBounds().toFloat().reduced(0.5f);
    const bool  toggled  = button.getToggleState();

    // Utiliser buttonOnColourId si défini sur le bouton, sinon l'accent du LAF
    juce::Colour onCol = button.findColour(juce::TextButton::buttonOnColourId, true);
    if (!button.isColourSpecified(juce::TextButton::buttonOnColourId))
        onCol = accent_;

    juce::Colour base = toggled ? onCol.withAlpha(0.80f) : juce::Colour(0xFF1A1A1A);

    if (isHighlighted) base = base.brighter(0.18f);
    if (isButtonDown)  base = base.darker(0.18f);

    if (toggled)
    {
        // Outer glow large et doux
        g.setColour(onCol.withAlpha(0.14f));
        g.fillRoundedRectangle(bounds.expanded(5.f), 8.0f);
        // Inner glow serré
        g.setColour(onCol.withAlpha(0.30f));
        g.fillRoundedRectangle(bounds.expanded(2.f), 6.0f);
    }

    // Gradient corps du bouton
    juce::ColourGradient grad(base.brighter(0.10f), bounds.getX(), bounds.getY(),
                               base.darker(0.10f),  bounds.getX(), bounds.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(bounds, 4.0f);

    // Bordure : vive si toggled, sobre sinon
    g.setColour(toggled ? onCol.brighter(0.25f).withAlpha(0.90f) : juce::Colour(0xFF3A3A3A));
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawButtonText
// ─────────────────────────────────────────────────────────────────────────────
void SaxFXLookAndFeel::drawButtonText(
    juce::Graphics& g,
    juce::TextButton& button,
    bool /*isHighlighted*/,
    bool /*isButtonDown*/)
{
    using namespace SaxFXColours;

    g.setFont(juce::Font(juce::FontOptions{}.withHeight(11.5f).withStyle("Bold")));
    g.setColour(button.getToggleState() ? juce::Colours::white : textSecondary);
    g.drawFittedText(button.getButtonText(),
                     button.getLocalBounds(),
                     juce::Justification::centred, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawPopupMenuBackground
// ─────────────────────────────────────────────────────────────────────────────
void SaxFXLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    using namespace SaxFXColours;
    g.setColour(juce::Colour(0xFF161616));
    g.fillRoundedRectangle(0, 0, static_cast<float>(width), static_cast<float>(height), 6.f);
    g.setColour(juce::Colour(0xFF444444));
    g.drawRoundedRectangle(0.5f, 0.5f,
                           static_cast<float>(width) - 1.f,
                           static_cast<float>(height) - 1.f, 6.f, 1.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawPopupMenuItem
// ─────────────────────────────────────────────────────────────────────────────
void SaxFXLookAndFeel::drawPopupMenuItem(
    juce::Graphics& g,
    const juce::Rectangle<int>& area,
    bool isSeparator,
    bool /*isActive*/,
    bool isHighlighted,
    bool isTicked,
    bool /*hasSubMenu*/,
    const juce::String& text,
    const juce::String& /*shortcutKeyText*/,
    const juce::Drawable* /*icon*/,
    const juce::Colour* /*textColour*/)
{
    using namespace SaxFXColours;

    if (isSeparator)
    {
        g.setColour(juce::Colour(0xFF333333));
        g.fillRect(area.reduced(8, 0).withHeight(1).withY(area.getCentreY()));
        return;
    }

    if (isHighlighted)
    {
        g.setColour(accent_.withAlpha(0.22f));
        g.fillRoundedRectangle(area.toFloat().reduced(2.f, 1.f), 3.f);
        g.setColour(accent_.withAlpha(0.40f));
        g.drawRoundedRectangle(area.toFloat().reduced(2.f, 1.f), 3.f, 1.f);
    }

    auto textArea = area.reduced(12, 0);

    if (isTicked)
    {
        g.setColour(accent_);
        g.fillEllipse(static_cast<float>(area.getX()) + 5.0f,
                      static_cast<float>(area.getCentreY()) - 3.5f, 7.0f, 7.0f);
        textArea = textArea.withTrimmedLeft(8);
    }

    g.setColour(isHighlighted ? juce::Colours::white : textSecondary);
    g.setFont(juce::Font(juce::FontOptions{}.withHeight(13.0f)));
    g.drawFittedText(text, textArea, juce::Justification::centredLeft, 1);
}

} // namespace ui
