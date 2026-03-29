#include "EnvelopeFilterEffect.h"
#include "DspCommon.h"

#include <cmath>

namespace dsp {

static constexpr ParamDescriptor kEnvParams[5] = {
    { "sensitivity", "Sensitivity",  0.1f,  10.0f,   1.0f },
    { "attack_ms",   "Attack (ms)",  1.0f,  200.0f,  5.0f },
    { "release_ms",  "Release (ms)", 10.0f, 2000.0f, 200.0f },
    { "resonance",   "Resonance",    0.5f,  10.0f,   1.0f },
    { "mix",         "Mix",          0.0f,  1.0f,    0.7f },
};

static float timeToCoeff(float timeMs, double sampleRate) noexcept
{
    if (timeMs <= 0.0f) return 0.0f;
    // One-pole coefficient: 1 - exp(-1 / (t_s * sr))
    const float tSamples = timeMs * 0.001f * static_cast<float>(sampleRate);
    return 1.0f - std::exp(-1.0f / tSamples);
}

void EnvelopeFilterEffect::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    updateCoefficients();
    reset();
}

void EnvelopeFilterEffect::reset() noexcept
{
    envLevel_ = 0.0f;
    svfIc1_   = 0.0f;
    svfIc2_   = 0.0f;
}

void EnvelopeFilterEffect::updateCoefficients() noexcept
{
    attackCoeff_  = timeToCoeff(attackMs_.load(std::memory_order_relaxed),  sampleRate_);
    releaseCoeff_ = timeToCoeff(releaseMs_.load(std::memory_order_relaxed), sampleRate_);
}

void EnvelopeFilterEffect::process(float* buf, int numSamples, float /*pitchHz*/) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    // Minimum cutoff: ~200 Hz, maximum: ~8 kHz (mapped linearly by envelope)
    constexpr float kMinCutoffHz = 200.0f;
    constexpr float kMaxCutoffHz = 8000.0f;
    const float     kPi          = 3.14159265358979f;
    const float     srF          = static_cast<float>(sampleRate_);
    const float     sens         = sensitivity_.load(std::memory_order_relaxed);
    const float     res          = resonance_.load(std::memory_order_relaxed);
    const float     mx           = mix_.load(std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i)
    {
        const float drySample = buf[i];
        const float absIn     = drySample < 0.0f ? -drySample : drySample;

        // Envelope follower (one-pole with separate attack/release)
        const float diff = absIn * sens - envLevel_;
        if (diff > 0.0f)
            envLevel_ += attackCoeff_  * diff;
        else
            envLevel_ += releaseCoeff_ * diff;

        // Map envelope [0, 1] → cutoff frequency
        const float envClamped = envLevel_ < 0.0f ? 0.0f : (envLevel_ > 1.0f ? 1.0f : envLevel_);
        const float cutoffHz   = kMinCutoffHz + envClamped * (kMaxCutoffHz - kMinCutoffHz);

        // Simper SVF coefficients (normalised angular frequency g, damping k)
        const float g  = std::tan(kPi * cutoffHz / srF);
        const float k  = 1.0f / res;
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;

        // Two integrator states  ic1_, ic2_
        const float v3   = drySample - svfIc2_;
        const float v1   = a1 * svfIc1_ + a2 * v3;
        const float v2   = svfIc2_       + a2 * svfIc1_ + a3 * v3;
        svfIc1_ = 2.0f * v1 - svfIc1_;
        svfIc2_ = 2.0f * v2 - svfIc2_;

        // LP output is v2
        const float wetSample = v2;
        buf[i] = clipSample((1.0f - mx) * drySample + mx * wetSample);
    }
}

ParamDescriptor EnvelopeFilterEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kEnvParams[i] : ParamDescriptor{};
}

float EnvelopeFilterEffect::getParam(int i) const noexcept
{
    switch (i)
    {
        case kSensitivity: return sensitivity_.load(std::memory_order_relaxed);
        case kAttack:      return attackMs_.load(std::memory_order_relaxed);
        case kRelease:     return releaseMs_.load(std::memory_order_relaxed);
        case kResonance:   return resonance_.load(std::memory_order_relaxed);
        case kMix:         return mix_.load(std::memory_order_relaxed);
        default:           return 0.0f;
    }
}

void EnvelopeFilterEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
        case kSensitivity:
            sensitivity_.store(v, std::memory_order_relaxed);
            break;
        case kAttack:
            attackMs_.store(v, std::memory_order_relaxed);
            updateCoefficients();
            break;
        case kRelease:
            releaseMs_.store(v, std::memory_order_relaxed);
            updateCoefficients();
            break;
        case kResonance:
            resonance_.store(v, std::memory_order_relaxed);
            break;
        case kMix:
            mix_.store(v, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Presets — 20 presets: funky → dub techno → electro
//
// Each preset: { name, { sensitivity, attack_ms, release_ms, resonance, mix } }
// ─────────────────────────────────────────────────────────────────────────────

struct EnvFilterPreset
{
    const char* name;
    float params[5]; // sens, atk, rel, reso, mix
};

static constexpr EnvFilterPreset kEnvPresets[] = {
    //                              sens    atk    rel    reso   mix
    //
    // Sensitivity : amplifie l'enveloppe du souffle → contrôle l'ouverture du filtre
    // Attack      : vitesse d'ouverture quand tu souffles (1ms = snap, 200ms = swell)
    // Release     : vitesse de fermeture quand tu relâches (30ms = gate, 2000ms = traîne)
    // Resonance   : pic du filtre SVF (1 = doux, 10 = auto-oscillation/sifflement)

    // ═══════════════════════════════════════════════════════════════════════
    // FUNK / WAH — le sax qui parle comme une guitare wah
    // ═══════════════════════════════════════════════════════════════════════
    { "Bootsy Quack",          {  6.0f,   1.0f,   50.0f, 5.5f, 0.90f }},
    // Hyper-sensible, snap immédiat → chaque attaque claque un "wah"
    { "Shaft Groove",          {  3.0f,   5.0f,  180.0f, 2.5f, 0.75f }},
    // Reso modérée, release moyen → wah musicale smooth pour impro funk
    { "Cry Baby",              {  4.5f,   2.0f,  120.0f, 4.0f, 0.85f }},
    // Reso qui pleure, ouverture rapide → expressif comme un pédalier

    // ═══════════════════════════════════════════════════════════════════════
    // ACID — le filtre qui squelche, 303 style
    // ═══════════════════════════════════════════════════════════════════════
    { "303 Squelch",           {  5.0f,   1.0f,   40.0f, 8.0f, 0.95f }},
    // Reso 8 + snap ultra court → chaque note crache de l'acide
    { "Acid Bubble",           {  3.5f,   3.0f,  100.0f, 7.0f, 0.90f }},
    // Reso haute, release moyen → bulles acides qui roulent
    { "Acid Scream",           {  8.0f,   1.0f,   25.0f,10.0f, 1.00f }},
    // Tout à fond : reso max, sens max → le filtre HURLE, full wet

    // ═══════════════════════════════════════════════════════════════════════
    // DUB TECHNO — le filtre qui respire avec le souffle
    // ═══════════════════════════════════════════════════════════════════════
    { "Basic Channel Fog",     {  0.5f,  80.0f, 2000.0f, 1.8f, 0.55f }},
    // Très lent, subtil → le filtre s'ouvre en minutes, brouillard sonore
    { "Deepchord Breath",      {  1.2f,  40.0f, 1200.0f, 2.2f, 0.65f }},
    // Le sax respire → le filtre respire avec, organique
    { "Echospace Veil",        {  0.8f, 120.0f, 1800.0f, 3.0f, 0.50f }},
    // Voile qui s'ouvre très lentement avec reso qui colore

    // ═══════════════════════════════════════════════════════════════════════
    // TECHNO — filtre percussif et tranchant
    // ═══════════════════════════════════════════════════════════════════════
    { "Berghain Gate",         {  7.0f,   1.0f,   30.0f, 3.5f, 0.85f }},
    // Ultra-sensible, release court → chaque note = gate tranchant
    { "Industrial Clank",      {  9.0f,   1.0f,   20.0f, 6.0f, 0.90f }},
    // Sens max, release 20ms → son métallique percussif
    { "Warehouse Pump",        {  2.5f,  10.0f,  250.0f, 4.5f, 0.80f }},
    // Atk/rel moyens → filtre qui pompe avec le groove du sax

    // ═══════════════════════════════════════════════════════════════════════
    // AMBIENT — métamorphose lente, textures
    // ═══════════════════════════════════════════════════════════════════════
    { "Tidal Drift",           {  0.3f, 200.0f, 2000.0f, 1.0f, 0.45f }},
    // Quasi-imperceptible, très lent → le timbre évolue sur 30 secondes
    { "Singing Resonance",     {  1.5f,  30.0f,  800.0f, 8.5f, 0.70f }},
    // Reso très haute + release long → le filtre "chante" après chaque note
    { "Glass Bell",            {  2.0f,   1.0f,  600.0f, 9.5f, 0.60f }},
    // Reso quasi self-osc, atk instant → ping cristallin puis traîne

    // ═══════════════════════════════════════════════════════════════════════
    // EXPRESSIF — pensé pour le jeu live au sax
    // ═══════════════════════════════════════════════════════════════════════
    { "Breath Control",        {  1.8f,  15.0f,  400.0f, 2.0f, 0.70f }},
    // Le filtre suit exactement ta dynamique de souffle, naturel
    { "Growl Enhancer",        {  4.0f,   2.0f,  150.0f, 5.0f, 0.80f }},
    // Amplifie les growls : chaque attaque ouvre le filtre agressivement
    { "Subtone Bloom",         {  0.8f,  50.0f,  800.0f, 1.5f, 0.55f }},
    // Pour jeu doux/subtone : le filtre s'ouvre tout doucement
    { "Overtone Whistle",      {  3.0f,   5.0f,  200.0f, 9.0f, 0.75f }},
    // Reso très haute → fait siffler les harmoniques du sax
};

static constexpr int kEnvPresetCount = static_cast<int>(sizeof(kEnvPresets) / sizeof(kEnvPresets[0]));

int EnvelopeFilterEffect::presetCount() const noexcept { return kEnvPresetCount; }

const char* EnvelopeFilterEffect::presetName(int index) const noexcept
{
    if (index < 0 || index >= kEnvPresetCount) return "";
    return kEnvPresets[index].name;
}

void EnvelopeFilterEffect::applyPreset(int index) noexcept
{
    if (index < 0 || index >= kEnvPresetCount) return;
    const auto& p = kEnvPresets[index].params;
    for (int i = 0; i < kParamCount; ++i)
        setParam(i, p[i]);
}

} // namespace dsp
