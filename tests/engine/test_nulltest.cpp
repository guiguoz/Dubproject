#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>
#include "engine/OfflineRender.h"

using namespace engine;

// ═══ NULL-TEST (§11.2) ═══════════════════════════════════════════════════════
// Rendu offline déterministe complet (séquenceur + transitions + AutoMix v2.0).
// Toute modification de rendu NON intentionnelle doit faire échouer NULL1.
// Une modification INTENTIONNELLE régénère la référence dans le même commit,
// avec justification (voir la fin de ce fichier).

static constexpr double kSr = 44100.0;

// ─── Helpers PCM déterministes ────────────────────────────────────────────────

static OfflinePcm makeSinePcm(float freqHz, float amp, int numFrames,
                              float sr = static_cast<float>(kSr)) {
    OfflinePcm p;
    p.numChannels = 1;
    p.sampleRate  = sr;
    p.data.resize(static_cast<size_t>(numFrames));
    const float w = 2.f * 3.141592653589793f * freqHz / sr;
    for (int i = 0; i < numFrames; ++i)
        p.data[static_cast<size_t>(i)] = amp * std::sin(w * static_cast<float>(i));
    return p;
}

// Bruit déterministe (LCG 32 bits) avec enveloppe de décroissance.
static OfflinePcm makeNoisePcm(float amp, int numFrames, uint32_t seed) {
    OfflinePcm p;
    p.numChannels = 1;
    p.sampleRate  = static_cast<float>(kSr);
    p.data.resize(static_cast<size_t>(numFrames));
    uint32_t state = seed;
    for (int i = 0; i < numFrames; ++i) {
        state = state * 1664525u + 1013904223u;
        const float v = (static_cast<float>(state) / 4294967296.f) * 2.f - 1.f;
        const float env = std::exp(-3.f * static_cast<float>(i)
                                   / static_cast<float>(numFrames));
        p.data[static_cast<size_t>(i)] = amp * env * v;
    }
    return p;
}

// Kick : fréquence glissante 120→40 Hz, enveloppe exponentielle.
static OfflinePcm makeKickPcm() {
    const int frames = static_cast<int>(kSr * 0.22);
    OfflinePcm p;
    p.numChannels = 1;
    p.sampleRate  = static_cast<float>(kSr);
    p.data.resize(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSr);
        const float f = 120.f - 80.f * std::min(1.f, t / 0.15f);
        const float env = std::exp(-20.f * t);
        p.data[static_cast<size_t>(i)] = 0.9f * env * std::sin(2.f * 3.141592653589793f * f * t);
    }
    return p;
}

// ─── FNV-1a 64 bits ───────────────────────────────────────────────────────────

