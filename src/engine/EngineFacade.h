#pragma once
// ─── EngineFacade — pont entre l'UI existante et le moteur V2 ────────────────
// Masqué derrière DUB_ENGINE_V2.
// SerumHost est injecté via setSerumHost() (reste dans src/dsp/, dépendances JUCE).

#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include "engine/AudioGraph.h"
#include "engine/Transport.h"
#include "engine/ImportPipeline.h"
#include "engine/SceneStore.h"
#include "engine/TransitionEngine.h"
#include "engine/EventScheduler.h"

// Forward-declare SerumHost (JUCE dep, reste dans src/dsp/)
namespace dsp { class SerumHost; }

namespace engine {

// Callback d'analyse asynchrone (appelé sur le message thread)
using ImportCallback = std::function<void(int slot, const AnalysisResult&)>;

// ─── EngineFacade ────────────────────────────────────────────────────────────
class EngineFacade {
public:
    EngineFacade();
    ~EngineFacade() = default;

    // ── Cycle de vie ───────────────────────────────────────────────────────────
    // Appelé depuis prepareToPlay (message thread).
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    // Appelé depuis releaseResources.
    void releaseResources() noexcept;

    // Injection du SerumHost (reste dans src/dsp/, non porté).
    void setSerumHost(dsp::SerumHost* sh) noexcept { serumHost_ = sh; }

    // ── Audio callback ─────────────────────────────────────────────────────────
    // Remplace dspPipeline_.processStereo() + stepSequencer_.process().
    // left/right : buffers float raw (de JUCE AudioBuffer::getWritePointer).
    void processBlock(float* left, float* right, int numSamples) noexcept;

    // ── Transport ──────────────────────────────────────────────────────────────
    void play() noexcept;
    void stop() noexcept;
    bool isPlaying() const noexcept { return transport_.state().playing; }
    void setBpm(float bpm) noexcept;
    float getBpm() const noexcept { return static_cast<float>(transport_.state().bpm); }
    void setSwing(float swing) noexcept { swingFactor_ = swing; }
    float getSwing() const noexcept { return swingFactor_; }

    // ── Slots (9 slots) ────────────────────────────────────────────────────────
    // Import asynchrone : le callback est appelé sur le message thread.
    void importSampleAsync(int slot, const std::string& filePath,
                           ImportCallback cb = nullptr);

    void clearSlot(int slot) noexcept;
    void triggerSlot(int slot) noexcept;  // trigger immédiat (pad live)
    void stopSlot(int slot, bool immediate = false) noexcept;

    void setSlotGain(int slot, float gain) noexcept;
    float getSlotGain(int slot) const noexcept;
    void setSlotMuted(int slot, bool muted, bool quantized = false) noexcept;
    bool isSlotMuted(int slot) const noexcept;
    void setSlotTransposeSemitones(int slot, float semitones) noexcept;
    void setSlotMode(int slot, PlayMode mode) noexcept;
    void setSlotRole(int slot, SlotRole role) noexcept;

    // Métriques (thread-safe via atomics dans SlotPlayer)
    float getSlotPlayheadRatio(int slot) const noexcept;
    float getSlotOutputPeak(int slot)    const noexcept;
    bool  isSlotPlaying(int slot)        const noexcept;
    bool  isSlotLoaded(int slot)         const noexcept;

    // ── Séquenceur (patterns) ──────────────────────────────────────────────────
    void setStep(int track, int step, bool active) noexcept;
    bool getStep(int track, int step) const noexcept;
    int  getTrackStepCount(int track) const noexcept;
    void setTrackBarCount(int track, int bars) noexcept;
    int  getTrackBarCount(int track) const noexcept;
    void flipPatternBuffer() noexcept; // flip atomique après modification

    // ── Scènes ─────────────────────────────────────────────────────────────────
    int  currentSceneIdx() const noexcept { return currentScene_; }
    void setCurrentScene(int idx) noexcept;
    void requestTransition(int toScene) noexcept;
    SceneData& scene(int idx) noexcept  { return sceneStore_.getScene(idx); }

    // ── DubDelay (accès direct à l'objet porté) ───────────────────────────────
    fx::PingPongDelay& delay() noexcept { return graph_.delay(); }

    // ── AutoMix ────────────────────────────────────────────────────────────────
    float getMasterRms() const noexcept { return masterRms_.load(); }

    // ── Diagnostics / debug ────────────────────────────────────────────────────
    // CPU callback budget : p99 < 50 % (§11.4)
    float getCpuLoadPercent() const noexcept { return cpuLoad_.load(); }

private:
    // Sous-systèmes moteur
    Transport        transport_;
    AudioGraph       graph_;
    SceneStore       sceneStore_;
    TransitionEngine transition_;
    ImportPipeline   importPipeline_;
    EventScheduler   scheduler_;

    // État
    double sampleRate_    = 44100.0;
    int    maxBlockSize_  = 512;
    float  swingFactor_   = 0.f;
    int    currentScene_  = 0;

    // SerumHost (non porté — reste JUCE)
    dsp::SerumHost* serumHost_ = nullptr;

    // Métriques thread-safe
    std::atomic<float> masterRms_ {0.f};
    std::atomic<float> cpuLoad_   {0.f};

    // État par slot (metrics, lecture depuis UI thread)
    std::atomic<float> slotPeak_[kMaxSlots] {};
    std::atomic<bool>  slotLoaded_[kMaxSlots] {};

    // Patterns (write side, message thread)
    TrackPattern writePatterns_[kMaxSlots];
    int          trackBars_[kMaxSlots] = {};

    // File SPSC message→audio pour triggers/stops live (bitmask 9 bits)
    std::atomic<uint16_t> pendingTriggers_{0};
    std::atomic<uint16_t> pendingStops_{0};

    // Compteur de blocs audio (incrémenté en tête de processBlock).
    // Permet à importSampleAsync d'attendre N blocs complets avant d'écrire le PCM,
    // garantissant qu'aucune voix n'est active sur le slot cible.
    std::atomic<int64_t>  audioBlockCounter_{0};

    // Buffer entrelacé pré-alloué (évite toute allocation en audio callback)
    std::vector<float> interleavedOut_;

    void applySceneInternal(int idx) noexcept;
};

} // namespace engine
