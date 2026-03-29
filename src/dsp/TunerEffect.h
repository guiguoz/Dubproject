#pragma once

#include "IEffect.h"

#include <atomic>
#include <cmath>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// TunerEffect
//
// Chromatic tuner that reads pitchHz from the YIN tracker and computes
// the nearest note, octave, and cents deviation.
//
// Parameters:
//   0  mute      Mute the audio output while tuning  [0..1]  default 1
//   1  ref_freq  Reference A4 frequency             [415..465] default 442
//
// Readable state (atomic, GUI thread safe):
//   getNoteName(), getCentsDeviation(), getDetectedHz()
// ─────────────────────────────────────────────────────────────────────────────
class TunerEffect : public IEffect
{
  public:
    EffectType type() const noexcept override { return EffectType::Tuner; }

    void prepare(double sampleRate, int maxBlockSize) noexcept override;
    void process(float* buf, int numSamples, float pitchHz) noexcept override;
    void reset() noexcept override;

    int             paramCount()           const noexcept override { return kParamCount; }
    ParamDescriptor paramDescriptor(int i) const noexcept override;
    float           getParam(int i)        const noexcept override;
    void            setParam(int i, float v) noexcept override;

    // ── Tuner readback (GUI thread) ─────────────────────────────────────────
    /// Nearest MIDI note number (0–127). -1 = no signal.
    int   getNoteNumber()      const noexcept { return noteNumber_.load(std::memory_order_relaxed); }
    /// Deviation in cents from the nearest note. Range: -50..+50.
    float getCentsDeviation()  const noexcept { return centsDeviation_.load(std::memory_order_relaxed); }
    /// Last stable detected frequency.
    float getDetectedHz()      const noexcept { return detectedHz_.load(std::memory_order_relaxed); }
    /// True if a valid pitch is currently detected.
    bool  hasSignal()          const noexcept { return noteNumber_.load(std::memory_order_relaxed) >= 0; }

    /// Static helper: MIDI note number → note name string ("C4", "F#5", …)
    static const char* midiNoteToName(int midiNote) noexcept;

  private:
    enum : int
    {
        kMute = 0,
        kRefFreq,
        kParamCount
    };

    std::atomic<float> mute_    { 1.0f };    // mute by default (tuner mode)
    std::atomic<float> refFreq_ { 442.0f };  // A4 reference

    // Tuner state — written by audio thread, read by GUI
    std::atomic<int>   noteNumber_     { -1 };
    std::atomic<float> centsDeviation_ { 0.0f };
    std::atomic<float> detectedHz_     { 0.0f };

    // Smoothing for stability
    float smoothedPitch_ { 0.0f };
    double sampleRate_   { 44100.0 };
};

} // namespace dsp
