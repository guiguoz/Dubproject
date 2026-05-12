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

void MidiManager::setEwiDeviceName(const juce::String& name) noexcept
{
    ewiDeviceName_ = name;
}

bool MidiManager::isEwiDevice(const juce::String& deviceName) const noexcept
{
    return deviceName.containsIgnoreCase(ewiDeviceName_);
}

void MidiManager::consumeEwiMidi(juce::MidiBuffer& dest, int numSamples) noexcept
{
    const int numReady = ewiAbstractFifo_.getNumReady();
    if (numReady <= 0) return;

    int start1, size1, start2, size2;
    ewiAbstractFifo_.prepareToRead(numReady, start1, size1, start2, size2);

    for (int i = 0; i < size1; ++i)
        dest.addEvent(ewiMessages_[static_cast<std::size_t>(start1 + i)], 0);
    for (int i = 0; i < size2; ++i)
        dest.addEvent(ewiMessages_[static_cast<std::size_t>(start2 + i)], 0);

    ewiAbstractFifo_.finishedRead(size1 + size2);
    juce::ignoreUnused(numSamples);
}

void MidiManager::handleIncomingMidiMessage(juce::MidiInput* source,
                                             const juce::MidiMessage& message)
{
    // This runs on the MIDI system thread — NO allocation, NO locks.

    // Cache all CC values globally for MIDI learn (any device)
    if (message.isController())
    {
        const int cc = message.getControllerNumber();
        ccLatest_[static_cast<std::size_t>(cc)].store(
            message.getControllerValue() / 127.f, std::memory_order_relaxed);
        lastCC_.store(cc, std::memory_order_relaxed);
    }

    const juce::String deviceName = (source != nullptr) ? source->getName() : juce::String{};
    const bool fromEwi = isEwiDevice(deviceName);

    if (fromEwi)
    {
        // CC2 breath — also cache as atomic for ducking
        if (message.isController() && message.getControllerNumber() == 2)
            lastBreath_.store(message.getControllerValue() / 127.f,
                              std::memory_order_relaxed);

        // Forward all EWI events to the audio thread via lock-free fifo
        const int numFree = ewiAbstractFifo_.getFreeSpace();
        if (numFree > 0)
        {
            int start1, size1, start2, size2;
            ewiAbstractFifo_.prepareToWrite(1, start1, size1, start2, size2);
            if (size1 > 0)
                ewiMessages_[static_cast<std::size_t>(start1)] = message;
            else if (size2 > 0)
                ewiMessages_[static_cast<std::size_t>(start2)] = message;
            ewiAbstractFifo_.finishedWrite(size1 + size2);
        }
        // EWI notes are NOT routed to the sampler
        return;
    }

    // Non-EWI: route note on/off to sampler slots as before
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
