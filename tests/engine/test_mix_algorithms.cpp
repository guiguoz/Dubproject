#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>
#include "engine/mix/MixAlgorithms.h"
#include "engine/mix/MixDecisions.h"
#include "engine/mix/MixEngine.h"
#include "engine/mix/MixState.h"

using namespace engine::mix;
using Catch::Approx;

static constexpr float kSR   = 44100.f;
static constexpr int   kSlots = 9;

static std::vector<float> makeSine(float freq, int numFrames, float sr = kSR, float amp = 0.5f) {
    std::vector<float> v(static_cast<size_t>(numFrames));
    for (int i = 0; i < numFrames; ++i)
        v[i] = amp * std::sin(2.f * 3.14159265f * freq * static_cast<float>(i) / sr);
    return v;
}

// ─── MIX-1 : classification de contenu (détection par bandes spectrales) ──
TEST_CASE("MIX-1: content classification via band energies", "[mix]") {
    // Kick simulé : impulsion basse fréquence courte (attaque forte, sub dominant)
    {
        std::vector<float> kick(2000, 0.f);
        for (size_t i = 0; i < 300; ++i)
            kick[i] = 0.9f * std::sin(2.f * 3.14159265f * 55.f * static_cast<float>(i) / kSR)
                      * std::exp(-static_cast<float>(i) / 40.f);
        const auto t = detectContentType(kick, kSR);
        CHECK(t == MixContentType::KICK);
    }

    // Pad : sinus 880 Hz maintenu plus d'1,5 s, transitoire faible
    {
        const auto pad = makeSine(880.f, static_cast<int>(kSR * 2.f), kSR, 0.3f);
        const auto t = detectContentType(pad, kSR);
        CHECK(t == MixContentType::PAD);
    }

    // Hi-hat : bruit haute fréquence court et transitoire très fort
    {
        std::vector<float> hat(3000, 0.f);
        unsigned s = 42u;
        for (size_t i = 0; i < 800; ++i)
        {
            s = s * 1103515245u + 12345u;
            const float rnd = (static_cast<float>((s >> 16u) & 0x7FFFu) / 32767.f - 0.5f) * 2.f;
            hat[i] = rnd * std::exp(-static_cast<float>(i % 800) / 60.f);
        }
        const auto t = detectContentType(hat, kSR);
        CHECK(t == MixContentType::HIHAT);
    }

    // Silence → OTHER
    {
        const std::vector<float> silence(1000, 0.f);
        CHECK(detectContentType(silence, kSR) == MixContentType::OTHER);
    }
}

// ─── MIX-2 : centroïde spectral respecte les plages sémantiques ─────────
TEST_CASE("MIX-2: spectral centroid within semantic bands", "[mix]") {
    const auto low = estimateSpectralCentroid(makeSine(55.f, static_cast<int>(kSR * 0.5f), kSR), kSR);
    const auto high = estimateSpectralCentroid(makeSine(6000.f, static_cast<int>(kSR * 0.5f), kSR), kSR);
    CHECK(low < high);
    CHECK(low < 300.f);
    CHECK(high > 5000.f);
    CHECK(estimateSpectralCentroid({}, kSR) == Catch::Approx(1000.f));
}

// ─── MIX-3 : les filtres biquad ne produisent pas de NaN ────────────────
TEST_CASE("MIX-3: biquad filters are stable (no NaN, bounded output)", "[mix]") {
    auto x = makeSine(100.f, static_cast<int>(kSR * 0.2f), kSR, 0.9f);
    const auto before = x;

    std::vector<BiquadCoeffs> cs;
    cs.push_back(makeHP(30.f, kSR));
    cs.push_back(makeLP(8000.f, kSR));
    cs.push_back(makeLowShelf(60.f, 5.f, kSR));
    cs.push_back(makeHighShelf(6000.f, -3.f, kSR));
    cs.push_back(makePeaking(250.f, -2.f, 1.5f, kSR));

    for (const auto& c : cs)
        applyBiquad(x, c);

    float maxAbs = 0.f;
    for (const auto s : x)
    {
        CHECK(std::isfinite(s));
        maxAbs = std::max(maxAbs, std::abs(s));
    }
    CHECK(maxAbs < 6.f);
    CHECK(before != x);  // le filtre a réellement modifié le signal
}

