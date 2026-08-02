#pragma once
#include <cstdint>
#include <vector>
#include "signalsmith-stretch.h"

namespace engine {

// ─── StretchConform ──────────────────────────────────────────────────────────
//
// Wrapper autour de signalsmith::stretch::SignalsmithStretch<float>.
// Gère le bypass transparent (bit-identique) quand timeRatio ≈ 1.0 et semitones ≈ 0.
// Zéro allocation dans le callback audio (buffers pré-alloués dans prepare()).

class StretchConform {
public:
    // channels   : 1 ou 2 (PCM source)
    // sampleRate : taux du device
    void prepare(int channels, float sampleRate) noexcept;

    // timeRatio : bpmSample / bpmProjet (ex. 126/120 = 1.05)
    // semitones : transposition (0 = aucune)
    void setParams(float timeRatio, float semitones) noexcept;

    // true si bypass total (timeRatio ≈ 1.0 && semitones ≈ 0).
    // En bypass, process() copie simplement le PCM.
    bool isBypass() const noexcept { return bypass_; }

    // Latences signalées par Signalsmith (0 en bypass).
    int inputLatency()  const noexcept;
    int outputLatency() const noexcept;

    // Nourrit le stretcher avec `inputFrames` frames depuis `src`
    // (tableau de `channels_` pointeurs, chacun de `inputFrames` float).
    // Produit `outputFrames` frames dans `dst`.
    // En bypass : copie simple (bit-transparent, frame-par-frame mono/stéréo).
    void process(const float* const* src, int inputFrames,
                 float** dst, int outputFrames) noexcept;

    // Pré-roll : nourrit `frames` frames depuis src sans produire de sortie
    // (amortit la latence avant le premier trigger).
    void preRoll(const float* const* src, int frames) noexcept;

    // Reset complet du stretcher (changement de device, etc.).
    void reset() noexcept;

private:
    signalsmith::stretch::SignalsmithStretch<float> stretcher_;

    int   channels_   = 1;
    float timeRatio_  = 1.0f;
    float semitones_  = 0.0f;
    bool  bypass_     = true;
    bool  prepared_   = false;

    // Buffers planaires pré-alloués (évite toute allocation en callback audio).
    // Capacité fixée au prepare() ; redimensionnés si blockSize dépasse.
    std::vector<float> inBufStorage_;    // channels * maxBlock floats
    std::vector<float> outBufStorage_;
    std::vector<float*> inPtrs_;         // [channels] pointeurs dans inBufStorage_
    std::vector<float*> outPtrs_;

    static constexpr float kBypassTol = 1e-4f;
    static constexpr int   kMaxBlock  = 4096;

    // Reconfigure interne après changement de timeRatio/semitones.
    void updateBypass() noexcept;
};

} // namespace engine
