#pragma once
#include <cstdint>
#include <atomic>
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
    void generateEvents(const TransportState& ts,
                        int64_t blockStart, int32_t numSamples,
                        float swingFactor,
                        EventScheduler& scheduler) noexcept {
        if (!ts.playing || numSamples <= 0) return;

        // Marquer que le bloc est lu — met à jour front pour que writeBuffer() ait un back libre.
        patterns_.consume();

        const int64_t blockEnd = blockStart + static_cast<int64_t>(numSamples);

        const int64_t firstStep = stepIndexAt(ts, blockStart);
        const int64_t lastStep  = stepIndexAt(ts, blockEnd - 1);

        for (int64_t step = firstStep; step <= lastStep; ++step) {
            const int64_t baseSample = sampleOfStep(ts, step);

            int64_t triggerSample = baseSample;
            if (swingFactor > 0.0f && (step & 1) == 1) {
                const int64_t swingOffset =
                    static_cast<int64_t>(std::round(
                        static_cast<double>(swingFactor) * ts.samplesPerStep));
                triggerSample = baseSample + swingOffset;
            }

            if (triggerSample < blockStart || triggerSample >= blockEnd) continue;

            for (int slot = 0; slot < kMaxSlots; ++slot) {
                const TrackPattern* pat = patterns_.readBuffer(slot);
                if (pat->numSteps <= 0) continue;
                const int32_t stepInPattern =
                    static_cast<int32_t>(step % static_cast<int64_t>(pat->numSteps));
                if (!pat->steps[stepInPattern]) continue;

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

private:
    PatternBuffer patterns_;
};

} // namespace engine