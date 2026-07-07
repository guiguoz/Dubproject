#include "Sampler.h"
#include <algorithm>
#include <cassert>
#include <cmath>

// RT-safety guarantee: atomic<float> must be lock-free (single instruction load/store).
static_assert(std::atomic<float>::is_always_lock_free,
              "std::atomic<float> is not lock-free on this platform — audio thread will block");

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Hermite 4-point (Catmull-Rom) interpolation.
// pts[0..3] = data at i-1, i, i+1, i+2. frac ∈ [0, 1).
// At frac=0.0 with integer readPos: returns pts[1] = data[i] exactly.
// ─────────────────────────────────────────────────────────────────────────────
inline float hermite4(const float* pts, double frac) noexcept
{
    const double c3 = 0.5 * (pts[3] - pts[0]) + 1.5 * (pts[1] - pts[2]);
    const double c2 = pts[0] - 2.5 * pts[1] + 2.0 * pts[2] - 0.5 * pts[3];
    const double c1 = 0.5 * (pts[2] - pts[0]);
    return static_cast<float>(pts[1] + frac * (c1 + frac * (c2 + frac * c3)));
}

// Tail-taper length (samples): silences the last N samples before wrap → 0→0 transition.
constexpr int kLoopTaperLen = 88;

// Read from data[] at fractional position with Hermite interpolation.
// Loop: neighbours wrap; tail taper fades to 0 in last kLoopTaperLen samples.
// One-shot: neighbours clamp at buffer boundaries.
// At rate=1.0, integer readPos (frac=0): identical to the former integer read (T-A1a).
inline float readHermite(const float* data, int totalSamp, double readPos, bool loop) noexcept
{
    const int    i1   = static_cast<int>(readPos);
    const double frac = readPos - static_cast<double>(i1);

    int im1, ip1, ip2;
    if (loop)
    {
        im1 = (i1 - 1 + totalSamp) % totalSamp;
        ip1 = (i1 + 1)             % totalSamp;
        ip2 = (i1 + 2)             % totalSamp;
    }
    else
    {
        im1 = std::max(0,             i1 - 1);
        ip1 = std::min(totalSamp - 1, i1 + 1);
        ip2 = std::min(totalSamp - 1, i1 + 2);
    }

    const float pts[4] = {
        data[static_cast<std::size_t>(im1)],
        data[static_cast<std::size_t>(i1 )],
        data[static_cast<std::size_t>(ip1)],
        data[static_cast<std::size_t>(ip2)],
    };
    const float s = hermite4(pts, frac);

    // Tail taper: linear fade on last kLoopTaperLen samples before the wrap point.
    if (loop)
    {
        const int taperLen = std::min(kLoopTaperLen, totalSamp / 4);
        if (taperLen > 0 && i1 >= totalSamp - taperLen)
        {
            const float t = std::max(0.f, float(totalSamp - i1) / float(taperLen));
            return s * t;
        }
    }
    return s;
}

// Coeff one-pole : 99 % convergence en `ms` sur un bloc de `n` samples.
// Forme d'usage : state = target + coeff * (state - target)
inline float sidechainCoeff(float ms, double sr, int n) noexcept
{
    if (ms <= 0.f || sr <= 0.0 || n <= 0) return 0.f;
    return std::exp(-4.6052f * static_cast<float>(n)
                    / (ms * 0.001f * static_cast<float>(sr)));
}

} // namespace

namespace dsp {

void Sampler::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    beatClock_.prepare(sampleRate);
    // T99 = 50 ms : exp(-4.6052 / (T*sr)) → 99 % convergence en 50 ms per-sample
    gainRampCoeff_ = std::exp(-4.6052f / (0.050f * static_cast<float>(sampleRate)));
    for (int v = 0; v < kMaxSlots; ++v)
        gainSmoothed_[v] = slots_[static_cast<std::size_t>(v)].gain.load(std::memory_order_relaxed);
    for (auto& d : slotDynamics_) d.prepare(sampleRate);
}

void Sampler::loadSample(int slot, const float* data, int numSamples,
                          double /*fileSampleRate*/) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    if (!data || numSamples <= 0)      return;

    auto& s  = slots_[static_cast<std::size_t>(slot)];

    int bgIdx = 1 - s.activeDataIdx.load(std::memory_order_acquire);

    // Do NOT set loaded=false here: it would abruptly kill voices mid-fadeout.
    // The double-buffer ensures the background slot is never read by live voices.
    // (Same rationale as clearSlot() — "let the fadeout finish".)
    std::vector<float> newData(data, data + numSamples);
    s.data[bgIdx].swap(newData);
    s.sampleCount[bgIdx].store(numSamples, std::memory_order_relaxed);

    s.activeDataIdx.store(bgIdx, std::memory_order_release);
    s.loaded.store(true, std::memory_order_release);
}

void Sampler::clearSlot(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& sl = slots_[static_cast<std::size_t>(slot)];
    // loaded=false coupe le playback immédiatement ET bloque les re-triggers futurs.
    // L'audio thread bail avant toute lecture PCM (ligne ~433). PCM laissé en mémoire
    // pour éviter le UAF ; écrasé au prochain loadSample().
    sl.loaded.store(false, std::memory_order_release);
    auto& ps = playStates_[static_cast<std::size_t>(slot)];
    ps.voices[0].playing = false;
    ps.voices[1].playing = false;
}

