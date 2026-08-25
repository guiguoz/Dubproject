#pragma once
#include <cstdint>
#include <atomic>
#include <vector>
#include "engine/Transport.h"
#include "engine/EventScheduler.h"
#include "engine/StretchConform.h"

namespace engine {

// ─── Types de base ───────────────────────────────────────────────────────────

enum class PlayMode : uint8_t { OneShot = 0, Free = 1, LoopSync = 2 };
enum class SlotRole  : uint8_t { Kick=0, Bass=1, Snare=2, Pad=3, Melodic=4, Perc=5, Fx=6, Loop=7, Drum=8,
                                  Unknown=255 // sentinelle : classifieur indisponible ou confiance nulle
                                };

// PCM immuable après chargement (message thread uniquement).
struct SlotPcm {
    std::vector<float> data;      // interleaved stéréo OU mono
    int   numChannels = 1;        // 1 ou 2
    int   numFrames   = 0;
    float sampleRate  = 44100.f;
};

// Paramètres atomiques : lus depuis le thread audio, écrits depuis le message thread.
struct SlotParams {
    std::atomic<PlayMode>  mode      {PlayMode::OneShot};
    std::atomic<float>     gain      {1.0f};
    std::atomic<bool>      muted     {false};
    // M4 :
    std::atomic<float>     timeRatio {1.0f};   // bpmSample / bpmProjet
    std::atomic<float>     semitones {0.0f};
    std::atomic<int32_t>   loopBeats {0};      // longueur musicale en beats
    std::atomic<int64_t>   anchor    {0};      // sample transport du trigger
    // M9 étape 6 — spatialisation runtime (pan + Haas) :
    std::atomic<float>     pan   {0.f};  // −1 (L) … +1 (R), loi égal-power
    std::atomic<float>     width {0.f};  // 0 = mono, 1 = max Haas (25 ms)
};

// ─── Voix (2 par slot pour les chevauchements) ───────────────────────────────

struct Voice {
    std::atomic<bool> active{false};
    bool    fadingOut = false;  // true = fade-out 1→0 (stop), false = fade-in 0→1 (attaque)
    int64_t readPos   = 0;      // en frames (pas en samples entrelacés)
    float   readFrac  = 0.f;    // partie fractionnaire pour correction SR
    float   fadeGain  = 1.0f;   // gain courant du fade (in ou out)
    int     fadeLeft  = 0;      // samples restants dans le fade
};

// ─── SlotPlayer ──────────────────────────────────────────────────────────────

class SlotPlayer {
public:
    // Charge le PCM dans le slot (message thread uniquement).
    void loadSlot(int slot, SlotPcm pcm, PlayMode mode = PlayMode::Free) noexcept;

    // Efface le slot (arrêt immédiat des voix).
    void clearSlot(int slot) noexcept;

    // Traite un bloc pour tous les slots actifs.
    // output : buffer stéréo entrelacé [L0,R0,L1,R1,...], numFrames frames.
    // Contenu AJOUTÉ (+=) au buffer — wet-only additif.
    // events : events avec offset temporel dans le bloc (triés par offset croissant).
    void processBlock(const TransportState& ts,
                      float* output, int32_t numFrames,
                      const EventWithOffset* events, int numEvents) noexcept;

    // Active/désactive le mode LOOP SYNC pour un slot (message thread).
    // loopBeats : durée de la loop en beats (ex. 8 = 2 mesures 4/4).
    // timeRatio : bpmSample / bpmProjet.
    // semitones : transposition.
    // anchorSample : sample transport absolu du trigger (frontière de step).
    void armLoopSync(int slot, int loopBeats, float timeRatio, float semitones,
                     int64_t anchorSample) noexcept;

    // Appelé une fois au changement de device (prépare les stretchers + buffers).
    void prepareStretchers(int channels, float sampleRate, int maxBlockSize = 2048) noexcept;

