#pragma once
#include <vector>
#include "engine/SceneStore.h"
#include "engine/Sequencer.h"

namespace engine {

// PCM prêt pour le rendu offline (déjà décodé — aucune dépendance disque/JUCE).
struct OfflinePcm {
    int   numChannels = 1;         // 1 ou 2
    float sampleRate  = 44100.f;   // SR d'origine du fichier
    std::vector<float> data;       // entrelacé si stéréo, sinon mono
};

// Configuration LOOP SYNC d'un slot (opt-in indépendant de la scène).
struct OfflineLoopSync {
    bool   enabled   = false;
    int    loopBeats = 0;          // longueur musicale en beats
    float  timeRatio = 1.0f;       // bpmSample / bpmProjet
};

// Session offline : décrit un rendu complet et déterministe (§11.2).
struct OfflineSession {
    double bpm          = 120.0;
    double sampleRate   = 44100.0;
    int    maxBlockSize = 512;
    int    numScenes    = 1;
    int    startScene   = 0;

    OfflinePcm      pcm[kMaxSlots];        // PCM par slot (chargé dans tous les cas)
    OfflineLoopSync loopSync[kMaxSlots];   // config LOOP SYNC par slot
    TrackPattern    patterns[kMaxSlots];   // patterns du séquenceur
    SceneData       scenes[kMaxScenes];    // scènes (configs slots, gains, rôles)

    struct Transition {
        int64_t atSample = 0;   // sample transport où demander la transition
        int     toScene  = 0;
    };
    std::vector<Transition> transitions;
};

// Rendu offline déterministe : séquenceur + transitions + AutoMix v2.0 (§11.2).
// Le thread de mix est simulé : computeTargets() recalculées toutes les 50 ms.
// Aucune source de temps réel ni aléa non seedé — sortie reproductible.
// Retourne un buffer stéréo entrelacé de numSamples * 2 floats.
std::vector<float> renderOffline(const OfflineSession& session, int64_t numSamples);

} // namespace engine
