#pragma once
#include <JuceHeader.h>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <array>
#include <memory>
#include <atomic>
#include "../engine/SceneTransitionPlan.h"   // engine::AssetId, engine::assetIdFor
#include "WaveformThreadPool.h"

class WaveformCache
{
public:
    static constexpr int kEnvelopeBins = 200;
    static constexpr int kSlots = 9;

    using EnvelopeCb = std::function<void(int slot, std::vector<float> envelope)>;

    WaveformCache();
    ~WaveformCache();

    // Demande l'enveloppe pour (slot, assetId).
    // Cache hit  → cb appelé immédiatement sur le message thread.
    // Cache miss → job enfilé dans le pool, cb sur message thread quand prêt.
    // Job précédent pour ce slot avec assetId différent → annulé.
    void request(int slot, const std::string& filePath,
                 int trimStart, int trimEnd,
                 engine::AssetId assetId, EnvelopeCb cb);

    // Annule le job en cours pour ce slot (navigation rapide / clearSlot).
    void cancel(int slot);

    // Préchargement silencieux (pas de slot tracking, pas de callback).
    // Cache hit ou job en cours → no-op. Sinon : job enfilé dans le pool.
    void prefetch(const std::string& filePath, int trimStart, int trimEnd,
                  engine::AssetId assetId) noexcept;

    // Calcul peak-amplitude en `bins` valeurs [0..1].
    static std::vector<float> computeEnvelope(const std::vector<float>& pcm,
                                              int bins = kEnvelopeBins);

private:
    // Partagé avec les lambdas du pool → survit à la destruction de WaveformCache
    struct State
    {
        std::unordered_map<engine::AssetId, std::vector<float>> cache;
        std::atomic<bool>                                        alive { true };
    };

    // Lit le fichier depuis disque, downmix stéréo→mono, applique le trim.
    // Appelé depuis un thread pool — crée son propre AudioFormatManager local.
    static std::vector<float> loadAndDecode(const std::string& path, int trimStart, int trimEnd);

    std::shared_ptr<State>                                   state_;
    WaveformThreadPool                                       pool_;
    std::array<WaveformThreadPool::StopToken, kSlots>        slotTokens_;
};
