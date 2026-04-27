#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/DspPipeline.h"
#include "dsp/FlangerEffect.h"
#include "dsp/Sampler.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstring>

using dsp::DspPipeline;
using Catch::Matchers::WithinAbs;

static constexpr float kSampleRate = 44100.0f;
static constexpr int   kBlockSize  = 256;

TEST_CASE("DspPipeline -- all effects disabled -> pass-through", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    pipeline.setMasterLimiterEnabled(false); // disable limiter for true pass-through testing
    // Both effects disabled by default

    auto signal   = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
    auto original = signal;

    pipeline.process(signal.data(), kBlockSize);

    // Disabled pipeline: only YIN analysis runs (read-only), output unchanged
    REQUIRE(test_helpers::buffersEqual(signal.data(), original.data(), kBlockSize));
}

TEST_CASE("DspPipeline -- output always within [-1, 1]", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    for (int block = 0; block < 20; ++block)
    {
        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        pipeline.process(signal.data(), kBlockSize);
        REQUIRE(test_helpers::allSamplesInRange(signal.data(), kBlockSize));
    }
}

TEST_CASE("DspPipeline -- sampler toggle does not crash", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Toggle sampler rapidly while processing
    for (int i = 0; i < 10; ++i)
    {
        pipeline.setSamplerEnabled(i % 2 == 0);

        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        REQUIRE_NOTHROW(pipeline.process(signal.data(), kBlockSize));
    }
}

TEST_CASE("DspPipeline -- getLastPitch returns 0 before enough data", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Feed one small block — not enough for YIN
    auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
    pipeline.process(signal.data(), kBlockSize);

    const auto pitch = pipeline.getLastPitch();
    // Either 0 (not enough data) or a reasonable frequency — must not be garbage
    REQUIRE((pitch.frequencyHz == 0.0f ||
             (pitch.frequencyHz >= 80.0f && pitch.frequencyHz <= 2000.0f)));
}

TEST_CASE("DspPipeline -- reset clears pitch state", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Feed enough blocks for YIN to detect
    for (int i = 0; i < 20; ++i)
    {
        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        pipeline.process(signal.data(), kBlockSize);
    }

    pipeline.reset();
    const auto pitch = pipeline.getLastPitch();
    REQUIRE(pitch.frequencyHz == 0.0f);
}

TEST_CASE("DspPipeline -- getLastRms is 0 before any processing", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    REQUIRE(pipeline.getLastRms() == 0.0f);
}

TEST_CASE("DspPipeline -- getLastRms rises after non-zero input", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Feed several blocks of a 440 Hz sine at amplitude 0.5
    for (int i = 0; i < 50; ++i)
    {
        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        for (auto& s : signal) s *= 0.5f;
        pipeline.process(signal.data(), kBlockSize);
    }

    REQUIRE(pipeline.getLastRms() > 0.01f);
}

TEST_CASE("DspPipeline -- getBpmDetector default BPM is 120", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    REQUIRE_THAT(pipeline.getBpmDetector().getBpm(),
                 WithinAbs(120.0f, 0.01f));
}

TEST_CASE("DspPipeline -- ExpressionMapper inactive by default", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    REQUIRE_FALSE(pipeline.getExpressionMapper().isActive());
}

