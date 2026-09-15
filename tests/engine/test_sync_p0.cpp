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

// ─── T-SYNC3 : flip de pattern quantisé (P0-3) ───────────────────────────────
// Driver : Sequencer + scheduler persistant, blocs fixes, collecte des temps.
static std::vector<int64_t> collectSeqTriggers(Sequencer& seq, double sr, double bpm,
                                               int blockSize, int64_t total) {
    TransportState ts{};
    ts.sampleRate     = sr;
    ts.bpm            = bpm;
    ts.samplesPerBeat = sr * 60.0 / bpm;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;
    ts.playing        = true;
    EventScheduler sched;
    std::vector<int64_t> times;
    int64_t pos = 0;
    while (pos < total) {
        const int32_t n = static_cast<int32_t>(std::min<int64_t>(blockSize, total - pos));
        ts.blockStart = pos;
        ts.samplePos  = pos + n;
        seq.generateEvents(ts, pos, n, 0.f, sched);
        sched.processBlock(pos, n, [&](int32_t, const EngineEvent& ev) {
            if (ev.slot == 0) times.push_back(ev.time);
        });
        pos += n;
    }
    return times;
}

// T-SYNC3a : pattern A (tout on) puis stage de B (step0 seul) à la frontière
// mid-bloc — le flip tombe exactement sur la frontière, pas avant/après.
TEST_CASE("T-SYNC3a: staged pattern flips exactly at boundary", "[sync][p0]") {
    constexpr double sr = 44100.0, bpm = 120.0;
    constexpr int block = 512;
    TransportState ts{};
    ts.samplesPerBeat = sr * 60.0 / bpm;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;

    Sequencer seq;
    TrackPattern patA{};
    patA.numSteps = 16;
    for (int i = 0; i < 16; ++i) patA.steps[i] = true;
    *seq.patterns().writeBuffer(0) = patA;
    seq.patterns().publish();

    TrackPattern staged[9]{};
    staged[0].numSteps = 16;
    staged[0].steps[0] = true;
    const int64_t boundary = sampleOfStep(ts, 32);   // 176400, 176400 % 512 = 272
    REQUIRE(boundary % block != 0);   // garde : frontière vraiment mid-bloc
    seq.stageForBoundary(staged, boundary);

    const int64_t total = sampleOfStep(ts, 48);
    const std::vector<int64_t> times = collectSeqTriggers(seq, sr, bpm, block, total);

    std::vector<int64_t> expected;
    for (int64_t s = 0; s < 32; ++s) expected.push_back(sampleOfStep(ts, s));
    expected.push_back(sampleOfStep(ts, 32));   // B step0 : 32 % 16 == 0
    REQUIRE(times == expected);
    REQUIRE(!seq.hasStaged());   // stage consommé à la frontière
}

// T-SYNC3b : clearStaged annule (A persiste) ; publication live immédiate.
TEST_CASE("T-SYNC3b: clearStaged cancels, live publish stays immediate", "[sync][p0]") {
    constexpr double sr = 44100.0, bpm = 120.0;
    constexpr int block = 512;
    TransportState ts{};
    ts.samplesPerBeat = sr * 60.0 / bpm;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;

    Sequencer seq;
    TrackPattern patA{};
    patA.numSteps = 16;
    for (int i = 0; i < 16; ++i) patA.steps[i] = true;
    *seq.patterns().writeBuffer(0) = patA;
    seq.patterns().publish();

    TrackPattern staged[9]{};
    staged[0].numSteps = 16;   // tout éteint
    seq.stageForBoundary(staged, sampleOfStep(ts, 32));
    seq.clearStaged();
    REQUIRE(!seq.hasStaged());

    const int64_t total = sampleOfStep(ts, 48);
    const std::vector<int64_t> times = collectSeqTriggers(seq, sr, bpm, block, total);
    std::vector<int64_t> expected;
    for (int64_t s = 0; s < 48; ++s) expected.push_back(sampleOfStep(ts, s));
    REQUIRE(times == expected);

    // Édition live : publication immédiate, sans frontière.
    TrackPattern patB{};
    patB.numSteps = 16;
    patB.steps[0] = true;
    *seq.patterns().writeBuffer(0) = patB;
    seq.patterns().publish();
    const std::vector<int64_t> live =
        collectSeqTriggers(seq, sr, bpm, block, sampleOfStep(ts, 16));
    std::vector<int64_t> expectedLive = { sampleOfStep(ts, 0) };
    REQUIRE(live == expectedLive);
}

