#pragma once

#include "ScaleType.h"
#include <JuceHeader.h>
#include <vector>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// ScaleStaffComponent — draws a treble-clef staff showing the notes of a given
// scale in a given key. Pre-computes note positions in setKey(); paint() only
// reads cached data.
// ─────────────────────────────────────────────────────────────────────────────
class ScaleStaffComponent : public juce::Component
{
public:
    // root: 0=C … 11=B  |  type: see ScaleType.h
    void setKey(int root, ScaleType type);

    void paint(juce::Graphics& g) override;

private:
    int       root_  { 0 };
    ScaleType scale_ { ScaleType::Major };

    // Cached per setKey() — one entry per scale degree (including root at top octave)
    struct NoteInfo {
        float staffPos;  // 0.0 = top staff line, 4.0 = bottom staff line, 0.5 increments
        bool  isSharp;   // true → draw a '#' accidental
    };
    std::vector<NoteInfo> noteInfos_;

    // Semitone intervals above root for each scale type (not including the octave repeat)
    static const std::vector<int>& scaleIntervals(ScaleType t);

    // Maps a MIDI pitch class (0=C…11=B) to its diatonic staff position offset within
    // one octave of treble clef (C4=60 sits on the 3rd ledger line below the staff).
    // Returns {staffOffset, isSharp}
    static std::pair<float, bool> pitchClassToStaffInfo(int pitchClass);

    // Converts a MIDI note number to absolute staff position.
    // Position 0 = top line (F5=77), 4 = bottom line (E4=64).
    // Values outside [0,4] need ledger lines.
    static float midiToStaffPos(int midiNote);

    void rebuildNoteInfos();
};

} // namespace ui
