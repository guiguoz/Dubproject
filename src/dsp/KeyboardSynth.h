#pragma once

#include <array>
#include <atomic>
#include <cmath>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// KeyboardSynth  —  Mono/Poly synthesizer for keyboard solos
//
// Params (0-12) written by GUI thread (atomic), read by audio thread:
//   0  Wave        0-1  → sine(0-0.25) saw(0.25-0.5) square(0.5-0.75) tri(0.75-1)
//   1  Oct         0-1  → -2..+2 octaves
//   2  Detune      0-1  → ±50 cents (poly only — even voices +, odd -)
//   3  Cutoff      0-1  → filter cutoff 80 Hz … sr/2
//   4  Res         0-1  → SVF resonance
//   5  Atk         0-1  → attack 0.001..2 s
//   6  Rel         0-1  → release 0.01..4 s
//   7  Mix         0-1  → reserved
//   8  Sustain     0-1  → ADSR sustain level (default 0.7)
//   9  Decay       0-1  → decay 0.005..2 s (default ~300ms)
//  10  Glide       0-1  → portamento 0..500ms (mono only, default 0)
//  11  VelAmt      0-1  → velocity→amplitude scaling (default 0.5)
//  12  FiltEnvAmt  0-1  → filter envelope amount 0..+3 octaves (default 0)
// ─────────────────────────────────────────────────────────────────────────────

class KeyboardSynth
{
public:
    static constexpr int kParamCount = 13;

    void prepare(double sampleRate, int maxBlockSize) noexcept;

    // Called from the audio thread (drained from SPSC queue in DspPipeline)
    void noteOn (int midiNote, float vel) noexcept;
    void noteOff(int midiNote)            noexcept;

    // Written by GUI thread
    void setParam   (int idx, float value) noexcept;
    void setGain    (float g)              noexcept { gain_.store(g, std::memory_order_relaxed); }
    void setMonoMode(bool mono)            noexcept { monoMode_.store(mono, std::memory_order_relaxed); }
    bool getMonoMode() const               noexcept { return monoMode_.load(std::memory_order_relaxed); }

    // Preset bank (6 dub presets)
    static int         presetCount()         noexcept;
    static const char* presetName(int index) noexcept;
    void               applyPreset(int index) noexcept;

    // Additive mix into existing buffers (do not zero them)
    void processStereoAdd(float* left, float* right, int numSamples) noexcept;
    void processMonoAdd  (float* buf,                int numSamples) noexcept;

private:
    static constexpr int   kVoices  = 8;
    static constexpr float kWidth   = 0.35f;
    static constexpr int   kMaxHeld = 16;

    struct Voice {
        int   note      { -1 };
        float targetHz  { 0.f };
        float currentHz { 0.f };
        float phase     { 0.f };
        float envAmp    { 0.f };
        float panL      { 0.707f };
        float panR      { 0.707f };
        bool  active    { false };
        bool  gateOn    { false };
        float velocity  { 1.f };
        enum Stage : uint8_t { Off, Attack, Decay, Sustain, Release } stage { Off };
        float svfLp     { 0.f };
        float svfBp     { 0.f };
    };

    struct HeldNote { int note; float vel; };

    std::array<Voice, kVoices>                   voices_    {};
    std::array<HeldNote, kMaxHeld>               heldNotes_ {};
    int                                          heldCount_ { 0 };
    std::array<std::atomic<float>, kParamCount>  params_    {};
    std::atomic<float>                           gain_      { 0.5f };
    std::atomic<bool>                            monoMode_  { false };
    double                                       sampleRate_{ 44100.0 };

    void  updateVoicePan(int voiceIdx) noexcept;
    int   stealVoice    () noexcept;

    // PolyBLEP residual — correct aliasing near phase discontinuities (2-sample window)
    static float polyBlep(float t, float dt) noexcept;

    // Anti-aliased oscillator: dt = phase increment (currentHz / sampleRate)
    float oscillator(float phase, float dt, float waveSel) noexcept;

    // Tick one sample: glide + ADSR + PolyBLEP OSC + filter (with env mod) + VCA
    float tickVoice(Voice& v, float glideCoeff,
                    float atkInc, float decInc, float relInc, float susLevel,
                    float cutOffHz, float svfR, float filterEnvAmt,
                    float waveP, float velAmtP) noexcept;
};

} // namespace dsp
