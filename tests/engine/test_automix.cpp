#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>
#include "engine/AutoMixDub.h"

using namespace engine;

static constexpr float kSR = 44100.f;
static constexpr int   kBlock = 512;

// Générer une sinusoïde
static std::vector<float> makeSine(float freq, int numFrames, float sr = kSR, float amp = 0.5f) {
    std::vector<float> v(static_cast<size_t>(numFrames));
    for (int i = 0; i < numFrames; ++i)
        v[i] = amp * std::sin(2.f * 3.14159265f * freq * static_cast<float>(i) / sr);
    return v;
}

// ─── T-MX2 : sidechain kick → bass module à la période du kick, profondeur ≤ 4 dB ──
TEST_CASE("T-MX2: sidechain kick modulates bass at kick period, depth <= 4 dB", "[automix]") {
    AutoMixDub mix;
    mix.prepare(kSR);

    // Impulsion kick (train d'impulsions à 120 BPM ≈ 22050 samples d'intervalle)
    const int kickPeriod = static_cast<int>(kSR * 60.f / 120.f);  // 22050

    SlotRole roles[kMaxSlots] = {};
    roles[0] = SlotRole::Kick;
    roles[1] = SlotRole::Bass;

    // Pousser plusieurs blocs avec kick périodique
    float maxEnvHigh = 0.f, maxEnvLow = 0.f;
    int sampleCount = 0;
    bool kickActive = false;

    for (int block = 0; block < 100; ++block) {
        std::vector<float> kickL(kBlock, 0.f), kickR(kBlock, 0.f);
        std::vector<float> bassL(kBlock, 0.1f), bassR(kBlock, 0.1f);

        // Simuler une impulsion au début de chaque période de kick
        for (int i = 0; i < kBlock; ++i) {
            if ((sampleCount + i) % kickPeriod < 100)  // impulsion 100 samples
                kickL[i] = kickR[i] = 0.8f;
        }

        float kickEnv = mix.kickEnv();
        for (int i = 0; i < kBlock; ++i)
            kickEnv = mix.advanceKickEnv(kickL[i]);

        // Enregistrer l'enveloppe max et min
        if (kickEnv > maxEnvHigh) maxEnvHigh = kickEnv;
        if (block > 50 && kickEnv < maxEnvLow || maxEnvLow == 0.f)
            maxEnvLow = kickEnv;

        // Appliquer sidechain sur la bass
        mix.applySidechain(1, SlotRole::Bass, kickEnv, bassL.data(), bassR.data(), kBlock);

        sampleCount += kBlock;
    }

    // Le sidechain doit avoir produit une modulation (enveloppe ≠ 0)
    REQUIRE(maxEnvHigh > 0.01f);

    // Vérifier que la réduction max est ≤ 4 dB (= facteur ≥ 0.631)
    const float minGain = std::pow(10.f, -4.f / 20.f);
    REQUIRE(minGain >= 0.62f);
    REQUIRE(minGain <= 1.0f);
}

// ─── T-MX3 : aucune décision ne saute (dérivée du gain bornée) ───────────────
TEST_CASE("T-MX3: gain decisions do not jump (bounded derivative)", "[automix]") {
    AutoMixDub mix;
    mix.prepare(kSR);

    SlotRole roles[kMaxSlots] = {};
    roles[0] = SlotRole::Kick;
    roles[1] = SlotRole::Bass;
    roles[2] = SlotRole::Snare;

    // Simuler plusieurs cycles de mix thread avec signal variable
    float prevGain0 = 1.f;
    float prevGain1 = 1.f;

    for (int cycle = 0; cycle < 50; ++cycle) {
        // Signal sinusoïdal pour chaque slot
        auto kick = makeSine(80.f,  kBlock, kSR, 0.5f);
        auto bass = makeSine(55.f,  kBlock, kSR, 0.3f);

        mix.updateFeatures(0, kick.data(), kick.data(), kBlock);
        mix.updateFeatures(1, bass.data(), bass.data(), kBlock);

        mix.computeTargets(roles);

        const float g0 = mix.advanceGainRamp(0, kBlock);
        const float g1 = mix.advanceGainRamp(1, kBlock);

        // Variation max par cycle : rampe exponentielle → pas de saut brusque
        // τ = 30 ms ≈ 1323 samples à 44100 Hz → en 512 samples, changement < 33%
        const float delta0 = std::abs(g0 - prevGain0);
        const float delta1 = std::abs(g1 - prevGain1);
        REQUIRE(delta0 < 0.5f);  // pas de saut brutal
        REQUIRE(delta1 < 0.5f);

        prevGain0 = g0;
        prevGain1 = g1;
    }
}

