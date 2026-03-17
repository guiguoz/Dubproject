#pragma once

#include <cmath>
#include <cstdint>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// Lowest frequency to detect (Bb2 on alto sax ≈ 116 Hz, rounded up for safety)
constexpr float kMinFrequencyHz   = 120.0f;

/// Highest frequency to detect (above soprano sax high range)
constexpr float kMaxFrequencyHz   = 1400.0f;

/// Default sample rate (actual value set via prepare())
constexpr double kDefaultSampleRate = 44100.0;

/// Maximum audio block size (samples)
constexpr int kMaxBlockSize = 512;

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

/// Convert semitones to a pitch ratio (e.g. +12 → 2.0, -12 → 0.5)
inline float semitonesToRatio(float semitones) noexcept
{
    return std::pow(2.0f, semitones / 12.0f);
}

/// Hard-clip a sample to [-1, 1]
inline float clipSample(float s) noexcept
{
    if (s >  1.0f) return  1.0f;
    if (s < -1.0f) return -1.0f;
    return s;
}

/// Linear interpolation between a and b at position t ∈ [0, 1]
inline float lerp(float a, float b, float t) noexcept
{
    return a + t * (b - a);
}

} // namespace dsp
