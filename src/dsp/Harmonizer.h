#pragma once

#include "WsolaShifter.h"  // defines WsolaShifter + using OlaShifter = WsolaShifter
#include <array>
#include <atomic>

namespace dsp {

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

    // Read-back (GUI thread only)
    float getVoiceInterval(int voiceIndex) const noexcept;
    float getMix() const noexcept { return mix_.load(std::memory_order_relaxed); }

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
