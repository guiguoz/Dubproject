#include "ExpressionMapper.h"

#include <algorithm>

namespace dsp
{

void ExpressionMapper::setConfig(const ExpressionConfig& cfg) noexcept
{
    effectIndex_.store(cfg.effectIndex, std::memory_order_relaxed);
    paramIndex_.store(cfg.paramIndex,   std::memory_order_relaxed);
    inMin_.store(cfg.inMin,             std::memory_order_relaxed);
    inMax_.store(cfg.inMax,             std::memory_order_relaxed);
    outMin_.store(cfg.outMin,           std::memory_order_relaxed);
    outMax_.store(cfg.outMax,           std::memory_order_relaxed);
}

ExpressionConfig ExpressionMapper::getConfig() const noexcept
{
    return {
        effectIndex_.load(std::memory_order_relaxed),
        paramIndex_.load(std::memory_order_relaxed),
        inMin_.load(std::memory_order_relaxed),
        inMax_.load(std::memory_order_relaxed),
        outMin_.load(std::memory_order_relaxed),
        outMax_.load(std::memory_order_relaxed)
    };
}

bool ExpressionMapper::isActive() const noexcept
{
    return effectIndex_.load(std::memory_order_relaxed) >= 0;
}

int ExpressionMapper::getEffectIndex() const noexcept
{
    return effectIndex_.load(std::memory_order_relaxed);
}

int ExpressionMapper::getParamIndex() const noexcept
{
    return paramIndex_.load(std::memory_order_relaxed);
}

float ExpressionMapper::mapValue(float rmsLevel) const noexcept
{
    const float in0  = inMin_.load(std::memory_order_relaxed);
    const float in1  = inMax_.load(std::memory_order_relaxed);
    const float out0 = outMin_.load(std::memory_order_relaxed);
    const float out1 = outMax_.load(std::memory_order_relaxed);

    if (in1 <= in0)
        return out0; // degenerate range

    const float t = std::max(0.0f, std::min(1.0f, (rmsLevel - in0) / (in1 - in0)));
    return out0 + t * (out1 - out0);
}

} // namespace dsp
