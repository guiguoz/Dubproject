#pragma once

#include <array>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// KeyResult — output shared by ScaleHarmonizer and KeyDetector.
// ─────────────────────────────────────────────────────────────────────────────
struct KeyResult
{
    int key  = -1; // 0=C .. 11=B ; -1 = unknown (not enough data)
    int mode = 0;  // 0 = major, 1 = minor

    // Absolute pitch classes (0..11) of the 7 diatonic scale degrees
    std::array<int, 7> scaleDegrees{};
};

} // namespace dsp
