#pragma once

#include "ProjectData.h"

#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <functional>

// Forward declarations using global namespace qualifier to avoid juce::dsp ambiguity
namespace dsp { class EffectChain; }

namespace project {

// ─────────────────────────────────────────────────────────────────────────────
// ProjectLoader
//
// Serialises / deserialises ProjectData to/from a .saxfx JSON file (v2 format).
// Uses juce::JSON (linked via juce_core).
//
// Format v2 replaces the old hardcoded "effects" object with a dynamic
// "effectChain" array:
//
//   {
//     "version": 2,
//     "projectName": "My Patch",
//     "effectChain": [
//       { "type": "Harmonizer", "enabled": true,
//         "params": [4.0, 7.0, 0.45] },
//       { "type": "Delay", "enabled": true,
//         "params": [375.0, 0.35, 0.25] }
//     ],
//     "samples": [...],
//     "midiMappings": [...]
//   }
//
// Backward compatibility: v1 files (with "effects" object) are automatically
// migrated to v2 on load (harmonizer + flanger only, other effects default).
// ─────────────────────────────────────────────────────────────────────────────
class ProjectLoader
{
public:
    /// Parse a .saxfx file.  Returns nullopt on I/O or parse error.
    static std::optional<ProjectData> load(const std::string& filePath);

    /// Write a .saxfx file.  Returns false on I/O error.
    static bool save(const ProjectData& data, const std::string& filePath);

    /// Snapshot the current EffectChain into a ProjectData's effectChain field.
    /// aiManagedFlags[i] = true when effect i was set by SmartMixEngine.
    /// Call before save() to capture the live chain state.
    static void captureChain(const ::dsp::EffectChain& chain,
                              ProjectData& data,
                              const std::vector<bool>& aiManagedFlags = {}) noexcept;

    /// Recreate IEffect objects from data and add them to chain.
    /// chain must be empty (or will be cleared first if clearFirst = true).
    /// Requires JUCE to be linked (ReverbEffect included in factory).
    static void applyChain(const ProjectData& data,
                            ::dsp::EffectChain& chain,
                            bool clearFirst = true) noexcept;
};

} // namespace project
