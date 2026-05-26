#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "dsp/Sampler.h"
#include "util/PcmDownmix.h"
#include "TestHelpers.h"

using Catch::Matchers::WithinAbs;

namespace {

struct WavData
{
    int sampleRate = 0;
    int numChannels = 0;
    std::vector<float> interleaved;  // frame-major
};

bool readWavFile(const std::string& path, WavData& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    char riff[4];
    f.read(riff, 4);
    if (std::string(riff, 4) != "RIFF") return false;

    uint32_t chunkSize = 0;
    f.read(reinterpret_cast<char*>(&chunkSize), 4);

    char wave[4];
    f.read(wave, 4);
    if (std::string(wave, 4) != "WAVE") return false;

    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    std::vector<uint8_t> pcmBytes;

    while (f && !f.eof())
    {
        char id[4];
        f.read(id, 4);
        if (!f) break;

        uint32_t size = 0;
        f.read(reinterpret_cast<char*>(&size), 4);
        if (!f) break;

        const std::string chunkId(id, 4);
        if (chunkId == "fmt ")
        {
            if (size < 16) return false;
            f.read(reinterpret_cast<char*>(&audioFormat), 2);
            f.read(reinterpret_cast<char*>(&numChannels), 2);
            f.read(reinterpret_cast<char*>(&sampleRate), 4);
            uint32_t byteRate = 0;
            uint16_t blockAlign = 0;
            f.read(reinterpret_cast<char*>(&byteRate), 4);
            f.read(reinterpret_cast<char*>(&blockAlign), 2);
            f.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            if (size > 16)
                f.seekg(static_cast<std::streamoff>(size - 16), std::ios::cur);
        }
        else if (chunkId == "data")
        {
            pcmBytes.resize(size);
            f.read(reinterpret_cast<char*>(pcmBytes.data()),
                   static_cast<std::streamsize>(size));
        }
        else
        {
            f.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        }
    }

    if (audioFormat != 1 || bitsPerSample != 16 || numChannels < 1 || sampleRate == 0
        || pcmBytes.empty())
        return false;

    const int numFrames = static_cast<int>(pcmBytes.size())
                          / (numChannels * (bitsPerSample / 8));
    if (numFrames <= 0) return false;

    out.sampleRate   = static_cast<int>(sampleRate);
    out.numChannels  = numChannels;
    out.interleaved.resize(static_cast<std::size_t>(numFrames * numChannels));

    const auto* samples = reinterpret_cast<const int16_t*>(pcmBytes.data());
    for (int i = 0; i < numFrames * numChannels; ++i)
        out.interleaved[static_cast<std::size_t>(i)] =
            static_cast<float>(samples[i]) / 32768.0f;

    return true;
}

std::string fixturePath(const char* name)
{
#ifdef P0_FIXTURES_DIR
    return std::string(P0_FIXTURES_DIR) + "/" + name;
#else
    return std::string("tests/fixtures/p0/") + name;
#endif
}

}  // namespace

// ── Downmix (équivalent A2 / B3) ─────────────────────────────────────────────

TEST_CASE("P0 downmix — mono fixture unchanged", "[p0]")
{
    WavData wav;
    REQUIRE(readWavFile(fixturePath("mono.wav"), wav));
    REQUIRE(wav.numChannels == 1);

    const auto mono = util::downmixInterleavedToMono(wav.interleaved.data(),
                                                     static_cast<int>(wav.interleaved.size()),
                                                     wav.numChannels);
    REQUIRE(static_cast<int>(mono.size()) == static_cast<int>(wav.interleaved.size()));

    const float rmsIn  = util::computeRms(wav.interleaved.data(),
                                            static_cast<int>(wav.interleaved.size()));
    const float rmsOut = util::computeRms(mono.data(), static_cast<int>(mono.size()));
    REQUIRE_THAT(rmsOut, WithinAbs(rmsIn, 1e-5f));
}

