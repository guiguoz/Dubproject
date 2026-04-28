#include "EffectChain.h"
#include "DelayEffect.h"

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// Audio thread
// ─────────────────────────────────────────────────────────────────────────────

IEffect* EffectChain::getActiveEffect(int index) noexcept
{
    const int idx = activeIdx_.load(std::memory_order_acquire);
    if (index < 0 || index >= buffers_[idx].count)
        return nullptr;
    return buffers_[idx].effects[index];
}

void EffectChain::drainCommands() noexcept
{
    ChainSnapshot snap;
    while (commandQueue_.tryPop(snap))
    {
        const int inactive = 1 - activeIdx_.load(std::memory_order_relaxed);
        buffers_[inactive].count = snap.count;
        for (int i = 0; i < snap.count; ++i)
            buffers_[inactive].effects[i] = snap.effects[i];

        activeIdx_.store(inactive, std::memory_order_release);
        swapGeneration_.fetch_add(1, std::memory_order_release);
    }
}

void EffectChain::prepare(double sampleRate, int maxBlockSize) noexcept
{
    preparedSampleRate_ = sampleRate;
    preparedBlockSize_  = maxBlockSize;

    for (int i = 0; i < ownedCount_; ++i)
        if (owned_[i])
            owned_[i]->prepare(sampleRate, maxBlockSize);
}

void EffectChain::process(float* buf, int numSamples, float pitchHz) noexcept
{
    drainCommands();

    const auto& chain = buffers_[activeIdx_.load(std::memory_order_acquire)];
    for (int i = 0; i < chain.count; ++i)
    {
        IEffect* eff = chain.effects[i];
        if (eff && eff->enabled.load(std::memory_order_acquire))
            eff->process(buf, numSamples, pitchHz);
    }
}

void EffectChain::processStereo(float* left, float* right,
                                int numSamples, float pitchHz) noexcept
{
    drainCommands();

    const auto& chain = buffers_[activeIdx_.load(std::memory_order_acquire)];
    for (int i = 0; i < chain.count; ++i)
    {
        IEffect* eff = chain.effects[i];
        if (eff && eff->enabled.load(std::memory_order_acquire))
            eff->processStereo(left, right, numSamples, pitchHz);
    }
}

void EffectChain::reset() noexcept
{
    for (int i = 0; i < ownedCount_; ++i)
        if (owned_[i])
            owned_[i]->reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// GUI thread
// ─────────────────────────────────────────────────────────────────────────────

bool EffectChain::addEffect(std::unique_ptr<IEffect> effect) noexcept
{
    if (ownedCount_ >= kMaxEffects) return false;

    // Prepare immediately if we are already streaming
    if (preparedSampleRate_ > 0.0 && preparedBlockSize_ > 0)
        effect->prepare(preparedSampleRate_, preparedBlockSize_);

    owned_[ownedCount_++] = std::move(effect);
    publishSnapshot();
    return true;
}

void EffectChain::removeEffect(int index) noexcept
{
    if (index < 0 || index >= ownedCount_) return;

    // Record generation so collectGarbage() knows when it's safe to delete
    lastRemoveGen_ = swapGeneration_.load(std::memory_order_acquire);

    // Move to graveyard before shrinking owned_ so the audio thread pointer
    // stays valid until the swap is confirmed
    if (graveyardCount_ < kMaxEffects)
        graveyard_[graveyardCount_++] = std::move(owned_[index]);

    // Shift remaining owned effects down
    for (int i = index; i < ownedCount_ - 1; ++i)
        owned_[i] = std::move(owned_[i + 1]);
    --ownedCount_;

    publishSnapshot();
}

void EffectChain::moveEffect(int from, int to) noexcept
{
    if (from < 0 || from >= ownedCount_ || to < 0 || to >= ownedCount_ || from == to)
        return;

    if (from < to)
    {
        // Rotate left: shift [from+1 .. to] one position toward from
        for (int i = from; i < to; ++i)
            owned_[i].swap(owned_[i + 1]);
    }
    else
    {
        // Rotate right: shift [to .. from-1] one position toward from
        for (int i = from; i > to; --i)
            owned_[i].swap(owned_[i - 1]);
    }

    publishSnapshot();
}

IEffect* EffectChain::getEffect(int index) noexcept
{
    if (index < 0 || index >= ownedCount_) return nullptr;
    return owned_[index].get();
}

void EffectChain::setBpm(float bpm) noexcept
{
    for (int i = 0; i < ownedCount_; ++i)
    {
        if (owned_[i] && owned_[i]->type() == EffectType::Delay)
        {
            static_cast<DelayEffect*>(owned_[i].get())->setSyncBpm(bpm);
        }
    }
}

void EffectChain::collectGarbage() noexcept
{
    if (graveyardCount_ == 0) return;

    // Safe to delete once the audio thread has processed at least one swap
    // after the removal was published (generation advanced beyond lastRemoveGen_)
    if (swapGeneration_.load(std::memory_order_acquire) > lastRemoveGen_)
    {
        for (int i = 0; i < graveyardCount_; ++i)
            graveyard_[i].reset();
        graveyardCount_ = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Private
// ─────────────────────────────────────────────────────────────────────────────

void EffectChain::publishSnapshot() noexcept
{
    ChainSnapshot snap;
    snap.count = ownedCount_;
    for (int i = 0; i < ownedCount_; ++i)
        snap.effects[i] = owned_[i].get();

    // If the queue is full, the audio thread hasn't drained yet — not a
    // problem; we'd just overwrite the queued snapshot with the newest state.
    // Drop the oldest entry if needed so we always push the latest snapshot.
    ChainSnapshot discard;
    while (!commandQueue_.tryPush(snap))
        commandQueue_.tryPop(discard);
}

} // namespace dsp
