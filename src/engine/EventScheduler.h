#pragma once
#include <cstdint>
#include <algorithm>
#include <cassert>

namespace engine {

// Constante spéciale : exécuter au premier sample du bloc suivant (offset 0).
static constexpr int64_t kNextBlock = -1;

enum class EventType : uint8_t {
    Trigger      = 0,
    Release      = 1,
    Mute         = 2,
    Unmute       = 3,
    GainRamp     = 4,
    SceneFlip    = 5,
    TransposeSet = 6,
    SendRamp     = 7,
    PerfFx       = 8,
};

struct EngineEvent {
    int64_t   time;   // sample transport absolu, ou kNextBlock
    EventType type;
    uint8_t   slot;
    float     a, b;   // params selon type
};

// EngineEvent avec offset en samples depuis le début du bloc courant.
// Utilisé par SlotPlayer pour le dispatch temporel sub-bloc.
struct EventWithOffset {
    int32_t   offset;  // position dans le bloc (0 = début)
    EngineEvent ev;
};

// File d'événements triée par time, capacité fixe 256 — ZÉRO allocation.
// Overflow : drop silencieux + compteur debug.
// Thread-model : appelé depuis le thread audio uniquement (pas de lock).
class EventScheduler {
public:
    // Pousse un événement dans la file et maintient l'ordre chronologique.
    // kNextBlock est traité comme time = -1 (toujours en tête).
    void push(EngineEvent ev) noexcept {
        if (count_ >= kCapacity) {
            ++overflows_;
            return;
        }
        // Insertion triée par time (kNextBlock = -1 → toujours premier)
        int insertPos = count_;
        while (insertPos > 0 && events_[insertPos - 1].time > ev.time) {
            events_[insertPos] = events_[insertPos - 1];
            --insertPos;
        }
        events_[insertPos] = ev;
        ++count_;
    }

    // Vide tous les événements.
    void clear() noexcept {
        count_ = 0;
    }

    // Retourne le nombre d'événements dans la file.
    int size() const noexcept { return count_; }

    // Retourne l'événement d'index i (0 = le plus tôt).
    const EngineEvent& at(int i) const noexcept {
        assert(i >= 0 && i < count_);
        return events_[i];
    }

    // Nombre d'overflows (debug).
    int overflowCount() const noexcept { return overflows_; }

    // Traite un bloc [blockStart, blockStart+numSamples).
    // Pour chaque événement dont time < blockEnd (ou time == kNextBlock),
    // appelle cb(offsetInBlock, event) dans l'ordre chronologique.
    // Les événements traités sont retirés de la file.
    // kNextBlock → offset 0.
    template<typename Callback>
    void processBlock(int64_t blockStart, int32_t numSamples, Callback&& cb) noexcept {
        const int64_t blockEnd = blockStart + static_cast<int64_t>(numSamples);

        int readIdx  = 0;
        int writeIdx = 0;

        while (readIdx < count_) {
            const EngineEvent& ev = events_[readIdx];

            // Calcul de l'offset dans le bloc
            int32_t offset = 0;
            bool dispatch  = false;

            if (ev.time == kNextBlock) {
                // Exécuter immédiatement à l'offset 0
                offset   = 0;
                dispatch = true;
            } else if (ev.time >= blockStart && ev.time < blockEnd) {
                offset   = static_cast<int32_t>(ev.time - blockStart);
                dispatch = true;
            }

            if (dispatch) {
                cb(offset, ev);
                ++readIdx;
            } else {
                // Garder cet événement pour un prochain bloc
                if (writeIdx != readIdx) {
                    events_[writeIdx] = ev;
                }
                ++writeIdx;
                ++readIdx;
            }
        }
        count_ = writeIdx;
    }

private:
    static constexpr int kCapacity = 256;
    EngineEvent events_[kCapacity];
    int count_     = 0;
    int overflows_ = 0;
};

} // namespace engine
