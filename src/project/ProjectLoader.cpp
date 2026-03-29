#include "ProjectLoader.h"

#include "dsp/EffectChain.h"
#include "dsp/EffectFactory.h"

#include <JuceHeader.h>

#include <algorithm>

namespace project {

// ─────────────────────────────────────────────────────────────────────────────
// JSON helpers
// ─────────────────────────────────────────────────────────────────────────────
static float getFloat(const juce::var& obj, const char* key, float def)
{
    return static_cast<float>(static_cast<double>(obj.getProperty(key, def)));
}

static bool getBool(const juce::var& obj, const char* key, bool def)
{
    return static_cast<bool>(obj.getProperty(key, def));
}

static std::string getString(const juce::var& obj, const char* key,
                              const char* def = "")
{
    return obj.getProperty(key, def).toString().toStdString();
}

// ─────────────────────────────────────────────────────────────────────────────
// Migration: v1 → v2
// ─────────────────────────────────────────────────────────────────────────────
static void migrateV1Effects(const juce::var& root, ProjectData& data)
{
    const auto& fx = root["effects"];
    if (!fx.isObject()) return;

    // Harmonizer slot
    {
        EffectSlotData slot;
        slot.type    = "Harmonizer";
        slot.enabled = getBool(fx, "harmonizerEnabled", false);
        slot.params  = {
            getFloat(fx, "harmVoice0Interval", 3.0f),
            getFloat(fx, "harmVoice1Interval", -5.0f),
            getFloat(fx, "harmMix",             0.5f)
        };
        data.effectChain.push_back(std::move(slot));
    }

    // Flanger slot
    {
        EffectSlotData slot;
        slot.type    = "Flanger";
        slot.enabled = getBool(fx, "flangerEnabled", false);
        slot.params  = {
            getFloat(fx, "flangerRate",     0.5f),
            getFloat(fx, "flangerDepth",    0.7f),
            getFloat(fx, "flangerFeedback", 0.3f),
            getFloat(fx, "flangerMix",      0.5f)
        };
        data.effectChain.push_back(std::move(slot));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// load
// ─────────────────────────────────────────────────────────────────────────────
std::optional<ProjectData> ProjectLoader::load(const std::string& filePath)
{
    juce::File file { juce::String(filePath) };
    if (!file.existsAsFile()) return std::nullopt;

    const juce::var root = juce::JSON::parse(file.loadFileAsString());
    if (!root.isObject()) return std::nullopt;

    ProjectData data;
    data.projectName = getString(root, "projectName", "Untitled");
    data.version     = static_cast<int>(root.getProperty("version", 1));
    data.bpm         = getFloat(root, "bpm", 120.f);  // v4

    // ── Effect chain ──────────────────────────────────────────────────────────
    if (data.version >= 2)
    {
        if (const auto* arr = root["effectChain"].getArray())
        {
            for (const auto& entry : *arr)
            {
                EffectSlotData slot;
                slot.type      = getString(entry, "type");
                slot.enabled   = getBool(entry,   "enabled",   true);
                slot.aiManaged = getBool(entry,   "aiManaged", false);  // v3

                if (const auto* paramsArr = entry["params"].getArray())
                    for (const auto& p : *paramsArr)
                        slot.params.push_back(static_cast<float>(static_cast<double>(p)));

                if (!slot.type.empty())
                    data.effectChain.push_back(std::move(slot));
            }
        }
    }
    else
    {
        // v1 compatibility
        migrateV1Effects(root, data);
        data.version = 2;  // upgrade in memory
    }

    // ── Samples ───────────────────────────────────────────────────────────────
    if (const auto* samplesArr = root["samples"].getArray())
    {
        for (const auto& entry : *samplesArr)
        {
            const int slot = static_cast<int>(entry.getProperty("slot", -1));
            if (slot < 0 || slot >= 8) continue;
            auto& sc    = data.samples[static_cast<std::size_t>(slot)];
            sc.filePath = getString(entry, "path");
            sc.gain     = getFloat(entry,  "gain",    1.0f);
            sc.loop     = getBool(entry,   "loop",    false);
            sc.oneShot  = getBool(entry,   "oneShot", true);
            sc.muted    = getBool(entry,   "muted",   false);                   // v3
            sc.gridDiv  = static_cast<int>(entry.getProperty("gridDiv", 1));    // v3

            // v4 — step pattern
            if (const auto* stepsArr = entry["steps"].getArray())
            {
                for (int s = 0; s < 16 && s < stepsArr->size(); ++s)
                    sc.stepPattern[s] = static_cast<bool>((*stepsArr)[s]);
            }
        }
    }

    // ── MIDI mappings ─────────────────────────────────────────────────────────
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

    // ── Music context (v3) ────────────────────────────────────────────────────
    if (data.version >= 3)
    {
        const auto& mc = root["musicContext"];
        if (mc.isObject())
        {
            data.musicContext.bpm     = getFloat(mc, "bpm",     0.f);
            data.musicContext.keyRoot = static_cast<int>(mc.getProperty("keyRoot", -1));
            data.musicContext.isMajor = getBool(mc,  "isMajor", true);
            data.musicContext.style   = static_cast<int>(mc.getProperty("style", 0));
        }
    }

    return data;
}

// ─────────────────────────────────────────────────────────────────────────────
// save
// ─────────────────────────────────────────────────────────────────────────────
bool ProjectLoader::save(const ProjectData& data, const std::string& filePath)
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("version",     4);  // always write latest format
    root->setProperty("projectName", juce::String(data.projectName));
    root->setProperty("bpm",         static_cast<double>(data.bpm));  // v4

    // ── Effect chain ──────────────────────────────────────────────────────────
    juce::Array<juce::var> chainArr;
    for (const auto& slot : data.effectChain)
    {
        juce::DynamicObject::Ptr entry = new juce::DynamicObject();
        entry->setProperty("type",      juce::String(slot.type));
        entry->setProperty("enabled",   slot.enabled);
        entry->setProperty("aiManaged", slot.aiManaged);  // v3

        juce::Array<juce::var> paramsArr;
        for (float p : slot.params)
            paramsArr.add(static_cast<double>(p));
        entry->setProperty("params", paramsArr);

        chainArr.add(juce::var(entry.get()));
    }
    root->setProperty("effectChain", chainArr);

    // ── Samples ───────────────────────────────────────────────────────────────
    juce::Array<juce::var> samplesArr;
    for (int i = 0; i < 8; ++i)
    {
        const auto& sc = data.samples[static_cast<std::size_t>(i)];
        if (sc.filePath.empty()) continue;
        juce::DynamicObject::Ptr entry = new juce::DynamicObject();
        entry->setProperty("slot",    i);
        entry->setProperty("path",    juce::String(sc.filePath));
        entry->setProperty("gain",    static_cast<double>(sc.gain));
        entry->setProperty("loop",    sc.loop);
        entry->setProperty("oneShot", sc.oneShot);
        entry->setProperty("muted",   sc.muted);           // v3
        entry->setProperty("gridDiv", sc.gridDiv);         // v3

        // v4 — step pattern (always write 16 booleans)
        {
            juce::Array<juce::var> stepsArr;
            for (int s = 0; s < 16; ++s)
                stepsArr.add(sc.stepPattern[s]);
            entry->setProperty("steps", stepsArr);
        }

        samplesArr.add(juce::var(entry.get()));
    }
    root->setProperty("samples", samplesArr);

    // ── MIDI mappings ─────────────────────────────────────────────────────────
    juce::Array<juce::var> mappingsArr;
    for (const auto& m : data.midiMappings)
    {
        juce::DynamicObject::Ptr entry = new juce::DynamicObject();
        entry->setProperty("note", m.midiNote);
        entry->setProperty("slot", m.slotIndex);
        mappingsArr.add(juce::var(entry.get()));
    }
    root->setProperty("midiMappings", mappingsArr);

    // ── Music context (v3) ────────────────────────────────────────────────────
    {
        juce::DynamicObject::Ptr mc = new juce::DynamicObject();
        mc->setProperty("bpm",     static_cast<double>(data.musicContext.bpm));
        mc->setProperty("keyRoot", data.musicContext.keyRoot);
        mc->setProperty("isMajor", data.musicContext.isMajor);
        mc->setProperty("style",   data.musicContext.style);
        root->setProperty("musicContext", juce::var(mc.get()));
    }

    const juce::String json = juce::JSON::toString(juce::var(root.get()), true);
    return juce::File(juce::String(filePath)).replaceWithText(json);
}

// ─────────────────────────────────────────────────────────────────────────────
// captureChain — snapshot live EffectChain → ProjectData
// ─────────────────────────────────────────────────────────────────────────────
void ProjectLoader::captureChain(const ::dsp::EffectChain& chain,
                                  ProjectData& data,
                                  const std::vector<bool>& aiManagedFlags) noexcept
{
    data.effectChain.clear();
    // EffectChain::getEffect is non-const; use const_cast (GUI thread only)
    auto& mutableChain = const_cast<::dsp::EffectChain&>(chain);
    const int count = mutableChain.effectCount();
    for (int i = 0; i < count; ++i)
    {
        ::dsp::IEffect* fx = mutableChain.getEffect(i);
        if (!fx) continue;

        EffectSlotData slot;
        slot.type      = ::dsp::effectTypeName(fx->type());
        slot.enabled   = fx->enabled.load(std::memory_order_acquire);
        slot.aiManaged = (i < static_cast<int>(aiManagedFlags.size()))
                             ? aiManagedFlags[static_cast<std::size_t>(i)]
                             : false;
        const int pc = fx->paramCount();
        slot.params.resize(static_cast<std::size_t>(pc));
        for (int p = 0; p < pc; ++p)
            slot.params[static_cast<std::size_t>(p)] = fx->getParam(p);

        data.effectChain.push_back(std::move(slot));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// applyChain — recreate IEffect objects and populate EffectChain
// ─────────────────────────────────────────────────────────────────────────────
void ProjectLoader::applyChain(const ProjectData& data,
                                ::dsp::EffectChain&  chain,
                                bool clearFirst) noexcept
{
    if (clearFirst)
    {
        // Remove all current effects (from last to first to keep indices valid)
        for (int i = chain.effectCount() - 1; i >= 0; --i)
            chain.removeEffect(i);
        chain.collectGarbage();
    }

    for (const auto& slot : data.effectChain)
    {
        auto fx = ::dsp::createEffect(slot.type);
        if (!fx) continue;

        // Apply saved params
        const int pc = fx->paramCount();
        for (int p = 0; p < pc && p < static_cast<int>(slot.params.size()); ++p)
            fx->setParam(p, slot.params[static_cast<std::size_t>(p)]);

        fx->enabled.store(slot.enabled, std::memory_order_relaxed);

        chain.addEffect(std::move(fx));
    }
}

} // namespace project
