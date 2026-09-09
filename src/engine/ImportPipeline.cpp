#include "engine/ImportPipeline.h"

#include "engine/Analysis/BpmDetector.h"
#include "engine/Analysis/KeyDetector.h"
#include "engine/Analysis/AiContentClassifier.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <vector>

namespace engine {

// ─────────────────────────────────────────────────────────────────────────────
// roleFromFilename
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Vérifie si le mot-clé kw apparaît dans text avec des frontières de mot :
// gauche = début ou char non-alphanumérique ; droite = fin ou char non-alphabétique
// (les chiffres sont acceptés comme terminaison : "kick01" → match "kick").
bool hasKeyword(const std::string& text, const std::string& kw) noexcept
{
    std::size_t pos = text.find(kw);
    while (pos != std::string::npos) {
        const bool leftOk  = (pos == 0 ||
                              !std::isalnum(static_cast<unsigned char>(text[pos - 1])));
        const bool rightOk = (pos + kw.size() >= text.size() ||
                              !std::isalpha(static_cast<unsigned char>(text[pos + kw.size()])));
        if (leftOk && rightOk) return true;
        pos = text.find(kw, pos + 1);
    }
    return false;
}

} // namespace

SlotRoleV2 ImportPipeline::roleFromFilename(const std::string& filePath) noexcept
{
    if (filePath.empty()) return SlotRoleV2::Unknown;

    // Extraire le stem (nom de fichier sans extension ni répertoire)
    const auto lastSep = filePath.find_last_of("/\\");
    std::string stem = (lastSep == std::string::npos)
                       ? filePath : filePath.substr(lastSep + 1);
    const auto dotPos = stem.rfind('.');
    if (dotPos != std::string::npos) stem = stem.substr(0, dotPos);
    for (char& c : stem)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Familles de mots-clés par rôle (ordre interne sans importance).
    // Règle de désambiguïsation : si ≥ 2 familles matchent → Unknown (ONNX fallback).
    struct Family { SlotRoleV2 role; const char* kws[8]; };
    static constexpr Family families[] = {
        { SlotRoleV2::Kick,    { "kick", "bd", "bassdrum", "808",    nullptr } },
        { SlotRoleV2::Snare,   { "snare", "snr", "clap",             nullptr } },
        { SlotRoleV2::HiHat,   { "hihat", "hat", "hh",               nullptr } },
        { SlotRoleV2::Bass,    { "bass", "sub", "basse",              nullptr } },
        { SlotRoleV2::Melodic, { "lead", "melody", "mel",             nullptr } },
        { SlotRoleV2::Pad,     { "pad", "atm", "atmosphere", "ambient", "drone", nullptr } },
        { SlotRoleV2::Perc,    { "perc", "rim", "cowbell", "shaker", "clave", nullptr } },
        { SlotRoleV2::Fx,      { "fx", "sfx", "riser", "sweep", "foley", nullptr } },
        { SlotRoleV2::Loop,    { "loop", "break", "arp",              nullptr } },
    };

    SlotRoleV2 matched = SlotRoleV2::Unknown;
    int matchCount = 0;

    for (const auto& fam : families) {
        for (int ki = 0; fam.kws[ki] != nullptr; ++ki) {
            if (hasKeyword(stem, fam.kws[ki])) {
                ++matchCount;
                if (matchCount == 1)
                    matched = fam.role;
                else
                    return SlotRoleV2::Unknown; // ambigu → ONNX
                break; // une famille est comptée une seule fois
            }
        }
    }
    return matched; // Unknown si 0 famille, sinon le rôle unique trouvé
}

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
                                            float projectBpm,
                                            const std::string& filePath) noexcept
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

    // ── Étape 1 : Classification de rôle ─────────────────────────────────────
    // Priorité : (1) nom de fichier (mots-clés non ambigus) → (2) ONNX fallback
    {
        const SlotRoleV2 nameRole = roleFromFilename(filePath);
        if (nameRole != SlotRoleV2::Unknown) {
            result.role           = nameRole;
            result.roleConfidence = 1.0f; // déterministe : pas de score ONNX
        } else {
            analysis::AiContentClassifier classifier;
            const std::vector<float> pcmVec(monoPcm, monoPcm + monoSize);
            const auto cr = classifier.classifyWithConfidence(pcmVec,
                                                              static_cast<double>(sampleRate));
            result.roleConfidence = cr.confidence;

            // Mapping ContentType → SlotRoleV2
            // CT::OTHER → Unknown (traitement neutre, pas d'EQ agressive)
            switch (cr.type) {
                using CT = analysis::AiContentClassifier::ContentType;
                case CT::KICK:  result.role = SlotRoleV2::Kick;    break;
                case CT::SNARE: result.role = SlotRoleV2::Snare;   break;
                case CT::HIHAT: result.role = SlotRoleV2::HiHat;   break;
                case CT::BASS:  result.role = SlotRoleV2::Bass;    break;
                case CT::SYNTH: result.role = SlotRoleV2::Melodic; break;
                case CT::PAD:   result.role = SlotRoleV2::Pad;     break;
                case CT::PERC:  result.role = SlotRoleV2::Perc;    break;
                case CT::OTHER: result.role = SlotRoleV2::Unknown; break;
            }
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
