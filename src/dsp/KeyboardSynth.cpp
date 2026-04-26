#include "KeyboardSynth.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
static constexpr float kPi    = 3.14159265358979323846f;
static constexpr float kTwoPi = 6.28318530717958647692f;
static constexpr float kLn2   = 0.693147180559945309f;
}

namespace dsp {

namespace {

inline float paramToAttackSec  (float p) noexcept { return 0.001f + p * 1.999f; }
inline float paramToDecaySec   (float p) noexcept { return 0.005f + p * 1.995f; }
inline float paramToReleaseSec (float p) noexcept { return 0.01f  + p * 3.99f;  }
inline float paramToOctave     (float p) noexcept { return (p - 0.5f) * 4.f; }
inline float paramToDetuneCents(float p) noexcept { return (p - 0.5f) * 100.f; }

inline float paramToCutoffHz(float p, double sr) noexcept
{
    const float minHz = 80.f;
    const float maxHz = static_cast<float>(sr * 0.49);
    return minHz * std::pow(maxHz / minHz, p);
}

inline float paramToResonance(float p) noexcept { return 1.0f - p * 0.95f; }

inline float svfProcess(float in, float& lp, float& bp, float f, float r) noexcept
{
    const float hp = in - r * bp - lp;
    bp += f * hp;
    lp += f * bp;
    return lp;
}

inline float calcHz(float baseHz, float octOffset, float detuneCents) noexcept
{
    return baseHz * std::pow(2.f, octOffset + detuneCents / 1200.f);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Preset table
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct Preset {
    const char* name;
    // params[0..12]: Wave Oct Detune Cutoff Res Atk Rel Mix Sus Dec Glide VelAmt FiltEnvAmt
    float params[KeyboardSynth::kParamCount];
    bool  mono;
};

// Oct: 0.5=0, 0.25=-1oct, 0.75=+1oct
// Glide: param × 0.5s → e.g. 0.08 → 40ms
static constexpr Preset kPresets[] = {
    {
        "Classic Mono Lead",
        //  Wave   Oct  Det   Cut   Res   Atk   Rel   Mix   Sus   Dec   Glide VelAmt FEnv
        { 0.35f, 0.5f, 0.5f, 0.70f, 0.25f, 0.02f, 0.25f, 1.f, 0.80f, 0.10f, 0.08f, 0.60f, 0.15f },
        true
    },
    {
        "Acid Dub",
        { 0.60f, 0.5f, 0.5f, 0.45f, 0.82f, 0.01f, 0.15f, 1.f, 0.35f, 0.04f, 0.00f, 0.80f, 0.65f },
        true
    },
    {
        "Sub Bass",
        { 0.12f, 0.25f, 0.5f, 0.55f, 0.10f, 0.05f, 0.40f, 1.f, 0.90f, 0.20f, 0.10f, 0.40f, 0.00f },
        true
    },
    {
        "Warm Chord",
        { 0.78f, 0.5f, 0.65f, 0.65f, 0.15f, 0.06f, 0.35f, 1.f, 0.70f, 0.20f, 0.00f, 0.50f, 0.00f },
        false
    },
    {
        "Reggae Stab",
        { 0.60f, 0.5f, 0.60f, 0.60f, 0.40f, 0.01f, 0.10f, 1.f, 0.30f, 0.03f, 0.00f, 0.70f, 0.30f },
        false
    },
    {
        "Gritty Lead",
        { 0.40f, 0.5f, 0.5f, 0.50f, 0.60f, 0.01f, 0.30f, 1.f, 0.75f, 0.12f, 0.14f, 0.65f, 0.40f },
        true
    },
};

static constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

int KeyboardSynth::presetCount() noexcept { return kPresetCount; }

const char* KeyboardSynth::presetName(int i) noexcept
{
    if (i < 0 || i >= kPresetCount) return "";
    return kPresets[i].name;
}

void KeyboardSynth::applyPreset(int index) noexcept
{
    if (index < 0 || index >= kPresetCount) return;
    const auto& p = kPresets[index];
    for (int i = 0; i < kParamCount; ++i)
        params_[static_cast<std::size_t>(i)].store(p.params[i], std::memory_order_relaxed);
    monoMode_.store(p.mono, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
void KeyboardSynth::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;

    for (int i = 0; i < kVoices; ++i)
    {
        voices_[i] = Voice{};
        updateVoicePan(i);
    }
    heldCount_ = 0;

    params_[0].store(0.25f, std::memory_order_relaxed); // Wave = saw
    params_[1].store(0.5f,  std::memory_order_relaxed); // Oct  = 0
    params_[2].store(0.5f,  std::memory_order_relaxed); // Detune = 0
    params_[3].store(0.75f, std::memory_order_relaxed); // Cutoff = open
    params_[4].store(0.2f,  std::memory_order_relaxed); // Res = light
    params_[5].store(0.02f, std::memory_order_relaxed); // Atk = ~40ms
    params_[6].store(0.25f, std::memory_order_relaxed); // Rel = ~1s
    params_[7].store(1.f,   std::memory_order_relaxed); // Mix = unused
    params_[8].store(0.7f,  std::memory_order_relaxed); // Sustain = 70%
    params_[9].store(0.15f, std::memory_order_relaxed); // Decay = ~300ms
    params_[10].store(0.f,  std::memory_order_relaxed); // Glide = off
    params_[11].store(0.5f, std::memory_order_relaxed); // VelAmt = 50%
    params_[12].store(0.f,  std::memory_order_relaxed); // FiltEnvAmt = off
}

// ─────────────────────────────────────────────────────────────────────────────
void KeyboardSynth::setParam(int idx, float value) noexcept
{
    if (idx >= 0 && idx < kParamCount)
        params_[static_cast<std::size_t>(idx)].store(value, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
void KeyboardSynth::updateVoicePan(int i) noexcept
{
    const float t     = (kVoices > 1) ? static_cast<float>(i) / static_cast<float>(kVoices - 1)
                                      : 0.5f;
    const float angle = (t - 0.5f) * 2.f * kWidth * (kPi / 2.f);
    voices_[i].panL = std::cos(angle + kPi / 4.f);
    voices_[i].panR = std::sin(angle + kPi / 4.f);
}

// ─────────────────────────────────────────────────────────────────────────────
int KeyboardSynth::stealVoice() noexcept
{
    for (int i = 0; i < kVoices; ++i)
        if (!voices_[i].active) return i;

    int   bestIdx = 0;
    float bestEnv = std::numeric_limits<float>::max();
    for (int i = 0; i < kVoices; ++i)
    {
        if (!voices_[i].gateOn && voices_[i].envAmp < bestEnv)
        {
            bestEnv = voices_[i].envAmp;
            bestIdx = i;
        }
    }
    if (bestEnv < std::numeric_limits<float>::max()) return bestIdx;

    bestEnv = std::numeric_limits<float>::max();
    for (int i = 0; i < kVoices; ++i)
    {
        if (voices_[i].envAmp < bestEnv)
        {
            bestEnv = voices_[i].envAmp;
            bestIdx = i;
        }
    }
    return bestIdx;
}

// ─────────────────────────────────────────────────────────────────────────────
void KeyboardSynth::noteOn(int midiNote, float vel) noexcept
{
    const float baseHz    = 440.f * std::pow(2.f, (midiNote - 69) / 12.f);
    const float octOff    = paramToOctave(params_[1].load(std::memory_order_relaxed));
    const float detCents  = paramToDetuneCents(params_[2].load(std::memory_order_relaxed));
    const float scaledVel = std::clamp(vel, 0.f, 1.f);

    if (monoMode_.load(std::memory_order_relaxed))
    {
        if (heldCount_ < kMaxHeld)
            heldNotes_[static_cast<std::size_t>(heldCount_++)] = { midiNote, scaledVel };

        Voice& v = voices_[0];
        const bool legato = v.active && v.gateOn;

        v.note     = midiNote;
        v.targetHz = calcHz(baseHz, octOff, 0.f);
        v.gateOn   = true;
        v.velocity = scaledVel;

        if (!legato)
        {
            v.active    = true;
            v.envAmp    = 0.f;
            v.stage     = Voice::Attack;
            v.phase     = 0.f;
            v.currentHz = v.targetHz;
            v.svfLp     = 0.f;
            v.svfBp     = 0.f;
        }
        return;
    }

    const int  vi   = stealVoice();
    Voice&     v    = voices_[vi];
    const float sign = (vi % 2 == 0) ? 1.f : -1.f;

    v.note      = midiNote;
    v.targetHz  = calcHz(baseHz, octOff, sign * detCents);
    v.currentHz = v.targetHz;
    v.gateOn    = true;
    v.active    = true;
    v.envAmp    = 0.f;
    v.stage     = Voice::Attack;
    v.phase     = 0.f;
    v.velocity  = scaledVel;
    v.svfLp     = 0.f;
    v.svfBp     = 0.f;
    updateVoicePan(vi);
}

// ─────────────────────────────────────────────────────────────────────────────
void KeyboardSynth::noteOff(int midiNote) noexcept
{
    if (monoMode_.load(std::memory_order_relaxed))
    {
        for (int i = 0; i < heldCount_; ++i)
        {
            if (heldNotes_[static_cast<std::size_t>(i)].note == midiNote)
            {
                for (int j = i; j < heldCount_ - 1; ++j)
                    heldNotes_[static_cast<std::size_t>(j)] = heldNotes_[static_cast<std::size_t>(j + 1)];
                --heldCount_;
                break;
            }
        }

        Voice& v = voices_[0];
        if (v.note == midiNote && v.gateOn)
        {
            if (heldCount_ > 0)
            {
                const auto& prev   = heldNotes_[static_cast<std::size_t>(heldCount_ - 1)];
                const float bHz    = 440.f * std::pow(2.f, (prev.note - 69) / 12.f);
                const float octOff = paramToOctave(params_[1].load(std::memory_order_relaxed));
                v.note     = prev.note;
                v.targetHz = calcHz(bHz, octOff, 0.f);
                v.velocity = prev.vel;
            }
            else
            {
                v.gateOn = false;
                v.stage  = Voice::Release;
            }
        }
        return;
    }

    for (auto& v : voices_)
        if (v.note == midiNote && v.gateOn)
        {
            v.gateOn = false;
            v.stage  = Voice::Release;
        }
}

// ─────────────────────────────────────────────────────────────────────────────
// PolyBLEP residual — bandlimited correction near phase discontinuities.
// Applied within a 2-sample window around each discontinuity.
// Ref: Valimaki & Huovilainen (2007), "Antialiasing Oscillators in Subtractive Synthesis"
float KeyboardSynth::polyBlep(float t, float dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.f;
    }
    if (t > 1.f - dt)
    {
        t = (t - 1.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Anti-aliased oscillator using PolyBLEP corrections on Saw and Square.
// Triangle and Sine have no discontinuities — no correction needed.
float KeyboardSynth::oscillator(float phase, float dt, float waveSel) noexcept
{
    if (waveSel < 0.25f)
    {
        // Sine — no aliasing
        return std::sin(phase * kTwoPi);
    }
    if (waveSel < 0.5f)
    {
        // PolyBLEP Saw — discontinuity at phase wrap (t=1→0)
        float saw = 2.f * phase - 1.f;
        saw -= polyBlep(phase, dt);
        return saw;
    }
    if (waveSel < 0.75f)
    {
        // PolyBLEP Square — discontinuities at t=0 (rising) and t=0.5 (falling)
        float sq = (phase < 0.5f) ? 1.f : -1.f;
        sq += polyBlep(phase, dt);
        sq -= polyBlep(phase < 0.5f ? phase + 0.5f : phase - 0.5f, dt);
        return sq;
    }
    // Triangle — integrate square; no extra correction needed
    return (phase < 0.5f) ? (4.f * phase - 1.f) : (3.f - 4.f * phase);
}

// ─────────────────────────────────────────────────────────────────────────────
float KeyboardSynth::tickVoice(Voice& v, float glideCoeff,
                               float atkInc, float decInc, float relInc, float susLevel,
                               float cutOffHz, float svfR, float filterEnvAmt,
                               float waveP, float velAmtP) noexcept
{
    // Exponential glide (coeff=0 → instant snap)
    v.currentHz = glideCoeff * v.currentHz + (1.f - glideCoeff) * v.targetHz;

    // ADSR
    switch (v.stage)
    {
        case Voice::Attack:
            v.envAmp += atkInc;
            if (v.envAmp >= 1.f) { v.envAmp = 1.f; v.stage = Voice::Decay; }
            break;
        case Voice::Decay:
            v.envAmp -= decInc;
            if (v.envAmp <= susLevel) { v.envAmp = susLevel; v.stage = Voice::Sustain; }
            break;
        case Voice::Sustain:
            v.envAmp = susLevel;
            break;
        case Voice::Release:
            v.envAmp -= relInc;
            if (v.envAmp <= 0.f) { v.envAmp = 0.f; v.active = false; v.stage = Voice::Off; }
            break;
        case Voice::Off:
            break;
    }

    // Filter cutoff modulated by envelope (0..+3 octaves, exp2 via fast exp)
    const float sr    = static_cast<float>(sampleRate_);
    const float modHz = cutOffHz * std::exp(filterEnvAmt * v.envAmp * 3.f * kLn2);
    const float effHz = std::clamp(modHz, 20.f, sr * 0.49f);
    const float svfF  = 2.f * std::sin(kPi * effHz / sr);

    // PolyBLEP oscillator
    const float dt = v.currentHz / sr;
    float s = oscillator(v.phase, dt, waveP);

    // SVF filter
    s = svfProcess(s, v.svfLp, v.svfBp, svfF, svfR);

    // VCA
    s *= v.envAmp * (1.f - velAmtP + velAmtP * v.velocity);

    // Advance phase
    v.phase += dt;
    if (v.phase >= 1.f) v.phase -= 1.f;

    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
void KeyboardSynth::processStereoAdd(float* left, float* right, int numSamples) noexcept
{
    const float waveP      = params_[0].load(std::memory_order_relaxed);
    const float cutP       = params_[3].load(std::memory_order_relaxed);
    const float resP       = params_[4].load(std::memory_order_relaxed);
    const float atkP       = params_[5].load(std::memory_order_relaxed);
    const float relP       = params_[6].load(std::memory_order_relaxed);
    const float susP       = params_[8].load(std::memory_order_relaxed);
    const float decP       = params_[9].load(std::memory_order_relaxed);
    const float glideP     = params_[10].load(std::memory_order_relaxed);
    const float velAmtP    = params_[11].load(std::memory_order_relaxed);
    const float filtEnvAmt = params_[12].load(std::memory_order_relaxed);
    const float gain       = gain_.load(std::memory_order_relaxed);
    const bool  mono       = monoMode_.load(std::memory_order_relaxed);

    const float sr       = static_cast<float>(sampleRate_);
    const float cutOffHz = paramToCutoffHz(cutP, sampleRate_);
    const float atkInc   = 1.f / (paramToAttackSec (atkP) * sr);
    const float decInc   = 1.f / (paramToDecaySec  (decP) * sr);
    const float relInc   = 1.f / (paramToReleaseSec(relP) * sr);
    const float svfR     = paramToResonance(resP);

    const float glideTimeSec = glideP * 0.5f;
    const float glideCoeff   = (mono && glideTimeSec > 0.001f)
        ? std::exp(-4.6051702f / (glideTimeSec * sr))
        : 0.f;

    for (auto& v : voices_)
    {
        if (!v.active) continue;
        for (int i = 0; i < numSamples; ++i)
        {
            const float s = tickVoice(v, glideCoeff, atkInc, decInc, relInc, susP,
                                      cutOffHz, svfR, filtEnvAmt,
                                      waveP, velAmtP) * gain;
            left [i] += s * v.panL;
            right[i] += s * v.panR;
            if (!v.active) break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void KeyboardSynth::processMonoAdd(float* buf, int numSamples) noexcept
{
    const float waveP      = params_[0].load(std::memory_order_relaxed);
    const float cutP       = params_[3].load(std::memory_order_relaxed);
    const float resP       = params_[4].load(std::memory_order_relaxed);
    const float atkP       = params_[5].load(std::memory_order_relaxed);
    const float relP       = params_[6].load(std::memory_order_relaxed);
    const float susP       = params_[8].load(std::memory_order_relaxed);
    const float decP       = params_[9].load(std::memory_order_relaxed);
    const float glideP     = params_[10].load(std::memory_order_relaxed);
    const float velAmtP    = params_[11].load(std::memory_order_relaxed);
    const float filtEnvAmt = params_[12].load(std::memory_order_relaxed);
    const float gain       = gain_.load(std::memory_order_relaxed);
    const bool  mono       = monoMode_.load(std::memory_order_relaxed);

    const float sr       = static_cast<float>(sampleRate_);
    const float cutOffHz = paramToCutoffHz(cutP, sampleRate_);
    const float atkInc   = 1.f / (paramToAttackSec (atkP) * sr);
    const float decInc   = 1.f / (paramToDecaySec  (decP) * sr);
    const float relInc   = 1.f / (paramToReleaseSec(relP) * sr);
    const float svfR     = paramToResonance(resP);

    const float glideTimeSec = glideP * 0.5f;
    const float glideCoeff   = (mono && glideTimeSec > 0.001f)
        ? std::exp(-4.6051702f / (glideTimeSec * sr))
        : 0.f;

    for (auto& v : voices_)
    {
        if (!v.active) continue;
        for (int i = 0; i < numSamples; ++i)
        {
            buf[i] += tickVoice(v, glideCoeff, atkInc, decInc, relInc, susP,
                                cutOffHz, svfR, filtEnvAmt,
                                waveP, velAmtP) * gain;
            if (!v.active) break;
        }
    }
}

} // namespace dsp
