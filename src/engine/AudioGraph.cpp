#include "engine/AudioGraph.h"
#include <cstring>
#include <cmath>

namespace engine {

void AudioGraph::setSlotRole(int slot, SlotRole role) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    roles_[slot] = role;

    // Spatialisation runtime dérivée du rôle (pan + Haas). Centroid neutre
    // (spatialForType) — le centroid réel du PCM est pris en charge par le
    // thread de mix (étapes 7-8).
    const auto sp = mix::spatialForType(slot, roleToMixType(role));
    slotPlayer_.setSpatial(slot, sp.pan, sp.width);

    if (role == SlotRole::Kick)
        kickSlot_ = slot;
    else if (kickSlot_ == slot)
        findKickSlot();
}

void AudioGraph::prepare(double sampleRate, int maxBlockSize) noexcept
{
    maxBlock_ = maxBlockSize;
    slotPlayer_.prepareStretchers(2, static_cast<float>(sampleRate));
    autoMix_.prepare(static_cast<float>(sampleRate));
    delay_.prepare(sampleRate, maxBlockSize);
    limiter_.prepare(sampleRate);

    mixL_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    mixR_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    slotMixL_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    slotMixR_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    delayInL_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    delayInR_.assign(static_cast<size_t>(maxBlockSize), 0.f);

    for (int s = 0; s < kMaxSlots; ++s) {
        slotBufL_[s].assign(static_cast<size_t>(maxBlockSize), 0.f);
        slotBufR_[s].assign(static_cast<size_t>(maxBlockSize), 0.f);
    }
    findKickSlot();
}

void AudioGraph::findKickSlot() noexcept
{
    kickSlot_ = -1;
    for (int s = 0; s < kMaxSlots; ++s)
        if (roles_[s] == SlotRole::Kick) { kickSlot_ = s; return; }
}

