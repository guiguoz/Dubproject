#pragma once

#include "MidiNoteMapper.h"
#include "../dsp/LockFreeQueue.h"

#include <JuceHeader.h>
#include <array>
#include <atomic>

namespace midi {

// ─────────────────────────────────────────────────────────────────────────────
// MidiManager
//
// Listens to all active MIDI input devices via JUCE.
// Routes note-on/off to the sampler via a lock-free queue.
// Routes EWI events (all messages from the designated EWI device) via
// AbstractFifo — lock-free between the MIDI system thread and the audio thread.
//
// EWI-specific signals captured:
//   CC 2  (breath)     → lastBreath_ atomic [0..1]
//   CC 74 (brightness) → forwarded in EWI MidiBuffer
//   PitchBend          → forwarded in EWI MidiBuffer
//   NoteOn/Off         → forwarded in EWI MidiBuffer (not routed to sampler)
// ─────────────────────────────────────────────────────────────────────────────
class MidiManager : private juce::MidiInputCallback
{
public:
    explicit MidiManager(::dsp::LockFreeQueue<::dsp::SamplerEvent, 64>& eventQueue) noexcept;
    ~MidiManager() override;

    // Attach to the shared AudioDeviceManager and start receiving MIDI.
    void start(juce::AudioDeviceManager& deviceManager);
    void stop();

    // Delegate to the internal MidiNoteMapper.
    void setNoteMapping(int midiNote, int slotIndex) noexcept;
    int  getSlotForNote(int midiNote) const noexcept;
    void clearMappings() noexcept;

    // ── EWI device identification ────────────────────────────────────────────
    // Set the name of the EWI MIDI device (substring match, case-insensitive).
    // e.g. "EWI" matches "AKAI EWI USB".
    void setEwiDeviceName(const juce::String& name) noexcept;

    // ── EWI MIDI drain (audio thread) ────────────────────────────────────────
    // Drain buffered EWI MIDI events into dest. Lock-free.
    void consumeEwiMidi(juce::MidiBuffer& dest, int numSamples) noexcept;

    // Latest CC2 breath value, updated by MIDI system thread, read by audio thread.
    float lastBreath() const noexcept
    {
        return lastBreath_.load(std::memory_order_relaxed);
    }

    // ── MIDI Learn support ───────────────────────────────────────────────────
    // Latest value [0..1] for any received CC (written MIDI thread, read GUI timer).
    float getCcValue(int cc) const noexcept
    {
        if (cc < 0 || cc >= 128) return 0.f;
        return ccLatest_[static_cast<std::size_t>(cc)].load(std::memory_order_relaxed);
    }

    // Returns the last received CC number and resets it to -1 (one-shot, acq_rel).
    // Returns -1 if no CC was received since last call.
    int getLastCC() const noexcept
    {
        return lastCC_.exchange(-1, std::memory_order_acq_rel);
    }

private:
    void handleIncomingMidiMessage(juce::MidiInput* source,
                                   const juce::MidiMessage& message) override;

    bool isEwiDevice(const juce::String& deviceName) const noexcept;

    ::dsp::LockFreeQueue<::dsp::SamplerEvent, 64>& eventQueue_;
    MidiNoteMapper                                  mapper_;
    juce::AudioDeviceManager*                       deviceManager_ { nullptr };

    // EWI device name filter (set from GUI thread, read from MIDI thread)
    juce::String ewiDeviceName_ { "EWI" };

    // Lock-free EWI MIDI queue (MIDI system thread → audio thread)
    static constexpr int kEwiQueueSize = 128;
    juce::AbstractFifo                            ewiAbstractFifo_ { kEwiQueueSize };
    std::array<juce::MidiMessage, kEwiQueueSize>  ewiMessages_;

    // CC2 breath [0..1] — written MIDI thread, read audio thread
    std::atomic<float> lastBreath_ { 0.f };

    // MIDI Learn: CC latency cache (written MIDI thread, read GUI timer)
    std::array<std::atomic<float>, 128> ccLatest_ {};
    mutable std::atomic<int>            lastCC_   { -1 };
};

} // namespace midi
