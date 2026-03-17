#include "MainComponent.h"

#include <cmath>

//==============================================================================
MainComponent::MainComponent()
{
    // ── Audio settings button ─────────────────────────────────────────────────
    audioSettingsButton_.setButtonText("Audio Settings");
    audioSettingsButton_.onClick = [this]
    {
        auto* selector = new juce::AudioDeviceSelectorComponent(
            deviceManager, 0, 1, 0, 2, true, false, false, false);
        selector->setSize(500, 400);

        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(selector);
        options.dialogTitle              = "Audio Device Settings";
        options.dialogBackgroundColour   = juce::Colours::darkgrey;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar        = true;
        options.resizable                = false;
        options.launchAsync();
    };
    addAndMakeVisible(audioSettingsButton_);

    // ── Status labels ─────────────────────────────────────────────────────────
    statusLabel_.setJustificationType(juce::Justification::centred);
    statusLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(16.0f).withStyle("Bold")));
    statusLabel_.setText("Initialising...", juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    infoLabel_.setJustificationType(juce::Justification::centred);
    infoLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(13.0f)));
    infoLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(infoLabel_);

    pitchLabel_.setJustificationType(juce::Justification::centred);
    pitchLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(20.0f).withStyle("Bold")));
    pitchLabel_.setColour(juce::Label::textColourId, juce::Colours::cyan);
    pitchLabel_.setText("--", juce::dontSendNotification);
    addAndMakeVisible(pitchLabel_);

    // ── Effect controls ───────────────────────────────────────────────────────
    setupHarmonizerControls();
    setupFlangerControls();

    setSize(820, 680);
    setAudioChannels(1, 2);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    shutdownAudio();
}

//==============================================================================
void MainComponent::setupHarmonizerControls()
{
    harmLabel_.setText("HARMONISEUR", juce::dontSendNotification);
    harmLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(12.0f).withStyle("Bold")));
    harmLabel_.setColour(juce::Label::textColourId, juce::Colours::orange);
    addAndMakeVisible(harmLabel_);

    harmonizerToggle_.setButtonText("ON");
    harmonizerToggle_.setToggleState(false, juce::dontSendNotification);
    harmonizerToggle_.onClick = [this] {
        dspPipeline_.setHarmonizerEnabled(harmonizerToggle_.getToggleState());
    };
    addAndMakeVisible(harmonizerToggle_);

    // Voice 1 interval: -7 to +7 semitones
    harmVoice1Slider_.setRange(-7.0, 7.0, 1.0);
    harmVoice1Slider_.setValue(3.0);
    harmVoice1Slider_.setSliderStyle(juce::Slider::LinearHorizontal);
    harmVoice1Slider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 20);
    harmVoice1Slider_.onValueChange = [this] {
        dspPipeline_.getHarmonizer().setVoiceInterval(
            0, static_cast<float>(harmVoice1Slider_.getValue()));
    };
    addAndMakeVisible(harmVoice1Slider_);

    harmVoice2Slider_.setRange(-7.0, 7.0, 1.0);
    harmVoice2Slider_.setValue(-5.0);
    harmVoice2Slider_.setSliderStyle(juce::Slider::LinearHorizontal);
    harmVoice2Slider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 20);
    harmVoice2Slider_.onValueChange = [this] {
        dspPipeline_.getHarmonizer().setVoiceInterval(
            1, static_cast<float>(harmVoice2Slider_.getValue()));
    };
    addAndMakeVisible(harmVoice2Slider_);

    harmMixSlider_.setRange(0.0, 1.0, 0.01);
    harmMixSlider_.setValue(0.5);
    harmMixSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    harmMixSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 20);
    harmMixSlider_.onValueChange = [this] {
        dspPipeline_.getHarmonizer().setMix(static_cast<float>(harmMixSlider_.getValue()));
    };
    addAndMakeVisible(harmMixSlider_);
}

