#include "WaveformCache.h"
#include <algorithm>

WaveformCache::WaveformCache()
    : state_(std::make_shared<State>())
{
}

WaveformCache::~WaveformCache()
{
    state_->alive.store(false, std::memory_order_relaxed);
    for (int s = 0; s < kSlots; ++s)
        cancel(s);
    // pool_ dtor rejoint les workers après que tous les tokens slot sont cancelled
}

void WaveformCache::cancel(int slot)
{
    if (slot < 0 || slot >= kSlots) return;
    const auto sidx = static_cast<std::size_t>(slot);
    if (slotTokens_[sidx])
        slotTokens_[sidx]->store(true, std::memory_order_relaxed);
}

void WaveformCache::request(int slot, const std::string& filePath,
                             int trimStart, int trimEnd,
                             engine::AssetId assetId, EnvelopeCb cb)
{
    if (slot < 0 || slot >= kSlots || filePath.empty() || assetId == 0)
        return;

    const auto sidx = static_cast<std::size_t>(slot);

    // Cache hit → callback immédiat sur le message thread courant
    {
        auto it = state_->cache.find(assetId);
        if (it != state_->cache.end())
        {
            cb(slot, it->second);
            return;
        }
    }

    // Annuler job précédent pour ce slot
    if (slotTokens_[sidx])
        slotTokens_[sidx]->store(true, std::memory_order_relaxed);
    slotTokens_[sidx] = std::make_shared<std::atomic<bool>>(false);
    auto token = slotTokens_[sidx];

    auto state = state_;
    pool_.enqueue(
        token,
        [filePath, trimStart, trimEnd]() -> std::vector<float> {
            return loadAndDecode(filePath, trimStart, trimEnd);
        },
        [state, token, slot, assetId, cb = std::move(cb)](std::vector<float> env) mutable {
            // onResult depuis le worker — guard avant d'enregistrer dans la file JUCE
            if (!state->alive.load(std::memory_order_relaxed)) return;
            if (token->load(std::memory_order_relaxed))         return;
            juce::MessageManager::callAsync(
                [state, token, slot, assetId,
                 cb = std::move(cb), env = std::move(env)]() mutable {
                    if (!state->alive.load(std::memory_order_relaxed)) return;
                    if (token->load(std::memory_order_relaxed))         return;
                    state->cache.emplace(assetId, env);
                    cb(slot, std::move(env));
                });
        });
}

void WaveformCache::prefetch(const std::string& filePath, int trimStart, int trimEnd,
                              engine::AssetId assetId) noexcept
{
    if (assetId == 0 || filePath.empty()) return;
    if (state_->cache.count(assetId)) return;  // déjà en cache

    auto state = state_;
    pool_.enqueueUnique(
        assetId,
        [filePath, trimStart, trimEnd]() -> std::vector<float> {
            return loadAndDecode(filePath, trimStart, trimEnd);
        },
        [state, assetId](std::vector<float> env) mutable {
            // guard prefetch : le pool ne peut pas annuler les jobs sans token
            if (!state->alive.load(std::memory_order_relaxed)) return;
            juce::MessageManager::callAsync(
                [state, assetId, env = std::move(env)]() mutable {
                    if (!state->alive.load(std::memory_order_relaxed)) return;
                    state->cache.emplace(assetId, std::move(env));
                });
        });
}

std::vector<float> WaveformCache::computeEnvelope(const std::vector<float>& pcm, int bins)
{
    return WaveformThreadPool::computeEnvelope(pcm, bins);
}

std::vector<float> WaveformCache::loadAndDecode(const std::string& path, int trimStart, int trimEnd)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    auto reader = std::unique_ptr<juce::AudioFormatReader>(
        fm.createReaderFor(juce::File(juce::String(path))));
    if (!reader) return {};

    const int numCh = static_cast<int>(reader->numChannels);
    const int total = static_cast<int>(reader->lengthInSamples);
    if (total <= 0) return {};

    const int start = (trimStart >= 0) ? std::min(trimStart, total) : 0;
    const int end   = (trimEnd   >= 0) ? std::min(trimEnd,   total) : total;
    const int len   = std::max(0, end - start);
    if (len == 0) return {};

    juce::AudioBuffer<float> buf(numCh, len);
    reader->read(&buf, 0, len, start, true, true);

    std::vector<float> mono(static_cast<std::size_t>(len));
    if (numCh >= 2)
    {
        const float* L = buf.getReadPointer(0);
        const float* R = buf.getReadPointer(1);
        for (int i = 0; i < len; ++i)
            mono[static_cast<std::size_t>(i)] = (L[i] + R[i]) * 0.5f;
    }
    else
    {
        const float* ch = buf.getReadPointer(0);
        std::copy(ch, ch + len, mono.begin());
    }
    return mono;
}