void Sampler::trigger(int slot, int offset) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& ps = playStates_[static_cast<std::size_t>(slot)];
    ps.triggerOffset.store(offset, std::memory_order_relaxed);
    ps.triggerPending.store(true, std::memory_order_release);
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

void Sampler::stop(int slot, StopMode mode) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    playStates_[static_cast<std::size_t>(slot)]
        .stopPending.store(static_cast<int>(mode), std::memory_order_release);
}

void Sampler::stopAllSlots(StopMode mode) noexcept
{
    for (int i = 0; i < kMaxSlots; ++i)
        stop(i, mode);
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

void Sampler::setSlotMuted(int slot, bool muted, bool quantized) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& sl = slots_[static_cast<std::size_t>(slot)];
    auto& ps = playStates_[static_cast<std::size_t>(slot)];

    if (muted) {
        ps.unmutePending.store(false, std::memory_order_release);
        sl.muted.store(true, std::memory_order_release);
    } else if (quantized) {
        ps.unmutePending.store(true, std::memory_order_release);
        // muted reste true — l'audio thread l'effacera au prochain step 0
    } else {
        ps.unmutePending.store(false, std::memory_order_release);
        sl.muted.store(false, std::memory_order_release);
    }
}

void Sampler::onTrackStep0(int slot, int offsetInBlock) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& ps = playStates_[static_cast<std::size_t>(slot)];

    if (ps.unmutePending.exchange(false, std::memory_order_acq_rel))
        slots_[static_cast<std::size_t>(slot)].muted
            .store(false, std::memory_order_release);

    if (ps.pendingPhaseReset.exchange(false, std::memory_order_acq_rel))
    {
        ps.triggerOffset.store(offsetInBlock, std::memory_order_relaxed);
        ps.triggerPending.store(true, std::memory_order_release);
    }

    // A2: swap pending stretchedBpm atomically at every step 0 (not just on phase reset).
    {
        auto& sl2 = slots_[static_cast<std::size_t>(slot)];
        const float newBpm = sl2.pendingStretchedBpm.exchange(0.f, std::memory_order_acq_rel);
        if (newBpm > 0.f)
            sl2.stretchedBpm.store(newBpm, std::memory_order_relaxed);

        // B3: quantized halftime factor — consume at step 0.
        {
            const float pHal = sl2.pendingHaltime.exchange(0.f, std::memory_order_acq_rel);
            if (pHal > 0.f)
                sl2.halttimeFactor.store(pHal, std::memory_order_relaxed);
        }

        // D3: reverse toggle — consume at step 0.
        if (sl2.pendingReverse.exchange(false, std::memory_order_acq_rel))
            ps.perfFx.reverse = !ps.perfFx.reverse;
    }
}

void Sampler::schedulePhaseReset(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    playStates_[static_cast<std::size_t>(slot)]
        .pendingPhaseReset.store(true, std::memory_order_release);
}

void Sampler::setSlotStretchedBpm(int slot, float bpm) noexcept
{
    if (slot < 0 || slot >= kMaxSlots || bpm <= 0.f) return;
    slots_[static_cast<std::size_t>(slot)]
        .pendingStretchedBpm.store(bpm, std::memory_order_release);
}

void Sampler::setSlotTransposeSemitones(int slot, float semitones) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(slot)]
        .transposeRatio.store(std::pow(2.f, semitones / 12.f),
                              std::memory_order_relaxed);
}

void Sampler::setPendingHaltime(int slot, float factor) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(slot)]
        .pendingHaltime.store(factor, std::memory_order_release);
}

float Sampler::getHalttimeFactor(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 1.f;
    return slots_[static_cast<std::size_t>(slot)]
        .halttimeFactor.load(std::memory_order_relaxed);
}

void Sampler::triggerTapeStop(int slot, float durationMs) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& sl = slots_[static_cast<std::size_t>(slot)];
    sl.tapeDurMs.store(durationMs > 0.f ? durationMs : 500.f, std::memory_order_relaxed);
    sl.perfCmd.store(static_cast<int>(PerfCmd::TapeStop), std::memory_order_release);
}

void Sampler::triggerTapeStart(int slot, float durationMs) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& sl = slots_[static_cast<std::size_t>(slot)];
    sl.tapeDurMs.store(durationMs > 0.f ? durationMs : 500.f, std::memory_order_relaxed);
    sl.perfCmd.store(static_cast<int>(PerfCmd::TapeStart), std::memory_order_release);
}

void Sampler::triggerGlide(int slot, float startSemitones, float timeMs) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& sl = slots_[static_cast<std::size_t>(slot)];
    sl.glideStartSt.store(startSemitones, std::memory_order_relaxed);
    sl.glideTimeMs.store(timeMs > 0.f ? timeMs : 800.f, std::memory_order_relaxed);
    sl.perfCmd.store(static_cast<int>(PerfCmd::Glide), std::memory_order_release);
}

void Sampler::cancelPerfFx(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(slot)]
        .perfCmd.store(static_cast<int>(PerfCmd::Cancel), std::memory_order_release);
}

void Sampler::toggleReverse(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& sl = slots_[static_cast<std::size_t>(slot)];
    // GUI is sole writer; audio thread only exchange(false) at step 0.
    sl.pendingReverse.store(!sl.pendingReverse.load(std::memory_order_relaxed),
                            std::memory_order_release);
}