// ─── MIX-4 : true peak >= sample peak et borné ──────────────────────────
TEST_CASE("MIX-4: true peak bounds", "[mix]") {
    const auto x = makeSine(1000.f, static_cast<int>(kSR * 0.1f), kSR, 0.9f);
    float samplePeak = 0.f;
    for (const auto s : x)
        samplePeak = std::max(samplePeak, std::abs(s));
    const float tp = calculateTruePeak(x);
    CHECK(tp >= samplePeak - 1e-4f);
    CHECK(tp <= 1.f + 1e-2f);
    CHECK(calculateTruePeak({}) == 0.f);
}

// ─── MIX-5 : EQ par rôle + démasquage + sub-ownership restent .finite ──
TEST_CASE("MIX-5: role EQ/unmasking/sub-ownership pipelines are stable", "[mix]") {
    MixContentType types[kSlots] = {};
    types[0] = MixContentType::KICK;
    types[1] = MixContentType::BASS;
    types[2] = MixContentType::HIHAT;
    types[3] = MixContentType::SNARE;

    std::vector<float> pcms[kSlots];
    pcms[0] = makeSine(55.f, static_cast<int>(kSR * 0.4f), kSR, 0.6f);
    pcms[1] = makeSine(70.f, static_cast<int>(kSR * 0.8f), kSR, 0.5f);
    pcms[2] = makeSine(8000.f, static_cast<int>(kSR * 0.2f), kSR, 0.4f);
    pcms[3] = makeSine(400.f, static_cast<int>(kSR * 0.3f), kSR, 0.5f);

    for (int i = 0; i < kSlots; ++i)
    {
        if (pcms[i].empty()) continue;
        applyRoleEQ(pcms[i], types[i], kSR);
        applyUnmasking(pcms[i], i, types, kSlots, kSR);
        for (const auto s : pcms[i])
            CHECK(std::isfinite(s));
    }

    applySubOwnership(pcms, types, kSlots, kSR);
    for (int i = 0; i < kSlots; ++i)
        for (const auto s : pcms[i])
            CHECK(std::isfinite(s));

    applyKickTransient(pcms[0], kSR);
    applyBassHarmonics(pcms[1], kSR);
    for (const auto s : pcms[0])
        CHECK(std::isfinite(s));
    for (const auto s : pcms[1])
        CHECK(std::isfinite(s));
}

// ─── MIX-6 : gains cibles et headroom sax ──────────────────────────────
TEST_CASE("MIX-6: target gains and sax clearance", "[mix]") {
    CHECK(targetGainForType(MixContentType::BASS) == Approx(0.70f));
    CHECK(targetGainForType(MixContentType::KICK) == Approx(0.65f));
    CHECK(saxClearance(MixContentType::BASS) == Approx(1.00f));
    CHECK(saxClearance(MixContentType::KICK) == Approx(1.00f));
    CHECK(saxClearance(MixContentType::PAD) == Approx(0.707f).epsilon(1e-3f));
    CHECK(saxClearance(MixContentType::LOOP) == Approx(0.85f));
}

// ─── MIX-7 : spatialisation (mono sub-bass, hat alterné) ───────────────
TEST_CASE("MIX-7: spatialization rules", "[mix]") {
    const auto kick = computeSpatialization(0, MixContentType::KICK, 90.f);
    CHECK(kick.pan == 0.f);
    CHECK(kick.width == 0.f);

    const auto hat0 = computeSpatialization(0, MixContentType::HIHAT, 6000.f);
    const auto hat1 = computeSpatialization(1, MixContentType::HIHAT, 6000.f);
    CHECK(hat0.pan > 0.f);
    CHECK(hat1.pan < 0.f);
    CHECK(hat0.width == Approx(0.3f));

    // Sub-bass mono enforcement
    const auto padSub = computeSpatialization(2, MixContentType::PAD, 150.f);
    CHECK(padSub.pan == 0.f);
    CHECK(padSub.width == 0.f);

    const auto defaultSpatial = spatialForType(0, MixContentType::KICK);
    CHECK(defaultSpatial.pan == 0.f);
}

