#include "ScaleHarmonizer.h"

#include <algorithm>
#include <cmath>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// Krumhansl-Schmuckler key profiles
// ─────────────────────────────────────────────────────────────────────────────
static constexpr float kMajorProfile[12] = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
    2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
};
static constexpr float kMinorProfile[12] = {
    6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
    2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
};

static constexpr int kMajorScale[7] = { 0, 2, 4, 5, 7, 9, 11 };
static constexpr int kMinorScale[7] = { 0, 2, 3, 5, 7, 8, 10 };

// ─────────────────────────────────────────────────────────────────────────────
// Static helpers
// ─────────────────────────────────────────────────────────────────────────────
static float normVec(const float* v, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; ++i) s += v[i] * v[i];
    return std::sqrt(s);
}

static float dot(const float* a, const float* b, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// detectFromChroma — Krumhansl-Schmuckler correlation
// ─────────────────────────────────────────────────────────────────────────────
KeyResult ScaleHarmonizer::detectFromChroma(const std::array<float, 12>& chroma) noexcept
{
    const float cn = normVec(chroma.data(), 12);
    if (cn < 1e-6f)
        return {}; // unknown

    const float majN = normVec(kMajorProfile, 12);
    const float minN = normVec(kMinorProfile, 12);

    float     bestScore = -1e9f;
    KeyResult best;

    for (int key = 0; key < 12; ++key)
    {
        float rotMaj[12], rotMin[12];
        for (int i = 0; i < 12; ++i)
        {
            rotMaj[i] = kMajorProfile[(i - key + 12) % 12];
            rotMin[i] = kMinorProfile[(i - key + 12) % 12];
        }

        const float majScore = dot(chroma.data(), rotMaj, 12) / (cn * majN);
        const float minScore = dot(chroma.data(), rotMin, 12) / (cn * minN);

        if (majScore > bestScore)
        {
            bestScore  = majScore;
            best.key   = key;
            best.mode  = 0;
            for (int i = 0; i < 7; ++i)
                best.scaleDegrees[i] = (key + kMajorScale[i]) % 12;
        }
        if (minScore > bestScore)
        {
            bestScore  = minScore;
            best.key   = key;
            best.mode  = 1;
            for (int i = 0; i < 7; ++i)
                best.scaleDegrees[i] = (key + kMinorScale[i]) % 12;
        }
    }
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
// pitchToClass
// ─────────────────────────────────────────────────────────────────────────────
int ScaleHarmonizer::pitchToClass(float pitchHz) noexcept
{
    const float midi = 69.0f + 12.0f * std::log2(pitchHz / 440.0f);
    return ((static_cast<int>(std::round(midi)) % 12) + 12) % 12;
}

// ─────────────────────────────────────────────────────────────────────────────
// pushNote — audio thread (non-blocking)
// ─────────────────────────────────────────────────────────────────────────────
void ScaleHarmonizer::pushNote(float pitchHz, float confidence) noexcept
{
    if (confidence < 0.4f || pitchHz < 60.0f)
        return;

    if (histMutex_.try_lock())
    {
        history_[histHead_] = { pitchToClass(pitchHz), confidence };
        histHead_            = (histHead_ + 1) % kHistSize;
        if (histCount_ < kHistSize)
            ++histCount_;
        histMutex_.unlock();
    }
    // If lock unavailable, note is silently dropped (timer holds for < 1 ms)
}

// ─────────────────────────────────────────────────────────────────────────────
// updateScale — timer thread
// ─────────────────────────────────────────────────────────────────────────────
KeyResult ScaleHarmonizer::updateScale() noexcept
{
    // Build chromagram from note history
    std::array<float, 12> chroma{};
    {
        std::lock_guard<std::mutex> lk(histMutex_);
        for (int i = 0; i < histCount_; ++i)
            chroma[static_cast<std::size_t>(history_[i].pitchClass)] += history_[i].confidence;
    }

    const KeyResult result = detectFromChroma(chroma);

    if (result.key >= 0)
    {
        key_.store(result.key,  std::memory_order_relaxed);
        mode_.store(result.mode, std::memory_order_relaxed);

        // Intervals: major → 3rd=+4st, 5th=+7st; minor → 3rd=+3st, 5th=+7st
        intervalThird_.store(result.mode == 0 ? 4 : 3, std::memory_order_relaxed);
        intervalFifth_.store(7, std::memory_order_relaxed);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Getters
// ─────────────────────────────────────────────────────────────────────────────
KeyResult ScaleHarmonizer::getKey() const noexcept
{
    KeyResult r;
    r.key  = key_.load(std::memory_order_relaxed);
    r.mode = mode_.load(std::memory_order_relaxed);
    return r;
}

std::array<int, 2> ScaleHarmonizer::getIntervals() const noexcept
{
    return { intervalThird_.load(std::memory_order_relaxed),
             intervalFifth_.load(std::memory_order_relaxed) };
}

void ScaleHarmonizer::reset() noexcept
{
    std::lock_guard<std::mutex> lk(histMutex_);
    histHead_  = 0;
    histCount_ = 0;
    key_.store(-1,  std::memory_order_relaxed);
    mode_.store(0,  std::memory_order_relaxed);
    intervalThird_.store(4, std::memory_order_relaxed);
    intervalFifth_.store(7, std::memory_order_relaxed);
}

} // namespace dsp
