#pragma once

#include "Colours.h"
#include "PianoKeyboardPanel.h"

#include <JuceHeader.h>
#include <array>
#include <vector>
#include <cmath>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// SaxStaffPanel
//
// Displays a beautifully engraved treble-clef musical staff showing the notes 
// playable on the selected saxophone for the current master key.
// Includes true diatonic spelling and key signature handling to prevent note overlap.
// ─────────────────────────────────────────────────────────────────────────────

class SaxStaffPanel : public juce::Component
{
public:
    enum class Transposition { TenorBb = 0, AltoEb, ConcertC };

    SaxStaffPanel()
    {
        // Scale selector
        scaleCombo_.addItem("Majeur",            1);
        scaleCombo_.addItem("Mineur",            2);
        scaleCombo_.addItem("Pentatonique Maj", 3);
        scaleCombo_.addItem("Pentatonique Min", 4);
        scaleCombo_.addItem("Blues",            5);
        scaleCombo_.addItem("Dorien",           6);
        scaleCombo_.setSelectedId(1, juce::dontSendNotification);
        scaleCombo_.onChange = [this] {
            scaleType_ = static_cast<PianoKeyboardPanel::ScaleType>(scaleCombo_.getSelectedId() - 1);
            repaint();
        };
        addAndMakeVisible(scaleCombo_);

        // Transposition selector
        transCombo_.addItem(juce::String::fromUTF8("T\xc3\xa9nor/Sopr (Si\xe2\x99\xad)"), 1);
        transCombo_.addItem(juce::String::fromUTF8("Alto/Bari (Mi\xe2\x99\xad)"), 2);
        transCombo_.addItem(juce::String::fromUTF8("Concert (Ut)"), 3);
        transCombo_.setSelectedId(1, juce::dontSendNotification);
        transCombo_.onChange = [this] {
            trans_ = static_cast<Transposition>(transCombo_.getSelectedId() - 1);
            repaint();
        };
        addAndMakeVisible(transCombo_);
    }

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

    void resized() override
    {
        scaleCombo_.setBounds(8, 6, 160, 24);
        transCombo_.setBounds(176, 6, 160, 24);
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

        // Transposition math
        int transOffset = 0;
        juce::String transName = "";
        if (trans_ == Transposition::TenorBb) { transOffset = 2; transName = juce::String::fromUTF8("T\xc3\xa9nor/Soprano"); }
        else if (trans_ == Transposition::AltoEb) { transOffset = 9; transName = "Alto/Baryton"; }
        else { transOffset = 0; transName = "Concert"; }

        const int transposedKey = (masterKey_ + transOffset) % 12;

        // Info banner
        g.setColour(SaxFXColours::textSecondary);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(11.f)));
        const juce::String info = "Instrument : " + transName +
                                  "  \xe2\x80\x94  Grille concert : " + noteNameStr(masterKey_) + (masterMajor_ ? " Maj" : " min") +
                                  "  \xe2\x86\x92  Lecture : " + noteNameStr(transposedKey) + " " + scaleTypeName();
        g.drawText(info, 8, 36, W - 16, 16, juce::Justification::centredLeft, true);

        // Get notes to draw based on correct diatonic spelling
        auto notes = spellScaleNotes(transposedKey);

        drawStaff(g, transposedKey, notes, W, H);
    }