// ─── MIX-8 : densité de scène → scale de gain adaptatif ────────────────
TEST_CASE("MIX-8: scene density drives gain scale", "[mix]") {
    SceneSnapshot drop;
    drop.slotActive.fill(true);
    drop.activeCount = 8;
    drop.isDrop      = true;
    CHECK(densityScale(drop) == Approx(0.95f));

    SceneSnapshot breakdown;
    breakdown.slotActive.fill(false);
    breakdown.slotActive[0] = true;
    breakdown.slotActive[1] = true;
    breakdown.activeCount = 2;
    breakdown.isBreakdown = true;
    CHECK(densityScale(breakdown) == Approx(1.15f));

    SceneSnapshot buildUp;
    buildUp.slotActive.fill(false);
    for (int i = 0; i < 4; ++i) buildUp.slotActive[i] = true;
    buildUp.activeCount = 4;
    CHECK(densityScale(buildUp) == Approx(1.05f));

    SceneSnapshot sparse;
    sparse.slotActive.fill(false);
    sparse.slotActive[0] = true;
    sparse.activeCount   = 1;
    CHECK(densityScale(sparse) == Approx(1.10f));

    // Présence de bass : BASS actif → compensé ; absent → non
    SceneSnapshot withBass;
    withBass.slotTypes[0] = MixContentType::BASS;
    withBass.slotActive[0] = true;
    CHECK(hasActiveBass(withBass));
    CHECK(!hasActiveBass(sparse));
}

// ─── MIX-9 : gain calibré + duck Serum ─────────────────────────────────
TEST_CASE("MIX-9: calibrated gain and serum duck", "[mix]") {
    // kick, density=1.1, clearance=1, truePeak=0.5 → 0.65*1.1/0.5 = 1.43
    const float gKick = computeTargetGain(MixContentType::KICK, 1.10f, 1.00f, 0.50f);
    CHECK(gKick == Approx(1.43f).epsilon(1e-2f));

    // clamp 1.5 : truePeak très faible
    const float gClamp = computeTargetGain(MixContentType::LOOP, 1.10f, 0.85f, 1e-6f);
    CHECK(gClamp == Approx(1.5f));

    // PAD co-localisé au spectre Serum + RMS fort → duck significatif
    const float ducked = serumDuckGain(1.0f, MixContentType::PAD, 800.f, 1000.f, 0.2f);
    CHECK(ducked < 1.0f);
    CHECK(ducked >= 0.05f);

    // Types « groove » jamais duckés
    const float noDuck = serumDuckGain(1.0f, MixContentType::KICK, 800.f, 1000.f, 0.2f);
    CHECK(noDuck == Approx(1.0f));

    // RMS trop faible → no-op
    const float quiet = serumDuckGain(1.0f, MixContentType::PAD, 800.f, 1000.f, 0.01f);
    CHECK(quiet == Approx(1.0f));

    // Type effectif : override prioritaire
    CHECK(effectiveType(MixContentType::KICK, true, MixContentType::BASS)
          == MixContentType::BASS);
    CHECK(effectiveType(MixContentType::KICK, false, MixContentType::BASS)
          == MixContentType::KICK);
}

