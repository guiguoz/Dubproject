#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <cmath>
#include "engine/SlotPlayer.h"

using namespace engine;

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Crée un PCM mono rempli d'une rampe : data[i] = (i+1) * 0.01f
// Valeur initiale : 0.01f > kFadeThreshold (0.001f) → déclenche le micro-fade.
static SlotPcm makeMono(int numFrames, float value = 0.5f) {
    SlotPcm pcm;
    pcm.numChannels = 1;
    pcm.numFrames   = numFrames;
    pcm.sampleRate  = 44100.f;
    pcm.data.resize(static_cast<size_t>(numFrames), value);
    return pcm;
}

// Crée un PCM mono dont chaque frame a une valeur unique (index + 1) * 0.001f.
// Valeur initiale 0.001f == kFadeThreshold → PAS de micro-fade (seuil strict >).
static SlotPcm makeMonoRamp(int numFrames) {
    SlotPcm pcm;
    pcm.numChannels = 1;
    pcm.numFrames   = numFrames;
    pcm.sampleRate  = 44100.f;
    pcm.data.resize(static_cast<size_t>(numFrames));
    for (int i = 0; i < numFrames; ++i)
        pcm.data[static_cast<size_t>(i)] = static_cast<float>(i + 1) * 0.001f;
    return pcm;
}

// Crée un PCM stéréo interleaved : L[i] = (i+1)*0.001f, R[i] = -(i+1)*0.001f
// Valeur L[0] = 0.001f → PAS de micro-fade.
static SlotPcm makeStereoRamp(int numFrames) {
    SlotPcm pcm;
    pcm.numChannels = 2;
    pcm.numFrames   = numFrames;
    pcm.sampleRate  = 44100.f;
    pcm.data.resize(static_cast<size_t>(numFrames) * 2u);
    for (int i = 0; i < numFrames; ++i) {
        pcm.data[static_cast<size_t>(i) * 2u]      =  static_cast<float>(i + 1) * 0.001f;
        pcm.data[static_cast<size_t>(i) * 2u + 1u] = -static_cast<float>(i + 1) * 0.001f;
    }
    return pcm;
}

// TransportState minimal (non utilisé par le rendu en M3 mais requis par l'API).
static TransportState makeTS() {
    TransportState ts;
    ts.samplePos      = 0;
    ts.sampleRate     = 44100.0;
    ts.bpm            = 120.0;
    ts.samplesPerBeat = 22050.0;
    ts.samplesPerStep = 5512.5;
    ts.playing        = true;
    return ts;
}

// Construit un EngineEvent Trigger pour un slot donné.
static EngineEvent makeTrigger(int slot) {
    EngineEvent ev{};
    ev.time = 0;
    ev.type = EventType::Trigger;
    ev.slot = static_cast<uint8_t>(slot);
    return ev;
}

// ─── T-SP1a : ONE-SHOT mono → sortie bit-identique au PCM (gain 1.0) ─────────
TEST_CASE("T-SP1a: ONE-SHOT mono output is bit-identical to PCM (no fade)", "[slotplayer]") {
    // Valeur frame 0 = 0.001f (== threshold, pas de fade)
    const int N = 32;
    SlotPlayer sp;
    sp.loadSlot(0, makeMonoRamp(N), PlayMode::OneShot);

    const auto ts = makeTS();
    const EngineEvent ev = makeTrigger(0);

    std::vector<float> out(static_cast<size_t>(N) * 2u, 0.f);
    sp.processBlock(ts, out.data(), N, &ev, 1);

    // Vérifier chaque frame : L == R == pcm[i]
    for (int i = 0; i < N; ++i) {
        const float expected = static_cast<float>(i + 1) * 0.001f;
        REQUIRE(out[static_cast<size_t>(i) * 2u]      == expected);
        REQUIRE(out[static_cast<size_t>(i) * 2u + 1u] == expected);
    }
}