TEST_CASE("P0 downmix — stereo_lr is (L+R)*0.5 not left-only", "[p0]")
{
    WavData wav;
    REQUIRE(readWavFile(fixturePath("stereo_lr.wav"), wav));
    REQUIRE(wav.numChannels == 2);

    const int frames = static_cast<int>(wav.interleaved.size()) / wav.numChannels;
    const auto mono  = util::downmixInterleavedToMono(wav.interleaved.data(), frames,
                                                     wav.numChannels);

    std::vector<float> leftOnly(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
        leftOnly[static_cast<std::size_t>(i)] = wav.interleaved[static_cast<std::size_t>(i * 2)];

    const float rmsCorrect = util::computeRms(mono.data(), frames);
    const float rmsBug     = util::computeRms(leftOnly.data(), frames);

    REQUIRE(rmsCorrect > 0.01f);
    REQUIRE_THAT(rmsCorrect, WithinAbs(rmsBug * 0.5f, 0.02f));
}

TEST_CASE("P0 downmix — stereo_full in-phase matches mono amplitude", "[p0]")
{
    WavData wav;
    REQUIRE(readWavFile(fixturePath("stereo_full.wav"), wav));
    REQUIRE(wav.numChannels == 2);

    const int frames = static_cast<int>(wav.interleaved.size()) / wav.numChannels;
    const auto mono  = util::downmixInterleavedToMono(wav.interleaved.data(), frames,
                                                     wav.numChannels);

    WavData monoRef;
    REQUIRE(readWavFile(fixturePath("mono.wav"), monoRef));

    const float rmsFull = util::computeRms(mono.data(), frames);
    const float rmsMono = util::computeRms(monoRef.interleaved.data(),
                                           static_cast<int>(monoRef.interleaved.size()));

    // stereo_full: L=R=0.6*sin → downmix 0.6*sin ; mono.wav: 0.8*sin — ratio ~0.75
    REQUIRE(rmsFull > 0.01f);
    REQUIRE(rmsMono > rmsFull);
    REQUIRE(rmsFull / rmsMono > 0.65f);
    REQUIRE(rmsFull / rmsMono < 0.85f);
}

// ── Trim + loadSample (équivalent C1 / loadSampleIntoSlot) ───────────────────

TEST_CASE("P0 trim — slice length matches loadSampleIntoSlot rules", "[p0]")
{
    WavData wav;
    REQUIRE(readWavFile(fixturePath("mono.wav"), wav));
    const int numSamples = static_cast<int>(wav.interleaved.size());

    const auto [start, end] = util::trimRange(numSamples, 500, 2500);
    REQUIRE(end - start == 2000);

    const auto [s2, e2] = util::trimRange(numSamples, 0, -1);
    REQUIRE(e2 - s2 == numSamples);
}

TEST_CASE("P0 load — trim + loadSample produces audio", "[p0]")
{
    WavData wav;
    REQUIRE(readWavFile(fixturePath("stereo_full.wav"), wav));
    const int frames = static_cast<int>(wav.interleaved.size()) / wav.numChannels;
    auto pcm = util::downmixInterleavedToMono(wav.interleaved.data(), frames,
                                              wav.numChannels);

    const int trimStart = 400;
    const int trimEnd   = 2400;
    const auto [start, end] = util::trimRange(static_cast<int>(pcm.size()), trimStart, trimEnd);
    const int len = end - start;

    dsp::Sampler sampler;
    sampler.prepare(44100.0, 512);
    sampler.loadSample(0, pcm.data() + start, len, static_cast<double>(wav.sampleRate));
    sampler.setSlotOneShot(0, true);
    sampler.trigger(0);

    std::vector<float> buf(512, 0.f);
    sampler.process(buf.data(), 512);
    const float rms = test_helpers::computeRms(buf.data(), 512);
    REQUIRE(rms > 0.01f);
}

// ── Régression: downmix ≠ lecture canal 0 seul sur stereo_lr ───────────────────

TEST_CASE("P0 regression — planar downmix matches interleaved", "[p0]")
{
    WavData wav;
    REQUIRE(readWavFile(fixturePath("stereo_lr.wav"), wav));
    const int frames = static_cast<int>(wav.interleaved.size()) / wav.numChannels;

    const auto fromInterleaved = util::downmixInterleavedToMono(wav.interleaved.data(), frames,
                                                                wav.numChannels);

    std::vector<float> ch0(static_cast<std::size_t>(frames));
    std::vector<float> ch1(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
    {
        ch0[static_cast<std::size_t>(i)] = wav.interleaved[static_cast<std::size_t>(i * 2)];
        ch1[static_cast<std::size_t>(i)] = wav.interleaved[static_cast<std::size_t>(i * 2 + 1)];
    }

    const float* planar[2] = { ch0.data(), ch1.data() };
    const auto fromPlanar = util::downmixPlanarToMono(planar, frames, 2);

    REQUIRE(fromInterleaved.size() == fromPlanar.size());
    for (std::size_t i = 0; i < fromInterleaved.size(); ++i)
        REQUIRE_THAT(fromInterleaved[i], WithinAbs(fromPlanar[i], 1e-6f));
}
