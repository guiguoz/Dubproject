#pragma once

#include "ProjectData.h"

#include <optional>
#include <string>

namespace project {

// ─────────────────────────────────────────────────────────────────────────────
// ProjectLoader
//
// Serialises / deserialises ProjectData to/from a .saxfx JSON file.
// ─────────────────────────────────────────────────────────────────────────────
class ProjectLoader
{
public:
    static constexpr int kFormatVersion = 20;

    static std::optional<ProjectData> load(const std::string& filePath);
    static bool save(const ProjectData& data, const std::string& filePath);
};

} // namespace project
