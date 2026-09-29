#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>
#include <vector>
#include "../engine/SceneTransitionPlan.h"  // engine::AssetId

// ─────────────────────────────────────────────────────────────────────────────
// WaveformThreadPool — pool de threads JUCE-free pour le décodage de waveforms.
//
// Deux familles de jobs :
//   enqueue()       : job à token d'annulation externe (slot navigation)
//   enqueueUnique() : job dédupliqué par assetId (prefetch silencieux)
//
// Le pool vérifie le StopToken avant IO, avant compute envelope, avant onResult.
// onResult est appelé depuis le thread worker — en production WaveformCache
// l'enveloppe dans un callAsync JUCE ; en test on peut l'appeler directement.
// ─────────────────────────────────────────────────────────────────────────────
class WaveformThreadPool
{
public:
    static constexpr int kEnvelopeBins  = 200;
    static constexpr int kDefaultWorkers = 2;

    // StopToken partagé : store(true) = annuler le job.
    using StopToken = std::shared_ptr<std::atomic<bool>>;
    // Compute : produit le PCM brut. Appelé sur le thread worker.
    using Compute   = std::function<std::vector<float>()>;
    // OnResult : reçoit l'enveloppe si non annulé. Appelé sur le thread worker.
    using OnResult  = std::function<void(std::vector<float>)>;

    explicit WaveformThreadPool(int numWorkers = kDefaultWorkers);
    ~WaveformThreadPool();

    // Enfile un job avec token d'annulation fourni par l'appelant.
    void enqueue(StopToken token, Compute compute, OnResult onResult) noexcept;

    // Enfile un job dédupliqué : si assetId est déjà en file ou en cours,
    // retourne false (no-op). Le pool retire assetId du set pending quand
    // le job termine (qu'il soit annulé ou pas).
    bool enqueueUnique(engine::AssetId assetId,
                       Compute compute, OnResult onResult) noexcept;

    // Bloque jusqu'à queue vide ET workers inactifs. Usage tests uniquement.
    void waitForIdle();

    // Calcul peak-amplitude, JUCE-free. Partagé avec test_waveform_cache.
    static std::vector<float> computeEnvelope(const std::vector<float>& pcm,
                                               int bins = kEnvelopeBins);

private:
    struct Job {
        Compute         compute;
        OnResult        onResult;
        StopToken       token;
        engine::AssetId uniqueId { 0 };  // 0 = pas de dédup
    };

    void workerLoop();

    std::vector<std::thread>            workers_;
    std::queue<Job>                     queue_;
    std::unordered_set<engine::AssetId> pendingUnique_;
    std::mutex                          mx_;
    std::condition_variable             cv_;    // workers
    std::condition_variable             idle_;  // waitForIdle
    int                                 inFlight_ { 0 };
    bool                                stopping_ { false };
};
