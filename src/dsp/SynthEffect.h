#pragma once

#include "IEffect.h"

#include <array>
#include <atomic>
#include <cmath>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// SynthEffect  —  Pitch-tracking monophonic synthesizer (v2)
//
// Based on industry-standard techniques:
//   • PolyBLEP anti-aliased oscillators (saw / square / triangle)
//   • SuperSaw unison (7 detuned voices, Roland JP-8000 style)
//   • Moog ladder filter (4-pole / 24 dB/oct with self-oscillation)
//   • Envelope follower with configurable attack/release
//
// Follows the detected input pitch (pitchHz from YIN) and generates a
// synthesized oscillator tone mixed with the dry sax signal.
//
// Parameters:
//   0  waveform   0=SuperSaw, 0.33=Square, 0.66=Sine, 1=SubBass
//   1  octave     Octave offset [-2 .. +2]  default 0
//   2  detune     Unison detune spread in cents [0 .. 100]  default 25
//   3  cutoff     Moog ladder cutoff [20 .. 16000 Hz]  default 2000
//   4  resonance  Moog ladder resonance [0 .. 1.0]  default 0.3
//   5  attack     Envelope attack  [0.001 .. 0.5 s]  default 0.005
//   6  release    Envelope release [0.01  .. 2.0 s]  default 0.15
//   7  mix        Dry/wet mix [0 .. 1]  default 0.5
// ─────────────────────────────────────────────────────────────────────────────
class SynthEffect : public IEffect
{
  public:
    EffectType type() const noexcept override { return EffectType::Synth; }

    void prepare(double sampleRate, int maxBlockSize) noexcept override;
    void process(float* buf, int numSamples, float pitchHz) noexcept override;
    void reset() noexcept override;

    int            paramCount() const noexcept override { return kParamCount; }
    ParamDescriptor paramDescriptor(int i) const noexcept override;
    float          getParam(int i) const noexcept override;
    void           setParam(int i, float v) noexcept override;

    // ── Presets ─────────────────────────────────────────────────────────────
    int         presetCount()             const noexcept override;
    const char* presetName(int index)     const noexcept override;
    void        applyPreset(int index)          noexcept override;

  private:
    // Parameter indices
    enum : int
    {
        kWaveform = 0,
        kOctave,
        kDetune,
        kCutoff,
        kResonance,
        kAttack,
        kRelease,
        kMix,
        kParamCount
    };

    // Atomic params (GUI ↔ audio)
    std::atomic<float> waveform_  { 0.0f };
    std::atomic<float> octave_    { 0.0f };
    std::atomic<float> detune_    { 25.0f };  // cents
    std::atomic<float> cutoff_    { 2000.0f };
    std::atomic<float> resonance_ { 0.3f };
    std::atomic<float> attack_    { 0.005f }; // seconds
    std::atomic<float> release_   { 0.15f };  // seconds
    std::atomic<float> mix_       { 0.5f };

    // ── SuperSaw unison (7 voices) ──────────────────────────────────────────
    static constexpr int kNumVoices = 7;
    // Per-voice detune ratios (symmetric around centre voice)
    static constexpr float kVoiceSpread[kNumVoices] = {
        -1.0f, -0.666f, -0.333f, 0.0f, 0.333f, 0.666f, 1.0f
    };

    struct OscVoice
    {
        double phase { 0.0 };
    };
    std::array<OscVoice, kNumVoices> voices_ {};

    // ── Moog Ladder Filter (4-pole) ─────────────────────────────────────────
    float stage_[4] {};          // 4 cascaded 1-pole stages
    float delay_[4] {};          // unit delay for each stage

    // ── Envelope follower ───────────────────────────────────────────────────
    float envLevel_ { 0.0f };

    // ── Keyboard gate (overrides envelope when > 0) ──────────────────────────
    std::atomic<float> keyGate_ { 0.f };

  public:
    // Set a gate level [0..1] to drive the synth from keyboard without mic input.
    // 0 = envelope-follower mode (default), >0 = held at that level.
    void setKeyGate(float gate) noexcept { keyGate_.store(gate, std::memory_order_relaxed); }

  private:

    // ── Pitch tracking ──────────────────────────────────────────────────────
    double sampleRate_ { 44100.0 };
    float  targetFreq_ { 0.0f };
    float  currentFreq_{ 0.0f };

    // Internal buffer
    static constexpr int kMaxBlock = 8192;
    std::array<float, kMaxBlock> synthBuf_ {};

    // ── DSP helpers ─────────────────────────────────────────────────────────
    // PolyBLEP residual (applied near discontinuities)
    static float polyBlep(double t, double dt) noexcept;

    // Generate one PolyBLEP sample for a single voice
    float generateVoiceSample(OscVoice& v, float freq, float wf) noexcept;

    // Moog ladder: process one sample
    float moogFilter(float input, float cutoffHz, float reso) noexcept;
};

} // namespace dsp
