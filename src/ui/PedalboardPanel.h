#pragma once

#include "Colours.h"
#include "EffectRackUnit.h"
#include "MagicButton.h"
#include "dsp/EffectChain.h"
#include "dsp/SmartMixEngine.h"
#include "dsp/DelayEffect.h"
#include "dsp/EnvelopeFilterEffect.h"
#include "dsp/FlangerEffect.h"
#include "dsp/HarmonizerEffect.h"
#include "dsp/OctaverEffect.h"
#include "dsp/PitchForkEffect.h"
#include "dsp/ReverbEffect.h"
#include "dsp/SlicerEffect.h"
#include "dsp/SynthEffect.h"
#include "dsp/TunerEffect.h"
#include "dsp/WhammyEffect.h"

#include <JuceHeader.h>
#include <memory>
#include <vector>


namespace ui
{

// ─────────────────────────────────────────────────────────────────────────────
// PedalboardPanel
//
// Container of EffectRackUnit cards.
// - Cards fill the full available width equally (optimal grid, no fixed size).
// - Below a minimum card width (100 px) a horizontal scrollbar appears.
// - A "+" button on the right opens a popup to add a new effect.
// - Each card's [X] button removes that effect from the chain.
// - A ✨ MagicButton triggers auto-order + smart defaults.
// ─────────────────────────────────────────────────────────────────────────────
class PedalboardPanel : public juce::Component, private juce::Timer
{
  public:
    static constexpr int kAddBtnW  = 36;
    static constexpr int kMagicW   = 36;
    static constexpr int kMinCardW = 100;
    static constexpr int kGap      = 6;
    static constexpr int kPad      = 6;

    explicit PedalboardPanel(::dsp::EffectChain& chain) : chain_(chain)
    {
        viewport_.setScrollBarsShown(false, true);
        viewport_.setScrollBarThickness(6);
        viewport_.setViewedComponent(&inner_, false);
        addAndMakeVisible(viewport_);

        addBtn_.setButtonText("+");
        addBtn_.onClick = [this] { showAddMenu(); };
        addAndMakeVisible(addBtn_);

        magicBtn_.onAutoMix = [this]
        {
            magicBtn_.setContext(currentCtx_);
            triggerAutoMix(currentCtx_.style);
        };
        magicBtn_.onStylePreset = [this](::dsp::MusicContext::Style s)
        {
            currentCtx_.style = s;
            magicBtn_.setContext(currentCtx_);
            triggerAutoMix(s);
        };
        addAndMakeVisible(magicBtn_);

        startTimer(100);
    }

    ~PedalboardPanel() override { stopTimer(); }

    // ── Public API ────────────────────────────────────────────────────────────

    /// Called by MainComponent when master sample analysis completes.
    void setMusicContext(const ::dsp::MusicContext& ctx)
    {
        currentCtx_ = ctx;
        magicBtn_.setContext(ctx);
    }

    const ::dsp::MusicContext& getMusicContext() const { return currentCtx_; }

    /// Force an immediate rebuild of all effect cards from the chain.
    /// Call after applyChain() to avoid dangling IEffect& references.
    void forceRebuild() { rebuild(); }

    /// Returns one bool per effect card: true if SmartMixEngine managed it.
    std::vector<bool> captureAiManagedFlags() const
    {
        std::vector<bool> result;
        result.reserve(units_.size());
        for (const auto& u : units_)
            result.push_back(u->isAiManaged());
        return result;
    }

    /// Restore AI-managed badges on cards after a project load.
    void applyAiManagedFlags(const std::vector<bool>& aiFlags)
    {
        for (std::size_t i = 0; i < units_.size() && i < aiFlags.size(); ++i)
            units_[i]->setAllKnobsAiManaged(aiFlags[i]);
    }

    void resized() override
    {
        const int magicX  = getWidth() - kMagicW - kPad;
        const int addBtnX = magicX    - kAddBtnW - kPad;

        magicBtn_.setBounds(magicX,  kPad, kMagicW,  getHeight() - kPad * 2);
        addBtn_.setBounds  (addBtnX, kPad, kAddBtnW, getHeight() - kPad * 2);

        viewport_.setBounds(0, 0, addBtnX - kPad, getHeight());
        layoutInner();
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(SaxFXColours::background);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        g.setColour(SaxFXColours::cardBorder);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 8.0f, 1.0f);
    }

