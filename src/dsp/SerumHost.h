#pragma once

#include <JuceHeader.h>
#include <atomic>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// SerumHost — VST3 plugin host for Serum V2 (or any VST3 synth)
//
// Threading model:
//   load() / unload() / createEditor() / getState() / setState()
//     → message thread ONLY (never call during audio processing)
//   processBlock() → audio thread ONLY
//   isLoaded() / getOutputGain() / setOutputGain() → any thread (atomic)
//
// To swap plugin at runtime:
//   1. setProcessingEnabled(false)   (audio thread stops calling processBlock)
//   2. unload()                      (message thread)
//   3. load(newPath, ...)            (message thread)
//   4. setProcessingEnabled(true)
// ─────────────────────────────────────────────────────────────────────────────
class SerumHost
{
public:
    SerumHost();
    ~SerumHost();

    // ── Lifecycle (message thread only) ──────────────────────────────────────

    // Load the VST3 at vst3Path, prepare it for playback.
    // Returns true on success. outError receives a human-readable diagnostic on failure.
    bool load(const juce::String& vst3Path, double sampleRate, int blockSize,
              juce::String* outError = nullptr);

    // Release the plugin. Call only when processingEnabled_ is false.
    void unload();

    // ── Audio thread ─────────────────────────────────────────────────────────

    // Process one block. midi should contain EWI events for this block.
    // Output written to internal serumBuffer_ (stereo).
    // No-op if !isLoaded() || !processingEnabled_.
    void processBlock(juce::MidiBuffer& midi) noexcept;

    // Access the output buffer (audio thread only, valid after processBlock).
    juce::AudioBuffer<float>& getOutputBuffer() noexcept { return serumBuffer_; }

    // ── State (any thread) ───────────────────────────────────────────────────

    bool isLoaded() const noexcept { return loaded_.load(std::memory_order_acquire); }

    void setProcessingEnabled(bool e) noexcept
    {
        processingEnabled_.store(e, std::memory_order_release);
    }

    void setOutputGain(float g) noexcept { outputGain_.store(g, std::memory_order_relaxed); }
    float getOutputGain() const noexcept { return outputGain_.load(std::memory_order_relaxed); }

    // ── Preset (message thread only) ─────────────────────────────────────────

    void getState(juce::MemoryBlock& dest) const;
    void setState(const void* data, int size);

    // ── Editor (message thread only) ─────────────────────────────────────────

    // Creates and returns the native plugin editor. Caller owns the component.
    // Returns nullptr if not loaded or plugin has no editor.
    juce::AudioProcessorEditor* createEditor();

    // Plugin name for display (empty if not loaded).
    juce::String getPluginName() const;

private:
    juce::AudioPluginFormatManager             formatManager_;
    std::unique_ptr<juce::AudioPluginInstance> plugin_;
    juce::AudioBuffer<float>                   serumBuffer_;

    double sampleRate_ { 48000.0 };
    int    blockSize_  { 512 };

    std::atomic<bool>  loaded_            { false };
    std::atomic<bool>  processingEnabled_ { false };
    std::atomic<float> outputGain_        { 0.5f };
};

} // namespace dsp
