#include "TunerEffect.h"

#include <algorithm>
#include <cmath>

namespace dsp
{

static constexpr ParamDescriptor kParams[2] = {
    {"mute",     "Mute",     0.0f, 1.0f,   1.0f},
    {"ref_freq", "A4 (Hz)", 415.0f, 465.0f, 442.0f},
};

// ─────────────────────────────────────────────────────────────────────────────

void TunerEffect::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
}

void TunerEffect::reset() noexcept
{
    smoothedPitch_ = 0.0f;
    noteNumber_.store(-1, std::memory_order_relaxed);
    centsDeviation_.store(0.0f, std::memory_order_relaxed);
    detectedHz_.store(0.0f, std::memory_order_relaxed);
}

void TunerEffect::process(float* buf, int numSamples, float pitchHz) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    const float refA4 = refFreq_.load(std::memory_order_relaxed);

    // ── Pitch analysis ──────────────────────────────────────────────────────
    if (pitchHz > 20.0f && pitchHz < 10000.0f)
    {
        // Smooth the pitch to avoid jitter (one-pole, ~50ms time constant)
        constexpr float kSmooth = 0.92f;
        if (smoothedPitch_ < 10.0f)
            smoothedPitch_ = pitchHz;  // init on first valid reading
        else
            smoothedPitch_ = kSmooth * smoothedPitch_ + (1.0f - kSmooth) * pitchHz;

        // Compute MIDI note number from frequency
        // MIDI note = 69 + 12 * log2(freq / A4)
        const float midiFloat = 69.0f + 12.0f * std::log2(smoothedPitch_ / refA4);
        const int   midiNote  = static_cast<int>(std::round(midiFloat));

        if (midiNote >= 0 && midiNote <= 127)
        {
            // Cents deviation = (midiFloat - round(midiFloat)) * 100
            const float cents = (midiFloat - static_cast<float>(midiNote)) * 100.0f;

            noteNumber_.store(midiNote, std::memory_order_relaxed);
            centsDeviation_.store(cents, std::memory_order_relaxed);
            detectedHz_.store(smoothedPitch_, std::memory_order_relaxed);
        }
    }
    else
    {
        // Signal lost — decay smoothed pitch toward zero
        smoothedPitch_ *= 0.95f;
        if (smoothedPitch_ < 10.0f)
        {
            noteNumber_.store(-1, std::memory_order_relaxed);
            centsDeviation_.store(0.0f, std::memory_order_relaxed);
            detectedHz_.store(0.0f, std::memory_order_relaxed);
        }
    }

    // ── Mute output (so you can tune silently) ──────────────────────────────
    if (mute_.load(std::memory_order_relaxed) > 0.5f)
        std::fill(buf, buf + numSamples, 0.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameters
// ─────────────────────────────────────────────────────────────────────────────

ParamDescriptor TunerEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kParams[i] : ParamDescriptor{};
}

float TunerEffect::getParam(int i) const noexcept
{
    switch (i)
    {
    case kMute:    return mute_.load(std::memory_order_relaxed);
    case kRefFreq: return refFreq_.load(std::memory_order_relaxed);
    default:       return 0.0f;
    }
}

void TunerEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
    case kMute:    mute_.store(v, std::memory_order_relaxed);    break;
    case kRefFreq: refFreq_.store(v, std::memory_order_relaxed); break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// midiNoteToName — returns "C4", "F#5", etc.
// ─────────────────────────────────────────────────────────────────────────────
const char* TunerEffect::midiNoteToName(int midiNote) noexcept
{
    if (midiNote < 0 || midiNote > 127) return "--";

    // Pre-computed table: 128 note names
    // Note names repeat every 12 semitones, octave = note/12 - 1
    static const char* const kNames[128] = {
        "C-1","C#-1","D-1","D#-1","E-1","F-1","F#-1","G-1","G#-1","A-1","A#-1","B-1",
        "C0","C#0","D0","D#0","E0","F0","F#0","G0","G#0","A0","A#0","B0",
        "C1","C#1","D1","D#1","E1","F1","F#1","G1","G#1","A1","A#1","B1",
        "C2","C#2","D2","D#2","E2","F2","F#2","G2","G#2","A2","A#2","B2",
        "C3","C#3","D3","D#3","E3","F3","F#3","G3","G#3","A3","A#3","B3",
        "C4","C#4","D4","D#4","E4","F4","F#4","G4","G#4","A4","A#4","B4",
        "C5","C#5","D5","D#5","E5","F5","F#5","G5","G#5","A5","A#5","B5",
        "C6","C#6","D6","D#6","E6","F6","F#6","G6","G#6","A6","A#6","B6",
        "C7","C#7","D7","D#7","E7","F7","F#7","G7","G#7","A7","A#7","B7",
        "C8","C#8","D8","D#8","E8","F8","F#8","G8","G#8","A8","A#8","B8",
        "C9","C#9","D9","D#9","E9","F9","F#9","G9",
    };
    return kNames[midiNote];
}

} // namespace dsp
