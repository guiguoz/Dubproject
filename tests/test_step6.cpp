#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

#include "dsp/AutoPitchCorrect.h"
#include "dsp/BpmDetector.h"
#include "dsp/ExpressionMapper.h"
#include "dsp/KeyDetector.h"
#include "dsp/ScaleHarmonizer.h"

static constexpr double kSR    = 44100.0;
static constexpr int    kBlock = 512;

// ─────────────────────────────────────────────────────────────────────────────
// BpmDetector
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("BpmDetector default BPM is 120", "[bpm]")
{
    dsp::BpmDetector det;
    REQUIRE(det.getBpm() == Catch::Approx(120.0f));
}

TEST_CASE("BpmDetector reset restores default BPM", "[bpm]")
{
    dsp::BpmDetector det;
    det.prepare(kSR);

    // Feed some signal to influence state
    std::array<float, kBlock> buf{};
    buf.fill(0.5f);
    det.process(buf.data(), kBlock);

    det.reset();
    REQUIRE(det.getBpm() == Catch::Approx(120.0f));
}

TEST_CASE("BpmDetector detects 120 BPM from synthetic onsets", "[bpm]")
{
    // 120 BPM = 1 beat per 0.5 s = 22050 samples at 44100 Hz
    // Block size = 512 → beats every 22050/512 ≈ 43 blocks
    dsp::BpmDetector det;
    det.prepare(kSR);
    det.setThreshold(0.01f);

    constexpr int   kBeatBlocks = 43; // ~120 BPM
    constexpr int   kNumOnsets  = 12;
    constexpr float kLow        = 0.001f;
    constexpr float kHigh       = 0.5f;

    std::array<float, kBlock> low{}, high{};
    low.fill(kLow);
    high.fill(kHigh);

    int blocksSinceOnset = 0;
    for (int onset = 0; onset < kNumOnsets; ++onset)
    {
        // Feed one high-RMS block (onset)
        det.process(high.data(), kBlock);
        ++blocksSinceOnset;

        // Feed (kBeatBlocks - 1) low-RMS blocks
        for (int b = 1; b < kBeatBlocks; ++b)
        {
            det.process(low.data(), kBlock);
            ++blocksSinceOnset;
        }
    }

    const float detectedBpm = det.getBpm();
    // Allow ±10 BPM tolerance (quantisation at block granularity)
    REQUIRE(detectedBpm >= 100.0f);
    REQUIRE(detectedBpm <= 140.0f);
}