  private:
    // ── Timer — animation + chain change detection ────────────────────────────

    void timerCallback() override
    {
        // Animation tick
        if (animProgress_ < 1.f)
        {
            const float step = 16.f / 300.f; // ~300ms at 16ms interval
            animProgress_ = animProgress_ + step < 1.f ? animProgress_ + step : 1.f;

            const int n = static_cast<int>(units_.size());
            for (int i = 0; i < n; ++i)
            {
                const auto si = static_cast<std::size_t>(i);
                if (si >= animFrom_.size() || si >= animTo_.size()) break;

                const float t   = animProgress_;
                const auto& af  = animFrom_[si];
                const auto& at  = animTo_[si];
                const int   x   = af.getX() + static_cast<int>(static_cast<float>(at.getX() - af.getX()) * t);
                const int   y   = af.getY() + static_cast<int>(static_cast<float>(at.getY() - af.getY()) * t);
                units_[si]->setBounds(x, y, at.getWidth(), at.getHeight());
            }
            repaint();

            if (animProgress_ >= 1.f)
                startTimer(100); // back to slow polling

            return; // skip chain check during animation
        }

        // Chain change detection (only when not animating)
        if (chain_.effectCount() != static_cast<int>(units_.size()))
            rebuild();
    }

    // ── Rebuild cards from chain ──────────────────────────────────────────────

    void rebuild()
    {
        units_.clear();
        for (int i = 0; i < chain_.effectCount(); ++i)
        {
            auto* fx = chain_.getEffect(i);
            if (fx == nullptr) continue;

            auto unit = std::make_unique<EffectRackUnit>(*fx);

            const int idx = i;
            unit->onRemove = [this, idx] { chain_.removeEffect(idx); rebuild(); };

            if (fx->type() == ::dsp::EffectType::Synth)
            {
                unit->onToggle = [this, idx](bool enabled) {
                    if (enabled) { chain_.moveEffect(idx, 0); rebuild(); }
                };
            }

            inner_.addAndMakeVisible(*unit);
            units_.push_back(std::move(unit));
        }
        layoutInner();
    }

    // ── Layout — optimal grid filling the available space ────────────────────

    void layoutInner()
    {
        const int n   = static_cast<int>(units_.size());
        const int vpW = viewport_.getWidth();
        const int vpH = viewport_.getHeight();

        if (n == 0) { inner_.setSize(vpW, vpH); repaint(); return; }

        int   bestCols = 1;
        float bestArea = 0.0f;
        for (int cols = 1; cols <= n; ++cols)
        {
            const int   rows = (n + cols - 1) / cols;
            const float cw   = static_cast<float>(vpW - kPad * (cols + 1)) / static_cast<float>(cols);
            const float ch   = static_cast<float>(vpH - kPad * (rows + 1)) / static_cast<float>(rows);
            if (cw < static_cast<float>(kMinCardW)) break;
            const float area = cw * ch;
            if (area > bestArea) { bestArea = area; bestCols = cols; }
        }

        const int cols  = bestCols;
        const int rows  = (n + cols - 1) / cols;
        const int cardW = (vpW - kPad * (cols + 1)) / cols;
        const int cardH = (vpH - kPad * (rows + 1)) / rows;

        for (int i = 0; i < n; ++i)
        {
            const int col = i % cols;
            const int row = i / cols;
            units_[static_cast<std::size_t>(i)]->setBounds(
                kPad + col * (cardW + kGap),
                kPad + row * (cardH + kGap),
                cardW, cardH);
        }

        inner_.setSize(vpW, vpH);
        repaint();
    }

    // ── Magic: auto-order + smart defaults + animation ────────────────────────

