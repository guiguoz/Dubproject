#pragma once

#include "BeatClock.h"
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
    std::vector<float> data[2];          // Double buffer for smooth sample changes
    int                sampleCount[2] { 0, 0 };
    std::atomic<int>   activeDataIdx{ 0 };

    std::atomic<float> gain        { 1.0f };
    std::atomic<bool>  loopEnabled { false };
    std::atomic<bool>  oneShot     { true };
    std::atomic<bool>  loaded      { false }; // set after data is ready
    std::atomic<bool>  muted       { false }; // silenced but keeps playing
};

// ─────────────────────────────────────────────────────────────────────────────
// Sampler
//
// 9-slot sample player (slots 0–7 = S1–S8, slot 8 = MASTER).
// All parameter writes are atomic; playback state
// (readPos) is only touched by the audio thread.
// ─────────────────────────────────────────────────────────────────────────────
class Sampler
{
public:
    static constexpr int kMaxSlots   = 9;
    static constexpr int kMasterSlot = 8;

    void prepare(double sampleRate, int maxBlockSize) noexcept;
    double getSampleRate() const noexcept { return sampleRate_; }

    // Load mono PCM into a slot.  Call from the GUI thread BEFORE the slot
    // is triggered.  fileSampleRate is used for basic rate detection
    // (mismatch emits a debug warning but still loads).
    void loadSample(int slot, const float* data, int numSamples,
                    double fileSampleRate) noexcept;

    void clearSlot(int slot) noexcept;

    // Trigger / stop — safe to call from any thread (audio or MIDI).
    void trigger(int slot) noexcept;
    void stop(int slot) noexcept;
    void stopAllSlots() noexcept;  // immediately stops all playing slots

    // Sidechain: sourceSlot (e.g. KICK) ducks targetSlot (e.g. BASS) in real-time.
    // Call from GUI thread after magic mix. Max kMaxSidechainPairs pairs.
    void setSidechainPair(int sourceSlot, int targetSlot) noexcept;
    void clearSidechain() noexcept;

    // Quantized trigger: starts at the next GridDiv boundary of the BeatClock.
    // Falls back to immediate trigger if no BPM is set.
    void triggerQuantized(int slot, GridDiv div) noexcept;

    // Per-slot quantization preset (persisted between triggers).
    void setSlotGrid(int slot, GridDiv div) noexcept;
    GridDiv getSlotGrid(int slot) const noexcept;

    // True while a quantized trigger is waiting for its beat boundary.
    bool isPendingTrigger(int slot) const noexcept;

    // Feed master tempo from MasterSampleSelector / MusicContext.
    void setBpm(float bpm) noexcept { beatClock_.setBpm(bpm); }

    void setSlotGain(int slot, float gain) noexcept;
    void setSlotLoop(int slot, bool loop) noexcept;
    void setSlotOneShot(int slot, bool oneShot) noexcept;
    void setSlotMuted(int slot, bool muted) noexcept;

    // Solo: only the solo slot produces audio; -1 = no solo.
    // Safe to call from any thread (atomic).
    void setSoloSlot(int slot) noexcept;
    void clearSolo() noexcept;
    int  getSoloSlot() const noexcept;

    bool isLoaded(int slot) const noexcept;
    bool isPlaying(int slot) const noexcept;
    bool isSlotMuted(int slot) const noexcept;

    // Returns current runtime gain multiplier for a slot (GUI thread safe).
    float getSlotGain(int slot) const noexcept;

    // Returns a copy of the slot's PCM data for display / offline editing (GUI thread only).
    std::vector<float> getSlotPcmSnapshot(int slot) const noexcept;

    // Returns the peak absolute amplitude of the slot's PCM data (0 if not loaded).
    // Safe to call from the GUI thread (data is not modified by the audio thread).
    float getSlotPeakLevel(int slot) const noexcept;

    // Returns the number of PCM samples in a slot (0 if not loaded).
    int getSlotSampleCount(int slot) const noexcept;

    // Returns current playhead position as a 0..1 ratio (GUI thread, approximate).
    // Value is 0 when not playing or not loaded.
    float getSlotPlayheadRatio(int slot) const noexcept;

