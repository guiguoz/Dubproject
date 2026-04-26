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

        const int numCh = static_cast<int>(reader->numChannels);
        const int numIn = static_cast<int>(reader->lengthInSamples);
        if (numIn <= 0) return;

        // Lire tous les canaux, downmix → mono (somme / numCh)
        juce::AudioBuffer<float> srcBuf(numCh, numIn);
        reader->read(&srcBuf, 0, numIn, 0, true, numCh > 1);

        std::vector<float> mono(static_cast<std::size_t>(numIn), 0.f);
        const float invCh = 1.f / static_cast<float>(numCh);
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* ptr = srcBuf.getReadPointer(ch);
            for (int i = 0; i < numIn; ++i)
                mono[i] += ptr[i] * invCh;
        }

        const double fileSR   = reader->sampleRate;
        const double engineSR = sampler_.getSampleRate();

        if (std::abs(fileSR / engineSR - 1.0) < 1e-4)
        {
            sampler_.loadSample(slot, mono.data(), numIn, engineSR);
            return;
        }

        // Resampling offline — LagrangeInterpolator
        const double speed  = fileSR / engineSR;
        const int    numOut = static_cast<int>(std::ceil(static_cast<double>(numIn) / speed));

        // Pad +4 samples (valeur finale répétée) pour éviter glitch en fin de buffer
        std::vector<float> padded(mono.begin(), mono.end());
        padded.resize(mono.size() + 4, padded.empty() ? 0.f : padded.back());

        std::vector<float> dst(static_cast<std::size_t>(numOut));
        juce::LagrangeInterpolator interp;
        interp.reset();
        interp.process(speed, padded.data(), dst.data(), numOut);

        sampler_.loadSample(slot, dst.data(), numOut, engineSR);
    }

    // ── Members ───────────────────────────────────────────────────────────────

    ::dsp::Sampler&               sampler_;
    std::array<SamplerChannel, 8> channels_;
    std::array<std::string, 8>    slotFilePaths_ {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerPanel)
};

} // namespace ui
