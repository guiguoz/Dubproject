#pragma once
#include <cstdint>
#include <atomic>
#include <algorithm>
#include <cmath>
#include "engine/Transport.h"
#include "engine/EventScheduler.h"

namespace engine {

static constexpr int kMaxSlots = 9;
static constexpr int kMaxSteps = 512;

struct TrackPattern {
    bool    steps[kMaxSteps] = {};
    int32_t numSteps         = 16;
};

// Triple-buffer atomique pour les patterns — sûr par construction.
//
// Invariants :  front, ready, writeIdx forment toujours une permutation de {0,1,2}.
//
// Rôles :
//   front    — buffer que le consommateur lit (mis à jour par consume())
//   ready    — buffer le plus récemment publié (lu par le consommateur via readBuffer())
//   writeIdx — buffer libre pour l'écriture par le producteur
//
// Sémantique :
//   Producteur : writeBuffer(s) → écrit dans writeIdx → publish() → swap(writeIdx, ready)
//   Consommateur : readBuffer(s) → lit de ready → consume() → swap(front, ready)
//   Note : consume() peut être appelé ou non — readBuffer() lit toujours ready.
//
struct PatternBuffer {
    TrackPattern tracks[kMaxSlots][3]; // [slot][bufIdx]

    // front = buffer lu par le consommateur (consommateur le met à jour)
    // ready = buffer le plus récemment publié (producteur le met à jour)
    // writeIdx = buffer libre pour l'écriture (producteur le calcule)
    std::atomic<int> front{0};   // consommateur : lu après consume()
    std::atomic<int> ready{1}; // producteur : lu après publish()

    TrackPattern* writeBuffer(int slot) noexcept {
        // back = 3 - front - ready (le buffer ni lu par consommateur, ni prêt par producteur)
        const int f = front.load(std::memory_order_acquire);
        const int r = ready.load(std::memory_order_acquire);
        const int b = 3 - f - r;
        return &tracks[slot][b];
    }

    // Retourne le buffer le plus récemment publié.
    // L'acquisition garantit que le consommateur voit toutes les écritures du producteur.
    const TrackPattern* readBuffer(int slot) const noexcept {
        return &tracks[slot][ready.load(std::memory_order_acquire)];
    }

    // Producteur : publie le buffer écrit en échangeant writeIdx ↔ ready.
    void publish() noexcept {
        const int f = front.load(std::memory_order_acquire);
        const int r = ready.load(std::memory_order_acquire);
        const int b = 3 - f - r;
        ready.store(b, std::memory_order_release);
    }

    // Consommateur : échange front ↔ ready après avoir fini de lire.
    // Optionnel si on veut garder front à jour, mais readBuffer() utilise ready.
    void consume() noexcept {
        const int f = front.load(std::memory_order_acquire);
        const int r = ready.load(std::memory_order_acquire);
        front.store(r, std::memory_order_release);
    }

    // Alias rétrocompatible pour publish().
    void flip() noexcept { publish(); }
};

class Sequencer {
public:
    // ── Flip de pattern quantisé (scènes, P0 sync) ──────────────────────────
    // stageForBoundary() mémorise un pattern + frontière (message thread) ;
    // le flip est appliqué dans generateEvents quand la frontière est franchie
    // (audio thread) — aucun changement musical à un instant arbitraire.
    // L'édition live (flipPatternBuffer) reste immédiate.
    // Discipline : un seul stage en cours (navigateScene est bloqué pendant
    // une transition) ; toute publication immédiate annule le stage.
    //
    // Surcharge 3 arguments : la scène ENTRANTE redémarre sur son step 0 à la
    // frontière (phase recalée), quelles que soient les longueurs respectives
    // des patterns sortant/entrant. Sans le 3e argument, la phase est conservée
    // (comportement historique : index = step % numSteps).
    void stageForBoundary(const TrackPattern staged[kMaxSlots],
                          int64_t boundarySample) noexcept {
        stageForBoundaryImpl(staged, boundarySample, kKeepPhase);
    }
    void stageForBoundary(const TrackPattern staged[kMaxSlots],
                          int64_t boundarySample,
                          int64_t boundaryStep) noexcept {
        stageForBoundaryImpl(staged, boundarySample, boundaryStep);
    }
    void clearStaged() noexcept {
        stagedActive_.store(false, std::memory_order_release);
        // Annuler aussi les gates ENTER : sinon, si une transition est abandonnée
        // en cours de route, les slotActiveAt_ restent à un sample futur → les
        // slots silencieux au redémarrage du transport (blockStart repart de 0).
        for (int s = 0; s < kMaxSlots; ++s)
            slotActiveAt_[s].store(0, std::memory_order_release);
        // Un stage annulé = aucun recalage de phase en attente : on repart de la
        // grille globale (sinon la base resterait à un step > 0 et le transport
        // redémarrant à 0 jouerait un pattern décalé).
        resetPatternPhase();
    }
    bool hasStaged() const noexcept {
        return stagedActive_.load(std::memory_order_acquire);
    }