// ─── T-BOUNDARY : scène à la frontière ───────────────────────────────────────
// Pattern A (step 15 seul), pattern B (step 0 seul). B armé pendant A :
// dernier event A et premier event B de part et d'autre de la frontière
// exacte — pas de B prématuré, pas de cycle A supplémentaire.
TEST_CASE("T-BOUNDARY: last A and first B straddle the exact boundary", "[sync][p0]") {
    constexpr double sr = 44100.0, bpm = 120.0;
    constexpr int block = 512;
    TransportState ts{};
    ts.samplesPerBeat = sr * 60.0 / bpm;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;

    Sequencer seq;
    TrackPattern patA{};
    patA.numSteps   = 16;
    patA.steps[15]  = true;
    *seq.patterns().writeBuffer(0) = patA;
    seq.patterns().publish();

    TrackPattern staged[9]{};
    staged[0].numSteps = 16;
    staged[0].steps[0] = true;
    const int64_t boundary = sampleOfStep(ts, 48);   // 264600
    seq.stageForBoundary(staged, boundary);

    const int64_t total = sampleOfStep(ts, 64);
    const std::vector<int64_t> times = collectSeqTriggers(seq, sr, bpm, block, total);

    const std::vector<int64_t> expected = {
        sampleOfStep(ts, 15), sampleOfStep(ts, 31),
        sampleOfStep(ts, 47), sampleOfStep(ts, 48),
    };
    REQUIRE(times == expected);
    REQUIRE(times[2] < boundary);    // dernier A strictement avant
    REQUIRE(times[3] == boundary);   // premier B exactement dessus
}

// ─── T-DRIFT : alignement longue durée ───────────────────────────────────────
// Une loop impulsionnelle LoopSync (frame 1 = 1.0, bypass) rendue sur des
// centaines de mesures : chaque début de burst tombe EXACTEMENT sur
// ceil(k*L + L/N) — erreur nulle, aucune accumulation. Streaming (pas de
// gros buffer) sur la matrice SR × BPM × tailles de bloc.
static std::vector<int64_t> loopBurstStarts(double sr, double bpm, int blockSize,
                                            int numLoops, int pcmFrames = 1024) {
    SlotPlayer sp;
    sp.prepareStretchers(1, static_cast<float>(sr));
    sp.setSpatial(0, 0.f, 0.f);
    sp.loadSlot(0, makeImpulsePcm(pcmFrames, static_cast<float>(sr)), PlayMode::LoopSync);
    sp.armLoopSync(0, 1, 1.0f, 0.0f, 0);

    const double loopLen = sr * 60.0 / bpm;   // loopBeats = 1
    // total EXACT : les bursts k < numLoops sont < total, le burst k = numLoops
    // est > total (pas de +blockSize : il admettrait un burst de trop).
    const int64_t total =
        static_cast<int64_t>(std::ceil(numLoops * loopLen));

    TransportState ts{};
    ts.sampleRate     = sr;
    ts.bpm            = bpm;
    ts.samplesPerBeat = loopLen;
    ts.samplesPerStep = loopLen / 4.0;
    ts.playing        = true;

    std::vector<int64_t> starts;
    int64_t pos = 0;
    float prevVal = 0.f;
    bool first = true;
    std::vector<float> out(static_cast<size_t>(blockSize) * 2u, 0.f);
    while (pos < total) {
        const int32_t n = static_cast<int32_t>(std::min<int64_t>(blockSize, total - pos));
        ts.blockStart = pos;
        ts.samplePos  = pos + n;
        std::vector<EventWithOffset> evs;
        if (first) {
            evs.push_back({ 0, trigEv(0) });
            first = false;
        }
        std::fill(out.begin(), out.end(), 0.f);
        sp.processBlock(ts, out.data(), n, evs.data(), static_cast<int>(evs.size()));
        for (int i = 0; i < n; ++i) {
            const float v = out[static_cast<size_t>(i) * 2u];
            if (v != 0.f && prevVal == 0.f) starts.push_back(pos + i);
            prevVal = v;
        }
        pos += n;
    }
    return starts;
}

