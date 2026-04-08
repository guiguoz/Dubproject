#pragma once

#include "Colours.h"
#include "NeonButton.h"
#include "dsp/StepSequencer.h"
#include "dsp/Sampler.h"

#include <JuceHeader.h>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui
{

// ─────────────────────────────────────────────────────────────────────────────
// StepSequencerPanel
//
// Grid UI : 8 tracks × 16 step buttons.
// Left zone per row: [S#] [LOAD] [●] [LCD name] [MUTE]
// Right zone: volume slider
// Top bar: ▶/■ | TAP | BPM | ⚡
// Playhead + per-track VU bars drawn via paintOverChildren at 30 Hz.
// ─────────────────────────────────────────────────────────────────────────────
class StepSequencerPanel : public juce::Component,
                           public juce::FileDragAndDropTarget,
                           public juce::ScrollBar::Listener,
                           private juce::Timer
{
public:
    // ── Callbacks ─────────────────────────────────────────────────────────────
    std::function<void(int track, int step, bool active)> onStepChanged;
    std::function<void(int slot, std::string path)>       onSlotFileLoaded;
    std::function<void(int slot)>                         onSlotCleared;
    std::function<void(float bpm)>                        onBpmChanged;
    std::function<void(bool playing)>                     onPlayChanged;
    std::function<void(int slot, float gain)>             onVolumeChanged;
    std::function<void(int slot, bool muted)>             onMutedChanged;
    std::function<void()>                                 onMagicButtonPressed;
    std::function<bool(int slot)>                         isSlotPlaying;  // for VU (legacy)
    std::function<float(int slot)>                        getSlotLevel;   // real output peak 0..1+
    std::function<float()>                                getDuckingGain; // 1.0 = no duck, 0.5 = -6dB
    std::function<void(int slot, bool soloed)>            onSoloChanged;
    /// Called when user right-clicks a slot indicator and picks a type override.
    /// typeIndex = 0-7 (maps to ContentType enum), or -1 = clear override.
    std::function<void(int slot, int typeIndex)>          onTypeOverrideChanged;
    std::function<void(int slot)>                         onTrackCopyRequest;
    std::function<void(int slot)>                         onTrackPasteRequest;
    bool                                                  hasPasteData { false };
    /// Called when user clicks the [ED] edit button for a slot.
    std::function<void(int slot)>                         onEditPressed;
    /// Returns the playhead ratio [0..1] for a slot (for waveform animation).
    std::function<float(int slot)>                        getSlotPlayhead;
    /// Called when the global step count changes via the page label menu.
    /// (track, bars) — MainComponent uses this to update StepSequencer.
    std::function<void(int track, int bars)>              onTrackBarCountChanged;

    // ── Constructor ───────────────────────────────────────────────────────────
    explicit StepSequencerPanel(::dsp::StepSequencer& seq)
        : seq_(seq)
    {
        // Play/stop
        playBtn_.setButtonText(juce::CharPointer_UTF8("\xe2\x96\xb6"));  // ▶
        playBtn_.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playBtn_);

        // Tap tempo
        tapBtn_.setButtonText("TAP");
        tapBtn_.onClick = [this] { onTap(); };
        addAndMakeVisible(tapBtn_);

        // BPM label (double-click to edit)
        bpmLabel_.setText("120 BPM", juce::dontSendNotification);
        bpmLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(14.f).withStyle("Bold")));
        bpmLabel_.setColour(juce::Label::textColourId, SaxFXColours::textPrimary);
        bpmLabel_.setEditable(false, true, false);
        bpmLabel_.onEditorHide = [this]
        {
            const juce::String raw = bpmLabel_.getText()
                                         .retainCharacters("0123456789.");
            const float bpm = raw.getFloatValue();
            if (bpm >= kMinBpm && bpm <= kMaxBpm)
            {
                currentBpm_ = bpm;
                bpmLabel_.setText(juce::String(juce::roundToInt(bpm)) + " BPM",
                                  juce::dontSendNotification);
                if (onBpmChanged) onBpmChanged(currentBpm_);
            }
            else
            {
                bpmLabel_.setText(juce::String(juce::roundToInt(currentBpm_)) + " BPM",
                                  juce::dontSendNotification);
            }
        };
        addAndMakeVisible(bpmLabel_);

        // Magic Mix ⚡ — logic kept here but button hidden (moved to sidebar in MainComponent)
        magicBtn_.setButtonText(juce::CharPointer_UTF8("\xe2\x9a\xa1"));
        magicBtn_.setClickingTogglesState(true);
        magicBtn_.onClick = [this] { if (onMagicButtonPressed) onMagicButtonPressed(); };
        magicBtn_.setVisible(false);
        // Play/Stop and TAP also hidden — driven from MainComponent sidebar
        playBtn_.setVisible(false);
        tapBtn_ .setVisible(false);

        // Per-track controls
        for (int t = 0; t < 8; ++t)
        {
            // Slot label — rôle fixe de la piste
            static constexpr const char* kRoleNames[8] = {
                "MST", "BASS", "KICK", "SNR", "HAT", "PAD", "SYN", "PRC"
            };
            slotLabels_[t].setText(kRoleNames[t], juce::dontSendNotification);
            slotLabels_[t].setFont(juce::Font(juce::FontOptions{}.withHeight(9.f).withStyle("Bold")));
            slotLabels_[t].setColour(juce::Label::textColourId, trackColour(t));
            slotLabels_[t].setJustificationType(juce::Justification::centred);
            addAndMakeVisible(slotLabels_[t]);

            // Load button (right-click → context menu)
            loadBtns_[t].setButtonText("LOAD");
            loadBtns_[t].setColour(juce::TextButton::buttonColourId,
                                   SaxFXColours::cardBody);
            loadBtns_[t].setColour(juce::TextButton::textColourOffId,
                                   SaxFXColours::textSecondary);
            loadBtns_[t].setAccentColour(SaxFXColours::aiBadge);   // neon green
            loadBtns_[t].onClick = [this, t] { openFileDialog(t); };
            loadBtns_[t].addMouseListener(this, false);
            addAndMakeVisible(loadBtns_[t]);

            // Loaded indicator (● / ○ or type tag when magic is active)
            loadedIndicators_[t].setText(juce::CharPointer_UTF8("\xe2\x97\x8b"),
                                         juce::dontSendNotification);
            loadedIndicators_[t].setFont(juce::Font(juce::FontOptions{}.withHeight(9.f)));
            loadedIndicators_[t].setColour(juce::Label::textColourId,
                                           SaxFXColours::textSecondary);
            loadedIndicators_[t].setJustificationType(juce::Justification::centred);
            loadedIndicators_[t].addMouseListener(this, false);  // right-click override
            addAndMakeVisible(loadedIndicators_[t]);

            // Retro LCD sample name
            sampleNameLabels_[t].setText("--------", juce::dontSendNotification);
            sampleNameLabels_[t].setFont(
                juce::Font(juce::FontOptions{}.withName("Courier New").withHeight(9.5f)));
            sampleNameLabels_[t].setColour(juce::Label::backgroundColourId,
                                           juce::Colours::transparentBlack);
            sampleNameLabels_[t].setColour(juce::Label::textColourId,
                                           juce::Colour(0xFF4CDFA8));
            sampleNameLabels_[t].setColour(juce::Label::outlineColourId,
                                           juce::Colours::transparentBlack);
            sampleNameLabels_[t].setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(sampleNameLabels_[t]);

            // Mute button (toggle)
            muteBtns_[t].setButtonText("M");
            muteBtns_[t].setClickingTogglesState(true);
            muteBtns_[t].setColour(juce::TextButton::buttonColourId,
                                   SaxFXColours::cardBody);
            muteBtns_[t].setColour(juce::TextButton::buttonOnColourId,
                                   juce::Colour(0xFFCC2222));
            muteBtns_[t].setColour(juce::TextButton::textColourOffId,
                                   SaxFXColours::textSecondary);
            muteBtns_[t].setColour(juce::TextButton::textColourOnId,
                                   juce::Colours::white);
            muteBtns_[t].setAccentColour(juce::Colour(0xFFFF4444));  // red mute
            muteBtns_[t].onClick = [this, t]
            {
                const bool muted = muteBtns_[t].getToggleState();
                if (onMutedChanged) onMutedChanged(t, muted);
            };
            addAndMakeVisible(muteBtns_[t]);
            muteBtns_[t].setFadeOutMs(0);

            // Edit button — opens waveform/trim editor
            editBtns_[t].setButtonText("ED");
            editBtns_[t].setEnabled(false);
            editBtns_[t].setColour(juce::TextButton::buttonColourId,  SaxFXColours::cardBody);
            editBtns_[t].setColour(juce::TextButton::textColourOffId, SaxFXColours::textSecondary);
            editBtns_[t].setAccentColour(SaxFXColours::neonCyan);    // cyan edit
            editBtns_[t].onClick = [this, t] {
                if (onEditPressed) onEditPressed(t);
            };
            addAndMakeVisible(editBtns_[t]);
            editBtns_[t].setFadeOutMs(0);

            // Volume slider
            volSliders_[t].setSliderStyle(juce::Slider::LinearVertical);
            volSliders_[t].setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            volSliders_[t].setRange(0.0, 1.0, 0.01);
            volSliders_[t].setValue(1.0, juce::dontSendNotification);
            volSliders_[t].setColour(juce::Slider::trackColourId,
                                     trackColour(t).withAlpha(0.5f));
            volSliders_[t].onValueChange = [this, t]
            {
                if (onVolumeChanged)
                    onVolumeChanged(t, static_cast<float>(volSliders_[t].getValue()));
            };
            addAndMakeVisible(volSliders_[t]);

            // Solo button — quand actif, seule cette piste est audible
            soloBtns_[t].setButtonText("S");
            soloBtns_[t].setClickingTogglesState(true);
            soloBtns_[t].setColour(juce::TextButton::buttonColourId,   SaxFXColours::cardBody);
            soloBtns_[t].setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFDD9900));
            soloBtns_[t].setColour(juce::TextButton::textColourOffId,  SaxFXColours::textSecondary);
            soloBtns_[t].setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
            soloBtns_[t].setAccentColour(juce::Colour(0xFFFFAA00));  // amber solo
            soloBtns_[t].onClick = [this, t]
            {
                const bool nowSoloed = soloBtns_[t].getToggleState();
                // Désactiver tous les autres boutons solo
                for (int i = 0; i < 8; ++i)
                    if (i != t) soloBtns_[i].setToggleState(false, juce::dontSendNotification);
                if (onSoloChanged) onSoloChanged(t, nowSoloed);
            };
            addAndMakeVisible(soloBtns_[t]);
            soloBtns_[t].setFadeOutMs(0);

            // Step buttons (32 visible at a time; page navigation scrolls through the full pattern)
            for (int s = 0; s < 32; ++s)
            {
                stepBtns_[t][s].setClickingTogglesState(true);
                stepBtns_[t][s].setColour(juce::TextButton::buttonColourId,
                                          SaxFXColours::cardBorder.darker(0.4f));
                stepBtns_[t][s].setColour(juce::TextButton::buttonOnColourId,
                                          trackColour(t));
                stepBtns_[t][s].onClick = [this, t, s]
                {
                    const int  actualStep = viewOffsetSteps_ + s;
                    const bool active     = stepBtns_[t][s].getToggleState();
                    seq_.setStep(t, actualStep, active);
                    if (onStepChanged) onStepChanged(t, actualStep, active);
                };
                addAndMakeVisible(stepBtns_[t][s]);
            }
        }

        // Page navigation buttons (◀ / ▶)
        prevPageBtn_.setButtonText(juce::CharPointer_UTF8("\xe2\x97\x80"));  // ◀
        prevPageBtn_.onClick = [this] { navigatePage(-1); };
        addAndMakeVisible(prevPageBtn_);

        nextPageBtn_.setButtonText(juce::CharPointer_UTF8("\xe2\x96\xb6"));  // ▶
        nextPageBtn_.onClick = [this] { navigatePage(+1); };
        addAndMakeVisible(nextPageBtn_);

        pageLabel_.setText("1 MESURE \xe2\x96\xbe", juce::dontSendNotification);
        pageLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(11.f)));
        pageLabel_.setColour(juce::Label::textColourId, SaxFXColours::textSecondary);
        pageLabel_.setJustificationType(juce::Justification::centred);
        pageLabel_.addMouseListener(this, false);
        addAndMakeVisible(pageLabel_);

        // Scrollbar horizontale sous la grille de steps
        hScrollBar_.setAutoHide(false);
        hScrollBar_.setColour(juce::ScrollBar::thumbColourId,
                              juce::Colour(0xFF4CDFA8).withAlpha(0.55f));
        hScrollBar_.setColour(juce::ScrollBar::trackColourId,
                              juce::Colour(0xFF1C1B1C));
        hScrollBar_.addListener(this);
        addAndMakeVisible(hScrollBar_);

        startTimerHz(30);
    }

    ~StepSequencerPanel() override { stopTimer(); }

    // ── Public API ────────────────────────────────────────────────────────────

    /// Called by MainComponent sidebar play/stop button.
    void triggerPlay() { togglePlay(); }

    /// Called by MainComponent sidebar TAP button.
    void triggerTap()  { onTap(); }

    float getBpm() const noexcept { return currentBpm_; }

    void setStepState(int track, int step, bool active)
    {
        if (track < 0 || track >= 8) return;
        // Only update the button if this step is in the current visible page
        const int visIdx = step - viewOffsetSteps_;
        if (visIdx >= 0 && visIdx < 32)
            stepBtns_[track][visIdx].setToggleState(active, juce::dontSendNotification);
    }

    void setTrackStepCount(int track, int count)
    {
        if (track < 0 || track >= 8) return;
        trackStepCounts_[track] = juce::jlimit(1, ::dsp::StepSequencer::kMaxSteps, count);
        refreshStepButtons();
        resized();
    }

    void setBpm(float bpm)
    {
        currentBpm_ = juce::jlimit(kMinBpm, kMaxBpm, bpm);
        bpmLabel_.setText(juce::String(juce::roundToInt(currentBpm_)) + " BPM",
                          juce::dontSendNotification);
    }

    void setSlotMuted(int slot, bool muted)
    {
        if (slot >= 0 && slot < 8)
            muteBtns_[slot].setToggleState(muted, juce::dontSendNotification);
    }

    const std::string& getSlotFilePath(int slot) const
    {
        static const std::string empty;
        if (slot < 0 || slot >= 8) return empty;
        return slotFilePaths_[static_cast<std::size_t>(slot)];
    }

    void setSlotFilePath(int slot, const std::string& path)
    {
        if (slot >= 0 && slot < 8)
        {
            slotFilePaths_[static_cast<std::size_t>(slot)] = path;
            setSlotSampleName(slot, path);
        }
    }

    /// Set the content-type tag shown in the loaded indicator (e.g. "KICK", "BASS").
    /// Pass empty string to restore the normal ●/○ indicator.
    void setSlotContentType(int slot, const std::string& typeName)
    {
        if (slot < 0 || slot >= 8) return;
        if (typeName.empty())
        {
            // Restore normal indicator — check if loaded
            const bool loaded = !slotFilePaths_[static_cast<std::size_t>(slot)].empty();
            loadedIndicators_[slot].setFont(
                juce::Font(juce::FontOptions{}.withHeight(9.f)));
            loadedIndicators_[slot].setText(
                loaded ? juce::CharPointer_UTF8("\xe2\x97\x8f")
                       : juce::CharPointer_UTF8("\xe2\x97\x8b"),
                juce::dontSendNotification);
            loadedIndicators_[slot].setColour(
                juce::Label::textColourId,
                loaded ? trackColour(slot) : SaxFXColours::textSecondary);
        }
        else
        {
            loadedIndicators_[slot].setFont(
                juce::Font(juce::FontOptions{}.withHeight(8.f).withStyle("Bold")));
            loadedIndicators_[slot].setText(typeName, juce::dontSendNotification);
            loadedIndicators_[slot].setColour(
                juce::Label::textColourId, juce::Colour(0xFFFFCC44));  // amber
        }
    }

    /// Sync the ⚡ button toggle state to match the actual magic-active state.
    void setMagicActive(bool active)
    {
        magicBtn_.setToggleState(active, juce::dontSendNotification);
    }

    void setSlotLoaded(int slot, bool loaded)
    {
        if (slot >= 0 && slot < 8)
        {
            editBtns_[slot].setEnabled(loaded);
            loadedIndicators_[slot].setText(
                loaded ? juce::CharPointer_UTF8("\xe2\x97\x8f")
                       : juce::CharPointer_UTF8("\xe2\x97\x8b"),
                juce::dontSendNotification);
            loadedIndicators_[slot].setColour(
                juce::Label::textColourId,
                loaded ? trackColour(slot) : SaxFXColours::textSecondary);
            if (!loaded)
                setSlotSampleName(slot, "");
        }
    }

    /// Store a pre-computed peak envelope (200 bins) for the waveform preview.
    /// Called from MainComponent after sample load. Thread: GUI only.
    void setSlotWaveform(int slot, std::vector<float> envelope) noexcept
    {
        if (slot < 0 || slot >= 8) return;
        slotEnvelopes_[static_cast<std::size_t>(slot)] = std::move(envelope);
        repaint();
    }

    /// Update the slot's BPM indicator after auto-match processing.
    /// Colours the loaded-indicator and appends a BPM tooltip to the sample label.
    ///   confidence ≥ 0.7 → neon green ✓ ; ≥ 0.5 → orange ~ ; < 0.5 → red ?
    void setSlotBpm(int slot, float bpm, float confidence) noexcept
    {
        if (slot < 0 || slot >= 8) return;
        const auto idx = static_cast<std::size_t>(slot);

        const juce::String bpmStr = (bpm > 0.f)
            ? (juce::String(juce::roundToInt(bpm)) + " BPM")
            : juce::String("-- BPM");

        juce::Colour indicatorColour;
        juce::String tooltipSuffix;
        if (bpm <= 0.f || confidence < 0.01f) {
            indicatorColour = SaxFXColours::textSecondary;
            tooltipSuffix   = " (BPM: n/a)";
        } else if (confidence >= 0.7f) {
            indicatorColour = juce::Colour(0xFF4CDFA8);  // neon green
            tooltipSuffix   = " | " + bpmStr + " \xe2\x9c\x93";
        } else if (confidence >= 0.5f) {
            indicatorColour = juce::Colour(0xFFFFAA00);  // amber
            tooltipSuffix   = " | " + bpmStr + " ~";
        } else {
            indicatorColour = juce::Colour(0xFFFF4444);  // red
            tooltipSuffix   = " | " + bpmStr + " ?";
        }

        // Only override indicator colour when slot is loaded (don't fight setSlotLoaded)
        if (loadedIndicators_[idx].getText().contains(
                juce::CharPointer_UTF8("\xe2\x97\x8f")))
        {
            loadedIndicators_[idx].setColour(juce::Label::textColourId, indicatorColour);
        }

        // Append BPM info as tooltip on the sample name label
        sampleNameLabels_[idx].setTooltip(
            sampleNameLabels_[idx].getText() + tooltipSuffix);
    }

    void setSlotSampleName(int slot, const std::string& path)
    {
        if (slot < 0 || slot >= 8) return;
        if (path.empty())
        {
            sampleNameLabels_[slot].setText("--------", juce::dontSendNotification);
            sampleNameLabels_[slot].setColour(juce::Label::textColourId,
                                              juce::Colour(0xFF226622));
        }
        else
        {
            const juce::File f { juce::String(path) };
            juce::String name = f.getFileNameWithoutExtension().toUpperCase();
            if (name.length() > 11) name = name.substring(0, 11);
            sampleNameLabels_[slot].setText(name, juce::dontSendNotification);
            sampleNameLabels_[slot].setColour(juce::Label::textColourId,
                                              juce::Colour(0xFF44EE44));
        }
    }

    // ── Layout ────────────────────────────────────────────────────────────────

    void resized() override
    {
        const int W = getWidth();
        const int H = getHeight();

        static constexpr int kTopH       = 18;  // page nav bar
        static constexpr int kScrollH    = 12;  // scrollbar height
        static constexpr int kLeftW      = 222;
        static constexpr int kRightW     = 36;
        static constexpr int kPad        = 3;
        static constexpr int kViewSteps  = 32;  // always 32 steps visible

        // Page navigation row
        prevPageBtn_.setBounds(kLeftW + kPad,            1, 20, kTopH - 2);
        pageLabel_  .setBounds(kLeftW + kPad + 22,       1, 80, kTopH - 2);
        nextPageBtn_.setBounds(kLeftW + kPad + 104,      1, 20, kTopH - 2);

        // Scrollbar — sous la grille, alignée sur la zone des steps
        hScrollBar_.setBounds(kLeftW + kPad, H - kScrollH - 1,
                              W - kLeftW - kRightW - 2 * kPad, kScrollH);
        updateScrollBar();

        const int rowAreaY = kTopH + kPad;
        const int rowAreaH = H - rowAreaY - kScrollH - kPad * 2;
        const int rowH     = rowAreaH / 8;
        const int gridW    = W - kLeftW - kRightW - 2 * kPad;
        const int stepW    = gridW / kViewSteps;

        for (int t = 0; t < 8; ++t)
        {
            const int ry     = rowAreaY + t * rowH;
            const int nSteps = trackStepCounts_[t];

            slotLabels_[t]      .setBounds(kPad,           ry,      22, rowH);
            loadBtns_[t]        .setBounds(kPad + 24,      ry + 2,  36, rowH - 4);
            loadedIndicators_[t].setBounds(kPad + 62,      ry,      12, rowH);
            sampleNameLabels_[t].setBounds(kPad + 76,  ry + 2, 60, rowH - 4);  // réduit 76→60
            editBtns_[t]        .setBounds(kPad + 138, ry + 2, 20, rowH - 4);  // nouveau
            muteBtns_[t]        .setBounds(kPad + 160, ry + 2, 24, rowH - 4);  // décalé
            soloBtns_[t]        .setBounds(kPad + 186, ry + 2, 28, rowH - 4);  // décalé

            for (int s = 0; s < kViewSteps; ++s)
            {
                const int actualStep = viewOffsetSteps_ + s;
                if (actualStep < nSteps)
                {
                    stepBtns_[t][s].setBounds(kLeftW + s * stepW, ry + 1, stepW - 1, rowH - 2);
                    stepBtns_[t][s].setVisible(true);
                }
                else
                {
                    stepBtns_[t][s].setVisible(false);
                    stepBtns_[t][s].setBounds(0, 0, 0, 0);
                }
            }

            volSliders_[t].setBounds(W - kRightW - kPad + 14, ry, 22, rowH);
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(SaxFXColours::cardBody);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.f);
        g.setColour(SaxFXColours::cardBorder);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.f, 1.f);
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        static constexpr int kLeftW  = 222;  // matches resized()
        static constexpr int kRightW = 36;
        static constexpr int kPad    = 3;
        static constexpr int kTopH   = 18;
        static constexpr int kScrollH = 12;
        static constexpr int kViewSteps = 32;

        const int W     = getWidth();
        const int H     = getHeight();
        const int gridW = W - kLeftW - kRightW - 2 * kPad;
        const int rowH  = (H - kTopH - kPad - kScrollH - kPad * 2) / 8;
        const int gridY = kTopH + kPad;

        // ── Waveform preview + playhead (behind LCD label, in slot left zone) ──
        for (int t = 0; t < 8; ++t)
        {
            const auto& env = slotEnvelopes_[static_cast<std::size_t>(t)];
            if (env.empty()) continue;

            const int ry = gridY + t * rowH;
            // Waveform area = where sampleNameLabels_[t] lives (x=kPad+76, w=60)
            const juce::Rectangle<float> wfArea(
                static_cast<float>(kPad + 76), static_cast<float>(ry + 2),
                60.f, static_cast<float>(rowH - 4));

            // Dark background
            g.setColour(ui::SaxFXColours::bgInput);
            g.fillRoundedRectangle(wfArea, 3.f);

            // Waveform — gradient cyan → green, symmetric vertical bars
            const juce::ColourGradient wfGrad(
                ui::SaxFXColours::neonCyan, wfArea.getX(),     wfArea.getCentreY(),
                ui::SaxFXColours::aiBadge,  wfArea.getRight(), wfArea.getCentreY(), false);
            g.setGradientFill(wfGrad);

            // Reduced opacity when muted
            const bool muted = [&]() {
                if (auto* b = dynamic_cast<juce::ToggleButton*>(&muteBtns_[static_cast<std::size_t>(t)]))
                    return b->getToggleState();
                return muteBtns_[static_cast<std::size_t>(t)].getToggleState();
            }();
            g.setOpacity(muted ? 0.20f : 0.65f);

            const int   N  = static_cast<int>(env.size());
            const float cy = wfArea.getCentreY();
            const float hh = wfArea.getHeight() * 0.44f;
            for (int b = 0; b < N; ++b)
            {
                const float x   = wfArea.getX()
                                  + (static_cast<float>(b) / static_cast<float>(N))
                                    * wfArea.getWidth();
                const float amp = env[static_cast<std::size_t>(b)] * hh;
                g.drawVerticalLine(static_cast<int>(x), cy - amp, cy + amp);
            }
            g.setOpacity(1.f);

            // Playhead — white vertical line tracking sample position
            if (getSlotPlayhead)
            {
                const float ratio = getSlotPlayhead(t);
                if (ratio > 0.001f && ratio < 0.999f)
                {
                    const float px = wfArea.getX() + ratio * wfArea.getWidth();
                    g.setColour(juce::Colours::white.withAlpha(0.80f));
                    g.drawVerticalLine(static_cast<int>(px),
                                       wfArea.getY() + 1.f,
                                       wfArea.getBottom() - 1.f);
                }
            }
        }

        // ── Per-track VU bars ──────────────────────────────────────────────
        for (int t = 0; t < 8; ++t)
        {
            const int ry    = gridY + t * rowH + 1;
            const int barH  = rowH - 2;

            if (vuLevels_[t] >= 0.01f)
            {
                // Glowing bar in track colour, at left edge of grid
                g.setColour(trackColour(t).withAlpha(vuLevels_[t] * 0.90f));
                g.fillRect(kLeftW + 2, ry, 4, barH);
                // Softer halo
                g.setColour(trackColour(t).withAlpha(vuLevels_[t] * 0.25f));
                g.fillRect(kLeftW, ry, 8, barH);
            }

            // Real VU meter on the right side next to volume slider
            const int vuX = W - kRightW - kPad + 2;
            const int vuW = 8;
            const int vuH = barH - 4;
            const int vyy = ry + 2;

            g.setColour(juce::Colour(0xff111122));
            g.fillRoundedRectangle(static_cast<float>(vuX), static_cast<float>(vyy),
                                   static_cast<float>(vuW), static_cast<float>(vuH), 2.f);

            const float filled = vuLevels_[t] * static_cast<float>(vuH);
            if (filled > 0.0f)
            {
                const float h1 = juce::jmin(filled, static_cast<float>(vuH) * 0.6f);
                g.setColour(ui::SaxFXColours::vuLow.withAlpha(0.85f));
                g.fillRoundedRectangle(static_cast<float>(vuX), static_cast<float>(vyy + vuH) - h1,
                                       static_cast<float>(vuW), h1, 2.f);

                if (filled > static_cast<float>(vuH) * 0.6f)
                {
                    const float h2 = juce::jmin(filled - static_cast<float>(vuH) * 0.6f, static_cast<float>(vuH) * 0.25f);
                    g.setColour(ui::SaxFXColours::vuMid.withAlpha(0.9f));
                    g.fillRoundedRectangle(static_cast<float>(vuX), static_cast<float>(vyy + vuH) - static_cast<float>(vuH) * 0.6f - h2,
                                           static_cast<float>(vuW), h2, 2.f);
                }
                if (filled > static_cast<float>(vuH) * 0.85f)
                {
                    const float h3 = filled - static_cast<float>(vuH) * 0.85f;
                    g.setColour(ui::SaxFXColours::vuHigh.withAlpha(0.95f));
                    g.fillRoundedRectangle(static_cast<float>(vuX), static_cast<float>(vyy + vuH) - static_cast<float>(vuH) * 0.85f - h3,
                                           static_cast<float>(vuW), h3, 2.f);
                }
            }
        }

        // ── Drag-and-drop highlight overlay ──────────────────────────────────
        if (dragHighlightTrack_ >= 0 && dragHighlightTrack_ < 8)
        {
            const int ry   = gridY + dragHighlightTrack_ * rowH;
            const juce::Colour dropCol = trackColour(dragHighlightTrack_);

            // Glowing row background
            g.setColour(dropCol.withAlpha(0.15f));
            g.fillRoundedRectangle(2.f, static_cast<float>(ry),
                                   static_cast<float>(W - 4), static_cast<float>(rowH), 3.f);

            // Bright border
            g.setColour(dropCol.withAlpha(0.70f));
            g.drawRoundedRectangle(2.f, static_cast<float>(ry),
                                   static_cast<float>(W - 4), static_cast<float>(rowH), 3.f, 2.f);

            // "DROP" label centred on the row
            g.setColour(dropCol.withAlpha(0.90f));
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(12.f).withStyle("Bold")));
            g.drawText("DROP", kLeftW, ry, gridW, rowH, juce::Justification::centred);
        }

        if (!seq_.isPlaying()) return;

        const int globalStep = seq_.getCurrentStep();
        const juce::Colour playheadCol(0xFF4CDFA8);  // primary green

        // Per-track playhead (each track has its own step count)
        const int tStepW = gridW / kViewSteps;  // fixed 32-step window
        for (int t = 0; t < 8; ++t)
        {
            const int nSteps    = trackStepCounts_[t];
            const int trackStep = globalStep % nSteps;
            // Only draw if the current step is in the visible page
            const int visStep   = trackStep - viewOffsetSteps_;
            if (visStep < 0 || visStep >= kViewSteps) continue;
            const int colX      = kLeftW + visStep * tStepW;
            const int ry        = gridY + t * rowH;

            // Neon wash over column for this row
            g.setColour(playheadCol.withAlpha(0.12f));
            g.fillRect(colX, ry + 1, tStepW - 1, rowH - 2);

            // Bright border
            g.setColour(playheadCol.withAlpha(0.70f));
            g.drawRect(colX, ry + 1, tStepW - 1, rowH - 2, 2);
        }

        // Beat group dots above grid (8 groups of 4 for the 32-step window)
        const int tStepW0    = gridW / kViewSteps;
        const int nSteps0    = trackStepCounts_[0];
        const int trackStep0 = globalStep % nSteps0;
        const int visStep0   = trackStep0 - viewOffsetSteps_;
        for (int b = 0; b < 8; ++b)
        {
            const int beatStep      = b * 4;  // visual step index within page
            const int actualBeat    = viewOffsetSteps_ + beatStep;
            if (actualBeat >= nSteps0) break;
            const int dotX     = kLeftW + beatStep * tStepW0 + tStepW0 / 2 - 3;
            const bool isCurr  = (visStep0 >= beatStep && visStep0 < beatStep + 4);
            g.setColour(isCurr ? playheadCol.withAlpha(0.90f)
                               : juce::Colour(0xFF1C3A2A).withAlpha(0.70f));
            g.fillEllipse(static_cast<float>(dotX),
                          static_cast<float>(gridY - 7),
                          6.f, 6.f);
        }

        // ── Ducking Gain Reduction Meter ─────────────────────────────────────
        if (duckingLevel_ < 0.99f)
        {
            const float reduction = 1.0f - duckingLevel_; // 0.0 to 0.5 (max reduction)
            const float barWidth = 40.0f * (reduction * 2.0f); // Scale to 40px max width
            const int meterX = W - kRightW - kPad - 45; // Just left of the volume sliders
            const int meterY = 5;
            const int meterH = 8;

            g.setColour(juce::Colour(0xFFEAB308).withAlpha(0.8f)); // Yellow/Orange warning color
            g.fillRoundedRectangle(static_cast<float>(meterX + 40) - barWidth, static_cast<float>(meterY),
                                   barWidth, static_cast<float>(meterH), 2.0f);

            g.setColour(SaxFXColours::textSecondary);
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(9.f)));
            g.drawText("DUCK", meterX - 25, meterY, 25, meterH, juce::Justification::centredRight);
        }
    }

    // ── FileDragAndDropTarget — drag samples from Sononym / Explorer ────────────

    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        // Accept audio files only
        for (const auto& f : files)
        {
            const juce::File file(f);
            const auto ext = file.getFileExtension().toLowerCase();
            if (ext == ".wav" || ext == ".aif" || ext == ".aiff" ||
                ext == ".mp3" || ext == ".flac" || ext == ".ogg")
                return true;
        }
        return false;
    }

    void fileDragEnter(const juce::StringArray& /*files*/, int /*x*/, int y) override
    {
        dragHighlightTrack_ = trackIndexAtY(y);
        repaint();
    }

    void fileDragMove(const juce::StringArray& /*files*/, int /*x*/, int y) override
    {
        const int newTrack = trackIndexAtY(y);
        if (newTrack != dragHighlightTrack_)
        {
            dragHighlightTrack_ = newTrack;
            repaint();
        }
    }

    void fileDragExit(const juce::StringArray& /*files*/) override
    {
        dragHighlightTrack_ = -1;
        repaint();
    }

    void filesDropped(const juce::StringArray& files, int /*x*/, int y) override
    {
        const int track = trackIndexAtY(y);
        dragHighlightTrack_ = -1;
        repaint();

        if (track < 0 || track >= 8) return;

        // Take the first valid audio file from the drop
        for (const auto& f : files)
        {
            const juce::File file(f);
            const auto ext = file.getFileExtension().toLowerCase();
            if (ext == ".wav" || ext == ".aif" || ext == ".aiff" ||
                ext == ".mp3" || ext == ".flac" || ext == ".ogg")
            {
                const std::string path = file.getFullPathName().toStdString();
                slotFilePaths_[static_cast<std::size_t>(track)] = path;
                setSlotLoaded(track, true);
                setSlotSampleName(track, path);
                if (onSlotFileLoaded)
                    onSlotFileLoaded(track, path);
                return;  // one file per drop
            }
        }
    }

    // Right-click on LOAD or loaded indicator → context menu
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &pageLabel_)
        {
            showGlobalStepCountMenu();
            return;
        }

        if (!e.mods.isRightButtonDown()) return;
        for (int t = 0; t < 8; ++t)
        {
            if (e.eventComponent == &loadBtns_[t])
            {
                showSlotContextMenu(t);
                return;
            }
            if (e.eventComponent == &loadedIndicators_[t])
            {
                showTypeMenu(t);
                return;
            }
        }
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        (void)e;
        wheelAccumulator_ += wheel.deltaX - wheel.deltaY;
        if (wheelAccumulator_ > 0.1f)
        {
            navigatePage(-1);
            wheelAccumulator_ = 0.f;
        }
        else if (wheelAccumulator_ < -0.1f)
        {
            navigatePage(1);
            wheelAccumulator_ = 0.f;
        }
    }

