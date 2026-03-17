#pragma once

#include "DspCommon.h"
#include <array>
#include <atomic>
#include <vector>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// OlaShifter
//
// Single-voice pitch shifter using OLA (Overlap-Add) with linear resampling.
// Quality is sufficient for ±7 semitones on monophonic saxophone.
// ─────────────────────────────────────────────────────────────────────────────
class OlaShifter
{
public:
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    void setShiftSemitones(float semitones) noexcept;

    /// Process one block: read from `input`, write shifted audio to `output`.
    /// `inputPitchHz` guides the window size (period-synchronous OLA).
    void process(const float* input, float* output,
                 int numSamples, float inputPitchHz) noexcept;

    void reset() noexcept;

private:
    double sampleRate_    { 44100.0 };
    float  shiftSemitones_{ 0.0f };
    float  shiftRatio_    { 1.0f };

    // Analysis ring buffer (max 2 periods of kMinFrequencyHz)
    static constexpr int kAnalysisBufSize = 4096;
    std::array<float, kAnalysisBufSize> analysisBuffer_{};
    int  analysisWritePos_  { 0 };

    // Fractional read position into the analysis buffer
    float readPos_          { 0.0f };

    // Overlap-Add state
    static constexpr int kMaxOverlap = 512;
    std::array<float, kMaxOverlap> overlapBuffer_{};
    std::array<float, kMaxOverlap> hannWindow_{};
    int  overlapLength_     { 256 };
    int  overlapPos_        { 0 };

    void buildHannWindow(int length) noexcept;
    int  computeWindowLength(float pitchHz) const noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// Harmonizer
//
// 2-voice harmonizer: blends shifted copies of the input with the dry signal.
// ─────────────────────────────────────────────────────────────────────────────
class Harmonizer
{
public:
    static constexpr int kNumVoices = 2;

    Harmonizer() noexcept;

    void prepare(double sampleRate, int maxBlockSize) noexcept;

    /// Set interval in semitones for voice 0 (+3) and voice 1 (-5).
    void setVoiceInterval(int voiceIndex, float semitones) noexcept;

    /// Enable/disable individual voices.
    void setVoiceEnabled(int voiceIndex, bool enabled) noexcept;

    /// Dry/wet mix [0.0 = dry, 1.0 = wet only]. Default: 0.5.
    void setMix(float mix) noexcept;

    /// Process block: reads input, writes blended output to output buffer.
    void process(const float* input, float* output,
                 int numSamples, float inputPitchHz) noexcept;

    void reset() noexcept;

private:
    std::array<OlaShifter, kNumVoices> voices_;
    std::array<std::atomic<bool>,  kNumVoices> voiceEnabled_;
    std::array<float, kNumVoices>  voiceIntervals_ { 3.0f, -5.0f };
    std::atomic<float>             mix_            { 0.5f };

    // Scratch buffer — static size avoids heap allocation in the audio thread.
    static constexpr int kMaxBlockSize = 8192;
    std::array<float, kMaxBlockSize> voiceBuffer_{};
};

} // namespace dsp
