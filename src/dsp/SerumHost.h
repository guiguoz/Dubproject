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
class SerumHost : private juce::AudioProcessorListener
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

    // Current preset name. Uses AudioProcessorListener + state parsing to detect
    // preset changes even when the plugin doesn't expose names via the standard API.
    // Returns empty string if unavailable (caller should show a fallback).
    juce::String getCurrentPresetName() const;

    // Signal that the cached preset name should be refreshed on next call.
    // Called automatically via AudioProcessorListener; exposed for testing.
    void markPresetNameDirty() { presetNameDirty_ = true; }

    void setBpm (float bpm) noexcept
    {
        bpmPlayHead_.bpm.store (static_cast<double> (bpm), std::memory_order_relaxed);
    }

    void setIsPlaying (bool playing) noexcept
    {
        bpmPlayHead_.isPlaying.store (playing, std::memory_order_relaxed);
    }

    // Remet la position PPQ à zéro — resync LFOs/arp Serum au prochain bloc
    void resetPlaybackPosition() noexcept
    {
        resetPositionFlag_.store (true, std::memory_order_relaxed);
    }

    // Injecte CC 123 (All Notes Off) sur les 16 canaux au prochain processBlock.
    // Thread-safe : peut être appelé depuis le message thread.
    void sendAllNotesOff() noexcept
    {
        allNotesOffPending_.store (true, std::memory_order_relaxed);
    }

    // Envoie un RPN Pitch Bend Sensitivity (semitones) au prochain processBlock.
    // Appelé automatiquement avec 12 ST après load(). Override si l'EWI est configuré autrement.
    void setPitchBendRange (int semitones) noexcept
    {
        pendingPitchBendRange_.store (semitones, std::memory_order_relaxed);
    }

    // Latence introduite par le plugin (oversampling etc.). 0 si non chargé.
    int getLatencySamples() const noexcept
    {
        return (plugin_ != nullptr) ? plugin_->getLatencySamples() : 0;
    }

private:
    // ── BPM PlayHead — audio thread reads, GUI thread writes ─────────────────
    struct BpmPlayHead : juce::AudioPlayHead
    {
        std::atomic<double> bpm        { 120.0 };
        std::atomic<double> sampleRate { 48000.0 };
        std::atomic<bool>   isPlaying  { true };
        int64_t             samplePos  { 0 };   // audio thread only

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo pos;
            const double bpmVal = bpm.load (std::memory_order_relaxed);
            const double sr     = sampleRate.load (std::memory_order_relaxed);

            pos.setBpm (bpmVal);
            pos.setIsPlaying (isPlaying.load (std::memory_order_relaxed));
            pos.setIsLooping (false);
            pos.setTimeSignature (TimeSignature { 4, 4 });

            const double ppq = samplePos / sr * bpmVal / 60.0;
            pos.setPpqPosition (ppq);
            pos.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
            pos.setTimeInSamples (samplePos);

            return pos;
        }
    } bpmPlayHead_;

    std::atomic<bool> resetPositionFlag_     { false };
    std::atomic<bool> allNotesOffPending_    { false };
    std::atomic<int>  pendingPitchBendRange_ { -1 };

    juce::AudioPluginFormatManager             formatManager_;
    std::unique_ptr<juce::AudioPluginInstance> plugin_;
    juce::AudioBuffer<float>                   serumBuffer_;

    double sampleRate_ { 48000.0 };
    int    blockSize_  { 512 };

    std::atomic<bool>  loaded_            { false };
    std::atomic<bool>  processingEnabled_ { false };
    std::atomic<float> outputGain_        { 0.5f };

    // AudioProcessorListener — message thread only
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override
    {
        presetNameDirty_ = true;
    }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override
    {
        presetNameDirty_ = true;
    }

    mutable juce::String cachedPresetName_;
    mutable bool         presetNameDirty_ { true };
};

} // namespace dsp
