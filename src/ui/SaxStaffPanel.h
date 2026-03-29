#pragma once

#include "Colours.h"
#include "PianoKeyboardPanel.h"  // for ScaleType enum

#include <JuceHeader.h>
#include <array>
#include <cmath>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// SaxStaffPanel
//
// Displays a treble-clef musical staff showing the notes playable on tenor
// saxophone for the current master key.
//
// Tenor sax transposition: written pitch = concert pitch + 2 semitones (1 tone).
// If master key = C major, the sax reads / the panel displays D major.
//
// Scale notes are shown as filled note-heads on the staff over 1 octave.
// ─────────────────────────────────────────────────────────────────────────────

class SaxStaffPanel : public juce::Component
{
public:
    SaxStaffPanel()
    {
        // Scale selector (mirrors PianoKeyboardPanel)
        scaleCombo_.addItem("Major",            1);
        scaleCombo_.addItem("Minor",            2);
        scaleCombo_.addItem("Pentatonique Maj", 3);
        scaleCombo_.addItem("Pentatonique Min", 4);
        scaleCombo_.addItem("Blues",            5);
        scaleCombo_.addItem("Dorian",           6);
        scaleCombo_.setSelectedId(1, juce::dontSendNotification);
        scaleCombo_.onChange = [this] {
            scaleType_ = static_cast<PianoKeyboardPanel::ScaleType>(
                             scaleCombo_.getSelectedId() - 1);
            repaint();
        };
        addAndMakeVisible(scaleCombo_);
    }

    // ── Public API ────────────────────────────────────────────────────────────

    void setMasterKey(int keyRoot, bool isMajor) noexcept
    {
        masterKey_   = keyRoot;
        masterMajor_ = isMajor;
        repaint();
    }

    void setScaleType(PianoKeyboardPanel::ScaleType t) noexcept
    {
        scaleType_ = t;
        scaleCombo_.setSelectedId(static_cast<int>(t) + 1, juce::dontSendNotification);
        repaint();
    }

    // ── Layout ────────────────────────────────────────────────────────────────

    void resized() override
    {
        scaleCombo_.setBounds(8, 4, 180, 24);
    }

    void paint(juce::Graphics& g) override
    {
        // Background
        g.setColour(SaxFXColours::cardBody);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.f);
        g.setColour(juce::Colour(0xFF4CDFA8).withAlpha(0.12f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.f, 1.f);

        const int W = getWidth();
        const int H = getHeight();

        // Sax key = master key + 2 semitones (tenor Bb transposition)
        const int saxKey = (masterKey_ + 2) % 12;

        // Info banner
        g.setColour(SaxFXColours::textSecondary);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(11.f)));
        const juce::String info = "Sax Tenor Si\xe2\x99\xad  \xe2\x80\x94  "
                                  "Concert : " + noteNameStr(masterKey_) +
                                  (masterMajor_ ? " Maj" : " min") +
                                  "  \xe2\x86\x92  Lecture sax : " +
                                  noteNameStr(saxKey) +
                                  (masterMajor_ ? " Maj" : " min");
        g.drawText(info, 8, 32, W - 16, 16, juce::Justification::centredLeft, true);

        // Get scale notes (in sax-transposed key)
        auto scaleNotes = buildScaleNotes(saxKey);

        drawStaff(g, saxKey, scaleNotes, W, H);
    }

