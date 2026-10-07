#include "engine/TransitionPolicy.h"
#include <algorithm>
#include <cmath>

namespace engine {

int64_t TransitionPolicy::samplesPerBeat(const PolicyContext& ctx) noexcept {
    if (ctx.bpm <= 0.0) return static_cast<int64_t>(ctx.sampleRate * 60.0 / 120.0);
    return static_cast<int64_t>(std::round(ctx.sampleRate * 60.0 / ctx.bpm));
}
int64_t TransitionPolicy::samplesPerStep(const PolicyContext& ctx) noexcept {
    return samplesPerBeat(ctx) / 4;
}

PolicyType TransitionPolicy::choose(const SceneTransitionPlan& plan) noexcept {
    if (forceDirect_) return PolicyType::Direct;
    if (!plan.valid) return PolicyType::Direct;
    // Audit continuité : forte variation sans socle commun → fallback DIRECT
    // Métrique V1 : (KEEP+MORPH) avec asset non vide < 2 → pas assez de socle (ex. 2→6 sans overlap)
    {
        int keepMorph = 0;
        for (int s = 0; s < kMaxSlots; ++s) if ((plan.slots[s].action == SlotPlanAction::Keep || plan.slots[s].action == SlotPlanAction::Morph) && plan.slots[s].asset != 0) keepMorph++;
        if (keepMorph < 2) return PolicyType::Direct;
    }
    int enters = 0, leaves = 0;
    for (int s = 0; s < kMaxSlots; ++s) {
        const auto a = plan.slots[s].action;
        if (a == SlotPlanAction::Enter) {
            // Distinguer vrai ENTER vs Replace (même slot asset différent)
            // Dans le plan actuel, Replace est encodé comme Enter avec inA&&inB&&id diff,
            // on le compte comme 1 Leave + 1 Enter pour le net.
            // On ne peut pas distinguer sans les ids ici, donc on compte Enter comme 1 et on
            // déduit Leave si le plan contient à la fois GainRamp à T et PcmFlip à T+fade pour ce slot.
            // Simplification : chaque Enter compte 1, chaque Leave compte 1. Pour Replace, le plan
            // a 2 GainRamps + 1 Release + 1 PcmFlip, mais action est Enter seul → on compte 1 Enter.
            // Pour garder le net équilibré, on compte Replace comme 1 Enter + 1 Leave en regardant
            // si le slot avait à la fois un GainRamp à boundary et un PcmFlip décalé. Pour l'instant,
            // on approxime : si action==Enter et qu'il y a 2 GainRamps pour ce slot, c'est un Replace.
            int gainRamps = 0;
            for (int i = 0; i < plan.numEvents; ++i) if (plan.events[i].slot == s && plan.events[i].type == PlanEventType::GainRamp) gainRamps++;
            if (gainRamps == 2) { enters++; leaves++; } else { enters++; }
        } else if (a == SlotPlanAction::Leave) {
            leaves++;
        }
    }
    const int net = enters - leaves;
    if (net >= 3) return PolicyType::Build;
    if (net <= -3) return PolicyType::Breakdown;
    // Churn équilibré / remplacement / ambiguïté → DIRECT (fallback sacré)
    return PolicyType::Direct;
}

// ── V2 : offset en beats selon le rôle ───────────────────────────────────────

static int v2BuildBeatOffset(SlotRole r) noexcept {
    switch (r) {
        case SlotRole::Kick:
        case SlotRole::Bass:
        case SlotRole::Snare:
        case SlotRole::Drum:    return 0;
        case SlotRole::Perc:    return 1;
        case SlotRole::Pad:
        case SlotRole::Melodic:
        case SlotRole::Loop:    return 2;
        case SlotRole::Fx:      return 3;
        default:                return 1;
    }
}

static int v2BreakdownBeatOffset(SlotRole r) noexcept {
    switch (r) {
        case SlotRole::Fx:      return -3;
        case SlotRole::Pad:
        case SlotRole::Melodic:
        case SlotRole::Loop:    return -2;
        case SlotRole::Perc:
        case SlotRole::Snare:   return -1;
        case SlotRole::Kick:
        case SlotRole::Bass:
        case SlotRole::Drum:    return 0;
        default:                return -1;
    }
}

static void shiftSlotEvents(SceneTransitionPlan& plan, int s, int64_t newAt, PolicyType p) noexcept {
    // Option A — clamp cycle sortant intangible : si le décalage amènerait un événement
    // avant la frontière (Breakdown), ne pas décaler ce slot. Les événements restent à leur
    // position d'origine (GainRamp = boundary, Release = boundary + fade) → invariant préservé.
    if (newAt < plan.boundary) return;
    for (int i = 0; i < plan.numEvents; ++i) {
        if (plan.events[i].slot != s) continue;
        const auto type = plan.events[i].type;
        if (p == PolicyType::Build) {
            if (type == PlanEventType::PcmFlip) {
                plan.events[i].atSample = newAt + (plan.events[i].atSample - plan.boundary);
            } else if (type == PlanEventType::GainRamp) {
                if (plan.events[i].atSample != plan.boundary) {
                    plan.events[i].atSample = newAt + (plan.events[i].atSample - plan.boundary);
                } else {
                    int cnt = 0;
                    for (int k = 0; k < plan.numEvents; ++k)
                        if (plan.events[k].slot == s && plan.events[k].type == PlanEventType::GainRamp) cnt++;
                    if (cnt == 1) plan.events[i].atSample = newAt;
                }
            } else if (type == PlanEventType::ModeSet || type == PlanEventType::RoleSet || type == PlanEventType::MuteSet) {
                if (plan.events[i].atSample != plan.boundary)
                    plan.events[i].atSample = newAt + (plan.events[i].atSample - plan.boundary);
                else
                    plan.events[i].atSample = newAt;
            }
        } else {
            if (type == PlanEventType::GainRamp || type == PlanEventType::Release)
                plan.events[i].atSample = newAt + (plan.events[i].atSample - plan.boundary);
        }
    }
}

static void applyV2(SceneTransitionPlan& plan, int64_t beatSamples, int64_t measureSamples, PolicyType p) noexcept {
    for (int s = 0; s < kMaxSlots; ++s) {
        const auto act = plan.slots[s].action;
        if (p == PolicyType::Build && act != SlotPlanAction::Enter) continue;
        if (p == PolicyType::Breakdown && act != SlotPlanAction::Leave) continue;
        const auto role = static_cast<SlotRole>(plan.slots[s].role);
        int64_t offset = 0;
        if (p == PolicyType::Build) {
            offset = static_cast<int64_t>(v2BuildBeatOffset(role)) * beatSamples;
            if (offset >= measureSamples) offset = measureSamples - 1;
        } else {
            offset = static_cast<int64_t>(v2BreakdownBeatOffset(role)) * beatSamples;
            if (offset < -measureSamples) offset = -measureSamples;
        }
        shiftSlotEvents(plan, s, plan.boundary + offset, p);
    }
}

// ── V1 : tri par priorité de rôle ────────────────────────────────────────────

static int rolePriority(SlotRole r) noexcept {
    // Ordre BUILD : Kick d'abord (ancrage rythmique), puis Bass, Snare, etc.
    switch (r) {
        case SlotRole::Kick: return 0;
        case SlotRole::Bass: return 1;
        case SlotRole::Snare: return 2;
        case SlotRole::Pad: return 3;
        case SlotRole::Melodic: return 4;
        case SlotRole::Perc: return 5;
        case SlotRole::Fx: return 6;
        case SlotRole::Loop: return 7;
        case SlotRole::Drum: return 8;
        default: return 9;
    }
}

void TransitionPolicy::apply(SceneTransitionPlan& plan, const PolicyContext& ctx) noexcept {
    if (!plan.valid) return;
    const PolicyType p = choose(plan);
    if (p == PolicyType::Direct) return; // référence 125d27b, strictement identique

    const int64_t beatSamples    = samplesPerBeat(ctx);
    const int64_t measureSamples = beatSamples * 4;

    if (policyMode_ == PolicyMode::V2) {
        applyV2(plan, beatSamples, measureSamples, p);
        std::sort(plan.events, plan.events + plan.numEvents, [](const PlanEvent& a, const PlanEvent& b) {
            if (a.atSample != b.atSample) return a.atSample < b.atSample;
            return static_cast<int>(a.type) < static_cast<int>(b.type);
        });
        return;
    }

    // ── V1 : distribution linéaire par priorité de rôle ──────────────────────
    // KEEP sacré : aucun événement ne touche un slot Keep.
    struct Item { int slot; int prio; };
    std::array<Item, kMaxSlots> items;
    int nItems = 0;
    for (int s = 0; s < kMaxSlots; ++s) {
        const auto act = plan.slots[s].action;
        if (p == PolicyType::Build && act == SlotPlanAction::Enter) {
            items[nItems++] = { s, rolePriority(static_cast<SlotRole>(plan.slots[s].role)) };
        } else if (p == PolicyType::Breakdown && act == SlotPlanAction::Leave) {
            int prio = rolePriority(static_cast<SlotRole>(plan.slots[s].role));
            prio = 10 - prio; // inverse : décoratif d'abord, structurel en dernier
            items[nItems++] = { s, prio };
        }
    }
    if (nItems == 0) return;
    std::sort(items.begin(), items.begin()+nItems, [](const Item& a, const Item& b){
        if (a.prio != b.prio) return a.prio < b.prio;
        return a.slot < b.slot;
    });

    for (int idx = 0; idx < nItems; ++idx) {
        const int slot = items[idx].slot;
        int64_t offset = 0;
        if (p == PolicyType::Build) {
            offset = static_cast<int64_t>(idx) * beatSamples;
            if (offset >= measureSamples) offset = (static_cast<int64_t>(idx) * measureSamples) / nItems;
        } else {
            if (nItems == 1) offset = 0;
            else offset = -measureSamples + (static_cast<int64_t>(idx) * measureSamples) / (nItems - 1);
        }
        shiftSlotEvents(plan, slot, plan.boundary + offset, p);
    }

    std::sort(plan.events, plan.events + plan.numEvents, [](const PlanEvent& a, const PlanEvent& b){
        if (a.atSample != b.atSample) return a.atSample < b.atSample;
        return static_cast<int>(a.type) < static_cast<int>(b.type);
    });
}

} // namespace engine
