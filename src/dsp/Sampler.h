#pragma once

#include "BeatClock.h"
#include "DspCommon.h"
#include "SlotDynamics.h"
#include <array>
#include <atomic>
#include <vector>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// PcmView — vue zéro-copie sur les données PCM internes d'un slot.
// Valide uniquement pendant la durée d'un traitement offline (worker thread) ;
// le double-buffer activeDataIdx garantit que data[] n'est pas modifié pendant
// que le worker tourne.
// ─────────────────────────────────────────────────────────────────────────────
struct PcmView
{
    const float* data = nullptr;
    int          size = 0;
    bool         empty()              const noexcept { return size == 0 || data == nullptr; }
    const float* begin()              const noexcept { return data; }
    const float* end()                const noexcept { return data + size; }
    const float& operator[](int i)    const noexcept { return data[i]; }
};

// ─────────────────────────────────────────────────────────────────────────────
// SampleSlot
// One loaded sample. Data is pre-allocated on the GUI thread; the audio
// thread only reads it.
// ─────────────────────────────────────────────────────────────────────────────
struct SampleSlot
{
    std::vector<float> data[2];          // Double buffer for smooth sample changes
    std::atomic<int>   sampleCount[2] {};
    std::atomic<int>   activeDataIdx{ 0 };

    std::atomic<float> gain        { 1.0f };
    std::atomic<bool>  loopEnabled { false };
    std::atomic<bool>  oneShot     { true };
    std::atomic<bool>  loaded      { false }; // set after data is ready
    std::atomic<bool>  muted       { false }; // silenced but keeps playing
    std::atomic<float> delaySend   { 0.0f };  // 0=dry-only, 1=full send to delay bus

    // A2: BPM at which the active PCM buffer was stretched (0 = no info → rate=1.0).
    // stretchedBpm updated atomically at step 0 via pendingStretchedBpm.
    std::atomic<float> stretchedBpm        { 0.f };
    std::atomic<float> pendingStretchedBpm { 0.f };

    // A3: original PCM (pre-stretch, post-trim) — message thread only, never read by audio thread.
    std::vector<float> originalPcm {};
    double             originalSr  { 44100.0 };


    // C1: real-time pitch transpose — 2^(semitones/12). Set by GUI; read by audio.
    std::atomic<float> transposeRatio  { 1.0f };
    // B3: half/double-time factor {0.5, 1.0, 2.0} — quantized to step 0.
    std::atomic<float> halttimeFactor  { 1.0f };
    std::atomic<float> pendingHaltime  { 0.f };  // 0 = nothing pending

    // D: Performance FX commands (GUI→audio, consumed by audio thread each block).
    std::atomic<int>   perfCmd       { 0 };     // encodes PerfCmd; exchange(0) consumes it
    std::atomic<float> tapeDurMs     { 500.f }; // D1: stop/start ramp duration in ms
    std::atomic<float> glideStartSt  { 12.f };  // D2: glide start pitch offset in semitones
    std::atomic<float> glideTimeMs   { 800.f }; // D2: glide 99%-convergence time in ms
    std::atomic<bool>  pendingReverse{ false };  // D3: toggle reverse at next step 0
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
    void trigger(int slot, int offset = 0) noexcept;

    // StopMode controls the fade-out duration applied when stopping a slot.
    // Encoded as int so it fits in a single atomic<int> (stopPending).
    // 0 = no stop pending; 1..4 = mode values.
    enum class StopMode : int
    {
        Normal    = 1,  // 350 ms — extinction douce (pads, basses)
        SceneSwap = 2,  // 20 ms  — coupure nette avant crossfade de scene
        Retrigger = 3,  // 6 ms   — choke rapide (hihat, perc)
        Instant   = 4   // 0 ms   — coupure immediate
    };

    void stop(int slot, StopMode mode = StopMode::Normal) noexcept;
    void stopAllSlots(StopMode mode = StopMode::Normal) noexcept;

    // D: Performance FX — rate modulations applied in real-time by the audio thread.
    // Commands are consumed at the next audio block (not quantized, except toggleReverse).
    enum class PerfCmd : int {
        TapeStop  = 1,  // ramp rate 1→0 quadratically over tapeDurMs
        TapeStart = 2,  // ramp rate 0→1 quadratically over tapeDurMs
        Glide     = 3,  // pitch from +glideStartSt semitones, exp. convergence over glideTimeMs
        Cancel    = 4   // cancel active perf FX, restore ratio=1
    };
    void triggerTapeStop (int slot, float durationMs = 500.f) noexcept;
    void triggerTapeStart(int slot, float durationMs = 500.f) noexcept;
    void triggerGlide    (int slot, float startSemitones = 12.f, float timeMs = 800.f) noexcept;
    void cancelPerfFx    (int slot) noexcept;
    // D3: toggle reverse playback, quantized to next step 0.
    void toggleReverse(int slot) noexcept;
    bool isReversed   (int slot) const noexcept;

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
    void setSlotMuted(int slot, bool muted, bool quantized = false) noexcept;
    void onTrackStep0(int slot, int offsetInBlock = 0) noexcept;
    void setSlotDelaySend(int slot, float send) noexcept;
    float getSlotDelaySend(int slot) const noexcept;

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