    void generateEvents(const TransportState& ts,
                        int64_t blockStart, int32_t numSamples,
                        float swingFactor,
                        EventScheduler& scheduler) noexcept {
        if (!ts.playing || numSamples <= 0) return;

        const int64_t blockEnd = blockStart + static_cast<int64_t>(numSamples);

        // Frontière stagée dans ce bloc : générer l'ancien pattern avant,
        // publier, puis générer le nouveau après — flip sample-accurate,
        // même sample que les GainRamps de la transition.
        if (stagedActive_.load(std::memory_order_acquire)) {
            const int64_t boundary = stagedBoundary_.load(std::memory_order_acquire);
            if (boundary > blockStart && boundary < blockEnd) {
                const int32_t preLen = static_cast<int32_t>(boundary - blockStart);
                emitRange(ts, blockStart, preLen, swingFactor, scheduler);
                publishStaged();
                emitRange(ts, boundary, numSamples - preLen, swingFactor, scheduler);
                return;
            }
            if (boundary <= blockStart) {
                publishStaged();   // rattrapage : frontière déjà passée
            }
            // boundary >= blockEnd : flip au bloc suivant, garder l'ancien pattern.
        }
        emitRange(ts, blockStart, numSamples, swingFactor, scheduler);
    }

    // Émet les triggers dont le sample tombe dans [rangeStart, rangeStart+rangeLen).
    void emitRange(const TransportState& ts,
                   int64_t rangeStart, int32_t rangeLen,
                   float swingFactor,
                   EventScheduler& scheduler) noexcept {
        if (!ts.playing || rangeLen <= 0) return;

        const int64_t blockEnd = rangeStart + static_cast<int64_t>(rangeLen);

        // Fenêtre élargie vers le passé : un step dont la base précède le bloc
        // peut déclencher DANS le bloc via le swing. Chaque trigger appartient
        // à exactement un bloc (test d'appartenance sur le trigger, pas la base)
        // → ni perte aux alignements grille/bloc, ni doublon. Clamp défensif :
        // setSwing() n'est pas borné côté façade.
        const double swClamped = std::clamp(static_cast<double>(swingFactor), 0.0, 1.0);
        const int64_t maxSwing = static_cast<int64_t>(
            std::ceil(swClamped * ts.samplesPerStep));

        const int64_t firstStep = stepIndexAt(ts, rangeStart - maxSwing);
        const int64_t lastStep  = stepIndexAt(ts, blockEnd - 1);

        for (int64_t step = firstStep; step <= lastStep; ++step) {
            if (step < 0) continue;   // le transport démarre à 0
            const int64_t baseSample = sampleOfStep(ts, step);

            int64_t triggerSample = baseSample;
            if (swClamped > 0.0 && (step & 1) == 1) {
                const int64_t swingOffset =
                    static_cast<int64_t>(std::round(swClamped * ts.samplesPerStep));
                triggerSample = baseSample + swingOffset;
            }

            if (triggerSample < rangeStart || triggerSample >= blockEnd) continue;

            for (int slot = 0; slot < kMaxSlots; ++slot) {
                const TrackPattern* pat = patterns_.readBuffer(slot);
                if (pat->numSteps <= 0) continue;
                // Base de phase : un step antérieur à la frontière appartient au
                // cycle PRÉCÉDENT (sa base a été jouée par l'ancien pattern). Seul
                // cas atteignable : la fenêtre de swing relâchée après la
                // frontière — sans ce garde, ce step déclencherait la DERNIÈRE
                // case du pattern entrant (index négatif bouclé), soit un coup
                // fantôme pile à la bascule.
                const int64_t base = phaseBase_[slot].load(std::memory_order_acquire);
                if (base > 0 && step < base) continue;
                const int32_t stepInPattern = stepInPatternFor(slot, step, pat->numSteps);
                if (!pat->steps[stepInPattern]) continue;
                // Gate ENTER : avant PcmFlip, ignorer les triggers de B
                if (triggerSample < slotActiveAt_[slot].load(std::memory_order_acquire)) continue;

                EngineEvent ev{};
                ev.time = triggerSample;
                ev.type = EventType::Trigger;
                ev.slot = static_cast<uint8_t>(slot);
                ev.a    = 0.0f;
                ev.b    = 0.0f;
                scheduler.push(ev);
            }
        }
    }

