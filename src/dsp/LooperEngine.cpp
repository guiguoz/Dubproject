#include "LooperEngine.h"
#include <algorithm>
#include <cmath>

namespace dsp {

void LooperEngine::prepare(double sampleRate, int maxBars) noexcept
{
    sampleRate_ = sampleRate;
    // maxBars bars at the slowest supported BPM (40) = 6s/bar
    buf_.setSize(2, static_cast<int>(sampleRate * 6.0 * maxBars), false, true, false);
    buf_.clear();
    loopLenSamples_ = 0;
    recPos_ = 0;
    playPos_ = 0;
    displayBars_.store(0.f, std::memory_order_relaxed);
    state_.store(State::Idle, std::memory_order_relaxed);
    updateBarSize();
}

void LooperEngine::setBpm(float bpm) noexcept
{
    bpm_.store(bpm, std::memory_order_relaxed);
    updateBarSize();
}

void LooperEngine::updateBarSize() noexcept
{
    const float bpm = bpm_.load(std::memory_order_relaxed);
    if (bpm > 0.f && sampleRate_ > 0.0)
        barSizeSamples_ = static_cast<int>(sampleRate_ * 60.0 / bpm * 4.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// process — audio thread only
// ─────────────────────────────────────────────────────────────────────────────
void LooperEngine::process(const float* inL, const float* inR,
                            float* outL,       float* outR,
                            int numSamples, double beatPhase) noexcept
{
    // Priority: clear
    if (pendingClear_.exchange(false, std::memory_order_relaxed))
    {
        state_.store(State::Idle, std::memory_order_relaxed);
        recPos_ = 0;
        playPos_ = 0;
        loopLenSamples_ = 0;
        buf_.clear();
        displayBars_.store(0.f, std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; }
        return;
    }

    State st = state_.load(std::memory_order_relaxed);

    // Consume one press per block (double-press accumulated safely)
    if (pendingPresses_.exchange(0, std::memory_order_relaxed) > 0)
    {
        switch (st)
        {
            case State::Idle:
                st = State::Armed;
                state_.store(st, std::memory_order_relaxed);
                break;
            case State::Armed:
                st = State::Idle;
                state_.store(st, std::memory_order_relaxed);
                break;
            case State::Recording:
                if (recPos_ > 0 && barSizeSamples_ > 0)
                {
                    // Arm stop at next bar boundary
                    const int bars = (recPos_ + barSizeSamples_ - 1) / barSizeSamples_;
                    loopLenSamples_ = std::min(bars * barSizeSamples_, buf_.getNumSamples());
                }
                else if (recPos_ == 0)
                {
                    // Accidental press before anything was recorded → cancel
                    st = State::Idle;
                    state_.store(st, std::memory_order_relaxed);
                }
                break;
            case State::Playing:
                st = State::Overdubbing;
                state_.store(st, std::memory_order_relaxed);
                break;
            case State::Overdubbing:
                st = State::Playing;
                state_.store(st, std::memory_order_relaxed);
                break;
        }
    }

    // ── IDLE ──────────────────────────────────────────────────────────────────
    if (st == State::Idle)
    {
        for (int i = 0; i < numSamples; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; }
        return;
    }

    // ── ARMED — wait for bar downbeat ─────────────────────────────────────────
    if (st == State::Armed)
    {
        const float bpm = bpm_.load(std::memory_order_relaxed);
        if (bpm > 0.f)
        {
            const double beatsPerBlock = numSamples / (sampleRate_ * 60.0 / bpm);
            const double phaseEnd = beatPhase + beatsPerBlock;
            if (static_cast<int64_t>(phaseEnd / 4.0) > static_cast<int64_t>(beatPhase / 4.0))
            {
                recPos_ = 0;
                loopLenSamples_ = 0;
                st = State::Recording;
                state_.store(st, std::memory_order_relaxed);
                // Fall through to Recording handling below
            }
        }
        if (st == State::Armed)
        {
            for (int i = 0; i < numSamples; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; }
            return;
        }
    }

    // ── RECORDING ─────────────────────────────────────────────────────────────
    if (st == State::Recording)
    {
        float* bL = buf_.getWritePointer(0);
        float* bR = buf_.getWritePointer(1);
        const int maxLen = buf_.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            const float srcL = inL[i];
            const float srcR = inR[i];
            outL[i] = srcL;
            outR[i] = srcR;
            if (recPos_ < maxLen)
            {
                bL[recPos_] = srcL;
                bR[recPos_] = srcR;
                ++recPos_;
            }
        }

        // Check arm-stop: transition to PLAYING when target length reached
        if (loopLenSamples_ > 0 && recPos_ >= loopLenSamples_)
        {
            playPos_ = 0;
            state_.store(State::Playing, std::memory_order_relaxed);
        }

        const float bars = (barSizeSamples_ > 0)
            ? static_cast<float>(recPos_) / barSizeSamples_ : 0.f;
        displayBars_.store(bars, std::memory_order_relaxed);
        return;
    }

    // ── PLAYING / OVERDUBBING ─────────────────────────────────────────────────
    if (loopLenSamples_ <= 0)
    {
        // Safety: no loop content yet — pass through
        for (int i = 0; i < numSamples; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; }
        return;
    }

    const bool isOvr = (st == State::Overdubbing);
    const OverdubMode mode = overdubMode_.load(std::memory_order_relaxed);
    float* bL = buf_.getWritePointer(0);
    float* bR = buf_.getWritePointer(1);

    for (int i = 0; i < numSamples; ++i)
    {
        const int   pos   = playPos_;
        const float loopL = bL[pos];
        const float loopR = bR[pos];
        const float srcL  = inL[i];   // save before possible outL overwrite
        const float srcR  = inR[i];

        outL[i] = srcL + loopL;
        outR[i] = srcR + loopR;

        if (isOvr)
        {
            if (mode == OverdubMode::Tape)
            {
                bL[pos] = loopL * kFeedback + srcL;
                bR[pos] = loopR * kFeedback + srcR;
            }
            else  // Replace
            {
                bL[pos] = loopL * kReplaceFade + srcL * (1.f - kReplaceFade);
                bR[pos] = loopR * kReplaceFade + srcR * (1.f - kReplaceFade);
            }
        }

        if (++playPos_ >= loopLenSamples_) playPos_ = 0;
    }

    const float bars = (barSizeSamples_ > 0)
        ? static_cast<float>(loopLenSamples_) / barSizeSamples_ : 0.f;
    displayBars_.store(bars, std::memory_order_relaxed);
}

} // namespace dsp
