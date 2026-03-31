#include "Sampler.h"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace dsp {

void Sampler::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    beatClock_.prepare(sampleRate);
}

void Sampler::loadSample(int slot, const float* data, int numSamples,
                          double /*fileSampleRate*/) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    if (!data || numSamples <= 0)      return;

    auto& s  = slots_[static_cast<std::size_t>(slot)];
    auto& ps = playStates_[static_cast<std::size_t>(slot)];

    // Stop playback first so the audio thread no longer reads s.data.
    ps.playing.store(false, std::memory_order_release);
    ps.triggerPending.store(false, std::memory_order_release);
    ps.stopPending.store(false, std::memory_order_release);
    ps.readPos = 0;

    // Mark as unloaded while we modify data (audio thread checks loaded flag).
    s.loaded.store(false, std::memory_order_release);

    // Swap into a new vector to avoid the audio thread reading a
    // partially-reallocated buffer.  The old vector is freed here on the
    // GUI thread, which is safe because playback was stopped above.
    std::vector<float> newData(data, data + numSamples);
    s.data.swap(newData);
    s.sampleCount = numSamples;

    // Make data visible to the audio thread before setting the loaded flag.
    s.loaded.store(true, std::memory_order_release);
}

void Sampler::clearSlot(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& s = slots_[static_cast<std::size_t>(slot)];
    s.loaded.store(false, std::memory_order_release);
    stop(slot);
}

void Sampler::trigger(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    playStates_[static_cast<std::size_t>(slot)]
        .triggerPending.store(true, std::memory_order_release);
}

void Sampler::triggerQuantized(int slot, GridDiv div) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& ps = playStates_[static_cast<std::size_t>(slot)];
    ps.quantDiv.store(static_cast<int>(div), std::memory_order_relaxed);
    if (!beatClock_.isRunning())
    {
        // No BPM set: fall back to immediate trigger.
        ps.triggerPending.store(true, std::memory_order_release);
    }
    else
    {
        ps.quantTrigPending.store(true, std::memory_order_release);
    }
}

void Sampler::setSlotGrid(int slot, GridDiv div) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    playStates_[static_cast<std::size_t>(slot)]
        .quantDiv.store(static_cast<int>(div), std::memory_order_relaxed);
}

GridDiv Sampler::getSlotGrid(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return GridDiv::Quarter;
    return static_cast<GridDiv>(
        playStates_[static_cast<std::size_t>(slot)]
            .quantDiv.load(std::memory_order_relaxed));
}

bool Sampler::isPendingTrigger(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return playStates_[static_cast<std::size_t>(slot)]
        .quantTrigPending.load(std::memory_order_acquire);
}

void Sampler::stop(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    playStates_[static_cast<std::size_t>(slot)]
        .stopPending.store(true, std::memory_order_release);
}

void Sampler::stopAllSlots() noexcept
{
    for (int i = 0; i < kMaxSlots; ++i)
        stop(i);
}

void Sampler::setSidechainPair(int sourceSlot, int targetSlot) noexcept
{
    if (sourceSlot < 0 || sourceSlot >= kMaxSlots) return;
    if (targetSlot < 0 || targetSlot >= kMaxSlots) return;
    for (int i = 0; i < numSidechains_; ++i)
        if (sidechains_[static_cast<std::size_t>(i)].source == sourceSlot &&
            sidechains_[static_cast<std::size_t>(i)].target == targetSlot)
            return;
    if (numSidechains_ >= kMaxSidechainPairs) return;
    sidechains_[static_cast<std::size_t>(numSidechains_++)] = { sourceSlot, targetSlot, 0.f };
}

void Sampler::clearSidechain() noexcept
{
    sidechains_     = {};
    numSidechains_  = 0;
    for (auto& g : sidechainGains_) g = 1.f;
    for (auto& p : slotPeaks_)      p = 0.f;
}