private:
    struct SpelledNote {
        int absStep;
        int accidental;    // -1 (flat), 0 (natural), 1 (sharp)
        juce::String name; // Formatted note name for UI
    };

    struct ScaleDef { int interval; int degreeOffset; };

    std::vector<ScaleDef> getScaleDef() const
    {
        switch (scaleType_)
        {
            case PianoKeyboardPanel::ScaleType::Major:         return {{0,0},{2,1},{4,2},{5,3},{7,4},{9,5},{11,6}};
            case PianoKeyboardPanel::ScaleType::Minor:         return {{0,0},{2,1},{3,2},{5,3},{7,4},{8,5},{10,6}};
            case PianoKeyboardPanel::ScaleType::PentatonicMaj: return {{0,0},{2,1},{4,2},{7,4},{9,5}};
            case PianoKeyboardPanel::ScaleType::PentatonicMin: return {{0,0},{3,2},{5,3},{7,4},{10,6}};
            case PianoKeyboardPanel::ScaleType::Blues:         return {{0,0},{3,2},{5,3},{6,3},{7,4},{10,6}}; // Degree 3 used twice intentionally (e.g., F and F#)
            case PianoKeyboardPanel::ScaleType::Dorian:        return {{0,0},{2,1},{3,2},{5,3},{7,4},{9,5},{10,6}};
        }
        return {};
    }

    std::vector<SpelledNote> spellScaleNotes(int rootPitch) const
    {
        std::vector<SpelledNote> result;
        
        // 1. Determine key signature of the scale's relative major
        int majorRoot = rootPitch;
        if (scaleType_ == PianoKeyboardPanel::ScaleType::Minor ||
            scaleType_ == PianoKeyboardPanel::ScaleType::PentatonicMin ||
            scaleType_ == PianoKeyboardPanel::ScaleType::Blues) {
            majorRoot = (rootPitch + 3) % 12;
        } else if (scaleType_ == PianoKeyboardPanel::ScaleType::Dorian) {
            majorRoot = (rootPitch - 2 + 12) % 12;
        }

        static const int majorPitchToK[12] = {0, -5, 2, -3, 4, -1, -6, 1, -4, 3, -2, 5};
        int K = majorPitchToK[majorRoot];

        // 2. Determine root diatonic step (0=C .. 6=B)
        int rootStep = 0;
        int nat[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
        if (nat[rootPitch] != -1) rootStep = nat[rootPitch];
        else {
            if (rootPitch == 1) rootStep = (K > 0) ? 0 : 1;
            if (rootPitch == 3) rootStep = (K > 0) ? 1 : 2;
            if (rootPitch == 6) rootStep = (K > 0) ? 3 : 4;
            if (rootPitch == 8) rootStep = (K > 0) ? 4 : 5;
            if (rootPitch == 10) rootStep = (K > 0) ? 5 : 6;
        }

        // 3. Anchor in a comfortable saxophone reading octave (C4 = step 28. D4 = step 29)
        int rootAbsStep = 4 * 7 + rootStep; 
        if (rootAbsStep < 29) rootAbsStep += 7; // Push up to D4-C5 range

        // 4. Spell notes
        auto def = getScaleDef();
        for (size_t i = 0; i <= def.size(); ++i) {
            bool isOctave = (i == def.size());
            auto d = def[isOctave ? 0 : i];
            
            int interval = d.interval + (isOctave ? 12 : 0);
            int degOffset = d.degreeOffset + (isOctave ? 7 : 0);

            int absStep = rootAbsStep + degOffset;
            int stepName = absStep % 7;
            int targetPitchClass = (rootPitch + interval) % 12;
            static const int kNat[7] = {0, 2, 4, 5, 7, 9, 11};
            int naturalPitchClass = kNat[stepName];

            int acc = targetPitchClass - naturalPitchClass;
            if (acc > 6) acc -= 12;
            if (acc < -5) acc += 12;

            static const char* kNames[7] = {"C", "D", "E", "F", "G", "A", "B"};
            juce::String name = kNames[stepName];
            if (acc == 1) name += juce::String::fromUTF8("\xe2\x99\xaf"); // Sharp
            else if (acc == -1) name += juce::String::fromUTF8("\xe2\x99\xad"); // Flat

            result.push_back({absStep, acc, name});
        }

        return result;
    }

    void drawStaff(juce::Graphics& g, int rootPitch, const std::vector<SpelledNote>& notes, int W, int H) const
    {
        const float staffX = 70.f;
        const float staffRight = W - 30.f;
        const float staffMidY = H * 0.55f; // Centred slightly lower
        const float lineSpacing = 10.f;
        const float staffBot = staffMidY + 2.f * lineSpacing; // E4
        const float staffTop = staffMidY - 2.f * lineSpacing; // F5

        // 1. Draw 5 staff lines
        g.setColour(SaxFXColours::textPrimary.withAlpha(0.6f));
        for (int i = 0; i < 5; ++i) {
            float ly = staffTop + i * lineSpacing;
            g.drawLine(staffX - 10.f, ly, staffRight, ly, 1.2f);
        }

        // 2. Draw treble clef
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(lineSpacing * 7.0f)));
        g.setColour(SaxFXColours::textPrimary);
        g.drawText(juce::String::fromUTF8("\xf0\x9d\x84\x9e"), // G Clef Unicode
                   juce::Rectangle<float>(staffX - 5.f, staffTop - lineSpacing * 1.3f, 30.f, lineSpacing * 8.f),
                   juce::Justification::centred, false);

        // 3. Determine Key Signature
        int majorRoot = rootPitch;
        if (scaleType_ == PianoKeyboardPanel::ScaleType::Minor ||
            scaleType_ == PianoKeyboardPanel::ScaleType::PentatonicMin ||
            scaleType_ == PianoKeyboardPanel::ScaleType::Blues) majorRoot = (rootPitch + 3) % 12;
        else if (scaleType_ == PianoKeyboardPanel::ScaleType::Dorian) majorRoot = (rootPitch - 2 + 12) % 12;

        static const int majorPitchToK[12] = {0, -5, 2, -3, 4, -1, -6, 1, -4, 3, -2, 5};
        int K = majorPitchToK[majorRoot];

        int keySig[7] = {0}; // Array tracking accidental for C, D, E, F, G, A, B
        float kx = staffX + 30.f;
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(lineSpacing * 2.2f)));
        
        if (K > 0) {
            int sharpsOrder[7] = {3, 0, 4, 1, 5, 2, 6}; // F C G D A E B
            int sSteps[7] = {38, 35, 39, 36, 33, 37, 34}; // Treble positions for sharps
            for (int i = 0; i < K; ++i) {
                keySig[sharpsOrder[i]] = 1;
                float sy = staffBot - (sSteps[i] - 30) * (lineSpacing / 2.0f);
                g.drawText(juce::String::fromUTF8("\xe2\x99\xaf"), juce::Rectangle<float>(kx, sy - lineSpacing, 12.f, lineSpacing * 2.f), juce::Justification::centred, false);
                kx += 10.f;
            }
        } else if (K < 0) {
            int flatsOrder[7] = {6, 2, 5, 1, 4, 0, 3}; // B E A D G C F
            int fSteps[7] = {34, 37, 33, 36, 32, 35, 31}; // Treble positions for flats
            for (int i = 0; i < -K; ++i) {
                keySig[flatsOrder[i]] = -1;
                float sy = staffBot - (fSteps[i] - 30) * (lineSpacing / 2.0f);
                g.drawText(juce::String::fromUTF8("\xe2\x99\xad"), juce::Rectangle<float>(kx, sy - lineSpacing, 12.f, lineSpacing * 2.f), juce::Justification::centred, false);
                kx += 10.f;
            }
        }

        kx += 15.f; // Spacing before first note

        // 4. Draw Scale Notes
        if (notes.empty()) return;
        float stepX = (staffRight - kx - 20.f) / notes.size();
        float noteHeadR = lineSpacing * 0.48f;

        int barAccidentals[7]; // Track overrides within the bar
        for(int i=0; i<7; ++i) barAccidentals[i] = keySig[i];

        for (size_t i = 0; i < notes.size(); ++i) {
            auto& n = notes[i];
            float nx = kx + i * stepX;
            float ny = staffBot - (n.absStep - 30) * (lineSpacing / 2.0f);

            // Ledger lines
            g.setColour(SaxFXColours::textPrimary.withAlpha(0.6f));
            if (n.absStep <= 29) { // D4 and below
                for (int s = 28; s >= n.absStep; s -= 2) {
                    float ly = staffBot - (s - 30) * (lineSpacing / 2.f);
                    g.drawLine(nx - noteHeadR*2.2f, ly, nx + noteHeadR*2.2f, ly, 1.2f);
                }
            } else if (n.absStep >= 39) { // G5 and above
                for (int s = 40; s <= n.absStep; s += 2) {
                    float ly = staffBot - (s - 30) * (lineSpacing / 2.f);
                    g.drawLine(nx - noteHeadR*2.2f, ly, nx + noteHeadR*2.2f, ly, 1.2f);
                }
            }

            // Accidental (Draw only if it differs from the current bar's key signature state)
            int stepClass = n.absStep % 7;
            if (n.accidental != barAccidentals[stepClass]) {
                juce::String accStr;
                if (n.accidental == 1) accStr = juce::String::fromUTF8("\xe2\x99\xaf"); // #
                else if (n.accidental == -1) accStr = juce::String::fromUTF8("\xe2\x99\xad"); // b
                else accStr = juce::String::fromUTF8("\xe2\x99\xae"); // natural

                g.setColour(SaxFXColours::textPrimary);
                g.setFont(juce::Font(juce::FontOptions{}.withHeight(lineSpacing * 2.2f)));
                g.drawText(accStr, juce::Rectangle<float>(nx - noteHeadR*4.0f, ny - lineSpacing, noteHeadR*2.5f, lineSpacing * 2.f), juce::Justification::centredRight, false);
                barAccidentals[stepClass] = n.accidental; // update state for next note on this line
            }

            // Note head
            g.setColour(juce::Colour(0xFF4CDFA8)); // Neon green
            g.fillEllipse(nx - noteHeadR*1.3f, ny - noteHeadR, noteHeadR*2.6f, noteHeadR*2.0f);

            // Stem
            g.setColour(juce::Colour(0xFF4CDFA8));
            if (n.absStep >= 34) { // B4 and above -> Stem goes DOWN from LEFT side
                g.drawLine(nx - noteHeadR*0.9f, ny, nx - noteHeadR*0.9f, ny + lineSpacing * 3.2f, 1.5f);
            } else { // Stem goes UP from RIGHT side
                g.drawLine(nx + noteHeadR*0.9f, ny, nx + noteHeadR*0.9f, ny - lineSpacing * 3.2f, 1.5f);
            }

            // Text Label (Staggered to prevent overlap)
            g.setColour(SaxFXColours::textSecondary);
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(10.f)));
            float labelY = staffBot + lineSpacing * 2.5f + (i % 2 == 1 ? 12.f : 0.f);
            g.drawText(n.name, juce::Rectangle<float>(nx - 15.f, labelY, 30.f, 12.f), juce::Justification::centred, false);
        }
    }

    static juce::String noteNameStr(int pitchClass) noexcept
    {
        static const char* kNames[12] = {
            "C", "C\xe2\x99\xaf", "D", "E\xe2\x99\xad", "E", "F",
            "F\xe2\x99\xaf", "G", "A\xe2\x99\xad", "A", "B\xe2\x99\xad", "B"
        };
        return juce::String::fromUTF8(kNames[pitchClass % 12]);
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

    juce::ComboBox scaleCombo_;
    juce::ComboBox transCombo_;

    int         masterKey_   = 0;
    bool        masterMajor_ = true;
    PianoKeyboardPanel::ScaleType scaleType_ = PianoKeyboardPanel::ScaleType::Major;
    Transposition trans_ = Transposition::TenorBb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SaxStaffPanel)
};

} // namespace ui
