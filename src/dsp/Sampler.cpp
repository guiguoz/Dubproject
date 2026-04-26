#include "Sampler.h"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

// Equal-power overlap crossfade at loop wrap (~23 ms @ 44.1 kHz). Dub-friendly for sub bass.
constexpr int kLoopXfadeMax = 1024;
constexpr int kLoopXfadeMin = 8;

inline int loopXfadeSamples(int totalSamp) noexcept
{
    if (totalSamp < kLoopXfadeMin * 2)
        return 0;
    const int n = std::min(kLoopXfadeMax, totalSamp / 2);
    return (n >= kLoopXfadeMin) ? n : 0;
}

inline float sampleLoopWithXfade(const float* data, int totalSamp, int readPos, bool loop) noexcept
{
    if (!loop)
        return data[static_cast<std::size_t>(readPos)];
    const int n = loopXfadeSamples(totalSamp);
    if (n == 0)
        return data[static_cast<std::size_t>(readPos)];
    if (readPos < totalSamp - n)
        return data[static_cast<std::size_t>(readPos)];
    const float phase = static_cast<float>(readPos - (totalSamp - n)) / static_cast<float>(n);
    constexpr float kHalfPi = 1.57079632679489661923f;
    const float wOut = std::cos(phase * kHalfPi);
    const float wIn  = std::sin(phase * kHalfPi);
    const int headIdx = readPos - (totalSamp - n);
    return data[static_cast<std::size_t>(readPos)] * wOut
         + data[static_cast<std::size_t>(headIdx)] * wIn;
}

} // namespace

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

    int bgIdx = 1 - s.activeDataIdx.load(std::memory_order_acquire);
    
    s.loaded.store(false, std::memory_order_release);

    std::vector<float> newData(data, data + numSamples);
    s.data[bgIdx].swap(newData);
    s.sampleCount[bgIdx] = numSamples;

    s.activeDataIdx.store(bgIdx, std::memory_order_release);
    s.loaded.store(true, std::memory_order_release);
}

void Sampler::clearSlot(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    stop(slot);
    // We do not set loaded=false immediately so the fadeout can finish!
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
    const auto& ps = playStates_[static_cast<std::size_t>(slot)];
    return ps.voices[0].playing || ps.voices[1].playing;
}

bool Sampler::isSlotMuted(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return slots_[static_cast<std::size_t>(slot)].muted.load(std::memory_order_relaxed);
}

float Sampler::getSlotGain(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 1.f;
    return slots_[static_cast<std::size_t>(slot)].gain.load(std::memory_order_relaxed);
}

void Sampler::setSoloSlot(int slot) noexcept
{
    soloSlot_.store(slot, std::memory_order_relaxed);
}

void Sampler::clearSolo() noexcept
{
    soloSlot_.store(-1, std::memory_order_relaxed);
}

int Sampler::getSoloSlot() const noexcept
{
    return soloSlot_.load(std::memory_order_relaxed);
}

std::vector<float> Sampler::getSlotPcmSnapshot(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return {};
    auto& s = slots_[static_cast<std::size_t>(slot)];
    return s.data[s.activeDataIdx.load(std::memory_order_relaxed)];
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
    for (const float v : s.data[s.activeDataIdx.load(std::memory_order_relaxed)])
        peak = std::max(peak, std::abs(v));
    return peak;
}

int Sampler::getSlotSampleCount(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0;
    auto& s = slots_[static_cast<std::size_t>(slot)];
    return s.sampleCount[s.activeDataIdx.load(std::memory_order_relaxed)];
}

float Sampler::getSlotPlayheadRatio(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0.f;
    const auto& ps = playStates_[static_cast<std::size_t>(slot)];
    const auto& sl = slots_     [static_cast<std::size_t>(slot)];
    
    int activeIdx = sl.activeDataIdx.load(std::memory_order_relaxed);
    if (!sl.loaded.load() || sl.sampleCount[activeIdx] <= 0) return 0.f;
    
    // readPos is written only by the audio thread
    return static_cast<float>(ps.voices[ps.currentVoice].readPos)
           / static_cast<float>(sl.sampleCount[activeIdx]);
}