private:
    // ── Scale tables ─────────────────────────────────────────────────────────

    static constexpr int kMajorScale[7]        = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int kMinorScale[7]        = { 0, 2, 3, 5, 7, 8, 10 };
    static constexpr int kPentatonicMajor[5]   = { 0, 2, 4, 7, 9 };
    static constexpr int kPentatonicMinor[5]   = { 0, 3, 5, 7, 10 };
    static constexpr int kBlues[6]             = { 0, 3, 5, 6, 7, 10 };
    static constexpr int kDorian[7]            = { 0, 2, 3, 5, 7, 9, 10 };

    // Treble clef staff: diatonic steps from bottom line (E4=0)
    // C=−1 (ledger below), D=0.5, E=1, F=1.5, G=2, A=2.5, B=3, C=3.5, D=4, E=4.5, F=5
    // Mapping: MIDI semitone within octave → diatonic step offset from E4
    // We'll use absolute MIDI note numbers for precise placement.

    // Semitone → diatonic step from E4 (E4 = MIDI 64 = step 0 on bottom line)
    // Steps: 0=E4, 0.5=F4, 1=G4, 1.5=A4, 2=B4, 2.5=C5, 3=D5, 3.5=E5, 4=F5, 4.5=G5, 5=A5...
    static float midiToDiatonicStep(int midi) noexcept
    {
        // C4=60, D4=62, E4=64, F4=65, G4=67, A4=69, B4=71, C5=72...
        // Diatonic step offset from E4 (step 0), each white key = 0.5 steps
        static constexpr int kDiatonicFromC[12] = {
        //  C   C#  D   D#  E   F   F#  G   G#  A   A#  B
            -3, -3, -2, -2, -1,  0,  0,  1,  1,  2,  2,  3
        };  // offset in half-steps (diatonic) from E4 when in same octave

        // Base: E4 = MIDI 64
        const int diffFromE4 = midi - 64;
        const int octave     = (int)std::floor(diffFromE4 / 12.f);
        const int semInOct   = ((midi % 12) + 12) % 12;  // 0=C
        // Diatonic position of E = 0 (in C major: C=0,D=1,E=2 → relative to E: E=0)
        // semitone class within octave, diatonic half-steps above C of that octave
        const float diatonicAboveC = kDiatonicFromC[semInOct] + 2.f; // E = index 2 above C
        return static_cast<float>(octave) * 7.f + diatonicAboveC;
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    std::array<bool, 12> buildScaleNotes(int root) const
    {
        std::array<bool, 12> out {};
        const int* ptr = nullptr;
        int len = 0;
        switch (scaleType_)
        {
            case PianoKeyboardPanel::ScaleType::Major:
                ptr = kMajorScale; len = 7; break;
            case PianoKeyboardPanel::ScaleType::Minor:
                ptr = kMinorScale; len = 7; break;
            case PianoKeyboardPanel::ScaleType::PentatonicMaj:
                ptr = kPentatonicMajor; len = 5; break;
            case PianoKeyboardPanel::ScaleType::PentatonicMin:
                ptr = kPentatonicMinor; len = 5; break;
            case PianoKeyboardPanel::ScaleType::Blues:
                ptr = kBlues; len = 6; break;
            case PianoKeyboardPanel::ScaleType::Dorian:
                ptr = kDorian; len = 7; break;
        }
        for (int i = 0; i < len; ++i)
            out[(root + ptr[i]) % 12] = true;
        return out;
    }

    static juce::String noteNameStr(int pitchClass) noexcept
    {
        static const char* kNames[12] = {
            "C","C\xe2\x99\xaf","D","E\xe2\x99\xad","E","F",
            "F\xe2\x99\xaf","G","A\xe2\x99\xad","A","B\xe2\x99\xad","B"
        };
        return kNames[pitchClass % 12];
    }

    // ── Staff drawing ─────────────────────────────────────────────────────────

    void drawStaff(juce::Graphics& g, int saxKey,
                   const std::array<bool, 12>& scaleNotes,
                   int W, int H) const
    {
        // Staff area
        const float staffX     = 60.f;   // left margin (after clef)
        const float staffRight = W - 24.f;
        const float staffMidY  = H * 0.50f;   // centre of the 5 lines
        const float lineSpacing = 10.f;  // pixels between staff lines
        const float staffTop   = staffMidY - 2.f * lineSpacing;
        const float staffBot   = staffMidY + 2.f * lineSpacing;

        // 5 staff lines
        g.setColour(SaxFXColours::textPrimary.withAlpha(0.85f));
        for (int li = 0; li < 5; ++li)
        {
            const float ly = staffTop + li * lineSpacing;
            g.drawLine(staffX, ly, staffRight, ly, 1.0f);
        }

        // Treble clef (simplified — Unicode glyph)
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(lineSpacing * 7.5f)));
        g.setColour(SaxFXColours::textPrimary);
        g.drawText(juce::String::fromUTF8("\xf0\x9d\x84\x9e"),  // U+1D11E 𝄞
                   juce::Rectangle<float>(4.f, staffTop - lineSpacing * 1.5f,
                                          50.f, lineSpacing * 9.f),
                   juce::Justification::centred, false);

        // Key signature (sharps/flats for sax key)
        drawKeySignature(g, saxKey, staffX, staffTop, lineSpacing);

        // Scale note heads (1 octave from D4, the "middle" sax register)
        // Starting MIDI note: find the scale root in octave 5 (sax reading range)
        const int baseOctave   = 5;   // octave 5 = sax comfortable range
        const int baseMidi     = baseOctave * 12 + saxKey;  // e.g. D5 = 74 for D key
        const float noteHeadR  = lineSpacing * 0.45f;
        const float noteSpacingX = (staffRight - staffX - 60.f); // available width

        auto scaleArr = getScaleArray();
        const int numNotes = static_cast<int>(scaleArr.size());
        if (numNotes == 0) return;

        // Include octave repeat (first note + 1 octave)
        const int totalNotes = numNotes + 1;
        const float stepX    = noteSpacingX / static_cast<float>(totalNotes + 1);

        for (int ni = 0; ni <= numNotes; ++ni)
        {
            const int semOffset  = (ni < numNotes) ? scaleArr[ni] : 12;
            const int midi       = baseMidi + semOffset;
            const int pitchClass = midi % 12;
            const bool inScale   = scaleNotes[pitchClass];

            // Diatonic step position on staff (0 = bottom line E4, 0.5 = F4...)
            const float dStep  = midiToDiatonicStep(midi);
            // Bottom line of treble clef staff is E4 (dStep=0 → staffBot)
            const float noteY  = staffBot - dStep * lineSpacing;
            const float noteX  = staffX + 60.f + (ni + 1) * stepX;

            // Ledger lines if needed
            drawLedgerLines(g, noteX, noteY, noteHeadR, staffTop, staffBot, lineSpacing);

            // Note head
            const juce::Colour headCol = inScale
                ? juce::Colour(0xFF4CDFA8)
                : SaxFXColours::textSecondary.withAlpha(0.4f);
            g.setColour(headCol);
            g.fillEllipse(noteX - noteHeadR * 1.4f, noteY - noteHeadR,
                          noteHeadR * 2.8f, noteHeadR * 2.0f);

            // Stem up
            if (dStep < 4.f)
            {
                g.setColour(headCol);
                g.drawLine(noteX + noteHeadR * 1.2f, noteY,
                           noteX + noteHeadR * 1.2f, noteY - lineSpacing * 3.5f, 1.3f);
            }

            // Accidental (sharp/flat) if black key
            const bool isBlack = isBlackKey(pitchClass);
            if (isBlack)
            {
                g.setColour(headCol);
                g.setFont(juce::Font(juce::FontOptions{}.withHeight(lineSpacing * 1.6f)));
                g.drawText("#", juce::Rectangle<float>(noteX - noteHeadR * 3.0f,
                                                       noteY - noteHeadR * 1.5f,
                                                       noteHeadR * 2.5f, noteHeadR * 3.0f),
                           juce::Justification::centred, false);
            }

            // Note name label below staff
            g.setColour(inScale ? juce::Colour(0xFF4CDFA8).withAlpha(0.8f)
                                : SaxFXColours::textSecondary.withAlpha(0.5f));
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(9.f)));
            g.drawText(noteNameStr(pitchClass),
                       juce::Rectangle<float>(noteX - 12.f, staffBot + lineSpacing * 1.2f,
                                              24.f, 12.f),
                       juce::Justification::centred, false);
        }

        // Scale name label
        g.setColour(SaxFXColours::textSecondary);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(10.f)));
        const juce::String scaleLabel = noteNameStr(saxKey) + " " + scaleTypeName();
        g.drawText(scaleLabel, juce::Rectangle<float>(staffX, staffBot + lineSpacing * 2.5f,
                                                      200.f, 14.f),
                   juce::Justification::centredLeft, false);
    }

    void drawKeySignature(juce::Graphics& g, int saxKey,
                          float staffX, float staffTop, float lineSpacing) const
    {
        // Number of sharps/flats for major keys
        // Sharp keys:  G=1, D=2, A=3, E=4, B=5, F#=6, C#=7
        // Flat keys:   F=1, Bb=2, Eb=3, Ab=4, Db=5, Gb=6, Cb=7
        static constexpr int kSharps[12] = { 0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5 };
        // negative = flats count, positive = sharps count
        const int sig = kSharps[saxKey];  // sharps needed for this major key
        const bool useFlats = (sig > 6);
        const int count = useFlats ? (12 - sig) : sig;

        if (count == 0) return;

        // Sharp/flat positions on treble clef staff (diatonic steps above E4)
        static constexpr float kSharpPositions[7] = { 4.5f,3.0f,5.0f,3.5f,2.0f,4.0f,2.5f };
        static constexpr float kFlatPositions[7]  = { 2.0f,3.5f,1.5f,3.0f,1.0f,2.5f,0.5f };

        g.setColour(SaxFXColours::textPrimary.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(lineSpacing * 1.8f)));

        const float* positions = useFlats ? kFlatPositions : kSharpPositions;
        const juce::String symbol = useFlats ? "\xe2\x99\xad" : "#";

        for (int i = 0; i < count; ++i)
        {
            const float noteY = (staffTop + 4 * lineSpacing) - positions[i] * lineSpacing;
            g.drawText(symbol,
                       juce::Rectangle<float>(staffX + 2.f + i * 10.f,
                                              noteY - lineSpacing,
                                              10.f, lineSpacing * 2.f),
                       juce::Justification::centred, false);
        }
    }

    void drawLedgerLines(juce::Graphics& g, float noteX, float noteY,
                         float noteHeadR, float staffTop, float staffBot,
                         float lineSpacing) const
    {
        g.setColour(SaxFXColours::textPrimary.withAlpha(0.7f));
        const float lw = noteHeadR * 3.0f;

        // Above staff
        if (noteY < staffTop - 0.5f * lineSpacing)
        {
            for (float ly = staffTop - lineSpacing; ly >= noteY - lineSpacing * 0.5f;
                 ly -= lineSpacing)
                g.drawLine(noteX - lw, ly, noteX + lw, ly, 0.8f);
        }
        // Below staff
        if (noteY > staffBot + 0.5f * lineSpacing)
        {
            for (float ly = staffBot + lineSpacing; ly <= noteY + lineSpacing * 0.5f;
                 ly += lineSpacing)
                g.drawLine(noteX - lw, ly, noteX + lw, ly, 0.8f);
        }
    }

    std::vector<int> getScaleArray() const
    {
        switch (scaleType_)
        {
            case PianoKeyboardPanel::ScaleType::Major:
                return { kMajorScale, kMajorScale + 7 };
            case PianoKeyboardPanel::ScaleType::Minor:
                return { kMinorScale, kMinorScale + 7 };
            case PianoKeyboardPanel::ScaleType::PentatonicMaj:
                return { kPentatonicMajor, kPentatonicMajor + 5 };
            case PianoKeyboardPanel::ScaleType::PentatonicMin:
                return { kPentatonicMinor, kPentatonicMinor + 5 };
            case PianoKeyboardPanel::ScaleType::Blues:
                return { kBlues, kBlues + 6 };
            case PianoKeyboardPanel::ScaleType::Dorian:
                return { kDorian, kDorian + 7 };
        }
        return {};
    }

    juce::String scaleTypeName() const
    {
        switch (scaleType_)
        {
            case PianoKeyboardPanel::ScaleType::Major:         return "Majeur";
            case PianoKeyboardPanel::ScaleType::Minor:         return "Mineur";
            case PianoKeyboardPanel::ScaleType::PentatonicMaj: return "Pentatonique Maj";
            case PianoKeyboardPanel::ScaleType::PentatonicMin: return "Pentatonique Min";
            case PianoKeyboardPanel::ScaleType::Blues:         return "Blues";
            case PianoKeyboardPanel::ScaleType::Dorian:        return "Dorien";
        }
        return "";
    }

    static bool isBlackKey(int pitchClass) noexcept
    {
        static constexpr bool kBlack[12] = {
            false,true,false,true,false,false,true,false,true,false,true,false
        };
        return kBlack[pitchClass % 12];
    }

    // ── Members ───────────────────────────────────────────────────────────────

    juce::ComboBox scaleCombo_;

    int         masterKey_   = 0;
    bool        masterMajor_ = true;
    PianoKeyboardPanel::ScaleType scaleType_ = PianoKeyboardPanel::ScaleType::Major;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SaxStaffPanel)
};

} // namespace ui
