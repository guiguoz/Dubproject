#pragma once

#include "Colours.h"
#include "dsp/BpmDetector.h"
#include "dsp/KeyDetector.h"
#include "dsp/SmartMixEngine.h"

#include <JuceHeader.h>
#include <functional>
#include <memory>

namespace ui
{

// ─────────────────────────────────────────────────────────────────────────────
// MasterSampleSelector
//
// Tranche MASTER du sampler : analyse BPM + key ET lecture du sample de
// référence (slot 8 du Sampler DSP).
//
//  ┌──────────┐
//  │  MASTER  │  label or (accent gold)
//  │ beat.wav │  filename
//  │  [LOAD]  │  file chooser
//  │    ●     │  LED — amber pendant analyse, gold quand prêt
//  │ 120 BPM  │  BPM détecté
//  │  C maj   │  tonalité détectée
//  │[ANALYSE] │  relancer l'analyse
//  │   fader  │  volume 0–1.5
//  │  [MUTE]  │  toggle
//  │  [LOOP]  │  toggle
//  │ [▶ PLAY] │  trigger / stop
//  └──────────┘
//
// onContextReady   → déclenché après l'analyse (thread message)
// onTriggerPressed → panel câble vers sampler slot 8
// ─────────────────────────────────────────────────────────────────────────────
class MasterSampleSelector : public juce::Component
{
public:
    // ── Callbacks ─────────────────────────────────────────────────────────────
    std::function<void(::dsp::MusicContext)> onContextReady;
    std::function<void()>                   onTriggerPressed;
    std::function<void(float)>              onVolumeChanged;
    std::function<void(bool)>               onMuteChanged;
    std::function<void(bool)>               onLoopChanged;
    std::function<void(juce::File)>         onLoadFile;
    /// Fired when the user manually edits the BPM (double-click on the label).
    std::function<void(float /*newBpm*/)>   onBpmEdited;

    MasterSampleSelector()
    {
        // Slot label
        slotLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(11.f).withStyle("Bold")));
        slotLabel_.setJustificationType(juce::Justification::centred);
        slotLabel_.setColour(juce::Label::textColourId, kGold);
        slotLabel_.setText("MASTER", juce::dontSendNotification);
        addAndMakeVisible(slotLabel_);

