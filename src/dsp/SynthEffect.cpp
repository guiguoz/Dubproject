#include "SynthEffect.h"
#include "DspCommon.h"

#include <algorithm>
#include <cmath>

namespace dsp
{

static constexpr float kPi    = 3.14159265358979f;
static constexpr float kTwoPi = 6.28318530717959f;

// Static voice spread (must match declaration)
constexpr float SynthEffect::kVoiceSpread[kNumVoices];

static constexpr ParamDescriptor kParams[8] = {
    {"waveform",  "Waveform",     0.0f,     1.0f,    0.0f},
    {"octave",    "Octave",      -2.0f,     2.0f,    0.0f},
    {"detune",    "Detune",       0.0f,   100.0f,   25.0f},
    {"cutoff",    "Cutoff",      20.0f, 16000.0f, 2000.0f},
    {"resonance", "Resonance",    0.0f,     1.0f,    0.3f},
    {"attack",    "Attack",       0.001f,   0.5f,  0.005f},
    {"release",   "Release",      0.01f,    2.0f,   0.15f},
    {"mix",       "Volume",          0.0f,     1.0f,    1.0f},
};

// ─────────────────────────────────────────────────────────────────────────────
// prepare / reset
// ─────────────────────────────────────────────────────────────────────────────

void SynthEffect::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_  = sampleRate;
    reset();
}

