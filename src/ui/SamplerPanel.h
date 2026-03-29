#pragma once

#include "Colours.h"
#include "SamplerChannel.h"
#include "dsp/Sampler.h"

#include <JuceHeader.h>
#include <array>
#include <memory>

namespace ui
{

// ─────────────────────────────────────────────────────────────────────────────
// SamplerPanel
//
// Container of 8 SamplerChannel strips arranged side-by-side.
// Polls the Sampler's playback state at 30 fps and propagates it to each
// channel for LED animation.  Handles per-slot file loading and routes all
// control callbacks to the Sampler.
// ─────────────────────────────────────────────────────────────────────────────
class SamplerPanel : public juce::Component, private juce::Timer
{
public:
    explicit SamplerPanel(::dsp::Sampler& sampler) : sampler_(sampler)
    {
        for (int i = 0; i < 8; ++i)
        {
            channels_[static_cast<std::size_t>(i)].setSlotIndex(i);
            wireChannel(i);
            addAndMakeVisible(channels_[static_cast<std::size_t>(i)]);
        }
        startTimerHz(30);
    }

    ~SamplerPanel() override { stopTimer(); }

    // Update the filename display for a slot (called after project load).
    void applySlotName(int slot, const juce::String& name)
    {
        if (slot >= 0 && slot < 8)
            channels_[static_cast<std::size_t>(slot)].setFileName(name);
    }

    // Record the file path (called by MainComponent after project load).
    void setSlotFilePath(int slot, const std::string& path)
    {
        if (slot >= 0 && slot < 8)
            slotFilePaths_[static_cast<std::size_t>(slot)] = path;
    }

    const std::string& getSlotFilePath(int slot) const
    {
        static const std::string empty;
        if (slot < 0 || slot >= 8) return empty;
        return slotFilePaths_[static_cast<std::size_t>(slot)];
    }

    /// Called after a file is successfully loaded into a slot (UI or project load).
    /// Signature: (slotIndex, absoluteFilePath)
    std::function<void(int, std::string)> onSlotFileLoaded;

    // ── Layout ────────────────────────────────────────────────────────────────

    void resized() override
    {
        const int chW = getWidth() / 8;
        for (int i = 0; i < 8; ++i)
            channels_[static_cast<std::size_t>(i)].setBounds(
                i * chW, 0, chW, getHeight());
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(SaxFXColours::background.darker(0.1f));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.f);
        g.setColour(SaxFXColours::cardBorder);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.f, 1.f);
    }

private:
    // ── Wiring ────────────────────────────────────────────────────────────────

    void wireChannel(int i)
    {
        auto& ch = channels_[static_cast<std::size_t>(i)];

        ch.onTriggerPressed = [this, i]
        {
            if (sampler_.isPlaying(i) || sampler_.isPendingTrigger(i))
                sampler_.stop(i);
            else
                sampler_.triggerQuantized(i, sampler_.getSlotGrid(i));
        };

        ch.onGridChanged = [this, i](::dsp::GridDiv div)
        {
            sampler_.setSlotGrid(i, div);
        };

        ch.onVolumeChanged = [this, i](float v)
        {
            sampler_.setSlotGain(i, v);
        };

        ch.onMuteChanged = [this, i](bool m)
        {
            sampler_.setSlotMuted(i, m);
        };

        ch.onLoopChanged = [this, i](bool l)
        {
            sampler_.setSlotLoop(i, l);
        };

        ch.onLoadFile = [this, i](juce::File file)
        {
            slotFilePaths_[static_cast<std::size_t>(i)] = file.getFullPathName().toStdString();
            loadFileIntoSlot(i, file);
            if (onSlotFileLoaded)
                onSlotFileLoaded(i, slotFilePaths_[static_cast<std::size_t>(i)]);
        };
    }

    // ── Timer — poll playback state ───────────────────────────────────────────

    void timerCallback() override
    {
        for (int i = 0; i < 8; ++i)
            channels_[static_cast<std::size_t>(i)].updatePlayState(
                sampler_.isPlaying(i), sampler_.isPendingTrigger(i));
    }

    // ── File loading ──────────────────────────────────────────────────────────

    void loadFileIntoSlot(int slot, const juce::File& file)
    {
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(file));
        if (!reader) return;

        const int numSamples = static_cast<int>(reader->lengthInSamples);
        if (numSamples <= 0) return;

        juce::AudioBuffer<float> buf(1, numSamples);
        reader->read(&buf, 0, numSamples, 0, true, false);

        sampler_.loadSample(slot, buf.getReadPointer(0), numSamples,
                            static_cast<double>(reader->sampleRate));
    }

    // ── Members ───────────────────────────────────────────────────────────────

    ::dsp::Sampler&               sampler_;
    std::array<SamplerChannel, 8> channels_;
    std::array<std::string, 8>    slotFilePaths_ {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerPanel)
};

} // namespace ui
