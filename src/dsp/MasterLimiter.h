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

        const float invT = 1.0f / threshold_;
        for (int i = 0; i < numSamples; ++i)
        {
            // tanh soft-clipper: unity gain for small signals, smooth saturation above threshold.
            // tanh(x/T)*T ≈ x for |x| << T, saturates to ±T for |x| >> T.
            // No small-signal boost (unlike cubic * 1.5 which boosted everything by 50%).
            buffer[i] = std::tanh(buffer[i] * invT) * threshold_;
        }
    }

private:
    float threshold_ = 0.95f; // approx -0.4 dBFS
    bool  enabled_   = true;
};

} // namespace dsp
