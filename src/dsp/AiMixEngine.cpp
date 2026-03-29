#include "AiMixEngine.h"

#ifdef SAXFX_HAS_ONNX

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace dsp {

AiMixEngine::AiMixEngine(const std::string& modelPath, const std::string& normPath)
    : model_(modelPath)
{
    std::ifstream f(normPath, std::ios::binary);
    if (!f)
        throw std::runtime_error("AiMixEngine: cannot open norm file: " + normPath);

    for (int i = 0; i < kInputSize; ++i)
    {
        float v = 0.f;
        f.read(reinterpret_cast<char*>(&v), sizeof(float));
        if (!f) throw std::runtime_error("AiMixEngine: norm file truncated (means)");
        means_[i] = v;
    }
    for (int i = 0; i < kInputSize; ++i)
    {
        float v = 1.f;
        f.read(reinterpret_cast<char*>(&v), sizeof(float));
        if (!f) throw std::runtime_error("AiMixEngine: norm file truncated (stds)");
        stds_[i] = (v > 1e-8f) ? v : 1.f;
    }
}

std::array<MixDecision, AiMixEngine::kSlots>
AiMixEngine::predict(const std::array<MixFeatures, kSlots>& slots) const noexcept
{
    // Build normalised input vector (kInputSize = 48)
    std::vector<float> input(static_cast<std::size_t>(kInputSize), 0.f);
    for (int s = 0; s < kSlots; ++s)
    {
        const MixFeatures& mf  = slots[static_cast<std::size_t>(s)];
        const int          base = s * kFeatPerSlot;
        const float raw[kFeatPerSlot] = {
            mf.rms, mf.spectralCentroid, mf.crestFactor,
            mf.lowFrac, mf.midFrac, mf.highFrac
        };
        for (int k = 0; k < kFeatPerSlot; ++k)
        {
            const int idx = base + k;
            input[static_cast<std::size_t>(idx)] =
                (raw[k] - means_[idx]) / stds_[idx];
        }
    }

    // ONNX inference (best-effort: return defaults on exception)
    std::vector<float> out;
    try { out = model_.run(input); }
    catch (...) {}

    if (out.size() < static_cast<std::size_t>(kOutputSize))
        out.assign(static_cast<std::size_t>(kOutputSize), 0.f);

    // Parse output and clamp to safe ranges
    std::array<MixDecision, kSlots> decisions {};
    for (int s = 0; s < kSlots; ++s)
    {
        const int b = s * kOutPerSlot;
        MixDecision& d = decisions[static_cast<std::size_t>(s)];
        d.volume   = std::clamp(out[static_cast<std::size_t>(b + 0)], 0.f, 1.f);
        d.lowGain  = std::clamp(out[static_cast<std::size_t>(b + 1)], -12.f, 12.f);
        d.midGain  = std::clamp(out[static_cast<std::size_t>(b + 2)], -12.f, 12.f);
        d.highGain = std::clamp(out[static_cast<std::size_t>(b + 3)], -12.f, 12.f);
    }
    return decisions;
}

} // namespace dsp

#endif // SAXFX_HAS_ONNX