TEST_CASE("BpmDetector ignores silence", "[bpm]")
{
    dsp::BpmDetector det;
    det.prepare(kSR);

    std::array<float, kBlock> silent{};
    silent.fill(0.0f);

    for (int i = 0; i < 100; ++i)
        det.process(silent.data(), kBlock);

    // Should stay at default after silent input
    REQUIRE(det.getBpm() == Catch::Approx(120.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
// ExpressionMapper
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ExpressionMapper default is inactive", "[expr]")
{
    dsp::ExpressionMapper m;
    REQUIRE_FALSE(m.isActive());
    REQUIRE(m.getEffectIndex() == -1);
}

TEST_CASE("ExpressionMapper setConfig getConfig roundtrip", "[expr]")
{
    dsp::ExpressionMapper m;
    dsp::ExpressionConfig cfg{ 2, 3, 0.1f, 0.9f, 10.0f, 100.0f };
    m.setConfig(cfg);

    const auto got = m.getConfig();
    REQUIRE(got.effectIndex == 2);
    REQUIRE(got.paramIndex  == 3);
    REQUIRE(got.inMin  == Catch::Approx(0.1f));
    REQUIRE(got.inMax  == Catch::Approx(0.9f));
    REQUIRE(got.outMin == Catch::Approx(10.0f));
    REQUIRE(got.outMax == Catch::Approx(100.0f));
}

TEST_CASE("ExpressionMapper isActive after valid config", "[expr]")
{
    dsp::ExpressionMapper m;
    m.setConfig({ 0, 0, 0.0f, 1.0f, 0.0f, 1.0f });
    REQUIRE(m.isActive());
}

TEST_CASE("ExpressionMapper mapValue identity 0-1 to 0-1", "[expr]")
{
    dsp::ExpressionMapper m;
    m.setConfig({ 0, 0, 0.0f, 1.0f, 0.0f, 1.0f });

    REQUIRE(m.mapValue(0.0f)  == Catch::Approx(0.0f));
    REQUIRE(m.mapValue(0.5f)  == Catch::Approx(0.5f));
    REQUIRE(m.mapValue(1.0f)  == Catch::Approx(1.0f));
}

TEST_CASE("ExpressionMapper mapValue scaled range", "[expr]")
{
    // RMS [0.2, 0.8] → param [10, 100]
    dsp::ExpressionMapper m;
    m.setConfig({ 0, 0, 0.2f, 0.8f, 10.0f, 100.0f });

    // rms=0.5 → t=(0.5-0.2)/(0.8-0.2)=0.5 → 10+0.5*90=55
    REQUIRE(m.mapValue(0.5f) == Catch::Approx(55.0f));
    // rms below inMin → clamped to outMin
    REQUIRE(m.mapValue(0.0f) == Catch::Approx(10.0f));
    // rms above inMax → clamped to outMax
    REQUIRE(m.mapValue(1.0f) == Catch::Approx(100.0f));
}

TEST_CASE("ExpressionMapper degenerate range returns outMin", "[expr]")
{
    dsp::ExpressionMapper m;
    m.setConfig({ 0, 0, 0.5f, 0.5f, 42.0f, 99.0f }); // inMin == inMax
    REQUIRE(m.mapValue(0.5f) == Catch::Approx(42.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
// ScaleHarmonizer
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ScaleHarmonizer default key is unknown", "[scale]")
{
    dsp::ScaleHarmonizer sh;
    REQUIRE(sh.getKey().key == -1);
}

TEST_CASE("ScaleHarmonizer default intervals are major 3rd and 5th", "[scale]")
{
    dsp::ScaleHarmonizer sh;
    const auto iv = sh.getIntervals();
    REQUIRE(iv[0] == 4); // major 3rd
    REQUIRE(iv[1] == 7); // perfect 5th
}

TEST_CASE("ScaleHarmonizer detectFromChroma C major chromagram", "[scale]")
{
    // C major: strong energy on C(0), E(4), G(7) — tonic triad
    std::array<float, 12> chroma{};
    chroma[0]  = 6.35f; // C  (tonic)
    chroma[2]  = 3.48f; // D
    chroma[4]  = 4.38f; // E
    chroma[5]  = 4.09f; // F
    chroma[7]  = 5.19f; // G
    chroma[9]  = 3.66f; // A
    chroma[11] = 2.88f; // B

    const auto result = dsp::ScaleHarmonizer::detectFromChroma(chroma);
    REQUIRE(result.key  == 0); // C
    REQUIRE(result.mode == 0); // major
}

TEST_CASE("ScaleHarmonizer detectFromChroma A minor chromagram", "[scale]")
{
    // Build A minor chromagram by rotating the K-S minor profile to root A (pc=9)
    // rotated[i] = kMinorProfile[(i - 9 + 12) % 12]
    static constexpr float kMinor[12] = {
        6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
        2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
    };
    std::array<float, 12> chroma{};
    for (int i = 0; i < 12; ++i)
        chroma[static_cast<std::size_t>(i)] = kMinor[(i - 9 + 12) % 12];

    const auto result = dsp::ScaleHarmonizer::detectFromChroma(chroma);
    REQUIRE(result.key  == 9); // A
    REQUIRE(result.mode == 1); // minor
}

TEST_CASE("ScaleHarmonizer updateScale after pushing C major notes", "[scale]")
{
    dsp::ScaleHarmonizer sh;

    // Push C major tonic triad (C4, E4, G4) × 7 = 21 notes.
    // Ring buffer holds 20: last 20 = all three triad tones → unambiguous C major.
    const float cTriad[] = { 261.63f, 329.63f, 392.00f };
    for (int rep = 0; rep < 7; ++rep)
        for (float hz : cTriad)
            sh.pushNote(hz, 0.9f);

    const auto result = sh.updateScale();
    REQUIRE(result.key  == 0); // C
    REQUIRE(result.mode == 0); // major
}

TEST_CASE("ScaleHarmonizer low confidence notes are ignored", "[scale]")
{
    dsp::ScaleHarmonizer sh;
    sh.pushNote(261.63f, 0.1f); // C4 but low confidence
    const auto result = sh.updateScale();
    REQUIRE(result.key == -1); // not enough data
}

TEST_CASE("ScaleHarmonizer minor key sets third to 3", "[scale]")
{
    dsp::ScaleHarmonizer sh;

    // Push A minor tonic triad (A4=440, C5=523.25, E5=659.25) × 7 = 21 notes
    const float aMinorTriad[] = { 440.00f, 523.25f, 659.25f };
    for (int rep = 0; rep < 7; ++rep)
        for (float hz : aMinorTriad)
            sh.pushNote(hz, 0.9f);

    sh.updateScale();
    const auto iv = sh.getIntervals();
    REQUIRE(iv[0] == 3); // minor 3rd
    REQUIRE(iv[1] == 7); // perfect 5th
}

TEST_CASE("ScaleHarmonizer reset clears state", "[scale]")
{
    dsp::ScaleHarmonizer sh;
    sh.pushNote(261.63f, 0.9f);
    sh.updateScale();
    sh.reset();
    REQUIRE(sh.getKey().key == -1);
}

// ─────────────────────────────────────────────────────────────────────────────
// AutoPitchCorrect
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AutoPitchCorrect type is AutoPitchCorrect", "[autopitch]")
{
    dsp::AutoPitchCorrect e;
    REQUIRE(e.type() == dsp::EffectType::AutoPitchCorrect);
}

TEST_CASE("AutoPitchCorrect has 2 parameters", "[autopitch]")
{
    dsp::AutoPitchCorrect e;
    REQUIRE(e.paramCount() == 2);
}

TEST_CASE("AutoPitchCorrect default params", "[autopitch]")
{
    dsp::AutoPitchCorrect e;
    REQUIRE(e.getParam(0) == Catch::Approx(1.0f));   // strength
    REQUIRE(e.getParam(1) == Catch::Approx(440.0f)); // refHz
}

TEST_CASE("AutoPitchCorrect setParam getParam roundtrip", "[autopitch]")
{
    dsp::AutoPitchCorrect e;
    e.setParam(0, 0.5f);
    e.setParam(1, 432.0f);
    REQUIRE(e.getParam(0) == Catch::Approx(0.5f));
    REQUIRE(e.getParam(1) == Catch::Approx(432.0f));
}

TEST_CASE("AutoPitchCorrect silence in gives silence out", "[autopitch]")
{
    dsp::AutoPitchCorrect e;
    e.prepare(kSR, kBlock);

    std::array<float, kBlock> buf{};
    buf.fill(0.0f);
    e.process(buf.data(), kBlock, 440.0f);

    for (int i = 0; i < kBlock; ++i)
        REQUIRE(buf[i] == Catch::Approx(0.0f));
}

TEST_CASE("AutoPitchCorrect disabled bypasses entirely", "[autopitch]")
{
    dsp::AutoPitchCorrect e;
    e.prepare(kSR, kBlock);
    e.enabled.store(false, std::memory_order_release);

    std::array<float, kBlock> buf{};
    buf.fill(0.7f);
    e.process(buf.data(), kBlock, 450.0f); // slightly detuned

    for (int i = 0; i < kBlock; ++i)
        REQUIRE(buf[i] == Catch::Approx(0.7f));
}

TEST_CASE("AutoPitchCorrect bypasses when pitch unknown", "[autopitch]")
{
    // pitchHz < 20 → bypass (no correction possible)
    dsp::AutoPitchCorrect e;
    e.prepare(kSR, kBlock);

    std::array<float, kBlock> buf{};
    buf.fill(0.3f);
    e.process(buf.data(), kBlock, 0.0f); // no pitch

    for (int i = 0; i < kBlock; ++i)
        REQUIRE(buf[i] == Catch::Approx(0.3f));
}

// ─────────────────────────────────────────────────────────────────────────────
// KeyDetector
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("KeyDetector initial result is unknown", "[keydet]")
{
    dsp::KeyDetector kd;
    REQUIRE(kd.getResult().key == -1);
}

TEST_CASE("KeyDetector reset clears result", "[keydet]")
{
    dsp::KeyDetector kd;

    // Feed some silence to advance frames
    std::vector<float> silence(kBlock * 20, 0.0f);
    kd.process(silence.data(), static_cast<int>(silence.size()), kSR);
    kd.reset();
    REQUIRE(kd.getResult().key == -1);
}

TEST_CASE("KeyDetector goertzel high power at target frequency", "[keydet]")
{
    // Generate pure sine at 440 Hz, 4096 samples
    constexpr int   N  = 4096;
    constexpr float hz = 440.0f;

    std::array<float, N> sine{};
    for (int i = 0; i < N; ++i)
        sine[i] = std::sin(2.0f * 3.14159265f * hz * static_cast<float>(i) / static_cast<float>(kSR));

    const float powerAt440  = dsp::KeyDetector::goertzel(sine.data(), N, 440.0,  kSR);
    const float powerAt880  = dsp::KeyDetector::goertzel(sine.data(), N, 880.0,  kSR);
    const float powerAt220  = dsp::KeyDetector::goertzel(sine.data(), N, 220.0,  kSR);

    // Power at the signal frequency should be much greater than at others
    REQUIRE(powerAt440 > powerAt880 * 10.0f);
    REQUIRE(powerAt440 > powerAt220 * 10.0f);
}

TEST_CASE("KeyDetector detectFromChroma C major", "[keydet]")
{
    // Same chromagram as ScaleHarmonizer test
    std::array<float, 12> chroma{};
    chroma[0]  = 6.35f;
    chroma[2]  = 3.48f;
    chroma[4]  = 4.38f;
    chroma[5]  = 4.09f;
    chroma[7]  = 5.19f;
    chroma[9]  = 3.66f;
    chroma[11] = 2.88f;

    const auto r = dsp::KeyDetector::detectFromChroma(chroma);
    REQUIRE(r.key  == 0);
    REQUIRE(r.mode == 0);
}

TEST_CASE("KeyDetector process C major scale detects C major", "[keydet]")
{
    // Generate 8 frames (8 × 4096 = 32768 samples) of C major scale tones
    // C4=261.63, E4=329.63, G4=392.00, A4=440.00 mixed together
    dsp::KeyDetector kd;
    constexpr int kFrames = 8;
    constexpr int kN      = dsp::KeyDetector::kFrameSize * kFrames;

    std::vector<float> audio(kN);
    const float freqs[] = { 261.63f, 329.63f, 392.00f, 440.00f,
                             293.66f, 349.23f, 493.88f };

    for (int i = 0; i < kN; ++i)
    {
        float s = 0.0f;
        for (float f : freqs)
            s += 0.1f * std::sin(2.0f * 3.14159265f * f * static_cast<float>(i) / static_cast<float>(kSR));
        audio[i] = s;
    }

    kd.process(audio.data(), kN, kSR);

    const auto result = kd.getResult();
    REQUIRE(result.key  != -1);  // must have detected something
    REQUIRE(result.key  == 0);   // C
    REQUIRE(result.mode == 0);   // major
}
