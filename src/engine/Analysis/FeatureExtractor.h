#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// engine/Analysis/FeatureExtractor.h
//
// Port de src/dsp/FeatureExtractor (M9, étape 12) — analyse hors temps réel
// du contenu d'un buffer mono : RMS, centroïde spectral, fractions de bandes,
// rapport de transitoire + classification heuristique du contenu.
//
// Zéro dépendance JUCE / dsp/ : compilable par EngineTests (C++17).
// Utilisé par MainComponent (régie Serum : gain rider + compensation de
// masquage côté mix V2).
// ─────────────────────────────────────────────────────────────────────────────

#include <vector>

namespace engine::analysis {

// ─────────────────────────────────────────────────────────────────────────────
// ContentCategory — classification heuristique du contenu audio (sans ONNX).
// ─────────────────────────────────────────────────────────────────────────────
enum class ContentCategory { KICK, SNARE, HIHAT, BASS, SYNTH, PAD, PERC, OTHER };

// ─────────────────────────────────────────────────────────────────────────────
// MixFeatures — descripteur d'un slot/buffer pour la régie de mix.
// ─────────────────────────────────────────────────────────────────────────────
struct MixFeatures {
    float rms              = 0.f;  ///< Root mean square amplitude
    float spectralCentroid = 0.f;  ///< Energy-weighted mean frequency (Hz)
    float crestFactor      = 0.f;  ///< Peak / RMS  (≈1.41 sine, >4 sharp transient)
    ContentCategory contentType = ContentCategory::OTHER; ///< Heuristic or AI-override
    float lowFrac    = 0.f;  ///< Energy fraction below ≈500 Hz
    float midFrac    = 0.f;  ///< Energy fraction 500–4000 Hz
    float highFrac   = 0.f;  ///< Energy fraction above ≈4000 Hz
    float durationMs = 0.f;  ///< Signal duration in milliseconds
};

// ─────────────────────────────────────────────────────────────────────────────
// FeatureExtractor — stateless, pure C++, no JUCE dependency.
// ─────────────────────────────────────────────────────────────────────────────
class FeatureExtractor {
public:
    /// Extract mixing features from a mono PCM buffer.
    /// @param pcm        Samples in [-1, 1]
    /// @param sampleRate Sample rate in Hz
    static MixFeatures extract(const std::vector<float>& pcm, double sampleRate) noexcept;

private:
    /// Goertzel single-frequency energy estimate (limited to first 8192 samples).
    static float goertzelEnergy(const std::vector<float>& pcm,
                                 double sampleRate, float freqHz) noexcept;

    /// Energy-weighted mean frequency across kCentroidFreqs analysis bins.
    static float computeSpectralCentroid(const std::vector<float>& pcm,
                                          double sampleRate) noexcept;

    /// Heuristic content classification.
    static ContentCategory classifyHeuristic(float transientRatio,
                                              float lowFrac, float midFrac,
                                              float highFrac,
                                              float durationMs) noexcept;
};

} // namespace engine::analysis