static uint64_t fnv1a(const void* data, size_t bytes) {
    uint64_t h = 1469598103934665603ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// ─── Fixture de référence : 4 scènes, 6 samples, 2 transitions ────────────────

static OfflineSession makeFixture() {
    const double spb = kSr * 60.0 / 120.0;            // 22050
    const int64_t bar = static_cast<int64_t>(spb) * 4; // 88200

    OfflineSession s;
    s.bpm          = 120.0;
    s.sampleRate   = kSr;
    s.maxBlockSize = 512;
    s.numScenes    = 4;
    s.startScene   = 0;

    // ── 6 samples ────────────────────────────────────────────────────────────
    s.pcm[0] = makeSinePcm(330.f, 0.30f, static_cast<int>(kSr) * 1);      // melodic
    s.pcm[1] = makeSinePcm(55.f,  0.45f, static_cast<int>(kSr) * 1);      // bass
    s.pcm[2] = makeKickPcm();                                             // kick
    s.pcm[3] = makeNoisePcm(0.50f, static_cast<int>(kSr * 0.12f), 7u);    // snare
    s.pcm[4] = makeNoisePcm(0.30f, static_cast<int>(kSr * 0.03f), 11u);   // hat
    // Pad LOOP SYNC : 2 mesures à 126 BPM source (8 beats → 168000 frames).
    {
        const double spbSrc = kSr * 60.0 / 126.0;
        const int frames = static_cast<int>(std::round(8.0 * spbSrc));
        s.pcm[5] = makeSinePcm(220.f, 0.25f, frames);
        s.loopSync[5].enabled   = true;
        s.loopSync[5].loopBeats = 8;
        s.loopSync[5].timeRatio = 126.f / 120.f;
    }

    // ── Patterns (globaux, 16 steps) ─────────────────────────────────────────
    for (int slot = 0; slot < kMaxSlots; ++slot)
        s.patterns[slot].numSteps = 16;
    s.patterns[0].steps[0] = s.patterns[0].steps[6] = s.patterns[0].steps[10] = true;
    for (int st : {0, 3, 6, 9, 12, 15}) s.patterns[1].steps[st] = true;
    s.patterns[2].steps[0] = s.patterns[2].steps[4] = s.patterns[2].steps[8]
                           = s.patterns[2].steps[12] = true;
    s.patterns[3].steps[4] = s.patterns[3].steps[12] = true;
    for (int st = 1; st < 16; st += 2) s.patterns[4].steps[st] = true;
    s.patterns[5].steps[0] = true;

    // ── Rôles ────────────────────────────────────────────────────────────────
    s.scenes[0].slots[0].role = SlotRole::Melodic;
    s.scenes[0].slots[1].role = SlotRole::Bass;
    s.scenes[0].slots[2].role = SlotRole::Kick;
    s.scenes[0].slots[3].role = SlotRole::Snare;
    s.scenes[0].slots[4].role = SlotRole::Perc;
    s.scenes[0].slots[5].role = SlotRole::Pad;
    for (int i = 1; i < 4; ++i)
        for (int slot = 0; slot < kMaxSlots; ++slot)
            s.scenes[i].slots[slot].role = s.scenes[0].slots[slot].role;

    // ── Scène 0 « Groove » : kick + snare + hat (density 0.33 → musical) ─────
    s.scenes[0].slots[2].active = true;
    s.scenes[0].slots[3].active = true;
    s.scenes[0].slots[4].active = true;
    s.scenes[0].name = "Groove";

    // ── Scène 1 « Pad » : bass + pad (density 0.22 → calme) ──────────────────
    s.scenes[1].slots[1].active = true;
    s.scenes[1].slots[5].active = true;
    s.scenes[1].slots[1].mode = PlayMode::Free;
    s.scenes[1].slots[5].mode = PlayMode::LoopSync;
    s.scenes[1].name = "Pad";

    // ── Scène 2 « Full » : tout (density 0.67 → musical) ─────────────────────
    for (int slot = 0; slot < 6; ++slot) {
        s.scenes[2].slots[slot].active = true;
        if (slot == 5)      s.scenes[2].slots[slot].mode = PlayMode::LoopSync;
        else if (slot == 1) s.scenes[2].slots[slot].mode = PlayMode::Free;
    }
    s.scenes[2].name = "Full";

    // ── Scène 3 « Stripped » : kick + snare (density 0.22 → calme) ───────────
    s.scenes[3].slots[2].active = true;
    s.scenes[3].slots[3].active = true;
    s.scenes[3].name = "Stripped";

    // ── 2 transitions (à 4 et 8 mesures) ─────────────────────────────────────
    s.transitions.push_back({ 4 * bar, 1 });   // Groove → Pad (Breakdown)
    s.transitions.push_back({ 8 * bar, 2 });   // Pad → Full (Build)

    return s;
}

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST_CASE("NULL1: offline render bit-exact vs reference (§11.2)", "[nulltest]") {
    const OfflineSession s = makeFixture();
    const int64_t total = 12 * static_cast<int64_t>(kSr * 60.0 / 120.0) * 4; // 12 mesures
    const std::vector<float> out = renderOffline(s, total);
    REQUIRE(out.size() == static_cast<size_t>(total) * 2u);

    // Sanité : non-silence, pas de NaN/inf
    float peak = 0.f;
    bool bad = false;
    for (const float v : out) {
        peak = std::max(peak, std::abs(v));
        bad |= (std::isnan(v) || std::isinf(v));
    }
    REQUIRE_FALSE(bad);
    REQUIRE(peak > 0.001f);

    const uint64_t hashAudio = fnv1a(out.data(), out.size() * sizeof(float));

    // Diff RMS par fenêtre : fenêtre de 1024 frames stéréo.
    std::vector<float> rms;
    constexpr size_t kWinFrames = 1024;
    const size_t winSamples = kWinFrames * 2u;
    for (size_t w = 0; w + winSamples <= out.size(); w += winSamples) {
        double sum = 0.0;
        for (size_t i = 0; i < winSamples; ++i) {
            const double v = out[w + i];
            sum += v * v;
        }
        rms.push_back(static_cast<float>(std::sqrt(sum / static_cast<double>(winSamples))));
    }
    const uint64_t hashRms = fnv1a(rms.data(), rms.size() * sizeof(float));

    INFO("hashAudio=" << std::hex << hashAudio << " hashRms=" << hashRms);
    // Références committées (§11.2). Régénérer UNIQUEMENT sur changement
    // intentionnel du rendu, avec justification dans le commit.
    //
    // Référence v9 — Retrigger : fade-out court + attaque immédiate (kRetrigFadeLen 256→64).
    // Correction précédente (v8) : crossfade symétrique 256 frames (somme gains = 1.0).
    // Problème : fade-in 256 frames sur la nouvelle voix mangeait l'attaque (5.8 ms de silence
    // perçu → "décalage" sur kick/basse). Correction : seule l'ancienne voix fade-out (64 frames
    // ≈1.5 ms) ; la nouvelle voix démarre à plein gain immédiatement.
    // Slots affectés dans le fixture : slot 0 (mélodique OneShot, retrigger step 6 sur step 0)
    // et slot 1 (basse Free, retriggered toutes les 3 steps).
    // Pendant les 64 frames de chevauchement, la somme vaut entre C et 2C (overlap bref).
    // LoopSync (slot 5) exclu du mécanisme de retrigger : position dérivée du transport,
    // pas de double-voix possible → inchangé.
    //
    // v10 — Chaîne master warmth + limiter (ajout WarmthProcessor + MasterLimiter refait) :
    // • WarmthProcessor : saturation parallèle tanh (drive=1.0, mix=0.3), compensée en gain
    //   bas-niveau → A/B sans biais de volume.
    // • MasterLimiter : peak limiter par bloc stéréo-couplé (seuil 0.98 ≈ −0.17 dBFS),
    //   identité exacte sous le seuil (remplace l'ancien tanh/clip qui boostait bas niveau).
    // Signature : peak = 0.98 exact sur toutes les scènes (limiter actif), RMS légèrement
    // relevé par les harmoniques de la saturation.
    //
    // v11 — Invariant temporel P0-1/P0-2 (SlotPlayer::processBlock) :
    // • chaque segment est rendu AVANT dispatch de ses events (plus de fuite
    //   backward : un trigger@K ne sonne plus dans [0,K)) ;
    // • renderLoopSync() lit le début absolu du sous-bloc (subTs), anchor =
    //   temps absolu de l'event (plus de tranche rejouée depuis blockStart).
    // Slots affectés dans le fixture : slot 5 (pad LoopSync, triggers à offsets
    // mid-bloc) + tous les retriggers OneShot/Free (attaques désormais exactes
    // au sample, sans pré-écho jusqu'à un bloc).
    // Changement INTENTIONNEL du rendu. Régénéré via NULL0.
    //   hashAudio = 0xf89675cd5785446c
    //   hashRms   = 0x36d0d2ee976ff299
    CHECK(hashAudio == 0xf89675cd5785446cull);
    CHECK(hashRms   == 0x36d0d2ee976ff299ull);
}

TEST_CASE("NULL2: offline render deterministic across runs", "[nulltest]") {
    const OfflineSession s = makeFixture();
    const int64_t total = 4 * static_cast<int64_t>(kSr * 60.0 / 120.0) * 4; // 4 mesures
    const std::vector<float> a = renderOffline(s, total);
    const std::vector<float> b = renderOffline(s, total);
    REQUIRE(a == b);
}

TEST_CASE("NULL0: print reference hashes (régénération)", "[nulltest][.hidden]") {
    const OfflineSession s = makeFixture();
    const int64_t total = 12 * static_cast<int64_t>(kSr * 60.0 / 120.0) * 4;
    const std::vector<float> out = renderOffline(s, total);

    const uint64_t hashAudio = fnv1a(out.data(), out.size() * sizeof(float));

    std::vector<float> rms;
    constexpr size_t kWinFrames = 1024;
    const size_t winSamples = kWinFrames * 2u;
    for (size_t w = 0; w + winSamples <= out.size(); w += winSamples) {
        double sum = 0.0;
        for (size_t i = 0; i < winSamples; ++i) {
            const double v = out[w + i];
            sum += v * v;
        }
        rms.push_back(static_cast<float>(std::sqrt(sum / static_cast<double>(winSamples))));
    }
    const uint64_t hashRms = fnv1a(rms.data(), rms.size() * sizeof(float));

    std::cout << "=== NULL-TEST ANALYSE DU RENDU DE RÉFÉRENCE ===\n";
    std::cout << "hashAudio=0x" << std::hex << hashAudio
              << " hashRms=0x" << hashRms << std::dec << "\n\n";

    const double spb = kSr * 60.0 / 120.0;
    const int64_t barFrames = static_cast<int64_t>(spb) * 4; // 88200
    const char* sceneNames[4] = { "Scène 0: Groove (Kick+Snare+Hat)",
                                   "Scène 1: Pad (Bass+Pad)",
                                   "Scène 2: Full (Tout)",
                                   "Scène 3: Stripped (Kick+Snare)" };

    for (int sc = 0; sc < 4; ++sc) {
        const int64_t startFrame = sc * 3 * barFrames; // 3 mesures par scène dans 12 mesures total ? Non, 4 scènes x 3 mesures = 12
        const int64_t numFr = 3 * barFrames;
        double sumSq = 0.0;
        float scenePeak = 0.f;
        for (int64_t f = 0; f < numFr; ++f) {
            const size_t idx = static_cast<size_t>(startFrame + f) * 2u;
            if (idx + 1 >= out.size()) break;
            const float l = out[idx];
            const float r = out[idx + 1];
            sumSq += l * l + r * r;
            scenePeak = std::max(scenePeak, std::max(std::abs(l), std::abs(r)));
        }
        const double sceneRms = std::sqrt(sumSq / static_cast<double>(numFr * 2));
        const double rmsDb  = (sceneRms > 1e-9) ? 20.0 * std::log10(sceneRms) : -120.0;
        const double peakDb = (scenePeak > 1e-9) ? 20.0 * std::log10(scenePeak) : -120.0;

        std::cout << "[" << sceneNames[sc] << "]\n";
        std::cout << "  - Peak : " << scenePeak << " (" << peakDb << " dBFS)\n";
        std::cout << "  - RMS  : " << sceneRms << " (" << rmsDb << " dBFS)\n";
    }
    std::cout << "===============================================\n";
}
