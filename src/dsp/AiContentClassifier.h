#pragma once

#ifdef SAXFX_HAS_ONNX

#include <onnxruntime_cxx_api.h>
#include "OnnxInference.h"
#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>

namespace dsp {

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
// Thread safety: Not thread-safe. Use one instance per thread or protect with mutex.
// 
// Usage:
//   AiContentClassifier classifier("models/content_classifier.onnx",
//                                  "models/content_classifier_norm.bin");
//   auto type = classifier.classify(pcm, sampleRate);
// 
// ─────────────────────────────────────────────────────────────────────────────
class AiContentClassifier {
public:
    enum class ContentType { KICK, SNARE, HIHAT, BASS, SYNTH, PAD, PERC, OTHER };

    /// Construct and load model and normalization parameters.
    /// Throws std::runtime_error on failure.
    AiContentClassifier(const std::string& modelPath,
                        const std::string& normPath);

    /// Classify audio content type.
    /// @param pcm      Mono PCM samples (float in [-1, 1])
    /// @param sampleRate Sample rate in Hz (expected 44100)
    ContentType classify(const std::vector<float>& pcm, double sampleRate);

    /// Get number of elements expected by the model (after feature extraction).
    size_t featureSize() const noexcept { return 64; }

private:
    // Preprocessing: pad/crop to 0.5s at 44100 Hz
    std::vector<float> padOrCrop(const std::vector<float>& samples, size_t targetSize);

    // Feature extraction (ported from train_classifier.py)
    std::vector<float> extractFeatures(const std::vector<float>& pcm);

    // Normalization (z-score)
    void normalizeFeatures(std::vector<float>& features);

    OnnxInference model_;
    std::vector<float> means_;
    std::vector<float> stds_;
    static constexpr const char* classLabels_[8] = {
        "KICK", "SNARE", "HIHAT", "BASS", "SYNTH", "PAD", "PERC", "OTHER"
    };
};

} // namespace dsp

#endif // SAXFX_HAS_ONNX