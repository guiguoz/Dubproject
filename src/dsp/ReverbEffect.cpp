// ReverbEffect.cpp — compiled only as part of SaxFXLive (requires JUCE).
// NOT included in the test target (SaxFXTests) since juce::dsp::Reverb
// needs the full JUCE link.

#include "ReverbEffect.h"
#include <JuceHeader.h>

namespace dsp {

static constexpr ParamDescriptor kReverbParams[4] = {
    { "roomSize", "Room Size", 0.0f, 1.0f, 0.5f  },
    { "damping",  "Damping",   0.0f, 1.0f, 0.5f  },
    { "width",    "Width",     0.0f, 1.0f, 1.0f  },
    { "mix",      "Mix",       0.0f, 1.0f, 0.33f },
};

// ─────────────────────────────────────────────────────────────────────────────
// Pimpl
// ─────────────────────────────────────────────────────────────────────────────
struct ReverbEffect::Impl
{
    juce::dsp::Reverb reverb;

    float roomSize { 0.5f  };
    float damping  { 0.5f  };
    float width    { 1.0f  };
    float mix      { 0.33f };

    void applyParams() noexcept
    {
        juce::dsp::Reverb::Parameters p;
        p.roomSize   = roomSize;
        p.damping    = damping;
        p.width      = width;
        p.wetLevel   = mix;
        p.dryLevel   = 1.0f - mix;
        p.freezeMode = 0.0f;
        reverb.setParameters(p);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ReverbEffect
// ─────────────────────────────────────────────────────────────────────────────
ReverbEffect::ReverbEffect()
    : pimpl_ { std::make_unique<Impl>() }
{}

ReverbEffect::~ReverbEffect() = default;

void ReverbEffect::prepare(double sampleRate, int maxBlockSize) noexcept
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(maxBlockSize);
    spec.numChannels      = 1;
    pimpl_->reverb.prepare(spec);
    pimpl_->applyParams();
}

void ReverbEffect::process(float* buf, int numSamples, float /*pitchHz*/) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    juce::dsp::AudioBlock<float>  block (&buf, 1, static_cast<std::size_t>(numSamples));
    juce::dsp::ProcessContextReplacing<float> ctx (block);
    pimpl_->reverb.process(ctx);
}

void ReverbEffect::reset() noexcept
{
    pimpl_->reverb.reset();
}

ParamDescriptor ReverbEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kReverbParams[i] : ParamDescriptor{};
}

float ReverbEffect::getParam(int i) const noexcept
{
    switch (i)
    {
        case kRoomSize: return pimpl_->roomSize;
        case kDamping:  return pimpl_->damping;
        case kWidth:    return pimpl_->width;
        case kMix:      return pimpl_->mix;
        default:        return 0.0f;
    }
}

void ReverbEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
        case kRoomSize: pimpl_->roomSize = v; break;
        case kDamping:  pimpl_->damping  = v; break;
        case kWidth:    pimpl_->width    = v; break;
        case kMix:      pimpl_->mix      = v; break;
        default:        return;
    }
    pimpl_->applyParams();
}

// ─────────────────────────────────────────────────────────────────────────────
// Presets — 20 presets crafted for electronic / dub techno production
//
// Each preset: { name, { roomSize, damping, width, mix } }
// ─────────────────────────────────────────────────────────────────────────────

struct ReverbPreset
{
    const char* name;
    float params[4]; // roomSize, damping, width, mix
};

static constexpr ReverbPreset kRevPresets[] = {
    //                              room   damp   width  mix
    // ── Dub Techno signatures ────────────────────────────────────────────────
    { "Basic Channel",         {  0.92f, 0.30f, 1.00f, 0.55f }},
    { "Dub Chord Wash",        {  0.95f, 0.20f, 0.90f, 0.65f }},
    { "Deepchord Space",       {  0.88f, 0.35f, 1.00f, 0.50f }},
    { "Echospace Infinite",    {  1.00f, 0.15f, 1.00f, 0.75f }},
    { "Maurizio Haze",         {  0.85f, 0.45f, 0.80f, 0.45f }},
    { "Monolake Fog",          {  0.90f, 0.25f, 0.95f, 0.60f }},
    // ── Techno ───────────────────────────────────────────────────────────────
    { "Dark Warehouse",        {  0.78f, 0.55f, 0.70f, 0.35f }},
    { "Industrial Hall",       {  0.82f, 0.50f, 0.85f, 0.40f }},
    { "Concrete Bunker",       {  0.65f, 0.70f, 0.60f, 0.30f }},
    { "Berghain Hall",         {  0.88f, 0.40f, 0.95f, 0.50f }},
    { "Tresor Tunnel",         {  0.72f, 0.60f, 0.50f, 0.35f }},
    // ── Ambient / Pads ───────────────────────────────────────────────────────
    { "Infinite Shimmer",      {  1.00f, 0.10f, 1.00f, 0.80f }},
    { "Frozen Tail",           {  0.98f, 0.05f, 1.00f, 0.70f }},
    { "Cloud Layer",           {  0.93f, 0.18f, 1.00f, 0.60f }},
    { "Cathedral Drone",       {  0.96f, 0.22f, 0.90f, 0.55f }},
    // ── Tight / Percussion ───────────────────────────────────────────────────
    { "Snare Plate",           {  0.40f, 0.75f, 0.50f, 0.25f }},
    { "Kick Room",             {  0.30f, 0.85f, 0.30f, 0.18f }},
    { "Hi-Hat Sizzle",         {  0.55f, 0.60f, 0.70f, 0.30f }},
    // ── FX / Creative ────────────────────────────────────────────────────────
    { "100% Wet Dub",          {  0.90f, 0.28f, 1.00f, 1.00f }},
    { "Metallic Spring",       {  0.50f, 0.80f, 0.40f, 0.35f }},
};

static constexpr int kRevPresetCount = static_cast<int>(sizeof(kRevPresets) / sizeof(kRevPresets[0]));

int ReverbEffect::presetCount() const noexcept { return kRevPresetCount; }

const char* ReverbEffect::presetName(int index) const noexcept
{
    if (index < 0 || index >= kRevPresetCount) return "";
    return kRevPresets[index].name;
}

void ReverbEffect::applyPreset(int index) noexcept
{
    if (index < 0 || index >= kRevPresetCount) return;
    const auto& p = kRevPresets[index].params;
    for (int i = 0; i < kParamCount; ++i)
        setParam(i, p[i]);
}

} // namespace dsp
