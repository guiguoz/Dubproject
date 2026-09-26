#include "ui/TransitionStatusBar.h"
#include "ui/Colours.h"
#include "engine/TransitionStatus.h"

namespace ui {

TransitionStatusBar::TransitionStatusBar(engine::EngineFacade& facade)
    : facade_(facade)
{
    startTimerHz(kTimerHz);
}

TransitionStatusBar::~TransitionStatusBar()
{
    stopTimer();
}

void TransitionStatusBar::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xFF0D0D0E));
    g.fillRoundedRectangle(b, 3.f);

    g.setFont(juce::Font(juce::FontOptions{}.withHeight(kFontHeight)));
    g.setColour(cachedColour_);
    g.drawText(cachedText_, getLocalBounds().reduced(4, 0),
               juce::Justification::centredLeft, false);
}

void TransitionStatusBar::timerCallback()
{
    const auto sn = facade_.getTransitionStatusSnapshot(
        selectedScene_.load(std::memory_order_relaxed));

    const auto newText   = buildText(sn);
    const auto newColour = buildColour(sn);

    if (newText != cachedText_ || newColour != cachedColour_)
    {
        cachedText_   = newText;
        cachedColour_ = newColour;
        repaint();
    }
}

juce::String TransitionStatusBar::buildText(const engine::TransitionStatusSnapshot& sn) const
{
    auto sceneNum = [](int idx) -> juce::String {
        return idx >= 0 ? juce::String(idx + 1) : juce::String("?");
    };

    if (sn.pendingScene >= 0)
    {
        const char* policyBase =
            (sn.policy == 1) ? "BUILD" :
            (sn.policy == 2) ? "BRKDWN" : "DIRECT";
        juce::String policyStr = policyBase;
        if (sn.dubActive) policyStr += "+DUB";

        juce::String commitStr;
        if (sn.boundarySample >= 0 && sn.bpm > 0.0 && sn.sampleRate > 0.0)
        {
            const double beats = engine::transitionBeatsRemaining(sn);
            commitStr = juce::String(beats, 1) + "b";
        }
        else
        {
            commitStr = juce::CharPointer_UTF8("\xe2\x80\x94"); // em dash
        }

        static const juce::String kArrow { juce::CharPointer_UTF8(" \xe2\x86\x92 ") };
        return juce::String("Scn ") + sceneNum(sn.runtimeScene)
             + kArrow + sceneNum(sn.pendingScene)
             + juce::String(" (") + policyStr + juce::String(")  ")
             + commitStr;
    }

    if (!sn.playing && sn.selectedScene >= 0 && sn.selectedScene != sn.runtimeScene)
        return juce::String("Runtime:") + sceneNum(sn.runtimeScene)
             + juce::String("  Sel:") + sceneNum(sn.selectedScene);

    return juce::String("Scene ") + sceneNum(sn.runtimeScene);
}

juce::Colour TransitionStatusBar::buildColour(const engine::TransitionStatusSnapshot& sn) const
{
    if (!sn.playing)
        return ui::SaxFXColours::textSecondary;  // gris — transport arrêté

    if (sn.pendingScene >= 0)
        return juce::Colour(0xFFFFCC00);          // ambre — transition en cours

    if (sn.selectedScene >= 0 && sn.selectedScene != sn.runtimeScene)
        return juce::Colour(0xFFFFCC44);          // ambre clair — mismatch sélection

    return ui::SaxFXColours::aiBadge;             // teal — idle, tout va bien
}

} // namespace ui
