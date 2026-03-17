#pragma once

#include "MidiNoteMapper.h"
#include "../dsp/LockFreeQueue.h"

#include <JuceHeader.h>

namespace midi {

// ─────────────────────────────────────────────────────────────────────────────
// MidiManager
//
// Listens to all active MIDI input devices via JUCE and routes note-on/off
// messages to the DSP pipeline through a lock-free queue.
//
// The MIDI callback runs on a system thread; only tryPush() is called there
// (no allocation, no locks).
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

private:
    void handleIncomingMidiMessage(juce::MidiInput* source,
                                   const juce::MidiMessage& message) override;

    ::dsp::LockFreeQueue<::dsp::SamplerEvent, 64>& eventQueue_;
    MidiNoteMapper                              mapper_;
    juce::AudioDeviceManager*                   deviceManager_ { nullptr };
};

} // namespace midi
