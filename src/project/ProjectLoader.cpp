#include "ProjectLoader.h"

#include <JuceHeader.h>

#include <algorithm>

namespace project {

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
    data.bpm         = getFloat(root, "bpm", 120.f);

    // Silent defaults for dub delay bus when missing from older projects
    if (!root.hasProperty("dubDelayEnabled")) {
        data.dubDelayEnabled  = false;
        data.dubDelaySend     = 0.20f;
        data.dubDelayWet      = 0.28f;
        data.dubDelayFeedback = 0.48f;
        data.dubDelayTone     = 0.55f;
        data.dubDelayDrive    = 0.15f;
        data.dubDelayDiv      = 1;
    }

    // ── Samples ───────────────────────────────────────────────────────────────
    if (const auto* samplesArr = root["samples"].getArray())
    {
        for (const auto& entry : *samplesArr)
        {
            const int slot = static_cast<int>(entry.getProperty("slot", -1));
            if (slot < 0 || slot >= 9) continue;
            auto& sc    = data.samples[static_cast<std::size_t>(slot)];
            sc.filePath = getString(entry, "path");
            sc.gain     = getFloat(entry,  "gain",    1.0f);
            sc.loop     = getBool(entry,   "loop",    false);
            sc.oneShot  = getBool(entry,   "oneShot", true);
            sc.muted    = getBool(entry,   "muted",   false);
            sc.gridDiv  = static_cast<int>(entry.getProperty("gridDiv", 1));

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

    // ── v5 — master key, AI mix states, scenes ───────────────────────────────
    if (data.version >= 5)
    {
        data.masterKeyRoot  = static_cast<int>(root.getProperty("masterKeyRoot",  0));
        data.masterKeyMajor = getBool(root, "masterKeyMajor", true);
        data.currentScene   = static_cast<int>(root.getProperty("currentScene",   0));

        if (const auto* mixArr = root["slotMix"].getArray())
        {
            for (const auto& entry : *mixArr)
            {
                const int slot = static_cast<int>(entry.getProperty("slot", -1));
                if (slot < 0 || slot >= 9) continue;
                auto& sm    = data.slotMix[static_cast<std::size_t>(slot)];
                sm.gain     = getFloat(entry, "gain",    1.f);
                sm.pan      = getFloat(entry, "pan",     0.f);
                sm.width    = getFloat(entry, "width",   0.f);
                sm.depth    = getFloat(entry, "depth",   0.f);
                sm.applied  = getBool(entry,  "applied", false);
            }
        }

        if (const auto* scenesArr = root["scenes"].getArray())
        {
            for (const auto& entry : *scenesArr)
            {
                const int si = static_cast<int>(entry.getProperty("index", -1));
                if (si < 0 || si >= 8) continue;
                auto& sc  = data.scenes[static_cast<std::size_t>(si)];
                sc.bpm    = getFloat(entry, "bpm", 120.f);
                sc.used   = getBool(entry,  "used", false);

                if (const auto* bcArr = entry["trackBarCounts"].getArray())
                    for (int i = 0; i < 9 && i < bcArr->size(); ++i)
                        sc.trackBarCounts[static_cast<std::size_t>(i)] =
                            juce::jlimit(1, 32, static_cast<int>((*bcArr)[i]));

                if (const auto* pathsArr = entry["filePaths"].getArray())
                    for (int i = 0; i < 9 && i < pathsArr->size(); ++i)
                        sc.filePaths[static_cast<std::size_t>(i)] =
                            (*pathsArr)[i].toString().toStdString();

                if (const auto* muteArr = entry["mutes"].getArray())
                    for (int i = 0; i < 9 && i < muteArr->size(); ++i)
                        sc.mutes[static_cast<std::size_t>(i)] =
                            static_cast<bool>((*muteArr)[i]);

                if (const auto* gainArr = entry["gains"].getArray())
                    for (int i = 0; i < 9 && i < gainArr->size(); ++i)
                        sc.gains[static_cast<std::size_t>(i)] =
                            static_cast<float>(static_cast<double>((*gainArr)[i]));

                if (const auto* tracksArr = entry["steps"].getArray())
                {
                    for (int t = 0; t < 9 && t < tracksArr->size(); ++t)
                    {
                        const int maxSteps = sc.trackBarCounts[static_cast<std::size_t>(t)] * 16;
                        if (const auto* stArr = (*tracksArr)[t].getArray())
                            for (int s = 0; s < maxSteps && s < stArr->size(); ++s)
                                sc.steps[static_cast<std::size_t>(t)]
                                        [static_cast<std::size_t>(s)] =
                                    static_cast<bool>((*stArr)[s]);
                    }
                }

                if (const auto* tsArr = entry["trimStart"].getArray())
                    for (int i = 0; i < 9 && i < tsArr->size(); ++i)
                        sc.trimStart[static_cast<std::size_t>(i)] =
                            static_cast<int>((*tsArr)[i]);

                if (const auto* teArr = entry["trimEnd"].getArray())
                    for (int i = 0; i < 9 && i < teArr->size(); ++i)
                    {
                        const int te = static_cast<int>((*teArr)[i]);
                        const int ts = sc.trimStart[static_cast<std::size_t>(i)];
                        // Reject degenerate trims: trimEnd too close to trimStart
                        // means the resulting buffer would be inaudibly short (< 100 samples ≈ 2 ms).
                        sc.trimEnd[static_cast<std::size_t>(i)] =
                            (te >= 0 && te - ts < 100) ? -1 : te;
                    }

                if (const auto* dsArr = entry["delaySends"].getArray())
                    for (int i = 0; i < 9 && i < dsArr->size(); ++i)
                        sc.delaySends[static_cast<std::size_t>(i)] =
                            juce::jlimit(0.f, 1.f, static_cast<float>((*dsArr)[i]));

                if (data.version >= 13)
                    if (const auto* ugArr = entry["userGains"].getArray())
                        for (int i = 0; i < 9 && i < ugArr->size(); ++i)
                            sc.userGains[static_cast<std::size_t>(i)] =
                                juce::jlimit(0.f, 2.f, static_cast<float>((*ugArr)[i]));

                // Reset AI-calibrated userGains that are too low to hear.
                // v<16: old gains (HAT 0.09). v16-18: gains from old magic mix runs where
                // targetGainForType had not yet been raised (SNR/HAT/PAD/PRC ≈ 0.22-0.39).
                // v19 = first version where projects are guaranteed clean.
                if (data.version < 19)
                    for (int i = 0; i < 9; ++i)
                        if (!sc.filePaths[static_cast<std::size_t>(i)].empty()
                            && sc.userGains[static_cast<std::size_t>(i)] < 0.50f)
                            sc.userGains[static_cast<std::size_t>(i)] = 1.0f;

                if (data.version >= 15)
                    sc.serumGain = juce::jlimit(0.f, 2.f,
                        static_cast<float>(entry.getProperty("serumGain", 1.0)));

                if (data.version >= 18)
                {
                    sc.serumState      = getString(entry, "serumState");
                    sc.serumPresetName = getString(entry, "serumPresetName");
                }
            }
        }

        // v18 migration: distribute root-level serumState/serumPresetName to used scenes
        if (data.version < 18)
        {
            for (auto& sc : data.scenes)
            {
                if (!sc.used) continue;
                if (sc.serumState.empty() && !data.serumState.empty())
                    sc.serumState = data.serumState;
                if (sc.serumPresetName.empty() && !data.serumPresetName.empty())
                    sc.serumPresetName = data.serumPresetName;
            }
        }
    }

    // ── v11 — dub delay global bus ────────────────────────────────────────────
    if (data.version >= 11)
    {
        data.dubDelayEnabled  = getBool (root, "dubDelayEnabled",  false);
        data.dubDelaySend     = getFloat(root, "dubDelaySend",     0.20f);
        data.dubDelayWet      = getFloat(root, "dubDelayWet",      0.28f);
        data.dubDelayFeedback = getFloat(root, "dubDelayFeedback", 0.48f);
        data.dubDelayTone     = getFloat(root, "dubDelayTone",     0.55f);
        data.dubDelayDrive    = getFloat(root, "dubDelayDrive",    0.15f);
        data.dubDelayDiv      = static_cast<int>(root.getProperty("dubDelayDiv", 1));
    }

    // ── v12 — MIDI learn bindings (backwards-compatible: absent = empty) ─────
    if (const auto* learnArr = root["midiLearn"].getArray())
    {
        for (const auto& entry : *learnArr)
        {
            MidiLearnEntry e;
            e.target = static_cast<int>(entry.getProperty("target", -1));
            e.cc     = static_cast<int>(entry.getProperty("cc",     -1));
            e.min    = getFloat(entry, "min", 0.f);
            e.max    = getFloat(entry, "max", 1.f);
            if (e.target >= 0 && e.cc >= 0 && e.cc < 128)
                data.midiLearnEntries.push_back(e);
        }
    }

    // ── v14 — Serum preset state (absent = empty, Serum not used) ────────────
    data.serumState = getString(root, "serumState");

    // ── v17 — Serum preset name (user-provided; absent = empty) ──────────────
    data.serumPresetName = getString(root, "serumPresetName");

    // swing (absent in older projects → 0.0 = straight, backwards-compat)
    data.swing = getFloat(root, "swing", 0.f);

    return data;
}

// ─────────────────────────────────────────────────────────────────────────────
// save
// ─────────────────────────────────────────────────────────────────────────────
bool ProjectLoader::save(const ProjectData& data, const std::string& filePath)
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("version",     kFormatVersion);
    root->setProperty("projectName", juce::String(data.projectName));
    root->setProperty("bpm",         static_cast<double>(data.bpm));

    // ── Samples ───────────────────────────────────────────────────────────────
    juce::Array<juce::var> samplesArr;
    for (int i = 0; i < 9; ++i)
    {
        const auto& sc = data.samples[static_cast<std::size_t>(i)];
        if (sc.filePath.empty()) continue;
        juce::DynamicObject::Ptr entry = new juce::DynamicObject();
        entry->setProperty("slot",    i);
        entry->setProperty("path",    juce::String(sc.filePath));
        entry->setProperty("gain",    static_cast<double>(sc.gain));
        entry->setProperty("loop",    sc.loop);
        entry->setProperty("oneShot", sc.oneShot);
        entry->setProperty("muted",   sc.muted);
        entry->setProperty("gridDiv", sc.gridDiv);
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

    // ── Music context ─────────────────────────────────────────────────────────
    {
        juce::DynamicObject::Ptr mc = new juce::DynamicObject();
        mc->setProperty("bpm",     static_cast<double>(data.musicContext.bpm));
        mc->setProperty("keyRoot", data.musicContext.keyRoot);
        mc->setProperty("isMajor", data.musicContext.isMajor);
        mc->setProperty("style",   data.musicContext.style);
        root->setProperty("musicContext", juce::var(mc.get()));
    }

    root->setProperty("masterKeyRoot",  data.masterKeyRoot);
    root->setProperty("masterKeyMajor", data.masterKeyMajor);
    root->setProperty("currentScene",   data.currentScene);

    // ── Slot mix states ───────────────────────────────────────────────────────
    {
        juce::Array<juce::var> mixArr;
        for (int i = 0; i < 9; ++i)
        {
            const auto& sm = data.slotMix[static_cast<std::size_t>(i)];
            if (!sm.applied) continue;
            juce::DynamicObject::Ptr entry = new juce::DynamicObject();
            entry->setProperty("slot",    i);
            entry->setProperty("gain",    static_cast<double>(sm.gain));
            entry->setProperty("pan",     static_cast<double>(sm.pan));
            entry->setProperty("width",   static_cast<double>(sm.width));
            entry->setProperty("depth",   static_cast<double>(sm.depth));
            entry->setProperty("applied", sm.applied);
            mixArr.add(juce::var(entry.get()));
        }
        root->setProperty("slotMix", mixArr);
    }

    // ── Scenes ────────────────────────────────────────────────────────────────
    {
        juce::Array<juce::var> scenesArr;
        for (int si = 0; si < 8; ++si)
        {
            const auto& sc = data.scenes[static_cast<std::size_t>(si)];
            if (!sc.used) continue;
            juce::DynamicObject::Ptr entry = new juce::DynamicObject();
            entry->setProperty("index", si);
            entry->setProperty("bpm",   static_cast<double>(sc.bpm));
            entry->setProperty("used",  sc.used);

            juce::Array<juce::var> barCountsArr;
            for (int i = 0; i < 9; ++i)
                barCountsArr.add(sc.trackBarCounts[static_cast<std::size_t>(i)]);
            entry->setProperty("trackBarCounts", barCountsArr);

            juce::Array<juce::var> pathsArr;
            for (int i = 0; i < 9; ++i)
                pathsArr.add(juce::String(sc.filePaths[static_cast<std::size_t>(i)]));
            entry->setProperty("filePaths", pathsArr);

            juce::Array<juce::var> muteArr;
            for (int i = 0; i < 9; ++i)
                muteArr.add(sc.mutes[static_cast<std::size_t>(i)]);
            entry->setProperty("mutes", muteArr);

            juce::Array<juce::var> gainArr;
            for (int i = 0; i < 9; ++i)
                gainArr.add(static_cast<double>(sc.gains[static_cast<std::size_t>(i)]));
            entry->setProperty("gains", gainArr);

            juce::Array<juce::var> tracksArr;
            for (int t = 0; t < 9; ++t)
            {
                const int numSteps = sc.trackBarCounts[static_cast<std::size_t>(t)] * 16;
                juce::Array<juce::var> stArr;
                for (int s = 0; s < numSteps; ++s)
                    stArr.add(sc.steps[static_cast<std::size_t>(t)]
                                      [static_cast<std::size_t>(s)]);
                tracksArr.add(stArr);
            }
            entry->setProperty("steps", tracksArr);

            juce::Array<juce::var> trimStartArr, trimEndArr;
            for (int i = 0; i < 9; ++i)
            {
                trimStartArr.add(sc.trimStart[static_cast<std::size_t>(i)]);
                trimEndArr  .add(sc.trimEnd  [static_cast<std::size_t>(i)]);
            }
            entry->setProperty("trimStart", trimStartArr);
            entry->setProperty("trimEnd",   trimEndArr);

            juce::Array<juce::var> delaySendsArr;
            for (int i = 0; i < 9; ++i)
                delaySendsArr.add(sc.delaySends[static_cast<std::size_t>(i)]);
            entry->setProperty("delaySends", delaySendsArr);

            juce::Array<juce::var> userGainsArr;
            for (int i = 0; i < 9; ++i)
                userGainsArr.add(static_cast<double>(sc.userGains[static_cast<std::size_t>(i)]));
            entry->setProperty("userGains",  userGainsArr);
            entry->setProperty("serumGain",  static_cast<double>(sc.serumGain));

            if (!sc.serumState.empty())
                entry->setProperty("serumState", juce::String(sc.serumState));
            if (!sc.serumPresetName.empty())
                entry->setProperty("serumPresetName", juce::String(sc.serumPresetName));

            scenesArr.add(juce::var(entry.get()));
        }
        root->setProperty("scenes", scenesArr);
    }

    // ── v11 — dub delay global bus ────────────────────────────────────────────
    root->setProperty("dubDelayEnabled",  data.dubDelayEnabled);
    root->setProperty("dubDelaySend",     static_cast<double>(data.dubDelaySend));
    root->setProperty("dubDelayWet",      static_cast<double>(data.dubDelayWet));
    root->setProperty("dubDelayFeedback", static_cast<double>(data.dubDelayFeedback));
    root->setProperty("dubDelayTone",     static_cast<double>(data.dubDelayTone));
    root->setProperty("dubDelayDrive",    static_cast<double>(data.dubDelayDrive));
    root->setProperty("dubDelayDiv",      data.dubDelayDiv);

    // ── v12 — MIDI learn bindings ─────────────────────────────────────────────
    {
        juce::Array<juce::var> learnArr;
        for (const auto& e : data.midiLearnEntries)
        {
            juce::DynamicObject::Ptr entry = new juce::DynamicObject();
            entry->setProperty("target", e.target);
            entry->setProperty("cc",     e.cc);
            entry->setProperty("min",    static_cast<double>(e.min));
            entry->setProperty("max",    static_cast<double>(e.max));
            learnArr.add(juce::var(entry.get()));
        }
        root->setProperty("midiLearn", learnArr);
    }

    // serumState / serumPresetName moved to per-scene storage in v18 — not written at root level.

    // swing (always saved; readers default to 0 when absent for backwards-compat)
    if (data.swing > 0.f)
        root->setProperty("swing", static_cast<double>(data.swing));

    const juce::String json = juce::JSON::toString(juce::var(root.get()), true);
    return juce::File(juce::String(filePath)).replaceWithText(json);
}

} // namespace project