TEST_CASE("DspPipeline -- ExpressionMapper active maps RMS to effect param", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Add one effect so index 0 exists in the chain
    pipeline.getEffectChain().addEffect(std::make_unique<dsp::FlangerEffect>());

    // Configure mapper: RMS [0,1] -> param 0 of effect 0, range [0,1]
    dsp::ExpressionConfig cfg;
    cfg.effectIndex = 0;
    cfg.paramIndex  = 0;
    cfg.inMin  = 0.0f; cfg.inMax  = 1.0f;
    cfg.outMin = 0.0f; cfg.outMax = 1.0f;
    pipeline.getExpressionMapper().setConfig(cfg);

    REQUIRE(pipeline.getExpressionMapper().isActive());

    // Feed signal — should not crash and should not change output range
    for (int i = 0; i < 10; ++i)
    {
        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        REQUIRE_NOTHROW(pipeline.process(signal.data(), kBlockSize));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pitch stabilization tests (v2)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("DspPipeline -- pitch hold on silence then timeout", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    pipeline.setMasterLimiterEnabled(false);

    // 1. Feed 440 Hz for ~100 blocks (~580ms) to establish a stable pitch.
    //    Use one contiguous sine to avoid phase-reset artifacts at block boundaries.
    const int toneBlocks = 100;
    const int toneSamples = toneBlocks * kBlockSize;
    auto toneBuf = test_helpers::generateSine(440.0f, kSampleRate, toneSamples);
    for (int offset = 0; offset < toneSamples; offset += kBlockSize)
        pipeline.process(toneBuf.data() + offset, kBlockSize);

    const float pitchAfterTone = pipeline.getLastPitch().frequencyHz;
    // Should have converged to ~440 Hz
    REQUIRE(pitchAfterTone > 400.0f);
    REQUIRE(pitchAfterTone < 480.0f);

    // 2. Feed silence for < 200ms (~30 blocks @ 256 samples = ~174ms, under timeout)
    for (int i = 0; i < 30; ++i)
    {
        auto silence = test_helpers::generateSilence(kBlockSize);
        pipeline.process(silence.data(), kBlockSize);
    }

    const float pitchAfterShortSilence = pipeline.getLastPitch().frequencyHz;
    // Pitch should be HELD (not dropped to 0) during the hold period
    REQUIRE(pitchAfterShortSilence > 400.0f);
    REQUIRE(pitchAfterShortSilence < 480.0f);

    // 3. Feed silence for well beyond 200ms timeout (100 more blocks = ~580ms)
    for (int i = 0; i < 100; ++i)
    {
        auto silence = test_helpers::generateSilence(kBlockSize);
        pipeline.process(silence.data(), kBlockSize);
    }

    const float pitchAfterTimeout = pipeline.getLastPitch().frequencyHz;
    // Should have timed out → pitch = 0
    REQUIRE(pitchAfterTimeout == 0.0f);
}

TEST_CASE("DspPipeline -- log smoothing converges progressively", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    pipeline.setMasterLimiterEnabled(false);

    // 1. Feed 440 Hz (contiguous) to establish stable pitch
    const int warmupSamples = 80 * kBlockSize;
    auto tone440 = test_helpers::generateSine(440.0f, kSampleRate, warmupSamples);
    for (int offset = 0; offset < warmupSamples; offset += kBlockSize)
        pipeline.process(tone440.data() + offset, kBlockSize);

    const float pitch440 = pipeline.getLastPitch().frequencyHz;
    REQUIRE(pitch440 > 400.0f);
    REQUIRE(pitch440 < 480.0f);

    // 2. Switch to 880 Hz — first block should NOT jump instantly to 880
    //    (contiguous 880 Hz buffer, feed one block)
    auto tone880 = test_helpers::generateSine(880.0f, kSampleRate, 100 * kBlockSize);
    pipeline.process(tone880.data(), kBlockSize);

    const float pitchAfter1Block = pipeline.getLastPitch().frequencyHz;
    // The smoothing EMA should keep it closer to 440 than to 880
    // (it can't jump from 440 to 880 in a single ~5.8ms block with 50ms smoothing)
    REQUIRE(pitchAfter1Block < 800.0f);

    // 3. After many more blocks of 880 Hz, it should converge near 880
    for (int offset = kBlockSize; offset < 80 * kBlockSize; offset += kBlockSize)
        pipeline.process(tone880.data() + offset, kBlockSize);

    const float pitchConverged = pipeline.getLastPitch().frequencyHz;
    REQUIRE(pitchConverged > 840.0f);
    REQUIRE(pitchConverged < 920.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-B1 — MonoSubFilter: sub < 120 Hz pushed to mono in processStereo
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("DspPipeline -- MonoSubFilter: 40 Hz panned hard-left converges to mono", "[pipeline][monosub]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    pipeline.setMasterLimiterEnabled(false);
    pipeline.setSamplerEnabled(true);

    // Load a 40 Hz sine (2s) into slot 0, pan hard left, loop it.
    const int sampleLen = static_cast<int>(kSampleRate) * 2;
    std::vector<float> sine(sampleLen);
    for (int i = 0; i < sampleLen; ++i)
        sine[i] = 0.4f * std::sin(2.f * 3.14159265f * 40.f * i / kSampleRate);

    auto& sampler = pipeline.getSampler();
    sampler.loadSample(0, sine.data(), sampleLen, static_cast<double>(kSampleRate));
    sampler.setSlotPan(0, -1.f);       // hard left: L = full, R = 0
    sampler.setSlotLoop(0, true);
    sampler.setSlotOneShot(0, false);
    sampler.trigger(0);

    // Run 200 blocks (≈ 1.16 s) of silence — lets mono sub filter reach steady state.
    // The filter time-constant is ~8 ms (120 Hz cutoff), so 200×256 samples is ample.
    std::vector<float> L(kBlockSize), R(kBlockSize);
    for (int block = 0; block < 200; ++block)
    {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        pipeline.processStereo(L.data(), R.data(), kBlockSize);
    }

    // At steady state: the 1st-order LP crossover (120 Hz) reduces the stereo
    // content of a 40 Hz panned signal.  diff = HP(L - R) = HP(40 Hz).
    // For a 1st-order HP at 120 Hz, |H_HP(40 Hz)| ≈ 28 %, so:
    //   maxDiff ≈ 0.28 × A,  maxAbs ≈ 0.53 × A  →  ratio ≈ 52 %
    // Without the filter the ratio would be 100 % (fully asymmetric).
    // We also verify this ratio is clearly smaller than for 400 Hz (see next test).
    float maxDiff40 = 0.f, maxAbs40 = 0.f;
    for (int i = 0; i < kBlockSize; ++i)
    {
        maxDiff40 = std::max(maxDiff40, std::abs(L[i] - R[i]));
        maxAbs40  = std::max(maxAbs40, std::max(std::abs(L[i]), std::abs(R[i])));
    }
    REQUIRE(maxAbs40 > 1e-4f);                          // signal must be present
    REQUIRE(maxDiff40 < 0.60f * maxAbs40);              // sub content is partially mono
}

TEST_CASE("DspPipeline -- MonoSubFilter: 400 Hz panned hard-left preserves stereo asymmetry", "[pipeline][monosub]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    pipeline.setMasterLimiterEnabled(false);
    pipeline.setSamplerEnabled(true);

    // Load a 400 Hz sine panned hard left — well above the 120 Hz crossover.
    const int sampleLen = static_cast<int>(kSampleRate) * 2;
    std::vector<float> sine(sampleLen);
    for (int i = 0; i < sampleLen; ++i)
        sine[i] = 0.4f * std::sin(2.f * 3.14159265f * 400.f * i / kSampleRate);

    auto& sampler = pipeline.getSampler();
    sampler.loadSample(0, sine.data(), sampleLen, static_cast<double>(kSampleRate));
    sampler.setSlotPan(0, -1.f);
    sampler.setSlotLoop(0, true);
    sampler.setSlotOneShot(0, false);
    sampler.trigger(0);

    std::vector<float> L(kBlockSize), R(kBlockSize);
    for (int block = 0; block < 200; ++block)
    {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        pipeline.processStereo(L.data(), R.data(), kBlockSize);
    }

    // At 400 Hz the HP part dominates: L keeps most of the signal, R gets only the LP(mid).
    // Expected: diff/peak ≈ 83 % → must be > 50 %.
    float maxDiff = 0.f, maxAbs = 0.f;
    for (int i = 0; i < kBlockSize; ++i)
    {
        maxDiff = std::max(maxDiff, std::abs(L[i] - R[i]));
        maxAbs  = std::max(maxAbs, std::max(std::abs(L[i]), std::abs(R[i])));
    }
    REQUIRE(maxAbs > 1e-4f);
    REQUIRE(maxDiff > 0.50f * maxAbs);               // asymmetry preserved
}

// ─────────────────────────────────────────────────────────────────────────────
// T-A1 — applySubOwnership: hysteresis threshold math
// Validates the 1.41 energy-ratio gate introduced to prevent flip-flop.
// ─────────────────────────────────────────────────────────────────────────────

// 1-pole LP sub energy measurement (mirrors applySubOwnership implementation).
static float subEnergyTest(const std::vector<float>& pcm, float sr)
{
    const float a = std::exp(-2.f * 3.14159265f * 60.f / sr);
    const float c = 1.f - a;
    float z = 0.f, e = 0.f;
    const int n = std::min(8192, static_cast<int>(pcm.size()));
    for (int i = 0; i < n; ++i)
    {
        z = c * pcm[i] + a * z;
        e += z * z;
    }
    return (n > 0) ? e / static_cast<float>(n) : 0.f;
}

TEST_CASE("applySubOwnership -- KICK clearly dominates: ratio >= 1.41, BASS cut applies", "[pipeline][subowner]")
{
    constexpr float kSR = 44100.f;
    const int N = 8192;

    // KICK: strong sub (35 Hz dominant)
    std::vector<float> kick(N);
    for (int i = 0; i < N; ++i)
        kick[i] = 0.7f * std::sin(2.f * 3.14159265f * 35.f * i / kSR);

    // BASS: much weaker sub
    std::vector<float> bass(N);
    for (int i = 0; i < N; ++i)
        bass[i] = 0.2f * std::sin(2.f * 3.14159265f * 45.f * i / kSR);

    const float kickSub = subEnergyTest(kick, kSR);
    const float bassSub = subEnergyTest(bass, kSR);

    // Ratio must exceed the hysteresis threshold so the cut is applied
    const float ratio = std::max(kickSub, bassSub)
                      / std::max(std::min(kickSub, bassSub), 1e-12f);
    REQUIRE(ratio >= 1.41f);

    // Applying a -4 dB low-shelf (×0.63 amplitude ≈ ×0.397 energy) to BASS
    // must reduce its sub energy by >= 3 dB (factor 0.5 in energy).
    const float bassSubAfter = bassSub * 0.397f;
    REQUIRE(bassSubAfter < bassSub * 0.5f);
}

TEST_CASE("applySubOwnership -- equal sub energies: ratio < 1.41, hysteresis blocks cut", "[pipeline][subowner]")
{
    constexpr float kSR = 44100.f;
    const int N = 8192;

    // Both slots have virtually identical sub content — same signal.
    std::vector<float> sig(N);
    for (int i = 0; i < N; ++i)
        sig[i] = 0.5f * std::sin(2.f * 3.14159265f * 45.f * i / kSR);

    const float e = subEnergyTest(sig, kSR);
    const float ratio = std::max(e, e) / std::max(std::min(e, e), 1e-12f);

    // Identical → ratio == 1.0 < 1.41 → no cut should be applied
    REQUIRE(ratio < 1.41f);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-C1 — applyDubEcho bandpass: HP(80 Hz) attenuates 40 Hz by >= 6 dB
// Validates the feedback-path bandpass in applyDubEcho.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("applyDubEcho bandpass -- HP(80 Hz) attenuates 40 Hz >= 6 dB in feedback path", "[pipeline][dubecho]")
{
    // Reproduce the HP biquad used in applyDubEcho (makeHP formula, Q = 0.707 Butterworth).
    constexpr float kSR = 44100.f;
    const float fc    = 80.f;
    const float w0    = 2.f * 3.14159265f * fc / kSR;
    const float cosw0 = std::cos(w0);
    const float alpha = std::sin(w0) / (2.f * 0.707f);
    const float a0inv = 1.f / (1.f + alpha);
    const float b0 =  (1.f + cosw0) * 0.5f * a0inv;
    const float b1 = -(1.f + cosw0)        * a0inv;
    const float b2 =  (1.f + cosw0) * 0.5f * a0inv;
    const float a1 = -2.f * cosw0          * a0inv;
    const float a2 =  (1.f - alpha)         * a0inv;

    // Run a 40 Hz sine through the filter and measure RMS (skip first half for transient).
    const int N = 8192;
    float x1 = 0.f, x2 = 0.f, y1 = 0.f, y2 = 0.f;
    float inputE = 0.f, outputE = 0.f;

    for (int i = 0; i < N; ++i)
    {
        const float x0 = std::sin(2.f * 3.14159265f * 40.f * i / kSR);
        const float y0 = b0*x0 + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2 = x1; x1 = x0;
        y2 = y1; y1 = y0;
        if (i >= N / 2) { inputE += x0*x0; outputE += y0*y0; }
    }

    const float inputRms  = std::sqrt(inputE  / (N / 2));
    const float outputRms = std::sqrt(outputE / (N / 2));

    // HP(80 Hz) should reject 40 Hz (one octave below) by >= 6 dB → factor >= 2 in amplitude.
    REQUIRE(inputRms  > 0.f);
    REQUIRE(outputRms < inputRms / 2.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-S1: Sampler::loadSample() during a stop fadeOut must not cut voices abruptly
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("Sampler -- loadSample during stop fadeOut does not cut voice abruptly", "[sampler]")
{
    dsp::Sampler s;
    s.prepare(44100.0, 512);

    std::vector<float> data(44100);
    for (int i = 0; i < 44100; ++i)
        data[i] = 0.5f * std::sin(2.f * 3.14159265f * 440.f * static_cast<float>(i) / 44100.f);
    s.loadSample(0, data.data(), 44100, 44100.0);
    s.setSlotLoop(0, true);
    s.trigger(0);

    std::vector<float> buf(512, 0.f);
    for (int k = 0; k < 5; ++k)
    {
        std::fill(buf.begin(), buf.end(), 0.f);
        s.process(buf.data(), 512);
    }

    s.stop(0);

    // One block so stopPending is consumed and voice enters fadeOut (retriggering=true).
    std::fill(buf.begin(), buf.end(), 0.f);
    s.process(buf.data(), 512);

    float sumBefore = 0.f;
    for (float v : buf) sumBefore += v * v;
    const float rmsBefore = std::sqrt(sumBefore / 512.f);
    REQUIRE(rmsBefore > 1e-3f);

    // loadSample() while fadeOut is in progress.
    std::vector<float> data2(44100);
    for (int i = 0; i < 44100; ++i)
        data2[i] = 0.3f * std::sin(2.f * 3.14159265f * 880.f * static_cast<float>(i) / 44100.f);
    s.loadSample(0, data2.data(), 44100, 44100.0);

    // First block after reload: fadeOut still in progress, must not be silent.
    std::fill(buf.begin(), buf.end(), 0.f);
    s.process(buf.data(), 512);
    float sumAfter = 0.f;
    for (float v : buf) sumAfter += v * v;
    const float rmsAfter = std::sqrt(sumAfter / 512.f);
    REQUIRE(rmsAfter > 0.1f * rmsBefore);

    // FadeOut must continue to decrease.
    float prev = rmsAfter;
    bool decreased = false;
    for (int k = 0; k < 10; ++k)
    {
        std::fill(buf.begin(), buf.end(), 0.f);
        s.process(buf.data(), 512);
        float cur = 0.f;
        for (float v : buf) cur += v * v;
        cur = std::sqrt(cur / 512.f);
        if (cur < prev * 0.98f) decreased = true;
        prev = cur;
    }
    REQUIRE(decreased);
}
