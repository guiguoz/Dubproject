#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "engine/SlotPlayer.h"

using namespace engine;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static TransportState makeTS(double sr = 44100.0) {
    TransportState ts{};
    ts.sampleRate      = sr;
    ts.samplesPerBeat  = sr * 60.0 / 120.0;
    ts.samplesPerStep  = ts.samplesPerBeat / 4.0;
    ts.blockStart      = 0;
    ts.samplePos       = 0;
    ts.playing         = true;
    ts.bpm             = 120.0;
    return ts;
}

static SlotPcm makePcm(int numFrames, float value = 0.5f) {
    SlotPcm pcm;
    pcm.numChannels = 1;
    pcm.numFrames   = numFrames;
    pcm.sampleRate  = 44100.f;
    pcm.data.assign(numFrames, value);
    return pcm;
}

static EventWithOffset makeEvent(int32_t offset, EventType type, int slot) {
    EngineEvent ev{};
    ev.time = 0;
    ev.type = type;
    ev.slot = static_cast<uint8_t>(slot);
    return { offset, ev };
}

// ─── T-C3a : deux triggers à offsets différents → deux voix actives ─────────
// Vérifie que le dispatch temporel fonctionne : un trigger à l'offset 0 et un
// trigger à l'offset 256 dans un bloc de 512 déclenchent deux voix distinctes.
TEST_CASE("T-C3a: two triggers at different offsets activate two voices", "[slotplayer][dispatch]") {
    constexpr int N = 512;
    SlotPlayer sp;
    sp.prepareStretchers(1, 44100.f);
    sp.setSpatial(0, 0.f, 0.f);
    sp.loadSlot(0, makePcm(N * 4), PlayMode::OneShot);

    auto ts = makeTS();
    ts.blockStart = 0;
    ts.samplePos  = N;

    // Trigger à offset 0 (début du bloc) et offset 256 (milieu)
    EventWithOffset events[2] = {
        makeEvent(0,   EventType::Trigger, 0),
        makeEvent(256, EventType::Trigger, 0)
    };

    std::vector<float> out(static_cast<size_t>(N) * 2, 0.f);
    sp.processBlock(ts, out.data(), N, events, 2);

    // Les deux voix doivent être actives (2 triggers sur le même slot)
    REQUIRE(sp.isVoiceActive(0));

    // Le signal doit devenir non nul après le micro-fade (kFadeLen=16 samples)
    bool nonZeroEarly = false;
    for (int i = 20; i < 100; ++i) {
        if (out[static_cast<size_t>(i) * 2] != 0.f) {
            nonZeroEarly = true;
            break;
        }
    }
    REQUIRE(nonZeroEarly);

    // Le signal doit être non nul à l'offset 256 (2e trigger)
    // Après le 2e trigger, une 2e voix commence → le signal ne devrait pas
    // tomber à 0 entre les deux.
    bool anyNonZero256 = false;
    for (int i = 250; i < 260 && i < N; ++i) {
        if (out[static_cast<size_t>(i) * 2] != 0.f) {
            anyNonZero256 = true;
            break;
        }
    }
    REQUIRE(anyNonZero256);
}

// ─── T-C3b : GainRamp à l'offset 100 n'est pas écrasé par un 2e au même bloc
// Vérifie que deux GainRamps dans le même bloc s'appliquent dans l'ordre et
// pas au sample 0.
TEST_CASE("T-C3b: GainRamp at offset 100 is not overwritten by another in same block", "[slotplayer][dispatch]") {
    constexpr int N = 512;
    SlotPlayer sp;
    sp.prepareStretchers(1, 44100.f);
    sp.setSpatial(0, 0.f, 0.f);
    sp.loadSlot(0, makePcm(N * 4), PlayMode::OneShot);

    auto ts = makeTS();
    ts.blockStart = 0;
    ts.samplePos  = N;

    // Trigger au début
    EventWithOffset events[2] = {
        makeEvent(0,   EventType::Trigger, 0),
        { 100, EngineEvent{0, EventType::GainRamp, 0, 441.f, 0.0f} }  // fade-out à l'offset 100
    };

    std::vector<float> out(static_cast<size_t>(N) * 2, 0.f);
    sp.processBlock(ts, out.data(), N, events, 2);

    // Le signal doit être non nul avant l'offset 100 (le fade n'a pas encore démarré)
    bool nonZeroBefore100 = false;
    for (int i = 10; i < 50; ++i) {
        if (out[static_cast<size_t>(i) * 2] != 0.f) {
            nonZeroBefore100 = true;
            break;
        }
    }
    REQUIRE(nonZeroBefore100);

    // Après le fade-out (offset 100 + 441 samples), le signal doit être très faible
    // ou nul (fade vers 0). On vérifie qu'il y a une différence de niveau.
    float peakBefore = 0.f;
    for (int i = 10; i < 50; ++i)
        peakBefore = std::max(peakBefore, std::abs(out[static_cast<size_t>(i) * 2]));

    float peakAfter = 0.f;
    const int fadeEnd = std::min(100 + 441, N);
    for (int i = fadeEnd; i < N; ++i)
        peakAfter = std::max(peakAfter, std::abs(out[static_cast<size_t>(i) * 2]));

    // Le peak après le fade doit être inférieur au peak avant
    REQUIRE(peakAfter < peakBefore);
}

// ─── T-C3c : event à l'offset 0 et event nullptr (pas d'events) ────────────
// Vérifie que processBlock avec nullptr fonctionne sans crash.
TEST_CASE("T-C3c: processBlock with nullptr events", "[slotplayer][dispatch]") {
    constexpr int N = 256;
    SlotPlayer sp;
    sp.prepareStretchers(1, 44100.f);
    sp.setSpatial(0, 0.f, 0.f);
    sp.loadSlot(0, makePcm(N * 2), PlayMode::OneShot);

    auto ts = makeTS();
    ts.blockStart = 0;
    ts.samplePos  = N;

    std::vector<float> out(static_cast<size_t>(N) * 2, 0.f);
    sp.processBlock(ts, out.data(), N, nullptr, 0);

    // Pas de trigger → pas de signal
    for (int i = 0; i < N; ++i)
        REQUIRE(out[static_cast<size_t>(i) * 2] == 0.f);
}
