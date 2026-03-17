#include "ProjectLoader.h"
#include <JuceHeader.h>

namespace project {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static float getFloat(const juce::var& obj, const char* key, float defaultVal)
{
    return static_cast<float>(
        static_cast<double>(obj.getProperty(key, defaultVal)));
}

static bool getBool(const juce::var& obj, const char* key, bool defaultVal)
{
    return static_cast<bool>(obj.getProperty(key, defaultVal));
}

static std::string getString(const juce::var& obj, const char* key,
                              const char* defaultVal = "")
{
    return obj.getProperty(key, defaultVal).toString().toStdString();
}

// ─────────────────────────────────────────────────────────────────────────────
// load
// ─────────────────────────────────────────────────────────────────────────────
std::optional<ProjectData> ProjectLoader::load(const std::string& filePath)
{
    juce::String pathStr { filePath };
    juce::File   file    { pathStr };
    if (!file.existsAsFile())
        return std::nullopt;

    const juce::String content = file.loadFileAsString();
    const juce::var    root    = juce::JSON::parse(content);

    if (!root.isObject())
        return std::nullopt;

    ProjectData data;
    data.projectName = getString(root, "projectName", "Untitled");

    // Samples
    if (const auto* samplesArr = root["samples"].getArray())
    {
        for (const auto& entry : *samplesArr)
        {
            const int slot = static_cast<int>(entry.getProperty("slot", -1));
            if (slot < 0 || slot >= 8) continue;

            auto& sc    = data.samples[static_cast<std::size_t>(slot)];
            sc.filePath = getString(entry, "path");
            sc.gain     = getFloat(entry, "gain",    1.0f);
            sc.loop     = getBool (entry, "loop",    false);
            sc.oneShot  = getBool (entry, "oneShot", true);
        }
    }

    // MIDI mappings
    if (const auto* mappingsArr = root["midiMappings"].getArray())
    {
        for (const auto& entry : *mappingsArr)
        {
            MidiMapping m;
            m.midiNote  = static_cast<int>(entry.getProperty("note", -1));
            m.slotIndex = static_cast<int>(entry.getProperty("slot", -1));
            if (m.midiNote >= 0 && m.slotIndex >= 0)
                data.midiMappings.push_back(m);
        }
    }

    // Effects
    const auto& fx       = root["effects"];
    auto&       ep       = data.effects;
    ep.harmonizerEnabled  = getBool (fx, "harmonizerEnabled",  false);
    ep.harmVoice0Interval = getFloat(fx, "harmVoice0Interval", 3.0f);
    ep.harmVoice1Interval = getFloat(fx, "harmVoice1Interval", -5.0f);
    ep.harmMix            = getFloat(fx, "harmMix",            0.5f);
    ep.flangerEnabled     = getBool (fx, "flangerEnabled",     false);
    ep.flangerRate        = getFloat(fx, "flangerRate",        0.5f);
    ep.flangerDepth       = getFloat(fx, "flangerDepth",       0.7f);
    ep.flangerFeedback    = getFloat(fx, "flangerFeedback",    0.3f);
    ep.flangerMix         = getFloat(fx, "flangerMix",         0.5f);

    return data;
}

// ─────────────────────────────────────────────────────────────────────────────
// save
// ─────────────────────────────────────────────────────────────────────────────
bool ProjectLoader::save(const ProjectData& data, const std::string& filePath)
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("projectName", juce::String(data.projectName));

    // Samples array
    juce::Array<juce::var> samplesArr;
    for (int i = 0; i < 8; ++i)
    {
        const auto& sc = data.samples[static_cast<std::size_t>(i)];
        if (sc.filePath.empty()) continue;

        juce::DynamicObject::Ptr entry = new juce::DynamicObject();
        entry->setProperty("slot",    i);
        entry->setProperty("path",    juce::String(sc.filePath));
        entry->setProperty("gain",    sc.gain);
        entry->setProperty("loop",    sc.loop);
        entry->setProperty("oneShot", sc.oneShot);
        samplesArr.add(juce::var(entry.get()));
    }
    root->setProperty("samples", samplesArr);

    // MIDI mappings array
    juce::Array<juce::var> mappingsArr;
    for (const auto& m : data.midiMappings)
    {
        juce::DynamicObject::Ptr entry = new juce::DynamicObject();
        entry->setProperty("note", m.midiNote);
        entry->setProperty("slot", m.slotIndex);
        mappingsArr.add(juce::var(entry.get()));
    }
    root->setProperty("midiMappings", mappingsArr);

    // Effects
    const auto& ep = data.effects;
    juce::DynamicObject::Ptr fx = new juce::DynamicObject();
    fx->setProperty("harmonizerEnabled",  ep.harmonizerEnabled);
    fx->setProperty("harmVoice0Interval", ep.harmVoice0Interval);
    fx->setProperty("harmVoice1Interval", ep.harmVoice1Interval);
    fx->setProperty("harmMix",            ep.harmMix);
    fx->setProperty("flangerEnabled",     ep.flangerEnabled);
    fx->setProperty("flangerRate",        ep.flangerRate);
    fx->setProperty("flangerDepth",       ep.flangerDepth);
    fx->setProperty("flangerFeedback",    ep.flangerFeedback);
    fx->setProperty("flangerMix",         ep.flangerMix);
    root->setProperty("effects", juce::var(fx.get()));

    const juce::String json = juce::JSON::toString(juce::var(root.get()), true);
    juce::String pathStr2 { filePath };
    juce::File   file     { pathStr2 };
    return file.replaceWithText(json);
}

} // namespace project
