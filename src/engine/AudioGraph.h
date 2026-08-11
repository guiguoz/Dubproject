#pragma once
#include <vector>
#include "engine/Transport.h"
#include "engine/EventScheduler.h"
#include "engine/Sequencer.h"
#include "engine/SlotPlayer.h"
#include "engine/AutoMixDub.h"
#include "engine/TransitionEngine.h"
#include "engine/fx/PingPongDelay.h"
#include "engine/fx/MasterLimiter.h"

namespace engine {

// Graphe audio fixe V2.0 (§7.4) :
//   9 × SlotPlayer → [gain AutoMix] → [sidechain] →
//   bus mix + bus sends (delay) → PingPongDelay → MasterLimiter → out
//
// SerumHost est branché en M8 (dépendances JUCE).
class AudioGraph {
public:
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    // Appelé à chaque bloc audio (thread audio uniquement).
    // output : buffer stéréo entrelacé [L0,R0,L1,R1,...].
    // Le buffer est ÉCRASÉ (pas additif — c'est la sortie finale).
    void processBlock(const TransportState& ts,
                      const EngineEvent* events, int numEvents,
                      float* output, int numFrames) noexcept;

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
    void setSlotRole(int slot, SlotRole role) noexcept {
        if (slot >= 0 && slot < kMaxSlots) roles_[slot] = role;
    }

    // Thread de mix : recalcule les cibles AutoMix à partir des rôles courants.
    // Appelé toutes les 50 ms (thread de mix réel ou simulation offline §11.2).
    void updateAutoMixTargets() noexcept { autoMix_.computeTargets(roles_); }

private:
    SlotPlayer        slotPlayer_;
    AutoMixDub        autoMix_;
    Sequencer         sequencer_;
    TransitionEngine  transition_;
    fx::PingPongDelay delay_;
    fx::MasterLimiter limiter_;

    SlotRole roles_[kMaxSlots] = {};

    // Buffers pré-alloués (zéro allocation en audio callback)
    int maxBlock_ = 0;
    std::vector<float> slotBufL_[kMaxSlots];
    std::vector<float> slotBufR_[kMaxSlots];
    std::vector<float> mixL_, mixR_;
    std::vector<float> delayInL_, delayInR_;

    // Kick slot (−1 si non déterminé)
    int kickSlot_ = -1;

    void findKickSlot() noexcept;
};

} // namespace engine
