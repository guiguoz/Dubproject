#pragma once

#include <array>
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
// YinPitchTracker  (v2)
//
// Detects the fundamental frequency of a monophonic signal using the YIN
// algorithm (de Cheveigné & Kawahara, 2002).
//
// v2 improvements over v1:
//   • DC blocker (~40 Hz) removes rumble/DC offset before analysis
//   • Hop-based analysis with max 1 analysis per process() call (CPU-safe)
//   • Removed dangerous "global minimum" fallback that invented pitch in noise
//   • 5-tap median filter on voiced detections (kills outlier jitter)
//   • Configurable minFrequency (default 85 Hz for alto/ténor sax)
//   • YIN threshold relaxed to 0.20 — real gating done in DspPipeline
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
    /// - At most 1 YIN analysis per call (realtime-safe, no CPU spikes).
    /// - Realtime-safe: no allocation, no lock.
    PitchResult process(const float* input, int numSamples) noexcept;

    /// YIN threshold: lower = fewer false positives, higher = more detections.
    /// Typical range: 0.10 – 0.25. Default: 0.20.
    void setThreshold(float threshold) noexcept;

    /// Minimum detectable frequency. Must be called BEFORE prepare().
    /// Default 85 Hz (Alto/Ténor sax). For baritone use ~55 Hz.
    void setMinFrequency(float hz) noexcept;

    void reset() noexcept;

private:
    // ── YIN sub-steps ─────────────────────────────────────────────────────
    void  differenceFunction(int windowSize) noexcept;
    void  cumulativeMeanNormalize(int windowSize) noexcept;
    int   absoluteThreshold(int windowSize) const noexcept;
    float parabolicInterpolation(int tauEstimate, int windowSize) const noexcept;

    // ── State ──────────────────────────────────────────────────────────────
    double sampleRate_   { 44100.0 };
    float  threshold_    { 0.20f };    ///< Permissive — pipeline gates via confidence
    float  minFrequency_ { 85.0f };    ///< Configurable via setMinFrequency()
    int    windowSize_   { 0 };        ///< = 2 × halfWindowSize (set in prepare)
    int    hopSize_      { 192 };      ///< ~4ms @ 48kHz, min 128
    int    samplesSinceLastAnalysis_ { 0 };

    // DC blocker (~40 Hz, coefficient computed in prepare from sampleRate)
    float  hpfR_         { 0.995f };   ///< exp(-2π·fc/fs), fc=40 Hz
    float  hpfPrevIn_    { 0.0f };     ///< x[n-1]
    float  hpfPrevOut_   { 0.0f };     ///< y[n-1]

    // Median filter 5-taps (voiced values only — zeros never enter)
    std::array<float, 5> pitchHistory_ {};
    int    historyIndex_   { 0 };
    bool   historyPrimed_  { false };  ///< false until first valid pitch fills history

    // Pre-allocated working buffers (no allocation in process())
    std::vector<float> yinBuffer_;        ///< d'(tau) values [windowSize/2]
    std::vector<float> inputAccumulator_; ///< ring accumulator [windowSize]
    int  accWritePos_    { 0 };
    int  accFilled_      { 0 };           ///< samples written since last reset

    PitchResult lastResult_; ///< cached result until next analysis
};

} // namespace dsp