// ─── T-SP1b : ONE-SHOT stéréo → sortie bit-identique ─────────────────────────
TEST_CASE("T-SP1b: ONE-SHOT stereo output is bit-identical to PCM", "[slotplayer]") {
    const int N = 16;
    SlotPlayer sp;
    sp.loadSlot(0, makeStereoRamp(N), PlayMode::OneShot);

    const auto ts = makeTS();
    const EngineEvent ev = makeTrigger(0);

    std::vector<float> out(static_cast<size_t>(N) * 2u, 0.f);
    sp.processBlock(ts, out.data(), N, &ev, 1);

    for (int i = 0; i < N; ++i) {
        const float expL =  static_cast<float>(i + 1) * 0.001f;
        const float expR = -static_cast<float>(i + 1) * 0.001f;
        REQUIRE(out[static_cast<size_t>(i) * 2u]      == expL);
        REQUIRE(out[static_cast<size_t>(i) * 2u + 1u] == expR);
    }
}

// ─── T-SP1c : FREE → même chemin de rendu que ONE-SHOT ───────────────────────
TEST_CASE("T-SP1c: FREE mode output matches PCM for first N frames", "[slotplayer]") {
    const int N = 24;
    SlotPlayer sp;
    sp.loadSlot(0, makeMonoRamp(N), PlayMode::Free);

    const auto ts = makeTS();
    const EngineEvent ev = makeTrigger(0);

    std::vector<float> out(static_cast<size_t>(N) * 2u, 0.f);
    sp.processBlock(ts, out.data(), N, &ev, 1);

    for (int i = 0; i < N; ++i) {
        const float expected = static_cast<float>(i + 1) * 0.001f;
        REQUIRE(out[static_cast<size_t>(i) * 2u]      == expected);
        REQUIRE(out[static_cast<size_t>(i) * 2u + 1u] == expected);
    }
}

// ─── T-SP1d : slot muté → sortie = zéro ─────────────────────────────────────
TEST_CASE("T-SP1d: muted slot produces zero output", "[slotplayer]") {
    const int N = 16;
    SlotPlayer sp;
    sp.loadSlot(0, makeMonoRamp(N), PlayMode::OneShot);

    // Muter via un événement Mute avant le Trigger
    const auto ts = makeTS();
    EngineEvent evs[2];
    evs[0].time = 0; evs[0].type = EventType::Mute;    evs[0].slot = 0;
    evs[1].time = 0; evs[1].type = EventType::Trigger;  evs[1].slot = 0;

    std::vector<float> out(static_cast<size_t>(N) * 2u, 0.f);
    sp.processBlock(ts, out.data(), N, evs, 2);

    for (int i = 0; i < N * 2; ++i)
        REQUIRE(out[static_cast<size_t>(i)] == 0.f);
}

// ─── T-SP1e : chevauchement → deux voix s'additionnent ───────────────────────
TEST_CASE("T-SP1e: overlapping triggers sum two voices", "[slotplayer]") {
    // PCM ramp sans fade : valeur frame i = (i+1)*0.001f
    const int N = 8;
    SlotPlayer sp;
    sp.loadSlot(0, makeMonoRamp(N), PlayMode::OneShot);

    const auto ts = makeTS();

    // Premier trigger : lance voice[0]
    {
        const EngineEvent ev = makeTrigger(0);
        std::vector<float> out(static_cast<size_t>(N) * 2u, 0.f);
        sp.processBlock(ts, out.data(), N, &ev, 1);
        // La voix avance de N frames (readPos = N → inactive pour ONE-SHOT)
    }

    // Recharger le PCM pour remettre les voix à zéro, puis tester le chevauchement
    // On recharge → voice[0] et voice[1] sont reset.
    sp.loadSlot(0, makeMonoRamp(N), PlayMode::OneShot);

    // Trigger 1 : lance voice[0]
    {
        const EngineEvent ev = makeTrigger(0);
        std::vector<float> dummy(static_cast<size_t>(N / 2) * 2u, 0.f);
        // Rendre seulement N/2 frames : voice[0] a readPos = N/2 (toujours active)
        sp.processBlock(ts, dummy.data(), N / 2, &ev, 1);
    }

    // Trigger 2 : lance voice[1] (voice[0] encore active)
    {
        const EngineEvent ev = makeTrigger(0);
        std::vector<float> out(static_cast<size_t>(N / 2) * 2u, 0.f);
        sp.processBlock(ts, out.data(), N / 2, &ev, 1);

        // voice[0] est à readPos = N/2, voice[1] est à readPos = 0
        // frame f : voice[0] → (N/2 + f + 1)*0.001f, voice[1] → (f + 1)*0.001f
        for (int f = 0; f < N / 2; ++f) {
            const float v0 = static_cast<float>(N / 2 + f + 1) * 0.001f;
            const float v1 = static_cast<float>(f + 1) * 0.001f;
            const float expected = v0 + v1;
            REQUIRE(out[static_cast<size_t>(f) * 2u] == expected);
        }
    }
}

