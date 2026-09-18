#pragma once
#include <cstdint>

namespace engine {

// Snapshot POD lu par l'UI (timer 25 Hz) — aucune allocation, aucun lock.
// Produit par EngineFacade::getTransitionStatusSnapshot() (message thread).
struct TransitionStatusSnapshot {
    int      selectedScene   = -1;   // scène affichée/éditée dans l'UI
    int      runtimeScene    = -1;   // scène réellement active au moteur
    int      pendingScene    = -1;   // -1 si aucune transition armée
    uint8_t  policy          = 0;    // 0=DIRECT, 1=BUILD, 2=BREAKDOWN
    uint8_t  state           = 0;    // 0=Idle,1=Preparing,2=Armed,3=Executing,4=Settling
    bool     playing         = false;
    int64_t  nowSample       = 0;
    int64_t  boundarySample  = -1;
    double   bpm             = 0.0;
    int      timeSigNum      = 4;
    int      timeSigDen      = 4;
    double   sampleRate      = 0.0;
};

// Beats restants avant le commit de frontière.
// Retourne 0.0 si la frontière est passée, si bpm/sr sont invalides,
// ou si aucune transition n'est armée (boundarySample < 0).
inline double transitionBeatsRemaining(const TransitionStatusSnapshot& sn) noexcept
{
    if (sn.bpm <= 0.0 || sn.sampleRate <= 0.0 || sn.boundarySample < 0)
        return 0.0;
    const double samplesPerBeat = sn.sampleRate * 60.0 / sn.bpm;
    if (samplesPerBeat <= 0.0)
        return 0.0;
    const double rem = static_cast<double>(sn.boundarySample - sn.nowSample) / samplesPerBeat;
    return rem > 0.0 ? rem : 0.0;
}

} // namespace engine
