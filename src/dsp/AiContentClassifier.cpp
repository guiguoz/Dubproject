#include "AiContentClassifier.h"

#include <cmath>
#include <stdexcept>
#include <algorithm>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace dsp {

AiContentClassifier::AiContentClassifier(const std::string& modelPath,
                                         const std::string& normPath)
    : model_(modelPath)
{
    // Load normalization parameters (64 means and 64 stds)
    std::ifstream normFile(normPath, std::ios::binary);
    if (!normFile) {
        throw std::runtime_error("Could not open normalization file: " + normPath);
    }
    means_.resize(64);
    stds_.resize(64);
    normFile.read(reinterpret_cast<char*>(means_.data()), 64 * sizeof(float));
    normFile.read(reinterpret_cast<char*>(stds_.data()), 64 * sizeof(float));
    if (!normFile) {
        throw std::runtime_error("Could not read normalization data from: " + normPath);
    }
}

std::vector<float> AiContentClassifier::padOrCrop(const std::vector<float>& samples, size_t targetSize)
{
    if (samples.size() >= targetSize) {
        return std::vector<float>(samples.begin(), samples.begin() + targetSize);
    }
    std::vector<float> result(samples);
    result.resize(targetSize, 0.0f);
    return result;
}

std::vector<float> AiContentClassifier::extractFeatures(const std::vector<float>& pcm)
{
    // This function is a direct port of the Python feature extraction in train_classifier.py
    // We'll implement the same 64-feature extraction.

    const size_t n = pcm.size();
    if (n == 0) {
        return std::vector<float>(64, 0.0f);
    }

    // Basic stats
    float peak = 0.0f;
    float sumSquares = 0.0f;
    for (float s : pcm) {
        float absS = std::abs(s);
        if (absS > peak) peak = absS;
        sumSquares += s * s;
    }
    const float rms = std::sqrt(sumSquares / static_cast<float>(n));
    const float crest = (rms > 1e-8f) ? peak / rms : 1.0f;

    // Zero crossing rate
    size_t zcrCount = 0;
    for (size_t i = 1; i < n; ++i) {
        if (pcm[i] * pcm[i-1] < 0.0f) {
            ++zcrCount;
        }
    }
    const float zcr = static_cast<float>(zcrCount) / static_cast<float>(n);

    // Attack time (samples to reach peak)
    size_t peakIdx = 0;
    float peakVal = 0.0f;
    const size_t attackLookback = std::min(n, static_cast<size_t>(4410)); // first 100ms
    for (size_t i = 0; i < attackLookback; ++i) {
        float absS = std::abs(pcm[i]);
        if (absS > peakVal) {
            peakVal = absS;
            peakIdx = i;
        }
    }
    const float attackRatio = static_cast<float>(peakIdx) / 44100.0f; // in seconds

    // Duration above threshold
    const float thresh = peak * 0.1f;
    size_t aboveCount = 0;
    for (float s : pcm) {
        if (std::abs(s) > thresh) {
            ++aboveCount;
        }
    }
    const float above = static_cast<float>(aboveCount) / static_cast<float>(n);

    // Band energies via simple 1st-order LP cascade
    // We'll replicate the Python code exactly.
    auto alpha = [](float fc, float sr) -> float {
        const float t = 2.0f * static_cast<float>(M_PI) * fc / sr;
        return t / (t + 1.0f);
    };

    const float sr = 44100.0f; // We assume 44100 Hz for feature extraction (as in training)
    const std::vector<float> bands = {150.0f, 500.0f, 1500.0f, 4000.0f, 8000.0f};
    std::vector<float> lpStates(bands.size(), 0.0f);
    std::vector<float> bandEnergy(bands.size() + 1, 0.0f); // +1 for highest band
    float totalEnergy = 0.0f;

    for (float s : pcm) {
        float prev = 0.0f;
        for (size_t b = 0; b < bands.size(); ++b) {
            const float a = alpha(bands[b], sr);
            lpStates[b] = a * s + (1.0f - a) * lpStates[b];
            const float diff = lpStates[b] - prev;
            bandEnergy[b] += diff * diff;
            prev = lpStates[b];
        }
        // Highest band
        const float diff = s - prev;
        bandEnergy.back() += diff * diff;
        totalEnergy += s * s;
    }

    if (totalEnergy < 1e-10f) {
        totalEnergy = 1e-10f;
    }
    std::vector<float> bandRatios;
    for (float e : bandEnergy) {
        bandRatios.push_back(e / totalEnergy);
    }

    // Temporal envelope: split into 8 segments, compute RMS of each
    const size_t segCount = 8;
    const size_t segSize = n / segCount;
    std::vector<float> segRms;
    for (size_t seg = 0; seg < segCount; ++seg) {
        const size_t start = seg * segSize;
        const size_t end = start + segSize;
        float segSumSquares = 0.0f;
        for (size_t i = start; i < std::min(end, n); ++i) {
            segSumSquares += pcm[i] * pcm[i];
        }
        const float segSizeF = static_cast<float>(std::min(end, n) - start);
        segRms.push_back(segSizeF > 0.0f ? std::sqrt(segSumSquares / segSizeF) : 0.0f);
    }

    // Assemble feature vector
    std::vector<float> features;
    features.reserve(6 + bandRatios.size() + segRms.size());
    features.push_back(rms);
    features.push_back(peak);
    features.push_back(crest);
    features.push_back(zcr);
    features.push_back(attackRatio);
    features.push_back(above);
    features.insert(features.end(), bandRatios.begin(), bandRatios.end());
    features.insert(features.end(), segRms.begin(), segRms.end());

    // Pad or crop to 64 features
    if (features.size() > 64) {
        features.resize(64);
    } else {
        while (features.size() < 64) {
            features.push_back(0.0f);
        }
    }

    return features;
}

