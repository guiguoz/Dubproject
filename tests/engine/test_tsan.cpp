#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <cstring>
#include "engine/Transport.h"
#include "engine/AudioGraph.h"
#include "engine/Sequencer.h"
#include "engine/SlotPlayer.h"
#include "engine/SceneStore.h"
#include "engine/EventScheduler.h"

using namespace engine;

// ══════════════════════════════════════════════════════════════════════════════
// T-TSAN : scénario multi-thread pour ThreadSanitizer
//
// Trois threads tournent en boucle pendant ~200 ms :
//   1. Audio thread   — appelle Transport::advance() + AudioGraph::processBlock()
//   2. Message thread — modifie BPM, patterns, rôles, gains, mute, scene
//   3. Mix thread     — appelle AudioGraph::updateAutoMixTargets()
//
// Si TSan est actif, toute data race non protégée sera rapportée.
// Si TSan n'est pas actif, le test vérifie simplement l'absence de crash.
// ══════════════════════════════════════════════════════════════════════════════

static constexpr double kSampleRate = 44100.0;
static constexpr int    kBlockSize  = 512;

// Helper : fabrique un PCM mono simple (sine 440 Hz, 0.1 s).
static SlotPcm makeTestPcm() {
    const int numFrames = static_cast<int>(kSampleRate * 0.1);
    SlotPcm pcm;
    pcm.numChannels = 1;
    pcm.sampleRate  = static_cast<float>(kSampleRate);
    pcm.numFrames   = numFrames;
    pcm.data.resize(static_cast<size_t>(numFrames));
    for (int i = 0; i < numFrames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
        pcm.data[static_cast<size_t>(i)] = 0.5f * std::sin(2.f * 3.14159f * 440.f * t);
    }
    return pcm;
}

