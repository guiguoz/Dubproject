#pragma once

#include "Colours.h"
#include "dsp/BeatClock.h"

#include <JuceHeader.h>
#include <functional>

namespace ui
{

// ─────────────────────────────────────────────────────────────────────────────
// SamplerChannel
//
// One channel strip in the vintage "mixing board" sampler section.
//
//  ┌──────────┐
//  │    S1    │  slot label
//  │ kick.wav │  filename (truncated)
//  │  [LOAD]  │  file chooser
//  │    ●     │  activity LED (green glow)
//  │   fader  │  vertical slider 0–1.5
//  │  [MUTE]  │  toggle (red when on)
//  │  [LOOP]  │  toggle (green when on)
//  │ [▶ PLAY] │  trigger / stop
//  └──────────┘
//
// updatePlayState() must be called periodically (e.g. from SamplerPanel timer).
// ─────────────────────────────────────────────────────────────────────────────
class SamplerChannel : public juce::Component
{
public:
    std::function<void()>                onTriggerPressed;  // panel decides trigger vs stop
    std::function<void(float)>           onVolumeChanged;   // 0.0–1.5 linear
    std::function<void(bool)>            onMuteChanged;
    std::function<void(bool)>            onLoopChanged;
    std::function<void(juce::File)>      onLoadFile;
    std::function<void(::dsp::GridDiv)>  onGridChanged;     // beat-sync quantization

    SamplerChannel()
    {
        slotLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(11.f).withStyle("Bold")));
        slotLabel_.setJustificationType(juce::Justification::centred);
        slotLabel_.setColour(juce::Label::textColourId, SaxFXColours::textSecondary);
        addAndMakeVisible(slotLabel_);

        fileLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(9.5f)));
        fileLabel_.setJustificationType(juce::Justification::centred);
        fileLabel_.setColour(juce::Label::textColourId, SaxFXColours::textSecondary);
        fileLabel_.setText("--", juce::dontSendNotification);
        addAndMakeVisible(fileLabel_);

        loadBtn_.setButtonText("LOAD");
        loadBtn_.onClick = [this] { pickFile(); };
        addAndMakeVisible(loadBtn_);

        fader_.setSliderStyle(juce::Slider::LinearVertical);
        fader_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        fader_.setRange(0.0, 1.5, 0.01);
        fader_.setValue(1.0, juce::dontSendNotification);
        fader_.onValueChange = [this]
        {
            if (onVolumeChanged)
                onVolumeChanged(static_cast<float>(fader_.getValue()));
        };
        addAndMakeVisible(fader_);

        muteBtn_.setButtonText("MUTE");
        muteBtn_.setToggleable(true);
        muteBtn_.setClickingTogglesState(true);
        muteBtn_.onStateChange = [this]
        {
            muted_ = muteBtn_.getToggleState();
            if (onMuteChanged) onMuteChanged(muted_);
            repaint();
        };
        addAndMakeVisible(muteBtn_);

        loopBtn_.setButtonText("LOOP");
        loopBtn_.setToggleable(true);
        loopBtn_.setClickingTogglesState(true);
        loopBtn_.onStateChange = [this]
        {
            loop_ = loopBtn_.getToggleState();
            if (onLoopChanged) onLoopChanged(loop_);
        };
        addAndMakeVisible(loopBtn_);

        triggerBtn_.setButtonText(juce::CharPointer_UTF8("\xe2\x96\xb6")); // ▶
        triggerBtn_.onClick = [this]
        {
            if (onTriggerPressed) onTriggerPressed();
        };
        addAndMakeVisible(triggerBtn_);

        // Grid selector — four mutually-exclusive mini buttons
        static const char* kGridLabels[4] = { "1/8", "1/4", "1/2", "1" };
        static const ::dsp::GridDiv kGridDivs[4] = {
            ::dsp::GridDiv::Eighth, ::dsp::GridDiv::Quarter,
            ::dsp::GridDiv::HalfBar, ::dsp::GridDiv::Bar
        };
        for (int g = 0; g < 4; ++g)
        {
            gridBtns_[static_cast<std::size_t>(g)].setButtonText(kGridLabels[g]);
            gridBtns_[static_cast<std::size_t>(g)].setRadioGroupId(1001 + slotIdx_);
            gridBtns_[static_cast<std::size_t>(g)].setClickingTogglesState(true);
            gridBtns_[static_cast<std::size_t>(g)].setToggleable(true);
            const ::dsp::GridDiv div = kGridDivs[g];
            gridBtns_[static_cast<std::size_t>(g)].onClick = [this, div]
            {
                selectedDiv_ = div;
                if (onGridChanged) onGridChanged(div);
            };
            addAndMakeVisible(gridBtns_[static_cast<std::size_t>(g)]);
        }
        // Default: quarter note selected
        gridBtns_[1].setToggleState(true, juce::dontSendNotification);
        selectedDiv_ = ::dsp::GridDiv::Quarter;
    }

    void setSlotIndex(int i)
    {
        slotIdx_ = i;
        slotLabel_.setText("S" + juce::String(i + 1), juce::dontSendNotification);
        for (auto& b : gridBtns_)
            b.setRadioGroupId(1001 + i);
    }

    void setFileName(const juce::String& name)
    {
        // Truncate to fit narrow strip
        const juce::String truncated =
            (name.length() > 8) ? name.substring(0, 7) + juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"))
                                 : name;
        fileLabel_.setText(truncated, juce::dontSendNotification);
    }

    // Called by SamplerPanel timer (~30 fps).
    // pending = quantized trigger is waiting for next beat boundary.
    void updatePlayState(bool playing, bool pending = false)
    {
        const bool changed = (playing_ != playing) || (pending_ != pending);
        playing_ = playing;
        pending_ = pending;

        if (playing_)
        {
            ledAlpha_ = 1.f;
        }
        else if (pending_)
        {
            // Pulse amber while waiting for beat
            ledPulse_ = ledPulse_ + 0.15f;
            if (ledPulse_ > juce::MathConstants<float>::twoPi)
                ledPulse_ -= juce::MathConstants<float>::twoPi;
            ledAlpha_ = 0.4f + 0.35f * std::sin(ledPulse_);
        }
        else if (ledAlpha_ > 0.f)
        {
            ledAlpha_ = juce::jmax(0.f, ledAlpha_ - 0.05f);
            ledPulse_ = 0.f;
        }

        if (changed)
            updateTriggerLabel();

        repaint();
    }

    void setMuted(bool muted)
    {
        muted_ = muted;
        muteBtn_.setToggleState(muted, juce::dontSendNotification);
        repaint();
    }

    void setLoop(bool loop)
    {
        loop_ = loop;
        loopBtn_.setToggleState(loop, juce::dontSendNotification);
    }

    // ── Layout ────────────────────────────────────────────────────────────────

    void resized() override
    {
        auto b = getLocalBounds().reduced(2);

        slotLabel_.setBounds(b.removeFromTop(18));
        fileLabel_.setBounds(b.removeFromTop(14));
        loadBtn_.setBounds(b.removeFromTop(18).reduced(2, 0));
        b.removeFromTop(4);
        ledBounds_ = b.removeFromTop(14);
        b.removeFromTop(4);
        fader_.setBounds(b.removeFromTop(90).reduced(10, 0));
        b.removeFromTop(4);
        muteBtn_.setBounds(b.removeFromTop(18).reduced(2, 0));
        b.removeFromTop(2);
        loopBtn_.setBounds(b.removeFromTop(18).reduced(2, 0));
        b.removeFromTop(4);

        // Grid selector: 2 buttons per row, 2 rows
        {
            const int bw = b.getWidth() / 2;
            const int bh = 14;
            auto row1 = b.removeFromTop(bh);
            gridBtns_[0].setBounds(row1.removeFromLeft(bw).reduced(1, 0));
            gridBtns_[1].setBounds(row1.reduced(1, 0));
            b.removeFromTop(2);
            auto row2 = b.removeFromTop(bh);
            gridBtns_[2].setBounds(row2.removeFromLeft(bw).reduced(1, 0));
            gridBtns_[3].setBounds(row2.reduced(1, 0));
        }
        b.removeFromTop(4);
        triggerBtn_.setBounds(b); // remaining height
    }

    // ── Painting ──────────────────────────────────────────────────────────────

    void paint(juce::Graphics& g) override
    {
        const juce::Colour slotAccent = kSlotColours[static_cast<std::size_t>(slotIdx_) % 8u];
        const auto b = getLocalBounds().toFloat();
        const float W = b.getWidth();
        const float H = b.getHeight();

        // Card background
        g.setColour(SaxFXColours::cardBody);
        g.fillRoundedRectangle(b, 3.f);

        // Subtle vertical gradient wash (accent en haut → transparent)
        {
            juce::ColourGradient wash(slotAccent.withAlpha(0.12f), 0.f, 0.f,
                                     slotAccent.withAlpha(0.0f),  0.f, H * 0.5f, false);
            g.setGradientFill(wash);
            g.fillRoundedRectangle(b, 3.f);
        }

        // Accent top band (4px)
        {
            juce::Path clip;
            clip.addRoundedRectangle(0.f, 0.f, W, 4.f, 3.f, 3.f, true, true, false, false);
            g.saveState();
            g.reduceClipRegion(clip);
            g.setColour(slotAccent.withAlpha(0.70f));
            g.fillRect(0.f, 0.f, W, 4.f);
            g.restoreState();
        }

        // Border — accent teinté quand actif, sinon standard
        if (playing_)
            g.setColour(slotAccent.withAlpha(0.50f));
        else
            g.setColour(SaxFXColours::cardBorder);
        g.drawRoundedRectangle(b.reduced(0.5f), 3.f, 1.f);

        // Activity LED
        paintLed(g, ledBounds_);

        // Playing overlay — vert léger
        if (playing_)
        {
            g.setColour(SaxFXColours::aiBadge.withAlpha(0.06f));
            g.fillRoundedRectangle(b, 3.f);
        }

        // Mute overlay — red tint
        if (muted_)
        {
            g.setColour(juce::Colour(0xFFFF2244).withAlpha(0.14f));
            g.fillRoundedRectangle(b, 3.f);
        }
    }

