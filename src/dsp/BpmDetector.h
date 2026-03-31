#pragma once

#include <array>
#include <atomic>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// BpmDetector
//
// Real-time BPM estimator based on RMS onset detection.
//
// Algorithm:
//   1. Compute RMS of each audio block.
//   2. Onset = positive RMS derivative > threshold.
//   3. Accumulate last 16 inter-onset intervals (IOI).
//   4. BPM = 60 / mean(IOI).
//   5. Median filter (5 values) for stability.
//
// Thread safety:
//   - process() : audio thread only
//   - getBpm()  : any thread (atomic read)
//   - reset()   : call before streaming starts, not during
// ─────────────────────────────────────────────────────────────────────────────
class BpmDetector
{
  public:
    static constexpr float kDefaultBpm = 120.0f;
    static constexpr float kMinBpm     = 40.0f;
    static constexpr float kMaxBpm     = 240.0f;

    void  prepare(double sampleRate) noexcept;
    void  process(const float* buf, int numSamples) noexcept;
    float getBpm() const noexcept;
    void  reset() noexcept;

    /// Onset sensitivity (default 0.015). Lower = more sensitive.
    void setThreshold(float t) noexcept { threshold_ = t; }

    // ── Offline batch analysis ────────────────────────────────────────────────
    /// Autocorrelation-based BPM detection on a complete PCM buffer.
    /// More accurate than the streaming onset detector for pre-recorded files.
    /// Returns kDefaultBpm if detection fails.
    static float detectOffline(const float* data, int numSamples,
                                double sampleRate) noexcept;

    /// BPM detection result with confidence score (0.0–1.0).
    /// confidence ≥ 0.7 → reliable; 0.5–0.7 → plausible; < 0.5 → uncertain.
    struct BpmDetectionResult
    {
        float bpm;        ///< estimated BPM (kDefaultBpm if detection failed)
        float confidence; ///< 0.0 = no signal, 1.0 = very confident
    };

    /// Multi-method BPM detection with confidence score.
    /// Tries 3 methods in order (RMS autocorrelation, onset-strength autocorrelation,
    /// comb-filter energy); returns the first result whose confidence exceeds a
    /// threshold, otherwise returns the best result found.
    static BpmDetectionResult detectOfflineRobust(const float* data, int numSamples,
                                                  double sampleRate) noexcept;

  private:
    void  onOnset() noexcept;
    float estimateFromIoi() noexcept;
    float medianFilter(float newBpm) noexcept;

    double sampleRate_      = 44100.0;
    double currentSample_   = 0.0;
    double lastOnsetSample_ = -1.0;
    float  prevRms_         = 0.0f;
    float  threshold_       = 0.015f;

    // Last 16 inter-onset intervals (seconds)
    static constexpr int kIoiSize = 16;
    std::array<float, kIoiSize> ioi_{};
    int ioiHead_  = 0;
    int ioiCount_ = 0;

    // Median filter history (5 BPM estimates)
    static constexpr int kHistSize = 5;
    std::array<float, kHistSize> hist_{};
    int histHead_  = 0;
    int histCount_ = 0;

    std::atomic<float> bpm_{ kDefaultBpm };
};

} // namespace dsp