    PatternBuffer&       patterns()       noexcept { return patterns_; }
    const PatternBuffer& patterns() const noexcept { return patterns_; }

    // Activation gate pour ENTER : avant PcmFlip(atSample), les triggers du
    // pattern B pour ce slot sont ignorés (évite double trigger / pré-écho).
    // 0 = actif immédiatement (KEEP/MORPH/LEAVE). ENTER = PcmFlip time.
    void setSlotActiveAt(int slot, int64_t atSample) noexcept {
        if (slot < 0 || slot >= kMaxSlots) return;
        slotActiveAt_[slot].store(atSample, std::memory_order_release);
    }
    void clearSlotActiveAt(int slot) noexcept {
        if (slot < 0 || slot >= kMaxSlots) return;
        slotActiveAt_[slot].store(0, std::memory_order_release);
    }

    // ── Phase de pattern (recalage à la frontière de transition) ─────────────
    // phaseBase_[slot] = index de step GLOBAL qui correspond au step 0 du pattern
    // de ce slot. 0 (défaut) = grille globale depuis le démarrage du transport.
    // Posé à la frontière de transition pour que la scène entrante démarre sur
    // son propre step 0, même si la frontière n'est pas un multiple de sa
    // longueur (ex. A = 3 mesures → frontière au step 48, B = 2 mesures → 48 % 32
    // vaudrait 16 sans recalage).
    int64_t phaseBase(int slot) const noexcept {
        if (slot < 0 || slot >= kMaxSlots) return 0;
        return phaseBase_[slot].load(std::memory_order_acquire);
    }
    void resetPatternPhase() noexcept {
        for (int s = 0; s < kMaxSlots; ++s)
            phaseBase_[s].store(0, std::memory_order_release);
    }
    // Longueur du pattern publié (ce que l'audio joue réellement) — pour l'UI.
    int32_t numStepsForSlot(int slot) const noexcept {
        if (slot < 0 || slot >= kMaxSlots) return 16;
        const int32_t n = patterns_.readBuffer(slot)->numSteps;
        return n > 0 ? n : 16;
    }

private:
    static constexpr int64_t kKeepPhase = INT64_MIN;   // « ne pas recaler la phase »

    // Index du step DANS le pattern du slot, base de phase déduite.
    int32_t stepInPatternFor(int slot, int64_t step, int32_t numSteps) const noexcept {
        if (numSteps <= 0) return 0;
        const int64_t base = phaseBase_[slot].load(std::memory_order_acquire);
        int64_t rel = step - base;
        rel %= numSteps;
        if (rel < 0) rel += numSteps;   // les steps antérieurs à la base bouclent en fin de pattern
        return static_cast<int32_t>(rel);
    }

    void stageForBoundaryImpl(const TrackPattern staged[kMaxSlots],
                              int64_t boundarySample,
                              int64_t phaseBaseStep) noexcept {
        for (int s = 0; s < kMaxSlots; ++s) staged_[s] = staged[s];
        stagedBoundary_.store(boundarySample, std::memory_order_release);
        stagedPhaseBase_.store(phaseBaseStep, std::memory_order_release);
        stagedActive_.store(true, std::memory_order_release);
    }

    // Publie le pattern stagé (audio thread, à la frontière).
    // Copie bornée sans allocation ; aucun lock, UI, I/O.
    void publishStaged() noexcept {
        const int64_t base = stagedPhaseBase_.load(std::memory_order_acquire);
        if (base != kKeepPhase)
            for (int s = 0; s < kMaxSlots; ++s)
                phaseBase_[s].store(base, std::memory_order_release);
        for (int s = 0; s < kMaxSlots; ++s)
            *patterns_.writeBuffer(s) = staged_[s];
        patterns_.publish();
        stagedActive_.store(false, std::memory_order_release);
    }

    PatternBuffer patterns_;

    // Pattern stagé pour flip quantisé (scènes) + frontière absolue.
    TrackPattern staged_[kMaxSlots] = {};
    std::atomic<int64_t> stagedBoundary_  { 0 };
    std::atomic<int64_t> stagedPhaseBase_ { kKeepPhase };
    std::atomic<bool>    stagedActive_    { false };

    // Base de phase par slot (voir phaseBase()).
    std::atomic<int64_t> phaseBase_[kMaxSlots] = {};

    // Gate d'activation par slot (voir ci-dessus)
    std::atomic<int64_t> slotActiveAt_[kMaxSlots] = {};
};

} // namespace engine