void Sampler::setSlotGain(int slot, float gain) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(slot)].gain.store(gain, std::memory_order_relaxed);
}

void Sampler::setSlotLoop(int slot, bool loop) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(slot)].loopEnabled.store(loop, std::memory_order_relaxed);
}

void Sampler::setSlotOneShot(int slot, bool oneShot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(slot)].oneShot.store(oneShot, std::memory_order_relaxed);
}

void Sampler::setSlotMuted(int slot, bool muted) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(slot)].muted.store(muted, std::memory_order_relaxed);
}

bool Sampler::isLoaded(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return slots_[static_cast<std::size_t>(slot)].loaded.load(std::memory_order_acquire);
}

bool Sampler::isPlaying(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return playStates_[static_cast<std::size_t>(slot)].playing.load(std::memory_order_acquire);
}

bool Sampler::isSlotMuted(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return slots_[static_cast<std::size_t>(slot)].muted.load(std::memory_order_relaxed);
}

float Sampler::getSlotOutputPeak(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0.f;
    return outputPeaks_[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
}

float Sampler::getSlotPeakLevel(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0.f;
    const auto& s = slots_[static_cast<std::size_t>(slot)];
    if (!s.loaded.load(std::memory_order_acquire)) return 0.f;
    float peak = 0.f;
    for (const float v : s.data)
        peak = std::max(peak, std::abs(v));
    return peak;
}

int Sampler::getSlotSampleCount(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0;
    return slots_[static_cast<std::size_t>(slot)].sampleCount;
}

void Sampler::reloadSlotData(int slot, std::vector<float> newData) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& s  = slots_[static_cast<std::size_t>(slot)];
    auto& ps = playStates_[static_cast<std::size_t>(slot)];

    ps.playing.store(false, std::memory_order_release);
    ps.triggerPending.store(false, std::memory_order_release);
    ps.stopPending.store(false, std::memory_order_release);
    ps.readPos = 0;

    s.loaded.store(false, std::memory_order_release);
    s.data.swap(newData);
    s.sampleCount = static_cast<int>(s.data.size());
    s.loaded.store(true, std::memory_order_release);
}

