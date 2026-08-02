#include "engine/fx/PingPongDelay.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace engine::fx {

void PingPongDelay::prepare(double sr, int /*maxBlock*/) noexcept
{
    sampleRate_      = sr;
    maxDelaySamples_ = static_cast<int>(std::ceil(sr * 5.0));
    if (maxDelaySamples_ < 8) maxDelaySamples_ = 8;
    delayL_.assign(maxDelaySamples_, 0.f);
    delayR_.assign(maxDelaySamples_, 0.f);
    writePos_ = 0;
    lpStateL_ = lpStateR_ = hpStateL_ = hpStateR_ = 0.f;
    aLP_ = aHP_ = dpSmooth_ = 0.f;

    const float t    = clamp01(tone_.load());
    const float t2   = t * t;
    const float lpHz = 1800.f * (1.f - t2) + 8000.f * t2;
    const float hpHz = 90.f  * (1.f - t2) + 40.f   * t2;
    aLP_ = std::exp(-2.f * static_cast<float>(M_PI) * lpHz / static_cast<float>(sr));
    aHP_ = std::exp(-2.f * static_cast<float>(M_PI) * hpHz / static_cast<float>(sr));
}

void PingPongDelay::reset() noexcept
{
    std::fill(delayL_.begin(), delayL_.end(), 0.f);
    std::fill(delayR_.begin(), delayR_.end(), 0.f);
    writePos_   = 0;
    lpStateL_   = lpStateR_ = hpStateL_ = hpStateR_ = 0.f;
    dpSmooth_   = 0.f;
    enabled_.store(true);
    bpm_.store(120.f); send_.store(0.2f); wet_.store(0.28f);
    fb_.store(0.40f);  tone_.store(0.5f); drive_.store(0.15f);
    div_.store(0);     freeze_.store(false);
}

void PingPongDelay::processAdd(const float* inL, const float* inR,
                               float* outL, float* outR, int n) noexcept
{
    if (!enabled_.load(std::memory_order_relaxed) ||
        freeze_.load(std::memory_order_relaxed))
        return;

    const float wet   = wet_.load();
    const float send  = send_.load();
    const float fb    = fb_.load();
    const float bpm   = bpm_.load();
    const float drive = drive_.load();

    static constexpr float kDivBeats[] = { 0.5f, 1.0f, 2.0f, 4.0f };
    const int   divIdx      = std::clamp(div_.load(), 0, 3);
    const float delaySec    = (60.f / bpm) * kDivBeats[divIdx];
    const int   delaySamples = std::max(2,
        std::min(maxDelaySamples_ - 2,
                 static_cast<int>(std::ceil(delaySec * static_cast<float>(sampleRate_)))));

    int readPos = writePos_ - delaySamples;
    if (readPos < 0) readPos += maxDelaySamples_;

    for (int i = 0; i < n; ++i)
    {
        const float dl = delayL_[readPos];
        const float dr = delayR_[readPos];

        delayL_[writePos_] = inL[i] * send + dr * fb;
        delayR_[writePos_] = inR[i] * send + dl * fb;

        lpStateL_ = (1.f - aLP_) * dl + aLP_ * lpStateL_;
        lpStateR_ = (1.f - aLP_) * dr + aLP_ * lpStateR_;
        const float hpL = dl - lpStateL_;
        const float hpR = dr - lpStateR_;
        const float satL = std::tanh((1.f + 4.f * drive) * hpL);
        const float satR = std::tanh((1.f + 4.f * drive) * hpR);
        lpStateL_ = (1.f - aLP_) * satL + aLP_ * lpStateL_;
        lpStateR_ = (1.f - aLP_) * satR + aLP_ * lpStateR_;

        outL[i] += lpStateL_ * wet;
        outR[i] += lpStateR_ * wet;

        writePos_ = (writePos_ + 1) % maxDelaySamples_;
        readPos   = (readPos   + 1) % maxDelaySamples_;
    }
}

} // namespace engine::fx
