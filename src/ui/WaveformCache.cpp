#include "WaveformCache.h"
#include <algorithm>
#include <cmath>
#include <thread>

WaveformCache::WaveformCache()
    : state_(std::make_shared<State>())
{
}

WaveformCache::~WaveformCache()
{
    for (int s = 0; s < kSlots; ++s)
        cancel(s);
}

void WaveformCache::cancel(int slot)
{
    if (slot < 0 || slot >= kSlots) return;
    const auto sidx = static_cast<std::size_t>(slot);
    if (activeJob_[sidx])
        activeJob_[sidx]->cancelled.store(true, std::memory_order_relaxed);
    activeJob_[sidx] = nullptr;
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
    if (activeJob_[sidx])
        activeJob_[sidx]->cancelled.store(true, std::memory_order_relaxed);

    auto job = std::make_shared<SlotJob>();
    job->assetId = assetId;
    activeJob_[sidx] = job;

    // Capture state_ par valeur : survit à la destruction de WaveformCache
    auto state = state_;

    std::thread([filePath, trimStart, trimEnd, assetId, slot,
                 job = std::move(job), state = std::move(state),
                 cb = std::move(cb)]() mutable
    {
        auto pcm      = loadAndDecode(filePath, trimStart, trimEnd);
        auto envelope = computeEnvelope(pcm);

        juce::MessageManager::callAsync(
            [assetId, slot, job = std::move(job),
             state = std::move(state), cb = std::move(cb),
             envelope = std::move(envelope)]() mutable
            {
                if (job->cancelled.load(std::memory_order_relaxed))
                    return;
                state->cache.emplace(assetId, envelope);
                cb(slot, std::move(envelope));
            });
    }).detach();
}

void WaveformCache::prefetch(const std::string& filePath, int trimStart, int trimEnd,
                              engine::AssetId assetId) noexcept
{
    if (assetId == 0 || filePath.empty()) return;
    if (state_->cache.count(assetId))   return;  // déjà en cache
    if (state_->pending.count(assetId)) return;  // déjà en vol

    state_->pending.insert(assetId);
    auto state = state_;
    try
    {
        std::thread([filePath, trimStart, trimEnd, assetId, state = std::move(state)]() mutable
        {
            auto pcm = loadAndDecode(filePath, trimStart, trimEnd);
            if (pcm.empty())
            {
                juce::MessageManager::callAsync([state, assetId]
                    { state->pending.erase(assetId); });
                return;
            }
            auto env = computeEnvelope(pcm);
            juce::MessageManager::callAsync(
                [state, assetId, env = std::move(env)]() mutable
                {
                    state->pending.erase(assetId);
                    state->cache.emplace(assetId, std::move(env));
                });
        }).detach();
    }
    catch (...) { state_->pending.erase(assetId); }
}

std::vector<float> WaveformCache::computeEnvelope(const std::vector<float>& pcm, int bins)
{
    if (pcm.empty() || bins <= 0) return {};
    std::vector<float> env(static_cast<std::size_t>(bins), 0.f);
    const int total = static_cast<int>(pcm.size());
    for (int b = 0; b < bins; ++b)
    {
        const int first = b * total / bins;
        const int last  = std::min(total, (b + 1) * total / bins);
        float peak = 0.f;
        for (int i = first; i < last; ++i)
            peak = std::max(peak, std::abs(pcm[static_cast<std::size_t>(i)]));
        env[static_cast<std::size_t>(b)] = peak;
    }
    return env;
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
