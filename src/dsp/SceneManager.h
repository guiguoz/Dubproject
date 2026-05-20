#pragma once

#include "Sampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <string>

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// SceneData — runtime snapshot d'une scène (8 scènes max).
// Séparé de SceneSaveData (project/ProjectData.h) qui est la version sérialisée.
// ─────────────────────────────────────────────────────────────────────────────
struct SceneData
{
    float                                bpm            { 120.f };
    std::array<std::string, 9>           filePaths      {};
    std::array<std::array<bool, 512>, 9> steps          {};
    std::array<float, 9>                 gains          { 1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f };
    std::array<bool,  9>                 mutes          {};
    std::array<int,   9>                 trackBarCounts { 1,1,1,1,1,1,1,1,1 };
    std::array<int,   9>                 trimStart      { 0,0,0,0,0,0,0,0,0 };
    std::array<int,   9>                 trimEnd        { -1,-1,-1,-1,-1,-1,-1,-1,-1 };
    std::array<float, 9>                 delaySends     { 0.f,0.f,0.f,0.f,0.f,0.f,0.f,0.f,0.f };
    std::array<float, 9>                 userGains      { 1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f };
    float                                serumGain      { 1.f };
    std::string                          serumState;      // Serum preset state (base64), empty = none
    std::string                          serumPresetName; // User-provided name for this scene's preset
    bool                                 used           { false };
};

// ─────────────────────────────────────────────────────────────────────────────
// Forme du crossfade utilisée par armAdaptiveCrossfade().
// ─────────────────────────────────────────────────────────────────────────────
enum class CrossfadeCurve { Linear, EaseIn, EaseOut, Smoothstep };

// ─────────────────────────────────────────────────────────────────────────────
// SceneManager — propriétaire de l'état des scènes + crossfade.
//
// Thread safety :
//   - scene(), currentIdx(), setCurrentIdx(), armCrossfade() → GUI thread
//   - setPendingScene(), consumePendingScene(), hasPendingScene() → GUI thread
//     (pendingScene_ est atomique pour être lu depuis timerCallback)
//   - updateCrossfade() → GUI timer (message thread)
// ─────────────────────────────────────────────────────────────────────────────
class SceneManager
{
public:
    static constexpr int kMaxScenes          = 8;
    static constexpr int kCrossfadeDurationMs = 150;  // crossfade musical 50-200ms

    // ── Accès aux scènes ─────────────────────────────────────────────────────

    SceneData&       scene(int idx)       { return scenes_[static_cast<std::size_t>(idx)]; }
    const SceneData& scene(int idx) const { return scenes_[static_cast<std::size_t>(idx)]; }

    int  currentIdx() const noexcept { return currentIdx_; }
    void setCurrentIdx(int idx) noexcept { currentIdx_ = idx; }

    // ── Navigation quantisée ─────────────────────────────────────────────────

    void setPendingScene(int idx) noexcept
    {
        pendingScene_.store(idx, std::memory_order_relaxed);
    }

    /// Consomme la scène en attente. Retourne l'index ou -1 si aucun.
    int consumePendingScene() noexcept
    {
        return pendingScene_.exchange(-1, std::memory_order_relaxed);
    }

    bool hasPendingScene() const noexcept
    {
        return pendingScene_.load(std::memory_order_relaxed) >= 0;
    }

    int pendingIdx() const noexcept
    {
        return pendingScene_.load(std::memory_order_relaxed);
    }

    // ── Crossfade ────────────────────────────────────────────────────────────

    /// Arme un crossfade depuis startGains vers targetGains.
    /// À appeler depuis applyScene() après avoir posé les targetGains sur le sampler.
    void armCrossfade(const std::array<float, 9>& startGains,
                      const std::array<float, 9>& targetGains) noexcept
    {
        crossfade_.startGains  = startGains;
        crossfade_.targetGains = targetGains;
        crossfade_.elapsedMs   = 0;
        crossfade_.active      = true;
    }

    bool isCrossfadeActive() const noexcept { return crossfade_.active; }

    // ── Profil de crossfade ──────────────────────────────────────────────────

    struct CrossfadeProfile { int durationMs; CrossfadeCurve curve; };

    /// Retourne le profil (durée + courbe) pour une transition from→to.
    /// Public pour les tests unitaires ; aucun état modifié.
    static CrossfadeProfile chooseProfile(float from, float to) noexcept
    {
        constexpr float kT = 0.15f;
        const bool fHigh = (from >= kT), tHigh = (to >= kT);
        if ( fHigh && !tHigh) return { 600, CrossfadeCurve::EaseIn    };
        if (!fHigh &&  tHigh) return {  80, CrossfadeCurve::EaseOut   };
        if ( fHigh &&  tHigh) return { 250, CrossfadeCurve::Linear    };
        return                       { 350, CrossfadeCurve::Smoothstep };
    }