private:
    void timerCallback() override
    {
        // Update VU: use real output peak when available, fallback to bool playing flag.
        if (getSlotLevel)
        {
            for (int t = 0; t < 8; ++t)
            {
                const float level = juce::jlimit(0.f, 1.f, getSlotLevel(t));
                if (level > vuLevels_[t])
                    vuLevels_[t] = level;
                else
                    vuLevels_[t] *= 0.82f;
            }
        }
        else if (isSlotPlaying)
        {
            for (int t = 0; t < 8; ++t)
            {
                const bool playing = isSlotPlaying(t);
                vuLevels_[t] = playing ? 1.0f : vuLevels_[t] * 0.82f;
            }
        }
        // Update Ducking Gain Reduction
        if (getDuckingGain)
        {
            float targetGain = getDuckingGain();
            // Fast attack, slower release for visual
            if (targetGain < duckingLevel_)
                duckingLevel_ = duckingLevel_ * 0.5f + targetGain * 0.5f;
            else
                duckingLevel_ = duckingLevel_ * 0.85f + targetGain * 0.15f;
        }

        // Auto-scroll view to follow playhead when pattern is longer than 32 steps
        if (seq_.isPlaying())
        {
            const int globalStep = seq_.getCurrentStep();
            const int maxSteps   = trackStepCounts_[0];  // all tracks same length
            const int trackStep  = globalStep % maxSteps;

            // If playhead is outside the visible 32-step window, scroll to it
            if (trackStep < viewOffsetSteps_ || trackStep >= viewOffsetSteps_ + 32)
            {
                viewOffsetSteps_ = (trackStep / 32) * 32;
                refreshStepButtons();
                resized();
            }
        }

        repaint();
    }

    void togglePlay()
    {
        const bool nowPlaying = !seq_.isPlaying();
        seq_.setPlaying(nowPlaying);
        playBtn_.setButtonText(nowPlaying
                               ? juce::CharPointer_UTF8("\xe2\x96\xa0")
                               : juce::CharPointer_UTF8("\xe2\x96\xb6"));
        if (onPlayChanged) onPlayChanged(nowPlaying);
    }

    void onTap()
    {
        const juce::int64 now = juce::Time::currentTimeMillis();
        if (tapTimes_.size() >= 1 && now - tapTimes_.back() > 2000)
            tapTimes_.clear();
        tapTimes_.push_back(now);
        if (tapTimes_.size() >= 2)
        {
            while (tapTimes_.size() > 5)
                tapTimes_.erase(tapTimes_.begin());
            double totalMs = 0.0;
            for (std::size_t i = 1; i < tapTimes_.size(); ++i)
                totalMs += static_cast<double>(tapTimes_[i] - tapTimes_[i - 1]);
            const double avgMs = totalMs / static_cast<double>(tapTimes_.size() - 1);
            const float bpm = juce::jlimit(kMinBpm, kMaxBpm,
                                           static_cast<float>(60000.0 / avgMs));
            currentBpm_ = bpm;
            bpmLabel_.setText(juce::String(juce::roundToInt(bpm)) + " BPM",
                              juce::dontSendNotification);
            seq_.setBpm(bpm);
            if (onBpmChanged) onBpmChanged(bpm);
        }
    }

    void openFileDialog(int slot)
    {
        fileChooser_ = std::make_unique<juce::FileChooser>(
            "Load sample for S" + juce::String(slot + 1),
            juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.wav;*.aif;*.aiff;*.mp3;*.flac;*.ogg");
        fileChooser_->launchAsync(
            juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectFiles,
            [this, slot](const juce::FileChooser& fc)
            {
                const auto result = fc.getResult();
                if (!result.existsAsFile()) return;
                setSlotLoaded(slot, true);
                loadFileIntoSampler(slot, result);
            });
    }

    void loadFileIntoSampler(int slot, const juce::File& file)
    {
        if (!file.existsAsFile()) return;
        slotFilePaths_[slot] = file.getFullPathName().toStdString();
        setSlotSampleName(slot, slotFilePaths_[slot]);
        if (onSlotFileLoaded)
            onSlotFileLoaded(slot, slotFilePaths_[slot]);
    }

    void showSlotContextMenu(int slot)
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Load file...");
        if (!slotFilePaths_[static_cast<std::size_t>(slot)].empty())
            menu.addItem(2, "Clear slot");
        menu.addSeparator();
        menu.addItem(3, "Copy track");
        menu.addItem(4, "Paste track", hasPasteData);
        menu.showMenuAsync(juce::PopupMenu::Options{},
            [this, slot](int result)
            {
                if      (result == 1) openFileDialog(slot);
                else if (result == 2) clearSlot(slot);
                else if (result == 3 && onTrackCopyRequest)  onTrackCopyRequest(slot);
                else if (result == 4 && onTrackPasteRequest) onTrackPasteRequest(slot);
            });
    }

    void clearSlot(int slot)
    {
        slotFilePaths_[static_cast<std::size_t>(slot)] = {};
        setSlotLoaded(slot, false);
        if (onSlotCleared) onSlotCleared(slot);
    }

    void setGlobalStepCount(int newSteps)
    {
        const int newBars = newSteps / ::dsp::StepSequencer::kStepsPerBar;
        for (int t = 0; t < 8; ++t)
        {
            trackStepCounts_[t] = newSteps;
            seq_.setTrackStepCount(t, newSteps);
        }
        if (onTrackBarCountChanged)
            for (int t = 0; t < 8; ++t)
                onTrackBarCountChanged(t, newBars);

        if (viewOffsetSteps_ >= newSteps)
            viewOffsetSteps_ = 0;

        updatePageLabel();
        updateScrollBar();
        refreshStepButtons();
        resized();
    }

    void navigatePage(int delta)
    {
        int maxSteps = 16;
        for (int t = 0; t < 8; ++t)
            maxSteps = std::max(maxSteps, trackStepCounts_[t]);
        const int maxOffset = ((maxSteps - 1) / 32) * 32;
        viewOffsetSteps_ = juce::jlimit(0, maxOffset, viewOffsetSteps_ + delta * 32);
        refreshStepButtons();
        updatePageLabel();
        updateScrollBar();
        resized();
    }

    void updateScrollBar()
    {
        int maxSteps = 16;
        for (int t = 0; t < 8; ++t)
            maxSteps = std::max(maxSteps, trackStepCounts_[t]);
        hScrollBar_.setRangeLimits(0.0, static_cast<double>(maxSteps), juce::dontSendNotification);
        hScrollBar_.setCurrentRange(static_cast<double>(viewOffsetSteps_), 32.0,
                                    juce::dontSendNotification);
        // Masquer si tout tient dans la fenêtre
        hScrollBar_.setVisible(maxSteps > 32);
    }

    void refreshStepButtons()
    {
        for (int t = 0; t < 8; ++t)
            for (int s = 0; s < 32; ++s)
            {
                const int actualStep = viewOffsetSteps_ + s;
                if (actualStep < trackStepCounts_[t])
                    stepBtns_[t][s].setToggleState(
                        seq_.getStep(t, actualStep), juce::dontSendNotification);
            }
    }

    void updatePageLabel()
    {
        const int firstBar  = viewOffsetSteps_ / ::dsp::StepSequencer::kStepsPerBar + 1;
        const int lastBar   = firstBar + 1; // 32 steps visible = 2 bars
        const int totalBars = trackStepCounts_[0] / ::dsp::StepSequencer::kStepsPerBar;

        if (totalBars <= 1)
            pageLabel_.setText("Mesure 1 / 1 \xe2\x96\xbe", juce::dontSendNotification);
        else
            pageLabel_.setText("Mesures " + juce::String(firstBar) + "-" + juce::String(std::min(lastBar, totalBars)) + " / " + juce::String(totalBars) + " \xe2\x96\xbe", juce::dontSendNotification);
    }

    void showGlobalStepCountMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader("Longueur globale");
        const int curBars = trackStepCounts_[0] / ::dsp::StepSequencer::kStepsPerBar;

        for (int bars : { 1, 2, 4, 8, 16, 32 })
        {
            const int steps = bars * ::dsp::StepSequencer::kStepsPerBar;
            const bool isCurr = (curBars == bars);
            menu.addItem(steps, juce::String(bars) + (bars == 1 ? " mesure" : " mesures")
                         + "  (" + juce::String(steps) + " pas)"
                         + (isCurr ? "  \xe2\x9c\x93" : ""));
        }
        menu.showMenuAsync(juce::PopupMenu::Options{},
            [this](int result)
            {
                if (result > 0)
                    setGlobalStepCount(result);
            });
    }

    void showTypeMenu(int slot)
    {
        static const std::array<const char*, 8> kNames {
            "KICK", "SNR (Snare)", "HAT (Hi-hat)", "BASS",
            "SYN (Synth)", "PAD", "PRC (Perc)", "??? (Other)"
        };
        juce::PopupMenu menu;
        menu.addSectionHeader("Forcer le type (slot S" + juce::String(slot + 1) + ")");
        for (int i = 0; i < 8; ++i)
            menu.addItem(i + 1, kNames[static_cast<std::size_t>(i)]);
        menu.addSeparator();
        menu.addItem(9, "Auto-detect (annuler l'override)");
        menu.showMenuAsync(juce::PopupMenu::Options{},
            [this, slot](int result)
            {
                if (result == 0) return;
                if (onTypeOverrideChanged)
                    onTypeOverrideChanged(slot, result == 9 ? -1 : result - 1);
            });
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    /// Convert a Y coordinate to a track index (0-7), or -1 if outside rows.
    int trackIndexAtY(int y) const noexcept
    {
        static constexpr int kTopH    = 18;
        static constexpr int kPad     = 3;
        static constexpr int kScrollH = 12;
        const int rowAreaY = kTopH + kPad;
        const int rowAreaH = getHeight() - rowAreaY - kScrollH - kPad * 2;
        if (rowAreaH <= 0) return -1;
        const int rowH = rowAreaH / 8;
        if (rowH <= 0) return -1;
        const int rel = y - rowAreaY;
        if (rel < 0) return -1;
        const int track = rel / rowH;
        return (track >= 0 && track < 8) ? track : -1;
    }

    static constexpr float kMinBpm = 40.f;
    static constexpr float kMaxBpm = 240.f;

    static juce::Colour trackColour(int t) noexcept
    {
        static const juce::Colour kColours[8] = {
            juce::Colour { 0xFF4CDFA8 }, // primary green (kick)
            juce::Colour { 0xFF06B6D4 }, // cyan-500 (snare)
            juce::Colour { 0xFFC8C7C7 }, // tertiary (hat)
            juce::Colour { 0xFF8B5CF6 }, // violet-500 (synth)
            juce::Colour { 0xFFF97316 }, // orange-500
            juce::Colour { 0xFFF43F5E }, // rose-500
            juce::Colour { 0xFFEAB308 }, // yellow-500
            juce::Colour { 0xFF38BDF8 }, // sky-400
        };
        return kColours[t % 8];
    }

    // ── ScrollBar listener ────────────────────────────────────────────────────
    void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override
    {
        if (bar != &hScrollBar_) return;
        // Snap au multiple de 16 (une mesure)
        const int snapped = (static_cast<int>(newRangeStart) / 16) * 16;
        if (snapped != viewOffsetSteps_)
        {
            viewOffsetSteps_ = snapped;
            refreshStepButtons();
            updatePageLabel();
            resized();
        }
    }

    // ── Members ───────────────────────────────────────────────────────────────

    ::dsp::StepSequencer& seq_;

    juce::TextButton playBtn_;
    juce::TextButton tapBtn_;
    juce::Label      bpmLabel_;
    juce::TextButton magicBtn_;
    float            currentBpm_ = 120.f;

    std::array<juce::Label,      8> slotLabels_;
    std::array<NeonButton,       8> loadBtns_;
    std::array<juce::Label,      8> loadedIndicators_;
    std::array<juce::Label,      8> sampleNameLabels_;
    std::array<NeonButton,       8> editBtns_;        // opens waveform editor
    std::array<NeonButton,       8> muteBtns_;
    std::array<juce::Slider,     8> volSliders_;
    std::array<NeonButton,       8> soloBtns_;        // solo: seule piste audible
    int trackStepCounts_[8] = { 16,16,16,16,16,16,16,16 };
    juce::TextButton stepBtns_[8][32];

    // Page navigation
    juce::TextButton prevPageBtn_;
    juce::TextButton nextPageBtn_;
    juce::Label      pageLabel_;
    juce::ScrollBar  hScrollBar_ { false };  // horizontal
    int              viewOffsetSteps_ = 0;

    std::array<std::string, 8>  slotFilePaths_;
    std::vector<juce::int64>    tapTimes_;
    float                       vuLevels_[8] = {};
    int                         dragHighlightTrack_ = -1;  // -1 = no drag active
    float                       wheelAccumulator_ = 0.f;
    float                       duckingLevel_ = 1.0f; // Smoothed for display
    std::unique_ptr<juce::FileChooser> fileChooser_;

    // Waveform preview data — 200-bin peak envelopes, set via setSlotWaveform()
    std::array<std::vector<float>, 8> slotEnvelopes_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StepSequencerPanel)
};

} // namespace ui