void MainComponent::setupFlangerControls()
{
    flangerLabel_.setText("FLANGER", juce::dontSendNotification);
    flangerLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(12.0f).withStyle("Bold")));
    flangerLabel_.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    addAndMakeVisible(flangerLabel_);

    flangerToggle_.setButtonText("ON");
    flangerToggle_.setToggleState(false, juce::dontSendNotification);
    flangerToggle_.onClick = [this] {
        dspPipeline_.setFlangerEnabled(flangerToggle_.getToggleState());
    };
    addAndMakeVisible(flangerToggle_);

    flangerRateSlider_.setRange(0.05, 10.0, 0.05);
    flangerRateSlider_.setValue(0.5);
    flangerRateSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    flangerRateSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    flangerRateSlider_.onValueChange = [this] {
        dspPipeline_.getFlanger().setRate(static_cast<float>(flangerRateSlider_.getValue()));
    };
    addAndMakeVisible(flangerRateSlider_);

    flangerDepthSlider_.setRange(0.0, 1.0, 0.01);
    flangerDepthSlider_.setValue(0.7);
    flangerDepthSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    flangerDepthSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    flangerDepthSlider_.onValueChange = [this] {
        dspPipeline_.getFlanger().setDepth(static_cast<float>(flangerDepthSlider_.getValue()));
    };
    addAndMakeVisible(flangerDepthSlider_);

    flangerFeedbackSlider_.setRange(-0.95, 0.95, 0.01);
    flangerFeedbackSlider_.setValue(0.3);
    flangerFeedbackSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    flangerFeedbackSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    flangerFeedbackSlider_.onValueChange = [this] {
        dspPipeline_.getFlanger().setFeedback(
            static_cast<float>(flangerFeedbackSlider_.getValue()));
    };
    addAndMakeVisible(flangerFeedbackSlider_);

    flangerMixSlider_.setRange(0.0, 1.0, 0.01);
    flangerMixSlider_.setValue(0.5);
    flangerMixSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    flangerMixSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    flangerMixSlider_.onValueChange = [this] {
        dspPipeline_.getFlanger().setMix(static_cast<float>(flangerMixSlider_.getValue()));
    };
    addAndMakeVisible(flangerMixSlider_);
}

//==============================================================================
// Audio callbacks
//==============================================================================

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate_ = sampleRate;
    currentBufferSize_ = samplesPerBlockExpected;

    dspPipeline_.prepare(sampleRate, samplesPerBlockExpected);

    juce::Logger::writeToLog(juce::String("Audio prepared: ")
        + juce::String(sampleRate) + " Hz, buffer "
        + juce::String(samplesPerBlockExpected) + " samples ("
        + juce::String(1000.0 * samplesPerBlockExpected / sampleRate, 1) + " ms)");
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    // REALTIME-SAFE : zéro allocation, zéro lock, zéro I/O
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    // Get writable pointer to channel 0 (mono input from Scarlett)
    float* monoChannel = bufferToFill.buffer->getWritePointer(0, bufferToFill.startSample);
    const int numSamples = bufferToFill.numSamples;

    // Compute RMS before processing (for VU meter)
    float sumSquares = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        sumSquares += monoChannel[i] * monoChannel[i];
    currentRmsLevel_.store(
        std::sqrt(sumSquares / static_cast<float>(numSamples)),
        std::memory_order_relaxed);

    // Run DSP pipeline in-place on channel 0
    dspPipeline_.process(monoChannel, numSamples);

    // Copy processed mono to all output channels (stereo headphones)
    for (int ch = 1; ch < bufferToFill.buffer->getNumChannels(); ++ch)
    {
        bufferToFill.buffer->copyFrom(
            ch, bufferToFill.startSample,
            *bufferToFill.buffer, 0, bufferToFill.startSample, numSamples);
    }
}

void MainComponent::releaseResources()
{
    dspPipeline_.reset();
    juce::Logger::writeToLog("Audio resources released.");
}

//==============================================================================
// GUI
//==============================================================================

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1a2e));

    // Title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions{}.withHeight(24.0f).withStyle("Bold")));
    g.drawText("SaxFX Live", getLocalBounds().removeFromTop(50),
               juce::Justification::centred, false);

    // VU meter
    const float rms       = currentRmsLevel_.load(std::memory_order_relaxed);
    const float dbLevel   = juce::Decibels::gainToDecibels(rms, -60.0f);
    const float normalised = juce::jmap(dbLevel, -60.0f, 0.0f, 0.0f, 1.0f);

    const int vuX = 40, vuY = 190, vuW = getWidth() - 80, vuH = 22;

    g.setColour(juce::Colours::darkgrey);
    g.fillRoundedRectangle(static_cast<float>(vuX), static_cast<float>(vuY),
                           static_cast<float>(vuW), static_cast<float>(vuH), 3.0f);

    const float filled = static_cast<float>(vuW) * juce::jlimit(0.0f, 1.0f, normalised);
    const juce::Colour barColour = (normalised < 0.7f) ? juce::Colours::limegreen
                                 : (normalised < 0.9f) ? juce::Colours::orange
                                                       : juce::Colours::red;
    g.setColour(barColour);
    g.fillRoundedRectangle(static_cast<float>(vuX), static_cast<float>(vuY),
                           filled, static_cast<float>(vuH), 3.0f);

    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions{}.withHeight(11.0f)));
    g.drawText("INPUT", vuX, vuY - 16, 60, 14, juce::Justification::left, false);
    g.drawText(juce::String(dbLevel, 1) + " dB",
               vuX, vuY + vuH + 2, vuW, 14, juce::Justification::right, false);

    // Section separators
    g.setColour(juce::Colour(0xff2a2a4e));
    g.fillRect(40, 290, getWidth() - 80, 1);
    g.fillRect(40, 460, getWidth() - 80, 1);

    // Parameter labels
    g.setColour(juce::Colours::grey);
    g.setFont(juce::Font(juce::FontOptions{}.withHeight(11.0f)));
    g.drawText("Voice 1 (st)", 40, 330, 90, 20, juce::Justification::left, false);
    g.drawText("Voice 2 (st)", 40, 358, 90, 20, juce::Justification::left, false);
    g.drawText("Mix",          40, 386, 90, 20, juce::Justification::left, false);
    g.drawText("Rate (Hz)",    40, 500, 90, 20, juce::Justification::left, false);
    g.drawText("Depth",        40, 528, 90, 20, juce::Justification::left, false);
    g.drawText("Feedback",     40, 556, 90, 20, juce::Justification::left, false);
    g.drawText("Mix",          40, 584, 90, 20, juce::Justification::left, false);
}

