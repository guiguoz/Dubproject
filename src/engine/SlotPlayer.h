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
};

// ─── Voix (2 par slot pour les chevauchements) ───────────────────────────────

struct Voice {
    bool    active    = false;
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
    // events : EngineEvents triés chronologiquement pour ce bloc.
    void processBlock(const TransportState& ts,
                      float* output, int32_t numFrames,
                      const EngineEvent* events, int numEvents) noexcept;

    // Active/désactive le mode LOOP SYNC pour un slot (message thread).
    // loopBeats : durée de la loop en beats (ex. 8 = 2 mesures 4/4).
    // timeRatio : bpmSample / bpmProjet.
    // semitones : transposition.
    // anchorSample : sample transport absolu du trigger (frontière de step).
    void armLoopSync(int slot, int loopBeats, float timeRatio, float semitones,
                     int64_t anchorSample) noexcept;

    // Appelé une fois au changement de device (prépare les stretchers).
    void prepareStretchers(int channels, float sampleRate) noexcept;

    // ── Snapshot PCM (lecture message thread — légère race OK, voir §11.1) ──
    // Retourne un downmix mono du PCM chargé (sémantique Sampler V1
    // getSlotPcmSnapshot) — utilisé par l'UI pour les waveforms et l'éditeur.
    // Vide si le slot n'est pas chargé ou n'a aucun PCM.
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
        return voices_[slot][0].active || voices_[slot][1].active;
    }

    // Pic de sortie du slot pour le bloc courant (VU — lu depuis message thread).
    float getSlotPeak(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots) return 0.f;
        return slotPeak_[slot];
    }
    float playheadRatio(int slot) const noexcept {
        if (slot < 0 || slot >= kSlots || pcm_[slot].numFrames == 0) return 0.f;
        for (int v = 0; v < 2; ++v) {
            if (voices_[slot][v].active)
                return static_cast<float>(voices_[slot][v].readPos) /
                       static_cast<float>(pcm_[slot].numFrames);
        }
        return 0.f;
    }

    // ── Diagnostic voix (appelé depuis message thread — légère data race OK) ──
    struct VoiceDiagInfo {
        bool    active[2]    = {};
        bool    fadingOut[2] = {};
        int64_t readPos[2]   = {};
        float   fadeGain[2]  = {};
        int     numFrames    = 0;
    };
    VoiceDiagInfo getVoiceDiagInfo(int slot) const noexcept;
    float         diagSrRatio(int slot) const noexcept;

private:
    static constexpr int   kSlots         = 9;
    static constexpr int   kFadeLen       = 16;
    static constexpr float kFadeThreshold = 0.001f; // −60 dB
    static constexpr int   kCrossfadeLen  = 256;    // micro-crossfade recalage §10.2

    SlotPcm         pcm_[kSlots];
    Voice           voices_[kSlots][2];
    SlotParams      params_[kSlots];
    StretchConform  stretchers_[kSlots];

    // Indique si le slot a un PCM chargé (écrit message thread, lu audio thread).
    std::atomic<bool> loaded_[kSlots] {};

    // ── Rampe de gain de transition (EventType::GainRamp) ─────────────────────
    // Multiplicateur appliqué au-dessus du gain du slot. 1.0 = neutre.
    // Utilisé pour les fades Enter/Exit de TransitionEngine.
    float rampValue_[kSlots] = {};
    float rampTarget_[kSlots] = {};
    int   rampLeft_[kSlots]  = {};   // samples restants (0 = pas de rampe active)

    // Pic de sortie par slot (audio thread write, message thread read pour VU).
    float slotPeak_[kSlots] = {};

    float sampleRate_ = 44100.f;

    // Avance les rampes de transition de numFrames (appelé en tête de processBlock).
    void advanceRamps(int numFrames) noexcept;

    // Déclenche une voix sur le slot (choisit voice[0] ou voice[1]).
    void handleTrigger(int slot, int64_t transportAnchor) noexcept;

    // Rend une voix dans le buffer stéréo de sortie.
    void renderVoice(int slot, int v, float* out, int numFrames,
                     const TransportState& ts) noexcept;

    // Rend le mode LOOP SYNC avec position dérivée (avec ou sans stretch).
    // fadeScale : multiplicateur appliqué au gain de sortie (1.0 = normal, 0.0 = silence).
    void renderLoopSync(int slot, float* out, int numFrames,
                        const TransportState& ts, float fadeScale = 1.0f) noexcept;
};

} // namespace engine
