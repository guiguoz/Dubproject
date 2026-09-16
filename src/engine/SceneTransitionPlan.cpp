#include "engine/SceneTransitionPlan.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace engine {

// ── FNV-1a 64 ─────────────────────────────────────────────────────────────────
static constexpr uint64_t kFNVOffset = 1469598103934665603ull;
static constexpr uint64_t kFNVPrime  = 1099511628211ull;

static uint64_t fnv1a(const std::string& s) noexcept {
    uint64_t h = kFNVOffset;
    for (unsigned char c : s) { h ^= c; h *= kFNVPrime; }
    return h;
}

AssetId assetIdFor(const std::string& filePath, int trimStart, int trimEnd) noexcept {
    if (filePath.empty()) return 0;
    // Canonique : lower + '/' normalisé
    std::string canon;
    canon.reserve(filePath.size());
    for (char ch : filePath) {
        char c = ch;
        if (c == '\\') c = '/';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        canon.push_back(c);
    }
    uint64_t h = fnv1a(canon);
    // Mix trims (évite collision même fichier, trims différents)
    h ^= static_cast<uint64_t>(trimStart + 0x9e3779b9) + (h<<6) + (h>>2);
    h ^= static_cast<uint64_t>(trimEnd   + 0x9e3779b9) * 0x9e3779b97f4a7adull;
    h *= kFNVPrime;
    if (h == 0) h = 1; // réserve 0 = vide
    return h;
}

SlotPlanAction classifySlotForPlan(const SlotConfig& a, const SlotConfig& b,
                                   AssetId idA, AssetId idB) noexcept {
    const bool inA = a.active && idA != 0;
    const bool inB = b.active && idB != 0;
    if (!inA && !inB) return SlotPlanAction::Keep;
    if (!inA && inB)  return SlotPlanAction::Enter;
    if (inA && !inB)  return SlotPlanAction::Leave;
    if (idA != idB) return SlotPlanAction::Enter; // même slot, asset différent → Leave+Enter séquentiel (traité comme Enter avec phase Leave)
    if (a.mode != b.mode) return SlotPlanAction::Morph;
    if (a.semitones != b.semitones) return SlotPlanAction::Morph;
    if (a.muted != b.muted) return SlotPlanAction::Morph;
    if (a.gain != b.gain) return SlotPlanAction::Morph;
    if (static_cast<uint8_t>(a.role) != static_cast<uint8_t>(b.role)) return SlotPlanAction::Morph;
    return SlotPlanAction::Keep;
}

