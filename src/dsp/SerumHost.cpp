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
bool SerumHost::load(const juce::String& vst3Path, double sampleRate, int blockSize,
                     juce::String* outError)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    auto setErr = [&](const juce::String& msg)
    {
        juce::Logger::writeToLog("SerumHost: " + msg);
        if (outError != nullptr) *outError = msg;
    };

    unload();

    sampleRate_ = sampleRate;
    blockSize_  = blockSize;

    juce::OwnedArray<juce::PluginDescription> results;
    juce::VST3PluginFormat vst3;

    vst3.findAllTypesForFile(results, vst3Path);

    if (results.isEmpty())
    {
        setErr("no VST3 found at: " + vst3Path);
        return false;
    }

    juce::String instError;
    plugin_ = formatManager_.createPluginInstance(
        *results[0], sampleRate_, blockSize_, instError);

    if (plugin_ == nullptr)
    {
        setErr("instantiation failed — " + instError);
        return false;
    }

    plugin_->prepareToPlay(sampleRate_, blockSize_);
    serumBuffer_.setSize(2, blockSize_, false, true, false);

    plugin_->addListener(this);
    presetNameDirty_ = true;

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
        plugin_->removeListener(this);
        plugin_->releaseResources();
        plugin_.reset();
    }

    cachedPresetName_ = {};
    presetNameDirty_  = true;

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

juce::String SerumHost::getCurrentPresetName() const
{
    if (plugin_ == nullptr) return {};

    if (!presetNameDirty_)
        return cachedPresetName_;

    presetNameDirty_ = false;

    // ── 1. Standard API — only trust it if it looks like a real name ─────────
    {
        const auto n = plugin_->getProgramName(plugin_->getCurrentProgram());
        if (n.isNotEmpty() && !n.startsWith("prog ") && !n.startsWithIgnoreCase("program")
            && n.length() >= 2)
        {
            cachedPresetName_ = n;
            return cachedPresetName_;
        }
    }

    // ── 2. State data — Serum stores the preset name in its serialised state ─
    juce::MemoryBlock state;
    plugin_->getStateInformation(state);

    if (!state.isEmpty())
    {
        const char* raw = static_cast<const char*>(state.getData());
        // Only inspect the first 512 bytes — avoids reading wavetable data.
        const int   sz  = (int)std::min(state.getSize(), (size_t)512);

        // XML attribute search ("presetName=", "name=", …)
        const auto text = juce::String::fromUTF8(raw, sz);
        for (const char* attr : { "presetName=\"", "name=\"", "PresetName=\"", "patch_name=\"" })
        {
            const int idx = text.indexOf(attr);
            if (idx >= 0)
            {
                const int start = idx + (int)std::strlen(attr);
                const int end   = text.indexOf(start, "\"");
                if (end > start && (end - start) <= 64)
                {
                    auto candidate = text.substring(start, end);
                    if (candidate.isNotEmpty())
                    {
                        cachedPresetName_ = candidate;
                        return cachedPresetName_;
                    }
                }
            }
        }

        // Binary scan — look for the longest printable ASCII string in the header
        juce::String best;
        int i = 4;
        while (i < sz)
        {
            const auto c = static_cast<unsigned char>(raw[i]);
            if (c >= 32 && c < 127)
            {
                const int start = i;
                while (i < sz && static_cast<unsigned char>(raw[i]) >= 32
                               && static_cast<unsigned char>(raw[i]) < 127)
                    ++i;
                const int len = i - start;
                if (len >= 3 && len <= 64)
                {
                    juce::String s(raw + start, len);
                    if (!s.containsOnly("0123456789.+-=_") &&
                        !s.startsWith("<") && !s.startsWith("VST") &&
                        !s.startsWith("Xfer") && len > best.length())
                        best = s;
                }
            }
            else { ++i; }
        }

        DBG("SerumHost: state scan best candidate = \"" + best + "\"");

        if (best.isNotEmpty())
        {
            cachedPresetName_ = best;
            return cachedPresetName_;
        }
    }

    cachedPresetName_ = {};
    return {};
}

} // namespace dsp
