#pragma once

#include <vector>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// PitchResult
// ─────────────────────────────────────────────────────────────────────────────
struct PitchResult
{
    float frequencyHz { 0.0f };  ///< 0 if unvoiced / no pitch detected
    float confidence  { 0.0f };  ///< 0.0–1.0 (1 - YIN d' minimum value)
};

// ─────────────────────────────────────────────────────────────────────────────
// YinPitchTracker
//
// Detects the fundamental frequency of a monophonic signal using the YIN
// algorithm (de Cheveigné & Kawahara, 2002).
//
// Designed to be realtime-safe: all memory is allocated in prepare(), never
// in process(). Uses an internal accumulator to collect enough samples for
// analysis across multiple small audio blocks.
// ─────────────────────────────────────────────────────────────────────────────
class YinPitchTracker
{
public:
    /// Call once from AudioAppComponent::prepareToPlay().
    /// Pre-allocates all internal buffers.
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    /// Feed a block of mono samples. Returns the latest detected pitch.
    /// - frequencyHz == 0 means unvoiced or insufficient data.
    /// - Realtime-safe: no allocation, no lock.
    PitchResult process(const float* input, int numSamples) noexcept;

    /// YIN threshold: lower = fewer false positives, higher = more detections.
    /// Typical range: 0.10 – 0.20. Default: 0.15.
    void setThreshold(float threshold) noexcept;

    void reset() noexcept;

private:
    // ── YIN sub-steps ─────────────────────────────────────────────────────
    void  differenceFunction(int windowSize) noexcept;
    void  cumulativeMeanNormalize(int windowSize) noexcept;
    int   absoluteThreshold(int windowSize) const noexcept;
    float parabolicInterpolation(int tauEstimate, int windowSize) const noexcept;

    // ── State ──────────────────────────────────────────────────────────────
    double sampleRate_   { 44100.0 };
    float  threshold_    { 0.15f };
    int    windowSize_   { 0 };   ///< = 2 × halfWindowSize (set in prepare)

    // Pre-allocated working buffers (no allocation in process())
    std::vector<float> yinBuffer_;        ///< d'(tau) values [windowSize/2]
    std::vector<float> inputAccumulator_; ///< ring accumulator [windowSize]
    int  accWritePos_    { 0 };
    int  accFilled_      { 0 };           ///< samples written since last reset

    PitchResult lastResult_; ///< cached result until next analysis
};

} // namespace dsp