void Sampler::reloadSlotData(int slot, std::vector<float> newData) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& s  = slots_[static_cast<std::size_t>(slot)];

    int bgIdx = 1 - s.activeDataIdx.load(std::memory_order_acquire);
    
    s.loaded.store(false, std::memory_order_release);
    s.data[bgIdx].swap(newData);
    s.sampleCount[bgIdx] = static_cast<int>(s.data[bgIdx].size());
    
    s.activeDataIdx.store(bgIdx, std::memory_order_release);
    s.loaded.store(true, std::memory_order_release);
}

void Sampler::process(float* buffer, int numSamples) noexcept
{
    // ── Sidechain: update gain multipliers from previous block's peaks ────────
    static constexpr float kSCAttack  = 0.5f;
    static constexpr float kSCRelease = 0.985f;
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
                ? std::pow(kSCThresh / sc.envelope, 0.75f)
                : 1.f;
    }
    for (auto& p : slotPeaks_) p = 0.f;

    const double phaseBefore = beatClock_.advance(numSamples);
    const double phaseAfter  = beatClock_.getPhase();

    for (int v = 0; v < kMaxSlots; ++v)
    {
        auto& ps  = playStates_[static_cast<std::size_t>(v)];
        auto& sl  = slots_[static_cast<std::size_t>(v)];

        if (ps.quantTrigPending.load(std::memory_order_acquire))
        {
            const GridDiv div = static_cast<GridDiv>(ps.quantDiv.load(std::memory_order_relaxed));
            if (BeatClock::crossedBoundary(phaseBefore, phaseAfter, div))
            {
                ps.quantTrigPending.store(false, std::memory_order_relaxed);
                ps.triggerPending.store(true, std::memory_order_release);
            }
        }

        static constexpr int kFadeInLen  =  88;
        static constexpr int kRetriggerFadeOutLen = 256;
        const int kStopFadeOutLen = static_cast<int>(sampleRate_ * 0.35);

        if (ps.stopPending.load(std::memory_order_acquire))
        {
            ps.stopPending.store(false, std::memory_order_release);
            for (int vi = 0; vi < 2; ++vi)
            {
                if (ps.voices[vi].playing)
                {
                    ps.voices[vi].fadeOut      = kStopFadeOutLen;
                    ps.voices[vi].fadeOutTotal = kStopFadeOutLen;
                    ps.voices[vi].retriggering = true;
                    ps.voices[vi].stopAfterFadeOut = true;
                }
            }
        }
        if (ps.triggerPending.load(std::memory_order_acquire))
        {
            ps.triggerPending.store(false, std::memory_order_release);
            
            if (sl.loaded.load(std::memory_order_acquire))
            {
                int activeDataIdx = sl.activeDataIdx.load(std::memory_order_relaxed);
                int cv = ps.currentVoice;
                bool isSameSample = (ps.voices[cv].dataIdx == activeDataIdx);
                bool isLoop = sl.loopEnabled.load(std::memory_order_relaxed);
                
                if (isSameSample && isLoop && ps.voices[cv].playing)
                {
                    // Legato mode: keep playing seamlessly
                    ps.voices[cv].stopAfterFadeOut = false;
                    ps.voices[cv].retriggering = false;
                }
                else
                {
                    if (isSameSample && ps.voices[cv].playing)
                    {
                        // Choke/Retrigger same sample
                        ps.voices[cv].fadeOut      = kRetriggerFadeOutLen;
                        ps.voices[cv].fadeOutTotal = kRetriggerFadeOutLen;
                        ps.voices[cv].retriggering = true;
                        ps.voices[cv].stopAfterFadeOut = false;
                    }
                    else
                    {
                        // Polyphonic fade-out of current voice
                        if (ps.voices[cv].playing)
                        {
                            ps.voices[cv].fadeOut      = kStopFadeOutLen;
                            ps.voices[cv].fadeOutTotal = kStopFadeOutLen;
                            ps.voices[cv].retriggering = true;
                            ps.voices[cv].stopAfterFadeOut = true;
                        }
                        
                        // Start new voice
                        ps.currentVoice = 1 - cv;
                        int nv = ps.currentVoice;
                        ps.voices[nv].dataIdx = activeDataIdx;
                        ps.voices[nv].readPos = 0;
                        ps.voices[nv].fadeIn  = 0;
                        ps.voices[nv].retriggering = false;
                        ps.voices[nv].stopAfterFadeOut = false;
                        ps.voices[nv].playing = true;
                    }
                }
            }
        }

        if (!ps.voices[0].playing && !ps.voices[1].playing)
        {
            outputPeaks_[static_cast<std::size_t>(v)].store(0.f, std::memory_order_relaxed);
            continue;
        }

        if (!sl.loaded.load(std::memory_order_acquire))
        {
            for (int vi=0; vi<2; ++vi) ps.voices[vi].playing = false;
            outputPeaks_[static_cast<std::size_t>(v)].store(0.f, std::memory_order_relaxed);
            continue;
        }

        const float gain      = sl.gain.load(std::memory_order_relaxed);
        const bool  loop      = sl.loopEnabled.load(std::memory_order_relaxed);
        const bool  muted     = sl.muted.load(std::memory_order_relaxed);
        const int   solo      = soloSlot_.load(std::memory_order_relaxed);
        const bool  soloMuted = (solo >= 0 && v != solo);

        float blockPeak = 0.f;
        for (int i = 0; i < numSamples; ++i)
        {
            float s_mix = 0.f;
            
            for (int vi = 0; vi < 2; ++vi)
            {
                auto& vState = ps.voices[vi];
                if (!vState.playing) continue;
                
                const int totalSamp = sl.sampleCount[vState.dataIdx];
                if (totalSamp <= 0) {
                    vState.playing = false;
                    continue;
                }

                if (vState.retriggering)
                {
                    if (vState.fadeOut > 0 && !muted && !soloMuted)
                    {
                        const float fadeOutGain = static_cast<float>(vState.fadeOut)
                                                 / static_cast<float>(vState.fadeOutTotal);
                        const int   safePos = std::min(vState.readPos, totalSamp - 1);
                        s_mix += gain * fadeOutGain * sl.data[vState.dataIdx][static_cast<std::size_t>(safePos)];
                    }
                    --vState.fadeOut;
                    ++vState.readPos;
                    if (vState.fadeOut <= 0)
                    {
                        vState.retriggering = false;
                        vState.readPos      = 0;
                        vState.fadeIn       = 0;
                        if (vState.stopAfterFadeOut)
                        {
                            vState.stopAfterFadeOut = false;
                            vState.playing = false;
                        }
                    }
                    continue;
                }

                if (vState.readPos >= totalSamp)
                {
                    if (loop)
                    {
                        const int nxf = loopXfadeSamples(totalSamp);
                        if (nxf > 0)
                        {
                            vState.readPos = nxf;
                            vState.fadeIn  = kFadeInLen; // head already blended in xfade — skip note attack
                        }
                        else
                        {
                            vState.readPos = 0;
                            vState.fadeIn  = 0;
                        }
                    }
                    else
                    {
                        vState.playing = false;
                        continue;
                    }
                }

                if (!muted && !soloMuted)
                {
                    float fadeGain = 1.0f;
                    if (vState.fadeIn < kFadeInLen)
                    {
                        fadeGain = 1.0f - std::exp(-5.0f * vState.fadeIn / static_cast<float>(kFadeInLen));
                        ++vState.fadeIn;
                    }

                    const float* pcm = sl.data[vState.dataIdx].data();
                    const float s = sampleLoopWithXfade(pcm, totalSamp, vState.readPos, loop);
                    s_mix += gain * fadeGain * s;
                }
                ++vState.readPos;
            }
            
            const float s_final = sidechainGains_[static_cast<std::size_t>(v)] * s_mix;
            buffer[i] += s_final;
            slotPeaks_[static_cast<std::size_t>(v)] = std::max(slotPeaks_[static_cast<std::size_t>(v)], std::abs(s_final));
            blockPeak = std::max(blockPeak, std::abs(s_final));
        }
        outputPeaks_[static_cast<std::size_t>(v)].store(blockPeak, std::memory_order_relaxed);
    }
}
void Sampler::reset() noexcept
{
    for (int v = 0; v < kMaxSlots; ++v)
    {
        auto& ps = playStates_[static_cast<std::size_t>(v)];
        ps.triggerPending.store(false, std::memory_order_relaxed);
        ps.quantTrigPending.store(false, std::memory_order_relaxed);
        ps.stopPending.store(false, std::memory_order_relaxed);
        for (int vi=0; vi<2; ++vi) {
            ps.voices[vi].playing = false;
            ps.voices[vi].readPos = 0;
        }
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

    static constexpr int kFadeInLen  =  88;
    static constexpr int kRetriggerFadeOutLen = 256;
    const int kStopFadeOutLen = static_cast<int>(sampleRate_ * 0.35);

    for (int v = 0; v < kMaxSlots; ++v)
    {
        const auto  idx    = static_cast<std::size_t>(v);
        auto&       ps     = playStates_[idx];
        const auto& sl     = slots_[idx];

        if (ps.stopPending.load(std::memory_order_acquire))
        {
            ps.stopPending.store(false, std::memory_order_release);
            for (int vi = 0; vi < 2; ++vi)
            {
                if (ps.voices[vi].playing)
                {
                    ps.voices[vi].fadeOut      = kStopFadeOutLen;
                    ps.voices[vi].fadeOutTotal = kStopFadeOutLen;
                    ps.voices[vi].retriggering = true;
                    ps.voices[vi].stopAfterFadeOut = true;
                }
            }
        }
        if (ps.triggerPending.load(std::memory_order_acquire))
        {
            ps.triggerPending.store(false, std::memory_order_release);
            
            if (sl.loaded.load(std::memory_order_acquire))
            {
                int activeDataIdx = sl.activeDataIdx.load(std::memory_order_relaxed);
                int cv = ps.currentVoice;
                bool isSameSample = (ps.voices[cv].dataIdx == activeDataIdx);
                bool isLoop = sl.loopEnabled.load(std::memory_order_relaxed);
                
                if (isSameSample && isLoop && ps.voices[cv].playing)
                {
                    // Legato mode
                    ps.voices[cv].stopAfterFadeOut = false;
                    ps.voices[cv].retriggering = false;
                }
                else
                {
                    if (isSameSample && ps.voices[cv].playing)
                    {
                        // Choke
                        ps.voices[cv].fadeOut      = kRetriggerFadeOutLen;
                        ps.voices[cv].fadeOutTotal = kRetriggerFadeOutLen;
                        ps.voices[cv].retriggering = true;
                        ps.voices[cv].stopAfterFadeOut = false;
                    }
                    else
                    {
                        // Polyphonic switch
                        if (ps.voices[cv].playing)
                        {
                            ps.voices[cv].fadeOut      = kStopFadeOutLen;
                            ps.voices[cv].fadeOutTotal = kStopFadeOutLen;
                            ps.voices[cv].retriggering = true;
                            ps.voices[cv].stopAfterFadeOut = true;
                        }
                        
                        ps.currentVoice = 1 - cv;
                        int nv = ps.currentVoice;
                        ps.voices[nv].dataIdx = activeDataIdx;
                        ps.voices[nv].readPos = 0;
                        ps.voices[nv].fadeIn  = 0;
                        ps.voices[nv].retriggering = false;
                        ps.voices[nv].stopAfterFadeOut = false;
                        ps.voices[nv].playing = true;
                    }
                }
            }
        }

        if (!ps.voices[0].playing && !ps.voices[1].playing) continue;
        
        if (!sl.loaded.load(std::memory_order_acquire))
        {
            for (int vi=0; vi<2; ++vi) ps.voices[vi].playing = false;
            continue;
        }

        const float gain      = sl.gain.load(std::memory_order_relaxed);
        const bool  loop      = sl.loopEnabled.load(std::memory_order_relaxed);
        const bool  muted     = sl.muted.load(std::memory_order_relaxed);
        const int   solo      = soloSlot_.load(std::memory_order_relaxed);
        const bool  soloMuted = (solo >= 0 && v != solo);

        const float gL = panL_[idx].load(std::memory_order_relaxed);
        const float gR = panR_[idx].load(std::memory_order_relaxed);
        const bool  haasOnLeft = (gL < gR);

        float blockPeak = 0.f;
        for (int i = 0; i < numSamples; ++i)
        {
            float s_mix = 0.f;

            for (int vi = 0; vi < 2; ++vi)
            {
                auto& vState = ps.voices[vi];
                if (!vState.playing) continue;

                const int totalSamp = sl.sampleCount[vState.dataIdx];
                if (totalSamp <= 0) {
                    vState.playing = false;
                    continue;
                }

                if (vState.retriggering)
                {
                    if (vState.fadeOut > 0 && !muted && !soloMuted)
                    {
                        const float fadeOutGain = static_cast<float>(vState.fadeOut)
                                                 / static_cast<float>(vState.fadeOutTotal);
                        const int   safePos = std::min(vState.readPos, totalSamp - 1);
                        s_mix += gain * fadeOutGain * sl.data[vState.dataIdx][static_cast<std::size_t>(safePos)];
                    }
                    --vState.fadeOut;
                    ++vState.readPos;
                    if (vState.fadeOut <= 0)
                    {
                        vState.retriggering = false;
                        vState.readPos      = 0;
                        vState.fadeIn       = 0;
                        if (vState.stopAfterFadeOut)
                        {
                            vState.stopAfterFadeOut = false;
                            vState.playing = false;
                        }
                    }
                    continue;
                }

                if (vState.readPos >= totalSamp)
                {
                    if (loop)
                    {
                        const int nxf = loopXfadeSamples(totalSamp);
                        if (nxf > 0)
                        {
                            vState.readPos = nxf;
                            vState.fadeIn  = kFadeInLen;
                        }
                        else
                        {
                            vState.readPos = 0;
                            vState.fadeIn  = 0;
                        }
                    }
                    else
                    {
                        vState.playing = false;
                        continue;
                    }
                }

                if (!muted && !soloMuted)
                {
                    float fadeGain = 1.f;
                    if (vState.fadeIn < kFadeInLen)
                    {
                        fadeGain = 1.f - std::exp(-5.f * vState.fadeIn
                                                  / static_cast<float>(kFadeInLen));
                        ++vState.fadeIn;
                    }

                    const float* pcm = sl.data[vState.dataIdx].data();
                    const float s = sampleLoopWithXfade(pcm, totalSamp, vState.readPos, loop);
                    s_mix += gain * fadeGain * s;
                }
                ++vState.readPos;
            }

            const float s_final = sidechainGains_[idx] * s_mix;

            if (haasOnLeft)
            {
                left [i] += applyHaasDelay(v, s_final) * gL;
                right[i] += s_final * gR;
            }
            else
            {
                left [i] += s_final * gL;
                right[i] += applyHaasDelay(v, s_final) * gR;
            }

            slotPeaks_[idx] = std::max(slotPeaks_[idx], std::abs(s_final));
            blockPeak        = std::max(blockPeak, std::abs(s_final));
        }
        outputPeaks_[idx].store(blockPeak, std::memory_order_relaxed);
    }
}

} // namespace dsp
