#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include "engine/AudioGraph.h"

using namespace engine;

// ─── T-C2 : fumée AudioGraph — prepare + processBlock sans crash ───────────
// Vérifie que le graphe audio peut être préparé, qu'un slot peut être chargé,
// et que plusieurs processBlock tournent sans crash ni NaN.
TEST_CASE("T-C2: AudioGraph smoke — prepare, load, processBlock, destroy", "[audiograph]") {
    AudioGraph graph;
    constexpr double sr = 44100.0;
    constexpr int blockSz = 512;

    graph.prepare(sr, blockSz);

    // Charger un slot mono avec un signal constant
    SlotPcm pcm;
    pcm.numChannels = 1;
    pcm.numFrames   = 1024;
    pcm.sampleRate  = static_cast<float>(sr);
    pcm.data.assign(1024, 0.5f);
    graph.slotPlayer().loadSlot(0, std::move(pcm), PlayMode::OneShot);

    // Transport
    TransportState ts{};
    ts.sampleRate      = sr;
    ts.samplesPerBeat  = sr * 60.0 / 120.0;
    ts.samplesPerStep  = ts.samplesPerBeat / 4.0;
    ts.blockStart      = 0;
    ts.samplePos       = 0;
    ts.playing         = true;
    ts.bpm             = 120.0;

    // Déclencher le slot
    EngineEvent trig{};
    trig.time = kNextBlock;
    trig.type = EventType::Trigger;
    trig.slot = 0;

    EventWithOffset evwo{0, trig};

    // Process 10 blocs sans crash
    std::vector<float> out(static_cast<size_t>(blockSz) * 2, 0.f);
    for (int i = 0; i < 10; ++i) {
        ts.blockStart = static_cast<int64_t>(i) * blockSz;
        ts.samplePos  = ts.blockStart + blockSz;
        std::fill(out.begin(), out.end(), 0.f);

        if (i == 0)
            graph.processBlock(ts, &evwo, 1, out.data(), blockSz);
        else
            graph.processBlock(ts, nullptr, 0, out.data(), blockSz);

        // Vérifier pas de NaN
        for (size_t j = 0; j < out.size(); ++j)
            REQUIRE_FALSE(std::isnan(out[j]));
    }
}

// ─── T-MX9 : câblage AutoMix au niveau graphe ───────────────────────────────
// Vérifie que updateFeatures() et advanceGainRamp() sont bien appelés par
// processBlock() : sans eux, features.rms resterait 0 et currentGainLinear resterait 1.
TEST_CASE("T-MX9: AutoMix rule 1 wired in AudioGraph — features accumulate, gain applied",
          "[audiograph][automix]") {
    AudioGraph graph;
    constexpr double sr = 44100.0;
    constexpr int blockSz = 512;
    graph.prepare(sr, blockSz);

    // Slot 0 = kick (0,5), slot 1 = bass (0,05) en mode Free
    auto makePcm = [&](float amp) {
        SlotPcm pcm;
        pcm.numChannels = 1;
        pcm.numFrames   = static_cast<int>(sr * 2.0); // 2 s
        pcm.sampleRate  = static_cast<float>(sr);
        pcm.data.assign(static_cast<size_t>(pcm.numFrames), amp);
        return pcm;
    };
    graph.slotPlayer().loadSlot(0, makePcm(0.5f),  PlayMode::Free);
    graph.slotPlayer().loadSlot(1, makePcm(0.05f), PlayMode::Free);
    graph.setSlotRole(0, SlotRole::Kick);
    graph.setSlotRole(1, SlotRole::Bass);

    TransportState ts{};
    ts.sampleRate     = sr;
    ts.samplesPerBeat = sr * 60.0 / 120.0;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;
    ts.playing        = true;
    ts.bpm            = 120.0;

    // Déclencher les deux slots au bloc 0
    EngineEvent trig0{}, trig1{};
    trig0.time = kNextBlock; trig0.type = EventType::Trigger; trig0.slot = 0;
    trig1.time = kNextBlock; trig1.type = EventType::Trigger; trig1.slot = 1;
    EventWithOffset evs[2] = {{0, trig0}, {0, trig1}};

    std::vector<float> out(static_cast<size_t>(blockSz) * 2, 0.f);
    const int64_t mixInterval = static_cast<int64_t>(sr * 0.05); // 50 ms
    int64_t nextMixAt = 0;

    // 120 blocs ≈ 1,4 s : fenêtre RMS de 400 ms bien dépassée
    for (int i = 0; i < 120; ++i) {
        ts.blockStart = static_cast<int64_t>(i) * blockSz;
        ts.samplePos  = ts.blockStart + blockSz;
        if (ts.blockStart >= nextMixAt) {
            graph.updateAutoMixTargets();
            nextMixAt = ts.blockStart + mixInterval;
        }
        std::fill(out.begin(), out.end(), 0.f);
        if (i == 0)
            graph.processBlock(ts, evs, 2, out.data(), blockSz);
        else
            graph.processBlock(ts, nullptr, 0, out.data(), blockSz);
    }

    // La mesure tourne : features.rms non nul pour les deux slots actifs
    REQUIRE(graph.autoMix().features(0).rms > 0.f);
    REQUIRE(graph.autoMix().features(1).rms > 0.f);

    // L'application tourne : la bass a reçu une correction positive (kick est ref à 0 dB)
    REQUIRE(graph.autoMix().currentGainLinear(1) > 1.5f);
    // Le kick reste proche de 1 (il est sa propre référence)
    REQUIRE(graph.autoMix().currentGainLinear(0) == Catch::Approx(1.f).margin(0.1f));
}

// ─── T-C2b : AudioGraph avec 9 slots chargés ────────────────────────────────
TEST_CASE("T-C2b: AudioGraph with all 9 slots loaded", "[audiograph]") {
    AudioGraph graph;
    constexpr double sr = 48000.0;
    constexpr int blockSz = 256;

    graph.prepare(sr, blockSz);

    for (int s = 0; s < 9; ++s) {
        SlotPcm pcm;
        pcm.numChannels = 1;
        pcm.numFrames   = 2048;
        pcm.sampleRate  = static_cast<float>(sr);
        pcm.data.assign(2048, 0.1f * static_cast<float>(s + 1));
        graph.slotPlayer().loadSlot(s, std::move(pcm), PlayMode::Free);
    }

    TransportState ts{};
    ts.sampleRate      = sr;
    ts.samplesPerBeat  = sr * 60.0 / 120.0;
    ts.samplesPerStep  = ts.samplesPerBeat / 4.0;
    ts.blockStart      = 0;
    ts.samplePos       = blockSz;
    ts.playing         = true;
    ts.bpm             = 120.0;

    std::vector<float> out(static_cast<size_t>(blockSz) * 2, 0.f);
    for (int i = 0; i < 5; ++i) {
        ts.blockStart = static_cast<int64_t>(i) * blockSz;
        ts.samplePos  = ts.blockStart + blockSz;
        std::fill(out.begin(), out.end(), 0.f);
        graph.processBlock(ts, nullptr, 0, out.data(), blockSz);
        for (size_t j = 0; j < out.size(); ++j)
            REQUIRE_FALSE(std::isnan(out[j]));
    }
}
