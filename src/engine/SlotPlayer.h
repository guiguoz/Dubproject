#pragma once
#include <cstdint>
#include <atomic>
#include <vector>
#include "engine/Transport.h"
#include "engine/EventScheduler.h"

namespace engine {

// ─── Types de base ───────────────────────────────────────────────────────────

enum class PlayMode : uint8_t { OneShot = 0, Free = 1, LoopSync = 2 };
enum class SlotRole  : uint8_t { Kick=0, Bass=1, Snare=2, Pad=3, Melodic=4, Perc=5, Fx=6, Loop=7, Drum=8 };

// PCM immuable après chargement (message thread uniquement).
struct SlotPcm {
    std::vector<float> data;      // interleaved stéréo OU mono
    int   numChannels = 1;        // 1 ou 2
    int   numFrames   = 0;
    float sampleRate  = 44100.f;
};

// Paramètres atomiques : lus depuis le thread audio, écrits depuis le message thread.
struct SlotParams {
    std::atomic<PlayMode> mode  {PlayMode::OneShot};
    std::atomic<float>    gain  {1.0f};
    std::atomic<bool>     muted {false};
    // loopBeats et transpose ajoutés en M4
};

// ─── Voix (2 par slot pour les chevauchements) ───────────────────────────────

struct Voice {
    bool    active   = false;
    int64_t readPos  = 0;       // en frames (pas en samples entrelacés)
    float   fadeGain = 1.0f;    // micro-fade linéaire 16 samples
    int     fadeLeft = 0;       // samples de fade restants
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

private:
    static constexpr int kSlots     = 9;
    static constexpr int kFadeLen   = 16;
    static constexpr float kFadeThreshold = 0.001f; // −60 dB

    SlotPcm    pcm_[kSlots];
    Voice      voices_[kSlots][2];
    SlotParams params_[kSlots];

    // Indique si le slot a un PCM chargé (écrit message thread, lu audio thread).
    std::atomic<bool> loaded_[kSlots] {};

    // Déclenche une voix sur le slot (choisit voice[0] ou voice[1]).
    void handleTrigger(int slot) noexcept;

    // Rend une voix dans le buffer stéréo de sortie.
    void renderVoice(int slot, int v, float* out, int numFrames) noexcept;
};

} // namespace engine
