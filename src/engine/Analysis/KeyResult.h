#pragma once

#include <array>

namespace engine::analysis
{

// ─────────────────────────────────────────────────────────────────────────────
// KeyResult — output of KeyDetector.
// ─────────────────────────────────────────────────────────────────────────────
struct KeyResult
{
    int   key        = -1;  // 0=C .. 11=B ; -1 = unknown (not enough data)
    int   mode       = 0;   // 0 = major, 1 = minor
    float confidence = 0.f; // Pearson correlation [0,1]; >= 0.7 = reliable

    // Absolute pitch classes (0..11) of the 7 diatonic scale degrees
    std::array<int, 7> scaleDegrees{};
};

} // namespace engine::analysis
