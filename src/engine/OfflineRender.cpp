#include "engine/OfflineRender.h"
#include "engine/AudioGraph.h"
#include "engine/TransitionEngine.h"
#include "engine/EventScheduler.h"
#include <algorithm>
#include <cstring>

namespace engine {

std::vector<float> renderOffline(const OfflineSession& session, int64_t numSamples)
{
    std::vector<float> out(static_cast<size_t>(numSamples) * 2u, 0.f);
    if (numSamples <= 0) return out;

    const double sr = (session.sampleRate > 0.0) ? session.sampleRate : 44100.0;
    const int startScene = std::clamp(session.startScene, 0, kMaxScenes - 1);

    // ── Transport + graphe (état frais à chaque appel → déterministe) ────────
    Transport transport;
    transport.prepare(sr, session.bpm);

    AudioGraph graph;
    // Rôles posés AVANT prepare() : findKickSlot() est appelé dans prepare().
    for (int s = 0; s < kMaxSlots; ++s)
        graph.setSlotRole(s, session.scenes[startScene].slots[s].role);
    graph.prepare(sr, session.maxBlockSize);

    // ── Patterns → séquenceur ─────────────────────────────────────────────────
    for (int s = 0; s < kMaxSlots; ++s)
        *graph.sequencer().patterns().writeBuffer(s) = session.patterns[s];
    graph.sequencer().patterns().flip();

    // ── PCM + config par slot (tous les PCM fournis sont chargés) ─────────────
    SlotPlayer& sp = graph.slotPlayer();
    for (int s = 0; s < kMaxSlots; ++s) {
        const OfflinePcm& src = session.pcm[s];
        if (src.data.empty() || src.numChannels <= 0) continue;

        SlotPcm pcm;
        pcm.numChannels = src.numChannels;
        pcm.sampleRate  = (src.sampleRate > 0.f) ? src.sampleRate
                                                 : static_cast<float>(sr);
        pcm.numFrames   = static_cast<int>(src.data.size()
                                           / static_cast<size_t>(src.numChannels));
        pcm.data        = src.data;

        const SlotConfig& cfg = session.scenes[startScene].slots[s];
        sp.loadSlot(s, std::move(pcm), cfg.mode);
        sp.setGain(s, cfg.gain);
        sp.setSemitones(s, cfg.semitones);
        if (session.loopSync[s].enabled) {
            sp.setMode(s, PlayMode::LoopSync);
            sp.armLoopSync(s, session.loopSync[s].loopBeats,
                           session.loopSync[s].timeRatio, cfg.semitones,
                           /*anchor=*/0);
        }
    }

    // ── Scènes + transitions ──────────────────────────────────────────────────
    SceneStore store;
    const int nScenes = std::clamp(session.numScenes, 1, kMaxScenes);
    for (int i = 0; i < nScenes; ++i)
        store.setScene(i, session.scenes[i]);

    TransitionEngine transitions;
    EventScheduler    scheduler;

    const int blockSize = std::max(1, session.maxBlockSize);
    std::vector<float> interleaved(static_cast<size_t>(blockSize) * 2u, 0.f);

    // Thread de mix simulé : computeTargets() toutes les 50 ms de temps rendu.
    const int64_t mixInterval = static_cast<int64_t>(sr * 0.05);
    int64_t nextMixAt = 0;

    int currentScene = startScene;
    size_t trIdx = 0;

    transport.play();

    int64_t processed = 0;
    while (processed < numSamples) {
        const int64_t remain = numSamples - processed;
        const int n = static_cast<int>(std::min<int64_t>(blockSize, remain));
        const auto ts = transport.advance(n);

        // ── Transitions demandées dans ce bloc ────────────────────────────────
        // Le diff est calculé au moment de la requête (comportement EngineFacade).
        while (trIdx < session.transitions.size()) {
            const OfflineSession::Transition& tr = session.transitions[trIdx];
            if (tr.atSample < ts.blockStart) { ++trIdx; continue; }
            if (tr.atSample >= ts.samplePos) break;   // plus tard — bloc suivant
            transitions.requestTransition(currentScene, tr.toScene, store, ts);
            currentScene = tr.toScene;
            ++trIdx;
        }

        // ── TransitionEngine + séquenceur ─────────────────────────────────────
        transitions.processBlock(ts, scheduler);
        graph.sequencer().generateEvents(ts, ts.blockStart, n, 0.f, scheduler);

        // ── Thread de mix simulé (50 ms) ──────────────────────────────────────
        if (ts.blockStart >= nextMixAt) {
            graph.updateAutoMixTargets();
            nextMixAt = ts.blockStart + mixInterval;
        }

        // ── Events → graphe (dispatch temporel) ──────────────────────────────
        EventWithOffset evBuf[256];
        int evCount = 0;
        scheduler.processBlock(ts.blockStart, n,
            [&evBuf, &evCount](int32_t offset, const EngineEvent& ev) {
                if (evCount < 256) {
                    evBuf[evCount].offset = offset;
                    evBuf[evCount].ev     = ev;
                    ++evCount;
                }
            });

        graph.processBlock(ts, evBuf, evCount, interleaved.data(), n);
        scheduler.clear();

        std::memcpy(out.data() + processed * 2, interleaved.data(),
                    static_cast<size_t>(n) * 2u * sizeof(float));
        processed += n;
    }

    return out;
}

} // namespace engine
