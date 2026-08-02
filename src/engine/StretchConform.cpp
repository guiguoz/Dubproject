#include "engine/StretchConform.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace engine {

// ─── prepare ─────────────────────────────────────────────────────────────────

void StretchConform::prepare(int channels, float sampleRate) noexcept {
    channels_ = (channels >= 2) ? 2 : 1;

    stretcher_.presetDefault(channels_, sampleRate);

    // Pré-allouer les buffers planaires pour éviter toute allocation en audio.
    const int cap = kMaxBlock * channels_;
    inBufStorage_.assign(static_cast<size_t>(cap), 0.f);
    outBufStorage_.assign(static_cast<size_t>(cap), 0.f);

    inPtrs_.resize(static_cast<size_t>(channels_));
    outPtrs_.resize(static_cast<size_t>(channels_));
    for (int c = 0; c < channels_; ++c) {
        inPtrs_[static_cast<size_t>(c)]  = inBufStorage_.data()  + c * kMaxBlock;
        outPtrs_[static_cast<size_t>(c)] = outBufStorage_.data() + c * kMaxBlock;
    }

    prepared_ = true;
    updateBypass();
    stretcher_.reset();
}

// ─── setParams ───────────────────────────────────────────────────────────────

void StretchConform::setParams(float timeRatio, float semitones) noexcept {
    timeRatio_ = timeRatio;
    semitones_ = semitones;
    updateBypass();

    if (!bypass_ && prepared_) {
        // timeRatio > 1 : l'entrée est plus rapide que la sortie souhaitée.
        // Signalsmith produit outputFrames depuis inputFrames = outputFrames * timeRatio.
        // Pas de réglage spécial : le ratio est géré par le ratio inputFrames/outputFrames
        // dans chaque appel à process().
        stretcher_.setTransposeSemitones(semitones_);
    }
}

// ─── inputLatency / outputLatency ────────────────────────────────────────────

int StretchConform::inputLatency() const noexcept {
    return bypass_ ? 0 : stretcher_.inputLatency();
}

int StretchConform::outputLatency() const noexcept {
    return bypass_ ? 0 : stretcher_.outputLatency();
}

// ─── process ─────────────────────────────────────────────────────────────────

void StretchConform::process(const float* const* src, int inputFrames,
                             float** dst, int outputFrames) noexcept {
    if (bypass_) {
        // Chemin bit-transparent : copie directe frame-par-frame.
        const int frames = std::min(inputFrames, outputFrames);
        for (int c = 0; c < channels_; ++c) {
            if (src[c] && dst[c])
                std::memcpy(dst[c], src[c], static_cast<size_t>(frames) * sizeof(float));
        }
        return;
    }

    if (!prepared_) return;

    // Clamp pour éviter de déborder les buffers pré-alloués.
    const int safeIn  = std::min(inputFrames,  kMaxBlock);
    const int safeOut = std::min(outputFrames, kMaxBlock);

    // Copier l'entrée interleaved → planaire dans nos buffers internes.
    for (int c = 0; c < channels_; ++c) {
        if (src[c])
            std::memcpy(inPtrs_[static_cast<size_t>(c)], src[c],
                        static_cast<size_t>(safeIn) * sizeof(float));
        else
            std::memset(inPtrs_[static_cast<size_t>(c)], 0,
                        static_cast<size_t>(safeIn) * sizeof(float));
    }

    stretcher_.process(inPtrs_.data(), safeIn, outPtrs_.data(), safeOut);

    // Copier la sortie planaire vers dst.
    for (int c = 0; c < channels_; ++c) {
        if (dst[c])
            std::memcpy(dst[c], outPtrs_[static_cast<size_t>(c)],
                        static_cast<size_t>(safeOut) * sizeof(float));
    }
}

// ─── preRoll ─────────────────────────────────────────────────────────────────

void StretchConform::preRoll(const float* const* src, int frames) noexcept {
    if (bypass_ || !prepared_) return;

    // Silence output buffer (pas besoin de la sortie).
    const int safeIn = std::min(frames, kMaxBlock);

    for (int c = 0; c < channels_; ++c) {
        if (src[c])
            std::memcpy(inPtrs_[static_cast<size_t>(c)], src[c],
                        static_cast<size_t>(safeIn) * sizeof(float));
        else
            std::memset(inPtrs_[static_cast<size_t>(c)], 0,
                        static_cast<size_t>(safeIn) * sizeof(float));
        std::memset(outPtrs_[static_cast<size_t>(c)], 0,
                    static_cast<size_t>(safeIn) * sizeof(float));
    }

    // Produire autant de sortie que d'entrée (pour drainer la latence).
    stretcher_.process(inPtrs_.data(), safeIn, outPtrs_.data(), safeIn);
}

// ─── reset ───────────────────────────────────────────────────────────────────

void StretchConform::reset() noexcept {
    if (prepared_)
        stretcher_.reset();
}

// ─── updateBypass (privé) ────────────────────────────────────────────────────

void StretchConform::updateBypass() noexcept {
    const bool ratioOk = (std::abs(timeRatio_ - 1.0f) < kBypassTol);
    const bool pitchOk = (std::abs(semitones_)        < kBypassTol);
    bypass_ = ratioOk && pitchOk;
}

} // namespace engine
