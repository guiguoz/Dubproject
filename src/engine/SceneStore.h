#pragma once
#include <string>
#include "engine/SlotPlayer.h"   // PlayMode, SlotRole
#include "engine/Sequencer.h"    // kMaxSlots

namespace engine {

static constexpr int kMaxScenes = 8;

struct SlotConfig {
    std::string filePath;
    PlayMode    mode       = PlayMode::OneShot;  // défaut sûr — pas de boucle involontaire
    float       gain       = 1.0f;
    float       timeRatio  = 1.0f;
    float       semitones  = 0.0f;
    int         loopBeats  = 0;
    int         trimStart  = 0;   // en frames
    int         trimEnd    = 0;   // 0 = jusqu'à la fin
    SlotRole    role       = SlotRole::Loop;   // SlotRole::Loop ≈ Unknown
    bool        active     = false;
};

struct SceneData {
    SlotConfig  slots[kMaxSlots];
    int         bpm  = 120;
    std::string name;
};

// Stockage de 8 scènes.
// Thread-safety : les lectures/écritures sont protégées par copie snapshot
// (message thread uniquement — pas d'accès audio direct).
class SceneStore {
public:
    void setScene(int idx, SceneData scene) noexcept {
        if (idx < 0 || idx >= kMaxScenes) return;
        scenes_[idx] = std::move(scene);
    }

    SceneData& getScene(int idx) noexcept {
        if (idx < 0 || idx >= kMaxScenes) return scenes_[0];
        return scenes_[idx];
    }

    const SceneData& getScene(int idx) const noexcept {
        if (idx < 0 || idx >= kMaxScenes) return scenes_[0];
        return scenes_[idx];
    }

    int numScenes() const noexcept { return kMaxScenes; }

private:
    SceneData scenes_[kMaxScenes];
};

} // namespace engine
