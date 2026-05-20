#pragma once

#include <atomic>
#include <vector>
#include <cmath>
#include "DspCommon.h"

namespace dsp {

// Ping-Pong Delay (RT-safe, minimalistic patch implementing a stable API)
class PingPongDelay {
public:
  PingPongDelay();

  // Init delay buffers and state
  void prepare(double sr, int maxBlock) noexcept;
  void reset() noexcept;

  // Public API
  void setBpm(float bpm) noexcept;
  void setEnabled(bool) noexcept;
  void setSend(float) noexcept;
  void setWet(float) noexcept;
  void setFeedback(float) noexcept; // clamp to 0..0.95
  void setTone(float) noexcept;     // 0..1 (dub-friendly)
  void setDrive(float) noexcept;    // 0..1
  void setDiv(int div) noexcept;     // store as int (atomic)
  void setFreeze(bool) noexcept;     // optional freeze state for transitions

  float getFeedback() const noexcept { return fb_.load  (std::memory_order_relaxed); }
  float getWet()      const noexcept { return wet_.load (std::memory_order_relaxed); }
  float getTone()     const noexcept { return tone_.load(std::memory_order_relaxed); }
  float getDrive()    const noexcept { return drive_.load(std::memory_order_relaxed); }

  // Core processing: inL/inR are the inputs for this block; outputs written to outL/outR
  void processAdd(const float* inL, const float* inR,
                  float* outL, float* outR, int n) noexcept;

private:
  double sampleRate_ { 44100.0 };
  int maxDelaySamples_ { 0 };
  int writePos_ { 0 };
  std::vector<float> delayL_;
  std::vector<float> delayR_;

  std::atomic<float> bpm_ { 120.0f };
  std::atomic<float> send_ { 0.2f };
  std::atomic<float> wet_  { 0.28f };
  std::atomic<float> fb_   { 0.40f };
  std::atomic<float> tone_ { 0.50f };
  std::atomic<float> drive_ { 0.15f };
  std::atomic<int> div_   { 0 };
  std::atomic<bool> freeze_   { false };
  std::atomic<bool> enabled_  { true };

  // State for simple 1-pole filters (HP/LP) on the output path
  float lpStateL_ { 0.0f }, lpStateR_ { 0.0f };
  float hpStateL_ { 0.0f }, hpStateR_ { 0.0f };
  float aLP_ { 0.0f }, aHP_ { 0.0f };
  float dpSmooth_ { 0.0f };
  // Helpers
  static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
  static float clamp01_amed(float v) { return clamp01(v); }
};

} // namespace dsp
