#pragma once
#include "engine/SceneStore.h"
#include "project/ProjectData.h"

namespace project {

// Converts engine::SceneData → SceneSaveData (save path).
// Pure C++, no JUCE dependency. Used by saveProjectToFile and EngineTests.
inline SceneSaveData toSaveData(const engine::SceneData& src)
{
    SceneSaveData dst;
    dst.used             = src.used;
    dst.bpm              = static_cast<float>(src.bpm);
    dst.trackBarCounts   = src.trackBarCounts;
    dst.serumGain        = src.serumGain;
    dst.serumState       = src.serumState;
    dst.serumPresetName  = src.serumPresetName;
    dst.dubDelayFeedback = src.dubDelayFeedback;
    dst.dubDelayWet      = src.dubDelayWet;
    dst.dubDelayTone     = src.dubDelayTone;
    dst.dubDelayDrive    = src.dubDelayDrive;
    dst.sceneOwned       = src.sceneOwned;
    for (int t = 0; t < engine::kMaxSlots; ++t)
    {
        const auto st = static_cast<std::size_t>(t);
        dst.filePaths[st]         = src.slots[st].filePath;
        dst.gains[st]             = src.slots[st].gain;
        dst.userGains[st]         = src.slots[st].userGain;
        dst.mutes[st]             = src.slots[st].muted;
        dst.trimStart[st]         = src.slots[st].trimStart;
        dst.trimEnd[st]           = src.slots[st].trimEnd;
        dst.delaySends[st]        = src.slots[st].delaySend;
        dst.pitchOffsets[st]      = src.slots[st].semitones;
        dst.playModeOverrides[st] = src.playModeOverrides[st];
        dst.slotRoles[st]         = (src.slots[st].role == engine::SlotRole::Unknown)
                                    ? -1 : static_cast<int>(src.slots[st].role);
        dst.roleManual[st]        = src.slots[st].isRoleManual;
        dst.slotOwnership[st]     = src.slotOwnership[st];
        const int n = src.trackBarCounts[st] * 16;
        for (int s = 0; s < n; ++s)
            dst.steps[st][static_cast<std::size_t>(s)] = src.steps[st][static_cast<std::size_t>(s)];
    }
    return dst;
}

// Converts SceneSaveData → engine::SceneData (load path).
// Pure C++, no JUCE dependency. Used by loadProjectFromFile and EngineTests.
// slotRoles defaults (-1) map to Unknown, so v<24 files are handled transparently.
inline engine::SceneData fromSaveData(const SceneSaveData& src)
{
    engine::SceneData dst;
    dst.used             = src.used;
    dst.bpm              = static_cast<int>(src.bpm);
    dst.trackBarCounts   = src.trackBarCounts;
    dst.serumGain        = src.serumGain;
    dst.serumState       = src.serumState;
    dst.serumPresetName  = src.serumPresetName;
    dst.dubDelayFeedback = src.dubDelayFeedback;
    dst.dubDelayWet      = src.dubDelayWet;
    dst.dubDelayTone     = src.dubDelayTone;
    dst.dubDelayDrive    = src.dubDelayDrive;
    dst.sceneOwned       = src.sceneOwned;
    for (int t = 0; t < engine::kMaxSlots; ++t)
    {
        const auto st = static_cast<std::size_t>(t);
        dst.slots[st].filePath    = src.filePaths[st];
        dst.slots[st].gain        = src.gains[st];
        dst.slots[st].userGain    = src.userGains[st];
        dst.slots[st].muted       = src.mutes[st];
        dst.slots[st].trimStart   = src.trimStart[st];
        dst.slots[st].trimEnd     = src.trimEnd[st];
        dst.slots[st].delaySend   = src.delaySends[st];
        dst.slots[st].semitones   = src.pitchOffsets[st];
        dst.playModeOverrides[st] = src.playModeOverrides[st];
        const int rawRole = src.slotRoles[st];
        dst.slots[st].role        = (rawRole < 0 || rawRole > 254)
                                    ? engine::SlotRole::Unknown
                                    : static_cast<engine::SlotRole>(rawRole);
        dst.slots[st].isRoleManual = src.roleManual[st];
        dst.slotOwnership[st]     = src.slotOwnership[st];
        const int n = src.trackBarCounts[st] * 16;
        for (int s = 0; s < n; ++s)
            dst.steps[st][static_cast<std::size_t>(s)] = src.steps[st][static_cast<std::size_t>(s)];
    }
    return dst;
}

} // namespace project