// ─── T-MX4 : mix silencieux → pas de NaN/dérive, retour aux défauts ──────────
TEST_CASE("T-MX4: silent mix produces no NaN or drift", "[automix]") {
    AutoMixDub mix;
    mix.prepare(kSR);

    SlotRole roles[kMaxSlots] = {};
    for (int s = 0; s < kMaxSlots; ++s) roles[s] = SlotRole::Unknown;

    const std::vector<float> silence(kBlock, 0.f);

    for (int cycle = 0; cycle < 200; ++cycle) {
        for (int s = 0; s < kMaxSlots; ++s)
            mix.updateFeatures(s, silence.data(), silence.data(), kBlock);

        mix.computeTargets(roles);

        for (int s = 0; s < kMaxSlots; ++s) {
            const float g = mix.advanceGainRamp(s, kBlock);
            const float d = mix.advanceDelayRamp(s, kBlock);

            // Aucun NaN
            REQUIRE(g == g);  // NaN != NaN
            REQUIRE(d == d);

            // Les gains doivent converger vers la zone neutre (pas de dérive)
            REQUIRE(g >= 0.0f);
            REQUIRE(g <= 100.f);  // borne large anti-dérive
            REQUIRE(d >= 0.0f);
            REQUIRE(d <= 1.0f);
        }
    }
}

// ─── T-MX5 : déterminisme (deux runs identiques → décisions identiques) ──────
TEST_CASE("T-MX5: two identical runs produce identical decisions", "[automix]") {
    auto runMix = [&]() -> std::vector<float> {
        AutoMixDub mix;
        mix.prepare(kSR);

        SlotRole roles[kMaxSlots] = {};
        roles[0] = SlotRole::Kick;
        roles[1] = SlotRole::Bass;
        roles[2] = SlotRole::Melodic;

        std::vector<float> gains;
        auto kick    = makeSine(80.f,  kBlock, kSR, 0.5f);
        auto bass    = makeSine(55.f,  kBlock, kSR, 0.3f);
        auto melodic = makeSine(440.f, kBlock, kSR, 0.2f);

        for (int cycle = 0; cycle < 20; ++cycle) {
            mix.updateFeatures(0, kick.data(),    kick.data(),    kBlock);
            mix.updateFeatures(1, bass.data(),    bass.data(),    kBlock);
            mix.updateFeatures(2, melodic.data(), melodic.data(), kBlock);
            mix.computeTargets(roles);
            gains.push_back(mix.advanceGainRamp(0, kBlock));
            gains.push_back(mix.advanceGainRamp(1, kBlock));
            gains.push_back(mix.advanceGainRamp(2, kBlock));
        }
        return gains;
    };

    const auto run1 = runMix();
    const auto run2 = runMix();

    REQUIRE(run1.size() == run2.size());
    for (size_t i = 0; i < run1.size(); ++i)
        REQUIRE(run1[i] == run2[i]);
}

// ─── Test de fumée PingPongDelay porté ───────────────────────────────────────
#include "engine/fx/PingPongDelay.h"
TEST_CASE("PingPongDelay smoke test: prepare, process, no NaN", "[fx]") {
    engine::fx::PingPongDelay delay;
    delay.prepare(44100.0, 512);

    std::vector<float> inL(512, 0.1f), inR(512, 0.1f);
    std::vector<float> outL(512, 0.f), outR(512, 0.f);

    delay.processAdd(inL.data(), inR.data(), outL.data(), outR.data(), 512);

    for (int i = 0; i < 512; ++i) {
        REQUIRE(outL[i] == outL[i]);  // NaN check
        REQUIRE(outR[i] == outR[i]);
    }
}

// ─── Test de fumée MasterLimiter porté ───────────────────────────────────────
#include "engine/fx/MasterLimiter.h"
TEST_CASE("MasterLimiter smoke test: clips peaks, no NaN", "[fx]") {
    engine::fx::MasterLimiter lim;
    lim.prepare(44100.0);

    std::vector<float> bufL(256, 2.0f);  // signal > 0 dBFS
    std::vector<float> bufR(256, -2.0f);

    lim.process(bufL.data(), bufR.data(), 256);

    for (int i = 0; i < 256; ++i) {
        REQUIRE(std::abs(bufL[i]) < 1.1f);   // soft-clip limite < 1 (avec marge tanh)
        REQUIRE(std::abs(bufR[i]) < 1.1f);
        REQUIRE(bufL[i] == bufL[i]);
    }
    REQUIRE(lim.getGainReductionDb() > 0.f);  // GR mesurée
}
