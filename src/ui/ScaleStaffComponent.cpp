#include "ScaleStaffComponent.h"
#include "Colours.h"

namespace ui {

// ── Scale interval tables ─────────────────────────────────────────────────────
// Semitones above root, excluding the octave repeat.

const std::vector<int>& ScaleStaffComponent::scaleIntervals(ScaleType t)
{
    static const std::vector<int> major         { 0,2,4,5,7,9,11 };
    static const std::vector<int> minor         { 0,2,3,5,7,8,10 };
    static const std::vector<int> pentatonicMaj { 0,2,4,7,9 };
    static const std::vector<int> pentatonicMin { 0,3,5,7,10 };
    static const std::vector<int> blues         { 0,3,5,6,7,10 };
    static const std::vector<int> dorian        { 0,2,3,5,7,9,10 };

    switch (t)
    {
        case ScaleType::Major:         return major;
        case ScaleType::Minor:         return minor;
        case ScaleType::PentatonicMaj: return pentatonicMaj;
        case ScaleType::PentatonicMin: return pentatonicMin;
        case ScaleType::Blues:         return blues;
        case ScaleType::Dorian:        return dorian;
    }
    return major;
}

// ── Treble-clef staff mapping ─────────────────────────────────────────────────
// Staff positions for notes in the range E4(64) – F5(77), natural notes only.
// Position 0.0 = top line (F5), 4.0 = bottom line (E4), 0.5 = space between lines.
// Sharps/flats share the diatonic slot of their lower neighbour + accidental flag.

std::pair<float, bool> ScaleStaffComponent::pitchClassToStaffInfo(int pitchClass)
{
    // Map pitch class (0=C…11=B) → {diatonic staff step from E4, isSharp}
    // Diatonic steps counting from E4=0: E=0, F=0.5(top), G=1, A=1.5, B=2, C=2.5, D=3
    // (staff positions are *descending* from top to bottom)
    // We work relative to C4 (MIDI 60) to keep the table simple, then adjust for octave.

    // Position within one octave of C (C=0, D=1, E=2, F=2.5, G=3, A=3.5, B=4) — ascending.
    // Then we convert to treble staff (which is *descending*).

    // Diatonic slot ascending from C (each half-step = +0.5 position on ascending scale):
    // C=0, D=1, E=2, F=3, G=4, A=5, B=6  (diatonic steps)
    // Sharps sit on their lower neighbour's slot.

    static const std::pair<float, bool> table[12] = {
        { 0.f,   false },  // C  (pitch class 0)
        { 0.f,   true  },  // C# (sits on C slot, sharp)
        { 1.f,   false },  // D
        { 1.f,   true  },  // D# (Eb)
        { 2.f,   false },  // E
        { 3.f,   false },  // F
        { 3.f,   true  },  // F# (Gb)
        { 4.f,   false },  // G
        { 4.f,   true  },  // G# (Ab)
        { 5.f,   false },  // A
        { 5.f,   true  },  // A# (Bb)
        { 6.f,   false },  // B
    };

    return table[pitchClass % 12];
}

float ScaleStaffComponent::midiToStaffPos(int midiNote)
{
    // Treble staff reference: E4(64) = bottom line (pos 4.0), F5(77) = top line (pos 0.0).
    // Each diatonic step = 0.5 position.
    // C4 (MIDI 60) = below the staff: 3.5 spaces below E4 bottom line → pos 4.0 + 3.5 = 7.5
    // but expressed as ascending diatonic steps from C: E4 is diatonic step 2 above C4.
    // Strategy: compute diatonic distance from C4 upward, then convert to staff position.

    const int octave    = midiNote / 12;  // MIDI octave
    const int pitchClass = midiNote % 12;

    const float diatSlot = pitchClassToStaffInfo(pitchClass).first;
    // diatSlot: ascending diatonic slot from C within the octave (C=0…B=6)

    // Absolute diatonic step from C4 (MIDI 60):
    //   MIDI internal octave = midiNote/12; C4=60 → internal octave = 5 (not 4!).
    //   We use refOctave = 60/12 = 5 so the arithmetic cancels correctly.
    const int   refOctave = 60 / 12;  // = 5 (MIDI internal, not the "C4" display octave)
    const float absDiat   = static_cast<float>((octave - refOctave) * 7) + diatSlot;

    // E4 = diatonic step 2 above C4 → absDiat for E4 = 2.0
    // Staff pos 4.0 corresponds to absDiat = 2.0  → staffPos = 4.0 - (absDiat - 2.0)*0.5
    const float staffPos = 4.0f - (absDiat - 2.0f) * 0.5f;
    return staffPos;
}

// ── Public API ────────────────────────────────────────────────────────────────

void ScaleStaffComponent::setKey(int root, ScaleType type)
{
    root_  = root;
    scale_ = type;
    rebuildNoteInfos();
    repaint();
}

void ScaleStaffComponent::rebuildNoteInfos()
{
    noteInfos_.clear();

    // Anchor the scale starting at a comfortable octave (root around C4–B4).
    // MIDI 60 = C4. Find the root in octave 4/5 so notes sit on the staff.
    int startMidi = 60 + root_;   // root in octave 4-ish
    // Shift down one octave if the root would push notes very high on the staff
    if (startMidi > 71) startMidi -= 12;  // keep root below C5

    const auto& intervals = scaleIntervals(scale_);
    for (int semi : intervals)
    {
        const int  midi      = startMidi + semi;
        const bool isSharp   = pitchClassToStaffInfo(midi % 12).second;
        noteInfos_.push_back({ midiToStaffPos(midi), isSharp });
    }
    // Add root at top octave so the scale feels complete
    {
        const int  midi    = startMidi + 12;
        const bool isSharp = pitchClassToStaffInfo(midi % 12).second;
        noteInfos_.push_back({ midiToStaffPos(midi), isSharp });
    }
}

// ── paint() ──────────────────────────────────────────────────────────────────

void ScaleStaffComponent::paint(juce::Graphics& g)
{
    using namespace SaxFXColours;

    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());