    // ── Snapshot PCM (lecture message thread — légère race OK, voir §11.1) ──
    // Retourne un downmix mono du PCM chargé — utilisé par l'UI pour les
    // waveforms et l'éditeur. Vide si le slot n'est pas chargé ou n'a aucun PCM.
    std::vector<float> getPcmSnapshot(int slot) const noexcept;

    // SR du PCM chargé (0 si slot vide) — pour re-trim côté UI.
    float getPcmSampleRate(int slot) const noexcept;

    // ── Setters message thread (atomiques, RT-safe) ───────────────────────────
    void setGain(int slot, float g) noexcept {
        if (slot >= 0 && slot < kSlots)
            params_[slot].gain.store(g, std::memory_order_relaxed);
    }
    void setMuted(int slot, bool m) noexcept {
        if (slot >= 0 && slot < kSlots)
            params_[slot].muted.store(m, std::memory_order_relaxed);
    }
    void setMode(int slot, PlayMode m) noexcept {
        if (slot >= 0 && slot < kSlots)
            params_[slot].mode.store(m, std::memory_order_relaxed);
    }
    void setSemitones(int slot, float s) noexcept {
        if (slot >= 0 && slot < kSlots)
            params_[slot].semitones.store(s, std::memory_order_relaxed);
    }
    // Spatialisation pan + Haas (M9 étape 6) — valeurs issues du rôle (AudioGraph).
    void setSpatial(int slot, float pan, float width) noexcept {
        if (slot >= 0 && slot < kSlots) {
            params_[slot].pan.store(pan,   std::memory_order_relaxed);
            params_[slot].width.store(width, std::memory_order_relaxed);
        }
    }
    float getPan(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return 0.f;
        return params_[slot].pan.load(std::memory_order_relaxed);
    }
    float getWidth(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return 0.f;
        return params_[slot].width.load(std::memory_order_relaxed);
    }

    // ── Getters thread-safe ────────────────────────────────────────────────────
    bool isLoaded(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return false;
        return loaded_[slot].load(std::memory_order_acquire);
    }
    float getGain(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return 1.f;
        return params_[slot].gain.load(std::memory_order_relaxed);
    }
    bool isMuted(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return false;
        return params_[slot].muted.load(std::memory_order_relaxed);
    }
    float getSemitones(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return 0.f;
        return params_[static_cast<std::size_t>(slot)].semitones.load(std::memory_order_relaxed);
    }
    float getTimeRatio(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return 1.f;
        return params_[static_cast<std::size_t>(slot)].timeRatio.load(std::memory_order_relaxed);
    }
    PlayMode getMode(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return PlayMode::Free;
        return params_[static_cast<std::size_t>(slot)].mode.load(std::memory_order_relaxed);
    }
    int getLoopBeats(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return 0;
        return params_[static_cast<std::size_t>(slot)].loopBeats.load(std::memory_order_relaxed);
    }
    bool isVoiceActive(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return false;
        return voiceActive_[slot][0].load(std::memory_order_relaxed)
            || voiceActive_[slot][1].load(std::memory_order_relaxed);
    }

    // Pic de sortie du slot pour le bloc courant (VU — lu depuis message thread).
    float getSlotPeak(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return 0.f;
        return slotPeak_[slot].load(std::memory_order_relaxed);
    }
    float playheadRatio(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return false;
        const auto& pcm = activePcm(slot);
        if (pcm.numFrames == 0) return 0.f;
        for (int v = 0; v < 2; ++v) {
            if (voiceActive_[slot][v].load(std::memory_order_relaxed))
                return static_cast<float>(voices_[slot][v].readPos) /
                       static_cast<float>(pcm.numFrames);
        }
        return 0.f;
    }

private:
    static constexpr int   kSlots         = 9;
    static constexpr int   kFadeLen       = 16;
    static constexpr float kFadeThreshold = 0.001f; // −60 dB
    static constexpr int   kCrossfadeLen  = 256;    // micro-crossfade recalage §10.2

