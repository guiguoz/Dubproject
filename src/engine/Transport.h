#pragma once
#include <atomic>
#include <cstdint>
#include <cmath>

namespace engine {

// Snapshot POD distribué à tout le moteur, une fois par bloc.
// Tous les champs sont calculés à partir de samplePos + bpm + sampleRate.
struct TransportState {
    int64_t samplePos      = 0;   // fin du bloc courant == premier sample du bloc suivant
    int64_t blockStart     = 0;   // premier sample du bloc courant
    double  sampleRate     = 44100.0;
    double  bpm            = 120.0;
    double  samplesPerBeat = 22050.0;   // sampleRate * 60 / bpm
    double  samplesPerStep = 5512.5;    // samplesPerBeat / 4  (double-croches)
    bool    playing        = false;
};

// ─── Helpers purs (calculs uniquement, pas d'état) ──────────────────────────

// Retourne la position en beats (fractionnaire) pour un sample absolu.
inline double beatAt(const TransportState& ts, int64_t pos) noexcept {
    return static_cast<double>(pos) / ts.samplesPerBeat;
}

// Retourne l'index de step monotone (int64, jamais de modulo global).
// Utilise double pour éviter l'erreur d'arrondi sur de grandes valeurs.
inline int64_t stepIndexAt(const TransportState& ts, int64_t pos) noexcept {
    return static_cast<int64_t>(static_cast<double>(pos) / ts.samplesPerStep);
}

// Retourne l'offset en samples du PREMIER sample appartenant au step N.
// Utilise ceil : le step N commence au premier entier >= N * samplesPerStep.
// Garantit que stepIndexAt(sampleOfStep(N)) == N même pour des samplesPerStep
// non entiers (BPM non ronds comme 133.7).
inline int64_t sampleOfStep(const TransportState& ts, int64_t stepIndex) noexcept {
    return static_cast<int64_t>(std::ceil(static_cast<double>(stepIndex) * ts.samplesPerStep));
}

// Samples restants jusqu'au prochain step N (peut être 0 si exactement dessus).
inline int64_t samplesUntilStep(const TransportState& ts, int64_t stepIndex) noexcept {
    return sampleOfStep(ts, stepIndex) - ts.samplePos;
}

// Sample absolu de la prochaine frontière qui est un multiple de stepsPerCycle.
inline int64_t nextBoundary(const TransportState& ts, int64_t stepsPerCycle) noexcept {
    if (stepsPerCycle <= 0) return ts.samplePos;
    int64_t cur     = stepIndexAt(ts, ts.samplePos);
    int64_t nextStep = (cur / stepsPerCycle + 1) * stepsPerCycle;
    return sampleOfStep(ts, nextStep);
}

// ─── Spinlock non-récursif (basé sur atomic_flag) ──────────────────────────
// Object lock-free pour proteger des sections critiques courtes.
// Contention quasi-nulle dans notre cas (message thread lit ~60 Hz, audio écrit 1x/bloc).
class SpinLock {
public:
    struct ScopedLockType {
        explicit ScopedLockType(SpinLock& sl) noexcept : lock_(sl) { lock_.lock(); }
        ~ScopedLockType() noexcept { lock_.unlock(); }
        ScopedLockType(const ScopedLockType&) = delete;
        ScopedLockType& operator=(const ScopedLockType&) = delete;
    private:
        SpinLock& lock_;
    };

    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire))
            ; // spin
    }
    bool try_lock() noexcept {
        return !flag_.test_and_set(std::memory_order_acquire);
    }
    void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }
private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// ─── Classe Transport ────────────────────────────────────────────────────────

class Transport {
public:
    // Appelé au changement de device ou au démarrage.
    // Remet samplePos à 0 et recalcule les dérivés.
    // Pas de lock nécessaire : appelé avant le démarrage audio.
    void prepare(double sampleRate, double bpm) noexcept {
        state_.sampleRate     = sampleRate;
        state_.bpm            = bpm;
        state_.samplesPerBeat = sampleRate * 60.0 / bpm;
        state_.samplesPerStep = state_.samplesPerBeat / 4.0;
        state_.samplePos      = 0;
        state_.blockStart     = 0;
        state_.playing        = false;
    }

    // Démarre la lecture depuis 0 (départ propre).
    void play() noexcept {
        SpinLock::ScopedLockType scoped(spinlock_);
        state_.samplePos  = 0;
        state_.blockStart = 0;
        state_.playing    = true;
    }

    // Stop — l'arrêt audio (fades) est géré par le scheduler, pas ici.
    void stop() noexcept {
        SpinLock::ScopedLockType scoped(spinlock_);
        state_.playing = false;
    }

    // Change le BPM sans reset de samplePos ni de playing.
    void setBpm(double bpm) noexcept {
        if (bpm <= 0.0) return;
        SpinLock::ScopedLockType scoped(spinlock_);
        state_.bpm            = bpm;
        state_.samplesPerBeat = state_.sampleRate * 60.0 / bpm;
        state_.samplesPerStep = state_.samplesPerBeat / 4.0;
    }

    // Appelé UNE fois en tête de chaque callback audio.
    // Avance samplePos et retourne le snapshot const pour ce bloc.
    // blockStart est figé au DÉBUT du bloc courant (avant l'avancement) :
    // le snapshot décrit donc le bloc [blockStart, samplePos).
    const TransportState& advance(int32_t numSamples) noexcept {
        SpinLock::ScopedLockType scoped(spinlock_);
        if (state_.playing) {
            state_.blockStart = state_.samplePos;
            state_.samplePos += static_cast<int64_t>(numSamples);
        }
        return state_;
    }

    // Audio thread only — retourne la ref interne (pas de lock).
    const TransportState& state() const noexcept { return state_; }

    // Thread-safe : copie du snapshot pour le message thread.
    TransportState snapshot() const noexcept {
        SpinLock::ScopedLockType scoped(spinlock_);
        return state_;
    }

private:
    TransportState state_;
    mutable SpinLock spinlock_;
};

} // namespace engine