void Sampler::process(float* buffer, int numSamples) noexcept
{
    // ── Sidechain: update gain multipliers from previous block's peaks ────────
    // Uses 1-block lookahead (imperceptible at typical block sizes).
    // Compressor model: 4:1 ratio, threshold = 0.25 linear.
    static constexpr float kSCAttack  = 0.5f;    // fast attack (1 block)
    static constexpr float kSCRelease = 0.985f;  // ~300 ms @ 44100/512
    static constexpr float kSCThresh  = 0.25f;
    for (int k = 0; k < numSidechains_; ++k)
    {
        auto& sc = sidechains_[static_cast<std::size_t>(k)];
        if (sc.source < 0 || sc.target < 0) continue;
        const float src = slotPeaks_[static_cast<std::size_t>(sc.source)];
        sc.envelope = (src > sc.envelope)
            ? sc.envelope + kSCAttack * (src - sc.envelope)
            : sc.envelope * kSCRelease;
        sidechainGains_[static_cast<std::size_t>(sc.target)] =
            (sc.envelope > kSCThresh)
                ? std::pow(kSCThresh / sc.envelope, 0.75f)  // 4:1 ratio: (T/L)^(1-1/R)
                : 1.f;
    }
    // Reset per-slot peaks for this block
    for (auto& p : slotPeaks_) p = 0.f;

    // Advance the beat clock and check for quantized trigger boundaries.
    const double phaseBefore = beatClock_.advance(numSamples);
    const double phaseAfter  = beatClock_.getPhase();

    for (int v = 0; v < kMaxSlots; ++v)
    {
        auto& ps  = playStates_[static_cast<std::size_t>(v)];
        auto& sl  = slots_[static_cast<std::size_t>(v)];

        // Resolve quantized trigger when a beat boundary is crossed.
        if (ps.quantTrigPending.load(std::memory_order_acquire))
        {
            const GridDiv div = static_cast<GridDiv>(
                ps.quantDiv.load(std::memory_order_relaxed));
            if (BeatClock::crossedBoundary(phaseBefore, phaseAfter, div))
            {
                ps.quantTrigPending.store(false, std::memory_order_relaxed);
                ps.triggerPending.store(true, std::memory_order_release);
            }
        }

        // Handle stop request first.
        if (ps.stopPending.load(std::memory_order_acquire))
        {
            ps.stopPending.store(false, std::memory_order_relaxed);
            ps.quantTrigPending.store(false, std::memory_order_relaxed);
            ps.playing.store(false, std::memory_order_relaxed);
            ps.readPos = 0;
        }

        // Handle trigger request.
        if (ps.triggerPending.load(std::memory_order_acquire))
        {
            ps.triggerPending.store(false, std::memory_order_relaxed);
            if (sl.loaded.load(std::memory_order_acquire))
            {
                ps.readPos = 0;
                ps.fadeIn  = 0;   // restart fade-in envelope
                ps.playing.store(true, std::memory_order_relaxed);
            }
        }

        if (!ps.playing.load(std::memory_order_relaxed))
        {
            outputPeaks_[static_cast<std::size_t>(v)].store(0.f, std::memory_order_relaxed);
            continue;
        }

        if (!sl.loaded.load(std::memory_order_acquire))
        {
            ps.playing.store(false, std::memory_order_relaxed);
            outputPeaks_[static_cast<std::size_t>(v)].store(0.f, std::memory_order_relaxed);
            continue;
        }

        const float gain      = sl.gain.load(std::memory_order_relaxed);
        const bool  loop      = sl.loopEnabled.load(std::memory_order_relaxed);
        const bool  muted     = sl.muted.load(std::memory_order_relaxed);
        const int   totalSamp = sl.sampleCount;

        // Fade-in length: ~2 ms at 44.1 kHz (scales with actual sample rate)
        static constexpr int kFadeLen = 88;

        float blockPeak = 0.f;
        for (int i = 0; i < numSamples; ++i)
        {
            if (ps.readPos >= totalSamp)
            {
                if (loop)
                {
                    ps.readPos = 0;
                    ps.fadeIn  = 0;  // re-apply fade at loop restart
                }
                else
                {
                    ps.playing.store(false, std::memory_order_relaxed);
                    break;
                }
            }

            if (!muted)
            {
                // Exponential fade-in to eliminate click/pop at trigger onset
                float fadeGain = 1.0f;
                if (ps.fadeIn < kFadeLen)
                {
                    fadeGain = 1.0f - std::exp(-5.0f * ps.fadeIn / static_cast<float>(kFadeLen));
                    ++ps.fadeIn;
                }

                const float s = gain * fadeGain * sl.data[static_cast<std::size_t>(ps.readPos)];
                buffer[i] += sidechainGains_[static_cast<std::size_t>(v)] * s;
                // Track peak for sidechain source envelope
                slotPeaks_[static_cast<std::size_t>(v)] =
                    std::max(slotPeaks_[static_cast<std::size_t>(v)], std::abs(s));
                blockPeak = std::max(blockPeak, std::abs(s));
            }
            ++ps.readPos;
        }
        outputPeaks_[static_cast<std::size_t>(v)].store(blockPeak, std::memory_order_relaxed);
    }
}

void Sampler::reset() noexcept
{
    for (int v = 0; v < kMaxSlots; ++v)
    {
        auto& ps = playStates_[static_cast<std::size_t>(v)];
        ps.playing.store(false, std::memory_order_relaxed);
        ps.triggerPending.store(false, std::memory_order_relaxed);
        ps.quantTrigPending.store(false, std::memory_order_relaxed);
        ps.stopPending.store(false, std::memory_order_relaxed);
        ps.readPos = 0;
    }
    clearSidechain();
    resetSpatial();
}