private:
    // ── Helpers ───────────────────────────────────────────────────────────────

    void paintLed(juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        const int   r  = 6;  // rayon augmenté
        const float cx = static_cast<float>(bounds.getCentreX());
        const float cy = static_cast<float>(bounds.getCentreY());

        // Green when playing, amber when waiting for beat, dark when off
        const juce::Colour ledCol = playing_  ? SaxFXColours::aiBadge.withAlpha(ledAlpha_)
                                  : pending_  ? juce::Colour(0xFFFFAA00).withAlpha(ledAlpha_)
                                              : juce::Colour(0xFF333333);

        if (ledAlpha_ > 0.05f)
        {
            // Glow étendu 4× le rayon
            juce::ColourGradient glow(
                ledCol.withAlpha(ledAlpha_ * 0.50f), cx, cy,
                juce::Colours::transparentBlack, cx + static_cast<float>(r) * 4.f, cy,
                true);
            g.setGradientFill(glow);
            g.fillEllipse(cx - static_cast<float>(r) * 4.f, cy - static_cast<float>(r) * 4.f,
                          static_cast<float>(r) * 8.f, static_cast<float>(r) * 8.f);
        }

        g.setColour(ledCol);
        g.fillEllipse(cx - static_cast<float>(r), cy - static_cast<float>(r),
                      static_cast<float>(r * 2), static_cast<float>(r * 2));
    }

    void pickFile()
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Load sample into S" + juce::String(slotIdx_ + 1),
            juce::File{},
            "*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac");

        chooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, chooser](const juce::FileChooser& fc)
            {
                const auto results = fc.getResults();
                if (results.isEmpty()) return;
                setFileName(results[0].getFileNameWithoutExtension());
                if (onLoadFile) onLoadFile(results[0]);
            });
    }

    void updateTriggerLabel()
    {
        triggerBtn_.setButtonText(
            playing_ ? juce::String(juce::CharPointer_UTF8("\xe2\x96\xa0")) // ■
                     : juce::String(juce::CharPointer_UTF8("\xe2\x96\xb6")) // ▶
        );
    }

    // ── Members ───────────────────────────────────────────────────────────────

    // Accent couleur par slot (style Xvox, 8 couleurs cycliques)
    inline static const juce::Colour kSlotColours[8] = {
        juce::Colour(0xFF2288FF), juce::Colour(0xFFCC44FF),
        juce::Colour(0xFF00FFD0), juce::Colour(0xFFFF8800),
        juce::Colour(0xFF44FF66), juce::Colour(0xFFFF1177),
        juce::Colour(0xFFFFEE00), juce::Colour(0xFF99FFEE)
    };

    juce::Label       slotLabel_;
    juce::Label       fileLabel_;
    juce::TextButton  loadBtn_;
    juce::Slider      fader_;
    juce::TextButton  muteBtn_;
    juce::TextButton  loopBtn_;
    juce::TextButton  triggerBtn_;
    juce::TextButton  gridBtns_[4];

    juce::Rectangle<int> ledBounds_;

    bool              playing_     = false;
    bool              pending_     = false;
    bool              muted_       = false;
    bool              loop_        = false;
    int               slotIdx_     = 0;
    float             ledAlpha_    = 0.f;
    float             ledPulse_    = 0.f;
    ::dsp::GridDiv    selectedDiv_ = ::dsp::GridDiv::Quarter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerChannel)
};

} // namespace ui
