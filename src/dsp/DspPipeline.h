#pragma once

#include "YinPitchTracker.h"
#include "Harmonizer.h"
#include "Flanger.h"

#include <atomic>
#include <vector>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// DspPipeline
//
// Orchestrates: YIN pitch track → harmonizer → flanger → clip.
//
// Thread safety:
//   - enable/disable flags: atomic<bool> with acquire/release
//   - float parameters:     forwarded directly to sub-modules (atomic<float>)
//   - process() is called from the audio thread only
//   - set*() methods are called from the GUI thread only
// ─────────────────────────────────────────────────────────────────────────────
class DspPipeline
{
public:
    /// Call from prepareToPlay(). Pre-allocates all internal memory.
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    /// Process a mono block in-place. Realtime-safe.
    void process(float* buffer, int numSamples) noexcept;

    void reset() noexcept;

    // ── Enable / disable (GUI thread) ───────────────────────────────────────
    void setHarmonizerEnabled(bool enabled) noexcept;
    void setFlangerEnabled(bool enabled) noexcept;

    bool isHarmonizerEnabled() const noexcept;
    bool isFlangerEnabled()    const noexcept;

    // ── Parameter access (delegates to sub-modules) ─────────────────────────
    Harmonizer&      getHarmonizer() noexcept { return harmonizer_; }
    Flanger&         getFlanger()    noexcept { return flanger_;    }
    YinPitchTracker& getPitchTracker() noexcept { return pitchTracker_; }

    /// Latest pitch result (safe to read from GUI thread — atomic copy)
    PitchResult getLastPitch() const noexcept;

private:
    YinPitchTracker pitchTracker_;
    Harmonizer      harmonizer_;
    Flanger         flanger_;

    std::atomic<bool> harmonizerEnabled_ { false };
    std::atomic<bool> flangerEnabled_    { false };

    // Latest pitch (written by audio thread, read by GUI thread)
    std::atomic<float> lastPitchHz_    { 0.0f };
    std::atomic<float> lastConfidence_ { 0.0f };

    // Scratch buffer for harmonizer output (pre-allocated in prepare)
    std::vector<float> scratchBuffer_;
};

} // namespace dsp