// ─── T-SP1f : micro-fade → les 16 premiers frames ont un fade-in ──────────────
TEST_CASE("T-SP1f: micro-fade applies to first 16 frames when first sample > 0.001f", "[slotplayer]") {
    // makeMono(N, 0.5f) : data[i] = 0.5f, premier sample = 0.5f > 0.001f → fade
    const int N = 32;
    const float pcmValue = 0.5f;
    SlotPlayer sp;
    sp.loadSlot(0, makeMono(N, pcmValue), PlayMode::OneShot);

    const auto ts = makeTS();
    const EngineEvent ev = makeTrigger(0);

    std::vector<float> out(static_cast<size_t>(N) * 2u, 0.f);
    sp.processBlock(ts, out.data(), N, &ev, 1);

    // Pendant les 16 premiers frames, le gain va de 0 → 1 (linéaire, step = 1/16).
    // frame 0 : fadeGain=0 → g=0, puis fadeGain += 1/16
    // frame 1 : fadeGain=1/16 → g=1/16
    // ...
    // frame 15 : fadeGain=15/16 → g=15/16
    // frame 16+ : fadeGain=1 → g=1
    for (int f = 0; f < 16; ++f) {
        const float expectedGain = static_cast<float>(f) / 16.f;
        const float expectedVal  = pcmValue * expectedGain;
        REQUIRE(out[static_cast<size_t>(f) * 2u] == expectedVal);
    }

    // Après le fade : gain = 1.0, valeur = pcmValue
    for (int f = 16; f < N; ++f) {
        REQUIRE(out[static_cast<size_t>(f) * 2u] == pcmValue);
    }
}

// ─── T-SP1g : fin de ONE-SHOT → voix inactive après le dernier frame ──────────
TEST_CASE("T-SP1g: ONE-SHOT voice goes silent after last frame", "[slotplayer]") {
    const int N = 8;
    // makeMono sans fade (valeur 0.001f == threshold, pas de fade)
    SlotPlayer sp;
    sp.loadSlot(0, makeMonoRamp(N), PlayMode::OneShot);

    const auto ts = makeTS();

    // Premier bloc : trigger + N frames → consomme tout le PCM
    {
        const EngineEvent ev = makeTrigger(0);
        std::vector<float> out(static_cast<size_t>(N) * 2u, 0.f);
        sp.processBlock(ts, out.data(), N, &ev, 1);
    }

    // Deuxième bloc : sans événement → sortie doit être zéro
    {
        std::vector<float> out(static_cast<size_t>(N) * 2u, 0.f);
        sp.processBlock(ts, out.data(), N, nullptr, 0);

        for (int i = 0; i < N * 2; ++i)
            REQUIRE(out[static_cast<size_t>(i)] == 0.f);
    }
}