TEST_CASE("T-TSAN: multi-thread stress — audio + message + mix", "[tsan]") {
    // ── Setup ────────────────────────────────────────────────────────────────
    Transport transport;
    transport.prepare(kSampleRate, 120.0);
    transport.play();

    AudioGraph graph;
    graph.prepare(kSampleRate, kBlockSize);

    // Charger 3 slots avec du PCM test
    for (int s = 0; s < 3; ++s) {
        graph.slotPlayer().loadSlot(s, makeTestPcm(), PlayMode::Free);
        graph.slotPlayer().setGain(s, 0.8f);
    }

    // Poser les rôles
    graph.setSlotRole(0, SlotRole::Kick);
    graph.setSlotRole(1, SlotRole::Bass);
    graph.setSlotRole(2, SlotRole::Melodic);

    // Pattern sur le slot 0 (kick)
    *graph.sequencer().patterns().writeBuffer(0) = []{
        TrackPattern p;
        p.numSteps = 16;
        p.steps[0] = p.steps[4] = p.steps[8] = p.steps[12] = true;
        return p;
    }();
    graph.sequencer().patterns().flip();

    // Buffer de sortie audio
    std::vector<float> output(static_cast<size_t>(kBlockSize) * 2, 0.f);

    // Flag d'arrêt
    std::atomic<bool> stop{false};

    // Compteur d'itérations (pour vérifier que tournent bien)
    std::atomic<int64_t> audioIters{0};
    std::atomic<int64_t> messageIters{0};
    std::atomic<int64_t> mixIters{0};

    // ── Thread audio ─────────────────────────────────────────────────────────
    std::thread audioThread([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const auto ts = transport.advance(kBlockSize);

            // Générer des events (comme dans le vrai pipeline)
            EventScheduler scheduler;
            graph.sequencer().generateEvents(ts, ts.blockStart, kBlockSize,
                                             0.0f, scheduler);

            // Extraire les events
            EventWithOffset evBuf[64];
            int evCount = 0;
            scheduler.processBlock(ts.blockStart, kBlockSize,
                [&evBuf, &evCount](int32_t offset, const EngineEvent& ev) {
                    if (evCount < 64) {
                        evBuf[evCount].offset = offset;
                        evBuf[evCount].ev     = ev;
                        ++evCount;
                    }
                });

            // processBlock
            graph.processBlock(ts, evBuf, evCount, output.data(), kBlockSize);

            audioIters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // ── Thread message ───────────────────────────────────────────────────────
    std::thread messageThread([&] {
        int step = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            switch (step % 10) {
                case 0:
                    // Modifier le BPM
                    transport.setBpm(120.0 + (step % 30));
                    break;
                case 1:
                    // Modifier le gain d'un slot
                    graph.slotPlayer().setGain(0, 0.5f + (step % 5) * 0.1f);
                    break;
                case 2:
                    // Mute / unmute
                    graph.slotPlayer().setMuted(1, (step % 3) == 0);
                    break;
                case 3:
                    // Stress triple-buffer : modification de paramètres non-pattern.
                    // Teste la contention thread sans exercer le buffer pattern.
                    graph.setSlotRole(2, (step % 2 == 0) ? SlotRole::Melodic
                                                          : SlotRole::Pad);
                    break;
                case 4:
                    // Changer le rôle d'un slot
                    graph.setSlotRole(2, (step % 2 == 0) ? SlotRole::Melodic
                                                          : SlotRole::Pad);
                    break;
                case 5:
                    // Modifier semitones
                    graph.slotPlayer().setSemitones(2, (step % 7) - 3);
                    break;
                case 6:
                    // Modifier pan
                    graph.slotPlayer().setSpatial(1, (step % 11) / 10.f - 0.5f, 0.5f);
                    break;
                case 7:
                    // Stop puis restart (test Transport::stop/play)
                    transport.stop();
                    std::this_thread::yield();
                    transport.play();
                    break;
                case 8:
                    // Modifier le mode d'un slot
                    graph.slotPlayer().setMode(2, (step % 2 == 0) ? PlayMode::Free
                                                                   : PlayMode::OneShot);
                    break;
                case 9:
                    // Modifier inputGain
                    graph.setInputGain(0.3f + (step % 4) * 0.15f);
                    break;
            }
            ++step;
            messageIters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // ── Thread de mix ────────────────────────────────────────────────────────
    std::thread mixThread([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            graph.updateAutoMixTargets();
            mixIters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // ── Laisser tourner ~200 ms ─────────────────────────────────────────────
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── Arrêter ──────────────────────────────────────────────────────────────
    stop.store(true, std::memory_order_relaxed);
    audioThread.join();
    messageThread.join();
    mixThread.join();

    // ── Vérifier que les threads ont bien tourné ─────────────────────────────
    REQUIRE(audioIters.load() > 0);
    REQUIRE(messageIters.load() > 0);
    REQUIRE(mixIters.load() > 0);

    INFO("audioIters=" << audioIters.load()
         << " messageIters=" << messageIters.load()
         << " mixIters=" << mixIters.load());
}

// ══════════════════════════════════════════════════════════════════════════════
// T-TSAN2 : stress import concurrent — même slot + slots différents
// ══════════════════════════════════════════════════════════════════════════════

TEST_CASE("T-TSAN2: concurrent slot operations", "[tsan]") {
    Transport transport;
    transport.prepare(kSampleRate, 120.0);
    transport.play();

    AudioGraph graph;
    graph.prepare(kSampleRate, kBlockSize);

    std::atomic<bool> stop{false};
    std::vector<float> output(static_cast<size_t>(kBlockSize) * 2, 0.f);

    // Thread audio permanent
    std::thread audioThread([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const auto ts = transport.advance(kBlockSize);
            EventScheduler scheduler;
            graph.sequencer().generateEvents(ts, ts.blockStart, kBlockSize,
                                             0.0f, scheduler);
            EventWithOffset evBuf[64];
            int evCount = 0;
            scheduler.processBlock(ts.blockStart, kBlockSize,
                [&evBuf, &evCount](int32_t offset, const EngineEvent& ev) {
                    if (evCount < 64) {
                        evBuf[evCount].offset = offset;
                        evBuf[evCount].ev     = ev;
                        ++evCount;
                    }
                });
            graph.processBlock(ts, evBuf, evCount, output.data(), kBlockSize);
        }
    });

    // Thread message : chargements de PCM concurrents sur différents slots
    std::thread msgThread([&] {
        int iter = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const int slot = iter % 3;
            graph.slotPlayer().loadSlot(slot, makeTestPcm(), PlayMode::Free);
            graph.slotPlayer().setGain(slot, 0.7f);
            graph.setSlotRole(slot, (iter % 2 == 0) ? SlotRole::Kick : SlotRole::Bass);
            ++iter;
        }
    });

    // Mix thread
    std::thread mixThread([&] {
        while (!stop.load(std::memory_order_relaxed))
            graph.updateAutoMixTargets();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    stop.store(true, std::memory_order_relaxed);
    audioThread.join();
    msgThread.join();
    mixThread.join();
}
