#include "SerumHost.h"

namespace {

static bool isBlacklisted(const juce::String& s)
{
    return s.startsWithIgnoreCase("prog ")
        || s.startsWithIgnoreCase("serum")
        || s.startsWithIgnoreCase("xfer")
        || s.startsWith("<")
        || s.startsWith("VST");
}

} // anonymous namespace

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

    bpmPlayHead_.sampleRate.store (sampleRate_, std::memory_order_relaxed);
    bpmPlayHead_.samplePos = 0;
    resetPositionFlag_.store (false, std::memory_order_relaxed);
    allNotesOffPending_.store (false, std::memory_order_relaxed);
    pendingPitchBendRange_.store (12, std::memory_order_relaxed);  // EWI default ±12 ST
    plugin_->setPlayHead (&bpmPlayHead_);

    DBG("SerumHost: latency = " << plugin_->getLatencySamples() << " samples");

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
        plugin_->setPlayHead (nullptr);
        plugin_->releaseResources();
        plugin_.reset();
    }

    cachedPresetName_  = {};
    presetNameDirty_   = true;
    presetNameSource_  = PresetNameSource::Auto;

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

    if (resetPositionFlag_.exchange (false, std::memory_order_relaxed))
        bpmPlayHead_.samplePos = 0;

    // Pending MIDI injections (message thread → audio thread via atomics)
    {
        juce::MidiBuffer extra;

        if (allNotesOffPending_.exchange (false, std::memory_order_relaxed))
        {
            for (int ch = 1; ch <= 16; ++ch)
                extra.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        }

        const int pbr = pendingPitchBendRange_.exchange (-1, std::memory_order_relaxed);
        if (pbr >= 0)
        {
            // RPN 0 = Pitch Bend Sensitivity
            extra.addEvent (juce::MidiMessage::controllerEvent (1, 101,   0), 0);
            extra.addEvent (juce::MidiMessage::controllerEvent (1, 100,   0), 0);
            extra.addEvent (juce::MidiMessage::controllerEvent (1,   6, pbr), 0);
            extra.addEvent (juce::MidiMessage::controllerEvent (1,  38,   0), 0);
            extra.addEvent (juce::MidiMessage::controllerEvent (1, 101, 127), 0);  // RPN null
            extra.addEvent (juce::MidiMessage::controllerEvent (1, 100, 127), 0);
        }

        for (const auto meta : extra)
            midi.addEvent (meta.getMessage(), meta.samplePosition);
    }

    serumBuffer_.clear();
    plugin_->processBlock(serumBuffer_, midi);

    bpmPlayHead_.samplePos += serumBuffer_.getNumSamples();

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

    // User-set name is authoritative — don't overwrite with scan results.
    if (presetNameSource_ == PresetNameSource::Manual)
    {
        presetNameDirty_ = false;
        return cachedPresetName_;
    }

    presetNameDirty_ = false;

    // ── 1. Standard API ───────────────────────────────────────────────────────
    {
        const auto n = plugin_->getProgramName(plugin_->getCurrentProgram());
        if (n.isNotEmpty() && !n.startsWithIgnoreCase("prog ") && !n.startsWithIgnoreCase("program")
            && n.length() >= 2)
        {
            cachedPresetName_ = n;
            return cachedPresetName_;
        }
    }

    // ── 2. VST3 state → JUCE XML wrapper → IEditController JSON → "presetName" ──
    // Serum V2 stores the preset name in the IEditController component as JSON:
    //   { "component": "controller", "presetName": "...", ... }
    // The outer state is a JUCE VST3PluginState XML, prefixed with an 8-byte binary
    // header ("VC2!" + 4-byte size). Each child element contains JUCE base64-encoded
    // binary. The binary itself starts with "XferJson\0" + 8-byte int64, followed by JSON.
    juce::MemoryBlock state;
    plugin_->getStateInformation(state);

    if (state.isEmpty()) { cachedPresetName_ = {}; return {}; }

    const char* raw = static_cast<const char*>(state.getData());
    const int   sz  = (int)state.getSize();

    // Find XML start (skip binary header)
    int xmlOffset = -1;
    for (int i = 0; i < std::min(sz - 1, 64); ++i)
        if (raw[i] == '<') { xmlOffset = i; break; }

    if (xmlOffset < 0) { cachedPresetName_ = {}; return {}; }

    auto xml = juce::XmlDocument::parse(juce::String::fromUTF8(raw + xmlOffset, sz - xmlOffset));
    if (!xml) { cachedPresetName_ = {}; return {}; }

    for (auto* child = xml->getFirstChildElement(); child != nullptr; child = child->getNextElement())
    {
        juce::MemoryBlock inner;
        if (!inner.fromBase64Encoding(child->getAllSubText())) continue;

        const char* p = static_cast<const char*>(inner.getData());
        const int   n = (int)inner.getSize();

        // Find JSON start (skip "XferJson\0" + 8-byte int64 header)
        int jsonStart = -1;
        if (n >= 9 && std::memcmp(p, "XferJson", 8) == 0)
        {
            for (int i = 9; i < std::min(n, 32); ++i)
                if (p[i] == '{') { jsonStart = i; break; }
        }
        else
        {
            for (int i = 0; i < std::min(n, 32); ++i)
                if (p[i] == '{') { jsonStart = i; break; }
        }
        if (jsonStart < 0) continue;

        const auto json = juce::String::fromUTF8(p + jsonStart, n - jsonStart);

        // Search for "presetName":"..." in the JSON (Serum V2 IEditController field)
        static constexpr const char kField[] = "\"presetName\":\"";
        const int idx = json.indexOf(kField);
        if (idx >= 0)
        {
            const int start = idx + (int)(sizeof(kField) - 1);
            const int end   = json.indexOf(start, "\"");
            if (end > start && (end - start) <= 64)
            {
                const auto val = json.substring(start, end).trim();
                if (val.isNotEmpty() && !isBlacklisted(val))
                {
                    cachedPresetName_ = val;
                    return cachedPresetName_;
                }
            }
        }
    }

    cachedPresetName_ = {};
    return {};
}

} // namespace dsp
