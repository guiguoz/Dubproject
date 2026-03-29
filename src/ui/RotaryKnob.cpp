#include "RotaryKnob.h"
#include "Colours.h"

#include <cmath>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
RotaryKnob::RotaryKnob(const juce::String& label, juce::Colour accentColour)
{
    laf_.setAccentColour(accentColour);

    // ── Slider ─────────────────────────────────────────────────────────────────
    slider_.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider_.setRotaryParameters(
        juce::MathConstants<float>::pi * 1.25f,   // start angle (bottom-left)
        juce::MathConstants<float>::pi * 2.75f,   // end angle   (bottom-right)
        true);                                     // stop at limits
    slider_.setLookAndFeel(&laf_);
    slider_.addListener(this);
    addAndMakeVisible(slider_);

    // ── Label ──────────────────────────────────────────────────────────────────
    nameLabel_.setText(label, juce::dontSendNotification);
    nameLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(11.0f)));
    nameLabel_.setColour(juce::Label::textColourId,
                         SaxFXColours::textSecondary);
    nameLabel_.setJustificationType(juce::Justification::centred);
    nameLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel_);

    setSize(64, 80);
}

RotaryKnob::~RotaryKnob()
{
    slider_.removeListener(this);
    slider_.setLookAndFeel(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────
void RotaryKnob::setRange(double min, double max, double defaultVal)
{
    slider_.setRange(min, max, 0.0);
    slider_.setValue(defaultVal, juce::dontSendNotification);
}

void RotaryKnob::setValue(double v, juce::NotificationType notification)
{
    slider_.setValue(v, notification);
}

double RotaryKnob::getValue() const
{
    return slider_.getValue();
}

void RotaryKnob::setAccentColour(juce::Colour c)
{
    laf_.setAccentColour(c);
    slider_.repaint();
    repaint();
}

void RotaryKnob::setAiManaged(bool managed)
{
    if (aiManaged_ == managed)
        return;
    aiManaged_ = managed;
    repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout
// ─────────────────────────────────────────────────────────────────────────────
void RotaryKnob::resized()
{
    auto bounds  = getLocalBounds();
    const int labelH = 16;
    nameLabel_.setBounds(bounds.removeFromBottom(labelH));
    slider_.setBounds(bounds);
}

// ─────────────────────────────────────────────────────────────────────────────
// Painting
//
// juce::Component::paint() is called AFTER all child components are painted.
// The slider and label are child components, so we only need to draw the
// AI badge on top here.
// ─────────────────────────────────────────────────────────────────────────────
void RotaryKnob::paint(juce::Graphics& g)
{
    if (!aiManaged_)
        return;

    // Small green diamond "◆" in the top-right corner of the knob area
    const float badgeR = 4.5f; // half-size of the diamond bounding box
    const float bx     = static_cast<float>(getWidth())  - badgeR * 2.0f - 1.0f;
    const float by     = 1.0f;

    // Diamond = rotated square
    juce::Path diamond;
    const float cx = bx + badgeR;
    const float cy = by + badgeR;
    diamond.startNewSubPath(cx,          cy - badgeR);  // top
    diamond.lineTo          (cx + badgeR, cy);           // right
    diamond.lineTo          (cx,          cy + badgeR);  // bottom
    diamond.lineTo          (cx - badgeR, cy);           // left
    diamond.closeSubPath();

    g.setColour(SaxFXColours::aiBadge);
    g.fillPath(diamond);

    // Thin border for legibility on dark bg
    g.setColour(SaxFXColours::cardBorder);
    g.strokePath(diamond, juce::PathStrokeType(0.5f));
}

// ─────────────────────────────────────────────────────────────────────────────
// Slider listener
// ─────────────────────────────────────────────────────────────────────────────
void RotaryKnob::sliderValueChanged(juce::Slider* /*slider*/)
{
    if (onValueChange)
        onValueChange(slider_.getValue());
}

} // namespace ui
