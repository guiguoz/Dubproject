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

    // Active/désactive le mode LOOP SYNC pour un slot (message thread).
    // loopBeats : durée de la loop en beats (ex. 8 = 2 mesures 4/4).
    // timeRatio : bpmSample / bpmProjet.
    // semitones : transposition.
    // anchorSample : sample transport absolu du trigger (frontière de step).
    void armLoopSync(int slot, int loopBeats, float timeRatio, float semitones,
                     int64_t anchorSample) noexcept;

    // Appelé une fois au changement de device (prépare les stretchers).
    void prepareStretchers(int channels, float sampleRate) noexcept;

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

    // Déclenche une voix sur le slot (choisit voice[0] ou voice[1]).
    void handleTrigger(int slot, int64_t transportAnchor) noexcept;

    // Rend une voix dans le buffer stéréo de sortie.
    void renderVoice(int slot, int v, float* out, int numFrames,
                     const TransportState& ts) noexcept;

    // Rend le mode LOOP SYNC avec position dérivée (avec ou sans stretch).
    void renderLoopSync(int slot, float* out, int numFrames,
                        const TransportState& ts) noexcept;
};

} // namespace engine
