#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "engine/SlotPlayer.h"
#include "engine/Sequencer.h"
#include "engine/Transport.h"

using namespace engine;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static TransportState makeSyncTS(double sr, double bpm, int64_t blockStart, int32_t blockLen) {
    TransportState ts{};
    ts.sampleRate     = sr;
    ts.bpm            = bpm;
    ts.samplesPerBeat = sr * 60.0 / bpm;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;
    ts.blockStart     = blockStart;
    ts.samplePos      = blockStart + blockLen;
    ts.playing        = true;
    return ts;
}

// PCM impulsionnel : frame 1 = 1.0, frame 0 = 0.0 (pas de micro-fade au
// trigger : |data[0]| <= kFadeThreshold). SR == device SR (pas d'interp).
static SlotPcm makeImpulsePcm(int numFrames, float sr) {
    SlotPcm pcm;
    pcm.numChannels = 1;
    pcm.numFrames   = numFrames;
    pcm.sampleRate  = sr;
    pcm.data.assign(static_cast<size_t>(numFrames), 0.f);
    if (numFrames > 1) pcm.data[1] = 1.0f;
    return pcm;
}

static EngineEvent trigEv(uint8_t slot) {
    EngineEvent ev{};
    ev.time = 0;
    ev.type = EventType::Trigger;
    ev.slot = slot;
    return ev;
}

static void setupLoopSyncImpulse(SlotPlayer& sp, int pcmFrames, float sr, int loopBeats) {
    sp.prepareStretchers(1, sr);
    sp.setSpatial(0, 0.f, 0.f);
    sp.loadSlot(0, makeImpulsePcm(pcmFrames, sr), PlayMode::LoopSync);
    sp.armLoopSync(0, loopBeats, 1.0f, 0.0f, 0);
}

// Rend [0, total) en UN seul bloc, triggers aux offsets absolus donnés.
static std::vector<float> renderSingle(SlotPlayer& sp, double sr, double bpm,
                                       int total, const std::vector<int32_t>& offsets,
                                       const std::vector<EventType>& types = {}) {
    auto ts = makeSyncTS(sr, bpm, 0, total);
    std::vector<EventWithOffset> evs;
    for (size_t i = 0; i < offsets.size(); ++i) {
        const EventType t = (i < types.size()) ? types[i] : EventType::Trigger;
        evs.push_back({ offsets[i], trigEv(0) });
        evs.back().ev.type = t;
    }
    std::vector<float> out(static_cast<size_t>(total) * 2u, 0.f);
    sp.processBlock(ts, out.data(), total, evs.data(), static_cast<int>(evs.size()));
    return out;
}

// Rend [0, total) découpé physiquement aux cuts, mêmes triggers absolus
// (offset relatif par segment). Référence : découpage == vérité terrain.
static std::vector<float> renderSliced(SlotPlayer& sp, double sr, double bpm,
                                       int total, const std::vector<int32_t>& offsets,
                                       const std::vector<int>& cuts,
                                       const std::vector<EventType>& types = {}) {
    std::vector<float> out(static_cast<size_t>(total) * 2u, 0.f);
    std::vector<int> bounds{ 0 };
    for (int c : cuts) bounds.push_back(c);
    bounds.push_back(total);
    for (size_t b = 0; b + 1 < bounds.size(); ++b) {
        const int bs = bounds[b], be = bounds[b + 1];
        auto ts = makeSyncTS(sr, bpm, bs, be - bs);
        std::vector<EventWithOffset> evs;
        for (size_t i = 0; i < offsets.size(); ++i) {
            if (offsets[i] >= bs && offsets[i] < be) {
                const EventType t = (i < types.size()) ? types[i] : EventType::Trigger;
                evs.push_back({ offsets[i] - bs, trigEv(0) });
                evs.back().ev.type = t;
            }
        }
        std::vector<float> seg(static_cast<size_t>(be - bs) * 2u, 0.f);
        sp.processBlock(ts, seg.data(), be - bs, evs.data(), static_cast<int>(evs.size()));
        std::copy(seg.begin(), seg.end(), out.begin() + static_cast<size_t>(bs) * 2u);
    }
    return out;
}

