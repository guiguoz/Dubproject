#include "engine/AudioGraph.h"
#include <cstring>
#include <cmath>

namespace engine {

void AudioGraph::setSlotRole(int slot, SlotRole role) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    roles_[slot].store(role, std::memory_order_relaxed);

    const auto sp = mix::spatialForType(slot, roleToMixType(role));
    slotPlayer_.setSpatial(slot, sp.pan, sp.width);

    if (role == SlotRole::Kick)
        kickSlot_.store(slot, std::memory_order_relaxed);
    else if (kickSlot_.load(std::memory_order_relaxed) == slot)
        findKickSlot();
}

void AudioGraph::prepare(double sampleRate, int maxBlockSize) noexcept
{
    maxBlock_ = maxBlockSize;
    slotPlayer_.prepareStretchers(2, static_cast<float>(sampleRate), maxBlockSize);
    autoMix_.prepare(static_cast<float>(sampleRate));
    delay_.prepare(sampleRate, maxBlockSize);
    limiter_.prepare(sampleRate);
    monoSubFilter_.prepare(sampleRate);
    monoSubFilter_.reset();

    mixL_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    mixR_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    slotMixL_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    slotMixR_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    delayInL_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    delayInR_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    interleavedScratch_.assign(static_cast<size_t>(maxBlockSize) * 2, 0.f);

    for (int s = 0; s < kMaxSlots; ++s) {
        slotBufL_[s].assign(static_cast<size_t>(maxBlockSize), 0.f);
        slotBufR_[s].assign(static_cast<size_t>(maxBlockSize), 0.f);
    }
    findKickSlot();
}

void AudioGraph::findKickSlot() noexcept
{
    kickSlot_.store(-1, std::memory_order_relaxed);
    for (int s = 0; s < kMaxSlots; ++s)
        if (roles_[s].load(std::memory_order_relaxed) == SlotRole::Kick) { kickSlot_.store(s, std::memory_order_relaxed); return; }
}