// T-DRIFT1 : 200 mesures, matrice SR × BPM × blocs — erreur de phase BORNÉE.
// Position idéale (maths réelles) du burst k : p* = k*L + L/N. Le rendu est
// exact au sample près ; la forme close ceil() utilisée comme référence peut
// elle-même arrondir d'un sample aux frontières ulp (constaté : k=346 à
// 48 kHz/133.7 — le rendu avait raison, la formule tort). On assert donc
// |pos - p*| <= 1 pour chaque k (borne ABSOLUE, indépendante de k : aucune
// accumulation possible) + compte exact (aucune perte/duplication).
TEST_CASE("T-DRIFT1: LoopSync aligned after hundreds of measures", "[sync][drift]") {
    constexpr int numLoops = 800;   // loopBeats=1 → 200 mesures 4/4
    for (double sr : { 44100.0, 48000.0 }) {
        for (double bpm : { 120.0, 133.7 }) {
            const double loopLen = sr * 60.0 / bpm;
            const double ratio   = loopLen / 1024.0;
            for (int block : { 64, 1024 }) {
                INFO("sr=" << sr << " bpm=" << bpm << " block=" << block);
                const std::vector<int64_t> starts = loopBurstStarts(sr, bpm, block, numLoops);
                REQUIRE(starts.size() == static_cast<size_t>(numLoops));
                for (int k = 0; k < numLoops; ++k) {
                    const double ideal = k * loopLen + ratio;
                    const double err = std::abs(static_cast<double>(starts[static_cast<size_t>(k)]) - ideal);
                    INFO("k=" << k << " got=" << starts[static_cast<size_t>(k)]
                         << " ideal=" << ideal << " err=" << err);
                    REQUIRE(err <= 1.0);
                }
            }
        }
    }
}

// T-DRIFT2 : identité bit-exact entre tailles de bloc (16 mesures).
TEST_CASE("T-DRIFT2: bit-identical output across block sizes", "[sync][drift]") {
    constexpr double sr = 44100.0, bpm = 120.0;
    constexpr int measures = 16;
    const double loopLen = sr * 60.0 / bpm;
    const int64_t total = static_cast<int64_t>(std::ceil(4 * measures * loopLen));

    std::vector<std::vector<float>> renders;
    for (int block : { 64, 128, 256, 512, 1024 }) {
        SlotPlayer sp;
        sp.prepareStretchers(1, static_cast<float>(sr));
        sp.setSpatial(0, 0.f, 0.f);
        sp.loadSlot(0, makeImpulsePcm(1024, static_cast<float>(sr)), PlayMode::LoopSync);
        sp.armLoopSync(0, 1, 1.0f, 0.0f, 0);
        TransportState ts{};
        ts.sampleRate = sr; ts.bpm = bpm;
        ts.samplesPerBeat = loopLen; ts.samplesPerStep = loopLen / 4.0;
        ts.playing = true;
        std::vector<float> out(static_cast<size_t>(total) * 2u, 0.f);
        int64_t pos = 0;
        bool first = true;
        std::vector<float> blk(static_cast<size_t>(block) * 2u, 0.f);
        while (pos < total) {
            const int32_t n = static_cast<int32_t>(std::min<int64_t>(block, total - pos));
            ts.blockStart = pos; ts.samplePos = pos + n;
            std::vector<EventWithOffset> evs;
            if (first) {
                evs.push_back({ 0, trigEv(0) });
                first = false;
            }
            std::fill(blk.begin(), blk.end(), 0.f);
            sp.processBlock(ts, blk.data(), n, evs.data(), static_cast<int>(evs.size()));
            std::copy(blk.begin(), blk.begin() + static_cast<size_t>(n) * 2u,
                      out.begin() + static_cast<size_t>(pos) * 2u);
            pos += n;
        }
        renders.push_back(std::move(out));
    }
    for (size_t i = 1; i < renders.size(); ++i) {
        INFO("block mismatch at index " << i);
        REQUIRE(renders[i] == renders[0]);
    }
}

