#pragma once

#include "IEffect.h"
#include "LockFreeQueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// EffectChain
//
// Manages a dynamic chain of IEffect objects with real-time-safe add/remove.
//
// Thread model
// ────────────
//   GUI thread  : addEffect(), removeEffect(), moveEffect(), collectGarbage()
//   Audio thread: prepare(), process(), reset()
//
// Double-buffer swap
// ──────────────────
//   Two ChainBuffer arrays hold raw IEffect* pointers.
//   activeIdx_ (atomic) tells the audio thread which buffer to read.
//
//   1. GUI builds desired chain in owned_[] (unique_ptr, GUI thread only).
//   2. GUI calls publishSnapshot() → pushes a ChainSnapshot into commandQueue_.
//   3. At the start of process(), the audio thread drains commandQueue_:
//       - copies snapshot into the inactive buffer
//       - stores 1-activeIdx_ into activeIdx_ (atomic release)
//       - increments swapGeneration_
//   4. GUI's collectGarbage() checks swapGeneration_ > lastRemoveGen_:
//       - if true → the audio thread has moved past the old chain → safe to delete
//
// Max chain length: kMaxEffects (8 by default).
// ─────────────────────────────────────────────────────────────────────────────
class EffectChain
{
public:
    static constexpr int kMaxEffects = 8;

    // ── Audio thread ─────────────────────────────────────────────────────────

    /// Prepare all currently-owned effects.  Call from prepareToPlay().
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    /// Drain pending chain updates then process one block.
    void process(float* buf, int numSamples, float pitchHz) noexcept;

    /// Drain pending chain updates then process one stereo block in-place.
    void processStereo(float* left, float* right, int numSamples, float pitchHz) noexcept;

    /// Reset all currently-owned effects.
    void reset() noexcept;

    // ── GUI thread ───────────────────────────────────────────────────────────

    /// Add an effect at the end of the chain.
    /// Returns false if the chain is already at kMaxEffects.
    bool addEffect(std::unique_ptr<IEffect> effect) noexcept;

    /// Remove the effect at index.  The effect is moved to the graveyard;
    /// call collectGarbage() from your GUI timer to actually delete it once
    /// the audio thread has confirmed it no longer uses it.
    /// No-op if index is out of range.
    void removeEffect(int index) noexcept;

    /// Swap the order of effects at positions from and to.
    /// No-op if either index is out of range.
    void moveEffect(int from, int to) noexcept;

    /// Number of live effects (GUI thread only).
    int effectCount() const noexcept { return ownedCount_; }

    /// Raw pointer to effect at index (GUI thread only).  Returns nullptr if OOB.
    IEffect* getEffect(int index) noexcept;

    /// Forward BPM to tempo-synced effects (e.g. Delay).
    void setBpm(float bpm) noexcept;

    /// Call from a GUI timer (~30 fps).
    /// Deletes effects removed since the last confirmed audio-thread swap.
    void collectGarbage() noexcept;

    /// For testing: drain commands without calling process.
    void drainCommands() noexcept;

    /// Audio thread only — raw pointer from the active buffer.
    /// Returns nullptr if index is out of range.
    IEffect* getActiveEffect(int index) noexcept;

private:
    // ── Inner types ──────────────────────────────────────────────────────────

    struct ChainBuffer
    {
        std::array<IEffect*, kMaxEffects> effects {};
        int count { 0 };
    };

    struct ChainSnapshot
    {
        std::array<IEffect*, kMaxEffects> effects {};
        int count { 0 };
    };

    // ── State ─────────────────────────────────────────────────────────────────

    // Double buffer (audio thread reads; GUI thread writes to the inactive one
    // indirectly through commandQueue_)
    ChainBuffer       buffers_[2];
    std::atomic<int>  activeIdx_ { 0 };

    // Command queue: GUI → audio thread
    LockFreeQueue<ChainSnapshot, 16> commandQueue_;

    // Generation counter: incremented by audio thread after each swap
    std::atomic<std::uint32_t> swapGeneration_ { 0 };

    // Owned effects (GUI thread only)
    std::array<std::unique_ptr<IEffect>, kMaxEffects> owned_;
    int ownedCount_ { 0 };

    // Graveyard: effects waiting for the audio thread to acknowledge the swap
    std::array<std::unique_ptr<IEffect>, kMaxEffects> graveyard_;
    int    graveyardCount_    { 0 };
    std::uint32_t lastRemoveGen_ { 0 };

    // Cached prepare params (so newly-added effects can be prepared)
    double preparedSampleRate_  { 0.0 };
    int    preparedBlockSize_   { 0 };

    // Publish current owned_ order to the command queue
    void publishSnapshot() noexcept;
};

} // namespace dsp
