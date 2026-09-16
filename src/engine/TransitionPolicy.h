#pragma once
// TransitionPolicy — couche PURE de scheduling au-dessus de SceneTransitionPlan.
// Ne charge aucun sample, ne touche aucun DSP. Convertit une fois les offsets
// musicaux en samples absolus lors de PREPARE ; COMMIT consomme le plan tel quel.
#include "engine/SceneTransitionPlan.h"

namespace engine {

enum class PolicyType : uint8_t { Direct = 0, Build = 1, Breakdown = 2 };

struct PolicyContext {
    double  sampleRate = 44100.0;
    double  bpm = 120.0;
    int     timeSigNum = 4;
    int     timeSigDenom = 4;
    int64_t boundary = 0; // == plan.boundary
};

class TransitionPolicy {
public:
    // Pour diagnostic : forcer DIRECT (élimine BUILD/BREAKDOWN du test)
    static void setForceDirect(bool v) noexcept { forceDirect_ = v; }
    static bool isForceDirect() noexcept { return forceDirect_; }

    // Choix déterministe basé sur ENTER/LEAVE nets (pas sur nombre brut de slots).
    // KEEP est sacré et ne compte pas. Replace (même slot asset différent) compte
    // comme 1 Leave + 1 Enter (churn équilibré → DIRECT).
    static PolicyType choose(const SceneTransitionPlan& plan) noexcept;

    // Applique la policy au plan en ajustant uniquement les atSample des
    // événements/rampes déjà supportés par DIRECT. Ne crée pas de nouveaux types.
    // Pour BUILD : étale les ENTER sur la grille musicale à partir de T.
    // Pour BREAKDOWN : étale les LEAVE.
    // DIRECT : identique à l'entrée (référence 125d27b).
    static void apply(SceneTransitionPlan& plan, const PolicyContext& ctx) noexcept;

    // Helpers publics pour tests
    static int64_t samplesPerStep(const PolicyContext& ctx) noexcept;
    static int64_t samplesPerBeat(const PolicyContext& ctx) noexcept;

private:
    static inline bool forceDirect_ = false;
};

} // namespace engine
