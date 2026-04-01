#pragma once

#include <JuceHeader.h>
#include <cmath>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// AnimatedValue — smooth 0→1 (or any range) interpolator
//
// Usage:
//   AnimatedValue hover{0.f};
//   hover.setTarget(1.f, 150);       // start 150ms animation
//   hover.tick(16);                  // call each frame (dt in ms)
//   float v = hover.get();           // current interpolated value
//
// Easing: cubic-out (fast start, smooth deceleration)
// ─────────────────────────────────────────────────────────────────────────────
class AnimatedValue
{
public:
    explicit AnimatedValue(float init = 0.f) noexcept
        : current_(init), target_(init), start_(init) {}

    // Set a new target; does nothing if already at target (tolerance 0.001)
    void setTarget(float t, int durationMs = 150) noexcept
    {
        if (std::abs(t - target_) < 0.001f) return;
        start_    = current_;
        target_   = t;
        elapsed_  = 0;
        duration_ = durationMs;
        active_   = true;
    }

    // Advance animation by dt milliseconds; returns current value
    float tick(int dt) noexcept
    {
        if (!active_) return current_;
        elapsed_ += dt;
        if (elapsed_ >= duration_)
        {
            current_ = target_;
            active_  = false;
            return current_;
        }
        const float t    = static_cast<float>(elapsed_) / static_cast<float>(duration_);
        const float ease = 1.f - std::pow(1.f - t, 3.f);  // cubic-out
        current_ = start_ + (target_ - start_) * ease;
        return current_;
    }

    float get()      const noexcept { return current_; }
    bool  isActive() const noexcept { return active_;  }

private:
    float current_, target_, start_;
    int   elapsed_  { 0   };
    int   duration_ { 150 };
    bool  active_   { false };
};

} // namespace ui
