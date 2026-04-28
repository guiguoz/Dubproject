#include "DelayEffect.h"
#include "DspCommon.h"

#include <algorithm>
#include <cmath>

namespace dsp
{

static constexpr ParamDescriptor kParams[4] = {
    {"time",     "Time (ms)",  1.0f,  2000.0f, 500.0f},
    {"feedback", "Feedback",   0.0f,  0.95f,   0.3f},
    {"mix",      "Mix",        0.0f,  1.0f,    0.5f},
    {"division", "Division",   0.0f,  7.0f,    3.0f},
};

// Division index → multiplier of one beat (at current BPM)
//   0=1/32  1=1/16  2=1/8  3=1/4  4=1/2  5=1bar  6=3/16(dotted 8th)  7=3/8(dotted 1/4)
static constexpr float kDivMultipliers[8] = {
    0.125f,   // 1/32
    0.25f,    // 1/16
    0.5f,     // 1/8
    1.0f,     // 1/4
    2.0f,     // 1/2
    4.0f,     // 1 bar
    0.75f,    // dotted 1/8  (3/16)
    1.5f,     // dotted 1/4  (3/8)
};

float DelayEffect::divisionToMs(int div, float bpm) noexcept
{
    if (bpm <= 0.0f) return 500.0f;
    const int idx = std::max(0, std::min(div, 7));
    // One beat = 60000 / BPM  ms
    return (60000.0f / bpm) * kDivMultipliers[idx];
}

// ─────────────────────────────────────────────────────────────────────────────

void DelayEffect::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    delayBuffer_  .setSize(kMaxDelayBuffer);
    delayBufferR_->setSize(kMaxDelayBuffer);
}

void DelayEffect::process(float* buf, int numSamples, float /*pitchHz*/) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    const float fb = std::min(feedback_.load(std::memory_order_relaxed), 0.95f);
    const float mx = mix_.load(std::memory_order_relaxed);

    // Compute effective delay time (sync or manual)
    float timeMs;
    const float bpm = syncBpm_.load(std::memory_order_relaxed);
    if (bpm > 0.0f)
    {
        const int div = static_cast<int>(division_.load(std::memory_order_relaxed) + 0.5f);
        timeMs = divisionToMs(div, bpm);
    }
    else
    {
        timeMs = time_.load(std::memory_order_relaxed);
    }

    // Clamp to buffer size
    const float maxMs = static_cast<float>(kMaxDelayBuffer) / static_cast<float>(sampleRate_) * 1000.0f;
    timeMs = std::max(1.0f, std::min(timeMs, maxMs));

    const float delayInSamples = (timeMs / 1000.0f) * static_cast<float>(sampleRate_);

    for (int i = 0; i < numSamples; ++i)
    {
        const float inSamp = buf[i];
        const float delayedSamp = delayBuffer_.read(delayInSamples);

        delayBuffer_.push(inSamp + fb * delayedSamp);

        buf[i] = inSamp * (1.0f - mx) + delayedSamp * mx;
    }
}

void DelayEffect::reset() noexcept
{
    delayBuffer_ .reset();
    delayBufferR_->reset();
}

