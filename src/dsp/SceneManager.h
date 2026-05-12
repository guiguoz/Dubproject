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
    bool                                 used           { false };
};

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

    /// Avance l'interpolation d'un tick (tickMs ≈ 33ms à 30Hz).
    /// Écrit directement sur le sampler via setSlotGain().
    /// Retourne false quand le crossfade est terminé.
    bool updateCrossfade(int tickMs, Sampler& sampler) noexcept
    {
        if (!crossfade_.active)
            return false;

        crossfade_.elapsedMs += tickMs;
        const float t = std::clamp(
            static_cast<float>(crossfade_.elapsedMs)
            / static_cast<float>(crossfade_.durationMs),
            0.f, 1.f);

        for (int i = 0; i < 9; ++i)
        {
            const float gain = crossfade_.startGains[i]
                + (crossfade_.targetGains[i] - crossfade_.startGains[i]) * t;
            sampler.setSlotGain(i, gain);
        }

        if (crossfade_.elapsedMs >= crossfade_.durationMs)
        {
            for (int i = 0; i < 9; ++i)
                sampler.setSlotGain(i, crossfade_.targetGains[i]);
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
        bool  active     { false };
        int   elapsedMs  { 0 };
        int   durationMs { kCrossfadeDurationMs };
        std::array<float, 9> startGains  {};
        std::array<float, 9> targetGains {};
    };
    CrossfadeState crossfade_;
};

} // namespace dsp
