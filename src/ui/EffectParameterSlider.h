#pragma once

#include "RotaryKnob.h"
#include "dsp/IEffect.h"

#include <JuceHeader.h>


namespace ui
{

/**
 * A RotaryKnob linked to a single DSP parameter.
 */
class EffectParameterSlider : public juce::Component
{
  public:
    EffectParameterSlider(::dsp::IEffect& effect, int paramIndex,
                          juce::Colour accentColour = juce::Colour(0xFF1ABC9C))
        : effect_(effect), paramIndex_(paramIndex), knob_(effect.paramDescriptor(paramIndex).label,
                                                          accentColour)
    {
        auto desc = effect.paramDescriptor(paramIndex);

        knob_.setRange(desc.min, desc.max, effect.getParam(paramIndex));
        knob_.onValueChange = [this](double v)
        { effect_.setParam(paramIndex_, static_cast<float>(v)); };

        addAndMakeVisible(knob_);
    }

    void resized() override
    {
        knob_.setBounds(getLocalBounds());
    }

  private:
    ::dsp::IEffect& effect_;
    int             paramIndex_;
    RotaryKnob      knob_;
};

} // namespace ui
