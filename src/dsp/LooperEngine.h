#pragma once

#include <JuceHeader.h>
#include <atomic>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// LooperEngine — EWI/Serum looper, bar-quantized, tape-style overdub
//
// Threading:
//   process() → audio thread ONLY
//   pressButton() / clear() / setOverdubMode() / setBpm() → GUI thread (atomic)
//   getState() / getLoopBars() / getOverdubMode() → any thread (atomic)
//
// State machine:
//   IDLE → pressButton() → ARMED → (next downbeat) → RECORDING
//   RECORDING → pressButton() → (arm stop at bar boundary) → still RECORDING
//                             → (recPos >= loopLenSamples) → PLAYING
//   PLAYING → pressButton() → OVERDUBBING → pressButton() → PLAYING
//   Any state → clear() → IDLE
// ─────────────────────────────────────────────────────────────────────────────
class LooperEngine
{
public:
    enum class State      { Idle, Armed, Recording, Playing, Overdubbing };
    enum class OverdubMode { Tape, Replace };

    void prepare(double sampleRate, int maxBars = 8) noexcept;
    void setBpm(float bpm) noexcept;

    // Audio thread — in-place safe (inL == outL allowed)
    // beatPhase: step sequencer phase in beats at block start (before seq advances)
    void process(const float* inL, const float* inR,
                 float* outL,       float* outR,
                 int numSamples, double beatPhase) noexcept;

    // GUI thread — thread-safe via atomics
    void pressButton()  noexcept { pendingPresses_.fetch_add(1, std::memory_order_relaxed); }
    void clear()        noexcept { pendingClear_.store(true, std::memory_order_relaxed); }
    void setOverdubMode(OverdubMode m) noexcept { overdubMode_.store(m, std::memory_order_relaxed); }

    // Any thread (atomic reads)
    State       getState()       const noexcept { return state_.load(std::memory_order_relaxed); }
    OverdubMode getOverdubMode() const noexcept { return overdubMode_.load(std::memory_order_relaxed); }

    // Loop length in bars — fractional during RECORDING, integer in PLAYING/OVERDUBBING.
    // Updated each audio block; safe to read from GUI (written with relaxed ordering).
    float getLoopBars() const noexcept { return displayBars_.load(std::memory_order_relaxed); }

private:
    static constexpr float kFeedback    = 0.98f;
    static constexpr float kReplaceFade = 0.70f;

    juce::AudioBuffer<float> buf_;           // pre-allocated, audio thread only
    int    loopLenSamples_ { 0 };            // 0 = no length yet (arm-stop not pressed)
    int    recPos_         { 0 };            // audio thread only
    int    playPos_        { 0 };            // audio thread only
    int    barSizeSamples_ { 0 };            // samples per bar at current BPM
    double sampleRate_     { 48000.0 };

    std::atomic<float>       bpm_           { 120.f };
    std::atomic<State>       state_         { State::Idle };
    std::atomic<OverdubMode> overdubMode_   { OverdubMode::Tape };
    std::atomic<int>         pendingPresses_{ 0 };
    std::atomic<bool>        pendingClear_  { false };
    std::atomic<float>       displayBars_   { 0.f };

    void updateBarSize() noexcept;
};

} // namespace dsp
