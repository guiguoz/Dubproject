#include "SerumHost.h"

namespace dsp {

SerumHost::SerumHost()
{
    formatManager_.addFormat(new juce::VST3PluginFormat());
}

SerumHost::~SerumHost()
{
    unload();
}

// ─────────────────────────────────────────────────────────────────────────────
// load — message thread only
// ─────────────────────────────────────────────────────────────────────────────
bool SerumHost::load(const juce::String& vst3Path, double sampleRate, int blockSize)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    unload();

    sampleRate_ = sampleRate;
    blockSize_  = blockSize;

    juce::OwnedArray<juce::PluginDescription> results;
    juce::VST3PluginFormat vst3;

    // Scan the specific file (fast — no full directory scan)
    vst3.findAllTypesForFile(results, vst3Path);

    if (results.isEmpty())
    {
        DBG("SerumHost: no VST3 found at " << vst3Path);
        return false;
    }

    juce::String error;
    plugin_ = formatManager_.createPluginInstance(
        *results[0], sampleRate_, blockSize_, error);

    if (plugin_ == nullptr)
    {
        DBG("SerumHost: failed to instantiate — " << error);
        return false;
    }

    plugin_->prepareToPlay(sampleRate_, blockSize_);
    serumBuffer_.setSize(2, blockSize_, false, true, false);

    loaded_.store(true, std::memory_order_release);
    setProcessingEnabled(true);

    DBG("SerumHost: loaded " << plugin_->getName());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// unload — message thread only, call after setProcessingEnabled(false)
// ─────────────────────────────────────────────────────────────────────────────
void SerumHost::unload()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    setProcessingEnabled(false);
    loaded_.store(false, std::memory_order_release);

    if (plugin_ != nullptr)
    {
        plugin_->releaseResources();
        plugin_.reset();
    }

    serumBuffer_.setSize(0, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// processBlock — audio thread only
// ─────────────────────────────────────────────────────────────────────────────
void SerumHost::processBlock(juce::MidiBuffer& midi) noexcept
{
    if (!processingEnabled_.load(std::memory_order_acquire) || plugin_ == nullptr)
    {
        serumBuffer_.clear();
        return;
    }

    serumBuffer_.clear();
    plugin_->processBlock(serumBuffer_, midi);

    const float gain = outputGain_.load(std::memory_order_relaxed);
    if (gain != 1.0f)
    {
        serumBuffer_.applyGain(gain);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Preset (message thread)
// ─────────────────────────────────────────────────────────────────────────────
void SerumHost::getState(juce::MemoryBlock& dest) const
{
    if (plugin_ != nullptr)
        plugin_->getStateInformation(dest);
}

void SerumHost::setState(const void* data, int size)
{
    if (plugin_ != nullptr)
        plugin_->setStateInformation(data, size);
}

// ─────────────────────────────────────────────────────────────────────────────
// Editor (message thread)
// ─────────────────────────────────────────────────────────────────────────────
juce::AudioProcessorEditor* SerumHost::createEditor()
{
    if (plugin_ == nullptr || !plugin_->hasEditor())
        return nullptr;
    return plugin_->createEditorIfNeeded();
}

juce::String SerumHost::getPluginName() const
{
    return (plugin_ != nullptr) ? plugin_->getName() : juce::String{};
}

} // namespace dsp
