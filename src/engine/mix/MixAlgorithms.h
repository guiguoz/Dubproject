#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// engine/mix/MixAlgorithms.h
//
// Helpers DSP purs du magic mix — M9.
// Zéro dépendance JUCE / dsp/ : compilable tel quel par EngineTests (C++17).
// Comportement identique aux originaux (recette M9, nulltest inchangé tant que
// le rendu V2 ne les consomme pas encore).
//
// Contenu :
//   - classification d'instrument (detectContentType, estimateSpectralCentroid)
//   - filtres biquad (HP/LP/shelfs/peaking) + applyBiquad
//   - EQ par rôle (applyRoleEQ), démasquage inter-pistes (applyUnmasking)
//   - ownership sub 30-60 Hz (applySubOwnership)
//   - transitoire kick (applyKickTransient), harmoniques bass
//     (applyBassHarmonics), echo dub (applyDubEcho)
//   - gains cibles et énergie sax par type, spatialisation par type
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace engine::mix {

// ── Types d'instrument (alignés sur les rôles V2) ────────────────────────────

enum class MixContentType { KICK, SNARE, HIHAT, BASS, SYNTH, PAD, PERC, LOOP, OTHER };

enum class MixCategory { KICK, SNARE, HIHAT, BASS, SYNTH, PAD, PERC, OTHER };

inline std::string contentTypeName(MixContentType t)
{
    switch (t)
    {
        case MixContentType::KICK:  return "KICK";
        case MixContentType::SNARE: return "SNR";
        case MixContentType::HIHAT: return "HAT";
        case MixContentType::BASS:  return "BASS";
        case MixContentType::SYNTH: return "SYN";
        case MixContentType::PAD:   return "PAD";
        case MixContentType::PERC:  return "PRC";
        case MixContentType::LOOP:  return "LOOP";
        default:                    return "???";
    }
}

inline constexpr float kTwoPi = 6.28318530718f;

inline constexpr float kBassTargetGain = 0.70f;  // plage recommandée 0.55–0.75

// Retourne le facteur headroom sax : 1.0 pour BASS/KICK, 0.707 pour les autres
inline float saxClearance(MixContentType type) noexcept
{
    if (type == MixContentType::BASS || type == MixContentType::KICK) return 1.00f;
    if (type == MixContentType::LOOP)                                 return 0.85f;
    if (type == MixContentType::SNARE || type == MixContentType::PERC) return 0.75f;
    return 0.707f;  // HIHAT, SYNTH, PAD, OTHER
}

// ── Gain cible par type ──────────────────────────────────────────────────────

inline float targetGainForType(MixContentType type) noexcept
{
    switch (type)
    {
        case MixContentType::KICK:  return 0.65f;              // −3.7 dBFS
        case MixContentType::SNARE: return 0.70f;              // −3.1 dBFS → eff. ×0.75 = 0.525
        case MixContentType::HIHAT: return 0.75f;              // −2.5 dBFS → eff. ×0.707 = 0.530
        case MixContentType::BASS:  return kBassTargetGain;    // −3.1 dBFS
        case MixContentType::SYNTH: return 0.75f;              // −2.5 dBFS → eff. ×0.707 = 0.530
        case MixContentType::PAD:   return 0.85f;              // −1.4 dBFS → eff. ×0.707 = 0.601
        case MixContentType::PERC:  return 0.75f;              // −2.5 dBFS → eff. ×0.75  = 0.563
        case MixContentType::LOOP:  return 0.55f;              // −5.2 dBFS → eff. ×0.85  = 0.468
        default:                    return 0.50f;
    }
}

// ── MixContentType → MixCategory ─────────────────────────────────────────────

inline MixCategory contentTypeToDynamicsCategory(MixContentType t) noexcept
{
    switch (t)
    {
        case MixContentType::KICK:  return MixCategory::KICK;
        case MixContentType::SNARE: return MixCategory::SNARE;
        case MixContentType::HIHAT: return MixCategory::HIHAT;
        case MixContentType::BASS:  return MixCategory::BASS;
        case MixContentType::SYNTH: return MixCategory::SYNTH;
        case MixContentType::PAD:   return MixCategory::PAD;
        case MixContentType::PERC:  return MixCategory::PERC;
        default:                    return MixCategory::OTHER;
    }
}

// ── True-peak estimate via 2× linear oversampling ───────────────────────────

inline float calculateTruePeak(const std::vector<float>& pcm)
{
    const int n = static_cast<int>(pcm.size());
    if (n == 0) return 0.f;
    float peak = 0.f;
    for (int i = 0; i < n - 1; ++i)
    {
        const float s0 = pcm[static_cast<std::size_t>(i)];
        const float s1 = pcm[static_cast<std::size_t>(i + 1)];
        peak = std::max(peak, std::abs(s0));
        peak = std::max(peak, std::abs(s0 + 0.50f * (s1 - s0)));
    }
    peak = std::max(peak, std::abs(pcm.back()));
    return peak;
}

// ── Biquad IIR filter ────────────────────────────────────────────────────────

struct BiquadCoeffs { float b0, b1, b2, a1, a2; };  // a0 normalisé à 1

inline void applyBiquad(std::vector<float>& pcm, const BiquadCoeffs& c)
{
    float x1 = 0.f, x2 = 0.f, y1 = 0.f, y2 = 0.f;
    for (auto& s : pcm)
    {
        const float x0 = s;
        const float y0 = c.b0*x0 + c.b1*x1 + c.b2*x2 - c.a1*y1 - c.a2*y2;
        x2 = x1; x1 = x0;
        y2 = y1; y1 = y0;
        s = y0;
    }
}

inline BiquadCoeffs makeHP(float fc, double sampleRate)
{
    const float w0    = kTwoPi * fc / static_cast<float>(sampleRate);
    const float cosw0 = std::cos(w0);
    const float alpha = std::sin(w0) / (2.f * 0.707f);
    const float a0    = 1.f + alpha;
    return { (1.f + cosw0) / (2.f * a0), -(1.f + cosw0) / a0,
             (1.f + cosw0) / (2.f * a0), -2.f * cosw0 / a0, (1.f - alpha) / a0 };
}

inline BiquadCoeffs makeLP(float fc, double sampleRate)
{
    const float w0    = kTwoPi * fc / static_cast<float>(sampleRate);
    const float cosw0 = std::cos(w0);
    const float alpha = std::sin(w0) / (2.f * 0.707f);
    const float a0    = 1.f + alpha;
    return { (1.f - cosw0) / (2.f * a0), (1.f - cosw0) / a0,
             (1.f - cosw0) / (2.f * a0), -2.f * cosw0 / a0, (1.f - alpha) / a0 };
}

inline BiquadCoeffs makeLowShelf(float fc, float dBgain, double sampleRate)
{
    const float A     = std::pow(10.f, dBgain / 40.f);
    const float w0    = kTwoPi * fc / static_cast<float>(sampleRate);
    const float cosw0 = std::cos(w0);
    const float q     = 2.f * std::sqrt(A) * std::sin(w0) / 2.f;  // S=1
    const float a0    = (A+1.f) + (A-1.f)*cosw0 + q;
    return { A * ((A+1.f) - (A-1.f)*cosw0 + q) / a0,
             2.f * A * ((A-1.f) - (A+1.f)*cosw0) / a0,
             A * ((A+1.f) - (A-1.f)*cosw0 - q) / a0,
             -2.f * ((A-1.f) + (A+1.f)*cosw0) / a0,
             ((A+1.f) + (A-1.f)*cosw0 - q) / a0 };
}

inline BiquadCoeffs makeHighShelf(float fc, float dBgain, double sampleRate)
{
    const float A     = std::pow(10.f, dBgain / 40.f);
    const float w0    = kTwoPi * fc / static_cast<float>(sampleRate);
    const float cosw0 = std::cos(w0);
    const float q     = 2.f * std::sqrt(A) * std::sin(w0) / 2.f;  // S=1
    const float a0    = (A+1.f) - (A-1.f)*cosw0 + q;
    return { A * ((A+1.f) + (A-1.f)*cosw0 + q) / a0,
             -2.f * A * ((A-1.f) + (A+1.f)*cosw0) / a0,
             A * ((A+1.f) + (A-1.f)*cosw0 - q) / a0,
             2.f * ((A-1.f) - (A+1.f)*cosw0) / a0,
             ((A+1.f) - (A-1.f)*cosw0 - q) / a0 };
}

inline BiquadCoeffs makePeaking(float fc, float dBgain, float Q, double sampleRate)
{
    const float A     = std::pow(10.f, dBgain / 40.f);
    const float w0    = kTwoPi * fc / static_cast<float>(sampleRate);
    const float cosw0 = std::cos(w0);
    const float alpha = std::sin(w0) / (2.f * Q);
    const float a0    = 1.f + alpha / A;
    return { (1.f + alpha*A) / a0, -2.f * cosw0 / a0, (1.f - alpha*A) / a0,
             -2.f * cosw0 / a0,    (1.f - alpha/A) / a0 };
}

// ── Classification d'instrument ──────────────────────────────────────────────

/// Lightweight spectral centroid (Hz) using the same 4-band LP cascade as
/// detectContentType(). Band centres: sub≈75, bass≈325, mid≈1750, high≈8000 Hz.
inline float estimateSpectralCentroid(const std::vector<float>& pcm,
                                      double sampleRate) noexcept
{
    if (pcm.empty()) return 1000.f;
    const float fs = static_cast<float>(sampleRate);

    const auto alphaFor = [fs](float fc) noexcept {
        const float t = kTwoPi * fc / fs;
        return t / (t + 1.f);
    };
    const float a150  = alphaFor(150.f);
    const float a500  = alphaFor(500.f);
    const float a3000 = alphaFor(3000.f);

    float y150 = 0.f, y500 = 0.f, y3000 = 0.f;
    float eSub = 0.f, eBass = 0.f, eMid = 0.f, eHigh = 0.f;

    const int N = std::min(static_cast<int>(pcm.size()),
                           static_cast<int>(sampleRate));
    for (int i = 0; i < N; ++i)
    {
        const float x = pcm[static_cast<std::size_t>(i)];
        y150  = a150  * x + (1.f - a150)  * y150;
        y500  = a500  * x + (1.f - a500)  * y500;
        y3000 = a3000 * x + (1.f - a3000) * y3000;
        eSub  += y150  * y150;
        eBass += (y500 - y150)  * (y500 - y150);
        eMid  += (y3000 - y500) * (y3000 - y500);
        eHigh += (x - y3000)    * (x - y3000);
    }
    const float total = eSub + eBass + eMid + eHigh;
    if (total < 1e-8f) return 1000.f;
    return (75.f * eSub + 325.f * eBass + 1750.f * eMid + 8000.f * eHigh) / total;
}

/// Classify a slot by spectral band energies + transient analysis.
inline MixContentType detectContentType(const std::vector<float>& pcm,
                                        double sampleRate)
{
    if (pcm.empty()) return MixContentType::OTHER;
    const float fs = static_cast<float>(sampleRate);

    // 1. Transient ratio: peak / RMS over first 20 ms
    const int attackN = std::min(static_cast<int>(pcm.size()),
                                 static_cast<int>(fs * 0.020f));
    float peak = 0.f, sumSqAtt = 0.f;
    for (int i = 0; i < attackN; ++i)
    {
        peak = std::max(peak, std::abs(pcm[static_cast<std::size_t>(i)]));
        sumSqAtt += pcm[static_cast<std::size_t>(i)] * pcm[static_cast<std::size_t>(i)];
    }
    const float rmsAtt = std::sqrt(sumSqAtt / std::max(attackN, 1));
    const float transientRatio = (rmsAtt > 0.001f) ? peak / rmsAtt : 1.0f;

    // 2. Duration in ms
    const float durationMs = static_cast<float>(pcm.size()) / fs * 1000.f;

    // 3. Band energies via cascaded 1st-order LP state variables
    const auto alphaFor = [fs](float fc) noexcept {
        const float t = kTwoPi * fc / fs;
        return t / (t + 1.f);
    };
    const float a150  = alphaFor(150.f);
    const float a500  = alphaFor(500.f);
    const float a3000 = alphaFor(3000.f);

    float y150 = 0.f, y500 = 0.f, y3000 = 0.f;
    float eSub = 0.f, eBass = 0.f, eMid = 0.f, eHigh = 0.f, eTotal = 0.f;

    const int N = std::min(static_cast<int>(pcm.size()),
                           static_cast<int>(sampleRate));  // max 1 s
    for (int i = 0; i < N; ++i)
    {
        const float x = pcm[static_cast<std::size_t>(i)];
        y150  = a150  * x + (1.f - a150)  * y150;
        y500  = a500  * x + (1.f - a500)  * y500;
        y3000 = a3000 * x + (1.f - a3000) * y3000;
        eSub  += y150  * y150;
        eBass += (y500 - y150)  * (y500 - y150);
        eMid  += (y3000 - y500) * (y3000 - y500);
        eHigh += (x - y3000)    * (x - y3000);
        eTotal += x * x;
    }
    if (eTotal < 1e-8f) return MixContentType::OTHER;

    const float subFrac  = eSub  / eTotal;
    const float lowFrac  = (eSub + eBass) / eTotal;
    const float midFrac  = eMid  / eTotal;
    const float highFrac = eHigh / eTotal;

    // Classification rules (order matters — most specific first)
    if (transientRatio > 3.0f && subFrac   > 0.28f && durationMs < 600.f)
        return MixContentType::KICK;
    if (transientRatio > 2.5f && highFrac  > 0.40f && durationMs < 400.f)
        return MixContentType::HIHAT;
    if (transientRatio > 4.5f && highFrac  > 0.30f && durationMs < 500.f)
        return MixContentType::HIHAT;
    if (transientRatio > 2.5f && midFrac   > 0.28f && durationMs < 700.f)
        return MixContentType::SNARE;
    if (transientRatio < 2.0f && lowFrac   > 0.50f)
        return MixContentType::BASS;
    if (transientRatio < 1.8f && durationMs > 1500.f && highFrac < 0.35f)
        return MixContentType::PAD;
    if (transientRatio > 2.5f)
        return MixContentType::PERC;
    if (transientRatio < 2.0f && midFrac   > 0.35f)
        return MixContentType::SYNTH;
    return MixContentType::OTHER;
}

// ── Spatialisation par type ──────────────────────────────────────────────────

struct SpatialDecision
{
    float pan   = 0.f;  // −1.0 (L) … +1.0 (R)
    float width = 0.f;  // 0 = mono, 1 = max Haas (25 ms)
    float depth = 0.f;  // 0 = front, 1 = back (high-shelf −6dB @ 8kHz)
};

/// Same defaults as spatialForType(); centroid enforces sub-bass mono.
inline SpatialDecision computeSpatialization(int slot, MixContentType type,
                                             float centroid) noexcept
{
    float pan = 0.f, width = 0.f, depth = 0.f;
    switch (type)
    {
        case MixContentType::KICK:
            pan = 0.f;  width = 0.f;  depth = 0.f;   break;
        case MixContentType::SNARE:
            pan = 0.f;  width = 0.1f; depth = 0.1f;  break;
        case MixContentType::BASS:
            pan = 0.f;  width = 0.f;  depth = 0.f;   break;
        case MixContentType::HIHAT:
            // Alternating L/R by slot index → natural stereo when 2 hats loaded
            pan = (slot % 2 == 0) ? 0.4f : -0.4f;
            width = 0.3f;  depth = 0.2f;              break;
        case MixContentType::PAD:
            pan = 0.f;  width = 0.8f; depth = 0.6f;  break;
        case MixContentType::SYNTH:
            pan = 0.f;  width = 0.4f; depth = 0.3f;  break;
        case MixContentType::PERC:
            pan = 0.3f; width = 0.2f; depth = 0.2f;  break;
        default:
            pan = 0.f;  width = 0.2f; depth = 0.2f;  break;
    }

    // Sub-bass mono enforcement: centroid < 200 Hz → pan=0, width=0
    if (centroid < 200.f) { pan = 0.f; width = 0.f; }

    return { pan, width, depth };
}

/// Defaults used by the UI (neutral centroid 1000 Hz).
inline SpatialDecision spatialForType(int slot, MixContentType type) noexcept
{
    return computeSpatialization(slot, type, 1000.f);
}

// ── Transient shaper (kick) ──────────────────────────────────────────────────

inline void applyKickTransient(std::vector<float>& pcm, double sr) noexcept
{
    if (pcm.empty()) return;
    const float fs = static_cast<float>(sr);
    const float cFastA = std::exp(-1.f / (fs * 0.0008f));  // 0.8 ms — front d'attaque
    const float cFastR = std::exp(-1.f / (fs * 0.040f));   // 40 ms
    const float cSlowA = std::exp(-1.f / (fs * 0.020f));   // 20 ms — corps
    const float cSlowR = std::exp(-1.f / (fs * 0.200f));   // 200 ms

    float envFast = 0.f, envSlow = 0.f;
    for (float& s : pcm)
    {
        const float ab = std::abs(s);
        envFast = (ab > envFast) ? cFastA * envFast + (1.f - cFastA) * ab
                                 : cFastR * envFast;
        envSlow = (ab > envSlow) ? cSlowA * envSlow + (1.f - cSlowA) * ab
                                 : cSlowR * envSlow;
        const float t = (envFast - envSlow) / std::max(envFast, 1e-9f);
        s *= 1.f + 0.4f * std::max(t, 0.f);  // ≤ +3 dB au pic de transitoire
    }
}

// ── Saturation harmonique (bass) ─────────────────────────────────────────────

inline void applyBassHarmonics(std::vector<float>& pcm, double sr) noexcept
{
    applyBiquad(pcm, makeHP(28.f, sr));
    constexpr float kDrive  = 1.6f;
    const float     invNorm = 1.f / std::tanh(kDrive);
    for (float& s : pcm)
        s = std::tanh(kDrive * s) * invNorm;  // gain ≈ 0 dB en régime linéaire
}

// ── Sub ownership 30-60 Hz ───────────────────────────────────────────────────

inline void applySubOwnership(std::vector<float>* pcms,
                              const MixContentType* types,
                              int numSlots,
                              double sr) noexcept
{
    auto subEnergy = [](const std::vector<float>& pcm, double sampleRate)
    {
        const float a = std::exp(-2.f * 3.14159265f * 60.f
                                 / static_cast<float>(sampleRate));
        const float c = 1.f - a;
        float z = 0.f, e = 0.f;
        // 8192 samples (~185 ms @ 44.1 kHz) — covers slow 808-style kicks
        const int n = std::min(8192, static_cast<int>(pcm.size()));
        for (int i = 0; i < n; ++i)
        {
            z = c * pcm[static_cast<std::size_t>(i)] + a * z;
            e += z * z;
        }
        return (n > 0) ? e / static_cast<float>(n) : 0.f;
    };

    int   kickSlot = -1, bassSlot = -1;
    float kickSub  = 0.f, bassSub  = 0.f;
    for (int i = 0; i < numSlots; ++i)
    {
        if (pcms[i].empty()) continue;
        if (types[i] == MixContentType::KICK)
        {
            const float e = subEnergy(pcms[i], sr);
            if (e > kickSub) { kickSub = e; kickSlot = i; }
        }
        if (types[i] == MixContentType::BASS)
        {
            const float e = subEnergy(pcms[i], sr);
            if (e > bassSub) { bassSub = e; bassSlot = i; }
        }
    }
    if (kickSlot < 0 || bassSlot < 0) return;

    const float maxE = std::max(kickSub, bassSub);
    const float minE = std::max(std::min(kickSub, bassSub), 1e-12f);
    const float ratio = maxE / minE;

    if (ratio < 1.25f)
    {
        // Zone neutre : sub-énergie similaire → half-cut léger sur les deux
        applyBiquad(pcms[kickSlot], makeLowShelf(60.f, -1.5f, sr));
        applyBiquad(pcms[bassSlot], makeLowShelf(55.f, -1.5f, sr));
        return;
    }

    if (kickSub >= bassSub)
        applyBiquad(pcms[bassSlot], makeLowShelf(55.f, -4.f, sr));
    else
        applyBiquad(pcms[kickSlot], makePeaking(45.f, -3.f, 0.7f, sr));
}

// ── EQ par rôle ──────────────────────────────────────────────────────────────

inline void applyRoleEQ(std::vector<float>& pcm, MixContentType type, double sr)
{
    switch (type)
    {
        case MixContentType::KICK:
            applyBiquad(pcm, makeHP(30.f, sr));
            applyBiquad(pcm, makeLowShelf(45.f,  6.f, sr));
            applyBiquad(pcm, makePeaking (80.f,  2.f, 1.0f, sr));
            applyBiquad(pcm, makePeaking (300.f,-4.f, 1.5f, sr));
            applyBiquad(pcm, makeLP(2800.f, sr));                    // ← 1800 → 2800 Hz
            break;
        case MixContentType::SNARE:
            applyBiquad(pcm, makeHP(80.f, sr));
            applyBiquad(pcm, makePeaking(200.f,  2.f, 1.5f, sr));
            applyBiquad(pcm, makePeaking(4500.f, 2.f, 2.0f, sr));    // ← snap un peu plus présent
            applyBiquad(pcm, makeHighShelf(8000.f, -1.5f, sr));      // ← -3 → -1.5 dB
            break;
        case MixContentType::HIHAT:
            applyBiquad(pcm, makeHP(600.f, sr));
            applyBiquad(pcm, makePeaking(3000.f, -1.f, 1.5f, sr));   // ← moins agressif
            applyBiquad(pcm, makeHighShelf(8000.f, -2.f, sr));       // ← -4 → -2 dB
            break;
        case MixContentType::BASS:
            applyBiquad(pcm, makeHP(25.f, sr));
            applyBiquad(pcm, makeLowShelf(60.f,  5.f, sr));
            applyBiquad(pcm, makePeaking (120.f, 2.f, 1.0f, sr));
            applyBiquad(pcm, makePeaking (250.f,-2.f, 1.5f, sr));
            applyBiquad(pcm, makeLP(7500.f, sr));                    // ← 5500 → 7500 Hz
            break;
        case MixContentType::SYNTH:
            applyBiquad(pcm, makeHP(80.f, sr));
            applyBiquad(pcm, makePeaking(400.f,  1.f, 1.5f, sr));
            applyBiquad(pcm, makePeaking(1200.f,-1.f, 1.5f, sr));
            applyBiquad(pcm, makeHighShelf(3500.f, -2.5f, sr));      // ← -5 → -2.5 dB
            break;
        case MixContentType::PAD:
            applyBiquad(pcm, makeHP(50.f, sr));
            applyBiquad(pcm, makePeaking(200.f, 3.f, 1.0f, sr));
            applyBiquad(pcm, makeHighShelf(2500.f, -3.5f, sr));      // ← -7 → -3.5 dB
            break;
        case MixContentType::PERC:
            applyBiquad(pcm, makeHP(100.f, sr));
            applyBiquad(pcm, makePeaking(5000.f, 0.f, 2.f, sr));     // ← présence neutre
            applyBiquad(pcm, makeHighShelf(7000.f, -1.5f, sr));      // ← -3 → -1.5 dB
            break;
        case MixContentType::LOOP:
            // Loop de drums complète (kick+hihat) — EQ neutre, préserve toute la bande
            applyBiquad(pcm, makeHP(30.f, sr));
            applyBiquad(pcm, makeLowShelf(80.f, 1.f, sr));
            applyBiquad(pcm, makeHighShelf(8000.f, 0.f, sr));        // ← flat
            break;
        case MixContentType::OTHER:
            applyBiquad(pcm, makeHP(60.f, sr));
            applyBiquad(pcm, makeHighShelf(6000.f, -3.f, sr));
            break;
    }
}

// ── Démasquage inter-pistes ──────────────────────────────────────────────────

inline void applyUnmasking(std::vector<float>& pcm, int slot,
                           const MixContentType* types, int numSlots,
                           double sr)
{
    const MixContentType myType = types[slot];

    bool kickPresent = false;
    int  bassCount   = 0;
    int  percCount   = 0;

    for (int i = 0; i < numSlots; ++i)
    {
        if (types[i] == MixContentType::KICK) kickPresent = true;
        if (types[i] == MixContentType::BASS) ++bassCount;
        if (types[i] == MixContentType::PERC
         || types[i] == MixContentType::SNARE
         || types[i] == MixContentType::KICK) ++percCount;
    }

    // Rule 1: KICK present → duck sub of BASS/SYNTH to let kick punch through
    if (kickPresent)
    {
        if (myType == MixContentType::BASS)
            applyBiquad(pcm, makePeaking(70.f, -3.f, 1.0f, sr));
        if (myType == MixContentType::SYNTH)
            applyBiquad(pcm, makePeaking(60.f, -1.f, 1.5f, sr));
    }

    // Rule 2: 2+ BASS tracks → secondary BASS slots get a 100 Hz cut
    if (myType == MixContentType::BASS && bassCount >= 2)
    {
        bool isSecondary = false;
        for (int i = 0; i < slot; ++i)
            if (types[i] == MixContentType::BASS) { isSecondary = true; break; }
        if (isSecondary)
            applyBiquad(pcm, makePeaking(100.f, -2.f, 1.0f, sr));
    }

    // Rule 3: KICK + SNARE overlap → SNARE cut at 300 Hz
    if (kickPresent && myType == MixContentType::SNARE)
        applyBiquad(pcm, makePeaking(300.f, -2.f, 1.5f, sr));

    // Rule 4: 3+ percussive tracks → secondary ones cut 2 kHz presence
    if (percCount >= 3
     && (myType == MixContentType::PERC || myType == MixContentType::SNARE))
    {
        bool isSecondary = false;
        for (int i = 0; i < slot; ++i)
        {
            if (types[i] == MixContentType::PERC
             || types[i] == MixContentType::SNARE)
            { isSecondary = true; break; }
        }
        if (isSecondary)
            applyBiquad(pcm, makePeaking(2000.f, -2.f, 1.5f, sr));
    }
}

// ── Echo dub rythmique (offline sur le PCM) ─────────────────────────────────

inline void applyDubEcho(std::vector<float>& pcm, double bpm, double sr,
                          float feedback, int divisions)
{
    if (bpm <= 0.0 || pcm.empty()) return;
    const int delaySamples = static_cast<int>((60.0 / bpm) * sr * (4.0 / divisions));
    if (delaySamples <= 0 || delaySamples >= static_cast<int>(pcm.size())) return;

    // Bandpass the feedback path (80 Hz – 8 kHz): no sub mud, no harsh highs.
    std::vector<float> filtered = pcm;
    applyBiquad(filtered, makeHP(80.f, sr));
    applyBiquad(filtered, makeLP(8000.f, sr));

    std::vector<float> out(pcm.size(), 0.f);
    for (int i = 0; i < static_cast<int>(pcm.size()); ++i)
    {
        out[static_cast<std::size_t>(i)] = pcm[static_cast<std::size_t>(i)];
        if (i >= delaySamples)
            out[static_cast<std::size_t>(i)] +=
                filtered[static_cast<std::size_t>(i - delaySamples)] * feedback;
    }
    pcm = std::move(out);
}

} // namespace engine::mix