#include "HarmonizerEffect.h"

#include <algorithm>

namespace dsp {

static constexpr ParamDescriptor kHarmParams[3] = {
    { "voice0", "Voice 1", -12.0f, 12.0f,  3.0f },
    { "voice1", "Voice 2", -12.0f, 12.0f, -5.0f },
    { "mix",    "Mix",       0.0f,  1.0f,  0.5f  },
};

void HarmonizerEffect::prepare(double sampleRate, int maxBlockSize) noexcept
{
    harmonizer_.prepare(sampleRate, maxBlockSize);
    scratchBuf_.fill(0.0f);
}

void HarmonizerEffect::process(float* buf, int numSamples, float pitchHz) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    const int n = std::min(numSamples, kMaxBlock);
    harmonizer_.process(buf, scratchBuf_.data(), n, pitchHz);

    for (int i = 0; i < n; ++i)
        buf[i] = scratchBuf_[i];
}

void HarmonizerEffect::reset() noexcept
{
    harmonizer_.reset();
}

ParamDescriptor HarmonizerEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kHarmParams[i] : ParamDescriptor{};
}

float HarmonizerEffect::getParam(int i) const noexcept
{
    switch (i)
    {
        case kVoice0: return harmonizer_.getVoiceInterval(0);
        case kVoice1: return harmonizer_.getVoiceInterval(1);
        case kMix:    return harmonizer_.getMix();
        default:      return 0.0f;
    }
}

void HarmonizerEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
        case kVoice0: harmonizer_.setVoiceInterval(0, v); break;
        case kVoice1: harmonizer_.setVoiceInterval(1, v); break;
        case kMix:    harmonizer_.setMix(v);              break;
        default:      break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Presets — intervalles musicaux pour saxophone live
//
// Chaque preset : { voice0 (demi-tons), voice1 (demi-tons), mix }
//
// Rappel intervalles :
//   +3 = tierce mineure    +4 = tierce majeure     +5 = quarte
//   +7 = quinte            +9 = sixte majeure     +12 = octave
//  -12 = octave grave      -5 = quarte descendante -7 = quinte desc.
// ─────────────────────────────────────────────────────────────────────────────

struct HarmPreset
{
    const char* name;
    float params[3]; // voice0, voice1, mix
};

static constexpr HarmPreset kHarmPresets[] = {
    //                              v0     v1     mix

    // ═══════════════════════════════════════════════════════════════════════
    // ACCORDS CLASSIQUES — harmonies naturelles
    // ═══════════════════════════════════════════════════════════════════════
    { "Tierce Majeure",        {  4.0f, -12.0f, 0.30f }},
    // +4 (tierce maj) + octave grave → accord majeur chaud
    { "Tierce Mineure",        {  3.0f, -12.0f, 0.30f }},
    // +3 (tierce min) + octave grave → mélancolique, jazz
    { "Quinte Ouverte",        {  7.0f, -5.0f,  0.25f }},
    // +7 (quinte) + quarte desc → power chord, puissant
    { "Sixte Douce",           {  9.0f,  4.0f,  0.25f }},
    // +9 (sixte maj) + tierce maj → doux, gospel, soul

    // ═══════════════════════════════════════════════════════════════════════
    // OCTAVES — épaissir le son
    // ═══════════════════════════════════════════════════════════════════════
    { "Octave Up",             { 12.0f,  0.0f,  0.35f }},
    // +12 → doublage une octave au-dessus, brillant
    { "Octave Down",           {-12.0f,  0.0f,  0.40f }},
    // -12 → basse d'octave, gros son
    { "Double Octave",         { 12.0f,-12.0f,  0.30f }},
    // +12 et -12 → 3 octaves simultanées, orgue
    { "Sub + Fifth",           {-12.0f,  7.0f,  0.30f }},
    // Octave grave + quinte → fondation massive + harmonique

    // ═══════════════════════════════════════════════════════════════════════
    // INTERVALLES MODAUX — couleurs jazz / world
    // ═══════════════════════════════════════════════════════════════════════
    { "Quartes (McCoy)",       {  5.0f, 10.0f,  0.25f }},
    // Quarte + quarte empilée → voicing McCoy Tyner, modal jazz
    { "Sus4 Ouvert",           {  5.0f, -7.0f,  0.20f }},
    // Quarte + quinte desc → suspendu, pas de tierce, planant
    { "Phrygien",              {  1.0f, -11.0f, 0.30f }},
    // Seconde mineure + 7ème desc → couleur espagnole/arabe
    { "Lydien Bright",         {  6.0f, 11.0f,  0.20f }},
    // Triton + 7ème maj → couleur lydienne lumineuse

    // ═══════════════════════════════════════════════════════════════════════
    // DUB / TECHNO — textures électroniques
    // ═══════════════════════════════════════════════════════════════════════
    { "Dub Fifth",             {  7.0f, -12.0f, 0.35f }},
    // Quinte + sub → dub massif, fondation + harmonique
    { "Detune Chorus",         {  0.0f,  0.0f,  0.40f }},
    // Unisson (0 demi-tons) → chorus naturel via le WSOLA
    { "Minor Dub",             {  3.0f, -7.0f,  0.35f }},
    // Tierce min + quinte desc → accord mineur sombre, dub

    // ═══════════════════════════════════════════════════════════════════════
    // DISSONANCES CRÉATIVES — tension et texture
    // ═══════════════════════════════════════════════════════════════════════
    { "Triton (Diabolus)",     {  6.0f, -6.0f,  0.25f }},
    // +6 et -6 → triton pur, tension maximum
    { "Cluster",               {  1.0f,  2.0f,  0.35f }},
    // Seconde min + seconde maj → cluster dense, abrasif
    { "Septième Mineure",      { 10.0f,  3.0f,  0.25f }},
    // 7ème min + tierce min → accord m7, jazzy
    { "Neuvième",              {  2.0f,  7.0f,  0.20f }},
    // Seconde maj + quinte → add9, ouvert et moderne

    // ═══════════════════════════════════════════════════════════════════════
    // FULL WET — le sax disparaît, reste l'harmonie
    // ═══════════════════════════════════════════════════════════════════════
    { "Ghost Quinte",          {  7.0f, -5.0f,  0.85f }},
    // Full wet quinte + quarte → le sax original quasi absent
    { "Alien Choir",           { 11.0f, -8.0f,  0.90f }},
    // 7ème maj + 6ème min desc → harmoniques irréelles, chœur alien
};

static constexpr int kHarmPresetCount = static_cast<int>(sizeof(kHarmPresets) / sizeof(kHarmPresets[0]));

int HarmonizerEffect::presetCount() const noexcept { return kHarmPresetCount; }

const char* HarmonizerEffect::presetName(int index) const noexcept
{
    if (index < 0 || index >= kHarmPresetCount) return "";
    return kHarmPresets[index].name;
}

void HarmonizerEffect::applyPreset(int index) noexcept
{
    if (index < 0 || index >= kHarmPresetCount) return;
    const auto& p = kHarmPresets[index].params;
    for (int i = 0; i < kParamCount; ++i)
        setParam(i, p[i]);
}

} // namespace dsp
