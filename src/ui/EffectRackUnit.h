#pragma once

#include "Colours.h"
#include "RotaryKnob.h"
#include "dsp/IEffect.h"
#include "dsp/TunerEffect.h"

#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <vector>


namespace ui
{

// ─────────────────────────────────────────────────────────────────────────────
// EffectRackUnit — resizable card, style Blockchain Ultra
//
//  ┌──────────────────────────────────────┐  ← kHeaderH coloured header
//  │  [X]      EFFECT NAME          [⏻]  │
//  ├──────────────────────────────────────┤
//  │        knobs grid (fills body)       │
//  ├──────────────────────────────────────┤
//  │  IN ─────────────────────────── OUT  │  ← kFooterH footer
//  └──────────────────────────────────────┘
//
// Set onRemove to be notified when the user clicks [X].
// ─────────────────────────────────────────────────────────────────────────────
class EffectRackUnit : public juce::Component
{
  public:
    std::function<void()> onRemove;

    explicit EffectRackUnit(::dsp::IEffect& effect)
        : effect_(effect),
          accent_(SaxFXColours::forEffectType(effect.type())),
          name_(getEffectName(effect.type()))
    {
        // Close button
        closeBtn_.setButtonText("x");
        closeBtn_.onClick = [this] { if (onRemove) onRemove(); };
        addAndMakeVisible(closeBtn_);

        // Power toggle
        powerBtn_.setButtonText(juce::CharPointer_UTF8("\xe2\x8f\xbb")); // ⏻
        powerBtn_.setClickingTogglesState(true);
        powerBtn_.setToggleState(effect.enabled.load(), juce::dontSendNotification);
        powerBtn_.onClick = [this] { effect_.enabled.store(powerBtn_.getToggleState()); };
        addAndMakeVisible(powerBtn_);

        // Preset selector (if the effect provides presets)
        if (effect.presetCount() > 0)
        {
            presetBox_ = std::make_unique<juce::ComboBox>();
            presetBox_->addItem("-- Preset --", 1);
            for (int p = 0; p < effect.presetCount(); ++p)
                presetBox_->addItem(effect.presetName(p), p + 2);
            presetBox_->setSelectedId(1, juce::dontSendNotification);
            presetBox_->setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xFF1A1A1A));
            presetBox_->setColour(juce::ComboBox::textColourId,       accent_);
            presetBox_->setColour(juce::ComboBox::outlineColourId,    accent_.withAlpha(0.5f));
            presetBox_->onChange = [this]
            {
                const int id = presetBox_->getSelectedId();
                if (id >= 2)
                {
                    effect_.applyPreset(id - 2);
                    refreshKnobValues();
                }
            };
            addAndMakeVisible(*presetBox_);
        }