    // ── Fond pleine largeur ───────────────────────────────────────────────────
    g.setColour(juce::Colour(0xFF0D0E11));
    g.fillRoundedRectangle(0.f, 0.f, w, h, 7.f);
    g.setColour(neonCyan.withAlpha(0.10f));
    g.drawRoundedRectangle(1.f, 1.f, w - 2.f, h - 2.f, 7.f, 1.5f);
    g.setColour(cardBorder);
    g.drawRoundedRectangle(0.5f, 0.5f, w - 1.f, h - 1.f, 7.f, 0.8f);

    // Label "SCALE"
    g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.f).withStyle("Bold")));
    g.setColour(neonCyan.withAlpha(0.45f));
    g.drawText("SCALE", 10, 5, 50, 10, juce::Justification::centredLeft);

    // ── Géométrie — pleine largeur du cadre ──────────────────────────────────
    const float marginLeft  = 48.f;
    const float marginRight = 12.f;
    const float staffTop    = h * 0.32f;
    const float staffBottom = h * 0.72f;
    const float lineSpacing = (staffBottom - staffTop) / 4.0f;
    const float staffLeft   = marginLeft;
    const float staffRight  = w - marginRight;
    const float noteRadius  = lineSpacing * 0.42f;

    const int   n    = static_cast<int>(noteInfos_.size());
    const float step = n > 1 ? (staffRight - staffLeft - noteRadius * 2.f)
                               / static_cast<float>(n - 1)
                             : 0.f;

    // ── 5 lignes de portée — épaisseur 1.5px, bien visibles ─────────────────
    g.setColour(juce::Colour(0xFF4A4E55));
    for (int line = 0; line < 5; ++line)
    {
        const float y = staffTop + static_cast<float>(line) * lineSpacing;
        g.fillRect(staffLeft, y - 0.75f, staffRight - staffLeft, 1.5f);
    }

    // ── Clef de sol ───────────────────────────────────────────────────────────
    // On utilise le caractère Unicode avec un grand font + fallback système
    {
        const float clefBoxTop = staffTop - lineSpacing * 1.6f;
        const float clefBoxH   = lineSpacing * 7.8f;
        const float clefBoxW   = marginLeft - 2.f;

        // Halo derrière la clef
        g.setColour(aiBadge.withAlpha(0.06f));
        g.fillRoundedRectangle(2.f, clefBoxTop, clefBoxW - 2.f, clefBoxH, 4.f);

        // Clef de sol — caractère Unicode U+1D11E
        // DirectWrite (Windows) tente un fallback automatique si le font primaire
        // ne contient pas le glyphe.
        g.setFont(juce::Font(juce::FontOptions{}
                             .withName("Segoe UI Symbol")
                             .withHeight(clefBoxH * 0.88f)));
        g.setColour(aiBadge.withAlpha(0.90f));
        g.drawText(juce::CharPointer_UTF8("\xf0\x9d\x84\x9e"),  // 𝄞 U+1D11E
                   juce::Rectangle<float>(0.f, clefBoxTop, clefBoxW, clefBoxH),
                   juce::Justification::centred);
    }

    if (noteInfos_.empty()) return;

    // ── Distribution des notes ────────────────────────────────────────────────

    for (int i = 0; i < n; ++i)
    {
        const auto& ni  = noteInfos_[static_cast<std::size_t>(i)];
        const float cx  = staffLeft + noteRadius + static_cast<float>(i) * step;
        const float cy  = staffTop + ni.staffPos * lineSpacing;
        const bool isRoot = (i == 0 || i == n - 1);

        // Couleur : racine = vert néon (aiBadge), autres = cyan
        const juce::Colour noteCol = isRoot ? aiBadge : neonCyan;

        // ── Lignes supplémentaires (ledger lines) ────────────────────────────
        g.setColour(juce::Colour(0xFF4A4E55));
        if (ni.staffPos < -0.05f)
        {
            for (float lp = -1.f; lp >= ni.staffPos - 0.05f; lp -= 1.0f)
            {
                const float ly = staffTop + lp * lineSpacing;
                g.fillRect(cx - noteRadius * 2.0f, ly - 0.75f,
                           noteRadius * 4.0f, 1.5f);
            }
        }
        if (ni.staffPos > 4.05f)
        {
            for (float lp = 5.f; lp <= ni.staffPos + 0.05f; lp += 1.0f)
            {
                const float ly = staffTop + lp * lineSpacing;
                g.fillRect(cx - noteRadius * 2.0f, ly - 0.75f,
                           noteRadius * 4.0f, 1.5f);
            }
        }

        // ── Halo extérieur (glow) ────────────────────────────────────────────
        g.setColour(noteCol.withAlpha(0.08f));
        g.fillEllipse(cx - noteRadius * 1.9f, cy - noteRadius * 1.4f,
                      noteRadius * 3.8f,      noteRadius * 2.8f);

        // ── Halo intermédiaire ────────────────────────────────────────────────
        g.setColour(noteCol.withAlpha(0.20f));
        g.fillEllipse(cx - noteRadius * 1.35f, cy - noteRadius * 1.0f,
                      noteRadius * 2.7f,       noteRadius * 2.0f);

        // ── Note pleine ───────────────────────────────────────────────────────
        g.setColour(noteCol);
        g.fillEllipse(cx - noteRadius,        cy - noteRadius * 0.70f,
                      noteRadius * 2.0f,      noteRadius * 1.40f);

        // Petit reflet blanc en haut à gauche
        g.setColour(juce::Colours::white.withAlpha(0.22f));
        g.fillEllipse(cx - noteRadius * 0.55f, cy - noteRadius * 0.52f,
                      noteRadius * 0.60f,      noteRadius * 0.42f);

        // ── Dièse (#) ─────────────────────────────────────────────────────────
        if (ni.isSharp)
        {
            g.setFont(juce::Font(juce::FontOptions{}
                                 .withHeight(noteRadius * 2.6f)
                                 .withStyle("Bold")));
            g.setColour(vuMid.withAlpha(0.92f));
            g.drawText("#",
                       juce::Rectangle<float>(cx - noteRadius * 3.4f,
                                              cy - noteRadius * 1.8f,
                                              noteRadius * 2.4f,
                                              noteRadius * 3.0f),
                       juce::Justification::centred);
        }
    }
}

} // namespace ui
