#pragma once
#include <atomic>
#include <vector>
#include <cmath>
#include <algorithm>

namespace engine::fx {

// Ping-Pong Delay — namespace engine::fx, sans dépendance JUCE.
// API identique + morphing conservé.
class PingPongDelay {
public:
    PingPongDelay() = default;

    void prepare(double sr, int maxBlock = 512) noexcept;
    void reset() noexcept;

    void setBpm(float bpm)       noexcept { bpm_.store(bpm); }
    void setEnabled(bool e)      noexcept { enabled_.store(e); }
    void setSend(float s)        noexcept { send_.store(clamp01(s)); }
    void setWet(float w)         noexcept { wet_.store(clamp01(w)); }
    void setFeedback(float f)    noexcept { fb_.store(std::min(0.95f, std::max(0.0f, f))); }
    void setTone(float t)        noexcept { tone_.store(clamp01(t)); }
    void setDrive(float d)       noexcept { drive_.store(clamp01(d)); }
    void setDiv(int div)         noexcept { div_.store(div); }
    void setFreeze(bool f)       noexcept { freeze_.store(f); }

    float getFeedback() const noexcept { return fb_.load(std::memory_order_relaxed); }
    float getWet()      const noexcept { return wet_.load(std::memory_order_relaxed); }
    float getTone()     const noexcept { return tone_.load(std::memory_order_relaxed); }
    float getDrive()    const noexcept { return drive_.load(std::memory_order_relaxed); }

    // Additif : ajoute le wet au mix déjà dans outL/outR.
    void processAdd(const float* inL, const float* inR,
                    float* outL, float* outR, int n) noexcept;

private:
    double sampleRate_       = 44100.0;
    int    maxDelaySamples_  = 0;
    int    writePos_         = 0;
    std::vector<float> delayL_, delayR_;

    std::atomic<float> bpm_  {120.0f};
    std::atomic<float> send_ {0.2f};
    std::atomic<float> wet_  {0.28f};
    std::atomic<float> fb_   {0.40f};
    std::atomic<float> tone_ {0.50f};
    std::atomic<float> drive_{0.15f};
    std::atomic<int>   div_  {0};
    std::atomic<bool>  freeze_  {false};
    std::atomic<bool>  enabled_ {true};

    float lpStateL_ = 0.f, lpStateR_ = 0.f;
    float hpStateL_ = 0.f, hpStateR_ = 0.f;
    float aLP_ = 0.f, aHP_ = 0.f;
    float dpSmooth_ = 0.f;

    static float clamp01(float v) noexcept {
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }
};

} // namespace engine::fx
