#pragma once

#include "Colours.h"
#include "EffectParameterSlider.h"
#include "dsp/IEffect.h"

#include <JuceHeader.h>
#include <memory>
#include <vector>


namespace ui
{

/**
 * A generic UI container for a single IEffect.
 */
class EffectEditor : public juce::Component
{
  public:
    EffectEditor(::dsp::IEffect& effect) : effect_(effect)
    {
        setComponentID("EffectEditor");


        // Toggle
        toggle_.setButtonText("ON");
        toggle_.setToggleState(effect.enabled.load(), juce::dontSendNotification);
        toggle_.onClick = [this, &effect]() { effect.enabled.store(toggle_.getToggleState()); };
        addAndMakeVisible(toggle_);

        // Parameters — accent colour per effect type
        const juce::Colour accent = SaxFXColours::forEffectType(effect.type());
        for (int i = 0; i < effect.paramCount(); ++i)
        {
            auto slider = std::make_unique<EffectParameterSlider>(effect, i, accent);
            addAndMakeVisible(*slider);
            sliders_.push_back(std::move(slider));
        }
    }

    static juce::String getEffectName(::dsp::EffectType t)
    {
        switch (t)
        {
        case ::dsp::EffectType::Flanger:
            return "Flanger";
        case ::dsp::EffectType::Harmonizer:
            return "Harmonizer";
        case ::dsp::EffectType::Reverb:
            return "Reverb";
        case ::dsp::EffectType::PitchFork:
            return "PitchFork";
        case ::dsp::EffectType::EnvelopeFilter:
            return "Env Filter";
        case ::dsp::EffectType::Delay:
            return "Delay";
        case ::dsp::EffectType::Whammy:
            return "Whammy";
        case ::dsp::EffectType::Octaver:
            return "Octaver";
        case ::dsp::EffectType::Tuner:
            return "Tuner";
        case ::dsp::EffectType::Slicer:
            return "Slicer";
        case ::dsp::EffectType::Synth:
            return "Synth";
        default:
            return "Effect";
        }
    }

    void paint(juce::Graphics& g) override
    {
        // Module background
        g.setColour(SaxFXColours::cardBody);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        
        // Border with a subtle glow color from the accent
        const juce::Colour accent = SaxFXColours::forEffectType(effect_.type());
        g.setColour(accent.withAlpha(0.2f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.0f, 1.0f);

        // Title text
        g.setColour(accent);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(11.0f).withStyle("Bold")));
        g.drawText(getEffectName(effect_.type()).toUpperCase(), 
                   12, 12, getWidth() - 70, 20, juce::Justification::left);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);
        
        // Top right for the ON toggle
        auto topArea = area.removeFromTop(24);
        toggle_.setBounds(topArea.removeFromRight(40));

        // Layout sliders below
        area.removeFromTop(10); // Spacing
        if (!sliders_.empty())
        {
            int w = area.getWidth() / (int)sliders_.size();
            for (auto& s : sliders_)
            {
                s->setBounds(area.removeFromLeft(w));
            }
        }
    }

  private:
    ::dsp::IEffect& effect_;
    juce::ToggleButton toggle_;
    std::vector<std::unique_ptr<EffectParameterSlider>> sliders_;
};

} // namespace ui