    SlotPcm         pcmBuffers_[2][kSlots];     // double-buffer PCM
    std::atomic<int> activePcmIdx_[kSlots] = {}; // index du buffer actif (0 ou 1)
    Voice           voices_[kSlots][2];
    SlotParams      params_[kSlots];
    StretchConform  stretchers_[kSlots];

    // Indique si le slot a un PCM chargé (écrit message thread, lu audio thread).
    std::atomic<bool> loaded_[kSlots] {};

    // Flag cross-thread pour voices_[].active : l'audio thread écrit (relaxed),
    // le message thread lit (relaxed). Pas de stale critique pour un VU/état UI.
    std::atomic<bool> voiceActive_[kSlots][2] = {};

    // ── Rampe de gain de transition (EventType::GainRamp) ─────────────────────
    // Multiplicateur appliqué au-dessus du gain du slot. 1.0 = neutre.
    // Utilisé pour les fades Enter/Exit de TransitionEngine.
    float rampValue_[kSlots] = {};
    float rampTarget_[kSlots] = {};
    std::atomic<int> rampLeft_[kSlots] = {};   // samples restants (0 = pas de rampe active)

    // ── Haas — ligne de retard du canal faible (M9 étape 6) ──────────────────
    // width ∈ [0,1] → retard 0 … 25 ms. Puissance de 2 pour le masquage.
    static constexpr int kHaasDelayMax = 2048;
    float haasDelay_   [kSlots][kHaasDelayMax] = {};
    int   haasWritePos_[kSlots] = {};

    // Pic de sortie par slot (audio thread write, message thread read pour VU).
    std::atomic<float> slotPeak_[kSlots] = {};

    // Buffers temporaires pré-alloués pour renderLoopSync (zéro allocation audio).
    std::vector<float> tmpL_, tmpR_;      // sortie stéréo (numFrames)
    std::vector<float> srcL_, srcR_;      // buffer source (inputFrames)

    float sampleRate_ = 44100.f;

    // Avance les rampes de transition de numFrames (appelé en tête de processBlock).
    void advanceRamps(int numFrames) noexcept;

    // Retourne le PCM actif du slot (thread audio — lecture atomique de l'index).
    const SlotPcm& activePcm(int slot) const noexcept {
        return pcmBuffers_[activePcmIdx_[slot].load(std::memory_order_acquire)][slot];
    }

    // Réinitialise la ligne de retard Haas d'un slot (loadSlot/clearSlot).
    void resetSpatialSlot(int slot) noexcept;

    // Gains pan (loi égal-power). Identité si pan == 0 && width == 0
    // (préserve la transparence T-SP1). haasOnLeft : le canal faible est la gauche.
    void spatialGains(int slot, float& gL, float& gR, bool& haasOnLeft) noexcept;

    // Retourne le sample retardé de width × 25 ms (0 = pas de retard).
    float applyHaasDelay(int slot, float sample) noexcept;

    // Déclenche une voix sur le slot (choisit voice[0] ou voice[1]).
    void handleTrigger(int slot, int64_t transportAnchor) noexcept;

    // Désactive une voix (écrit voice.active + voiceActive_ atomique).
    void deactivateVoice(int slot, int v) noexcept {
        voices_[slot][v].active.store(false, std::memory_order_relaxed);
        voiceActive_[slot][v].store(false, std::memory_order_relaxed);
    }

    // Rend une voix dans le buffer stéréo de sortie.
    void renderVoice(int slot, int v, float* out, int numFrames,
                     const TransportState& ts) noexcept;

    // Rend le mode LOOP SYNC avec position dérivée (avec ou sans stretch).
    // fadeScale : multiplicateur appliqué au gain de sortie (1.0 = normal, 0.0 = silence).
    void renderLoopSync(int slot, float* out, int numFrames,
                        const TransportState& ts, float fadeScale = 1.0f) noexcept;

    // Rend tous les slots actifs dans le buffer de sortie (appelé par processBlock en sous-blocs).
    void renderSlots(const TransportState& ts,
                     float* output, int32_t numFrames,
                     int64_t blockStart) noexcept;
};

} // namespace engine
