#pragma once

#include "KeyResult.h"

#include <array>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// KeyDetector
//
// Offline key analyser: accumulates a chromagram from audio frames using
// per-frequency Goertzel filters (no FFT library required), then applies
// Krumhansl-Schmuckler correlation to identify the musical key.
//
// Usage (offline, from a worker thread):
//   KeyDetector kd;
//   for (each block)  kd.process(block, n, sampleRate);
//   KeyResult r = kd.getResult();   // valid once enough frames processed
//
// All methods are single-threaded (call from one thread only).
// ─────────────────────────────────────────────────────────────────────────────
class KeyDetector
{
  public:
    void      reset() noexcept;
    void      process(const float* buf, int numSamples, double sampleRate) noexcept;
    KeyResult getResult() const noexcept;

    /// Exposed for reuse / unit testing.
    static KeyResult detectFromChroma(const std::array<float, 12>& chroma) noexcept;

    /// Goertzel power for a single frequency. Exposed for unit testing.
    static float goertzel(const float* buf, int n, double freq, double fs) noexcept;

    // Coverage: MIDI 48 (C3 ≈ 130 Hz) to MIDI 83 (B5 ≈ 988 Hz), 3 octaves
    static constexpr int kBaseNote   = 48;
    static constexpr int kNumOctaves = 3;
    static constexpr int kFrameSize  = 4096;

  private:
    static constexpr int kMinFrames  = 4; // minimum frames before reporting

    void processFrame() noexcept;

    std::array<float, kFrameSize> frame_{};
    int    frameFill_  = 0;
    int    frameCount_ = 0;
    double sampleRate_ = 44100.0;

    std::array<float, 12> chroma_{}; // accumulated pitch-class energy
    KeyResult             result_;   // updated after each complete frame
};

} // namespace dsp