// ─── T-SYNC1 : LoopSync retriggeré à offset N ≡ découpage physique à N ───────
// Invariant P0-1/P0-2 : le temps utilisé par renderLoopSync() et par l'anchor
// de handleTrigger() correspond au début absolu réel du sous-bloc/event.
// Sans le fix : anchor = blockStart et tranche rejouée depuis le début du bloc.
TEST_CASE("T-SYNC1: LoopSync retrigger at offset N equals physical slicing at N", "[sync][p0]") {
    constexpr double sr  = 44100.0;
    constexpr double bpm = 120.0;
    constexpr int pcmFrames = 4096;
    constexpr int loopBeats = 4;

    // Premier offset >= 23 OBLIGATOIRE : le backward-leak pré-fix (segment
    // [0,K) rendu après dispatch du trigger@K avec anchor=blockStart) n'est
    // visible que si du contenu non nul tient dans [0,K) — ici le burst à
    // partir du sample ~22 (impulsion frame 1, loop 88200). Avec un premier
    // offset de 1, les erreurs pré-fix s'annulent exactement et le test est
    // aveugle (constaté empiriquement).
    const std::vector<std::pair<int, std::vector<int32_t>>> cases = {
        { 64,   { 30, 63 } },
        { 128,  { 30, 63, 127 } },
        { 256,  { 30, 63, 127, 255 } },
        { 512,  { 30, 63, 127, 255, 511 } },
        { 1024, { 30, 63, 127, 255, 511 } },
    };

    for (const auto& [total, offsets] : cases) {
        SlotPlayer a, b;
        setupLoopSyncImpulse(a, pcmFrames, static_cast<float>(sr), loopBeats);
        setupLoopSyncImpulse(b, pcmFrames, static_cast<float>(sr), loopBeats);

        const std::vector<float> single = renderSingle(a, sr, bpm, total, offsets);
        // Cuts DÉCOUPLÉS des triggers : inclure des frontières SANS event.
        // (Si cuts == offsets, les deux erreurs pré-fix — anchor=blockStart et
        // tranche rejouée depuis blockStart — se compensent exactement et le
        // test devient aveugle. Les cuts triggerless forcent la divergence.)
        std::vector<int> cuts(offsets.begin(), offsets.end());
        for (int extra : { total / 2, total * 3 / 4 }) {
            if (extra > 0 && extra < total &&
                std::find(cuts.begin(), cuts.end(), extra) == cuts.end())
                cuts.push_back(extra);
        }
        std::sort(cuts.begin(), cuts.end());
        const std::vector<float> sliced = renderSliced(b, sr, bpm, total, offsets, cuts);

        INFO("total=" << total);
        REQUIRE(single == sliced);
    }
}

// T-SYNC1b : plusieurs events mixtes (retrigger + release) dans un même buffer.
TEST_CASE("T-SYNC1b: mixed trigger/release events equal physical slicing", "[sync][p0]") {
    constexpr double sr  = 48000.0;
    constexpr double bpm = 133.7;
    constexpr int total = 512;

    const std::vector<int32_t>   offsets = { 100, 300 };
    const std::vector<EventType> types   = { EventType::Trigger, EventType::Release };
    const std::vector<int>       cuts    = { 100, 300 };

    SlotPlayer a, b;
    setupLoopSyncImpulse(a, 2048, static_cast<float>(sr), 2);
    setupLoopSyncImpulse(b, 2048, static_cast<float>(sr), 2);

    REQUIRE(renderSingle(a, sr, bpm, total, offsets, types)
         == renderSliced(b, sr, bpm, total, offsets, cuts, types));
}