void MainComponent::resized()
{
    const int W  = getWidth();
    const int lx = 140; // slider start x
    const int sw = W - lx - 50; // slider width

    audioSettingsButton_.setBounds(W / 2 - 80, 240, 160, 32);
    statusLabel_        .setBounds(0,  60, W, 26);
    infoLabel_          .setBounds(0,  90, W, 20);
    pitchLabel_         .setBounds(0, 155, W, 28);

    // Harmoniser section
    harmLabel_          .setBounds(40, 300, 120, 20);
    harmonizerToggle_   .setBounds(W - 80, 298, 50, 22);
    harmVoice1Slider_   .setBounds(lx, 328, sw, 22);
    harmVoice2Slider_   .setBounds(lx, 356, sw, 22);
    harmMixSlider_      .setBounds(lx, 384, sw, 22);

    // Flanger section
    flangerLabel_           .setBounds(40, 470, 120, 20);
    flangerToggle_          .setBounds(W - 80, 468, 50, 22);
    flangerRateSlider_      .setBounds(lx, 498, sw, 22);
    flangerDepthSlider_     .setBounds(lx, 526, sw, 22);
    flangerFeedbackSlider_  .setBounds(lx, 554, sw, 22);
    flangerMixSlider_       .setBounds(lx, 582, sw, 22);
}

void MainComponent::timerCallback()
{
    repaint();

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device != nullptr)
    {
        statusLabel_.setText("LIVE — Pipeline actif", juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, juce::Colours::limegreen);

        infoLabel_.setText(
            juce::String(static_cast<int>(currentSampleRate_)) + " Hz  |  "
            + juce::String(currentBufferSize_) + " samples  ("
            + juce::String(1000.0 * currentBufferSize_ / currentSampleRate_, 1) + " ms)  |  "
            + device->getName(),
            juce::dontSendNotification);

        // Pitch display
        const auto pitch = dspPipeline_.getLastPitch();
        if (pitch.frequencyHz > 0.0f && pitch.confidence > 0.3f)
        {
            pitchLabel_.setText(
                frequencyToNoteName(pitch.frequencyHz)
                + "  " + juce::String(pitch.frequencyHz, 0) + " Hz"
                + "  (" + juce::String(static_cast<int>(pitch.confidence * 100)) + "%)",
                juce::dontSendNotification);
        }
        else
        {
            pitchLabel_.setText("--", juce::dontSendNotification);
        }
    }
    else
    {
        statusLabel_.setText("Aucun périphérique audio", juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, juce::Colours::orange);
        infoLabel_.setText("→ Cliquer sur 'Audio Settings'", juce::dontSendNotification);
        pitchLabel_.setText("--", juce::dontSendNotification);
    }
}

//==============================================================================
juce::String MainComponent::frequencyToNoteName(float hz)
{
    static const char* noteNames[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    if (hz <= 0.0f) return "--";

    const float midi   = 69.0f + 12.0f * std::log2(hz / 440.0f);
    const int   midiInt = static_cast<int>(std::round(midi));
    if (midiInt < 0 || midiInt > 127) return "--";

    const int octave   = midiInt / 12 - 1;
    const int noteIdx  = midiInt % 12;
    return juce::String(noteNames[noteIdx]) + juce::String(octave);
}
