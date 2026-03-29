#include "KeyDetector.h"

#include <algorithm>
#include <cmath>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// Krumhansl-Schmuckler profiles (same as ScaleHarmonizer)
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

static float normVec(const float* v, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; ++i) s += v[i] * v[i];
    return std::sqrt(s);
}

static float dotVec(const float* a, const float* b, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// goertzel — energy at a single frequency (single-pass, O(N))
// ─────────────────────────────────────────────────────────────────────────────
float KeyDetector::goertzel(const float* buf, int n, double freq, double fs) noexcept
{
    const double omega = 2.0 * 3.14159265358979323846 * freq / fs;
    const double coeff = 2.0 * std::cos(omega);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double s0 = static_cast<double>(buf[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    // Normalise by frame length so shorter frames give comparable values
    return static_cast<float>(power / static_cast<double>(n));
}

// ─────────────────────────────────────────────────────────────────────────────
// detectFromChroma — K-S correlation
// ─────────────────────────────────────────────────────────────────────────────
KeyResult KeyDetector::detectFromChroma(const std::array<float, 12>& chroma) noexcept
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

        const float majScore = dotVec(chroma.data(), rotMaj, 12) / (cn * majN);
        const float minScore = dotVec(chroma.data(), rotMin, 12) / (cn * minN);

        if (majScore > bestScore)
        {
            bestScore = majScore;
            best.key  = key;
            best.mode = 0;
            for (int i = 0; i < 7; ++i)
                best.scaleDegrees[i] = (key + kMajorScale[i]) % 12;
        }
        if (minScore > bestScore)
        {
            bestScore = minScore;
            best.key  = key;
            best.mode = 1;
            for (int i = 0; i < 7; ++i)
                best.scaleDegrees[i] = (key + kMinorScale[i]) % 12;
        }
    }
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
// processFrame — accumulate 4096-sample frame into chromagram
// ─────────────────────────────────────────────────────────────────────────────
void KeyDetector::processFrame() noexcept
{
    for (int semitone = 0; semitone < kNumOctaves * 12; ++semitone)
    {
        const int    midiNote = kBaseNote + semitone;
        const double freq     = 440.0 * std::pow(2.0, (midiNote - 69) / 12.0);
        const int    pc       = semitone % 12;
        chroma_[static_cast<std::size_t>(pc)] +=
            goertzel(frame_.data(), kFrameSize, freq, sampleRate_);
    }
    ++frameCount_;

    if (frameCount_ >= kMinFrames)
        result_ = detectFromChroma(chroma_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
void KeyDetector::reset() noexcept
{
    frameFill_  = 0;
    frameCount_ = 0;
    chroma_.fill(0.0f);
    frame_.fill(0.0f);
    result_ = {};
}

void KeyDetector::process(const float* buf, int numSamples, double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

    int offset = 0;
    while (offset < numSamples)
    {
        const int needed = kFrameSize - frameFill_;
        const int avail  = numSamples - offset;
        const int copy   = std::min(needed, avail);

        std::copy(buf + offset, buf + offset + copy, frame_.data() + frameFill_);
        frameFill_ += copy;
        offset     += copy;

        if (frameFill_ == kFrameSize)
        {
            processFrame();
            frameFill_ = 0;
        }
    }
}

KeyResult KeyDetector::getResult() const noexcept
{
    return result_;
}

} // namespace dsp
