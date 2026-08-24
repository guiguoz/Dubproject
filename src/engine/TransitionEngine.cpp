#include "engine/TransitionEngine.h"
#include <cstring>   // std::memset

namespace engine {

// ─── Helpers statiques ───────────────────────────────────────────────────────

float TransitionEngine::sceneDensity(const SceneData& scene) noexcept {
    if constexpr (kMaxSlots == 0) return 0.0f;
    int active = 0;
    for (int i = 0; i < kMaxSlots; ++i)
        if (scene.slots[i].active) ++active;
    return static_cast<float>(active) / static_cast<float>(kMaxSlots);
}

TransitionType TransitionEngine::chooseType(const SceneData& from,
                                             const SceneData& to) noexcept {
    const float dA = sceneDensity(from);
    const float dB = sceneDensity(to);
    const bool  calmA = dA < kDensityThreshold;
    const bool  calmB = dB < kDensityThreshold;

    if (calmA && calmB)   return TransitionType::Smooth;
    if (!calmA && calmB)  return TransitionType::Breakdown;
    if (calmA && !calmB)  return TransitionType::Build;
    // Musical → Musical
    return TransitionType::Smooth;
}

SlotAction TransitionEngine::diffSlot(const SlotConfig& a,
                                       const SlotConfig& b) noexcept {
    const bool inA = a.active;
    const bool inB = b.active;

    if (!inA && !inB) return SlotAction::Keep;   // aucun des deux → neutre
    if (!inA && inB)  return SlotAction::Enter;
    if (inA && !inB)  return SlotAction::Exit;

    // Les deux actifs : comparer fichier + paramètres stables
    if (a.filePath   != b.filePath)   return SlotAction::Replace;
    if (a.mode       != b.mode)       return SlotAction::Replace;
    if (a.trimStart  != b.trimStart)  return SlotAction::Replace;
    if (a.trimEnd    != b.trimEnd)    return SlotAction::Replace;
    // Comparaison float par valeur exacte — les SlotConfig sont copiés tels quels
    if (a.semitones  != b.semitones)  return SlotAction::Replace;

    return SlotAction::Keep;
}

// ─── requestTransition ───────────────────────────────────────────────────────

void TransitionEngine::requestTransition(int fromScene, int toScene,
                                          const SceneStore& store,
                                          const TransportState& ts) noexcept {
    // Ignorer si déjà en cours
    if (state_ != State::Idle) return;

    const SceneData& from = store.getScene(fromScene);
    const SceneData& to   = store.getScene(toScene);

    // Remplir le plan
    plan_.fromScene = fromScene;
    plan_.toScene   = toScene;
    plan_.type      = chooseType(from, to);
    plan_.valid     = true;

    for (int i = 0; i < kMaxSlots; ++i)
        plan_.slotActions[i] = diffSlot(from.slots[i], to.slots[i]);

    // Frontière d'exécution : prochaine frontière de 16 steps
    plan_.executionSample = nextBoundary(ts, 16);

    state_ = State::Armed;
}

// ─── compilePlan ─────────────────────────────────────────────────────────────

void TransitionEngine::compilePlan(EventScheduler& scheduler,
                                    int64_t boundary) noexcept {
    for (int i = 0; i < kMaxSlots; ++i) {
        const SlotAction action = plan_.slotActions[i];
        const uint8_t    slot   = static_cast<uint8_t>(i);

        switch (action) {
        case SlotAction::Keep:
            // Rien — la position dérivée est continue par construction.
            break;

        case SlotAction::Exit: {
            // GainRamp fade-out : a = durée samples, b = 0 (target gain)
            EngineEvent ev{};
            ev.time = boundary;
            ev.type = EventType::GainRamp;
            ev.slot = slot;
            ev.a    = kFadeOutDur;
            ev.b    = 0.0f;
            scheduler.push(ev);
            break;
        }

        case SlotAction::Enter: {
            // Trigger à la frontière
            {
                EngineEvent ev{};
                ev.time = boundary;
                ev.type = EventType::Trigger;
                ev.slot = slot;
                ev.a    = 0.0f;
                ev.b    = 0.0f;
                scheduler.push(ev);
            }
            // GainRamp fade-in : a = durée samples, b = 1 (target gain)
            {
                EngineEvent ev{};
                ev.time = boundary;
                ev.type = EventType::GainRamp;
                ev.slot = slot;
                ev.a    = kFadeInStart;
                ev.b    = 1.0f;
                scheduler.push(ev);
            }
            break;
        }

        case SlotAction::Replace: {
            // Fade-out voix existante → silence à la frontière
            {
                EngineEvent ev{};
                ev.time = boundary;
                ev.type = EventType::GainRamp;
                ev.slot = slot;
                ev.a    = kFadeOutDur;
                ev.b    = 0.0f;
                scheduler.push(ev);
            }
            // Trigger + fade-in juste après le fade-out terminé
            {
                EngineEvent ev{};
                ev.time = boundary + static_cast<int64_t>(kFadeOutDur);
                ev.type = EventType::Trigger;
                ev.slot = slot;
                ev.a    = 0.0f;
                ev.b    = 0.0f;
                scheduler.push(ev);
            }
            {
                EngineEvent ev{};
                ev.time = boundary + static_cast<int64_t>(kFadeOutDur);
                ev.type = EventType::GainRamp;
                ev.slot = slot;
                ev.a    = kFadeInStart;
                ev.b    = 1.0f;
                scheduler.push(ev);
            }
            break;
        }
        }
    }

    // DUB throw : si type Dub, émettre un SendRamp vers le delay avant la frontière
    if (plan_.type == TransitionType::Dub) {
        // Pour les slots PAD et MELODIC : SendRamp avant la frontière
        for (int i = 0; i < kMaxSlots; ++i) {
            const SlotRole role = SlotRole::Pad;   // placeholder — rôles connus à runtime
            // On cible les slots dont le rôle est Pad (3) ou Melodic (4)
            // La spec indique PAD/MELODIC ; ici on émet pour tous les slots sortants.
            // Une implémentation complète nécessiterait de connaître les rôles dans le plan.
            (void)role;
        }
        // SendRamp release du delay à la frontière
        EngineEvent ev{};
        ev.time = boundary;
        ev.type = EventType::SendRamp;
        ev.slot = 0;   // slot delay (convention : slot 0 = master send)
        ev.a    = 0.0f;
        ev.b    = 0.0f;
        scheduler.push(ev);
    }
}

// ─── processBlock ────────────────────────────────────────────────────────────

void TransitionEngine::processBlock(const TransportState& ts,
                                     EventScheduler& scheduler) noexcept {
    switch (state_) {
    case State::Idle:
    case State::Preparing:
        break;

    case State::Armed:
        if (ts.samplePos >= plan_.executionSample) {
            compilePlan(scheduler, plan_.executionSample);
            settleBlocksLeft_ = kSettleBlocks;
            state_ = State::Executing;
        }
        break;

    case State::Executing:
        if (settleBlocksLeft_ > 0) {
            --settleBlocksLeft_;
        } else {
            state_ = State::Settling;
        }
        break;

    case State::Settling:
        state_ = State::Idle;
        break;
    }
}

} // namespace engine