// ─────────────────────────────────────────────────────────────────────────────
// Spatial — pan + Haas width
// ─────────────────────────────────────────────────────────────────────────────

void Sampler::setSlotPan(int slot, float pan) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    // Equal-power law: map pan [-1, +1] to angle [0, π/2]
    constexpr float kPi = 3.14159265358979f;
    const float angle = (std::clamp(pan, -1.f, 1.f) + 1.f) * 0.25f * kPi;
    panL_[static_cast<std::size_t>(slot)].store(std::cos(angle),
                                                std::memory_order_relaxed);
    panR_[static_cast<std::size_t>(slot)].store(std::sin(angle),
                                                std::memory_order_relaxed);
}

void Sampler::setSlotHaasDelay(int slot, int samples) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    haasDelaySamples_[static_cast<std::size_t>(slot)] =
        std::clamp(samples, 0, kHaasDelayMax - 1);
}

void Sampler::resetSpatial() noexcept
{
    // Centre pan: equal-power at π/4 → cos = sin = 1/√2 ≈ 0.7071
    constexpr float kCentre = 0.70710678f;
    for (int v = 0; v < kMaxSlots; ++v)
    {
        const auto idx = static_cast<std::size_t>(v);
        panL_[idx].store(kCentre, std::memory_order_relaxed);
        panR_[idx].store(kCentre, std::memory_order_relaxed);
        haasDelaySamples_[idx] = 0;
        haasWritePos_[idx]     = 0;
        haasDelay_[idx].fill(0.f);
    }
}

inline float Sampler::applyHaasDelay(int slot, float sample) noexcept
{
    const auto   idx   = static_cast<std::size_t>(slot);
    const int    delay = haasDelaySamples_[idx];
    if (delay == 0) return sample;

    auto& buf = haasDelay_[idx];
    const int wp  = haasWritePos_[idx];
    buf[static_cast<std::size_t>(wp)] = sample;
    const int rp  = (wp - delay + kHaasDelayMax) & (kHaasDelayMax - 1);
    haasWritePos_[idx] = (wp + 1) & (kHaasDelayMax - 1);
    return buf[static_cast<std::size_t>(rp)];
}