void AudioGraph::processBlock(const TransportState& ts,
                               const EventWithOffset* events, int numEvents,
                               float* output, int numFrames,
                               const float* extInL, const float* extInR,
                               const float* serumL, const float* serumR,
                               float serumGain) noexcept
{
    if (numFrames <= 0 || numFrames > maxBlock_) {
        const int safeFrames = std::clamp(numFrames, 0, maxBlock_);
        if (safeFrames > 0)
            std::memset(output, 0, static_cast<size_t>(safeFrames) * 2 * sizeof(float));
        return;
    }

    // 1. Vider les buffers de travail
    const auto n = static_cast<size_t>(numFrames);
    std::fill(mixL_.begin(),     mixL_.begin()     + numFrames, 0.f);
    std::fill(mixR_.begin(),     mixR_.begin()     + numFrames, 0.f);
    std::fill(delayInL_.begin(), delayInL_.begin() + numFrames, 0.f);
    std::fill(delayInR_.begin(), delayInR_.begin() + numFrames, 0.f);
    (void)n;

    // 2. Rendu par slot (buffers planaires préalloués)
    float* slotLp[kMaxSlots];
    float* slotRp[kMaxSlots];
    for (int s = 0; s < kMaxSlots; ++s) {
        std::fill(slotBufL_[s].begin(), slotBufL_[s].begin() + numFrames, 0.f);
        std::fill(slotBufR_[s].begin(), slotBufR_[s].begin() + numFrames, 0.f);
        slotLp[s] = slotBufL_[s].data();
        slotRp[s] = slotBufR_[s].data();
    }
    std::fill(slotMixL_.begin(), slotMixL_.begin() + numFrames, 0.f);
    std::fill(slotMixR_.begin(), slotMixR_.begin() + numFrames, 0.f);

    slotPlayer_.processBlock(ts, nullptr, numFrames, events, numEvents, slotLp, slotRp);

    // ── Sidechain kick → BASS/PAD uniquement (règle 3 AutoMix V2) ──────────────
    // Appliqué échantillon par échantillon sur slotBuf_ pour un ducking fluide.
    {
        const int kickIdx = kickSlot_.load(std::memory_order_relaxed);
        if (kickIdx >= 0 && kickIdx < kMaxSlots) {
            constexpr float minGain = 0.631f; // -4 dB max reduction
            for (int i = 0; i < numFrames; ++i) {
                const float kickSample = slotBufL_[kickIdx][static_cast<size_t>(i)]
                                       + slotBufR_[kickIdx][static_cast<size_t>(i)];
                const float kickEnv = autoMix_.advanceKickEnv(kickSample);
                const float gain    = 1.f - kickEnv * (1.f - minGain);

                for (int s = 0; s < kMaxSlots; ++s) {
                    if (s == kickIdx) continue;
                    const SlotRole role = static_cast<SlotRole>(roles_[s].load(std::memory_order_relaxed));
                    if (role == SlotRole::Bass || role == SlotRole::Pad) {
                        slotBufL_[s][static_cast<size_t>(i)] *= gain;
                        slotBufR_[s][static_cast<size_t>(i)] *= gain;
                    }
                }
            }
        }
    }

    // 3b. Accumulation des sorties directes des slots → mix
    for (int s = 0; s < kMaxSlots; ++s) {
        for (int i = 0; i < numFrames; ++i) {
            mixL_[static_cast<size_t>(i)] += slotBufL_[s][static_cast<size_t>(i)];
            mixR_[static_cast<size_t>(i)] += slotBufR_[s][static_cast<size_t>(i)];
        }
    }

    // 4. Entrees externes (EWI/sax dry + Serum)
    {
        const float ig = inputGain_.load(std::memory_order_relaxed);
        if (extInL != nullptr && extInR != nullptr && ig > 0.001f) {
            for (int i = 0; i < numFrames; ++i) {
                mixL_[static_cast<size_t>(i)] += extInL[i] * ig;
                mixR_[static_cast<size_t>(i)] += extInR[i] * ig;
            }
        } else if (extInL != nullptr && ig > 0.001f) {
            for (int i = 0; i < numFrames; ++i) {
                const float v = extInL[i] * ig;
                mixL_[static_cast<size_t>(i)] += v;
                mixR_[static_cast<size_t>(i)] += v;
            }
        }

        if (serumL != nullptr && serumR != nullptr && serumGain > 0.001f) {
            for (int i = 0; i < numFrames; ++i) {
                mixL_[static_cast<size_t>(i)] += serumL[i] * serumGain;
                mixR_[static_cast<size_t>(i)] += serumR[i] * serumGain;
            }
        }
    }

    // 5. PingPongDelay (additif par slot)
    for (int s = 0; s < kMaxSlots; ++s) {
        const float send = autoMix_.advanceDelayRamp(s, numFrames);
        if (send > 0.001f) {
            for (int i = 0; i < numFrames; ++i) {
                delayInL_[i] += slotBufL_[s][static_cast<size_t>(i)] * send;
                delayInR_[i] += slotBufR_[s][static_cast<size_t>(i)] * send;
            }
        }
    }

    delay_.processAdd(delayInL_.data(), delayInR_.data(),
                      mixL_.data(), mixR_.data(), numFrames);

    // 5b. MonoSubFilter BYPASS DEBUG (bug : ecrase L/R avec LP 120 Hz)
    // monoSubFilter_.process(mixL_.data(), mixR_.data(), numFrames);

    // 6. MasterLimiter BYPASS DEBUG
    // limiter_.process(mixL_.data(), mixR_.data(), numFrames);
    // autoMix_.notifyLimiterReduction(limiter_.getGainReductionDb());

    for (int i = 0; i < numFrames; ++i) {
        output[i * 2]     = mixL_[i];
        output[i * 2 + 1] = mixR_[i];
    }
}

} // namespace engine
