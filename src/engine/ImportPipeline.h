#pragma once

#include <cstdint>

namespace engine {

// ─────────────────────────────────────────────────────────────────────────────
// SlotRoleV2 — rôle sémantique d'un slot dans le moteur V2.
// ─────────────────────────────────────────────────────────────────────────────
enum class SlotRoleV2 : uint8_t {
    Kick    = 0,
    Snare,
    HiHat,
    Bass,
    Melodic,
    Pad,
    Perc,
    Fx,
    Loop,
    Unknown
};

// ─────────────────────────────────────────────────────────────────────────────
// AnalysisResult — résultat complet de l'analyse d'un fichier audio.
// ─────────────────────────────────────────────────────────────────────────────
struct AnalysisResult {
    float      detectedBpm    = 0.0f;
    float      correctedBpm   = 0.0f;   ///< après correction d'octave
    float      bpmConfidence  = 0.0f;
    int        loopBeats      = 0;      ///< 0 = pas une loop calable
    SlotRoleV2 role           = SlotRoleV2::Unknown;
    float      roleConfidence = 0.0f;
    int        keyNote        = -1;     ///< -1 = inconnu (0=C, 1=C#, …)
    bool       isMinor        = false;
    float      keyConfidence  = 0.0f;
    bool       autoLoopSync   = false;  ///< vrai si passage auto en LOOP SYNC
    float      timeRatio      = 1.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// ImportPipeline
//
// Analyse synchrone d'un buffer PCM.
// À appeler depuis le message thread uniquement (jamais le thread audio).
// Pour M5, l'analyse est synchrone ; le multi-threading vient en M8/M9.
// ─────────────────────────────────────────────────────────────────────────────
class ImportPipeline {
public:
    /// Lance l'analyse synchrone d'un PCM.
    /// Le PCM doit rester valide pendant toute la durée de l'appel.
    /// @param pcm         Pointeur vers les samples entrelacés (ou mono)
    /// @param numFrames   Nombre de frames (samples si mono)
    /// @param numChannels Nombre de canaux (downmix vers mono effectué ici)
    /// @param sampleRate  Fréquence d'échantillonnage en Hz
    /// @param projectBpm  BPM courant du projet (0 = inconnu, pas de correction d'octave)
    AnalysisResult analyzeSync(const float* pcm, int numFrames,
                               int numChannels, float sampleRate,
                               float projectBpm) noexcept;

private:
    /// Vérifie si loopBeats est entier à < 2% près.
    static bool isLoopable(float durationSamples, float samplesPerBeat,
                           int& outLoopBeats) noexcept;

    /// Règles de passage auto en LOOP SYNC (§5.2 du plan).
    static bool shouldAutoLoopSync(const AnalysisResult& r,
                                   float projectBpm) noexcept;
};

} // namespace engine