// T-DRIFT4 : stepIndexAt(sampleOfStep(N)) == N (SR × BPM, petits et grands N).
TEST_CASE("T-DRIFT4: step index round-trips sample offset", "[sync][drift]") {
    TransportState ts{};
    for (double sr : { 44100.0, 48000.0 }) {
        for (double bpm : { 90.0, 120.0, 126.0, 133.7, 140.0, 160.0 }) {
            ts.sampleRate     = sr;
            ts.bpm            = bpm;
            ts.samplesPerBeat = sr * 60.0 / bpm;
            ts.samplesPerStep = ts.samplesPerBeat / 4.0;
            for (int64_t s : { 0LL, 1LL, 2LL, 3LL, 7LL, 15LL, 16LL, 17LL,
                               31LL, 32LL, 100LL, 1000LL, 100000LL, 1000000LL }) {
                INFO("sr=" << sr << " bpm=" << bpm << " step=" << s);
                REQUIRE(stepIndexAt(ts, sampleOfStep(ts, s)) == s);
            }
        }
    }
}

// T-DRIFT5 : wrap de loop identique mono/stéréo (OneShot + Free bouclé).
TEST_CASE("T-DRIFT5: loop wrap identical mono vs stereo", "[sync][drift]") {
    constexpr double sr = 44100.0, bpm = 120.0;
    constexpr int frames = 1000;
    constexpr int total = 352800;   // 4 mesures : ~352 wraps

    auto makeRamp = [&](int numChannels) {
        SlotPcm pcm;
        pcm.numChannels = numChannels;
        pcm.numFrames   = frames;
        pcm.sampleRate  = static_cast<float>(sr);
        pcm.data.reserve(static_cast<size_t>(frames * numChannels));
        for (int i = 0; i < frames; ++i) {
            const float v = static_cast<float>(i) / static_cast<float>(frames - 1);
            pcm.data.push_back(v);
            if (numChannels == 2) pcm.data.push_back(v);
        }
        return pcm;
    };

    for (PlayMode mode : { PlayMode::OneShot, PlayMode::Free }) {
        SlotPlayer mono, stereo;
        for (SlotPlayer* sp : { &mono, &stereo }) {
            sp->prepareStretchers(1, static_cast<float>(sr));
            sp->setSpatial(0, 0.f, 0.f);
        }
        mono.loadSlot(0, makeRamp(1), mode);
        stereo.loadSlot(0, makeRamp(2), mode);

        std::vector<float> outM(static_cast<size_t>(total) * 2u, 0.f);
        std::vector<float> outS(static_cast<size_t>(total) * 2u, 0.f);
        for (int pass = 0; pass < 2; ++pass) {
            SlotPlayer& sp  = (pass == 0) ? mono : stereo;
            std::vector<float>& out = (pass == 0) ? outM : outS;
            int64_t pos = 0;
            bool first = true;
            while (pos < total) {
                const int32_t n = static_cast<int32_t>(std::min<int64_t>(512, total - pos));
                auto ts = makeSyncTS(sr, bpm, pos, n);
                std::vector<EventWithOffset> evs;
                if (first) {
                    evs.push_back({ 0, trigEv(0) });
                    first = false;
                }
                std::vector<float> blk(static_cast<size_t>(n) * 2u, 0.f);
                sp.processBlock(ts, blk.data(), n, evs.data(), static_cast<int>(evs.size()));
                std::copy(blk.begin(), blk.end(), out.begin() + static_cast<size_t>(pos) * 2u);
                pos += n;
            }
        }
        INFO("mode=" << static_cast<int>(mode));
        REQUIRE(outM == outS);
    }
}
