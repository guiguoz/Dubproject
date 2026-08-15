#pragma once
// ─── EngineFacade — pont entre l'UI existante et le moteur V2 ────────────────
// SerumHost est injecté via setSerumHost() (reste dans src/dsp/, dépendances JUCE).

#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
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
    // extInL/extInR : entrée dry (EWI/sax) à mixer (nullptr = pas d'entrée).
    // serumL/serumR : sortie Serum post-proc × serumGain (nullptr = pas de Serum).
    void processBlock(float* left, float* right, int numSamples,
                      const float* extInL = nullptr, const float* extInR = nullptr,
                      const float* serumL = nullptr, const float* serumR = nullptr,
                      float serumGain = 0.f) noexcept;

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
    // Si trimStart/trimEnd ≥ 0 (coordonnées fichier), le PCM chargé est découpé
    // avant le stockage (même sémantique que le reload trim de la V1).
    // Fichier/trim courants exposés via slotFilePath()/slotTrim() pour que
    // l'UI ne réimporte que si la scène référence un PCM absent du SlotPlayer.
    void importSampleAsync(int slot, const std::string& filePath,
                           ImportCallback cb = nullptr,
                           int trimStart = -1, int trimEnd = -1);

    void clearSlot(int slot) noexcept;
    void triggerSlot(int slot) noexcept;  // trigger immédiat (pad live)
    void stopSlot(int slot, bool immediate = false) noexcept;

    // Fichier + trim actuellement chargés dans le SlotPlayer (message thread
    // uniquement — stockés au lancement/achèvement de l'import).
    const std::string& slotFilePath(int slot) const noexcept;
    int slotTrimStart(int slot) const noexcept;
    int slotTrimEnd(int slot) const noexcept;

    void setSlotGain(int slot, float gain) noexcept;
    float getSlotGain(int slot) const noexcept;
    void setSlotMuted(int slot, bool muted, bool quantized = false) noexcept;
    bool isSlotMuted(int slot) const noexcept;
    void setSlotSolo(int slot, bool soloed) noexcept;
    void setSlotTransposeSemitones(int slot, float semitones) noexcept;
    void setSlotMode(int slot, PlayMode mode) noexcept;
    void setSlotRole(int slot, SlotRole role) noexcept;

    // Rôle analysé par le moteur V2 (ImportPipeline) au dernier import du slot.
    // isSlotRoleReliable() == true quand l'analyse est fiable
    // (roleConfidence ≥ 0.75 et rôle ≠ Unknown) — dans ce cas slotRole() a la
    // priorité sur la détection V1 (samplerEngine_.getDetectedType) lors de la
    // sync des scènes. Message thread uniquement.
    SlotRole slotRole(int slot) const noexcept;
    bool     isSlotRoleReliable(int slot) const noexcept;

    // Métriques (thread-safe via atomics dans SlotPlayer)
    float getSlotPlayheadRatio(int slot) const noexcept;
    float getSlotOutputPeak(int slot)    const noexcept;
    bool  isSlotPlaying(int slot)        const noexcept;
    bool  isSlotLoaded(int slot)         const noexcept;

    // Snapshot PCM mono du slot (UI waveforms/éditeur) — message thread.
    std::vector<float> getSlotPcmSnapshot(int slot) const noexcept;
    // SR du PCM chargé (0 si vide) — pour re-trim côté UI.
    float getSlotPcmSampleRate(int slot) const noexcept;

    // Recharge le PCM d'un slot à partir d'un buffer mono (re-trim éditeur).
    // Conserve le mode courant ; arrête les voix. Message thread uniquement.
    void reloadSlotPcm(int slot, std::vector<float> mono, float sampleRate) noexcept;

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

    // ── Entrée externe (EWI/sax dry) ──────────────────────────────────────────
    // Gain du bus d'entrée dry. Rendu via graph_.processBlock(extInL/R).
    void setInputGain(float g) noexcept { graph_.setInputGain(g); }
    float getInputGain() const noexcept { return graph_.getInputGain(); }

    // ── AutoMix ────────────────────────────────────────────────────────────────
    float getMasterRms() const noexcept { return masterRms_.load(); }

    // Lance le thread de mix temps réel : recomputeTargets() toutes les 50 ms
    // depuis les rôles courants. Appelé depuis prepareToPlay // à arrêter dans releaseResources.
    void startMixThread() noexcept;
    void stopMixThread() noexcept;

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
    std::atomic<bool>  slotLoaded_[kMaxSlots] {};

    // Fichier + trim chargés dans le SlotPlayer (message thread seulement).
    std::string        slotPath_  [kMaxSlots];
    int                slotTrimStart_[kMaxSlots] { 0 };
    int                slotTrimEnd_  [kMaxSlots] { -1 };

    // Rôle analysé par le V2 au dernier import (worker → message thread via le
    // flag slotRoleReliable_, motif release/acquire).
    SlotRole           slotRoleAnalyzed_ [kMaxSlots] { SlotRole::Loop };
    std::atomic<bool>  slotRoleReliable_ [kMaxSlots] { false };

    // Solo par slot : si un slot est solo, les autres sont muets (audio thread).
    std::atomic<int32_t> soloSlot_ {-1};

    // Patterns (write side, message thread)
    TrackPattern writePatterns_[kMaxSlots];
    int          trackBars_[kMaxSlots] = {};

    // File SPSC message→audio pour triggers/stops live (bitmask 9 bits)
    std::atomic<uint16_t> pendingTriggers_{0};
    std::atomic<uint16_t> pendingStops_{0};

    // Flush du buffer delay demandé depuis message thread, exécuté en audio thread.
    std::atomic<bool> delayResetPending_{false};

    // Compteur de blocs audio (incrémenté en tête de processBlock).
    std::atomic<int64_t>  audioBlockCounter_{0};

    // Buffer entrelacé pré-alloué (évite toute allocation en audio callback)
    std::vector<float> interleavedOut_;

    // Thread de mix temps réel (50 ms) — recalcule les cibles AutoMix.
    std::thread             mixThread_;
    std::atomic<bool>       mixThreadRun_ {false};
    std::atomic<bool>       mixThreadStarted_ {false};

    void applySceneInternal(int idx) noexcept;
};

} // namespace engine