    // Returns a zero-copy view of the slot's internal PCM buffer.
    // Safe to use from a worker thread during applyNeutronMix(): the double-buffer
    // (activeDataIdx) ensures the active buffer is not modified while the worker runs.
    // The returned pointer is valid only for the duration of the current worker pass.
    PcmView getSlotPcmView(int slot) const noexcept;

    // Returns the peak absolute amplitude of the slot's PCM data (0 if not loaded).
    // Safe to call from the GUI thread (data is not modified by the audio thread).
    float getSlotPeakLevel(int slot) const noexcept;

    SlotDynamics& getSlotDynamics(int slot) noexcept
        { return slotDynamics_[static_cast<std::size_t>(slot)]; }

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
    // sendBufL/R: optional pre-pan mono send bus (accumulated per slot delaySend).
    void processStereo(float* left, float* right, int numSamples,
                       float* sendBufL = nullptr, float* sendBufR = nullptr) noexcept;

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

    // Arm a phase-aligned restart at the next trackStep=0 for this slot.
    // Called from the GUI thread after reloadSlotData() (BPM-matched buffer ready).
    // onTrackStep0() consumes the flag and fires triggerPending → path C restarts
    // the voice from readPos=0 with the new buffer, aligned to the step grid.
    void schedulePhaseReset(int slot) noexcept;

    // A2: store the BPM the new buffer was stretched to; consumed at next step 0.
    void setSlotStretchedBpm(int slot, float bpm) noexcept;

    // C1: set real-time transpose (2^(st/12)), applied immediately to all active voices.
    void setSlotTransposeSemitones(int slot, float semitones) noexcept;
    // C1: read current transpose ratio (GUI thread).
    float getSlotTransposeRatio(int slot) const noexcept
    {
        if (slot < 0 || slot >= kMaxSlots) return 1.f;
        return slots_[static_cast<std::size_t>(slot)]
            .transposeRatio.load(std::memory_order_relaxed);
    }
    // B3: queue half/double-time factor change (applied at next step 0).
    void setPendingHaltime(int slot, float factor) noexcept;
    // B3: read current haltime factor.
    float getHalttimeFactor(int slot) const noexcept;

    // A3: save/retrieve the pre-stretch PCM for re-stretch (A2) and transpose (C1).
    // Call saveOriginalPcm from the message thread before launching the async stretch worker.
    void saveOriginalPcm(int slot, const float* data, int numSamples, double sr) noexcept;
    bool getOriginalPcm (int slot, std::vector<float>& out, double& outSr) const noexcept;

private:
    struct VoiceState
    {
        bool   playing{ false };
        int    dataIdx{ 0 };
        double readPos{ 0.0 };
        double rate   { 1.0 };  // tempo × transpose × perf ratio (futurs A2/C1/D)
        int    fadeIn{ 0 };
        int  fadeOut{ 0 };
        int  fadeOutTotal{ 256 };
        bool retriggering{ false };
        bool stopAfterFadeOut{ false };
        int  startOffset{ 0 };   // intra-block sample offset (P0.3)
    };

    struct PlayState
    {
        std::atomic<bool> triggerPending  { false };
        std::atomic<int>  stopPending     { 0 };  // 0=none, encodes StopMode
        std::atomic<bool> quantTrigPending{ false };
        std::atomic<int>  quantDiv        { static_cast<int>(GridDiv::Quarter) };
        std::atomic<bool> unmutePending     { false };
        std::atomic<bool> pendingPhaseReset { false };
        std::atomic<int>  triggerOffset     { 0 };   // intra-block offset for triggerPending (P0.3)

        // D: per-slot performance FX state (audio-thread-only, no atomics needed).
        struct PerfFxState {
            enum class Mode : uint8_t { None, TapeStop, TapeStart, Glide };
            Mode  mode      { Mode::None };
            float ratio     { 1.f };    // current rate multiplier (0=stopped, 1=normal)
            float elapsed   { 0.f };    // samples elapsed since FX start
            float total     { 1.f };    // total samples for tape ramp
            float glideFrom { 1.f };    // glide: starting ratio (2^(startSt/12))
            float glideK    { 0.f };    // glide: precomputed k = 4.6052 / totalSamp
            bool  reverse   { false };  // rate sign: true → rate × -1
        } perfFx;

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
    std::array<SlotDynamics, kMaxSlots> slotDynamics_ {};
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

    // Returns fade-out length in samples for a given StopMode.
    int stopFadeSamples(StopMode mode) const noexcept
    {
        switch (mode)
        {
            case StopMode::SceneSwap: return static_cast<int>(sampleRate_ * 0.020);
            case StopMode::Retrigger: return 256;
            case StopMode::Instant:   return 0;
            case StopMode::Normal:
            default:                  return static_cast<int>(sampleRate_ * 0.350);
        }
    }

    // Applies Haas delay ring buffer for one slot and returns delayed sample.
    inline float applyHaasDelay(int slot, float sample) noexcept;
};

} // namespace dsp