void buildDirectPlan(const SceneData& from, const SceneData& to,
                     int fromIdx, int toIdx, int64_t boundary,
                     double sampleRate,
                     SceneTransitionPlan& out) noexcept {
    out = {};
    out.fromScene = fromIdx;
    out.toScene = toIdx;
    out.boundary = boundary;
    out.valid = true;

    // Durée GainRamp : 10 ms en samples, dérivée de sampleRate
    const int fadeSamples = std::max(1, static_cast<int>(std::round(sampleRate * 0.010)));

    // Morph dub : capturer from/to (4 params) si tous deux utilisés
    if (from.used && to.used) {
        out.morphActive = true;
        out.morphFrom[0] = from.dubDelayFeedback; out.morphTo[0] = to.dubDelayFeedback;
        out.morphFrom[1] = from.dubDelayWet;      out.morphTo[1] = to.dubDelayWet;
        out.morphFrom[2] = from.dubDelayTone;     out.morphTo[2] = to.dubDelayTone;
        out.morphFrom[3] = from.dubDelayDrive;    out.morphTo[3] = to.dubDelayDrive;
        out.morphStart = boundary;
        out.morphDur = std::max<int64_t>(1, static_cast<int64_t>(std::round(sampleRate * 4.0)));
    }

    for (int s = 0; s < kMaxSlots; ++s) {
        const SlotConfig& a = from.slots[s];
        const SlotConfig& b = to.slots[s];
        const AssetId idA = assetIdForSlot(a);
        const AssetId idB = assetIdForSlot(b);
        const bool inA = a.active && idA != 0;
        const bool inB = b.active && idB != 0;

        SlotPlan& sp = out.slots[s];
        sp.asset = idB != 0 ? idB : idA;
        sp.gain = b.gain;
        sp.curGain = a.gain;
        sp.semitones = b.semitones;
        sp.mode = static_cast<uint8_t>(b.mode);
        sp.muted = b.muted;
        sp.role = static_cast<uint8_t>(b.role);

        // Cas particulier : même slot, asset différent → on doit faire LEAVE (A) puis ENTER (B)
        // On modélise comme Enter avec deux phases, mais l'action unique ne suffit pas.
        // DIRECT : on émet Leave (fade A) à T et Enter (PcmFlip+B ramp) à T — mais comme le PCM doit être préchargé, on ne peut pas flipper immédiatement si on vient de fader l'ancien. On séquence : fade A à T (0), puis PcmFlip+fade B à T+fade. C'est l'ancien Replace. On le garde comme Enter avec offset.
        // Simplification : si idA != idB et inA && inB → on classe Morph si on veut garder, mais spec dit même asset différent slot = Leave+Enter. Pour même slot, on doit faire Leave+Enter.
        // On utilise un hack : si inA && inB && idA != idB, on force Enter et le builder émettra les deux.
        SlotPlanAction action;
        if (!inA && !inB) action = SlotPlanAction::Keep;
        else if (!inA && inB) action = SlotPlanAction::Enter;
        else if (inA && !inB) action = SlotPlanAction::Leave;
        else {
            if (idA != idB) action = SlotPlanAction::Enter; // Leave+Enter séquentiel
            else {
                if (a.mode != b.mode || a.semitones != b.semitones || a.muted != b.muted || a.gain != b.gain || a.role != b.role)
                    action = SlotPlanAction::Morph;
                else
                    action = SlotPlanAction::Keep;
            }
        }
        sp.action = action;

        // Émettre events selon action
        auto push = [&](PlanEventType type, int64_t at, float aa, float bb) {
            if (out.numEvents >= 64) return;
            out.events[out.numEvents++] = { at, type, static_cast<uint8_t>(s), aa, bb };
        };

        const float curG = a.gain;
        const float tgtG = b.gain;

        switch (action) {
            case SlotPlanAction::Keep:
                break;
            case SlotPlanAction::Morph: {
                if (curG != tgtG) push(PlanEventType::GainRamp, boundary, static_cast<float>(fadeSamples), tgtG);
                if (a.mode != b.mode) push(PlanEventType::ModeSet, boundary, static_cast<float>(b.mode), 0.f);
                if (a.semitones != b.semitones) push(PlanEventType::SemitoneSet, boundary, b.semitones, 0.f);
                if (a.muted != b.muted) push(PlanEventType::MuteSet, boundary, b.muted ? 1.f : 0.f, 0.f);
                if (a.role != b.role) push(PlanEventType::RoleSet, boundary, static_cast<float>(b.role), 0.f);
                break;
            }
            case SlotPlanAction::Leave: {
                push(PlanEventType::GainRamp, boundary, static_cast<float>(fadeSamples), 0.f);
                push(PlanEventType::Release, boundary + fadeSamples, 0.f, 0.f);
                break;
            }
            case SlotPlanAction::Enter: {
                // Si inA && inB && idA != idB → d'abord Leave de A à T, puis Enter à T+fade
                if (inA && inB && idA != idB) {
                    push(PlanEventType::GainRamp, boundary, static_cast<float>(fadeSamples), 0.f);
                    push(PlanEventType::Release, boundary + fadeSamples, 0.f, 0.f);
                    push(PlanEventType::PcmFlip,  boundary + fadeSamples, 0.f, 0.f);
                    push(PlanEventType::GainRamp, boundary + fadeSamples, static_cast<float>(fadeSamples), tgtG);
                    if (a.mode != b.mode) push(PlanEventType::ModeSet, boundary + fadeSamples, static_cast<float>(b.mode), 0.f);
                    if (a.role != b.role) push(PlanEventType::RoleSet, boundary + fadeSamples, static_cast<float>(b.role), 0.f);
                    if (a.muted != b.muted) push(PlanEventType::MuteSet, boundary + fadeSamples, b.muted ? 1.f : 0.f, 0.f);
                } else {
                    // Vrai ENTER (vide → B) : PCM déjà préchargé, flip à T, gain 0→tgt
                    push(PlanEventType::PcmFlip, boundary, 0.f, 0.f);
                    push(PlanEventType::GainRamp, boundary, static_cast<float>(fadeSamples), tgtG);
                    push(PlanEventType::ModeSet, boundary, static_cast<float>(b.mode), 0.f);
                    push(PlanEventType::RoleSet, boundary, static_cast<float>(b.role), 0.f);
                    push(PlanEventType::MuteSet, boundary, b.muted ? 1.f : 0.f, 0.f);
                }
                break;
            }
        }
    }
    // Trier events par atSample (stable, GainRamp avant Release si même temps)
    std::sort(out.events, out.events + out.numEvents, [](const PlanEvent& l, const PlanEvent& r){
        if (l.atSample != r.atSample) return l.atSample < r.atSample;
        return static_cast<int>(l.type) < static_cast<int>(r.type);
    });
}

} // namespace engine