// ─── T-SP2 : correction sample rate 44.1 kHz → 48 kHz ───────────────────────
//
// VERROU DE RÉGRESSION — bug découvert lors de la migration V2 :
// renderVoice faisait ++readPos (entier) par frame output, sans tenir compte
// du SR du sample. Conséquence : sample 44100 Hz joué sur device 48000 Hz
// = ×48000/44100 ≈ ×1.088 de vitesse → +1.47 demi-tons perceptibles.
//
// Invariant mathématique (441 × 160/147 = 480 exact) :
//   441 PCM frames @44100 Hz doivent s'épuiser en exactement 480 output frames
//   @48000 Hz — ni plus tôt (coupé trop tôt = ancienne régression), ni plus tard.
//
TEST_CASE("T-SP2: SR 44.1→48 kHz correction — pitch-correct duration", "[slotplayer][sr]") {
    // PCM constant @44100 Hz, valeur == kFadeThreshold → micro-fade désactivé.
    static constexpr int   kPcmFrames   = 441;   // ≡ 0.01 s @44100 Hz
    static constexpr int   kOut48k      = 480;   // 441 × 48000/44100 = 480 (exact)
    static constexpr float kVal         = 0.001f;

    auto makePcm44 = [](float sr = 44100.f) {
        SlotPcm p;
        p.numChannels = 1;
        p.numFrames   = kPcmFrames;
        p.sampleRate  = sr;
        p.data.assign(static_cast<size_t>(kPcmFrames), kVal);
        return p;
    };

    const TransportState ts = makeTS();

    // ── Section A : device 48 kHz — PCM épuisé en exactement 480 frames ──────
    SECTION("device 48 kHz: PCM exhausted after 480 output frames") {
        SlotPlayer sp;
        sp.prepareStretchers(1, 48000.f);
        sp.loadSlot(0, makePcm44(), PlayMode::OneShot);

        std::vector<float> out(static_cast<size_t>(kOut48k) * 2u, 0.f);
        const EngineEvent ev = makeTrigger(0);
        sp.processBlock(ts, out.data(), kOut48k, &ev, 1);

        // Le PCM doit être entièrement consommé : voix inactive.
        REQUIRE_FALSE(sp.isVoiceActive(0));

        // Les 440 premières frames doivent être non nulles
        // (le PCM ne peut pas être épuisé avant la frame 479).
        for (int f = 0; f < 440; ++f)
            REQUIRE(out[static_cast<size_t>(f) * 2u] > 0.f);
    }

    // ── Section B : voix encore active à frame 441 (non coupée trop tôt) ─────
    // C'est le verrou direct contre la régression :
    // sans correction SR, renderVoice s'arrêtait à readPos==441 après 441 output
    // frames. Avec correction, à frame 441 on n'a consommé que 441×(44100/48000)
    // ≈ 405 PCM frames → voix toujours active.
    SECTION("device 48 kHz: voice still active at output frame 441 (not cut early)") {
        SlotPlayer sp;
        sp.prepareStretchers(1, 48000.f);
        sp.loadSlot(0, makePcm44(), PlayMode::OneShot);

        std::vector<float> out(static_cast<size_t>(kPcmFrames) * 2u, 0.f);
        const EngineEvent ev = makeTrigger(0);
        sp.processBlock(ts, out.data(), kPcmFrames, &ev, 1);

        // 441 output frames @48 kHz → 441 × 0.91875 ≈ 405 PCM frames consommées.
        // La voix NE DOIT PAS être éteinte ici (ce serait la régression).
        REQUIRE(sp.isVoiceActive(0));
    }

    // ── Section C : référence sans delta SR (device == sample SR) ────────────
    // Garantit que le chemin bypass (needsSR=false) reste bit-identical.
    SECTION("device 44.1 kHz (no SR delta): PCM exhausted after 441 output frames") {
        SlotPlayer sp;
        sp.prepareStretchers(1, 44100.f);
        sp.loadSlot(0, makePcm44(44100.f), PlayMode::OneShot);

        std::vector<float> out(static_cast<size_t>(kPcmFrames + 1) * 2u, 0.f);
        const EngineEvent ev = makeTrigger(0);
        sp.processBlock(ts, out.data(), kPcmFrames + 1, &ev, 1);

        REQUIRE_FALSE(sp.isVoiceActive(0));
        // Frame 441 (index kPcmFrames) = silence : PCM épuisé au bon moment.
        REQUIRE(out[static_cast<size_t>(kPcmFrames) * 2u] == 0.f);
    }
}