    /// Crossfade adaptatif : durée et courbe choisies selon l'énergie from/to.
    void armAdaptiveCrossfade(const std::array<float, 9>& startGains,
                              const std::array<float, 9>& targetGains,
                              float fromEnergy,
                              float toEnergy,
                              float startSerumGain  = 1.f,
                              float targetSerumGain = 1.f) noexcept
    {
        const auto [dur, curve] = chooseProfile(fromEnergy, toEnergy);
        crossfade_ = { true, 0, dur, curve, startGains, targetGains,
                       startSerumGain, targetSerumGain };
    }

    // ── Énergie des scènes ───────────────────────────────────────────────────

    /// Score d'énergie musicale [0,1] dérivé des patterns + mutes, sans audio.
    static float computeSceneEnergy(const SceneData& sc) noexcept
    {
        if (!sc.used) return 0.f;
        float sum = 0.f;
        for (int i = 0; i < 9; ++i)
        {
            const auto idx = static_cast<std::size_t>(i);
            if (sc.mutes[idx] || sc.filePaths[idx].empty()) continue;
            const int total = sc.trackBarCounts[idx] * 16;
            int active = 0;
            for (int s = 0; s < total; ++s)
                if (sc.steps[idx][static_cast<std::size_t>(s)]) ++active;
            if (active == 0) continue;
            sum += 0.5f + 0.5f * (static_cast<float>(active) / static_cast<float>(total));
        }
        // Serum actif : contribue à hauteur de 1 slot supplémentaire (pondéré par son gain)
        if (sc.serumGain > 0.05f)
            sum += 0.5f + 0.5f * std::clamp(sc.serumGain, 0.f, 1.f);
        return std::clamp(sum / 10.f, 0.f, 1.f);
    }

    void  setSceneEnergy(int idx, float e) noexcept
    {
        sceneEnergy_[static_cast<std::size_t>(idx)] = e;
    }

    float getSceneEnergy(int idx) const noexcept
    {
        return sceneEnergy_[static_cast<std::size_t>(idx)];
    }

    /// Avance l'interpolation d'un tick (tickMs ≈ 33ms à 30Hz).
    /// Écrit directement sur le sampler via setSlotGain().
    /// serumGainOut (optionnel) : gain Serum interpolé à appliquer par l'appelant.
    /// Retourne false quand le crossfade est terminé.
    bool updateCrossfade(int tickMs, Sampler& sampler,
                         float* serumGainOut = nullptr) noexcept
    {
        if (!crossfade_.active)
            return false;

        crossfade_.elapsedMs += tickMs;
        float t = std::clamp(
            static_cast<float>(crossfade_.elapsedMs)
            / static_cast<float>(crossfade_.durationMs),
            0.f, 1.f);

        switch (crossfade_.curve)
        {
            case CrossfadeCurve::EaseIn:
                t = t * t * t;
                break;
            case CrossfadeCurve::EaseOut:
                { const float inv = 1.f - t; t = 1.f - inv * inv * inv; }
                break;
            case CrossfadeCurve::Smoothstep:
                t = t * t * (3.f - 2.f * t);
                break;
            default:
                break;
        }

        for (int i = 0; i < 9; ++i)
        {
            const float gain = crossfade_.startGains[i]
                + (crossfade_.targetGains[i] - crossfade_.startGains[i]) * t;
            sampler.setSlotGain(i, gain);
        }

        if (serumGainOut)
            *serumGainOut = crossfade_.startSerumGain
                + (crossfade_.targetSerumGain - crossfade_.startSerumGain) * t;

        if (crossfade_.elapsedMs >= crossfade_.durationMs)
        {
            for (int i = 0; i < 9; ++i)
                sampler.setSlotGain(i, crossfade_.targetGains[i]);
            if (serumGainOut)
                *serumGainOut = crossfade_.targetSerumGain;
            crossfade_.active = false;
            return false;
        }
        return true;
    }

private:
    std::array<SceneData, kMaxScenes> scenes_;
    int                  currentIdx_   { 0 };
    std::atomic<int>     pendingScene_ { -1 };

    struct CrossfadeState
    {
        bool           active          { false };
        int            elapsedMs       { 0 };
        int            durationMs      { kCrossfadeDurationMs };
        CrossfadeCurve curve           { CrossfadeCurve::Linear };
        std::array<float, 9> startGains  {};
        std::array<float, 9> targetGains {};
        float          startSerumGain  { 1.f };
        float          targetSerumGain { 1.f };
    };
    CrossfadeState crossfade_;

    std::array<float, kMaxScenes> sceneEnergy_ {};
};

} // namespace dsp
