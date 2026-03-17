#pragma once

#include "DspCommon.h"
#include <array>
#include <atomic>
#include <vector>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// SampleSlot
// One loaded sample. Data is pre-allocated on the GUI thread; the audio
// thread only reads it.
// ─────────────────────────────────────────────────────────────────────────────
struct SampleSlot
{
    std::vector<float> data;             // mono PCM, filled by loadSample()
    int                sampleCount { 0 };
    std::atomic<float> gain        { 1.0f };
    std::atomic<bool>  loopEnabled { false };
    std::atomic<bool>  oneShot     { true };
    std::atomic<bool>  loaded      { false }; // set after data is ready
};

// ─────────────────────────────────────────────────────────────────────────────
// Sampler
//
// 8-slot sample player.  All parameter writes are atomic; playback state
// (readPos) is only touched by the audio thread.
// ─────────────────────────────────────────────────────────────────────────────
class Sampler
{
public:
    static constexpr int kMaxSlots = 8;

    void prepare(double sampleRate, int maxBlockSize) noexcept;

    // Load mono PCM into a slot.  Call from the GUI thread BEFORE the slot
    // is triggered.  fileSampleRate is used for basic rate detection
    // (mismatch emits a debug warning but still loads).
    void loadSample(int slot, const float* data, int numSamples,
                    double fileSampleRate) noexcept;

    void clearSlot(int slot) noexcept;

    // Trigger / stop — safe to call from any thread (audio or MIDI).
    void trigger(int slot) noexcept;
    void stop(int slot) noexcept;

    void setSlotGain(int slot, float gain) noexcept;
    void setSlotLoop(int slot, bool loop) noexcept;
    void setSlotOneShot(int slot, bool oneShot) noexcept;

    bool isLoaded(int slot) const noexcept;
    bool isPlaying(int slot) const noexcept;

    // Mixes sampler output INTO buffer (additive, realtime-safe).
    void process(float* buffer, int numSamples) noexcept;

    void reset() noexcept;

private:
    struct PlayState
    {
        std::atomic<bool> playing { false };
        std::atomic<bool> triggerPending { false };
        std::atomic<bool> stopPending    { false };
        int               readPos { 0 };  // only written by audio thread
    };

    std::array<SampleSlot, kMaxSlots> slots_;
    std::array<PlayState,  kMaxSlots> playStates_;
    double sampleRate_ { 44100.0 };
};

} // namespace dsp