// ─── T-PARTITION : équivalence de partitionnement ────────────────────────────
// Même séquence rendue en 1×512 vs 8×64 : positions des transients (OneShot)
// et phase LoopSync bit-identiques. Aucune rampe active (pas d'effet dépendant
// du block size dans ce chemin) → égalité exacte attendue. Garde-fou principal
// contre le retour du bug P0-1 (tranche LoopSync décalée après un event).
TEST_CASE("T-PARTITION: 1x512 equals 8x64 on transients and LoopSync phase", "[sync][p0]") {
    constexpr double sr  = 44100.0;
    constexpr double bpm = 120.0;
    constexpr int total = 512;

    // Slot 0 : LoopSync impulsionnel ; slot 1 : OneShot transients.
    // Events répartis pour frapper plusieurs sous-blocs dans chaque partition.
    const std::vector<int32_t> offsets = { 0, 70, 200, 511 };

    auto setup = [&](SlotPlayer& sp) {
        setupLoopSyncImpulse(sp, 4096, static_cast<float>(sr), 4);
        SlotPcm click;
        click.numChannels = 1;
        click.numFrames   = 256;
        click.sampleRate  = static_cast<float>(sr);
        click.data.assign(256, 0.f);
        click.data[1] = 0.8f;   // frame 0 = 0 → pas de micro-fade
        sp.loadSlot(1, std::move(click), PlayMode::OneShot);
        sp.setSpatial(1, 0.f, 0.f);
    };

    // Rendu A : 1×512, tous les events à offsets absolus.
    SlotPlayer pa;
    setup(pa);
    auto tsa = makeSyncTS(sr, bpm, 0, total);
    std::vector<EventWithOffset> evsA;
    for (int32_t off : offsets) {
        evsA.push_back({ off, trigEv(0) });
        evsA.push_back({ off, trigEv(1) });
    }
    std::vector<float> outA(static_cast<size_t>(total) * 2u, 0.f);
    pa.processBlock(tsa, outA.data(), total, evsA.data(), static_cast<int>(evsA.size()));

    // Rendu B : 8×64, mêmes events absolus (offsets relatifs par segment).
    SlotPlayer pb;
    setup(pb);
    std::vector<float> outB(static_cast<size_t>(total) * 2u, 0.f);
    for (int bs = 0; bs < total; bs += 64) {
        auto tsb = makeSyncTS(sr, bpm, bs, 64);
        std::vector<EventWithOffset> evsB;
        for (int32_t off : offsets) {
            if (off >= bs && off < bs + 64) {
                evsB.push_back({ off - bs, trigEv(0) });
                evsB.push_back({ off - bs, trigEv(1) });
            }
        }
        std::vector<float> seg(64 * 2u, 0.f);
        pb.processBlock(tsb, seg.data(), 64, evsB.data(), static_cast<int>(evsB.size()));
        std::copy(seg.begin(), seg.end(), outB.begin() + static_cast<size_t>(bs) * 2u);
    }

    REQUIRE(outA == outB);
}

// ─── T-SYNC2 : comptabilité swing (P0-4) ─────────────────────────────────────
// Un trigger déplacé par swing hors du bloc courant doit être généré
// exactement dans le bloc contenant son timestamp final : jamais supprimé,
// jamais clampé, jamais dupliqué. Driver fidèle au live : Transport +
// Sequencer + scheduler PERSISTANT entre blocs (les events futurs survivent).
static std::vector<int64_t> collectSwingTriggers(double sr, double bpm, int blockSize,
                                                 int numSteps, float swing,
                                                 bool stepOneOnly = false) {
    TransportState ts{};
    ts.sampleRate     = sr;
    ts.bpm            = bpm;
    ts.samplesPerBeat = sr * 60.0 / bpm;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;
    ts.playing        = true;

    Sequencer seq;
    TrackPattern pat{};
    pat.numSteps = 16;
    if (stepOneOnly) {
        pat.steps[1] = true;   // impair → toujours swingué
    } else {
        for (int i = 0; i < 16; ++i) pat.steps[i] = true;
    }
    *seq.patterns().writeBuffer(0) = pat;
    seq.patterns().publish();

    const int64_t total =
        static_cast<int64_t>(std::ceil(static_cast<double>(numSteps) * ts.samplesPerStep));

    EventScheduler sched;
    std::vector<int64_t> times;
    int64_t pos = 0;
    while (pos < total) {
        const int32_t n = static_cast<int32_t>(std::min<int64_t>(blockSize, total - pos));
        ts.blockStart = pos;
        ts.samplePos  = pos + n;
        seq.generateEvents(ts, pos, n, swing, sched);
        sched.processBlock(pos, n, [&](int32_t, const EngineEvent& ev) {
            if (ev.slot == 0) times.push_back(ev.time);
        });
        pos += n;
    }
    return times;
}

