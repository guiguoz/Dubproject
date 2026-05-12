#include "DspPipeline.h"

#include "DspCommon.h"

namespace dsp
{

void DspPipeline::prepare(double sampleRate, int maxBlockSize) noexcept
{
    sampler_.prepare(sampleRate, maxBlockSize);
    bpmDetector_.prepare(sampleRate);
    dubDelay_.prepare(sampleRate, maxBlockSize);
    dubDelay_.setDiv(0);
    pingPongDelay_.prepare(sampleRate, maxBlockSize);

    tempBuffer_.assign(std::max(maxBlockSize, 256), 0.0f);
    tempBufL_.assign(std::max(maxBlockSize, 256), 0.0f);
    tempBufR_.assign(std::max(maxBlockSize, 256), 0.0f);
    tempSendL_.assign(std::max(maxBlockSize, 256), 0.0f);
    tempSendR_.assign(std::max(maxBlockSize, 256), 0.0f);
    monoSubFilter_.prepare(sampleRate);
    dubDelay_.setBpm(bpm_.load(std::memory_order_relaxed));
}

void DspPipeline::reset() noexcept
{
    sampler_.reset();
    bpmDetector_.reset();
    dubDelay_.reset();
    rmsRunning_ = 0.0f;
    smoothDuck_ = 1.0f;
    monoSubFilter_.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// process — mono audio thread
// ─────────────────────────────────────────────────────────────────────────────
void DspPipeline::process(float* buffer, int numSamples) noexcept
{
    // BPM detection (analysis only)
    bpmDetector_.process(buffer, numSamples);

    // Smoothed RMS
    for (int i = 0; i < numSamples; ++i)
        rmsRunning_ = kRmsAlpha * rmsRunning_ + (1.0f - kRmsAlpha) * std::abs(buffer[i]);
    rmsLevel_.store(rmsRunning_, std::memory_order_relaxed);

    // Sampler: drain MIDI queue, mix into buffer with optional ducking
    if (samplerEnabled_.load(std::memory_order_acquire))
    {
        SamplerEvent evt;
        while (midiEventQueue_.tryPop(evt))
        {
            if (evt.noteOn)
                sampler_.trigger(evt.slotIndex);
            else
                sampler_.stop(evt.slotIndex);
        }

        if (duckingEnabled_.load(std::memory_order_relaxed))
        {
            float targetDuck = 1.0f;
            if (rmsRunning_ > 0.05f)
            {
                const float ratio = std::min(1.0f, (rmsRunning_ - 0.05f) / 0.25f);
                targetDuck = 1.0f - ratio * 0.5f;
            }

            std::fill(tempBuffer_.begin(), tempBuffer_.begin() + numSamples, 0.0f);

            sampler_.process(tempBuffer_.data(), numSamples);

            static constexpr float kDuckCoeff = 0.002f;
            for (int i = 0; i < numSamples; ++i)
            {
                smoothDuck_ = std::clamp(smoothDuck_ + kDuckCoeff * (targetDuck - smoothDuck_),
                                         0.0f, 2.0f);
                tempBuffer_[i] *= smoothDuck_;
                buffer[i] += tempBuffer_[i];
            }
            currentDuckingGain_.store(smoothDuck_, std::memory_order_relaxed);
        }
        else
        {
            sampler_.process(buffer, numSamples);
            currentDuckingGain_.store(1.0f, std::memory_order_relaxed);
        }
    }
    else
    {
        currentDuckingGain_.store(1.0f, std::memory_order_relaxed);
    }

    masterLimiter_.process(buffer, numSamples);
}

// ─────────────────────────────────────────────────────────────────────────────
// processStereo — audio thread
// ─────────────────────────────────────────────────────────────────────────────
void DspPipeline::processStereo(float* left, float* right, int numSamples) noexcept
{
    // BPM detection (analysis on left)
    bpmDetector_.process(left, numSamples);

    // Smoothed RMS (left channel)
    for (int i = 0; i < numSamples; ++i)
        rmsRunning_ = kRmsAlpha * rmsRunning_ + (1.0f - kRmsAlpha) * std::abs(left[i]);
    rmsLevel_.store(rmsRunning_, std::memory_order_relaxed);

    // Sampler (MIDI drain + stereo mix with optional ducking)
    if (samplerEnabled_.load(std::memory_order_acquire))
    {
        SamplerEvent evt;
        while (midiEventQueue_.tryPop(evt))
        {
            if (evt.noteOn) sampler_.trigger(evt.slotIndex);
            else            sampler_.stop(evt.slotIndex);
        }

        std::fill(tempBufL_.begin(), tempBufL_.begin() + numSamples, 0.f);
        std::fill(tempBufR_.begin(), tempBufR_.begin() + numSamples, 0.f);
        std::fill(tempSendL_.begin(), tempSendL_.begin() + numSamples, 0.f);
        std::fill(tempSendR_.begin(), tempSendR_.begin() + numSamples, 0.f);

        sampler_.processStereo(tempBufL_.data(), tempBufR_.data(), numSamples,
                               tempSendL_.data(), tempSendR_.data());

        if (duckingEnabled_.load(std::memory_order_relaxed))
        {
            float targetDuck = 1.0f;
            if (rmsRunning_ > 0.05f)
            {
                const float ratio = std::min(1.0f, (rmsRunning_ - 0.05f) / 0.25f);
                targetDuck = 1.0f - ratio * 0.5f;
            }
            static constexpr float kDuckCoeff = 0.002f;
            for (int i = 0; i < numSamples; ++i)
            {
                smoothDuck_ = std::clamp(smoothDuck_ + kDuckCoeff * (targetDuck - smoothDuck_),
                                         0.0f, 2.0f);
                tempBufL_[i] *= smoothDuck_;
                tempBufR_[i] *= smoothDuck_;
                left [i] += tempBufL_[i];
                right[i] += tempBufR_[i];
            }
            currentDuckingGain_.store(smoothDuck_, std::memory_order_relaxed);
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
            {
                left [i] += tempBufL_[i];
                right[i] += tempBufR_[i];
            }
            currentDuckingGain_.store(1.0f, std::memory_order_relaxed);
        }

        dubDelay_.processAdd(tempSendL_.data(), tempSendR_.data(), left, right, numSamples);
        pingPongDelay_.processAdd(tempSendL_.data(), tempSendR_.data(), left, right, numSamples);
    }
    else
    {
        currentDuckingGain_.store(1.0f, std::memory_order_relaxed);
    }

    // Mono-sub < 120 Hz (PA mono compatibility)
    for (int i = 0; i < numSamples; ++i)
        monoSubFilter_.process(left[i], right[i]);

    masterLimiter_.process(left,  numSamples);
    masterLimiter_.process(right, numSamples);
}

void DspPipeline::setSamplerEnabled(bool enabled) noexcept
{
    samplerEnabled_.store(enabled, std::memory_order_release);
}

bool DspPipeline::isSamplerEnabled() const noexcept
{
    return samplerEnabled_.load(std::memory_order_acquire);
}

} // namespace dsp
