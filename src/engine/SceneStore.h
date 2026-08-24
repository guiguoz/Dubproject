#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <string>

#include "engine/Sequencer.h"    // kMaxSlots, kMaxSteps
#include "engine/SlotPlayer.h"   // PlayMode, SlotRole

namespace engine {

static constexpr int kMaxScenes = 8;

// ─────────────────────────────────────────────────────────────────────────────
// SlotConfig — configuration complète d'un slot.
// ─────────────────────────────────────────────────────────────────────────────
struct SlotConfig {
    std::string filePath;
    PlayMode    mode       = PlayMode::OneShot;
    float       gain       = 1.0f;
    float       userGain   = 1.0f;   // contrôle manuel UI (prioritaire sur gain si > 0)
    float       timeRatio  = 1.0f;
    float       semitones  = 0.0f;
    int         loopBeats  = 0;
    int         trimStart  = 0;
    int         trimEnd    = 0;      // 0 = jusqu'à la fin
    float       delaySend  = 0.0f;   // envoi au bus dub delay
    SlotRole    role       = SlotRole::Loop;
    bool        active     = false;
    bool        muted      = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// SceneData — snapshot complet d'une scène.
// Combine le moteur V2 (SlotConfig) + état UI (steps, serum, dubDelay).
// ─────────────────────────────────────────────────────────────────────────────
struct SceneData {
    SlotConfig  slots[kMaxSlots];
    int         bpm            = 120;
    std::string name;

    // ── Patterns pas à pas (UI) ──────────────────────────────────────────────
    std::array<std::array<bool, 512>, kMaxSlots> steps     = {};
    std::array<int, kMaxSlots>                   trackBarCounts = {1,1,1,1,1,1,1,1,1};

    // ── Serum ────────────────────────────────────────────────────────────────
    float        serumGain       = 1.0f;
    std::string  serumState;          // preset state (base64)
    std::string  serumPresetName;     // nom affiché

    // ── Dub Delay par scène ──────────────────────────────────────────────────
    float        dubDelayFeedback = 0.40f;
    float        dubDelayWet      = 0.28f;
    float        dubDelayTone     = 0.50f;
    float        dubDelayDrive    = 0.15f;

    bool         used             = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Forme du crossfade adaptatif.
// ─────────────────────────────────────────────────────────────────────────────
enum class CrossfadeCurve { Linear, EaseIn, EaseOut, Smoothstep };

// ─────────────────────────────────────────────────────────────────────────────
// SceneStore — stockage de 8 scènes + état transition (crossfade / morph).
//
// Thread-safety :
//   - Scènes : message thread (GUI) uniquement, snapshot par copie.
//   - pendingScene_ : atomique, écrit message thread, lu timer thread.
// ─────────────────────────────────────────────────────────────────────────────
class SceneStore {
public:
    // ── Accès aux scènes ─────────────────────────────────────────────────────

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

    // ── Navigation quantisée ─────────────────────────────────────────────────

    void setPendingScene(int idx) noexcept {
        pendingScene_.store(idx, std::memory_order_relaxed);
    }

    int consumePendingScene() noexcept {
        return pendingScene_.exchange(-1, std::memory_order_relaxed);
    }

    bool hasPendingScene() const noexcept {
        return pendingScene_.load(std::memory_order_relaxed) >= 0;
    }

    int pendingIdx() const noexcept {
        return pendingScene_.load(std::memory_order_relaxed);
    }

    // ── Crossfade adaptatif ──────────────────────────────────────────────────

    struct CrossfadeProfile { int durationMs; CrossfadeCurve curve; };

    static CrossfadeProfile chooseProfile(float from, float to) noexcept {
        constexpr float kT = 0.15f;
        const bool fHigh = (from >= kT), tHigh = (to >= kT);
        if ( fHigh && !tHigh) return { 600, CrossfadeCurve::EaseIn    };
        if (!fHigh &&  tHigh) return {  80, CrossfadeCurve::EaseOut   };
        if ( fHigh &&  tHigh) return { 250, CrossfadeCurve::Linear    };
        return                       { 350, CrossfadeCurve::Smoothstep };
    }

    void armCrossfade(const std::array<float, kMaxSlots>& startGains,
                      const std::array<float, kMaxSlots>& targetGains) noexcept
    {
        crossfade_.startGains  = startGains;
        crossfade_.targetGains = targetGains;
        crossfade_.elapsedMs   = 0;
        crossfade_.durationMs  = kDefaultCrossfadeMs;
        crossfade_.curve       = CrossfadeCurve::Linear;
        crossfade_.active      = true;
    }

    void armAdaptiveCrossfade(const std::array<float, kMaxSlots>& startGains,
                               const std::array<float, kMaxSlots>& targetGains,
                               float fromEnergy,
                               float toEnergy,
                               float startSerumGain  = 1.f,
                               float targetSerumGain = 1.f) noexcept
    {
        const auto [dur, curve] = chooseProfile(fromEnergy, toEnergy);
        crossfade_ = { true, 0, dur, curve, startGains, targetGains,
                       startSerumGain, targetSerumGain };
    }

    bool isCrossfadeActive() const noexcept { return crossfade_.active; }

    // ── Morphing PingPongDelay ───────────────────────────────────────────────

    void startDubDelayMorph(int from, int to, float durationMs = 4000.f) noexcept {
        if (from < 0 || to < 0 || from >= kMaxScenes || to >= kMaxScenes) return;
        morphState_ = { true, 0.f, durationMs, from, to, 0 };
    }

    void updateMorph() noexcept {
        if (!morphState_.active) return;
        morphState_.tickCounter += 33;
        morphState_.progress = std::clamp(
            static_cast<float>(morphState_.tickCounter) / morphState_.durationMs,
            0.f, 1.f);
        if (morphState_.progress >= 1.f)
            morphState_.active = false;
    }

    bool  isMorphing()        const noexcept { return morphState_.active; }
    float getMorphProgress()  const noexcept { return morphState_.progress; }
    void  stopMorph()               noexcept { morphState_.active = false; }
    int   getMorphFromScene() const noexcept { return morphState_.fromScene; }
    int   getMorphToScene()   const noexcept { return morphState_.toScene; }

    // ── Énergie des scènes ───────────────────────────────────────────────────

    void  setSceneEnergy(int idx, float e) noexcept {
        sceneEnergy_[static_cast<std::size_t>(idx)] = e;
    }

    float getSceneEnergy(int idx) const noexcept {
        return sceneEnergy_[static_cast<std::size_t>(idx)];
    }

private:
    static constexpr int kDefaultCrossfadeMs = 150;

    std::array<SceneData, kMaxScenes> scenes_ = {};
    std::atomic<int>     pendingScene_ { -1 };

    struct CrossfadeState {
        bool                      active          = false;
        int                       elapsedMs       = 0;
        int                       durationMs      = kDefaultCrossfadeMs;
        CrossfadeCurve            curve           = CrossfadeCurve::Linear;
        std::array<float, kMaxSlots> startGains   = {};
        std::array<float, kMaxSlots> targetGains  = {};
        float                     startSerumGain  = 1.f;
        float                     targetSerumGain = 1.f;
    };
    CrossfadeState crossfade_;

    struct MorphState {
        bool  active      = false;
        float progress    = 0.f;
        float durationMs  = 4000.f;
        int   fromScene   = -1;
        int   toScene     = -1;
        int   tickCounter = 0;
    };
    MorphState morphState_;

    std::array<float, kMaxScenes> sceneEnergy_ = {};
};

} // namespace engine