// ─── MIX-10 : orchestrateur heuristique (déterminisme, finis, gains) ──
TEST_CASE("MIX-10: heuristic mix engine is deterministic and finite", "[mix]") {
    MixInputs in;
    in.sampleRate = kSR;
    in.masterBpm  = 120.f;
    in.pcm[0] = makeSine(55.f,  static_cast<int>(kSR * 0.4f), kSR, 0.6f);  // kick
    in.pcm[1] = makeSine(70.f,  static_cast<int>(kSR * 0.8f), kSR, 0.5f);  // bass
    in.pcm[2] = makeSine(200.f, static_cast<int>(kSR * 1.6f), kSR, 0.4f);  // pad
    in.active.fill(true);
    in.detected[0] = MixContentType::KICK;
    in.detected[1] = MixContentType::BASS;
    in.detected[2] = MixContentType::PAD;
    in.scene.activeCount = 3;

    const auto a = processHeuristic(in);
    const auto b = processHeuristic(in);

    // Déterminisme bit à bit
    for (int i = 0; i < kMixSlots; ++i)
    {
        CHECK(a.pcm[i].size() == b.pcm[i].size());
        if (!a.pcm[i].empty())
            CHECK(a.pcm[i] == b.pcm[i]);
        CHECK(a.gain[i] == b.gain[i]);
        CHECK(a.pan[i] == b.pan[i]);
        CHECK(a.width[i] == b.width[i]);
    }

    // Finis et bornés
    for (int i = 0; i < kMixSlots; ++i)
    {
        if (!in.active[i]) continue;
        for (const auto s : a.pcm[i])
            CHECK(std::isfinite(s));
        CHECK(a.gain[i] > 0.f);
        CHECK(a.gain[i] <= 1.5f);
    }

    // PAD reçoit l'écho dub (bpm>0) → pcm modifié vs entrée
    CHECK(a.pcm[2].size() == in.pcm[2].size());

    // Slots inactifs non traités
    MixInputs sparse = in;
    sparse.active[0] = true;
    sparse.active[1] = false;
    sparse.active[2] = false;
    const auto s = processHeuristic(sparse);
    CHECK(s.pcm[1].empty());
    CHECK(s.pcm[2].empty());
    CHECK(s.pcm[0].size() == in.pcm[0].size());

    // Kick non ducké même si Serum RMS élevé
    MixInputs serumMix = in;
    serumMix.serumRms = 0.5f;
    serumMix.serumCentroid = 800.f;
    const auto sm = processHeuristic(serumMix);
    CHECK(sm.gain[0] == a.gain[0]);  // KICK jamais ducké
}

// ─── MIX-11 : balance L/R flip les PAD/SYNTH/PERC en excès ──────────────
TEST_CASE("MIX-11: L/R balancing flips non-critical slots", "[mix]") {
    // 5 slots pannés à gauche, 1 à droite → flip les PAD gauches
    std::array<SpatialDecision, kMixSlots> spatials {};
    std::array<bool, kMixSlots> active {};
    std::array<MixContentType, kMixSlots> types {};

    for (int i = 0; i < 5; ++i)
    {
        spatials[i].pan = -0.5f;
        active[i] = true;
        types[i] = (i == 2) ? MixContentType::PAD : MixContentType::HIHAT;
    }
    active[5] = true;
    spatials[5].pan = 0.5f;
    types[5] = MixContentType::PERC;

    balanceLeftRight(spatials, active, types);

    // Le PAD (slot 2) a été flippé à droite
    CHECK(spatials[2].pan == Approx(0.5f));
    // La balance globale est revenue proche de 0
    int left = 0, right = 0;
    for (int i = 0; i < kMixSlots; ++i)
    {
        if (!active[i]) continue;
        if (spatials[i].pan < 0.f) ++left;
        if (spatials[i].pan > 0.f) ++right;
    }
    CHECK(left >= 2);
    CHECK(right >= 2);
}

