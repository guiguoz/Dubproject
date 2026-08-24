#pragma once
#include <cmath>
#include <vector>
#include "engine/Transport.h"
#include "engine/EventScheduler.h"
#include "engine/Sequencer.h"
#include "engine/SlotPlayer.h"
#include "engine/AutoMixDub.h"
#include "engine/TransitionEngine.h"
#include "engine/fx/PingPongDelay.h"
#include "engine/fx/MasterLimiter.h"
#include "engine/mix/MixAlgorithms.h"

namespace engine {

// ─── MonoSubFilter ────────────────────────────────────────────────────────────
// 1er ordre Butterworth LP (6 dB/oct) à fc = 120 Hz — force le sub-bass en mono.
// Stereo in-place : même état pour L et R (analyse mono sur la somme L+R).
// Coefficient pré-calculé en prepare(), état audio-thread uniquement.
struct MonoSubFilter {
    float a1 = 0.f;
    float sL = 0.f;   // état du filtre LP sur le canal L
    float sR = 0.f;   // état du filtre LP sur le canal R

    void prepare(double sampleRate) noexcept {
        // Butterworth 1er ordre : a1 = -(1 - 2π·fc/fs) / (1 + 2π·fc/fs)
        const float wc = static_cast<float>(2.0 * 3.14159265 * 120.0 / sampleRate);
        a1 = -(1.f - wc) / (1.f + wc);
    }

    void process(float* L, float* R, int numFrames) noexcept {
        for (int i = 0; i < numFrames; ++i) {
            // LP sur chaque canal → sub-bass
            sL = L[i] + a1 * sL;
            sR = R[i] + a1 * sR;
            const float monoSub = (sL + sR) * 0.5f;
            // Remplacer le sub-bass stéréo par la version mono
            L[i] += monoSub - sL;
            R[i] += monoSub - sR;
        }
    }

    void reset() noexcept { sL = 0.f; sR = 0.f; }
};

// Rôle V2 → type magic mix (spatialisation runtime M9 étape 6 + worker M9
// étape 8). Centralisé ici pour un mapping unique façade/graphe.
inline mix::MixContentType roleToMixType(SlotRole role) noexcept
{
    using mix::MixContentType;
    switch (role) {
        case SlotRole::Kick:    return MixContentType::KICK;
        case SlotRole::Bass:    return MixContentType::BASS;
        case SlotRole::Snare:   return MixContentType::SNARE;
        case SlotRole::Pad:     return MixContentType::PAD;
        case SlotRole::Melodic: return MixContentType::SYNTH;
        case SlotRole::Perc:    return MixContentType::PERC;
        case SlotRole::Fx:      return MixContentType::OTHER;
        case SlotRole::Loop:    return MixContentType::LOOP;
        case SlotRole::Drum:    return MixContentType::PERC;
        default:                return MixContentType::OTHER;
    }
}

// Downmix stéréo entrelacé → mono : mono[i] = (L[i] + R[i]) * 0.5.
// Utilisé par EngineFacade::processBlock lorsque le device est mono
// (left == right) — le graphe rend toujours en stéréo entrelacé.
inline void downmixInterleavedToMono(const float* interleaved, float* monoOut,
                                     int numFrames) noexcept
{
    for (int i = 0; i < numFrames; ++i)
        monoOut[i] = (interleaved[i * 2] + interleaved[i * 2 + 1]) * 0.5f;
}

// Graphe audio fixe V2.0 (§7.4) :
//   9 × SlotPlayer → [gain AutoMix] → [sidechain] →
//   bus mix + bus sends (delay) → PingPongDelay → MasterLimiter → out
//
// Entrées externes mélangées AVANT le bus delay (elles profitent donc du dub
// echo et du limiteur) : dry bus EWI/sax (-- extension via setInputGain) et
// bus Serum (gain rider fourni par le message thread par bloc).
class AudioGraph {
public:
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    // Appelé à chaque bloc audio (thread audio uniquement).
    // output : buffer stéréo entrelacé [L0,R0,L1,R1,...].
    // Le buffer est ÉCRASÉ (pas additif — c'est la sortie finale).
    // extInL/extInR : entrée dry (EWI/sax) à centrer, + inputGain_ (nul si nullptr).
    // serumL/serumR : sortie Serum post-proc, multipliée par serumGain (nul si nullptr).
    void processBlock(const TransportState& ts,
                      const EngineEvent* events, int numEvents,
                      float* output, int numFrames,
                      const float* extInL = nullptr, const float* extInR = nullptr,
                      const float* serumL = nullptr, const float* serumR = nullptr,
                      float serumGain  = 0.f) noexcept;

    // Gain du bus d'entrée dry (EWI/sax), 0 = silence.
    void setInputGain(float g) noexcept { inputGain_.store(g, std::memory_order_relaxed); }
    float getInputGain() const noexcept { return inputGain_.load(std::memory_order_relaxed); }

    // Accès aux sous-systèmes (pour configuration depuis le message thread).
    SlotPlayer&             slotPlayer()       noexcept { return slotPlayer_; }
    const SlotPlayer&       slotPlayer() const noexcept { return slotPlayer_; }
    AutoMixDub&             autoMix()          noexcept { return autoMix_; }
    Sequencer&              sequencer()        noexcept { return sequencer_; }
    const Sequencer&        sequencer()  const noexcept { return sequencer_; }
    TransitionEngine&       transitionEngine() noexcept { return transition_; }
    fx::PingPongDelay&      delay()            noexcept { return delay_; }
    fx::MasterLimiter&      limiter()          noexcept { return limiter_; }

    // Rôle par slot (écrit depuis message thread, lu en audio thread).
    // Met à jour le slot kick pour le sidechain et la spatialisation pan+Haas
    // (M9 étape 6) si le rôle change.
    void setSlotRole(int slot, SlotRole role) noexcept;
    // Rôle courant du slot (write side / message thread) — pour le worker magic mix.
    SlotRole slotRole(int slot) const noexcept { return roles_[slot].load(std::memory_order_relaxed); }

    // Thread de mix : recalcule les cibles AutoMix à partir des rôles courants.
    // Appelé toutes les 50 ms (thread de mix réel ou simulation offline §11.2).
    void updateAutoMixTargets() noexcept {
        SlotRole snapshot[kMaxSlots];
        for (int i = 0; i < kMaxSlots; ++i)
            snapshot[i] = roles_[i].load(std::memory_order_relaxed);
        autoMix_.computeTargets(snapshot);
    }

private:
    SlotPlayer        slotPlayer_;
    AutoMixDub        autoMix_;
    Sequencer         sequencer_;
    TransitionEngine  transition_;
    fx::PingPongDelay delay_;
    fx::MasterLimiter limiter_;
    MonoSubFilter     monoSubFilter_;

    std::atomic<SlotRole> roles_[kMaxSlots] = {};

    // Gain du bus d'entrée dry (EWI/sax), appliqué en audio thread.
    std::atomic<float> inputGain_ {0.8f};

    // Buffers pré-alloués (zéro allocation en audio callback)
    int maxBlock_ = 0;
    std::vector<float> slotBufL_[kMaxSlots];
    std::vector<float> slotBufR_[kMaxSlots];
    std::vector<float> mixL_, mixR_;
    std::vector<float> slotMixL_, slotMixR_;   // contribution slots (bus delay)
    std::vector<float> delayInL_, delayInR_;
    std::vector<float> interleavedScratch_;    // scratch entrelacé (maxBlock × 2)

    // Kick slot (−1 si non déterminé)
    int kickSlot_ = -1;

    void findKickSlot() noexcept;
};

} // namespace engine