void DelayEffect::processStereo(float* left, float* right,
                                 int numSamples, float /*pitchHz*/) noexcept
{
    if (!enabled.load(std::memory_order_acquire)) return;

    const float fb = std::min(feedback_.load(std::memory_order_relaxed), 0.95f);
    const float mx = mix_.load(std::memory_order_relaxed);

    float timeMs;
    const float bpm = syncBpm_.load(std::memory_order_relaxed);
    if (bpm > 0.0f)
    {
        const int div = static_cast<int>(division_.load(std::memory_order_relaxed) + 0.5f);
        timeMs = divisionToMs(div, bpm);
    }
    else
    {
        timeMs = time_.load(std::memory_order_relaxed);
    }
    const float maxMs = static_cast<float>(kMaxDelayBuffer)
                        / static_cast<float>(sampleRate_) * 1000.0f;
    timeMs = std::max(1.0f, std::min(timeMs, maxMs));
    const float ds = (timeMs / 1000.0f) * static_cast<float>(sampleRate_);

    for (int i = 0; i < numSamples; ++i)
    {
        // Ping-pong: L buffer fed from R echo, R buffer fed from L echo → L→R→L→R
        const float echoL = delayBuffer_  .read(ds);
        const float echoR = delayBufferR_->read(ds);

        delayBuffer_  .push(left [i] + echoR * fb);
        delayBufferR_->push(right[i] + echoL * fb);

        left [i] = left [i] * (1.0f - mx) + echoL * mx;
        right[i] = right[i] * (1.0f - mx) + echoR * mx;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameters
// ─────────────────────────────────────────────────────────────────────────────

ParamDescriptor DelayEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kParams[i] : ParamDescriptor{};
}

float DelayEffect::getParam(int i) const noexcept
{
    switch (i)
    {
    case kTime:     return time_.load(std::memory_order_relaxed);
    case kFeedback: return feedback_.load(std::memory_order_relaxed);
    case kMix:      return mix_.load(std::memory_order_relaxed);
    case kDivision: return division_.load(std::memory_order_relaxed);
    default:        return 0.0f;
    }
}

void DelayEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
    case kTime:     time_.store(v, std::memory_order_relaxed);     break;
    case kFeedback: feedback_.store(v, std::memory_order_relaxed); break;
    case kMix:      mix_.store(v, std::memory_order_relaxed);      break;
    case kDivision: division_.store(v, std::memory_order_relaxed); break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Presets — 20 presets: dub techno, electro, techno, creative
//
// Each preset: { name, { time_ms, feedback, mix, division } }
//
// Note: when tempo sync is active, the division param overrides time.
//       Presets set sensible time values for non-synced use too.
// ─────────────────────────────────────────────────────────────────────────────

struct DelayPreset
{
    const char* name;
    float params[4]; // time, fb, mix, div
};

static constexpr DelayPreset kDelayPresets[] = {
    //                              time    fb     mix    div
    // ── Dub Techno ───────────────────────────────────────────────────────────
    { "Basic Channel Echo",    {  500.0f, 0.65f, 0.50f, 3.0f }},  // 1/4
    { "Deepchord Tape",        {  375.0f, 0.55f, 0.45f, 6.0f }},  // dotted 1/8
    { "Maurizio Dub",          {  750.0f, 0.70f, 0.55f, 7.0f }},  // dotted 1/4
    { "Monolake Ping-Pong",    {  250.0f, 0.60f, 0.50f, 2.0f }},  // 1/8
    { "Infinite Dub Trail",    {  500.0f, 0.88f, 0.40f, 3.0f }},  // 1/4 long fb
    { "Dub Swell",             { 1000.0f, 0.75f, 0.35f, 4.0f }},  // 1/2
    // ── Techno ───────────────────────────────────────────────────────────────
    { "Techno 1/16 Stutter",   {  125.0f, 0.40f, 0.55f, 1.0f }},  // 1/16
    { "Berghain Slap",         {   62.0f, 0.25f, 0.45f, 0.0f }},  // 1/32
    { "Warehouse Quarter",     {  500.0f, 0.50f, 0.50f, 3.0f }},  // 1/4
    { "Industrial Repeat",     {  250.0f, 0.70f, 0.60f, 2.0f }},  // 1/8
    { "Minimal Dotted",        {  375.0f, 0.45f, 0.40f, 6.0f }},  // dotted 1/8
    // ── Electro / House ──────────────────────────────────────────────────────
    { "House Groove",           {  375.0f, 0.35f, 0.40f, 6.0f }},  // dotted 1/8
    { "Acid Delay",            {  166.0f, 0.60f, 0.55f, 2.0f }},  // 1/8 (tight)
    { "Disco Slapback",        {   80.0f, 0.15f, 0.35f, 0.0f }},  // 1/32
    // ── Ambient / Creative ───────────────────────────────────────────────────
    { "Ambient Wash",          { 1000.0f, 0.80f, 0.50f, 4.0f }},  // 1/2
    { "Frozen Repeat",         {  500.0f, 0.92f, 0.35f, 3.0f }},  // 1/4 near-infinite
    { "Tape Degradation",      {  666.0f, 0.75f, 0.60f, 7.0f }},  // dotted 1/4
    { "Granular Micro",        {   30.0f, 0.50f, 0.70f, 0.0f }},  // very short
    { "Half-Speed Echo",       { 2000.0f, 0.60f, 0.40f, 5.0f }},  // 1 bar
    { "Lo-Fi Spring",          {  200.0f, 0.55f, 0.45f, 2.0f }},  // 1/8
};

static constexpr int kDelayPresetCount = static_cast<int>(sizeof(kDelayPresets) / sizeof(kDelayPresets[0]));

int DelayEffect::presetCount() const noexcept { return kDelayPresetCount; }

const char* DelayEffect::presetName(int index) const noexcept
{
    if (index < 0 || index >= kDelayPresetCount) return "";
    return kDelayPresets[index].name;
}

void DelayEffect::applyPreset(int index) noexcept
{
    if (index < 0 || index >= kDelayPresetCount) return;
    const auto& p = kDelayPresets[index].params;
    for (int i = 0; i < kParamCount; ++i)
        setParam(i, p[i]);
}

} // namespace dsp
