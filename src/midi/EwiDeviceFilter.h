#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace midi {

// Returns true if deviceName contains filterName (case-insensitive substring).
// Used by MidiManager to identify EWI input devices without JUCE dependency.
inline bool ewiDeviceMatches(std::string_view deviceName,
                              std::string_view filterName) noexcept
{
    if (filterName.empty()) return false;
    auto it = std::search(
        deviceName.begin(), deviceName.end(),
        filterName.begin(), filterName.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != deviceName.end();
}

// Normalises a raw CC value [0..127] to [0.0f..1.0f].
inline float normalizeBreathCc(int ccValue) noexcept
{
    return static_cast<float>(ccValue) / 127.f;
}

} // namespace midi
