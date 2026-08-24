#pragma once
#include <algorithm>

#include "engine/SceneStore.h"   // SceneData, SlotConfig

namespace engine {

// Score d'énergie musicale [0,1] d'une scène V2, calculé sans analyse audio.
//
// Utilise les slots actifs, leur gain, ET la densité de pas (steps + trackBarCounts).
// Les mutes sont exclues du calcul.
class SceneEnergy {
public:
    // Score [0,1] à partir des slots ACTIFS de la scène.
    // - un slot inactif, ou actif sans fichier (filePath vide), ne contribue pas ;
    // - chaque slot actif contribue 0.5 + 0.5 * clamp(gain, 0, 1)
    //   (gain 0 → 0.5, gain 1 → 1.0) ;
    // - le total est ramené sur kMaxSlots : clamp(sum / kMaxSlots, 0, 1).
    //   Si aucun slot ne contribue → 0.
    static float compute(const SceneData& scene) noexcept
    {
        constexpr float kHalfBase = 0.5f;

        float sum = 0.f;
        bool  any = false;

        for (int i = 0; i < kMaxSlots; ++i)
        {
            const SlotConfig& slot = scene.slots[i];
            if (!slot.active || slot.filePath.empty()) continue;

            sum += kHalfBase + kHalfBase * std::clamp(slot.gain, 0.f, 1.f);
            any  = true;
        }

        if (!any) return 0.f;
        return std::clamp(sum / static_cast<float>(kMaxSlots), 0.f, 1.f);
    }
};

} // namespace engine
