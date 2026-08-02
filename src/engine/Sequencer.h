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

// Double-buffer atomique pour les patterns.
// Le thread GUI prépare writeBuffer() puis appelle flip().
// Le thread audio lit readBuffer() sans aucun lock.
struct PatternBuffer {
    TrackPattern tracks[kMaxSlots][2]; // [slot][bufIdx]
    std::atomic<int> activeIdx{0};

    TrackPattern* writeBuffer(int slot) noexcept {
        return &tracks[slot][1 - activeIdx.load(std::memory_order_relaxed)];
    }
    const TrackPattern* readBuffer(int slot) const noexcept {
        return &tracks[slot][activeIdx.load(std::memory_order_acquire)];
    }
    void flip() noexcept {
        activeIdx.store(1 - activeIdx.load(std::memory_order_relaxed),
                        std::memory_order_release);
    }
};

class Sequencer {
public:
    // Génère les Trigger events pour le bloc courant.
    // Aucun état interne de phase : tout découle des helpers de Transport.h.
    //
    // swingFactor : fraction [0,1] du step impair décalé
    //   0.0 = pas de swing
    //   0.6 = steps impairs décalés de 60% d'un samplesPerStep
    void generateEvents(const TransportState& ts,
                        int64_t blockStart, int32_t numSamples,
                        float swingFactor,
                        EventScheduler& scheduler) noexcept {
        if (!ts.playing || numSamples <= 0) return;

        const int64_t blockEnd = blockStart + static_cast<int64_t>(numSamples);

        // Premier step dont le début >= blockStart
        const int64_t firstStep = stepIndexAt(ts, blockStart);
        // Dernier step dont le début < blockEnd
        // On cherche tous les steps N tels que sampleOfStep(N) est dans [blockStart, blockEnd)
        // Le step max possible est stepIndexAt(blockEnd - 1)
        const int64_t lastStep  = stepIndexAt(ts, blockEnd - 1);

        for (int64_t step = firstStep; step <= lastStep; ++step) {
            // Sample de début de ce step (sans swing)
            const int64_t baseSample = sampleOfStep(ts, step);

            // Appliquer le swing : step impair → décaler vers l'avant
            int64_t triggerSample = baseSample;
            if (swingFactor > 0.0f && (step & 1) == 1) {
                const int64_t swingOffset =
                    static_cast<int64_t>(std::round(
                        static_cast<double>(swingFactor) * ts.samplesPerStep));
                triggerSample = baseSample + swingOffset;
            }

            // Vérifier que le sample de trigger est dans ce bloc
            if (triggerSample < blockStart || triggerSample >= blockEnd) continue;

            // Pour chaque slot actif sur ce step
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