bool Sampler::isReversed(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return playStates_[static_cast<std::size_t>(slot)].perfFx.reverse;
}

void Sampler::saveOriginalPcm(int slot, const float* data, int numSamples, double sr) noexcept
{
    if (slot < 0 || slot >= kMaxSlots || !data || numSamples <= 0) return;
    auto& sl = slots_[static_cast<std::size_t>(slot)];
    sl.originalPcm.assign(data, data + numSamples);
    sl.originalSr = sr;
}

bool Sampler::getOriginalPcm(int slot, std::vector<float>& out, double& outSr) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    const auto& sl = slots_[static_cast<std::size_t>(slot)];
    if (sl.originalPcm.empty()) return false;
    out   = sl.originalPcm;
    outSr = sl.originalSr;
    return true;
}

void Sampler::setSlotDelaySend(int slot, float send) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    const float clamped = send < 0.f ? 0.f : (send > 1.f ? 1.f : send);
    slots_[static_cast<std::size_t>(slot)].delaySend.store(clamped, std::memory_order_relaxed);
}

float Sampler::getSlotDelaySend(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0.f;
    return slots_[static_cast<std::size_t>(slot)].delaySend.load(std::memory_order_relaxed);
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

PcmView Sampler::getSlotPcmView(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return {};
    const auto& s = slots_[static_cast<std::size_t>(slot)];
    const auto& v = s.data[s.activeDataIdx.load(std::memory_order_relaxed)];
    return { v.data(), static_cast<int>(v.size()) };
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
    return s.sampleCount[s.activeDataIdx.load(std::memory_order_relaxed)].load(std::memory_order_relaxed);
}

float Sampler::getSlotPlayheadRatio(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0.f;
    const auto& ps = playStates_[static_cast<std::size_t>(slot)];
    const auto& sl = slots_     [static_cast<std::size_t>(slot)];
    
    int activeIdx = sl.activeDataIdx.load(std::memory_order_relaxed);
    if (!sl.loaded.load() || sl.sampleCount[activeIdx].load(std::memory_order_relaxed) <= 0) return 0.f;

    // readPos is written only by the audio thread
    return static_cast<float>(ps.voices[ps.currentVoice].readPos)
           / static_cast<float>(sl.sampleCount[activeIdx].load(std::memory_order_relaxed));
}

void Sampler::reloadSlotData(int slot, std::vector<float> newData) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    auto& s  = slots_[static_cast<std::size_t>(slot)];

    int bgIdx = 1 - s.activeDataIdx.load(std::memory_order_acquire);

    s.data[bgIdx].swap(newData);
    s.sampleCount[bgIdx].store(static_cast<int>(s.data[bgIdx].size()),
                                std::memory_order_relaxed);

    s.activeDataIdx.store(bgIdx, std::memory_order_release);
    // loaded was already true; no store needed.
}

