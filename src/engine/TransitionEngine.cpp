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
    if (state_.load(std::memory_order_acquire) != State::Idle) return;

    const SceneData& from = store.getScene(fromScene);
    const SceneData& to   = store.getScene(toScene);

    // Remplir le plan
    plan_.fromScene = fromScene;
    plan_.toScene   = toScene;
    plan_.type      = chooseType(from, to);
    plan_.valid     = true;

    for (int i = 0; i < kMaxSlots; ++i)
        plan_.slotActions[i] = diffSlot(from.slots[i], to.slots[i]);

    // Frontière d'exécution : fin du cycle de la scène courante (même règle que
    // le plan DIRECT — « prochaine mesure » coupait les scènes multi-mesures au
    // milieu de leur pattern). Le recalage de phase de la scène entrante n'existe
    // que sur le chemin DIRECT (stageForBoundary) : ce chemin legacy sert au
    // rendu offline et aux tests.
    plan_.executionSample = planTransitionBoundary(ts, from).sample;

    hasDirectPlan_ = false;
    state_.store(State::Armed, std::memory_order_release);
}

void TransitionEngine::armWithPlan(const SceneTransitionPlan& plan) noexcept {
    if (state_.load(std::memory_order_acquire) != State::Idle) return;
    if (!plan.valid) return;
    directPlan_ = plan;
    hasDirectPlan_ = true;
    // Remplir aussi le plan legacy pour compatibilité (type, from/to, boundary)
    plan_.fromScene = plan.fromScene;
    plan_.toScene = plan.toScene;
    plan_.type = TransitionType::Smooth;
    plan_.executionSample = plan.boundary;
    plan_.valid = true;
    for (int i = 0; i < kMaxSlots; ++i) {
        switch (plan.slots[i].action) {
            case SlotPlanAction::Keep:  plan_.slotActions[i] = SlotAction::Keep; break;
            case SlotPlanAction::Morph: plan_.slotActions[i] = SlotAction::Keep; break; // Morph est Keep côté ancien plan (pas de retrigger)
            case SlotPlanAction::Leave: plan_.slotActions[i] = SlotAction::Exit; break;
            case SlotPlanAction::Enter: plan_.slotActions[i] = SlotAction::Enter; break;
        }
    }
    state_.store(State::Armed, std::memory_order_release);
}

// ─── compilePlan ─────────────────────────────────────────────────────────────

void TransitionEngine::compileDirectPlan(EventScheduler& scheduler) noexcept {
    if (!hasDirectPlan_ || !directPlan_.valid) return;
    for (int i = 0; i < directPlan_.numEvents; ++i) {
        const PlanEvent& pe = directPlan_.events[i];
        EngineEvent ev{};
        ev.time = pe.atSample;
        ev.slot = pe.slot;
        switch (pe.type) {
            case PlanEventType::GainRamp: ev.type = EventType::GainRamp; ev.a = pe.a; ev.b = pe.b; break;
            case PlanEventType::PcmFlip:  ev.type = EventType::PcmFlip;  ev.a = pe.a; ev.b = pe.b; break;
            case PlanEventType::Release:  ev.type = EventType::Release;  ev.a = pe.a; ev.b = pe.b; break;
            case PlanEventType::ModeSet:  ev.type = EventType::ModeSet;  ev.a = pe.a; ev.b = pe.b; break;
            case PlanEventType::RoleSet:  ev.type = EventType::RoleSet;  ev.a = pe.a; ev.b = pe.b; break;
            case PlanEventType::MuteSet:  ev.type = (pe.a > 0.5f) ? EventType::Mute : EventType::Unmute; break;
            case PlanEventType::SemitoneSet: ev.type = EventType::TransposeSet; ev.a = pe.a; break;
            case PlanEventType::SendRamp:    ev.type = EventType::SendRamp; ev.a = pe.a; ev.b = pe.b; break;
            default: continue;
        }
        scheduler.push(ev);
    }
}

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
    State st = state_.load(std::memory_order_acquire);
    switch (st) {
    case State::Idle:
    case State::Preparing:
        break;

    case State::Armed:
        if (ts.samplePos >= plan_.executionSample) {
            if (hasDirectPlan_ && directPlan_.valid) {
                compileDirectPlan(scheduler);
            } else {
                compilePlan(scheduler, plan_.executionSample);
            }
            settleBlocksLeft_ = kSettleBlocks;
            state_.store(State::Executing, std::memory_order_release);
        }
        break;

    case State::Executing:
        if (settleBlocksLeft_ > 0) {
            --settleBlocksLeft_;
        } else {
            state_.store(State::Settling, std::memory_order_release);
        }
        break;

    case State::Settling:
        state_.store(State::Idle, std::memory_order_release);
        hasDirectPlan_ = false;
        break;
    }
}

} // namespace engine
