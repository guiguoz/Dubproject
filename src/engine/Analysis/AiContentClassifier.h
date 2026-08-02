#pragma once

#include <string>
#include <vector>

#ifdef SAXFX_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#include "OnnxInference.h"
#include <fstream>
#include <stdexcept>
#endif

namespace engine::analysis {

// ─────────────────────────────────────────────────────────────────────────────
// AiContentClassifier
//
// Wrapper around OnnxInference for audio content type classification.
// Expects the same preprocessing as used in training:
//   - Input PCM at 44100 Hz (mono)
//   - Pad/crop to 22050 samples (0.5s)
//   - Extract 64 features (RMS, peak, crest, ZCR, attack ratio, above threshold,
//     5 band energy ratios, 8 segment RMS)
//   - Normalize with z-score using precomputed mean and std
//   - Run ONNX model (64 inputs -> 7 class logits)
//   - Return class with highest logit
//
// When SAXFX_HAS_ONNX is not defined (default in V2 tests), the classifier is
// unavailable. classify() returns OTHER and classifyWithConfidence() returns
// {OTHER, 0.0f}.
//
// Thread safety: Not thread-safe. Use one instance per thread or protect with mutex.
// ─────────────────────────────────────────────────────────────────────────────
class AiContentClassifier {
public:
    enum class ContentType { KICK, SNARE, HIHAT, BASS, SYNTH, PAD, PERC, OTHER };

    struct ClassifyResult {
        ContentType type       = ContentType::OTHER;
        float       confidence = 0.0f; ///< softmax probability of winning class [0,1]
    };

#ifdef SAXFX_HAS_ONNX
    /// Construct and load model and normalization parameters.
    /// Throws std::runtime_error on failure.
    AiContentClassifier(const std::string& modelPath,
                        const std::string& normPath);

    /// Classify audio content type.
    /// @param pcm        Mono PCM samples (float in [-1, 1])
    /// @param sampleRate Sample rate in Hz (expected 44100)
    ContentType classify(const std::vector<float>& pcm, double sampleRate);

    /// Classify with confidence score (softmax probability).
    ClassifyResult classifyWithConfidence(const std::vector<float>& pcm, double sampleRate);

    /// Get number of elements expected by the model (after feature extraction).
    size_t featureSize() const noexcept { return 64; }

private:
    std::vector<float> padOrCrop(const std::vector<float>& samples, size_t targetSize);
    std::vector<float> extractFeatures(const std::vector<float>& pcm);
    void               normalizeFeatures(std::vector<float>& features);

    OnnxInference      model_;
    std::vector<float> means_;
    std::vector<float> stds_;

    static constexpr const char* kClassLabels[8] = {
        "KICK", "SNARE", "HIHAT", "BASS", "SYNTH", "PAD", "PERC", "OTHER"
    };

#else // !SAXFX_HAS_ONNX — stub (no model available)

    AiContentClassifier() = default;
    // Constructor accepting paths but doing nothing (ONNX not available)
    AiContentClassifier(const std::string& /*modelPath*/,
                        const std::string& /*normPath*/) {}

    ContentType classify(const std::vector<float>& /*pcm*/,
                         double /*sampleRate*/) { return ContentType::OTHER; }

    ClassifyResult classifyWithConfidence(const std::vector<float>& /*pcm*/,
                                          double /*sampleRate*/)
    {
        return {ContentType::OTHER, 0.0f};
    }

    size_t featureSize() const noexcept { return 64; }

#endif // SAXFX_HAS_ONNX
};

} // namespace engine::analysis
