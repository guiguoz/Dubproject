#pragma once

#include "PedalboardPanel.h"
#include "dsp/EffectChain.h"
#include "dsp/SmartMixEngine.h"

#include <JuceHeader.h>


namespace ui
{

/**
 * Top-level container for the effect chain UI.
 * Delegates all rendering to PedalboardPanel.
 */
class EffectChainEditor : public juce::Component
{
  public:
    explicit EffectChainEditor(::dsp::EffectChain& chain) : panel_(chain)
    {
        addAndMakeVisible(panel_);
    }

    void resized() override { panel_.setBounds(getLocalBounds()); }

    /// Forward music context from MasterSampleSelector to the pedalboard.
    void setMusicContext(const ::dsp::MusicContext& ctx)
    {
        panel_.setMusicContext(ctx);
    }

    const ::dsp::MusicContext& getMusicContext() const
    {
        return panel_.getMusicContext();
    }

    std::vector<bool> captureAiManagedFlags() const
    {
        return panel_.captureAiManagedFlags();
    }

    void applyAiManagedFlags(const std::vector<bool>& aiFlags)
    {
        panel_.applyAiManagedFlags(aiFlags);
    }

    /// Force an immediate rebuild of all effect cards from the chain.
    /// Call after applyChain() to avoid dangling IEffect& references.
    void forceRebuild() { panel_.forceRebuild(); }

  private:
    PedalboardPanel panel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EffectChainEditor)
};

} // namespace ui