void SynthEffect::reset() noexcept
{
    for (auto& v : voices_) v.phase = 0.0;
    for (int i = 0; i < 4; ++i) { stage_[i] = 0.0f; delay_[i] = 0.0f; }
    envLevel_    = 0.0f;
    currentFreq_ = 0.0f;
    targetFreq_  = 0.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// PolyBLEP — band-limited polynomial residual
//
// Eliminates aliasing at waveform discontinuities.
// t  = normalised phase [0,1)
// dt = phase increment per sample (freq / sampleRate)
//
// Reference: Välimäki & Franck (2012), Tale @ KVR
// ─────────────────────────────────────────────────────────────────────────────
float SynthEffect::polyBlep(double t, double dt) noexcept
{
    if (t < dt)
    {
        const double n = t / dt;
        return static_cast<float>(n + n - n * n - 1.0);
    }
    if (t > 1.0 - dt)
    {
        const double n = (t - 1.0) / dt;
        return static_cast<float>(n * n + n + n + 1.0);
    }
    return 0.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Generate one PolyBLEP-antialiased sample from a single oscillator voice
// ─────────────────────────────────────────────────────────────────────────────
float SynthEffect::generateVoiceSample(OscVoice& v, float freq, float wf) noexcept
{
    if (freq <= 0.0f) return 0.0f;

    const double dt = static_cast<double>(freq) / sampleRate_;
    v.phase += dt;
    if (v.phase >= 1.0) v.phase -= 1.0;

    const double t = v.phase;
    float sample = 0.0f;

    if (wf < 0.25f)
    {
        // ── Saw (PolyBLEP) ──────────────────────────────────────────────
        sample = static_cast<float>(2.0 * t - 1.0);
        sample -= polyBlep(t, dt);
    }
    else if (wf < 0.5f)
    {
        // ── Square (PolyBLEP) ───────────────────────────────────────────
        sample = (t < 0.5) ? 1.0f : -1.0f;
        sample += polyBlep(t, dt);
        sample -= polyBlep(std::fmod(t + 0.5, 1.0), dt);
    }
    else if (wf < 0.75f)
    {
        // ── Sine (no anti-aliasing needed) ──────────────────────────────
        sample = std::sin(kTwoPi * static_cast<float>(t));
    }
    else
    {
        // ── Sub bass: sine one octave below ─────────────────────────────
        sample = std::sin(kPi * static_cast<float>(t));
    }

    return sample;
}

// ─────────────────────────────────────────────────────────────────────────────
// Moog Ladder Filter — 4-pole (24 dB/oct) with self-oscillation
//
// Based on Huovilainen's improved non-linear model.
// Thermal coefficient (tanh saturation) on each stage for analog warmth.
//
// Reference: Välimäki & Smith, "Discrete-Time Synthesis of the Moog VCF"
//            Huovilainen, "Non-Linear Digital Implementation of the Moog
//            Ladder Filter" (DAFx-04)
// ─────────────────────────────────────────────────────────────────────────────
float SynthEffect::moogFilter(float input, float cutoffHz, float reso) noexcept
{
    // Clamp cutoff to safe range
    const float nyq = static_cast<float>(sampleRate_) * 0.49f;
    const float fc  = std::min(std::max(cutoffHz, 20.0f), nyq);

    // Cutoff coefficient (attempt to match analog Moog response)
    const float f = 2.0f * fc / static_cast<float>(sampleRate_);
    const float g = f * 0.76f;                        // ~1.8 × Moog tuning

    // Feedback (k = 0..4; 4 = self-oscillation)
    const float k = 4.0f * reso;

    // Feedback signal from 4th stage (with 1-sample delay)
    const float fb = delay_[3];

    // Input with resonance feedback + saturation
    float x = input - k * fb;
    x = std::tanh(x);

    // 4 cascaded 1-pole filters with tanh non-linearity
    for (int i = 0; i < 4; ++i)
    {
        stage_[i] = stage_[i] + g * (x - stage_[i]);
        x = std::tanh(stage_[i]);
        delay_[i] = stage_[i];
    }

    return stage_[3];
}

// ─────────────────────────────────────────────────────────────────────────────
// process
// ─────────────────────────────────────────────────────────────────────────────
void SynthEffect::process(float* buf, int numSamples, float pitchHz) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    const int   safeN = std::min(numSamples, kMaxBlock);
    const float wf    = waveform_.load(std::memory_order_relaxed);
    const float oct   = octave_.load(std::memory_order_relaxed);
    const float det   = detune_.load(std::memory_order_relaxed);     // cents
    const float cut   = cutoff_.load(std::memory_order_relaxed);
    const float res   = resonance_.load(std::memory_order_relaxed);
    const float atk   = attack_.load(std::memory_order_relaxed);
    const float rel   = release_.load(std::memory_order_relaxed);
    const float mx    = mix_.load(std::memory_order_relaxed);

    // Envelope follower coefficients (1-pole)
    const float attackCoeff  = std::exp(-1.0f / (atk  * static_cast<float>(sampleRate_)));
    const float releaseCoeff = std::exp(-1.0f / (rel  * static_cast<float>(sampleRate_)));

    // Update target frequency from pitch tracker
    if (pitchHz > 20.0f && pitchHz < 10000.0f)
    {
        targetFreq_ = pitchHz * std::pow(2.0f, oct);
        // Snap currentFreq_ on first valid pitch to avoid glide from 0
        if (currentFreq_ < 20.0f)
            currentFreq_ = targetFreq_;
    }

    // Detune spread: convert cents to ratio  (e.g. 25 cents → ~1.0145)
    const float detuneRatio = std::pow(2.0f, det / 1200.0f);

    for (int i = 0; i < safeN; ++i)
    {
        // Smooth pitch glide (portamento) — faster snap for live playing
        constexpr float kGlide = 0.05f;
        currentFreq_ += kGlide * (targetFreq_ - currentFreq_);

        // ── Envelope follower ───────────────────────────────────────────
        const float inputAbs = std::abs(buf[i]);
        if (inputAbs > envLevel_)
            envLevel_ = attackCoeff  * envLevel_ + (1.0f - attackCoeff)  * inputAbs;
        else
            envLevel_ = releaseCoeff * envLevel_ + (1.0f - releaseCoeff) * inputAbs;

        // ── SuperSaw: sum 7 detuned voices ──────────────────────────────
        float osc = 0.0f;
        for (int v = 0; v < kNumVoices; ++v)
        {
            // Per-voice frequency with symmetric detune spread
            const float voiceRatio = std::pow(detuneRatio, kVoiceSpread[v]);
            const float voiceFreq = currentFreq_ * voiceRatio;
            osc += generateVoiceSample(voices_[v], voiceFreq, wf);
        }
        // Normalise: -1..1 range
        osc *= (1.0f / static_cast<float>(kNumVoices));

        // ── Moog ladder filter ──────────────────────────────────────────
        osc = moogFilter(osc, cut, res);

        // ── Apply envelope (synth "breathes" with the sax, or keyboard gate) ──
        osc *= std::max(envLevel_, keyGate_.load(std::memory_order_relaxed));

        // ── Wet only (dry sax signal discarded) ──────────────────────────
        synthBuf_[i] = clipSample(osc) * mx;
    }

    std::copy(synthBuf_.begin(), synthBuf_.begin() + safeN, buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameters
// ─────────────────────────────────────────────────────────────────────────────

ParamDescriptor SynthEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kParams[i] : ParamDescriptor{};
}

float SynthEffect::getParam(int i) const noexcept
{
    switch (i)
    {
    case kWaveform:  return waveform_.load(std::memory_order_relaxed);
    case kOctave:    return octave_.load(std::memory_order_relaxed);
    case kDetune:    return detune_.load(std::memory_order_relaxed);
    case kCutoff:    return cutoff_.load(std::memory_order_relaxed);
    case kResonance: return resonance_.load(std::memory_order_relaxed);
    case kAttack:    return attack_.load(std::memory_order_relaxed);
    case kRelease:   return release_.load(std::memory_order_relaxed);
    case kMix:       return mix_.load(std::memory_order_relaxed);
    default:         return 0.0f;
    }
}

void SynthEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
    case kWaveform:  waveform_.store(v, std::memory_order_relaxed);  break;
    case kOctave:    octave_.store(v, std::memory_order_relaxed);    break;
    case kDetune:    detune_.store(v, std::memory_order_relaxed);    break;
    case kCutoff:    cutoff_.store(v, std::memory_order_relaxed);    break;
    case kResonance: resonance_.store(v, std::memory_order_relaxed); break;
    case kAttack:    attack_.store(v, std::memory_order_relaxed);    break;
    case kRelease:   release_.store(v, std::memory_order_relaxed);   break;
    case kMix:       mix_.store(v, std::memory_order_relaxed);       break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Presets — 22 presets crafted for live saxophone
//
// Each preset is an array of 8 floats matching the parameter order:
//   waveform, octave, detune, cutoff, resonance, attack, release, mix
// ─────────────────────────────────────────────────────────────────────────────

struct SynthPreset
{
    const char* name;
    float params[8]; // wf, oct, det, cut, res, atk, rel, mix
};

static constexpr SynthPreset kPresets[] = {
    //                              wf     oct   det    cut     res    atk     rel    mix
    //
    // Waveform mapping:
    //   0.00 = Saw (PolyBLEP)     — bright, harmonics-rich
    //   0.33 = Square (PolyBLEP)  — hollow, woody
    //   0.66 = Sine               — pure, clean
    //   1.00 = Sub sine (-1 oct)  — deep, sub-bass

    // ═══════════════════════════════════════════════════════════════════════
    // MOOG BASSES — le sax devient un synthé basse analogique
    // ═══════════════════════════════════════════════════════════════════════
    { "Moog Taurus",           {  0.0f, -1.0f,   6.0f,  280.0f, 0.55f, 0.001f, 0.12f, 0.85f }},
    // Saw -1oct, filtre fermé + reso → gros Moog bass chaud
    { "Acid Squelch",          {  0.0f,  0.0f,   3.0f,  450.0f, 0.98f, 0.001f, 0.03f, 0.90f }},
    // Saw, reso quasi self-osc, cutoff bas → TB-303 qui crie
    { "Reese Wobble",          {  0.0f, -1.0f,  45.0f,  700.0f, 0.42f, 0.003f, 0.25f, 0.80f }},
    // Saw -1oct, gros detune → battement type DnB/dubstep reese
    { "Sub Cannon",            {  1.0f, -2.0f,   0.0f,  100.0f, 0.08f, 0.001f, 0.08f, 0.95f }},
    // Sub sine -2oct, cutoff 100Hz → basse viscérale sub-only
    { "Hoover",                {  0.0f, -1.0f, 100.0f, 2200.0f, 0.48f, 0.002f, 0.06f, 0.80f }},
    // Saw -1oct, detune max → son Hoover mentasm classic

    // ═══════════════════════════════════════════════════════════════════════
    // LEADS — le sax qui perce le mix
    // ═══════════════════════════════════════════════════════════════════════
    { "Trance Supersaw",       {  0.0f,  0.0f,  85.0f, 8000.0f, 0.18f, 0.001f, 0.05f, 0.75f }},
    // Saw, gros detune, filtre grand ouvert → mur de son JP-8000
    { "Screaming Lead",        {  0.0f,  1.0f,   8.0f, 1200.0f, 1.00f, 0.001f, 0.03f, 0.90f }},
    // Saw +1oct, reso max → filtre qui auto-oscille, lead perçant
    { "PWM Blade",             {  0.33f, 0.0f,  30.0f, 3500.0f, 0.60f, 0.001f, 0.04f, 0.80f }},
    // Square, detune modéré → son tranchant type Blade Runner
    { "Vintage Solo",          {  0.33f, 0.0f,   2.0f, 1800.0f, 0.45f, 0.003f, 0.08f, 0.70f }},
    // Square mono, peu de detune → Minimoog solo chaud
    { "Sync Rip",              {  0.0f,  1.0f,  65.0f, 6000.0f, 0.72f, 0.001f, 0.02f, 0.85f }},
    // Saw +1oct, reso forte + filtre haut → texture agressive

    // ═══════════════════════════════════════════════════════════════════════
    // PADS & ATMOSPHÈRES — transformer le sax en nappe
    // ═══════════════════════════════════════════════════════════════════════
    { "Blade Runner Pad",      {  0.0f,  0.0f,  55.0f, 1400.0f, 0.22f, 0.400f, 2.00f, 0.50f }},
    // Saw, lent attack/release → le sax respire dans la nappe
    { "Glass Choir",           {  0.66f, 1.0f,  40.0f, 9000.0f, 0.08f, 0.300f, 1.80f, 0.45f }},
    // Sine +1oct, filtre ouvert → voix cristallines éthérées
    { "Dark Drone",            {  0.0f, -1.0f,  20.0f,  400.0f, 0.30f, 0.500f, 2.00f, 0.60f }},
    // Saw -1oct, cutoff bas, long release → bourdon sombre
    { "Shimmer",               {  0.66f, 2.0f,  70.0f,14000.0f, 0.05f, 0.200f, 1.50f, 0.35f }},
    // Sine +2oct, detune large, filtre ouvert → scintillement
    { "Subaquatic",            {  0.66f,-1.0f,  30.0f,  600.0f, 0.35f, 0.350f, 2.00f, 0.55f }},
    // Sine -1oct, filtre fermé → son sous-marin, profond

    // ═══════════════════════════════════════════════════════════════════════
    // DUB TECHNO — textures deep pour le live
    // ═══════════════════════════════════════════════════════════════════════
    { "Deepchord Stab",        {  0.33f, 0.0f,  35.0f, 2800.0f, 0.28f, 0.001f, 0.06f, 0.65f }},
    // Square, cut moyen → stab court dub techno
    { "Basic Channel Wash",    {  0.0f,  0.0f,  50.0f, 1200.0f, 0.20f, 0.250f, 1.80f, 0.40f }},
    // Saw, slow envelope → fond harmonique qui évolue lentement
    { "Rhythm & Sound Sub",    {  1.0f, -1.0f,   5.0f,  180.0f, 0.12f, 0.010f, 0.40f, 0.75f }},
    // Sub -1oct, filtre très fermé → basse dub minimale

    // ═══════════════════════════════════════════════════════════════════════
    // FX / EXPÉRIMENTAL — le sax entre dans la matrice
    // ═══════════════════════════════════════════════════════════════════════
    { "Resonance Scream",      {  0.0f,  0.0f,  10.0f,  350.0f, 1.00f, 0.001f, 0.10f, 0.85f }},
    // Saw, reso max + cutoff bas → filtre qui hurle au pitch du sax
    { "Bit Crusher",           {  0.33f, 2.0f,  95.0f,  900.0f, 0.85f, 0.001f, 0.02f, 0.90f }},
    // Square +2oct, detune max, reso haute → chaos numérique
    { "Ghost Harmonics",       {  0.66f, 1.0f,   0.0f, 4000.0f, 0.65f, 0.050f, 0.80f, 0.30f }},
    // Sine +1oct mono, reso modérée → harmonique fantôme résonante
    { "Foghorn",               {  0.0f, -2.0f,  15.0f,  220.0f, 0.70f, 0.010f, 0.50f, 0.90f }},
    // Saw -2oct, filtre quasi fermé, reso → corne de brume massive
};

static constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

int SynthEffect::presetCount() const noexcept
{
    return kPresetCount;
}

const char* SynthEffect::presetName(int index) const noexcept
{
    if (index < 0 || index >= kPresetCount) return "";
    return kPresets[index].name;
}

void SynthEffect::applyPreset(int index) noexcept
{
    if (index < 0 || index >= kPresetCount) return;
    const auto& p = kPresets[index].params;
    for (int i = 0; i < kParamCount; ++i)
        setParam(i, p[i]);
}

} // namespace dsp
