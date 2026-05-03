#include "PingPongDelay.h"
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <algorithm>

using namespace dsp;

PingPongDelay::PingPongDelay()
{
  // defaults set in prepare(); keep constructor lightweight
}

void PingPongDelay::prepare(double sr, int /*maxBlock*/) noexcept
{
  sampleRate_ = sr;
  // 5 seconds buffer by default
  maxDelaySamples_ = static_cast<int>(std::ceil(sr * 5.0));
  if (maxDelaySamples_ < 8) maxDelaySamples_ = 8;
  delayL_.assign(maxDelaySamples_, 0.0f);
  delayR_.assign(maxDelaySamples_, 0.0f);
  writePos_ = 0;
  // reset filters state
  lpStateL_ = lpStateR_ = 0.f;
  hpStateL_ = hpStateR_ = 0.f;
  aLP_ = 0.0f; aHP_ = 0.0f;
  // initialize coefficients from current tone mapping
  const float tRaw = tone_.load();
  float t = tRaw < 0.0f ? 0.0f : (tRaw > 1.0f ? 1.0f : tRaw);
  const float t2 = t * t;
  const float lpHz = 1800.0f * (1.0f - t2) + 8000.0f * t2;
  const float hpHz = 90.0f * (1.0f - t2) + 40.0f * t2;
  aLP_ = static_cast<float>(std::exp(-2.0f * M_PI * lpHz / static_cast<float>(sampleRate_)));
  aHP_ = static_cast<float>(std::exp(-2.0f * M_PI * hpHz / static_cast<float>(sampleRate_)));
  // removed duplicate coefficient initialization
}

void PingPongDelay::reset() noexcept
{
  std::fill(delayL_.begin(), delayL_.end(), 0.0f);
  std::fill(delayR_.begin(), delayR_.end(), 0.0f);
  writePos_ = 0;
  lpStateL_ = lpStateR_ = 0.f;
  hpStateL_ = hpStateR_ = 0.f;
  enabled_ = true;
  bpm_.store(120.0f);
  send_.store(0.2f);
  wet_.store(0.28f);
  fb_.store(0.40f);
  tone_.store(0.50f);
  drive_.store(0.15f);
  div_.store(0);
  dpSmooth_ = 0.0f;
}

void PingPongDelay::setBpm(float bpm) noexcept { bpm_.store(bpm); }
void PingPongDelay::setEnabled(bool e) noexcept { enabled_ = e; }
void PingPongDelay::setSend(float s) noexcept { send_.store(clamp01(s)); }
void PingPongDelay::setWet(float w) noexcept { wet_.store(clamp01(w)); }
void PingPongDelay::setFeedback(float f) noexcept { fb_.store(std::min(0.95f, std::max(0.0f, f))); }
void PingPongDelay::setTone(float t) noexcept { tone_.store(clamp01(t)); }
void PingPongDelay::setDrive(float d) noexcept { drive_.store(clamp01(d)); }
void PingPongDelay::setDiv(int div) noexcept { div_.store(div); }
void PingPongDelay::setFreeze(bool f) noexcept { freeze_.store(f); }

void PingPongDelay::processAdd(const float* inL, const float* inR,
                              float* outL, float* outR, int n) noexcept
{
  // Bypass: leave outL/outR untouched (they already hold the full main mix)
  if (!enabled_ || freeze_.load(std::memory_order_relaxed))
    return;

  const float wet   = wet_.load();
  const float fb    = fb_.load();
  const float bpm   = bpm_.load();
  const float drive = drive_.load();

  const float delaySec = (60.0f / bpm) * 0.25f; // quarter-note
  const int delaySamples = std::max(2, std::min(maxDelaySamples_ - 2,
      static_cast<int>(std::ceil(delaySec * static_cast<float>(sampleRate_)))));

  int readPos = writePos_ - delaySamples;
  if (readPos < 0) readPos += maxDelaySamples_;

  for (int i = 0; i < n; ++i)
  {
    const float il = inL[i];
    const float ir = inR[i];
    const float dl = delayL_[readPos];
    const float dr = delayR_[readPos];

    // Ping-pong: write input + cross-feedback into delay lines
    delayL_[writePos_] = il * wet + dr * fb;
    delayR_[writePos_] = ir * wet + dl * fb;

    // Wet only: dry is already in outL/outR — applying filters to dl/dr only
    lpStateL_ = (1.0f - aLP_) * dl + aLP_ * lpStateL_;
    lpStateR_ = (1.0f - aLP_) * dr + aLP_ * lpStateR_;
    const float hpL = dl - lpStateL_;
    const float hpR = dr - lpStateR_;
    const float satL = std::tanh((1.0f + 4.0f * drive) * hpL);
    const float satR = std::tanh((1.0f + 4.0f * drive) * hpR);
    lpStateL_ = (1.0f - aLP_) * satL + aLP_ * lpStateL_;
    lpStateR_ = (1.0f - aLP_) * satR + aLP_ * lpStateR_;

    // ADD to main mix — never overwrite
    outL[i] += lpStateL_ * wet;
    outR[i] += lpStateR_ * wet;

    writePos_ = (writePos_ + 1) % maxDelaySamples_;
    readPos   = (readPos   + 1) % maxDelaySamples_;
  }
}