// ─────────────────────────────────────────────────────────────────────────────
// processStereo
//
// Identical to process() but outputs separate L/R channels.
// Pan gains (panL_/panR_) follow equal-power law.
// Width (Haas delay) is applied to the right channel only; pan determines
// which channel is "stronger" — the weaker channel gets the delayed signal.
// For pan > 0 (right-heavy): left gets delayed (Haas on L).
// For pan < 0 (left-heavy):  right gets delayed (Haas on R).
// For pan = 0 (centre):      right gets delayed (subtle stereo width).
// ─────────────────────────────────────────────────────────────────────────────
void Sampler::processStereo(float* left, float* right, int numSamples) noexcept
{
    // ── Sidechain update (same as mono process) ───────────────────────────────
    static constexpr float kAttack   = 0.5f;
    static constexpr float kRelease  = 0.985f;
    static constexpr float kThresh   = 0.25f;
    static constexpr float kRatio    = 4.0f;

    for (int sc = 0; sc < numSidechains_; ++sc)
    {
        auto& pair = sidechains_[static_cast<std::size_t>(sc)];
        if (pair.source < 0 || pair.target < 0) continue;

        const float srcPeak = slotPeaks_[static_cast<std::size_t>(pair.source)];
        if (srcPeak > kThresh)
        {
            const float over = (srcPeak - kThresh) / kThresh;
            const float targetGain = 1.f - over * (1.f - 1.f / kRatio);
            pair.envelope += kAttack * (targetGain - pair.envelope);
        }
        else
        {
            pair.envelope += kRelease * (1.f - pair.envelope);
        }
        sidechainGains_[static_cast<std::size_t>(pair.target)] = pair.envelope;
    }
    std::fill(slotPeaks_, slotPeaks_ + kMaxSlots, 0.f);

    // ── Beat clock + quantized triggers (same as mono process) ───────────────
    const double phaseBefore = beatClock_.advance(numSamples);
    const double phaseAfter  = beatClock_.getPhase();

    for (int v = 0; v < kMaxSlots; ++v)
    {
        auto& ps = playStates_[static_cast<std::size_t>(v)];

        if (ps.quantTrigPending.load(std::memory_order_acquire))
        {
            const auto div = static_cast<GridDiv>(ps.quantDiv.load(std::memory_order_relaxed));
            if (BeatClock::crossedBoundary(phaseBefore, phaseAfter, div))
            {
                ps.quantTrigPending.store(false, std::memory_order_release);
                ps.triggerPending.store(true,  std::memory_order_release);
            }
        }
    }

    // ── Per-slot playback ─────────────────────────────────────────────────────
    static constexpr int kFadeLen = 88; // ~2 ms @ 44.1 kHz — must match process()
    for (int v = 0; v < kMaxSlots; ++v)
    {
        const auto  idx    = static_cast<std::size_t>(v);
        auto&       ps     = playStates_[idx];
        const auto& sl     = slots_[idx];

        // Handle stop / trigger requests
        if (ps.stopPending.load(std::memory_order_acquire))
        {
            ps.playing.store(false, std::memory_order_relaxed);
            ps.stopPending.store(false, std::memory_order_release);
        }
        if (ps.triggerPending.load(std::memory_order_acquire))
        {
            ps.readPos = 0;
            ps.fadeIn  = 0;
            ps.playing.store(true,  std::memory_order_relaxed);
            ps.triggerPending.store(false, std::memory_order_release);
        }

        if (!ps.playing.load(std::memory_order_relaxed)) continue;
        if (!sl.loaded.load(std::memory_order_acquire))  continue;

        const int   totalSamp = sl.sampleCount;
        const float gain      = sl.gain.load(std::memory_order_relaxed);
        const bool  loop      = sl.loopEnabled.load(std::memory_order_relaxed);
        const bool  muted     = sl.muted.load(std::memory_order_relaxed);

        const float gL = panL_[idx].load(std::memory_order_relaxed);
        const float gR = panR_[idx].load(std::memory_order_relaxed);
        // Haas is applied to the weaker channel:
        // pan > 0 → L is weaker → delay L; pan ≤ 0 → R is weaker → delay R
        const bool  haasOnLeft = (gL < gR);

        float blockPeak = 0.f;
        for (int i = 0; i < numSamples; ++i)
        {
            if (ps.readPos >= totalSamp)
            {
                if (loop) { ps.readPos = 0; ps.fadeIn = 0; }
                else       { ps.playing.store(false, std::memory_order_relaxed); break; }
            }

            if (!muted)
            {
                float fadeGain = 1.f;
                if (ps.fadeIn < kFadeLen)
                {
                    fadeGain = 1.f - std::exp(-5.f * ps.fadeIn
                                              / static_cast<float>(kFadeLen));
                    ++ps.fadeIn;
                }

                const float s = sidechainGains_[idx] * gain * fadeGain
                                * sl.data[static_cast<std::size_t>(ps.readPos)];

                if (haasOnLeft)
                {
                    left [i] += applyHaasDelay(v, s) * gL;
                    right[i] += s * gR;
                }
                else
                {
                    left [i] += s * gL;
                    right[i] += applyHaasDelay(v, s) * gR;
                }

                slotPeaks_[idx] = std::max(slotPeaks_[idx], std::abs(s));
                blockPeak        = std::max(blockPeak, std::abs(s));
            }
            ++ps.readPos;
        }
        outputPeaks_[idx].store(blockPeak, std::memory_order_relaxed);
    }
}

} // namespace dsp