static std::vector<int64_t> expectedSwingTriggers(double sr, double bpm, int numSteps,
                                                  float swing, bool stepOneOnly = false) {
    TransportState ts{};
    ts.sampleRate     = sr;
    ts.samplesPerBeat = sr * 60.0 / bpm;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;
    const double swClamped = std::clamp(static_cast<double>(swing), 0.0, 1.0);
    const int64_t sw = static_cast<int64_t>(std::round(swClamped * ts.samplesPerStep));
    const int64_t total =
        static_cast<int64_t>(std::ceil(static_cast<double>(numSteps) * ts.samplesPerStep));
    std::vector<int64_t> times;
    for (int64_t s = 0; s < numSteps; ++s) {
        const bool on = stepOneOnly ? ((s % 16) == 1) : true;
        if (!on) continue;
        const int64_t t = sampleOfStep(ts, s) + (((s & 1) == 1) ? sw : 0);
        if (t < total) times.push_back(t);
    }
    return times;
}

// T-SYNC2a : mêmes timestamps exacts quelle que soit la taille de bloc.
TEST_CASE("T-SYNC2a: swung triggers identical across block sizes", "[sync][p0]") {
    constexpr double bpm = 120.0, sr = 44100.0;
    constexpr int numSteps = 256;
    const std::vector<int64_t> ref = collectSwingTriggers(sr, bpm, 512, numSteps, 0.5f);
    REQUIRE(ref == expectedSwingTriggers(sr, bpm, numSteps, 0.5f));
    for (int block : { 64, 128, 256, 1024, 2048 }) {
        INFO("block=" << block);
        REQUIRE(collectSwingTriggers(sr, bpm, block, numSteps, 0.5f) == ref);
    }
}

// T-SYNC2b : frontières explicites — triggers swingués tombant pile sur
// blockEnd (offset 0 du bloc suivant) ou blockEnd+1 (offset 1).
TEST_CASE("T-SYNC2b: swung triggers landing on block edges are exact", "[sync][p0]") {
    constexpr double bpm = 120.0, sr = 44100.0;
    constexpr int block = 512, numSteps = 2048;
    const std::vector<int64_t> times = collectSwingTriggers(sr, bpm, block, numSteps, 0.5f);
    const std::vector<int64_t> expected = expectedSwingTriggers(sr, bpm, numSteps, 0.5f);
    REQUIRE(times == expected);

    bool sawEdge0 = false, sawEdge1 = false;
    for (int64_t t : expected) {
        const int64_t m = t % block;
        if (m == 0) sawEdge0 = true;
        if (m == 1) sawEdge1 = true;
    }
    // Le run couvre ces cas (garde : le test resterait vert sinon par vacuité).
    REQUIRE(sawEdge0);
    REQUIRE(sawEdge1);
    // Chacun présent exactement une fois dans le flux émis.
    for (int64_t t : expected) {
        const int64_t m = t % block;
        if (m == 0 || m == 1)
            REQUIRE(std::count(times.begin(), times.end(), t) == 1);
    }
}

// T-SYNC2c : longue durée — 8192 steps (512 mesures), bloc 64, compte exact,
// unicité, ordre chronologique. Aucune perte/duplication.
TEST_CASE("T-SYNC2c: long-run swing accounting, no loss or duplication", "[sync][p0]") {
    constexpr double bpm = 133.7, sr = 48000.0;
    constexpr int numSteps = 8192;
    const std::vector<int64_t> times =
        collectSwingTriggers(sr, bpm, 64, numSteps, 0.7f, true);
    const std::vector<int64_t> expected =
        expectedSwingTriggers(sr, bpm, numSteps, 0.7f, true);
    REQUIRE(times.size() == expected.size());
    REQUIRE(times.size() == 512);   // 8192 / 16 : un trigger par groupe
    REQUIRE(times == expected);
    REQUIRE(std::is_sorted(times.begin(), times.end()));
    REQUIRE(std::adjacent_find(times.begin(), times.end()) == times.end());
}

// T-SYNC2d : clamp d'entrée — swing hors domaine rabattu sur [0,1].
TEST_CASE("T-SYNC2d: swing input clamped to legal domain", "[sync][p0]") {
    constexpr double bpm = 120.0, sr = 44100.0;
    constexpr int numSteps = 128;
    REQUIRE(collectSwingTriggers(sr, bpm, 256, numSteps, 2.0f)
         == collectSwingTriggers(sr, bpm, 256, numSteps, 1.0f));
    REQUIRE(collectSwingTriggers(sr, bpm, 256, numSteps, -1.0f)
         == collectSwingTriggers(sr, bpm, 256, numSteps, 0.0f));
}