    void triggerAutoMix(::dsp::MusicContext::Style styleOverride)
    {
        const int n = static_cast<int>(units_.size());
        if (n == 0) return;

        // 1. Capture current card positions (index → bounds)
        std::vector<juce::Rectangle<int>> oldBounds(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            oldBounds[static_cast<std::size_t>(i)] = units_[static_cast<std::size_t>(i)]->getBounds();

        // 2. Compute optimal order permutation
        auto order = ::dsp::SmartMixEngine::computeOptimalOrder(chain_);

        // 3. Apply the reorder to the chain
        ::dsp::SmartMixEngine::reorderChain(chain_, order);

        // 4. Apply style to context before defaults
        currentCtx_.style = styleOverride;

        // 5. Apply smart defaults (params updated on effect objects)
        ::dsp::SmartMixEngine::applySmartDefaults(chain_, currentCtx_);

        // 6. Rebuild UI — new EffectRackUnits read updated param values
        rebuild();

        // 7. Mark all knobs as AI-managed
        for (auto& unit : units_)
            unit->setAllKnobsAiManaged(true);

        // 8. Compute target positions from what layoutInner set
        const int newN = static_cast<int>(units_.size());
        animFrom_.resize(static_cast<std::size_t>(newN));
        animTo_.resize  (static_cast<std::size_t>(newN));

        for (int i = 0; i < newN; ++i)
        {
            const auto si = static_cast<std::size_t>(i);
            // The effect at new position i came from old position order[i]
            const int oldIdx = (si < order.size())
                                   ? order[si]
                                   : i;
            animFrom_[si] = (static_cast<std::size_t>(oldIdx) < oldBounds.size())
                                ? oldBounds[static_cast<std::size_t>(oldIdx)]
                                : units_[si]->getBounds();
            animTo_[si] = units_[si]->getBounds();

            // Start all cards at their "from" position
            units_[si]->setBounds(animFrom_[si]);
        }

        // 9. Start animation
        animProgress_ = 0.f;
        startTimer(16);
    }

    // ── Add effect popup ─────────────────────────────────────────────────────

    void showAddMenu()
    {
        if (chain_.effectCount() >= ::dsp::EffectChain::kMaxEffects)
            return;

        juce::PopupMenu menu;
        menu.addItem(1,  "Harmonizer");
        menu.addItem(2,  "Flanger");
        menu.addItem(3,  "Delay");
        menu.addItem(4,  "Octaver");
        menu.addItem(5,  "Reverb");
        menu.addItem(6,  "Whammy");
        menu.addItem(7,  "PitchFork");
        menu.addItem(8,  "Env Filter");
        menu.addItem(9,  "Accordeur");
        menu.addItem(10, "Slicer");
        menu.addItem(11, "Synth");

        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&addBtn_),
            [this](int result)
            {
                std::unique_ptr<::dsp::IEffect> fx;
                switch (result)
                {
                case 1:  fx = std::make_unique<::dsp::HarmonizerEffect>();     break;
                case 2:  fx = std::make_unique<::dsp::FlangerEffect>();        break;
                case 3:  fx = std::make_unique<::dsp::DelayEffect>();          break;
                case 4:  fx = std::make_unique<::dsp::OctaverEffect>();        break;
                case 5:  fx = std::make_unique<::dsp::ReverbEffect>();         break;
                case 6:  fx = std::make_unique<::dsp::WhammyEffect>();         break;
                case 7:  fx = std::make_unique<::dsp::PitchForkEffect>();      break;
                case 8:  fx = std::make_unique<::dsp::EnvelopeFilterEffect>(); break;
                case 9:  fx = std::make_unique<::dsp::TunerEffect>();          break;
                case 10: fx = std::make_unique<::dsp::SlicerEffect>();         break;
                case 11: fx = std::make_unique<::dsp::SynthEffect>();          break;
                default: return;
                }
                const bool isSynth = (result == 11);
                chain_.addEffect(std::move(fx));
                if (isSynth && chain_.effectCount() > 1)
                    chain_.moveEffect(chain_.effectCount() - 1, 0);
                rebuild();
            });
    }

    // ── Members ───────────────────────────────────────────────────────────────

    ::dsp::EffectChain&                          chain_;
    juce::Viewport                               viewport_;
    juce::Component                              inner_;
    juce::TextButton                             addBtn_;
    MagicButton                                  magicBtn_;
    std::vector<std::unique_ptr<EffectRackUnit>> units_;

    // Smart mix state
    ::dsp::MusicContext                    currentCtx_;

    // Card reorder animation
    std::vector<juce::Rectangle<int>> animFrom_;
    std::vector<juce::Rectangle<int>> animTo_;
    float                             animProgress_ = 1.f; // 1.f = done

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalboardPanel)
};

} // namespace ui
