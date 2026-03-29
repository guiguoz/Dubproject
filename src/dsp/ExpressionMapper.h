#pragma once

#include <atomic>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// ExpressionConfig — describes the mapping from RMS level to an effect param.
// ─────────────────────────────────────────────────────────────────────────────
struct ExpressionConfig
{
    int   effectIndex = -1;   // index in EffectChain; -1 = inactive
    int   paramIndex  = 0;
    float inMin       = 0.0f; // RMS range [inMin, inMax]
    float inMax       = 1.0f;
    float outMin      = 0.0f; // mapped param range [outMin, outMax]
    float outMax      = 1.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// ExpressionMapper
//
// Linearly maps a smoothed RMS level to an IEffect parameter.
//
//   rmsLevel → clamp to [inMin, inMax] → normalise → lerp to [outMin, outMax]
//
// Thread safety:
//   - setConfig() : GUI thread (all fields are stored atomically)
//   - mapValue()  : audio thread (all fields are loaded atomically)
//   - getConfig() : any thread
// ─────────────────────────────────────────────────────────────────────────────
class ExpressionMapper
{
  public:
    void             setConfig(const ExpressionConfig& cfg) noexcept;
    ExpressionConfig getConfig() const noexcept;

    /// True when effectIndex >= 0.
    bool isActive() const noexcept;
    int  getEffectIndex() const noexcept;
    int  getParamIndex() const noexcept;

    /// Map rmsLevel through the configured range. Clamps input to [inMin,inMax].
    float mapValue(float rmsLevel) const noexcept;

  private:
    std::atomic<int>   effectIndex_{ -1 };
    std::atomic<int>   paramIndex_{  0  };
    std::atomic<float> inMin_{  0.0f };
    std::atomic<float> inMax_{  1.0f };
    std::atomic<float> outMin_{ 0.0f };
    std::atomic<float> outMax_{ 1.0f };
};

} // namespace dsp