        // File label
        fileLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(9.5f)));
        fileLabel_.setJustificationType(juce::Justification::centred);
        fileLabel_.setColour(juce::Label::textColourId, SaxFXColours::textSecondary);
        fileLabel_.setText("--", juce::dontSendNotification);
        addAndMakeVisible(fileLabel_);

        // Load button
        loadBtn_.setButtonText("LOAD");
        loadBtn_.onClick = [this] { pickFile(); };
        addAndMakeVisible(loadBtn_);

        // BPM label — double-click to edit
        bpmLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(12.f).withStyle("Bold")));
        bpmLabel_.setJustificationType(juce::Justification::centred);
        bpmLabel_.setColour(juce::Label::textColourId, SaxFXColours::textSecondary);
        bpmLabel_.setText("-- BPM", juce::dontSendNotification);
        bpmLabel_.setEditable(false, true, false);   // editOnDoubleClick=true
        bpmLabel_.setTooltip("Double-clic pour corriger le BPM");
        bpmLabel_.onEditorHide = [this]
        {
            const juce::String raw = bpmLabel_.getText()
                                              .retainCharacters("0123456789.");
            const float bpm = raw.getFloatValue();
            if (bpm >= ::dsp::BpmDetector::kMinBpm &&
                bpm <= ::dsp::BpmDetector::kMaxBpm)
            {
                currentCtx_.bpm = bpm;
                bpmLabel_.setText(
                    juce::String(static_cast<int>(std::round(bpm))) + " BPM",
                    juce::dontSendNotification);
                bpmLabel_.setColour(juce::Label::textColourId, kGold);
                if (onBpmEdited) onBpmEdited(bpm);
            }
            else if (currentCtx_.bpm > 0.f)
            {
                // Revert to last valid value
                bpmLabel_.setText(
                    juce::String(static_cast<int>(std::round(currentCtx_.bpm))) + " BPM",
                    juce::dontSendNotification);
            }
        };
        addAndMakeVisible(bpmLabel_);

        // Key label
        keyLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(11.f)));
        keyLabel_.setJustificationType(juce::Justification::centred);
        keyLabel_.setColour(juce::Label::textColourId, SaxFXColours::textSecondary);
        keyLabel_.setText("-- ---", juce::dontSendNotification);
        addAndMakeVisible(keyLabel_);

        // Analyse button
        analyseBtn_.setButtonText("ANALYSE");
        analyseBtn_.onClick = [this] { if (!currentFile_.existsAsFile()) pickFile();
                                       else startAnalysis(currentFile_); };
        analyseBtn_.setEnabled(false);
        addAndMakeVisible(analyseBtn_);

        // Fader
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

        // Mute button
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

        // Loop button
        loopBtn_.setButtonText("LOOP");
        loopBtn_.setToggleable(true);
        loopBtn_.setClickingTogglesState(true);
        loopBtn_.onStateChange = [this]
        {
            loop_ = loopBtn_.getToggleState();
            if (onLoopChanged) onLoopChanged(loop_);
        };
        addAndMakeVisible(loopBtn_);

        // Trigger button
        triggerBtn_.setButtonText(juce::CharPointer_UTF8("\xe2\x96\xb6")); // ▶
        triggerBtn_.onClick = [this]
        {
            if (onTriggerPressed) onTriggerPressed();
        };
        addAndMakeVisible(triggerBtn_);
    }

    ~MasterSampleSelector() override { stopAnalyser(); }

    // ── Playback state (called by MainComponent timer ~30 fps) ────────────────

    void updatePlayState(bool playing)
    {
        const bool changed = (playing_ != playing);
        playing_ = playing;

        if (playing_)
            ledAlpha_ = 1.f;
        else if (ledAlpha_ > 0.f)
            ledAlpha_ = juce::jmax(0.f, ledAlpha_ - 0.05f);

        if (changed) updateTriggerLabel();
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

        slotLabel_  .setBounds(b.removeFromTop(18));
        fileLabel_  .setBounds(b.removeFromTop(14));
        loadBtn_    .setBounds(b.removeFromTop(18).reduced(2, 0));
        b.removeFromTop(4);
        ledBounds_ = b.removeFromTop(14);
        b.removeFromTop(4);
        bpmLabel_   .setBounds(b.removeFromTop(18));
        keyLabel_   .setBounds(b.removeFromTop(14));
        b.removeFromTop(4);
        analyseBtn_ .setBounds(b.removeFromTop(16).reduced(2, 0));
        b.removeFromTop(4);
        fader_      .setBounds(b.removeFromTop(60).reduced(10, 0));
        b.removeFromTop(4);
        muteBtn_    .setBounds(b.removeFromTop(18).reduced(2, 0));
        b.removeFromTop(2);
        loopBtn_    .setBounds(b.removeFromTop(18).reduced(2, 0));
        b.removeFromTop(4);
        triggerBtn_ .setBounds(b); // remaining height
    }

    // ── Painting ──────────────────────────────────────────────────────────────

    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const float W = b.getWidth();
        const float H = b.getHeight();

        // Card background
        g.setColour(SaxFXColours::cardBody.brighter(0.04f));
        g.fillRoundedRectangle(b, 3.f);

        // Gradient wash (gold → transparent)
        {
            juce::ColourGradient wash(kGold.withAlpha(0.12f), 0.f, 0.f,
                                      kGold.withAlpha(0.0f),  0.f, H * 0.5f, false);
            g.setGradientFill(wash);
            g.fillRoundedRectangle(b, 3.f);
        }

        // Accent top band (4px) — gold
        {
            juce::Path clip;
            clip.addRoundedRectangle(0.f, 0.f, W, 4.f, 3.f, 3.f, true, true, false, false);
            g.saveState();
            g.reduceClipRegion(clip);
            g.setColour(kGold.withAlpha(0.70f));
            g.fillRect(0.f, 0.f, W, 4.f);
            g.restoreState();
        }

        // Border — gold bright quand actif ou en cours d'analyse, sinon standard
        if (playing_)
            g.setColour(kGold.withAlpha(0.60f));
        else
            g.setColour(analysing_ ? kGold.withAlpha(0.90f) : kGold.withAlpha(0.40f));
        g.drawRoundedRectangle(b.reduced(0.5f), 3.f, 1.f);

        // Activity LED
        paintLed(g, ledBounds_);

        // Playing overlay — vert léger (cohérent avec SamplerChannel)
        if (playing_)
        {
            g.setColour(SaxFXColours::aiBadge.withAlpha(0.06f));
            g.fillRoundedRectangle(b, 3.f);
        }

        // Mute overlay — rouge
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
        const int   r  = 6;
        const float cx = static_cast<float>(bounds.getCentreX());
        const float cy = static_cast<float>(bounds.getCentreY());

        // Amber pendant analyse, gold quand prêt avec data, sombre sinon
        const juce::Colour ledCol = analysing_
                                    ? juce::Colour(0xFFFFAA00).withAlpha(0.8f)
                                    : (ledAlpha_ > 0.05f
                                           ? kGold.withAlpha(ledAlpha_)
                                           : juce::Colour(0xFF333333));

        if (ledAlpha_ > 0.05f || analysing_)
        {
            const float effectiveAlpha = analysing_ ? 0.8f : ledAlpha_;
            juce::ColourGradient glow(
                ledCol.withAlpha(effectiveAlpha * 0.50f), cx, cy,
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

    void updateTriggerLabel()
    {
        triggerBtn_.setButtonText(
            playing_ ? juce::String(juce::CharPointer_UTF8("\xe2\x96\xa0")) // ■
                     : juce::String(juce::CharPointer_UTF8("\xe2\x96\xb6")) // ▶
        );
    }

    // ── File selection ────────────────────────────────────────────────────────

    void pickFile()
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Select master sample",
            juce::File{},
            "*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac");

        chooser->launchAsync(
            juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectFiles,
            [this, chooser](const juce::FileChooser& fc)
            {
                const auto results = fc.getResults();
                if (results.isEmpty()) return;
                currentFile_ = results[0];
                const juce::String name = currentFile_.getFileNameWithoutExtension();
                const juce::String truncated =
                    (name.length() > 8) ? name.substring(0, 7)
                                          + juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"))
                                        : name;
                fileLabel_.setText(truncated, juce::dontSendNotification);
                analyseBtn_.setEnabled(true);

                if (onLoadFile) onLoadFile(currentFile_);
                startAnalysis(currentFile_);
            });
    }

    // ── Background analysis ───────────────────────────────────────────────────

    void startAnalysis(const juce::File& file)
    {
        stopAnalyser();
        analysing_ = true;
        bpmLabel_.setText("...", juce::dontSendNotification);
        keyLabel_.setText("...", juce::dontSendNotification);
        repaint();

        analyserThread_ = std::make_unique<AnalyserThread>(
            file,
            [this](::dsp::MusicContext ctx)
            {
                analysing_ = false;
                ledAlpha_ = (ctx.bpm > 0.f || ctx.keyRoot >= 0) ? 1.f : 0.f;
                updateDisplay(ctx);
                repaint();
                if (onContextReady) onContextReady(ctx);
            });

        analyserThread_->startThread();
    }

    void stopAnalyser()
    {
        if (analyserThread_ && analyserThread_->isThreadRunning())
            analyserThread_->stopThread(2000);
        analyserThread_.reset();
    }

    // ── Display update ────────────────────────────────────────────────────────

    void updateDisplay(const ::dsp::MusicContext& ctx)
    {
        currentCtx_ = ctx;
        static const char* kNoteNames[] = {
            "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
        };

        if (ctx.bpm > 0.f)
        {
            bpmLabel_.setText(juce::String(static_cast<int>(std::round(ctx.bpm))) + " BPM",
                              juce::dontSendNotification);
            bpmLabel_.setColour(juce::Label::textColourId, kGold);
        }
        else
        {
            bpmLabel_.setText("-- BPM", juce::dontSendNotification);
            bpmLabel_.setColour(juce::Label::textColourId, SaxFXColours::textSecondary);
        }

        if (ctx.keyRoot >= 0 && ctx.keyRoot < 12)
        {
            keyLabel_.setText(
                juce::String(kNoteNames[ctx.keyRoot]) + (ctx.isMajor ? " maj" : " min"),
                juce::dontSendNotification);
            keyLabel_.setColour(juce::Label::textColourId, SaxFXColours::aiBadge);
        }
        else
        {
            keyLabel_.setText("-- ---", juce::dontSendNotification);
            keyLabel_.setColour(juce::Label::textColourId, SaxFXColours::textSecondary);
        }
    }

    // ── Inner analysis thread ─────────────────────────────────────────────────

    class AnalyserThread : public juce::Thread
    {
    public:
        AnalyserThread(juce::File file,
                       std::function<void(::dsp::MusicContext)> callback)
            : juce::Thread("MasterSampleAnalyser"),
              file_(std::move(file)),
              callback_(std::move(callback))
        {}

        void run() override
        {
            ::dsp::MusicContext ctx;

            juce::AudioFormatManager fmt;
            fmt.registerBasicFormats();

            std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(file_));
            if (!reader) { postResult(ctx); return; }

            const double sr         = reader->sampleRate;
            const int    totalSamp  = static_cast<int>(reader->lengthInSamples);
            const int    chunkSize  = 4096;
            const int    maxSamples = static_cast<int>(sr * 30.0);
            const int    limit      = std::min(totalSamp, maxSamples);

            ::dsp::KeyDetector key;

            // Load full buffer for offline BPM autocorrelation
            juce::AudioBuffer<float> fullBuf(1, limit);
            reader->read(&fullBuf, 0, limit, 0, true, false);
            const float* fullData = fullBuf.getReadPointer(0);

            // Key detection: stream through chunks
            for (int offset = 0; offset < limit && !threadShouldExit(); offset += chunkSize)
            {
                const int n = std::min(chunkSize, limit - offset);
                key.process(fullData + offset, n, sr);
            }

            if (!threadShouldExit())
            {
                // Offline autocorrelation BPM (more accurate than onset-based)
                const float detectedBpm = ::dsp::BpmDetector::detectOffline(
                    fullData, limit, sr);
                if (detectedBpm >= ::dsp::BpmDetector::kMinBpm &&
                    detectedBpm <= ::dsp::BpmDetector::kMaxBpm)
                    ctx.bpm = detectedBpm;

                const auto keyResult = key.getResult();
                if (keyResult.key >= 0)
                {
                    ctx.keyRoot = keyResult.key;
                    ctx.isMajor = (keyResult.mode == 0);
                }
            }

            postResult(ctx);
        }

    private:
        void postResult(::dsp::MusicContext ctx)
        {
            auto cb = callback_;
            juce::MessageManager::callAsync([cb, ctx]{ cb(ctx); });
        }

        juce::File                               file_;
        std::function<void(::dsp::MusicContext)> callback_;
    };

    // ── Members ───────────────────────────────────────────────────────────────

    inline static const juce::Colour kGold { 0xFFFFCC44 };

    juce::Label              slotLabel_;
    juce::Label              fileLabel_;
    juce::TextButton         loadBtn_;
    juce::Label              bpmLabel_;
    juce::Label              keyLabel_;
    juce::TextButton         analyseBtn_;
    juce::Slider             fader_;
    juce::TextButton         muteBtn_;
    juce::TextButton         loopBtn_;
    juce::TextButton         triggerBtn_;

    juce::Rectangle<int>     ledBounds_;
    float                    ledAlpha_  = 0.f;
    bool                     analysing_ = false;
    bool                     playing_   = false;
    bool                     muted_     = false;
    bool                     loop_      = false;
    juce::File               currentFile_;
    ::dsp::MusicContext      currentCtx_;
    std::unique_ptr<AnalyserThread> analyserThread_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterSampleSelector)
};

} // namespace ui