void AiContentClassifier::normalizeFeatures(std::vector<float>& features)
{
    for (size_t i = 0; i < features.size(); ++i) {
        features[i] = (features[i] - means_[i]) / stds_[i];
    }
}

AiContentClassifier::ContentType AiContentClassifier::classify(const std::vector<float>& pcm, double sampleRate)
{
    // We expect the input to be at 44100 Hz. If not, we resample? 
    // For simplicity, we assume 44100 Hz as in training.
    // In a real system, we might resample, but for now we'll just use the samples as-is.
    // However, note that the feature extraction uses a fixed sample rate of 44100 Hz.
    // If the sample rate is different, we should resample to 44100 Hz.
    // But the task says the model expects 0.5s at 44100 Hz, so we'll resample if needed.

    // For now, we'll assume the input is at 44100 Hz. If not, we return OTHER.
    // Alternatively, we can resample using a simple linear interpolation.
    // Given the complexity, and since the task is about integrating the model, 
    // we'll assume the input is at 44100 Hz. In the SmartSamplerEngine, we control the sample rate.

    // However, to be safe, let's resample to 44100 Hz if the sample rate is different.
    // We'll use a simple linear interpolation for resampling.

    const double targetSampleRate = 44100.0;
    std::vector<float> resampledPcm;
    if (sampleRate != targetSampleRate) {
        // Linear interpolation resampling
        const double ratio = targetSampleRate / sampleRate;
        const size_t newSize = static_cast<size_t>(std::round(pcm.size() * ratio));
        resampledPcm.resize(newSize);
        for (size_t i = 0; i < newSize; ++i) {
            const double srcPos = i / ratio;
            const size_t idx0 = static_cast<size_t>(std::floor(srcPos));
            const size_t idx1 = std::min(idx0 + 1, pcm.size() - 1);
            const float t = static_cast<float>(srcPos - static_cast<double>(idx0));
            resampledPcm[i] = pcm[idx0] * (1.0f - t) + pcm[idx1] * t;
        }
    } else {
        resampledPcm = pcm;
    }

    // Step 1: Pad or crop to 0.5s (22050 samples at 44100 Hz)
    const size_t targetLen = 22050;
    std::vector<float> padded = padOrCrop(resampledPcm, targetLen);

    // Step 2: Extract features
    std::vector<float> features = extractFeatures(padded);

    // Step 3: Normalize features
    normalizeFeatures(features);

    // Step 4: Run inference
    std::vector<float> logits = model_.run(features);

    // Step 5: Get the class with the highest logit
    size_t bestIdx = 0;
    float bestLogit = logits[0];
    for (size_t i = 1; i < logits.size(); ++i) {
        if (logits[i] > bestLogit) {
            bestLogit = logits[i];
            bestIdx = i;
        }
    }

    // Map index to ContentType
    switch (bestIdx) {
        case 0: return ContentType::KICK;
        case 1: return ContentType::SNARE;
        case 2: return ContentType::HIHAT;
        case 3: return ContentType::BASS;
        case 4: return ContentType::SYNTH;
        case 5: return ContentType::PAD;
        case 6: return ContentType::PERC;
        case 7: return ContentType::OTHER;
        default: return ContentType::OTHER; // Should not happen
    }
}

} // namespace dsp