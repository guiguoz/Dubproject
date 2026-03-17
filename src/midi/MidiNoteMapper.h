#pragma once

#include <array>

namespace midi {

// ─────────────────────────────────────────────────────────────────────────────
// MidiNoteMapper
//
// Pure C++ (no JUCE dependency) mapping from MIDI note number to sampler
// slot index.  Extracted from MidiManager so it can be unit-tested without
// linking JUCE.
// ─────────────────────────────────────────────────────────────────────────────
class MidiNoteMapper
{
public:
    static constexpr int kNumNotes   = 128;
    static constexpr int kUnmapped   = -1;

    MidiNoteMapper() noexcept { noteToSlot_.fill(kUnmapped); }

    // Map MIDI note -> slot (0-7).  Out-of-range inputs are ignored.
    void setMapping(int midiNote, int slotIndex) noexcept
    {
        if (midiNote < 0 || midiNote >= kNumNotes) return;
        noteToSlot_[static_cast<std::size_t>(midiNote)] = slotIndex;
    }

    // Returns kUnmapped (-1) if the note has no mapping.
    int getSlot(int midiNote) const noexcept
    {
        if (midiNote < 0 || midiNote >= kNumNotes) return kUnmapped;
        return noteToSlot_[static_cast<std::size_t>(midiNote)];
    }

    void clearMappings() noexcept { noteToSlot_.fill(kUnmapped); }

private:
    std::array<int, kNumNotes> noteToSlot_;
};

} // namespace midi
