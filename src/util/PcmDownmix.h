#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace util {

/// Downmix interleaved PCM to mono — matches MainComponent / SmartSamplerEngine (L+R)*0.5.
inline std::vector<float> downmixInterleavedToMono(const float* interleaved,
                                                   int numFrames,
                                                   int numChannels)
{
    if (numFrames <= 0 || numChannels <= 0)
        return {};

    std::vector<float> mono(static_cast<std::size_t>(numFrames), 0.f);
    if (numChannels == 1)
    {
        std::copy(interleaved, interleaved + numFrames, mono.begin());
        return mono;
    }

    for (int i = 0; i < numFrames; ++i)
    {
        const int base = i * numChannels;
        float sum = 0.f;
        for (int ch = 0; ch < numChannels; ++ch)
            sum += interleaved[base + ch];
        mono[static_cast<std::size_t>(i)] = sum * 0.5f;  // stereo: (L+R)*0.5
    }
    return mono;
}

/// Planar channels (JUCE AudioBuffer layout) — same semantics as loadSampleIntoSlot.
inline std::vector<float> downmixPlanarToMono(const float* const* channelData,
                                              int numFrames,
                                              int numChannels)
{
    if (numFrames <= 0 || numChannels <= 0)
        return {};

    std::vector<float> mono(static_cast<std::size_t>(numFrames), 0.f);
    if (numChannels == 1)
    {
        std::copy(channelData[0], channelData[0] + numFrames, mono.begin());
        return mono;
    }

    const float* left  = channelData[0];
    const float* right = channelData[1];
    for (int i = 0; i < numFrames; ++i)
        mono[static_cast<std::size_t>(i)] = (left[i] + right[i]) * 0.5f;

    return mono;
}

/// Trim range — same jlimit rules as MainComponent::loadSampleIntoSlot.
inline std::pair<int, int> trimRange(int numSamples, int trimStart, int trimEnd)
{
    const int start = std::max(0, std::min(numSamples - 1, trimStart));
    const int end     = (trimEnd >= 0)
                        ? std::max(start + 1, std::min(numSamples, trimEnd))
                        : numSamples;
    return { start, end };
}

inline float computeRms(const float* data, int n)
{
    if (n <= 0) return 0.f;
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += static_cast<double>(data[i]) * static_cast<double>(data[i]);
    return static_cast<float>(std::sqrt(sum / static_cast<double>(n)));
}

}  // namespace util
