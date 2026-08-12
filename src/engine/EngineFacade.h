#pragma once
// ─── EngineFacade — pont entre l'UI existante et le moteur V2 ────────────────
// Masqué derrière DUB_ENGINE_V2.
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
    void importSampleAsync(int slot, const std::string& filePath,
                           ImportCallback cb = nullptr);

    void clearSlot(int slot) noexcept;
    void triggerSlot(int slot) noexcept;  // trigger immédiat (pad live)
    void stopSlot(int slot, bool immediate = false) noexcept;

    void setSlotGain(int slot, float gain) noexcept;
    float getSlotGain(int slot) const noexcept;
    void setSlotMuted(int slot, bool muted, bool quantized = false) noexcept;
    bool isSlotMuted(int slot) const noexcept;
    void setSlotSolo(int slot, bool soloed) noexcept;
    void setSlotTransposeSemitones(int slot, float semitones) noexcept;
    void setSlotMode(int slot, PlayMode mode) noexcept;
    void setSlotRole(int slot, SlotRole role) noexcept;

    // Métriques (thread-safe via atomics dans SlotPlayer)
    float getSlotPlayheadRatio(int slot) const noexcept;
    float getSlotOutputPeak(int slot)    const noexcept;
    bool  isSlotPlaying(int slot)        const noexcept;
    bool  isSlotLoaded(int slot)         const noexcept;

    // Diagnostic temporaire (à retirer en M8c)
    float    getSlotSemitones(int slot)  const noexcept;
    float    getSlotTimeRatio(int slot)  const noexcept;
    PlayMode getSlotMode(int slot)       const noexcept;
    int      getSlotLoopBeats(int slot)  const noexcept;

    // Retourne "write:[0,4] active:[0,4] nSteps=16" pour un slot donné.
    std::string getPatternDiag(int slot) const noexcept;

    // Retourne les derniers triggers reçus sur un slot (thread audio → message thread).
    // Format : "step=X pos=Y src=[seq|UI] | ..."
    std::string getTriggerDiag(int slot) const noexcept;

    // Retourne "1S/FR/LS sr=0.919 v0[r=N] v1[off]" pour un slot donné.
    std::string getVoiceDiag(int slot) const noexcept;

    // Retourne "bs=N n=M Δ=0" — régularité du transport entre blocs.
    std::string getTransportDiag() const noexcept;

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

    // ── Diagnostic de régularité du transport (M8c à retirer) ────────────────
    std::atomic<int64_t> diagPrevBlockStart_{-1};  // blockStart du bloc précédent
    std::atomic<int32_t> diagPrevBlockN_{0};        // numSamples du bloc précédent
    std::atomic<int64_t> diagBlockDeltaErr_{0};     // blockStart - expected (≠0 = anomalie)

    // ── Rôle détecté par l'analyse (écrit dans importSampleAsync) ────────────
    std::atomic<int32_t> diagLastRole_[kMaxSlots];  // SlotRoleV2 as int, -1 = jamais importé

    // ── Buffer circulaire de diagnostic trigger (audio thread → message thread) ──
    // Écrit depuis le thread audio avec relaxed, lu depuis timerCallback.
    // Pas de garantie stricte sur la cohérence inter-champs — usage diagnostic uniquement.
    static constexpr int kTrigDiag = 8;
    std::atomic<int64_t> diagTrigPos_ [kTrigDiag] {};   // position transport au trigger
    std::atomic<int32_t> diagTrigStep_[kTrigDiag] {};   // index de step calculé
    std::atomic<bool>    diagTrigSrc_ [kTrigDiag] {};   // true=séquenceur, false=UI/pad
    std::atomic<int>     diagTrigHead_{0};               // prochain index d'écriture (modulo kTrigDiag)

    // Buffer entrelacé pré-alloué (évite toute allocation en audio callback)
    std::vector<float> interleavedOut_;

    // Thread de mix temps réel (50 ms) — recalcule les cibles AutoMix.
    std::thread             mixThread_;
    std::atomic<bool>       mixThreadRun_ {false};
    std::atomic<bool>       mixThreadStarted_ {false};

    void applySceneInternal(int idx) noexcept;
};

} // namespace engine
