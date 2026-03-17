#include "MidiManager.h"

namespace midi {

MidiManager::MidiManager(::dsp::LockFreeQueue<::dsp::SamplerEvent, 64>& eventQueue) noexcept
    : eventQueue_(eventQueue)
{
}

MidiManager::~MidiManager()
{
    stop();
}

void MidiManager::start(juce::AudioDeviceManager& deviceManager)
{
    deviceManager_ = &deviceManager;

    const auto midiDevices = juce::MidiInput::getAvailableDevices();
    for (const auto& device : midiDevices)
    {
        if (deviceManager_->isMidiInputDeviceEnabled(device.identifier))
            deviceManager_->addMidiInputDeviceCallback(device.identifier, this);
    }
}

void MidiManager::stop()
{
    if (deviceManager_ == nullptr) return;

    const auto midiDevices = juce::MidiInput::getAvailableDevices();
    for (const auto& device : midiDevices)
        deviceManager_->removeMidiInputDeviceCallback(device.identifier, this);

    deviceManager_ = nullptr;
}

void MidiManager::setNoteMapping(int midiNote, int slotIndex) noexcept
{
    mapper_.setMapping(midiNote, slotIndex);
}

int MidiManager::getSlotForNote(int midiNote) const noexcept
{
    return mapper_.getSlot(midiNote);
}

void MidiManager::clearMappings() noexcept
{
    mapper_.clearMappings();
}

void MidiManager::handleIncomingMidiMessage(juce::MidiInput* /*source*/,
                                             const juce::MidiMessage& message)
{
    // This runs on the MIDI system thread — NO allocation, NO locks.
    if (message.isNoteOn())
    {
        const int slot = mapper_.getSlot(message.getNoteNumber());
        if (slot >= 0)
            eventQueue_.tryPush(::dsp::SamplerEvent{ slot, true });
    }
    else if (message.isNoteOff())
    {
        const int slot = mapper_.getSlot(message.getNoteNumber());
        if (slot >= 0)
            eventQueue_.tryPush(::dsp::SamplerEvent{ slot, false });
    }
}

} // namespace midi