// ─── MIX-12 : capture de l'état persistant depuis MixOutputs ─────────────
TEST_CASE("MIX-12: persistent mix state captured from MixOutputs", "[mix]") {
    MixOutputs out;
    out.pcm[0].assign(100, 0.f);   // slot 0 traité (PCM non vide)
    out.pcm[2].assign(200, 0.f);   // slot 2 traité
    out.gain[0]  = 0.8f;
    out.pan[0]   = -0.3f;
    out.width[0] = 0.4f;
    out.depth[0] = 0.2f;
    out.gain[2]  = 1.2f;
    out.pan[2]   = 0.5f;
    out.width[2] = 0.6f;
    out.depth[2] = 0.9f;

    const auto st = mixStateFromOutputs(out);

    // Slot traité → applied, valeurs copiées
    CHECK(st[0].applied);
    CHECK(st[0].gain  == Approx(0.8f));
    CHECK(st[0].pan   == Approx(-0.3f));
    CHECK(st[0].width == Approx(0.4f));
    CHECK(st[0].depth == Approx(0.2f));
    CHECK(st[2].applied);
    CHECK(st[2].gain  == Approx(1.2f));
    CHECK(st[2].depth == Approx(0.9f));

    // Slot non traité (PCM vide) → défaut non appliqué
    CHECK_FALSE(st[1].applied);
    CHECK(st[1].gain  == Approx(1.f));
    CHECK(st[1].pan   == Approx(0.f));
    CHECK(st[1].width == Approx(0.f));
    CHECK(st[1].depth == Approx(0.f));
}

// ─── MIX-13 : round-trip set → get + bornes ───────────────────────────────
TEST_CASE("MIX-13: persistent mix state set/get round-trip", "[mix]") {
    MixStateArray st;

    setSlotMixState(st, 3, 0.9f, 0.1f, 0.2f, 0.3f);
    const auto s3 = slotMixState(st, 3);
    CHECK(s3.applied);
    CHECK(s3.gain  == Approx(0.9f));
    CHECK(s3.pan   == Approx(0.1f));
    CHECK(s3.width == Approx(0.2f));
    CHECK(s3.depth == Approx(0.3f));

    // Réécriture d'un slot déjà persisté
    setSlotMixState(st, 3, 0.5f, -0.1f, 0.0f, 0.7f);
    CHECK(slotMixState(st, 3).gain  == Approx(0.5f));
    CHECK(slotMixState(st, 3).depth == Approx(0.7f));

    // Autres slots restent aux défauts
    CHECK_FALSE(slotMixState(st, 0).applied);
    CHECK(slotMixState(st, 8).gain == Approx(1.f));

    // Hors bornes : no-op (lecture) et écriture ignorée
    CHECK_FALSE(slotMixState(st, -1).applied);
    CHECK_FALSE(slotMixState(st, 9).applied);
    CHECK_FALSE(slotMixState(st, 100).applied);
    setSlotMixState(st, -1, 0.1f, 0.f, 0.f, 0.f);
    setSlotMixState(st, 99, 0.1f, 0.f, 0.f, 0.f);
    CHECK(slotMixState(st, 0).gain == Approx(1.f));
}

// ─── MIX-14 : cohérence état persistant ↔ décisions du mix heuristique ──
TEST_CASE("MIX-14: captured state matches heuristic mix outputs", "[mix]") {
    MixInputs in;
    in.sampleRate = kSR;
    in.masterBpm  = 120.f;
    in.pcm[0] = makeSine(55.f,  static_cast<int>(kSR * 0.4f), kSR, 0.6f);  // kick
    in.pcm[1] = makeSine(70.f,  static_cast<int>(kSR * 0.8f), kSR, 0.5f);  // bass
    in.pcm[2] = makeSine(200.f, static_cast<int>(kSR * 1.6f), kSR, 0.4f);  // pad
    in.active.fill(true);
    in.detected[0] = MixContentType::KICK;
    in.detected[1] = MixContentType::BASS;
    in.detected[2] = MixContentType::PAD;
    in.scene.activeCount = 3;

    const auto out = processHeuristic(in);
    const auto st  = mixStateFromOutputs(out);

    for (int i = 0; i < kMixSlots; ++i)
    {
        if (in.active[i] && !in.pcm[i].empty())
        {
            CHECK(st[i].applied);
            CHECK(st[i].gain  == out.gain[i]);
            CHECK(st[i].pan   == out.pan[i]);
            CHECK(st[i].width == out.width[i]);
            CHECK(st[i].depth == out.depth[i]);
        }
        else
        {
            CHECK_FALSE(st[i].applied);
        }
    }
}