        // Knobs
        for (int i = 0; i < effect.paramCount(); ++i)
        {
            auto desc = effect.paramDescriptor(i);
            auto knob = std::make_unique<RotaryKnob>(desc.label, accent_);
            knob->setRange(desc.min, desc.max, effect.getParam(i));
            knob->onValueChange = [this, i](double v)
            { effect_.setParam(i, static_cast<float>(v)); };
            addAndMakeVisible(*knob);
            knobs_.push_back(std::move(knob));
        }
    }

    void paint(juce::Graphics& g) override
    {
        using namespace SaxFXColours;
        const auto bounds = getLocalBounds().toFloat();
        const float W = bounds.getWidth();
        const float H = bounds.getHeight();

        // ── Outer module glow (HTML: module-glow-*) ──────────────────────
        g.setColour(accent_.withAlpha(0.18f));
        g.fillRoundedRectangle(bounds.expanded(6.f), 8.0f);
        g.setColour(accent_.withAlpha(0.06f));
        g.fillRoundedRectangle(bounds.expanded(14.f), 12.0f);

        // ── Card body (HTML: bg-[#131314]) ───────────────────────────────
        g.setColour(juce::Colour(0xFF131314));
        g.fillRoundedRectangle(bounds, 4.0f);

        // ── Subtle accent top wash ────────────────────────────────────────
        {
            juce::ColourGradient wash(accent_.withAlpha(0.07f), 0.f, 0.f,
                                     accent_.withAlpha(0.0f),  0.f, H * 0.45f, false);
            g.setGradientFill(wash);
            g.fillRoundedRectangle(bounds, 4.0f);
        }

        // ── Effect name (HTML: small uppercase colored tracking label) ────
        g.setColour(accent_);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(9.5f).withStyle("Bold")));
        g.drawFittedText(name_, kBtnW, 2,
                         static_cast<int>(W) - kBtnW * 2, kHeaderH - 4,
                         juce::Justification::centred, 1);

        // ── Big central arc display ───────────────────────────────────────
        {
            const int arcAreaY = kHeaderH;
            const int arcAreaH = static_cast<int>(H) - kHeaderH - kKnobsH;

            if (effect_.type() == ::dsp::EffectType::Tuner)
            {
                drawTunerUI(g, 0, arcAreaY, static_cast<int>(W), arcAreaH);
            }
            else
            {
                const float cx = W * 0.5f;
                const float cy = static_cast<float>(arcAreaY)
                               + static_cast<float>(arcAreaH) * 0.5f;
                const float r  = juce::jmin(W * 0.36f,
                                            static_cast<float>(arcAreaH) * 0.42f);

                // Normalized value + display info from first param
                float norm = 0.5f;
                float displayVal = 50.f;
                juce::String paramLabel;
                if (effect_.paramCount() > 0)
                {
                    const auto desc = effect_.paramDescriptor(0);
                    const float val = effect_.getParam(0);
                    if (desc.max > desc.min)
                        norm = juce::jlimit(0.f, 1.f,
                                            (val - desc.min) / (desc.max - desc.min));
                    displayVal = juce::jmap(val, desc.min, desc.max, 0.f, 100.f);
                    paramLabel = juce::String(desc.label).toUpperCase();
                }

                const float arcStart = juce::MathConstants<float>::pi * 0.75f;
                const float arcEnd   = juce::MathConstants<float>::pi * 2.25f;
                const float arcVal   = arcStart + norm * (arcEnd - arcStart);

                // Ambient glow
                g.setColour(accent_.withAlpha(0.10f));
                g.fillEllipse(cx - r * 1.4f, cy - r * 1.4f, r * 2.8f, r * 2.8f);

                // Track ring (HTML: border-[6px] border-[#1c1b1c])
                {
                    juce::Path track;
                    track.addCentredArc(cx, cy, r, r, 0.f, arcStart, arcEnd, true);
                    g.setColour(juce::Colour(0xFF1C1B1C));
                    g.strokePath(track, juce::PathStrokeType(r * 0.12f,
                                 juce::PathStrokeType::curved,
                                 juce::PathStrokeType::rounded));
                }

                // Value arc with glow pass
                {
                    juce::Path arc;
                    arc.addCentredArc(cx, cy, r, r, 0.f, arcStart, arcVal, true);
                    g.setColour(accent_.withAlpha(0.22f));
                    g.strokePath(arc, juce::PathStrokeType(r * 0.22f,
                                 juce::PathStrokeType::curved,
                                 juce::PathStrokeType::rounded));
                    g.setColour(accent_);
                    g.strokePath(arc, juce::PathStrokeType(r * 0.12f,
                                 juce::PathStrokeType::curved,
                                 juce::PathStrokeType::rounded));
                }

                // Value number (HTML: text-3xl font-black)
                g.setColour(juce::Colour(0xFFE5E2E3));
                g.setFont(juce::Font(juce::FontOptions{}
                                      .withHeight(r * 0.72f).withStyle("Bold")));
                g.drawFittedText(juce::String(juce::roundToInt(displayVal)),
                                 static_cast<int>(cx - r),
                                 static_cast<int>(cy - r * 0.65f),
                                 static_cast<int>(r * 2.f),
                                 static_cast<int>(r * 1.2f),
                                 juce::Justification::centred, 1);

                // Param label (HTML: text-[8px] tracking-widest uppercase 40% opacity)
                if (!paramLabel.isEmpty())
                {
                    g.setColour(juce::Colour(0xFFE5E2E3).withAlpha(0.40f));
                    g.setFont(juce::Font(juce::FontOptions{}.withHeight(7.5f).withStyle("Bold")));
                    g.drawFittedText(paramLabel,
                                     static_cast<int>(cx - r),
                                     static_cast<int>(cy + r * 0.35f),
                                     static_cast<int>(r * 2.f), 12,
                                     juce::Justification::centred, 1);
                }
            }
        }

        // ── Subtle colored border ─────────────────────────────────────────
        g.setColour(accent_.withAlpha(0.12f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);

        // ── Disabled overlay ──────────────────────────────────────────────
        if (!effect_.enabled.load())
        {
            g.setColour(juce::Colours::black.withAlpha(0.65f));
            g.fillRoundedRectangle(bounds, 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.30f));
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(18.f).withStyle("Bold")));
            g.drawText("OFF", getLocalBounds(), juce::Justification::centred);
        }
    }

    void resized() override
    {
        closeBtn_.setBounds(2, 3, kBtnW, kHeaderH - 6);
        powerBtn_.setBounds(getWidth() - kBtnW - 2, 3, kBtnW, kHeaderH - 6);

        int bodyY = kHeaderH + 2;
        if (presetBox_)
        {
            presetBox_->setBounds(4, bodyY, getWidth() - 8, kPresetH);
            bodyY += kPresetH + 2;
        }

        if (knobs_.empty()) return;

        // Knobs pinned to the bottom of the card
        const int count  = static_cast<int>(knobs_.size());
        const int cols   = juce::jmin(count, 4);
        const int knobW  = (getWidth() - 8) / cols;
        const int knobsY = getHeight() - kKnobsH;

        for (int i = 0; i < count; ++i)
        {
            const int col = i % cols;
            knobs_[static_cast<std::size_t>(i)]->setBounds(
                4 + col * knobW, knobsY, knobW, kKnobsH);
        }
    }

    /// Mark / unmark all knobs with the AI-managed badge.
    void setAllKnobsAiManaged(bool managed)
    {
        aiManaged_ = managed;
        for (auto& k : knobs_)
            k->setAiManaged(managed);
    }

    bool isAiManaged() const { return aiManaged_; }

    /// Refresh all knob positions to match current effect param values.
    void refreshKnobValues()
    {
        for (int i = 0; i < static_cast<int>(knobs_.size()); ++i)
            knobs_[static_cast<std::size_t>(i)]->setValue(
                static_cast<double>(effect_.getParam(i)));
    }

    static juce::String getEffectIcon(::dsp::EffectType t)
    {
        switch (t)
        {
        case ::dsp::EffectType::Harmonizer:     return juce::String(juce::CharPointer_UTF8("\xe2\x99\xaf")); // ♯
        case ::dsp::EffectType::Flanger:        return "~";
        case ::dsp::EffectType::Reverb:         return juce::String(juce::CharPointer_UTF8("\xe2\x88\xbf")); // ∿
        case ::dsp::EffectType::Delay:          return juce::String(juce::CharPointer_UTF8("\xe2\x86\xa9")); // ↩
        case ::dsp::EffectType::PitchFork:      return juce::String(juce::CharPointer_UTF8("\xe2\x86\x95")); // ↕
        case ::dsp::EffectType::Whammy:         return juce::String(juce::CharPointer_UTF8("\xe2\x88\xa7")); // ∧
        case ::dsp::EffectType::Octaver:        return juce::String(juce::CharPointer_UTF8("\xe2\x8a\x95")); // ⊕
        case ::dsp::EffectType::EnvelopeFilter: return juce::String(juce::CharPointer_UTF8("\xe2\x80\xbf")); // ‿
        case ::dsp::EffectType::Slicer:         return juce::String(juce::CharPointer_UTF8("\xe2\x9c\x82")); // ✂
        case ::dsp::EffectType::Tuner:          return juce::String(juce::CharPointer_UTF8("\xe2\x99\xa9")); // ♩
        case ::dsp::EffectType::Synth:          return juce::String(juce::CharPointer_UTF8("\xe2\x99\xab")); // ♫
        default:                                return "?";
        }
    }

    static juce::String getEffectName(::dsp::EffectType t)
    {
        switch (t)
        {
        case ::dsp::EffectType::Flanger:        return "FLANGER";
        case ::dsp::EffectType::Harmonizer:     return "HARMONIZER";
        case ::dsp::EffectType::Reverb:         return "REVERB";
        case ::dsp::EffectType::PitchFork:      return "PITCHFORK";
        case ::dsp::EffectType::EnvelopeFilter: return "ENV FILTER";
        case ::dsp::EffectType::Delay:          return "DELAY";
        case ::dsp::EffectType::Whammy:         return "WHAMMY";
        case ::dsp::EffectType::Octaver:        return "OCTAVER";
        case ::dsp::EffectType::Tuner:          return "ACCORDEUR";
        case ::dsp::EffectType::Synth:          return "SYNTH";
        case ::dsp::EffectType::Slicer:         return "SLICER";
        default:                                return "EFFECT";
        }
    }

  private:
    static constexpr int kHeaderH  = 28;
    static constexpr int kFooterH  = 0;
    static constexpr int kBtnW     = 20;
    static constexpr int kPresetH  = 22;
    static constexpr int kKnobsH   = 68;

    // ── Tuner display (simple needle arc) ────────────────────────────────
    void drawTunerUI(juce::Graphics& g, int x, int y, int w, int h)
    {
        const float cx = static_cast<float>(x + w / 2);
        const float cy = static_cast<float>(y + h) * 0.62f;
        const float r  = juce::jmin(static_cast<float>(w),
                                     static_cast<float>(h)) * 0.36f;

        // Semi-circle track
        {
            juce::Path track;
            track.addCentredArc(cx, cy, r, r, 0.f,
                                juce::MathConstants<float>::pi,
                                juce::MathConstants<float>::twoPi, true);
            g.setColour(juce::Colour(0xFF1C1B1C));
            g.strokePath(track, juce::PathStrokeType(r * 0.10f,
                         juce::PathStrokeType::curved,
                         juce::PathStrokeType::rounded));
        }

        // Center tick + needle
        const float needleAngle = juce::MathConstants<float>::pi * 1.5f; // centered
        const float nx = cx + std::cos(needleAngle) * r * 0.85f;
        const float ny = cy + std::sin(needleAngle) * r * 0.85f;
        g.setColour(accent_);
        g.drawLine(cx, cy, nx, ny, 2.0f);
        g.fillEllipse(cx - 3.f, cy - 3.f, 6.f, 6.f);

        // Tick marks
        for (int i = -2; i <= 2; ++i)
        {
            const float frac = (static_cast<float>(i) + 2.f) / 4.f;
            const float angle = juce::MathConstants<float>::pi
                              + frac * juce::MathConstants<float>::pi;
            const float tx1 = cx + std::cos(angle) * r * 0.92f;
            const float ty1 = cy + std::sin(angle) * r * 0.92f;
            const float tx2 = cx + std::cos(angle) * r * 0.76f;
            const float ty2 = cy + std::sin(angle) * r * 0.76f;
            g.setColour(i == 0 ? accent_ : juce::Colour(0xFF353436));
            g.drawLine(tx1, ty1, tx2, ty2, i == 0 ? 1.5f : 1.0f);
        }

        // Label
        g.setColour(accent_.withAlpha(0.55f));
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.f).withStyle("Bold")));
        g.drawFittedText("TUNE", x, y + 4, w, 12, juce::Justification::centred, 1);
    }

    ::dsp::IEffect&                          effect_;
    juce::Colour                             accent_;
    juce::String                             name_;
    juce::TextButton                         closeBtn_;
    juce::TextButton                         powerBtn_;
    std::unique_ptr<juce::ComboBox>          presetBox_;
    std::vector<std::unique_ptr<RotaryKnob>> knobs_;
    bool                                     aiManaged_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EffectRackUnit)
};

} // namespace ui
