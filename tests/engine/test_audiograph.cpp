#include <catch2/catch_test_macros.hpp>
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
