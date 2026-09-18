#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "engine/EngineFacade.h"
#include "engine/TransitionStatus.h"

namespace ui {

// Indicateur d'état de transition : lecture seule, timer 25 Hz.
// Affiche "Scene X" (idle), "Scn X → Y (POLICY)  Z.Z b" (pending),
// ou "Runtime:X  Sel:Y" (selected != runtime, transport arrêté).
// Aucune décision musicale ne dépend de ce composant.
class TransitionStatusBar : public juce::Component, private juce::Timer
{
public:
    explicit TransitionStatusBar(engine::EngineFacade& facade);
    ~TransitionStatusBar() override;

    // Appelé par MainComponent à chaque changement de scène sélectionnée dans l'UI.
    void setSelectedScene(int idx) noexcept
    {
        selectedScene_.store(idx, std::memory_order_relaxed);
    }

    void paint(juce::Graphics& g) override;

private:
    engine::EngineFacade& facade_;
    std::atomic<int>      selectedScene_ { 0 };
    juce::String          cachedText_;
    juce::Colour          cachedColour_ { juce::Colour(0xFF4CDFA8) }; // aiBadge par défaut

    static constexpr int   kTimerHz    = 25;
    static constexpr float kFontHeight = 10.f;

    void         timerCallback() override;
    juce::String buildText  (const engine::TransitionStatusSnapshot& sn) const;
    juce::Colour buildColour(const engine::TransitionStatusSnapshot& sn) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransitionStatusBar)
};

} // namespace ui
