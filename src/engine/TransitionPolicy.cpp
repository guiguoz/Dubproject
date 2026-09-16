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
    if (!plan.valid) return PolicyType::Direct;
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

    const int64_t beatSamples = samplesPerBeat(ctx);
    const int64_t measureSamples = beatSamples * 4; // 1 mesure = 4 temps (16 steps)
    // KEEP sacré : aucun événement ne touche un slot Keep.

    // Collecter les slots concernés
    struct Item { int slot; int prio; };
    std::array<Item, kMaxSlots> items;
    int nItems = 0;
    for (int s = 0; s < kMaxSlots; ++s) {
        const auto act = plan.slots[s].action;
        if (p == PolicyType::Build && act == SlotPlanAction::Enter) {
            items[nItems++] = { s, rolePriority(static_cast<SlotRole>(plan.slots[s].role)) };
        } else if (p == PolicyType::Breakdown && act == SlotPlanAction::Leave) {
            // Ordre inverse pour BREAKDOWN : décoratif d'abord, structurel en dernier
            // On inverse la priorité : FX/PERC/HAT (prio élevé) d'abord
            int prio = rolePriority(static_cast<SlotRole>(plan.slots[s].role));
            prio = 10 - prio; // inverse
            items[nItems++] = { s, prio };
        }
    }
    if (nItems == 0) return;
    std::sort(items.begin(), items.begin()+nItems, [](const Item& a, const Item& b){
        if (a.prio != b.prio) return a.prio < b.prio;
        return a.slot < b.slot;
    });

    // Pour chaque item, décaler ses événements
    // BUILD : ENTER dans [T, T+measure) — répartis sur beats à partir de T
    // BREAKDOWN : LEAVE dans [T-measure, T] — décoratif à T-measure, KICK à T
    for (int idx = 0; idx < nItems; ++idx) {
        const int slot = items[idx].slot;
        int64_t offset = 0;
        if (p == PolicyType::Build) {
            offset = static_cast<int64_t>(idx) * beatSamples;
            // Clamper dans [0, measure) — si trop d'ENTER, on compresse sur steps
            if (offset >= measureSamples) offset = (static_cast<int64_t>(idx) * measureSamples) / nItems;
        } else { // Breakdown
            // Premier LEAVE à T-measure, dernier à T
            if (nItems == 1) offset = 0;
            else offset = -measureSamples + (static_cast<int64_t>(idx) * measureSamples) / (nItems - 1);
        }
        const int64_t newAt = plan.boundary + offset;
        for (int i = 0; i < plan.numEvents; ++i) {
            if (plan.events[i].slot != slot) continue;
            const auto type = plan.events[i].type;
            if (p == PolicyType::Build) {
                // BUILD : décaler PcmFlip et le GainRamp qui le suit (à T ou T+fade)
                // Pour vrai ENTER (2 events à T), les deux sont décalés.
                // Pour Replace (4 events), seuls ceux à T+fade sont décalés (le Leave à T reste)
                if (type == PlanEventType::PcmFlip) {
                    plan.events[i].atSample = newAt + (plan.events[i].atSample - plan.boundary);
                    // Le GainRamp qui suit le PcmFlip a même atSample, il sera décalé aussi via la boucle
                } else if (type == PlanEventType::GainRamp) {
                    // Si cet event est à T+fade pour un Replace, il a déjà été décalé via PcmFlip offset?
                    // On décale GainRamp si son atSample >= boundary+fade (c-à-d second ramp), sinon on le laisse à T pour le Leave.
                    // Distinction : si slot est Enter et qu'il y a 2 GainRamps, le premier est à T (Leave), le second à T+fade (Enter)
                    // On décale uniquement le second.
                    // On détecte en comptant les GainRamps pour ce slot : le second a atSample != boundary
                    // Simplification : si atSample != plan.boundary, c'est le second → décaler
                    if (plan.events[i].atSample != plan.boundary) {
                        plan.events[i].atSample = newAt + (plan.events[i].atSample - plan.boundary);
                    } else {
                        // Pour vrai ENTER (pas Replace), le seul GainRamp est à T → décaler
                        // On distingue vrai ENTER vs Replace en comptant les GainRamps : vrai ENTER a 1 GainRamp à T
                        // Replace a 2 GainRamps (T et T+fade). On a déjà géré le second ci-dessus, le premier reste à T.
                        // Pour vrai ENTER, on veut décaler le GainRamp à T également.
                        // On détermine si c'est vrai ENTER : compte GainRamps ==1 → décaler
                        int cnt=0; for(int k=0;k<plan.numEvents;++k) if(plan.events[k].slot==slot && plan.events[k].type==PlanEventType::GainRamp) cnt++;
                        if (cnt==1) plan.events[i].atSample = newAt;
                    }
                } else if (type == PlanEventType::ModeSet || type == PlanEventType::RoleSet || type == PlanEventType::MuteSet) {
                    // Ces sets sont à T (ou T+fade pour Replace) → même logique que GainRamp
                    if (plan.events[i].atSample != plan.boundary) plan.events[i].atSample = newAt + (plan.events[i].atSample - plan.boundary);
                    else {
                        int cnt=0; for(int k=0;k<plan.numEvents;++k) if(plan.events[k].slot==slot && (plan.events[k].type==PlanEventType::ModeSet||plan.events[k].type==PlanEventType::RoleSet)) cnt++;
                        // Pour simplifier, décaler aussi
                        plan.events[i].atSample = newAt;
                    }
                }
            } else if (p == PolicyType::Breakdown) {
                // BREAKDOWN : décaler GainRamp à T et Release à T+fade
                if (type == PlanEventType::GainRamp || type == PlanEventType::Release) {
                    plan.events[i].atSample = newAt + (plan.events[i].atSample - plan.boundary);
                }
            }
        }
    }
    // Re-trier par atSample pour garantir l'ordre chronologique
    std::sort(plan.events, plan.events + plan.numEvents, [](const PlanEvent& a, const PlanEvent& b){
        if (a.atSample != b.atSample) return a.atSample < b.atSample;
        return static_cast<int>(a.type) < static_cast<int>(b.type);
    });
}

} // namespace engine
