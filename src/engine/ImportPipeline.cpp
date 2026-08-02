#include "engine/ImportPipeline.h"

#include "engine/Analysis/BpmDetector.h"
#include "engine/Analysis/KeyDetector.h"
#include "engine/Analysis/AiContentClassifier.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

bool ImportPipeline::isLoopable(float durationSamples, float samplesPerBeat,
                                 int& outLoopBeats) noexcept
{
    if (samplesPerBeat <= 0.0f || durationSamples <= 0.0f)
        return false;

    const float rawBeats = durationSamples / samplesPerBeat;
    const int   rounded  = static_cast<int>(std::round(rawBeats));
    if (rounded <= 0)
        return false;

    // Accept if within 2% of an integer number of beats
    const float error = std::abs(rawBeats - static_cast<float>(rounded)) / static_cast<float>(rounded);
    if (error < 0.02f) {
        outLoopBeats = rounded;
        return true;
    }
    return false;
}

bool ImportPipeline::shouldAutoLoopSync(const AnalysisResult& r,
                                         float projectBpm) noexcept
{
    // Rôle rythmique (Loop ou Bass) avec confiance suffisante
    const bool rhythmic = (r.role == SlotRoleV2::Loop || r.role == SlotRoleV2::Bass);
    if (!rhythmic)
        return false;
    if (r.roleConfidence < 0.75f)
        return false;
    if (r.loopBeats <= 0)
        return false;

    // Ratio de temps dans [0.8, 1.25]
    if (projectBpm > 0.0f && r.correctedBpm > 0.0f) {
        const float ratio = projectBpm / r.correctedBpm;
        if (ratio < 0.8f || ratio > 1.25f)
            return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeSync
// ─────────────────────────────────────────────────────────────────────────────
AnalysisResult ImportPipeline::analyzeSync(const float* pcm, int numFrames,
                                            int numChannels, float sampleRate,
                                            float projectBpm) noexcept
{
    AnalysisResult result;

    if (!pcm || numFrames <= 0 || numChannels <= 0 || sampleRate <= 0.0f)
        return result;

    // ── Downmix vers mono ────────────────────────────────────────────────────
    std::vector<float> mono;
    if (numChannels > 1) {
        mono.resize(static_cast<std::size_t>(numFrames));
        const float scale = 1.0f / static_cast<float>(numChannels);
        for (int f = 0; f < numFrames; ++f) {
            float sum = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                sum += pcm[f * numChannels + ch];
            mono[static_cast<std::size_t>(f)] = sum * scale;
        }
    }
    const float* monoPcm  = (numChannels > 1) ? mono.data() : pcm;
    const int    monoSize = numFrames;

    // ── Étape 1 : Classification de rôle (ONNX si disponible) ───────────────
    {
        analysis::AiContentClassifier classifier;
        const std::vector<float> pcmVec(monoPcm, monoPcm + monoSize);
        const auto cr = classifier.classifyWithConfidence(pcmVec,
                                                          static_cast<double>(sampleRate));
        result.roleConfidence = cr.confidence;

        // Mapping ContentType → SlotRoleV2
        switch (cr.type) {
            using CT = analysis::AiContentClassifier::ContentType;
            case CT::KICK:  result.role = SlotRoleV2::Kick;    break;
            case CT::SNARE: result.role = SlotRoleV2::Snare;   break;
            case CT::HIHAT: result.role = SlotRoleV2::HiHat;   break;
            case CT::BASS:  result.role = SlotRoleV2::Bass;    break;
            case CT::SYNTH: result.role = SlotRoleV2::Melodic; break;
            case CT::PAD:   result.role = SlotRoleV2::Pad;     break;
            case CT::PERC:  result.role = SlotRoleV2::Perc;    break;
            case CT::OTHER: result.role = SlotRoleV2::Loop;    break;
        }
    }

    // Les percussions (Kick/Snare/HiHat/Perc) ne sont pas calées sur le tempo
    const bool isPercussive = (result.role == SlotRoleV2::Kick  ||
                               result.role == SlotRoleV2::Snare ||
                               result.role == SlotRoleV2::HiHat ||
                               result.role == SlotRoleV2::Perc);

    if (isPercussive && result.roleConfidence >= 0.75f) {
        result.autoLoopSync = false;
        return result;
    }

    // ── Étape 2 : Détection BPM avec correction d'octave ────────────────────
    {
        const auto bpmRes = analysis::BpmDetector::detectWithOctaveCorrection(
            monoPcm, monoSize, sampleRate, projectBpm);

        result.detectedBpm   = bpmRes.bpm;   // avant correction (même valeur en V2
        result.correctedBpm  = bpmRes.bpm;   //   car detectWithOctaveCorrection retourne
                                             //   déjà le bpm corrigé)
        result.bpmConfidence = bpmRes.confidence;

        // detectedBpm = brut : on refait un appel sans correction pour l'exposer
        if (projectBpm > 0.0f) {
            const auto rawRes = analysis::BpmDetector::detectOfflineRobust(
                monoPcm, monoSize, static_cast<double>(sampleRate));
            result.detectedBpm  = rawRes.bpm;
            result.correctedBpm = bpmRes.bpm;
        }

        // loopBeats — uniquement si la confiance BPM est suffisante
        if (result.correctedBpm > 0.0f && result.bpmConfidence > 0.0f) {
            const float samplesPerBeat = sampleRate * 60.0f / result.correctedBpm;
            int beats = 0;
            if (isLoopable(static_cast<float>(monoSize), samplesPerBeat, beats)) {
                result.loopBeats = beats;
                result.timeRatio = (projectBpm > 0.0f && result.correctedBpm > 0.0f)
                                       ? projectBpm / result.correctedBpm
                                       : 1.0f;
            }
        }
    }

    // ── Étape 3 : Auto LOOP SYNC ─────────────────────────────────────────────
    result.autoLoopSync = shouldAutoLoopSync(result, projectBpm);

    // ── Étape 4 : Tonalité (informatif) ──────────────────────────────────────
    {
        const auto keyRes = analysis::KeyDetector::detect(
            monoPcm, monoSize, static_cast<double>(sampleRate));
        result.keyNote      = keyRes.key;
        result.isMinor      = (keyRes.mode == 1);
        result.keyConfidence = keyRes.confidence;
    }

    return result;
}

} // namespace engine