void AudioGraph::processBlock(const TransportState& ts,
                               const EngineEvent* events, int numEvents,
                               float* output, int numFrames,
                               const float* extInL, const float* extInR,
                               const float* serumL, const float* serumR,
                               float serumGain) noexcept
{
    if (numFrames <= 0 || numFrames > maxBlock_) {
        // Sortie silencieuse si le bloc est invalide
        std::memset(output, 0, static_cast<size_t>(numFrames) * 2 * sizeof(float));
        return;
    }

    // ─ 1. Vider les buffers de travail ──────────────────────────────────────
    const auto n = static_cast<size_t>(numFrames);
    std::fill(mixL_.begin(),     mixL_.begin()     + numFrames, 0.f);
    std::fill(mixR_.begin(),     mixR_.begin()     + numFrames, 0.f);
    std::fill(delayInL_.begin(), delayInL_.begin() + numFrames, 0.f);
    std::fill(delayInR_.begin(), delayInR_.begin() + numFrames, 0.f);
    (void)n;

    // ─ 2. SlotPlayer → buffer entrelacé → séparer L/R ────────────────────────
    // Buffer entrelacé temporaire (alloué en prepare, réutilisé chaque bloc
    // via mixL_/mixR_ qui sont de toute façon effacés ci-dessus).
    // Astuce : utiliser slotBufL_[0] comme scratch entrelacé (taille = maxBlock_).
    // On le réutilise — il sera réécrit avant usage.
    static thread_local std::vector<float> interleavedScratch;
    if (static_cast<int>(interleavedScratch.size()) < numFrames * 2)
        interleavedScratch.assign(static_cast<size_t>(numFrames * 2), 0.f);
    std::fill(interleavedScratch.begin(),
              interleavedScratch.begin() + numFrames * 2, 0.f);

    slotPlayer_.processBlock(ts, interleavedScratch.data(), numFrames,
                             events, numEvents);

    // SlotMix : contribution des slots (utilisée pour le bus send delay).
    // Le mix final (mixL_/R_) reçoit aussi les entrées externes plus bas.
    for (int i = 0; i < numFrames; ++i) {
        mixL_[i] = interleavedScratch[static_cast<size_t>(i * 2)];
        mixR_[i] = interleavedScratch[static_cast<size_t>(i * 2 + 1)];
    }
    slotMixL_.assign(mixL_.begin(), mixL_.begin() + numFrames);
    slotMixR_.assign(mixR_.begin(), mixR_.begin() + numFrames);

    // ─ 3. AutoMix features + sidechain + gain ────────────────────────────────
    // Mise à jour features (approximation M7 : le sum total, pas par slot)
    for (int s = 0; s < kMaxSlots; ++s)
        autoMix_.updateFeatures(s, mixL_.data(), mixR_.data(), numFrames);

    // Sidechain kick (enveloppe par sample, puis appliquée aux slots BASS/PAD)
    {
        float kickEnv = autoMix_.kickEnv();
        for (int i = 0; i < numFrames; ++i)
            kickEnv = autoMix_.advanceKickEnv(mixL_[i] + mixR_[i]);

        for (int s = 0; s < kMaxSlots; ++s) {
            if (s == kickSlot_) continue;
            autoMix_.applySidechain(s, roles_[s], kickEnv,
                                    mixL_.data(), mixR_.data(), numFrames);
        }
    }

    // Gain staging global (approximation M7 : gain moyen des slots actifs)
    {
        float gainSum = 0.f; int active = 0;
        for (int s = 0; s < kMaxSlots; ++s) {
            const float g = autoMix_.advanceGainRamp(s, numFrames);
            gainSum += g; ++active;
        }
        const float avgGain = (active > 0) ? gainSum / static_cast<float>(active) : 1.f;
        for (int i = 0; i < numFrames; ++i) {
            mixL_[i] *= avgGain;
            mixR_[i] *= avgGain;
        }
    }

    // ─ 4. Entrées externes (EWI/sax dry + Serum) → on mélange dans le bus,
    //      sans envoyer en loop dans le délai (send 0 pour ces bus). ───────────
    {
        // Dry EWI : entrée stéréo centrée avec inputGain_.
        const float ig = inputGain_;
        if (extInL != nullptr && extInR != nullptr && ig > 0.001f) {
            for (int i = 0; i < numFrames; ++i) {
                mixL_[static_cast<size_t>(i)] += extInL[i] * ig;
                mixR_[static_cast<size_t>(i)] += extInR[i] * ig;
            }
        } else if (extInL != nullptr && ig > 0.001f) {
            for (int i = 0; i < numFrames; ++i) {
                const float v = extInL[i] * ig;
                mixL_[static_cast<size_t>(i)] += v;
                mixR_[static_cast<size_t>(i)] += v;
            }
        }

        // Serum (déjà post-effets, gain rider aplicado par le message thread).
        if (serumL != nullptr && serumR != nullptr && serumGain > 0.001f) {
            for (int i = 0; i < numFrames; ++i) {
                mixL_[static_cast<size_t>(i)] += serumL[i] * serumGain;
                mixR_[static_cast<size_t>(i)] += serumR[i] * serumGain;
            }
        }
    }

    // ─ 5. PingPongDelay (additif dans le mix) ────────────────────────────────
    // Bus send delay (sends par rôle, rampe 120 ms) — alimenté uniquement par
    // la contribution des slots (pas par l'entrée dry EWI ni Serum).
    for (int s = 0; s < kMaxSlots; ++s) {
        const float send = autoMix_.advanceDelayRamp(s, numFrames);
        if (send > 0.001f) {
            for (int i = 0; i < numFrames; ++i) {
                delayInL_[i] += slotMixL_[i] * send;
                delayInR_[i] += slotMixR_[i] * send;
            }
        }
    }

    delay_.processAdd(delayInL_.data(), delayInR_.data(),
                      mixL_.data(), mixR_.data(), numFrames);

    // ─ 6. MasterLimiter → sortie entrelacée ──────────────────────────────────
    limiter_.process(mixL_.data(), mixR_.data(), numFrames);
    autoMix_.notifyLimiterReduction(limiter_.getGainReductionDb());

    for (int i = 0; i < numFrames; ++i) {
        output[i * 2]     = mixL_[i];
        output[i * 2 + 1] = mixR_[i];
    }
}

} // namespace engine
