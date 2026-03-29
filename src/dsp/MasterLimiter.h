#pragma once

#include <algorithm>
#include <cmath>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// MasterLimiter
//
// A fast, zero-latency soft-clipper to gracefully round off peaks before 0 dBFS.
// Uses a polynomial cubic approximation for smooth analog-style saturation.
// ─────────────────────────────────────────────────────────────────────────────
class MasterLimiter {
public:
    MasterLimiter() = default;

    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    bool isEnabled() const noexcept { return enabled_; }

    void setThreshold(float thresholdDB) noexcept {
        threshold_ = std::pow(10.0f, thresholdDB / 20.0f);
    }

    void process(float* buffer, int numSamples) noexcept {
        if (!enabled_) return;

        for (int i = 0; i < numSamples; ++i) {
            float x = buffer[i] / threshold_;

            // Fast cubic soft clipper: f(x) = x - x^3 / 3  for |x| <= 1
            if (x < -1.0f) {
                x = -2.0f / 3.0f;
            } else if (x > 1.0f) {
                x = 2.0f / 3.0f;
            } else {
                x = x - (x * x * x) / 3.0f;
            }

            // Restore gain (multiplying by 1.5 restores the 2/3 peak to 1.0)
            buffer[i] = x * threshold_ * 1.5f;
        }
    }

private:
    float threshold_ = 0.95f; // approx -0.4 dBFS
    bool  enabled_   = true;
};

} // namespace dsp
