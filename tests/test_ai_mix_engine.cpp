#include <catch2/catch_test_macros.hpp>

#ifdef SAXFX_HAS_ONNX

#include "dsp/AiMixEngine.h"
#include "dsp/FeatureExtractor.h"

#include <array>

// ─────────────────────────────────────────────────────────────────────────────
// Path configuration (injected by CMake)
// ─────────────────────────────────────────────────────────────────────────────
#ifndef MIX_MODEL_PATH
#define MIX_MODEL_PATH "../models/mix_model.onnx"
#endif
#ifndef MIX_MODEL_NORM_PATH
#define MIX_MODEL_NORM_PATH "../models/mix_model_norm.bin"
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static dsp::MixFeatures makeFeatures(float rms, float centroid, float crest,
                                      float low, float mid, float high)
{
    dsp::MixFeatures mf;
    mf.rms              = rms;
    mf.spectralCentroid = centroid;
    mf.crestFactor      = crest;
    mf.lowFrac          = low;
    mf.midFrac          = mid;
    mf.highFrac         = high;
    mf.durationMs       = 300.f;
    return mf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests — 9.4 validation: 4 slots loaded -> coherent EQ+gains, no clipping
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AiMixEngine -- loads model and norm without throw", "[ai_mix_engine]")
{
    REQUIRE_NOTHROW(dsp::AiMixEngine(MIX_MODEL_PATH, MIX_MODEL_NORM_PATH));
    dsp::AiMixEngine eng(MIX_MODEL_PATH, MIX_MODEL_NORM_PATH);
    REQUIRE(eng.isLoaded());
}

TEST_CASE("AiMixEngine -- 4 loaded slots volumes in valid range", "[ai_mix_engine]")
{
    dsp::AiMixEngine eng(MIX_MODEL_PATH, MIX_MODEL_NORM_PATH);

    std::array<dsp::MixFeatures, 8> slots {};
    // Slot 0: KICK-like   (sub-bass, high crest)
    slots[0] = makeFeatures(0.30f, 150.f, 5.0f, 0.50f, 0.30f, 0.20f);
    // Slot 1: SNARE-like  (mid transient)
    slots[1] = makeFeatures(0.25f, 1200.f, 4.0f, 0.20f, 0.45f, 0.35f);
    // Slot 2: HIHAT-like  (high freq, low energy)
    slots[2] = makeFeatures(0.15f, 7000.f, 3.5f, 0.05f, 0.15f, 0.80f);
    // Slot 3: BASS-like   (sub bass, sustained)
    slots[3] = makeFeatures(0.35f, 150.f, 2.0f, 0.65f, 0.25f, 0.10f);
    // Slots 4-7: empty (all zeros = default MixFeatures{})

    const auto decisions = eng.predict(slots);

    // All 8 decisions must be in valid range (no clipping)
    for (int s = 0; s < 8; ++s)
    {
        INFO("slot " << s);
        REQUIRE(decisions[s].volume   >= 0.f);
        REQUIRE(decisions[s].volume   <= 1.f);
        REQUIRE(decisions[s].lowGain  >= -12.f);
        REQUIRE(decisions[s].lowGain  <=  12.f);
        REQUIRE(decisions[s].midGain  >= -12.f);
        REQUIRE(decisions[s].midGain  <=  12.f);
        REQUIRE(decisions[s].highGain >= -12.f);
        REQUIRE(decisions[s].highGain <=  12.f);
    }
}

TEST_CASE("AiMixEngine -- KICK volume higher than HIHAT volume", "[ai_mix_engine]")
{
    // The model should learn that kick is louder than hihat
    dsp::AiMixEngine eng(MIX_MODEL_PATH, MIX_MODEL_NORM_PATH);

    std::array<dsp::MixFeatures, 8> slots {};
    slots[0] = makeFeatures(0.30f, 150.f,  5.0f, 0.50f, 0.30f, 0.20f);  // KICK
    slots[1] = makeFeatures(0.15f, 7000.f, 3.5f, 0.05f, 0.15f, 0.80f);  // HIHAT

    const auto d = eng.predict(slots);
    // targetGainForType: KICK=0.75, HIHAT=0.30 — model should reflect this
    REQUIRE(d[0].volume > d[1].volume);
}

TEST_CASE("AiMixEngine -- all-empty slots return values in range", "[ai_mix_engine]")
{
    dsp::AiMixEngine eng(MIX_MODEL_PATH, MIX_MODEL_NORM_PATH);
    std::array<dsp::MixFeatures, 8> slots {};  // all zero

    const auto decisions = eng.predict(slots);
    for (int s = 0; s < 8; ++s)
    {
        INFO("slot " << s);
        REQUIRE(decisions[s].volume   >= 0.f);
        REQUIRE(decisions[s].volume   <= 1.f);
        REQUIRE(decisions[s].lowGain  >= -12.f);
        REQUIRE(decisions[s].highGain <=  12.f);
    }
}

TEST_CASE("AiMixEngine -- bad model path throws", "[ai_mix_engine]")
{
    REQUIRE_THROWS(dsp::AiMixEngine("/nonexistent/model.onnx",
                                     MIX_MODEL_NORM_PATH));
}

TEST_CASE("AiMixEngine -- bad norm path throws", "[ai_mix_engine]")
{
    REQUIRE_THROWS(dsp::AiMixEngine(MIX_MODEL_PATH,
                                     "/nonexistent/norm.bin"));
}

#else // !SAXFX_HAS_ONNX

TEST_CASE("AiMixEngine -- skipped (ONNX Runtime not enabled)", "[ai_mix_engine]")
{
    SUCCEED("ONNX Runtime not available -- test skipped");
}

#endif // SAXFX_HAS_ONNX
