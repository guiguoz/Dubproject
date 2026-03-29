#include "SlicerEffect.h"

namespace dsp
{

static constexpr ParamDescriptor kParams[2] = {{"rate", "Rate (Hz)", 0.1f, 20.0f, 4.0f},
                                               {"depth", "Depth", 0.0f, 1.0f, 1.0f}};

void SlicerEffect::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    phase_ = 0.0f;
}

void SlicerEffect::process(float* buf, int numSamples, float /*pitchHz*/) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    const float r = rate_.load(std::memory_order_relaxed);
    const float d = depth_.load(std::memory_order_relaxed);
    const float phaseInc = r / static_cast<float>(sampleRate_);

    for (int i = 0; i < numSamples; ++i)
    {
        // Simple square wave LFO
        // phase < 0.5 means ON, >= 0.5 means OFF (attenuated by depth)
        const float lfoVal = (phase_ < 0.5f) ? 1.0f : (1.0f - d);

        buf[i] *= lfoVal;

        phase_ += phaseInc;
        if (phase_ >= 1.0f)
        {
            phase_ -= 1.0f;
        }
    }
}

void SlicerEffect::reset() noexcept
{
    phase_ = 0.0f;
}

ParamDescriptor SlicerEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kParams[i] : ParamDescriptor{};
}

float SlicerEffect::getParam(int i) const noexcept
{
    switch (i)
    {
    case kRate:
        return rate_.load(std::memory_order_relaxed);
    case kDepth:
        return depth_.load(std::memory_order_relaxed);
    default:
        return 0.0f;
    }
}

void SlicerEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
    case kRate:
        rate_.store(v, std::memory_order_relaxed);
        break;
    case kDepth:
        depth_.store(v, std::memory_order_relaxed);
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Presets — 15 presets for live saxophone + electronic music
//
// Each preset is { rateHz, depth }.
// At 120 BPM: quarter=2Hz, 8th=4Hz, 16th=8Hz, 32nd=16Hz,
//              dotted 8th≈2.67Hz, triplet 8th≈5.33Hz.
// ─────────────────────────────────────────────────────────────────────────────

struct SlicerPreset
{
    const char* name;
    float params[2]; // rateHz, depth
};

static constexpr SlicerPreset kPresets[] = {
    //                           rate    depth
    // ── Standard ────────────────────────────────────────────────────────────
    { "16th Gate",             {  8.0f, 1.00f } },
    { "8th Groove",            {  4.0f, 0.90f } },
    { "Half-time",             {  2.0f, 0.85f } },
    // ── Dub Techno ──────────────────────────────────────────────────────────
    { "Dub Pulse",             {  2.0f, 0.90f } },  // quarter-note dub beat
    { "Dub Offbeat",           {  2.67f, 0.95f } },  // dotted 8th dub techno
    { "Dub Quarter Light",     {  2.0f, 0.60f } },  // light quarter dub
    // ── Rhythmic ────────────────────────────────────────────────────────────
    { "Triplet Flow",          {  5.33f, 0.85f } },  // 8th triplet
    { "Syncopé",               {  5.0f, 1.00f } },  // syncopated 8th
    { "32nd Shimmer",          { 16.0f, 0.55f } },  // 32nd shimmer texture
    // ── Saxophone ───────────────────────────────────────────────────────────
    { "Sax Pulse",             {  1.5f, 0.70f } },  // gentle breathing
    { "Sax Pulse Deep",        {  1.5f, 0.95f } },  // deep sax pulse
    { "Bass Sax Sub",          {  0.5f, 0.80f } },  // slow sub bass gating
    // ── Ambient / Creative ──────────────────────────────────────────────────
    { "Subtle Pump",           {  0.5f, 0.40f } },  // sidechain-like breathing
    { "Deep Gate",             {  1.0f, 1.00f } },  // Basic Channel style
    { "Silence Gate",          {  1.0f, 0.85f } },  // dramatic silence
};

static constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

int SlicerEffect::presetCount() const noexcept
{
    return kPresetCount;
}

const char* SlicerEffect::presetName(int index) const noexcept
{
    if (index < 0 || index >= kPresetCount) return "";
    return kPresets[index].name;
}

void SlicerEffect::applyPreset(int index) noexcept
{
    if (index < 0 || index >= kPresetCount) return;
    const auto& p = kPresets[index].params;
    for (int i = 0; i < kParamCount; ++i)
        setParam(i, p[i]);
}

} // namespace dsp
