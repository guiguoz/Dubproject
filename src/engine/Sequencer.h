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
    void stageForBoundary(const TrackPattern staged[kMaxSlots],
                          int64_t boundarySample) noexcept {
        for (int s = 0; s < kMaxSlots; ++s) staged_[s] = staged[s];
        stagedBoundary_.store(boundarySample, std::memory_order_release);
        stagedActive_.store(true, std::memory_order_release);
    }
    void clearStaged() noexcept {
        stagedActive_.store(false, std::memory_order_release);
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
                const int32_t stepInPattern =
                    static_cast<int32_t>(step % static_cast<int64_t>(pat->numSteps));
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

private:
    // Publie le pattern stagé (audio thread, à la frontière).
    // Copie bornée sans allocation ; aucun lock, UI, I/O.
    void publishStaged() noexcept {
        for (int s = 0; s < kMaxSlots; ++s)
            *patterns_.writeBuffer(s) = staged_[s];
        patterns_.publish();
        stagedActive_.store(false, std::memory_order_release);
    }

    PatternBuffer patterns_;

    // Pattern stagé pour flip quantisé (scènes) + frontière absolue.
    TrackPattern staged_[kMaxSlots] = {};
    std::atomic<int64_t> stagedBoundary_ { 0 };
    std::atomic<bool>    stagedActive_   { false };

    // Gate d'activation par slot (voir ci-dessus)
    std::atomic<int64_t> slotActiveAt_[kMaxSlots] = {};
};

} // namespace engine