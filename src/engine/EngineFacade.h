#pragma once
// ─── EngineFacade — pont entre l'UI existante et le moteur V2 ────────────────
// SerumHost est injecté via setSerumHost() (reste dans src/dsp/, dépendances JUCE).

// JuceHeader d'abord : dsp/StepSequencer.h et dsp/Sampler.h utilisent juce
// (jlimit, etc.) dans des membres inline — ce header doit rester auto-suffisant
// quel que soit l'ordre des includes du TU appelant.
#include <JuceHeader.h>

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
#include "engine/mix/MixState.h"
#include "engine/mix/MixWorker.h"
#include "engine/Types.h"

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
    void setSerumHost(::dsp::SerumHost* sh) noexcept { serumHost_ = sh; }

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

    // ── État de mix persistant (étape 7 / M9) ────────────────────────────────
    // Restaure l'état de mix d'un slot (depuis un projet chargé) et l'applique
    // au runtime (gain + spatialisation SlotPlayer). Marque `applied`.
    void setSlotMixState(int slot, float gain, float pan,
                         float width, float depth) noexcept;
    // Lit l'état courant (pour la sauvegarde projet). Slots non mixés → défaut.
    mix::SlotMixState getSlotMixState(int slot) const noexcept;

    // ── Magic mix asynchrone (worker — étape 8 / M9) ──────────────────────────
    // Capture le snapshot runtime (PCM mono + rôles + scène courante) sur le
    // message thread puis exécute MixEngine::processHeuristic sur un thread
    // dédié. À la fin, applique gain/pan/width/depth au runtime (SlotPlayer) et
    // appelle onMagicMixDone sur le message thread.
    // Déclenché explicitement par l'UI (bouton ⚡) — jamais pendant renderOffline
    // (le nulltest reste inchangé).
    void triggerMagicMix() noexcept;
    // Variante chemin IA (étape 4 / M9) : mêmes captures, mais exécute
    // MixEngine::processAiMix avec les décisions du modèle ONNX (8 slots).
    // L'inférence reste côté appelant (src/dsp/, SAXFX_HAS_ONNX) ; ce worker
    // n'embarque que le post-traitement pur.
    void triggerAiMagicMix(const std::array<engine::mix::MixAiDecision, 8>& decisions) noexcept;
    // Toggle ⚡ (étape 5 / M9) : applique si le magic mix n'est pas actif,
    // revert sinon — sémantique V1 SmartSamplerEngine::toggleMagicMix.
    void toggleMagicMix() noexcept;
    // Revert du magic mix (étape 5 / M9) : remet gain 1 + spatial neutre sur
    // tous les slots (le PCM n'est jamais modifié — invariant), efface l'état
    // persistant et déclenche le callback done. Synchrone (message thread) —
    // pas de thread worker nécessaire (aucun traitement PCM).
    void revertMagicMix() noexcept;
    bool isMagicBusy() const noexcept { return magicMixBusy_.load(); }
    bool isMagicActive() const noexcept { return magicMixActive_.load(); }
    // Vrai si le dernier magic mix a utilisé le chemin heuristique (pas l'IA ONNX).
    void setMagicMixDoneCallback(std::function<void()> cb) noexcept;
    bool didLastMixUseFallback() const noexcept { return lastMixUsedFallback_.load(std::memory_order_acquire); }

    // Contexte Serum pour le magic mix (étape 4/9 / M9). Copié au lancement du
    // worker (message thread) — le thread de mix n'y touche pas.
    // rms <= 0.02 → pas de duck. contentType SYNTH/PAD → carving EQ (chemin IA).
    void setSerumContext(float rms, float centroid, float midFrac, float highFrac,
                         engine::mix::MixContentType contentType, bool active) noexcept;

    // Types effectifs du dernier magic mix (lecture UI : tags + spatial viz).
    // Avant le premier mix : retourne le rôle courant du slot (rôle analysé ou
    // rôle de scène). Message thread uniquement.
    engine::mix::MixContentType getDetectedType(int slot) const noexcept;

    // Override manuel de type (clic droit UI) — équivalent V2 du
    // manualTypeOverride_/setTypeOverride. Appliqué à la place de la détection
    // lors du prochain mix.
    void setManualTypeOverride(int slot, int typeIndex) noexcept;

    // Rôle analysé par le moteur V2 (ImportPipeline) au dernier import du slot.
    // isSlotRoleReliable() == true quand l'analyse est fiable
    // (roleConfidence ≥ 0.75 et rôle ≠ Unknown) — dans ce cas slotRole() a la
    // priorité sur le type effectif du mix V2 lors de la sync des scènes.
    // Message thread uniquement.
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

    // ── Transitions de scène (Tier 1 — Phase 4a) ────────────────────────────────
    // Accès aux transitions en attente et contrôle du morphing delay.
    // Remplace V1 sceneManager_/stepSequencer_ pour la logique de transition.
    int  pendingSceneIdx() const noexcept;           // Index scène en attente
    void setPendingScene(int idx) noexcept;          // Marquer scène pour transition
    int  consumePendingScene() noexcept;             // Consommer & appliquer transition
    bool hasPendingScene() const noexcept;           // Vérifier si transition armée
    
    bool hasPendingTransition() const noexcept;      // Transition quantisée en cours?
    void setPendingTransitionLen(int steps) noexcept; // Durée transition (steps)
    bool consumeSceneEnd() noexcept;                 // Consommer signal end-of-bar
    
    // Morphing du delay : transitions paramétriques (feedback/wet/tone/drive).
    void startDubDelayMorph(int from, int to, float durationMs = 4000.f) noexcept;
    void updateMorphing() noexcept;
    bool isMorphing() const noexcept;
    float getMorphProgress() const noexcept;         // [0..1]
    int  getMorphFromSceneIdx() const noexcept;
    int  getMorphToSceneIdx() const noexcept;
    
    // Énergie de scène (pour crossfade adaptatif).
    void setSceneEnergy(int idx, float energy) noexcept;
    float getSceneEnergy(int idx) const noexcept;

    // ── Patterns / Sequencer (helper) ───────────────────────────────────────────
    // Préparer le buffer de pattern pour la prochaine scène (message thread).
    void prepareStepBuffer(const StepBuf& buf) noexcept;

    // Arrêter tous les slots avec mode d'arrêt spécifié.
    void stopAllSlots(StopMode mode = StopMode::Normal) noexcept;

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

    // ── Playhead / séquenceur (pour l'UI) ──────────────────────────────────────
    // Index du step courant (0 .. kMaxSteps-1), récupéré depuis le Transport V2.
    // Remplace V1 StepSequencer::getCurrentStep() pour l'UI.
    int32_t getCurrentStep() const noexcept;
    // Phase fractionnaire du beat courant [0..∞), pour animation de playhead
    // et détection de downbeat par LooperEngine.
    double getCurrentPhase() const noexcept;

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
    ::dsp::SerumHost* serumHost_ = nullptr;

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

    // État de mix persistant par slot (étape 7 / M9) — message thread.
    mix::MixStateArray mixState_ {};

    // Worker magic mix (étape 8 / M9) : thread dédié + callback fin (message thread).
    std::atomic<bool> magicMixBusy_   {false};
    std::atomic<bool> magicMixActive_ {false};
    std::atomic<bool> lastMixUsedFallback_ {true};
    std::function<void()> magicMixDoneCb_;

    // Contexte Serum pour le magic mix (message thread — copié au lancement).
    float serumRms_      = 0.f;
    float serumCentroid_ = 0.f;
    float serumMidFrac_  = 0.f;
    float serumHighFrac_ = 0.f;
    mix::MixContentType serumContentType_ = mix::MixContentType::OTHER;

    // Types effectifs du dernier mix (message thread) + overrides manuels.
    mix::MixContentType detectedTypes_[kMaxSlots] {};
    mix::MixContentType manualOverrides_[kMaxSlots] {};
    bool manualOverrideActive_[kMaxSlots] {};

    // Solo par slot : si un slot est solo, les autres sont muets (audio thread).
    std::atomic<int32_t> soloSlot_ {-1};

    // Patterns (write side, message thread)
    TrackPattern writePatterns_[kMaxSlots];
    int          trackBars_[kMaxSlots] = {};

    // ── Transitions quantisées (Tier 1 — Phase 4a) ────────────────────────────
    // La frontière est détectée par TransitionEngine dans processBlock :
    // au passage Armed→Executing, sceneEndFlag_ est posé et pendingTransLen_
    // effacé — le signal « fin de scène » est consommé par l'UI (timer).
    std::atomic<int>  pendingScene_    { -1 };
    std::atomic<int>  pendingTransLen_ { 0 };
    std::atomic<bool> sceneEndFlag_    { false };

    // ── Morphing PingPongDelay (Tier 2 — Phase 4a) ────────────────────────────
    struct MorphState
    {
        bool  active      = false;
        float progress    = 0.f;
        float durationMs  = 4000.f;
        int   fromScene   = -1;
        int   toScene     = -1;
        int   tickCounter = 0;
    };
    MorphState morphState_;

    // Énergie de scène (crossfade adaptatif) — message thread.
    float sceneEnergy_[kMaxScenes] {};

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