void Sampler::process(float* buffer, int numSamples) noexcept
{
    // ── Sidechain: update gain multipliers from previous block's peaks ────────
    constexpr float kSCThresh  = 0.25f;
    const float scAttCoeff = sidechainCoeff(  5.f, sampleRate_, numSamples);
    const float scRelCoeff = sidechainCoeff(120.f, sampleRate_, numSamples);
    for (int k = 0; k < numSidechains_; ++k)
    {
        auto& sc = sidechains_[static_cast<std::size_t>(k)];
        if (sc.source < 0 || sc.target < 0) continue;
        const float src = slotPeaks_[static_cast<std::size_t>(sc.source)];
        const float targetEnv = (src > kSCThresh)
            ? std::pow(kSCThresh / src, 0.75f)
            : 1.f;
        const float coeff = (src > sc.envelope) ? scAttCoeff : scRelCoeff;
        sc.envelope = targetEnv + coeff * (sc.envelope - targetEnv);
        sidechainGains_[static_cast<std::size_t>(sc.target)] = sc.envelope;
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

        static constexpr int kFadeInLen          = 32;
        static constexpr int kRetriggerFadeOutLen = 256;

        const int stopMode = ps.stopPending.load(std::memory_order_acquire);
        if (stopMode != 0)
        {
            ps.stopPending.store(0, std::memory_order_release);
            const int fadeLen = stopFadeSamples(static_cast<StopMode>(stopMode));
            for (int vi = 0; vi < 2; ++vi)
            {
                if (ps.voices[vi].playing)
                {
                    ps.voices[vi].fadeOut          = fadeLen;
                    ps.voices[vi].fadeOutTotal     = (fadeLen > 0) ? fadeLen : 1;
                    ps.voices[vi].retriggering     = true;
                    ps.voices[vi].stopAfterFadeOut = true;
                }
            }
        }
        if (ps.triggerPending.load(std::memory_order_acquire))
        {
            ps.triggerPending.store(false, std::memory_order_release);
            const int trigOffset = ps.triggerOffset.load(std::memory_order_relaxed);

            if (sl.loaded.load(std::memory_order_acquire))
            {
                int activeDataIdx = sl.activeDataIdx.load(std::memory_order_relaxed);
                int cv = ps.currentVoice;
                bool isSameSample = (ps.voices[cv].dataIdx == activeDataIdx);
                bool isLoop = sl.loopEnabled.load(std::memory_order_relaxed);

                if (isSameSample && isLoop && ps.voices[cv].playing)
                {
                    // P0.2: resync if drifted from readPos=0 at step boundary.
                    const int totalSamp0 = sl.sampleCount[ps.voices[cv].dataIdx].load(std::memory_order_relaxed);
                    const int pos0       = static_cast<int>(ps.voices[cv].readPos);
                    const int dist       = (totalSamp0 > 0) ? std::min(pos0, totalSamp0 - pos0) : 0;
                    if (dist > 128 && totalSamp0 > 0)
                    {
                        ps.voices[cv].fadeOut          = kRetriggerFadeOutLen;
                        ps.voices[cv].fadeOutTotal     = kRetriggerFadeOutLen;
                        ps.voices[cv].retriggering     = true;
                        ps.voices[cv].stopAfterFadeOut = true;
                        ps.currentVoice                = 1 - cv;
                        const int nv2                  = ps.currentVoice;
                        ps.voices[nv2].dataIdx          = activeDataIdx;
                        ps.voices[nv2].readPos          = 0.0;
                        ps.voices[nv2].fadeIn           = 0;
                        ps.voices[nv2].startOffset      = trigOffset;
                        ps.voices[nv2].retriggering     = false;
                        ps.voices[nv2].stopAfterFadeOut = false;
                        ps.voices[nv2].playing          = true;
                    }
                    else
                    {
                        ps.voices[cv].stopAfterFadeOut = false;
                        ps.voices[cv].retriggering     = false;
                    }
                }
                else
                {
                    if (isSameSample && ps.voices[cv].playing)
                    {
                        // P0.4: choke — fade out current voice, start new voice immediately
                        ps.voices[cv].fadeOut          = kRetriggerFadeOutLen;
                        ps.voices[cv].fadeOutTotal     = kRetriggerFadeOutLen;
                        ps.voices[cv].retriggering     = true;
                        ps.voices[cv].stopAfterFadeOut = true;
                        ps.currentVoice                = 1 - cv;
                        const int nvc                  = ps.currentVoice;
                        ps.voices[nvc].dataIdx          = activeDataIdx;
                        ps.voices[nvc].readPos          = 0.0;
                        ps.voices[nvc].fadeIn           = 0;
                        ps.voices[nvc].startOffset      = trigOffset;
                        ps.voices[nvc].retriggering     = false;
                        ps.voices[nvc].stopAfterFadeOut = false;
                        ps.voices[nvc].playing          = true;
                    }
                    else
                    {
                        // Polyphonic fade-out of current voice
                        if (ps.voices[cv].playing)
                        {
                            const int kNormalFade = stopFadeSamples(StopMode::Normal);
                            ps.voices[cv].fadeOut          = kNormalFade;
                            ps.voices[cv].fadeOutTotal     = kNormalFade;
                            ps.voices[cv].retriggering     = true;
                            ps.voices[cv].stopAfterFadeOut = true;
                        }

                        // Start new voice
                        ps.currentVoice = 1 - cv;
                        int nv = ps.currentVoice;
                        ps.voices[nv].dataIdx          = activeDataIdx;
                        ps.voices[nv].readPos          = 0.0;
                        ps.voices[nv].fadeIn           = 0;
                        ps.voices[nv].startOffset      = trigOffset;
                        ps.voices[nv].retriggering     = false;
                        ps.voices[nv].stopAfterFadeOut = false;
                        ps.voices[nv].playing          = true;
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

        const float targetGain = sl.gain.load(std::memory_order_relaxed);
        const bool  loop      = sl.loopEnabled.load(std::memory_order_relaxed);
        const bool  muted     = sl.muted.load(std::memory_order_relaxed);
        const int   solo      = soloSlot_.load(std::memory_order_relaxed);
        const bool  soloMuted = (solo >= 0 && v != solo);

        // A2+C1+B3+D: rate = tempoRatio × transposeRatio × halttimeFactor × perfRatio × sign
        {
            const float strBpm  = sl.stretchedBpm.load(std::memory_order_relaxed);
            const float projBpm = beatClock_.getBpm();
            const double tempoR = (strBpm > 0.f && projBpm > 0.f)
                ? static_cast<double>(projBpm / strBpm) : 1.0;
            const double tpR  = static_cast<double>(sl.transposeRatio.load(std::memory_order_relaxed));
            const double halR = static_cast<double>(sl.halttimeFactor.load(std::memory_order_relaxed));
            const double baseRate = tempoR * tpR * halR;

            // D: PerfFX state machine — consume command, advance, compute perfRatio.
            using PFXMode = PlayState::PerfFxState::Mode;
            const int cmd = sl.perfCmd.exchange(0, std::memory_order_acq_rel);
            if (cmd != 0)
            {
                const float nSr = static_cast<float>(sampleRate_);
                if (cmd == static_cast<int>(PerfCmd::TapeStop))
                {
                    const float dur = sl.tapeDurMs.load(std::memory_order_relaxed) * 0.001f * nSr;
                    ps.perfFx.mode    = PFXMode::TapeStop;
                    ps.perfFx.elapsed = 0.f;
                    ps.perfFx.total   = dur > 1.f ? dur : 1.f;
                }
                else if (cmd == static_cast<int>(PerfCmd::TapeStart))
                {
                    const float dur = sl.tapeDurMs.load(std::memory_order_relaxed) * 0.001f * nSr;
                    ps.perfFx.mode    = PFXMode::TapeStart;
                    ps.perfFx.elapsed = 0.f;
                    ps.perfFx.total   = dur > 1.f ? dur : 1.f;
                    ps.perfFx.ratio   = 0.f;
                }
                else if (cmd == static_cast<int>(PerfCmd::Glide))
                {
                    const float startSt  = sl.glideStartSt.load(std::memory_order_relaxed);
                    const float timeSamp = sl.glideTimeMs.load(std::memory_order_relaxed) * 0.001f * nSr;
                    ps.perfFx.mode      = PFXMode::Glide;
                    ps.perfFx.glideFrom = std::pow(2.f, startSt / 12.f);
                    ps.perfFx.ratio     = ps.perfFx.glideFrom;
                    ps.perfFx.elapsed   = 0.f;
                    ps.perfFx.glideK    = timeSamp > 0.f ? 4.6052f / timeSamp : 10.f;
                }
                else  // Cancel
                {
                    ps.perfFx.mode  = PFXMode::None;
                    ps.perfFx.ratio = 1.f;
                }
            }
            const float nBlk = static_cast<float>(numSamples);
            switch (ps.perfFx.mode)
            {
            case PFXMode::TapeStop:
            {
                ps.perfFx.elapsed += nBlk;
                const float t = std::min(1.f, ps.perfFx.elapsed / ps.perfFx.total);
                ps.perfFx.ratio = (1.f - t) * (1.f - t);
                if (t >= 1.f) { ps.perfFx.mode = PFXMode::None; ps.perfFx.ratio = 0.f; }
                break;
            }
            case PFXMode::TapeStart:
            {
                ps.perfFx.elapsed += nBlk;
                const float t = std::min(1.f, ps.perfFx.elapsed / ps.perfFx.total);
                ps.perfFx.ratio = t * t;
                if (t >= 1.f) { ps.perfFx.mode = PFXMode::None; ps.perfFx.ratio = 1.f; }
                break;
            }
            case PFXMode::Glide:
            {
                ps.perfFx.elapsed += nBlk;
                const float decay = std::exp(-ps.perfFx.glideK * ps.perfFx.elapsed);
                ps.perfFx.ratio = 1.f + (ps.perfFx.glideFrom - 1.f) * decay;
                if (std::abs(ps.perfFx.ratio - 1.f) < 0.001f)
                {
                    ps.perfFx.mode  = PFXMode::None;
                    ps.perfFx.ratio = 1.f;
                }
                break;
            }
            case PFXMode::None:
            default:
                break;
            }

            const double sign     = ps.perfFx.reverse ? -1.0 : 1.0;
            const double finalRate = baseRate * static_cast<double>(ps.perfFx.ratio) * sign;
            for (int vi = 0; vi < 2; ++vi)
                if (ps.voices[vi].playing)
                    ps.voices[vi].rate = finalRate;
        }

        float blockPeak = 0.f;
        for (int i = 0; i < numSamples; ++i)
        {
            gainSmoothed_[static_cast<std::size_t>(v)] =
                gainRampCoeff_ * gainSmoothed_[static_cast<std::size_t>(v)]
                + (1.f - gainRampCoeff_) * targetGain;
            const float gain = std::clamp(gainSmoothed_[static_cast<std::size_t>(v)], 0.f, 2.f);

            float s_mix = 0.f;

            for (int vi = 0; vi < 2; ++vi)
            {
                auto& vState = ps.voices[vi];
                if (!vState.playing) continue;

                const int totalSamp = sl.sampleCount[vState.dataIdx].load(std::memory_order_relaxed);
                if (totalSamp <= 0) {
                    vState.playing = false;
                    continue;
                }

                // P0.3: delay voice start to step boundary
                if (vState.startOffset > 0)
                {
                    if (i < vState.startOffset)
                        continue;
                    vState.startOffset = 0;
                }

                if (vState.retriggering)
                {
                    if (vState.fadeOut > 0 && !muted && !soloMuted)
                    {
                        const float fadeOutGain = static_cast<float>(vState.fadeOut)
                                                 / static_cast<float>(vState.fadeOutTotal);
                        const int   safePos = std::clamp(static_cast<int>(vState.readPos), 0, totalSamp - 1);
                        s_mix += gain * fadeOutGain * sl.data[vState.dataIdx][static_cast<std::size_t>(safePos)];
                    }
                    --vState.fadeOut;
                    vState.readPos += vState.rate;
                    if (vState.fadeOut <= 0)
                    {
                        vState.retriggering = false;
                        vState.readPos      = 0.0;
                        vState.fadeIn       = 0;
                        if (vState.stopAfterFadeOut)
                        {
                            vState.stopAfterFadeOut = false;
                            vState.playing = false;
                        }
                    }
                    continue;
                }

                while (vState.readPos >= static_cast<double>(totalSamp))
                {
                    if (loop) { vState.readPos -= static_cast<double>(totalSamp); vState.fadeIn = 0; }
                    else      { vState.playing = false; break; }
                }
                if (!vState.playing) continue;
                // Reverse-ready: wrap negative readPos for future rate<0 support.
                while (vState.readPos < 0.0)
                {
                    if (loop) { vState.readPos += static_cast<double>(totalSamp); vState.fadeIn = 0; }
                    else      { vState.playing = false; break; }
                }
                if (!vState.playing) continue;

                if (!muted && !soloMuted)
                {
                    float fadeGain = 1.0f;
                    if (vState.fadeIn < kFadeInLen)
                    {
                        fadeGain = static_cast<float>(vState.fadeIn) / static_cast<float>(kFadeInLen);
                        ++vState.fadeIn;
                    }

                    const float* pcm = sl.data[vState.dataIdx].data();
                    const float s = readHermite(pcm, totalSamp, vState.readPos, loop);
                    s_mix += gain * fadeGain * s;
                }
                vState.readPos += vState.rate;
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
        ps.stopPending.store(0, std::memory_order_relaxed);
        ps.unmutePending.store(false, std::memory_order_relaxed);
        ps.pendingPhaseReset.store(false, std::memory_order_relaxed);
        ps.triggerOffset.store(0, std::memory_order_relaxed);
        for (int vi=0; vi<2; ++vi) {
            ps.voices[vi].playing   = false;
            ps.voices[vi].readPos   = 0.0;
            ps.voices[vi].rate      = 1.0;
            ps.voices[vi].startOffset = 0;
        }
        ps.perfFx = {};
        gainSmoothed_[v] = slots_[static_cast<std::size_t>(v)].gain.load(std::memory_order_relaxed);
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
void Sampler::processStereo(float* left, float* right, int numSamples,
                             float* sendBufL, float* sendBufR) noexcept
{
    constexpr float kSCThresh  = 0.25f;
    constexpr float kSCRatio   = 4.0f;
    const float scAttCoeff = sidechainCoeff(  5.f, sampleRate_, numSamples);
    const float scRelCoeff = sidechainCoeff(120.f, sampleRate_, numSamples);

    for (int sc = 0; sc < numSidechains_; ++sc)
    {
        auto& pair = sidechains_[static_cast<std::size_t>(sc)];
        if (pair.source < 0 || pair.target < 0) continue;

        const float srcPeak = slotPeaks_[static_cast<std::size_t>(pair.source)];
        float targetEnv;
        if (srcPeak > kSCThresh)
        {
            const float over = (srcPeak - kSCThresh) / kSCThresh;
            targetEnv = 1.f - over * (1.f - 1.f / kSCRatio);
        }
        else
        {
            targetEnv = 1.f;
        }
        const float coeff = (srcPeak > pair.envelope) ? scAttCoeff : scRelCoeff;
        pair.envelope = targetEnv + coeff * (pair.envelope - targetEnv);
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

    static constexpr int kFadeInLen          = 32;
    static constexpr int kRetriggerFadeOutLen = 256;

    for (int v = 0; v < kMaxSlots; ++v)
    {
        const auto  idx = static_cast<std::size_t>(v);
        auto&       ps  = playStates_[idx];
        auto&       sl  = slots_[idx];

        const int stopMode = ps.stopPending.load(std::memory_order_acquire);
        if (stopMode != 0)
        {
            ps.stopPending.store(0, std::memory_order_release);
            const int fadeLen = stopFadeSamples(static_cast<StopMode>(stopMode));
            for (int vi = 0; vi < 2; ++vi)
            {
                if (ps.voices[vi].playing)
                {
                    ps.voices[vi].fadeOut          = fadeLen;
                    ps.voices[vi].fadeOutTotal     = (fadeLen > 0) ? fadeLen : 1;
                    ps.voices[vi].retriggering     = true;
                    ps.voices[vi].stopAfterFadeOut = true;
                }
            }
        }
        if (ps.triggerPending.load(std::memory_order_acquire))
        {
            ps.triggerPending.store(false, std::memory_order_release);
            const int trigOffset = ps.triggerOffset.load(std::memory_order_relaxed);

            if (sl.loaded.load(std::memory_order_acquire))
            {
                int activeDataIdx = sl.activeDataIdx.load(std::memory_order_relaxed);
                int cv = ps.currentVoice;
                bool isSameSample = (ps.voices[cv].dataIdx == activeDataIdx);
                bool isLoop = sl.loopEnabled.load(std::memory_order_relaxed);

                if (isSameSample && isLoop && ps.voices[cv].playing)
                {
                    // P0.2: resync if drifted from readPos=0 at step boundary.
                    const int totalSamp0 = sl.sampleCount[ps.voices[cv].dataIdx].load(std::memory_order_relaxed);
                    const int pos0       = static_cast<int>(ps.voices[cv].readPos);
                    const int dist       = (totalSamp0 > 0) ? std::min(pos0, totalSamp0 - pos0) : 0;
                    if (dist > 128 && totalSamp0 > 0)
                    {
                        ps.voices[cv].fadeOut          = kRetriggerFadeOutLen;
                        ps.voices[cv].fadeOutTotal     = kRetriggerFadeOutLen;
                        ps.voices[cv].retriggering     = true;
                        ps.voices[cv].stopAfterFadeOut = true;
                        ps.currentVoice                = 1 - cv;
                        const int nv2                  = ps.currentVoice;
                        ps.voices[nv2].dataIdx          = activeDataIdx;
                        ps.voices[nv2].readPos          = 0.0;
                        ps.voices[nv2].fadeIn           = 0;
                        ps.voices[nv2].startOffset      = trigOffset;
                        ps.voices[nv2].retriggering     = false;
                        ps.voices[nv2].stopAfterFadeOut = false;
                        ps.voices[nv2].playing          = true;
                    }
                    else
                    {
                        ps.voices[cv].stopAfterFadeOut = false;
                        ps.voices[cv].retriggering     = false;
                    }
                }
                else
                {
                    if (isSameSample && ps.voices[cv].playing)
                    {
                        // P0.4: choke — fade out current voice, start new voice immediately
                        ps.voices[cv].fadeOut          = kRetriggerFadeOutLen;
                        ps.voices[cv].fadeOutTotal     = kRetriggerFadeOutLen;
                        ps.voices[cv].retriggering     = true;
                        ps.voices[cv].stopAfterFadeOut = true;
                        ps.currentVoice                = 1 - cv;
                        const int nvc                  = ps.currentVoice;
                        ps.voices[nvc].dataIdx          = activeDataIdx;
                        ps.voices[nvc].readPos          = 0.0;
                        ps.voices[nvc].fadeIn           = 0;
                        ps.voices[nvc].startOffset      = trigOffset;
                        ps.voices[nvc].retriggering     = false;
                        ps.voices[nvc].stopAfterFadeOut = false;
                        ps.voices[nvc].playing          = true;
                    }
                    else
                    {
                        // Polyphonic switch
                        if (ps.voices[cv].playing)
                        {
                            const int kNormalFade = stopFadeSamples(StopMode::Normal);
                            ps.voices[cv].fadeOut          = kNormalFade;
                            ps.voices[cv].fadeOutTotal     = kNormalFade;
                            ps.voices[cv].retriggering     = true;
                            ps.voices[cv].stopAfterFadeOut = true;
                        }

                        // Start new voice
                        ps.currentVoice = 1 - cv;
                        int nv = ps.currentVoice;
                        ps.voices[nv].dataIdx          = activeDataIdx;
                        ps.voices[nv].readPos          = 0.0;
                        ps.voices[nv].fadeIn           = 0;
                        ps.voices[nv].startOffset      = trigOffset;
                        ps.voices[nv].retriggering     = false;
                        ps.voices[nv].stopAfterFadeOut = false;
                        ps.voices[nv].playing          = true;
                    }
                }
            }
        }

        if (!ps.voices[0].playing && !ps.voices[1].playing) continue;
        
        if (!sl.loaded.load(std::memory_order_acquire))
        {
            for (int vi=0; vi<2; ++vi) ps.voices[vi].playing = false;
            outputPeaks_[idx].store(0.f, std::memory_order_relaxed);
            continue;
        }

        const float targetGain = sl.gain.load(std::memory_order_relaxed);
        const bool  loop      = sl.loopEnabled.load(std::memory_order_relaxed);
        const bool  muted     = sl.muted.load(std::memory_order_relaxed);
        const int   solo      = soloSlot_.load(std::memory_order_relaxed);
        const bool  soloMuted = (solo >= 0 && v != solo);

        // A2+C1+B3+D: rate = tempoRatio × transposeRatio × halttimeFactor × perfRatio × sign
        {
            const float strBpm  = sl.stretchedBpm.load(std::memory_order_relaxed);
            const float projBpm = beatClock_.getBpm();
            const double tempoR = (strBpm > 0.f && projBpm > 0.f)
                ? static_cast<double>(projBpm / strBpm) : 1.0;
            const double tpR  = static_cast<double>(sl.transposeRatio.load(std::memory_order_relaxed));
            const double halR = static_cast<double>(sl.halttimeFactor.load(std::memory_order_relaxed));
            const double baseRate = tempoR * tpR * halR;

            using PFXMode = PlayState::PerfFxState::Mode;
            const int cmd = sl.perfCmd.exchange(0, std::memory_order_acq_rel);
            if (cmd != 0)
            {
                const float nSr = static_cast<float>(sampleRate_);
                if (cmd == static_cast<int>(PerfCmd::TapeStop))
                {
                    const float dur = sl.tapeDurMs.load(std::memory_order_relaxed) * 0.001f * nSr;
                    ps.perfFx.mode    = PFXMode::TapeStop;
                    ps.perfFx.elapsed = 0.f;
                    ps.perfFx.total   = dur > 1.f ? dur : 1.f;
                }
                else if (cmd == static_cast<int>(PerfCmd::TapeStart))
                {
                    const float dur = sl.tapeDurMs.load(std::memory_order_relaxed) * 0.001f * nSr;
                    ps.perfFx.mode    = PFXMode::TapeStart;
                    ps.perfFx.elapsed = 0.f;
                    ps.perfFx.total   = dur > 1.f ? dur : 1.f;
                    ps.perfFx.ratio   = 0.f;
                }
                else if (cmd == static_cast<int>(PerfCmd::Glide))
                {
                    const float startSt  = sl.glideStartSt.load(std::memory_order_relaxed);
                    const float timeSamp = sl.glideTimeMs.load(std::memory_order_relaxed) * 0.001f * nSr;
                    ps.perfFx.mode      = PFXMode::Glide;
                    ps.perfFx.glideFrom = std::pow(2.f, startSt / 12.f);
                    ps.perfFx.ratio     = ps.perfFx.glideFrom;
                    ps.perfFx.elapsed   = 0.f;
                    ps.perfFx.glideK    = timeSamp > 0.f ? 4.6052f / timeSamp : 10.f;
                }
                else  // Cancel
                {
                    ps.perfFx.mode  = PFXMode::None;
                    ps.perfFx.ratio = 1.f;
                }
            }
            const float nBlk = static_cast<float>(numSamples);
            switch (ps.perfFx.mode)
            {
            case PFXMode::TapeStop:
            {
                ps.perfFx.elapsed += nBlk;
                const float t = std::min(1.f, ps.perfFx.elapsed / ps.perfFx.total);
                ps.perfFx.ratio = (1.f - t) * (1.f - t);
                if (t >= 1.f) { ps.perfFx.mode = PFXMode::None; ps.perfFx.ratio = 0.f; }
                break;
            }
            case PFXMode::TapeStart:
            {
                ps.perfFx.elapsed += nBlk;
                const float t = std::min(1.f, ps.perfFx.elapsed / ps.perfFx.total);
                ps.perfFx.ratio = t * t;
                if (t >= 1.f) { ps.perfFx.mode = PFXMode::None; ps.perfFx.ratio = 1.f; }
                break;
            }
            case PFXMode::Glide:
            {
                ps.perfFx.elapsed += nBlk;
                const float decay = std::exp(-ps.perfFx.glideK * ps.perfFx.elapsed);
                ps.perfFx.ratio = 1.f + (ps.perfFx.glideFrom - 1.f) * decay;
                if (std::abs(ps.perfFx.ratio - 1.f) < 0.001f)
                {
                    ps.perfFx.mode  = PFXMode::None;
                    ps.perfFx.ratio = 1.f;
                }
                break;
            }
            case PFXMode::None:
            default:
                break;
            }

            const double sign      = ps.perfFx.reverse ? -1.0 : 1.0;
            const double finalRate = baseRate * static_cast<double>(ps.perfFx.ratio) * sign;
            for (int vi = 0; vi < 2; ++vi)
                if (ps.voices[vi].playing)
                    ps.voices[vi].rate = finalRate;
        }

        const float gL = panL_[idx].load(std::memory_order_relaxed);
        const float gR = panR_[idx].load(std::memory_order_relaxed);
        const bool  haasOnLeft = (gL < gR);

        slotDynamics_[idx].beginBlock();
        float blockPeak = 0.f;
        for (int i = 0; i < numSamples; ++i)
        {
            gainSmoothed_[idx] =
                gainRampCoeff_ * gainSmoothed_[idx] + (1.f - gainRampCoeff_) * targetGain;
            const float gain = std::clamp(gainSmoothed_[idx], 0.f, 2.f);

            float s_mix = 0.f;

            for (int vi = 0; vi < 2; ++vi)
            {
                auto& vState = ps.voices[vi];
                if (!vState.playing) continue;

                const int totalSamp = sl.sampleCount[vState.dataIdx].load(std::memory_order_relaxed);
                if (totalSamp <= 0) {
                    vState.playing = false;
                    continue;
                }

                // P0.3: delay voice start to step boundary
                if (vState.startOffset > 0)
                {
                    if (i < vState.startOffset)
                        continue;
                    vState.startOffset = 0;
                }

                if (vState.retriggering)
                {
                    if (vState.fadeOut > 0 && !muted && !soloMuted)
                    {
                        const float fadeOutGain = static_cast<float>(vState.fadeOut)
                                                 / static_cast<float>(vState.fadeOutTotal);
                        const int   safePos = std::clamp(static_cast<int>(vState.readPos), 0, totalSamp - 1);
                        s_mix += gain * fadeOutGain * sl.data[vState.dataIdx][static_cast<std::size_t>(safePos)];
                    }
                    --vState.fadeOut;
                    vState.readPos += vState.rate;
                    if (vState.fadeOut <= 0)
                    {
                        vState.retriggering = false;
                        vState.readPos      = 0.0;
                        vState.fadeIn       = 0;
                        if (vState.stopAfterFadeOut)
                        {
                            vState.stopAfterFadeOut = false;
                            vState.playing = false;
                        }
                    }
                    continue;
                }

                while (vState.readPos >= static_cast<double>(totalSamp))
                {
                    if (loop) { vState.readPos -= static_cast<double>(totalSamp); vState.fadeIn = 0; }
                    else      { vState.playing = false; break; }
                }
                if (!vState.playing) continue;
                // Reverse-ready: wrap negative readPos for future rate<0 support.
                while (vState.readPos < 0.0)
                {
                    if (loop) { vState.readPos += static_cast<double>(totalSamp); vState.fadeIn = 0; }
                    else      { vState.playing = false; break; }
                }
                if (!vState.playing) continue;

                if (!muted && !soloMuted)
                {
                    float fadeGain = 1.f;
                    if (vState.fadeIn < kFadeInLen)
                    {
                        fadeGain = static_cast<float>(vState.fadeIn) / static_cast<float>(kFadeInLen);
                        ++vState.fadeIn;
                    }

                    const float* pcm = sl.data[vState.dataIdx].data();
                    const float s = readHermite(pcm, totalSamp, vState.readPos, loop);
                    s_mix += gain * fadeGain * s;
                }
                vState.readPos += vState.rate;
            }

            float s_final = sidechainGains_[idx] * s_mix;
            slotDynamics_[idx].processSample(s_final);

            if (sendBufL != nullptr) {
                const float sendGain = sl.delaySend.load(std::memory_order_relaxed);
                if (sendGain > 0.f) {
                    sendBufL[i] += s_final * sendGain;
                    sendBufR[i] += s_final * sendGain;
                }
            }

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
        slotDynamics_[idx].endBlock();
    }
}

} // namespace dsp
