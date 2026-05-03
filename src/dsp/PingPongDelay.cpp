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
  if (freeze_.load(std::memory_order_relaxed)) {
    // Freeze state: bypass processing to avoid glitches during scene changes
    for (int i = 0; i < n; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; }
    return;
  }
  if (!enabled_){ for(int i=0;i<n;++i){ outL[i]=inL[i]; outR[i]=inR[i]; } return; }
  // Local copies to avoid read/write hazards if inL/inR = outL/outR
  const float wet = wet_.load();
  const float fb  = fb_.load();
  const float bpm = bpm_.load();
  // Simple fixed delay calculation (quarter-note default)
  float delayBeats = 0.25f; // 1/4 note
  float delaySec = (60.0f / bpm) * delayBeats;
  int delaySamples = std::max(2, std::min(maxDelaySamples_ - 2, static_cast<int>(std::ceil(delaySec * static_cast<float>(sampleRate_)))));
  if (delaySamples <= 0) delaySamples = 1;

  int readPos = writePos_ - delaySamples;
  if (readPos < 0) readPos += maxDelaySamples_;

  for (int i = 0; i < n; ++i)
  {
    float il = inL[i];
    float ir = inR[i];
    float dl = delayL_[readPos];
    float dr = delayR_[readPos];
    float fbL = dr * fb;
    float fbR = dl * fb;
    delayL_[writePos_] = il * wet + fbL;
    delayR_[writePos_] = ir * wet + fbR;
    float oL = il * (1.0f - wet) + dl * wet; // direct + delayed
    float oR = ir * (1.0f - wet) + dr * wet;
    // simple LP/HP path (1-pole) and soft clipping
    // LP
    lpStateL_ = (1.0f - aLP_) * oL + aLP_ * lpStateL_;
    lpStateR_ = (1.0f - aLP_) * oR + aLP_ * lpStateR_;
    float hpL = oL - lpStateL_;
    float hpR = oR - lpStateR_;
    float drive = drive_.load();
    float satL = std::tanh((1.0f + 4.0f * drive) * hpL);
    float satR = std::tanh((1.0f + 4.0f * drive) * hpR);
    // Final LP after saturation
    lpStateL_ = (1.0f - aLP_) * satL + aLP_ * lpStateL_;
    lpStateR_ = (1.0f - aLP_) * satR + aLP_ * lpStateR_;
    outL[i] = lpStateL_;
    outR[i] = lpStateR_;
    writePos_  = (writePos_  + 1) % maxDelaySamples_;
    readPos   = (readPos   + 1) % maxDelaySamples_;
  }
  // end for
}
