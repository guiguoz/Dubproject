#include <catch2/catch_test_macros.hpp>

#ifdef SAXFX_HAS_ONNX

#include "dsp/AiContentClassifier.h"
#include "dsp/OnnxInference.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Path configuration (injected by CMake)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef CLASSIFIER_MODEL_PATH
#define CLASSIFIER_MODEL_PATH "../models/content_classifier.onnx"
#endif
#ifndef CLASSIFIER_NORM_PATH
#define CLASSIFIER_NORM_PATH "../models/content_classifier_norm.bin"
#endif
#ifndef DATASET_DIR
#define DATASET_DIR "../data/dataset"
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Minimal WAV reader (mono 16-bit PCM, no dependencies)
// ─────────────────────────────────────────────────────────────────────────────

static bool readWavMono(const std::string& path,
                        std::vector<float>& samples,
                        double& sampleRateOut)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    // RIFF header
    char riff[4];
    f.read(riff, 4);
    if (std::strncmp(riff, "RIFF", 4) != 0) return false;
    f.ignore(4); // file size
    char wave[4];
    f.read(wave, 4);
    if (std::strncmp(wave, "WAVE", 4) != 0) return false;

    uint32_t sampleRate = 44100;
    uint16_t bitsPerSample = 16;
    uint16_t channels = 1;

    while (f)
    {
        char chunkId[4];
        f.read(chunkId, 4);
        if (!f) break;

        uint32_t chunkSize = 0;
        f.read(reinterpret_cast<char*>(&chunkSize), 4);

        if (std::strncmp(chunkId, "fmt ", 4) == 0)
        {
            f.ignore(2); // audio format
            f.read(reinterpret_cast<char*>(&channels), 2);
            f.read(reinterpret_cast<char*>(&sampleRate), 4);
            f.ignore(4); // byte rate
            f.ignore(2); // block align
            f.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            if (chunkSize > 16) f.ignore(chunkSize - 16);
        }
        else if (std::strncmp(chunkId, "data", 4) == 0)
        {
            const uint32_t bytesPerSample = bitsPerSample / 8;
            const uint32_t numSamples = chunkSize / (bytesPerSample * channels);
            samples.reserve(numSamples);
            for (uint32_t i = 0; i < numSamples; ++i)
            {
                int16_t s = 0;
                f.read(reinterpret_cast<char*>(&s), 2);
                if (channels > 1) f.ignore((channels - 1) * bytesPerSample); // take ch0 only
                samples.push_back(static_cast<float>(s) / 32768.0f);
            }
            break;
        }
        else
        {
            f.ignore(chunkSize);
        }
    }

    sampleRateOut = static_cast<double>(sampleRate);
    return !samples.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AiContentClassifier -- featureSize is 64", "[ai_classifier]")
{
    dsp::AiContentClassifier clf(CLASSIFIER_MODEL_PATH, CLASSIFIER_NORM_PATH);
    REQUIRE(clf.featureSize() == 64);
}

TEST_CASE("AiContentClassifier -- classify silence returns a valid class", "[ai_classifier]")
{
    dsp::AiContentClassifier clf(CLASSIFIER_MODEL_PATH, CLASSIFIER_NORM_PATH);
    std::vector<float> silence(22050, 0.0f);
    // Must not throw and must return a valid enum value
    auto cls = clf.classify(silence, 44100.0);
    // All enum values are 0–7; just check it compiles and runs
    REQUIRE(static_cast<int>(cls) >= 0);
    REQUIRE(static_cast<int>(cls) <= 7);
}

TEST_CASE("AiContentClassifier -- kick_000.wav classified as KICK", "[ai_classifier]")
{
    std::vector<float> samples;
    double sr = 44100.0;
    const std::string path = std::string(DATASET_DIR) + "/KICK/kick_000.wav";
    const bool loaded = readWavMono(path, samples, sr);
    REQUIRE(loaded);
    REQUIRE_FALSE(samples.empty());

    dsp::AiContentClassifier clf(CLASSIFIER_MODEL_PATH, CLASSIFIER_NORM_PATH);
    const auto cls = clf.classify(samples, sr);
    REQUIRE(cls == dsp::AiContentClassifier::ContentType::KICK);
}

TEST_CASE("AiContentClassifier -- hihat_000.wav classified as HIHAT", "[ai_classifier]")
{
    std::vector<float> samples;
    double sr = 44100.0;
    const std::string path = std::string(DATASET_DIR) + "/HIHAT/hihat_000.wav";
    const bool loaded = readWavMono(path, samples, sr);
    REQUIRE(loaded);
    REQUIRE_FALSE(samples.empty());

    dsp::AiContentClassifier clf(CLASSIFIER_MODEL_PATH, CLASSIFIER_NORM_PATH);
    const auto cls = clf.classify(samples, sr);
    REQUIRE(cls == dsp::AiContentClassifier::ContentType::HIHAT);
}

TEST_CASE("AiContentClassifier -- snare_000.wav classified as SNARE", "[ai_classifier]")
{
    std::vector<float> samples;
    double sr = 44100.0;
    const std::string path = std::string(DATASET_DIR) + "/SNARE/snare_000.wav";
    const bool loaded = readWavMono(path, samples, sr);
    REQUIRE(loaded);
    REQUIRE_FALSE(samples.empty());

    dsp::AiContentClassifier clf(CLASSIFIER_MODEL_PATH, CLASSIFIER_NORM_PATH);
    const auto cls = clf.classify(samples, sr);
    REQUIRE(cls == dsp::AiContentClassifier::ContentType::SNARE);
}

TEST_CASE("AiContentClassifier -- classify resamples 22050 Hz input", "[ai_classifier]")
{
    // Feed a kick at 22050 Hz (half sample rate) — must not crash and must resample
    std::vector<float> samples;
    double sr = 44100.0;
    const std::string path = std::string(DATASET_DIR) + "/KICK/kick_000.wav";
    REQUIRE(readWavMono(path, samples, sr));

    // Downsample to 22050 Hz by taking every other sample
    std::vector<float> half;
    half.reserve(samples.size() / 2);
    for (size_t i = 0; i < samples.size(); i += 2)
        half.push_back(samples[i]);

    dsp::AiContentClassifier clf(CLASSIFIER_MODEL_PATH, CLASSIFIER_NORM_PATH);
    REQUIRE_NOTHROW(clf.classify(half, 22050.0));
}

#else // !SAXFX_HAS_ONNX

TEST_CASE("AiContentClassifier -- skipped (ONNX Runtime not enabled)", "[ai_classifier]")
{
    SUCCEED("ONNX Runtime not available — test skipped");
}

#endif // SAXFX_HAS_ONNX
