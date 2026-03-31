#pragma once

#include "DspCommon.h"
#include <array>
#include <cassert>
#include <vector>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// WsolaShifter
//
// WSOLA (Waveform Similarity Overlap-Add) pitch shifter.
//
// Algorithm: for each synthesis hop (kHopS samples), a grain of kWindowSize
// samples is extracted from the analysis ring buffer at a position advanced
// by hopA = kHopS * shiftRatio per hop. Before extraction, a cross-correlation
// search over ±kDelta samples finds the best offset minimising phase
// discontinuity with the previous output grain.  The grain is Hann-windowed
// and overlap-added into an accumulation buffer from which output is read.
//
// Real-time safe: all buffers pre-allocated in prepare(), zero heap in
// process().  Safe for ±24 semitones on monophonic saxophone (baritone or
// tenor).
// ─────────────────────────────────────────────────────────────────────────────
class WsolaShifter
{
public:
    // ── Algorithm constants ─────────────────────────────────────────────────
    static constexpr int kWindowSize = 512;   ///< grain / OLA window (samples)
    static constexpr int kHopS       = 256;   ///< synthesis hop (50% overlap)
    static constexpr int kDelta      = 128;   ///< cross-corr search range ±
    static constexpr int kTailSize   = 256;   ///< tail for cross-corr (= kHopS)
    static constexpr int kRingSize   = 8192;  ///< analysis ring (power-of-2)
    static constexpr int kAccumSize  = 8192;  ///< output accumulation (power-of-2)

    // ── Public API ───────────────────────────────────────────────────────────

    /// Call once from prepareToPlay(). Pre-allocates state and computes the
    /// Hann window. May be called again to change sampleRate.
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    /// Set pitch shift in semitones (range −24..+24 recommended).
    void setShiftSemitones(float semitones) noexcept;

    /// Process one block.  input and output may be the same pointer.
    /// inputPitchHz is accepted for API compatibility but not used by WSOLA.
    void process(const float* input, float* output,
                 int numSamples, float inputPitchHz) noexcept;

    void reset() noexcept;

    // ── Offline utility ──────────────────────────────────────────────────────

    /// Linear-interpolation time-scale change for offline (batch) processing.
    /// ratio = sourceBpm / targetBpm  (or inputSampleRate / outputSampleRate).
    ///   ratio < 1 → output longer  (time-stretch, e.g. 110/130 ≈ 0.846)
    ///   ratio > 1 → output shorter (time-compress)
    /// Pitch is NOT preserved — apply pitchShift separately after this call to
    /// compensate with semitones = -12 * log2(ratio).
    static std::vector<float> resampleLinear(const std::vector<float>& input,
                                             float ratio) noexcept;

    /// Hermite 4-point interpolation time-scale change (offline, batch).
    /// Same semantics as resampleLinear but THD+N ≈ 0.01% vs 0.5% for linear.
    /// Preferred for tempo-correction paths; falls back to linear near edges.
    static std::vector<float> resampleHermite(const std::vector<float>& input,
                                              float ratio) noexcept;

private:
    static constexpr int kRingMask  = kRingSize  - 1;
    static constexpr int kAccumMask = kAccumSize - 1;

    static_assert((kRingSize  & kRingMask)  == 0, "kRingSize must be power-of-2");
    static_assert((kAccumSize & kAccumMask) == 0, "kAccumSize must be power-of-2");
    static_assert(kWindowSize + kDelta <= kRingSize / 2, "ring buffer too small");

    double sampleRate_ { 44100.0 };
    float  shiftRatio_ { 1.0f };

    std::array<float, kRingSize>   ring_  {};  ///< analysis ring buffer
    std::array<float, kAccumSize>  accum_ {};  ///< OLA output accumulation
    std::array<float, kTailSize>   tail_  {};  ///< last output grain tail
    std::array<float, kWindowSize> hann_  {};  ///< pre-computed Hann window

    /// Absolute sample counters (int overflows after ~12.4 h @ 48 kHz — OK)
    int writeAbs_    { 0 };  ///< next input write position
    int analysisAbs_ { 0 };  ///< next grain analysis position
    int outputAbs_   { 0 };  ///< next grain synthesis position in accum
    int readAbs_     { 0 };  ///< next output read position

    void computeHann() noexcept;

    /// Returns best offset d in [−kDelta, +kDelta] maximising cross-corr
    /// between tail_ and the analysis ring around analysisAbs_.
    int findBestOffset() const noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// Backward-compatible alias for code written against the previous OlaShifter.
// ─────────────────────────────────────────────────────────────────────────────
using OlaShifter = WsolaShifter;

} // namespace dsp
