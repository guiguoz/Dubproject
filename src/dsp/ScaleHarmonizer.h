#pragma once

#include "KeyResult.h"

#include <array>
#include <atomic>
#include <mutex>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// ScaleHarmonizer
//
// Analyses the last 20 detected pitches using Krumhansl-Schmuckler key
// finding, then exposes consonant harmonisation intervals for the current key.
//
// Thread model:
//   - pushNote()    : audio thread  (try_lock — never blocks)
//   - updateScale() : timer thread  (blocks; call every ~500 ms)
//   - getKey()      : any thread    (atomic reads)
// ─────────────────────────────────────────────────────────────────────────────
class ScaleHarmonizer
{
  public:
    /// Push a newly detected pitch.  Skips silently if confidence < 0.4 or
    /// if the timer thread currently holds the history lock.
    void pushNote(float pitchHz, float confidence) noexcept;

    /// Recompute key from current history.  Call from timer thread (~500 ms).
    KeyResult updateScale() noexcept;

    /// Last computed key (safe from any thread).
    KeyResult getKey() const noexcept;

    /// Consonant intervals for the current key: [third, fifth] in semitones.
    std::array<int, 2> getIntervals() const noexcept;

    void reset() noexcept;

    /// Public for testing and sharing with KeyDetector.
    static KeyResult detectFromChroma(const std::array<float, 12>& chroma) noexcept;

  private:
    static int pitchToClass(float pitchHz) noexcept;

    struct NoteEntry
    {
        int   pitchClass = 0;
        float confidence = 0.0f;
    };

    static constexpr int kHistSize = 20;
    std::array<NoteEntry, kHistSize> history_{};
    int histHead_  = 0;
    int histCount_ = 0;
    mutable std::mutex histMutex_;

    std::atomic<int> key_{ -1 };
    std::atomic<int> mode_{ 0 };
    std::atomic<int> intervalThird_{ 4 }; // default: major 3rd
    std::atomic<int> intervalFifth_{ 7 }; // perfect 5th
};

} // namespace dsp