    // Returns the peak output level for slot in the last audio block (0.0–1.0+).
    // Written by the audio thread each block; safe to read from the GUI thread.
    float getSlotOutputPeak(int slot) const noexcept;


    // Atomically swap new PCM data into a slot (same guarantees as loadSample).
    // Stops playback first; called from GUI thread after offline processing.
    void reloadSlotData(int slot, std::vector<float> newData) noexcept;

    // Mixes sampler output INTO buffer (additive, realtime-safe).
    void process(float* buffer, int numSamples) noexcept;

    // Mixes sampler output into STEREO L/R buffers (additive, realtime-safe).
    // Pan law: equal-power (-3dB centre).
    // Width: Haas effect delay on the weaker channel (set via setSlotHaasDelay).
    void processStereo(float* left, float* right, int numSamples) noexcept;

    // Set panoramic position for a slot.  pan in [-1.0, +1.0].
    // Converts to equal-power L/R gains stored as atomics.
    // Call from GUI thread (after magic mix).
    void setSlotPan(int slot, float pan) noexcept;

    // Set Haas delay for a slot (0 = disabled, max kHaasDelayMax-1 samples).
    // Written by GUI thread, read by audio thread (audio-thread-only write pos is safe).
    void setSlotHaasDelay(int slot, int samples) noexcept;

    // Reset pan to centre and disable Haas for all slots.
    void resetSpatial() noexcept;

    void reset() noexcept;

private:
    struct VoiceState
    {
        bool playing{ false };
        int  dataIdx{ 0 };
        int  readPos{ 0 };
        int  fadeIn{ 0 };
        int  fadeOut{ 0 };
        int  fadeOutTotal{ 256 };
        bool retriggering{ false };
        bool stopAfterFadeOut{ false };
    };

    struct PlayState
    {
        std::atomic<bool> triggerPending  { false };
        std::atomic<bool> stopPending     { false };
        std::atomic<bool> quantTrigPending{ false };
        std::atomic<int>  quantDiv        { static_cast<int>(GridDiv::Quarter) };
        
        VoiceState voices[2];
        int currentVoice{ 0 };
    };

    std::array<SampleSlot, kMaxSlots> slots_;
    std::array<PlayState,  kMaxSlots> playStates_;
    BeatClock beatClock_;
    double sampleRate_ { 44100.0 };

    // ── Sidechain ─────────────────────────────────────────────────────────────
    static constexpr int kMaxSidechainPairs = 4;
    struct SidechainPair { int source{-1}; int target{-1}; float envelope{0.f}; };
    std::array<SidechainPair, kMaxSidechainPairs> sidechains_ {};
    int   numSidechains_                { 0 };
    float sidechainGains_[kMaxSlots]   { 1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f };
    float slotPeaks_     [kMaxSlots]   {};

    // Gain lissé per-sample (audio thread only) + coeff précalculé dans prepare().
    float gainSmoothed_  [kMaxSlots]   {};
    float gainRampCoeff_               { 0.998f };
    std::atomic<int>   soloSlot_       { -1 };  // -1 = no solo

    // Per-slot output peak — written by audio thread, read by GUI (VU meter).
    std::atomic<float> outputPeaks_[kMaxSlots] {};

    // ── Spatial (pan + Haas width) ─────────────────────────────────────────────
    // Equal-power pan gains — written by GUI thread, read by audio thread.
    std::array<std::atomic<float>, kMaxSlots> panL_ {};  // initialised in reset()
    std::array<std::atomic<float>, kMaxSlots> panR_ {};

    // Haas effect ring buffers — fixed size, never reallocated on audio thread.
    // 2048 samples > 42 ms @ 48 kHz — covers the full width=1 (25 ms) with margin.
    static constexpr int kHaasDelayMax = 2048;
    std::array<std::array<float, kHaasDelayMax>, kMaxSlots> haasDelay_ {};
    std::array<int, kMaxSlots> haasWritePos_    {};  // audio thread only
    std::array<int, kMaxSlots> haasDelaySamples_{};  // 0 = off; written by GUI thread

    // Applies Haas delay ring buffer for one slot and returns delayed sample.
    inline float applyHaasDelay(int slot, float sample) noexcept;
};

} // namespace dsp