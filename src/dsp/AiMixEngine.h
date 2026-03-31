#pragma once

#ifdef SAXFX_HAS_ONNX

#include "FeatureExtractor.h"  // MixFeatures, ContentCategory
#include "OnnxInference.h"

#include <array>
#include <string>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// MixDecision — per-slot output of AiMixEngine.
//
// volume   : linear gain [0, 1] — passed directly to Sampler::setGain()
// lowGain  : low-band  EQ trim (dB), applied as low shelf  at ~500 Hz
// midGain  : mid-band  EQ trim (dB), applied as peak      at ~1 kHz
// highGain : high-band EQ trim (dB), applied as high shelf at ~4 kHz
// ─────────────────────────────────────────────────────────────────────────────
struct MixDecision {
    float volume   = 0.5f;
    float lowGain  = 0.f;
    float midGain  = 0.f;
    float highGain = 0.f;
    float pan      = 0.f;   // −1.0 (L) … +1.0 (R)
    float width    = 0.f;   // 0 = mono, 1 = full Haas (25 ms)
    float depth    = 0.f;   // 0 = front, 1 = back (high-shelf attenuation)
};

// ─────────────────────────────────────────────────────────────────────────────
// AiMixEngine
//
// Replaces the heuristic applyRoleEQ + applyUnmasking + targetGainForType
// calls in SmartSamplerEngine::applyNeutronMix with a single MLP inference.
//
// Model I/O (must match train_mix_model.py):
//   Input  float32[1, 48]  — 8 slots × 6 features (zeros for empty slots)
//                            [rms, centroid, crest, lowFrac, midFrac, highFrac]
//   Output float32[1, 32]  — 8 slots × 4 values
//                            [volume, lowGain(dB), midGain(dB), highGain(dB)]
//
// Thread safety: same as OnnxInference — one instance per thread.
// ─────────────────────────────────────────────────────────────────────────────
class AiMixEngine {
public:
    static constexpr int kSlots       = 8;
    static constexpr int kFeatPerSlot = 6;   ///< rms, centroid, crest, lowF, midF, highF
    static constexpr int kOutPerSlot  = 4;   ///< volume, lowGain, midGain, highGain
    static constexpr int kInputSize   = kSlots * kFeatPerSlot;   // 48
    static constexpr int kOutputSize  = kSlots * kOutPerSlot;    // 32

    /// Load model and normalization params.
    /// @throws std::runtime_error if files cannot be opened or are malformed.
    AiMixEngine(const std::string& modelPath, const std::string& normPath);

    /// Predict per-slot mixing decisions from 8 MixFeatures.
    /// Empty slots (rms == 0) produce a default MixDecision (volume 0.5, gains 0).
    std::array<MixDecision, kSlots>
        predict(const std::array<MixFeatures, kSlots>& slots) const noexcept;

    bool isLoaded() const noexcept { return model_.isLoaded(); }

private:
    mutable OnnxInference model_;
    float         means_[kInputSize] {};
    float         stds_ [kInputSize] {};
};

} // namespace dsp

#endif // SAXFX_HAS_ONNX
