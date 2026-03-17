#pragma once

#include "ProjectData.h"
#include <optional>
#include <string>

namespace project {

// ─────────────────────────────────────────────────────────────────────────────
// ProjectLoader
//
// Serialises / deserialises ProjectData to/from a .saxfx JSON file.
// Uses juce::JSON (already linked via juce_core).
// ─────────────────────────────────────────────────────────────────────────────
class ProjectLoader
{
public:
    // Returns nullopt on parse error or missing mandatory fields.
    static std::optional<ProjectData> load(const std::string& filePath);

    // Returns false on I/O error.
    static bool save(const ProjectData& data, const std::string& filePath);
};

} // namespace project
