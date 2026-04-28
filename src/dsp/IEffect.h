#pragma once

#include <atomic>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// EffectType — identifies each concrete effect type.
// ─────────────────────────────────────────────────────────────────────────────
enum class EffectType
{
    Flanger,
    Harmonizer,
    Reverb,
    PitchFork,
    EnvelopeFilter,
    Delay,
    Whammy,
    Octaver,
    Tuner,
    Slicer,
    AutoPitchCorrect,
    Synth
};

// ─────────────────────────────────────────────────────────────────────────────
// ParamDescriptor — static metadata for a single parameter.
// ─────────────────────────────────────────────────────────────────────────────
struct ParamDescriptor
{
    const char* id         { "" };
    const char* label      { "" };
    float       min        { 0.0f };
    float       max        { 1.0f };
    float       defaultVal { 0.0f };
};

// ─────────────────────────────────────────────────────────────────────────────
// IEffect — abstract base for all DSP effects in the chain.
//
// Thread safety contract:
//   - prepare() / reset()    : called from the audio thread only
//   - process()              : called from the audio thread only
//   - paramDescriptor()      : called from any thread (returns const data)
//   - getParam() / setParam(): called from the GUI thread only
//   - enabled                : written from GUI thread, read on audio thread
//                              (atomic<bool> with acquire/release semantics)
//
// Pure C++ — no JUCE dependency in this header.
// ─────────────────────────────────────────────────────────────────────────────
class IEffect
{
public:
    virtual ~IEffect() = default;

    /// Effect type discriminator.
    virtual EffectType type() const noexcept = 0;

    /// Prepare buffers/state before streaming starts.
    virtual void prepare(double sampleRate, int maxBlockSize) noexcept = 0;

    /// Process one mono block in-place.  pitchHz = current detected pitch (0 = unknown).
    virtual void process(float* buf, int numSamples, float pitchHz) noexcept = 0;

    /// Process one stereo block in-place.
    /// Default: processes L and R independently — preserves any stereo divergence
    /// produced by upstream effects.  Override for true stereo behaviour (e.g. Reverb,
    /// ping-pong Delay).  Note: effects with cross-channel shared state (LFO, look-ahead
    /// compressor) must provide their own override.
    virtual void processStereo(float* left, float* right,
                                int numSamples, float pitchHz) noexcept
    {
        process(left,  numSamples, pitchHz);
        process(right, numSamples, pitchHz);
    }

    /// Reset DSP state (e.g. after a pause).
    virtual void reset() noexcept = 0;

    /// Number of tweakable parameters.
    virtual int paramCount() const noexcept = 0;

    /// Static metadata for parameter i (min/max/default/label).
    virtual ParamDescriptor paramDescriptor(int i) const noexcept = 0;

    /// Current value of parameter i.  GUI thread only.
    virtual float getParam(int i) const noexcept = 0;

    /// Set parameter i to v.  GUI thread only.
    virtual void setParam(int i, float v) noexcept = 0;

    /// Preset support (optional — default: no presets).
    virtual int         presetCount()                const noexcept { return 0; }
    virtual const char* presetName(int /*index*/)    const noexcept { return ""; }
    virtual void        applyPreset(int /*index*/)         noexcept {}

    /// Preferred YIN confidence threshold for this effect's active preset.
    /// DspPipeline reads this each block to override kConfidenceGate.
    /// Return -1 to use the pipeline default.
    virtual float confidenceHint() const noexcept { return -1.f; }

    // Enable / disable without removing from the chain.
    std::atomic<bool> enabled { true };

    // Non-copyable — effects own their DSP state.
    IEffect()                            = default;
    IEffect(const IEffect&)              = delete;
    IEffect& operator=(const IEffect&)   = delete;
    IEffect(IEffect&&)                   = delete;
    IEffect& operator=(IEffect&&)        = delete;
};

} // namespace dsp
