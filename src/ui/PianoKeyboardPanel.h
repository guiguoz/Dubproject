#pragma once

#include "Colours.h"
#include "SaxOsLookAndFeel.h"
#include "dsp/ScaleHarmonizer.h"  // for kMajorScale / kMinorScale tables
#include "dsp/KeyboardSynth.h"

#include <JuceHeader.h>
#include <array>
#include <functional>
#include <cmath>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// PianoKeyboardPanel
//
// Displays a 2-octave piano keyboard with scale notes highlighted in neon.
// Clicking a key injects that frequency into the DspPipeline (forcing
// the SynthEffect to play it).  Parameter knobs control SynthEffect params.
//
// ScaleType defines which notes are lit up.
// ─────────────────────────────────────────────────────────────────────────────

class PianoKeyboardPanel : public juce::Component
{
public:
    enum class ScaleType { Major, Minor, PentatonicMaj, PentatonicMin, Blues, Dorian };

    // ── Callbacks wired by MainComponent ─────────────────────────────────────
    std::function<void(float hz)> onNoteOn;   // conservé mais non câblé (sax SynthEffect)
    std::function<void()>         onNoteOff;
    std::function<void(int paramIdx, float value)> onSynthParam;

    // Keyboard synth indépendant (MIDI note int, pas Hz)
    std::function<void(int midiNote)> onKeyNoteOn;
    std::function<void(int midiNote)> onKeyNoteOff;
    std::function<void(float gain)>   onVolumeChanged;
    std::function<void(int presetIdx)> onPreset;

    // ── Constructor ───────────────────────────────────────────────────────────
    PianoKeyboardPanel()
    {
        // Scale selector
        scaleCombo_.addItem("Major",          1);
        scaleCombo_.addItem("Minor",          2);
        scaleCombo_.addItem("Pentatonique Maj", 3);
        scaleCombo_.addItem("Pentatonique Min", 4);
        scaleCombo_.addItem("Blues",          5);
        scaleCombo_.addItem("Dorian",         6);
        scaleCombo_.setSelectedId(1, juce::dontSendNotification);
        scaleCombo_.onChange = [this] {
            scaleType_ = static_cast<ScaleType>(scaleCombo_.getSelectedId() - 1);
            rebuildScaleSet();
            repaint();
        };
        addAndMakeVisible(scaleCombo_);

        // Preset selector
        presetCombo_.addItem("-- Preset --", 1);
        for (int i = 0; i < ::dsp::KeyboardSynth::presetCount(); ++i)
            presetCombo_.addItem(::dsp::KeyboardSynth::presetName(i), i + 2);
        presetCombo_.setSelectedId(1, juce::dontSendNotification);
        presetCombo_.onChange = [this] {
            const int id = presetCombo_.getSelectedId();
            if (id >= 2 && onPreset)
                onPreset(id - 2);
        };
        addAndMakeVisible(presetCombo_);

        // Octave selector
        octaveDownBtn_.setButtonText("<");
        octaveDownBtn_.onClick = [this] {
            if (baseOctave_ > 2) { --baseOctave_; updateOctaveLabel(); repaint(); }
        };
        addAndMakeVisible(octaveDownBtn_);

        octaveUpBtn_.setButtonText(">");
        octaveUpBtn_.onClick = [this] {
            if (baseOctave_ < 5) { ++baseOctave_; updateOctaveLabel(); repaint(); }
        };
        addAndMakeVisible(octaveUpBtn_);

        octaveLabel_.setJustificationType(juce::Justification::centred);
        octaveLabel_.setColour(juce::Label::textColourId, SaxFXColours::textPrimary);
        addAndMakeVisible(octaveLabel_);

        // SynthEffect parameter sliders (indices 0-7 matching SynthEffect params)
        static const char* kParamNames[] = {
            "Wave", "Oct", "Detune", "Cutoff", "Res", "Atk", "Rel", "Mix"
        };
        for (int i = 0; i < kNumParams; ++i)
        {
            paramSliders_[i].setSliderStyle(juce::Slider::RotaryVerticalDrag);
            paramSliders_[i].setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            paramSliders_[i].setRange(0.0, 1.0, 0.001);
            paramSliders_[i].setValue(0.5, juce::dontSendNotification);
            paramSliders_[i].onValueChange = [this, i] {
                if (onSynthParam)
                    onSynthParam(i, static_cast<float>(paramSliders_[i].getValue()));
            };
            addAndMakeVisible(paramSliders_[i]);

            paramLabels_[i].setText(kParamNames[i], juce::dontSendNotification);
            paramLabels_[i].setJustificationType(juce::Justification::centred);
            paramLabels_[i].setFont(juce::Font(juce::FontOptions{}.withHeight(9.f)));
            paramLabels_[i].setColour(juce::Label::textColourId,
                                      SaxFXColours::textSecondary);
            addAndMakeVisible(paramLabels_[i]);
        }

        // Volume slider clavier
        volumeSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
        volumeSlider_.setRange(0.0, 1.5, 0.01);
        volumeSlider_.setValue(0.5, juce::dontSendNotification);
        volumeSlider_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        volumeSlider_.setTooltip("Volume clavier");
        volumeSlider_.onValueChange = [this] {
            if (onVolumeChanged)
                onVolumeChanged(static_cast<float>(volumeSlider_.getValue()));
        };
        volumeLabel_.setText("Vol", juce::dontSendNotification);
        volumeLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(9.f)));
        volumeLabel_.setColour(juce::Label::textColourId, SaxFXColours::textSecondary);
        volumeLabel_.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(volumeSlider_);
        addAndMakeVisible(volumeLabel_);

        rebuildScaleSet();
        updateOctaveLabel();
    }

    // ── Public API ────────────────────────────────────────────────────────────

    void setMasterKey(int keyRoot, bool isMajor) noexcept
    {
        masterKey_  = keyRoot;
        masterMajor_ = isMajor;
        rebuildScaleSet();
        repaint();
    }

    void setScaleType(ScaleType t) noexcept
    {
        scaleType_ = t;
        scaleCombo_.setSelectedId(static_cast<int>(t) + 1, juce::dontSendNotification);
        rebuildScaleSet();
        repaint();
    }

    // ── Layout ────────────────────────────────────────────────────────────────

    void resized() override
    {
        const int W = getWidth();
        const int H = getHeight();

        // Top bar: scale combo | preset combo | ... | vol | octave
        const int topH = 28;
        scaleCombo_   .setBounds(  8,       4, 150, topH - 8);
        presetCombo_  .setBounds(162,       4, 150, topH - 8);
        volumeLabel_  .setBounds(W - 200,   4,  24, topH - 8);
        volumeSlider_ .setBounds(W - 175,   4,  80, topH - 8);
        octaveDownBtn_.setBounds(W -  90,   4,  24, topH - 8);
        octaveLabel_  .setBounds(W -  62,   4,  44, topH - 8);
        octaveUpBtn_  .setBounds(W -  18,   4,  24, topH - 8);

        // Bottom bar: param sliders
        const int paramH  = 60;
        const int sliderW = W / kNumParams;
        const int paramY  = H - paramH;
        for (int i = 0; i < kNumParams; ++i)
        {
            paramSliders_[i].setBounds(i * sliderW + 4,  paramY,     sliderW - 8, 40);
            paramLabels_[i] .setBounds(i * sliderW,      paramY + 42, sliderW,    12);
        }

        // Keyboard area
        keyboardY_ = topH + 4;
        keyboardH_ = H - topH - 4 - paramH - 4;
    }

    void paint(juce::Graphics& g) override
    {
        // Background
        g.setColour(SaxFXColours::cardBody);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.f);
        g.setColour(juce::Colour(0xFF4CDFA8).withAlpha(0.12f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.f, 1.f);

        drawKeyboard(g);
    }

    // ── Mouse ─────────────────────────────────────────────────────────────────

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int note = hitTestKey(e.x, e.y);
        if (note >= 0)
        {
            pressedNote_ = note;
            if (onKeyNoteOn) onKeyNoteOn(note);
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        const int released = pressedNote_;
        pressedNote_ = -1;
        if (onKeyNoteOff && released >= 0) onKeyNoteOff(released);
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const int note = hitTestKey(e.x, e.y);
        if (note >= 0 && note != pressedNote_)
        {
            const int prev = pressedNote_;
            pressedNote_ = note;
            if (onKeyNoteOff && prev >= 0) onKeyNoteOff(prev);
            if (onKeyNoteOn) onKeyNoteOn(note);
            repaint();
        }
    }

private:
    // ── Scale tables ─────────────────────────────────────────────────────────

    static constexpr int kMajorScale[7]        = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int kMinorScale[7]        = { 0, 2, 3, 5, 7, 8, 10 };
    static constexpr int kPentatonicMajor[5]   = { 0, 2, 4, 7, 9 };
    static constexpr int kPentatonicMinor[5]   = { 0, 3, 5, 7, 10 };
    static constexpr int kBlues[6]             = { 0, 3, 5, 6, 7, 10 };
    static constexpr int kDorian[7]            = { 0, 2, 3, 5, 7, 9, 10 };

    // White key order within an octave: C D E F G A B = semitones 0,2,4,5,7,9,11
    static constexpr int kWhiteSemitones[7]    = { 0, 2, 4, 5, 7, 9, 11 };
    // Black key positions (semitone within octave → white-key gap index)
    static constexpr int kBlackSemitones[5]    = { 1, 3, 6, 8, 10 };

    static constexpr int kNumParams   = 8;
    static constexpr int kNumOctaves  = 3;
    static constexpr int kWhiteKeysTotal = 7 * kNumOctaves;

    // ── Helpers ───────────────────────────────────────────────────────────────

    void rebuildScaleSet()
    {
        scaleNotes_.fill(false);
        const int* ptr = nullptr;
        int len = 0;
        switch (scaleType_)
        {
            case ScaleType::Major:         ptr = kMajorScale;       len = 7; break;
            case ScaleType::Minor:         ptr = kMinorScale;       len = 7; break;
            case ScaleType::PentatonicMaj: ptr = kPentatonicMajor;  len = 5; break;
            case ScaleType::PentatonicMin: ptr = kPentatonicMinor;  len = 5; break;
            case ScaleType::Blues:         ptr = kBlues;            len = 6; break;
            case ScaleType::Dorian:        ptr = kDorian;           len = 7; break;
        }
        for (int i = 0; i < len; ++i)
            scaleNotes_[(masterKey_ + ptr[i]) % 12] = true;
    }

    void updateOctaveLabel()
    {
        octaveLabel_.setText("Oct " + juce::String(baseOctave_),
                             juce::dontSendNotification);
    }

    static float midiToHz(int midi) noexcept
    {
        return 440.f * std::pow(2.f, (midi - 69) / 12.f);
    }

    // Returns the MIDI note under pixel (px, py), or -1
    int hitTestKey(int px, int py) const noexcept
    {
        if (py < keyboardY_ || py > keyboardY_ + keyboardH_) return -1;

        const int W = getWidth();
        const float whiteW = static_cast<float>(W) / static_cast<float>(kWhiteKeysTotal);
        const float blackW = whiteW * 0.55f;
        const float blackH = static_cast<float>(keyboardH_) * 0.60f;

        // Check black keys first (they are on top visually)
        for (int oct = 0; oct < kNumOctaves; ++oct)
        {
            for (int bi = 0; bi < 5; ++bi)
            {
                // Black key 'gap index' — position between white keys
                // Semitone gaps: C#=between C(0) and D(1), D#=between D(1) and E(2),
                // F#=between F(3) and G(4), G#=between G(4) and A(5), A#=between A(5) and B(6)
                static constexpr float kBlackGaps[5] = { 0.7f, 1.7f, 3.7f, 4.7f, 5.7f };
                const float cx = (oct * 7 + kBlackGaps[bi]) * whiteW;
                if (px >= static_cast<int>(cx - blackW * 0.5f) &&
                    px <= static_cast<int>(cx + blackW * 0.5f) &&
                    py <= keyboardY_ + static_cast<int>(blackH))
                {
                    const int semitone = kBlackSemitones[bi];
                    return (baseOctave_ + oct) * 12 + semitone;
                }
            }
        }

        // White keys
        const int whiteIdx = static_cast<int>(static_cast<float>(px) / whiteW);
        if (whiteIdx >= 0 && whiteIdx < kWhiteKeysTotal)
        {
            const int oct     = whiteIdx / 7;
            const int semitone = kWhiteSemitones[whiteIdx % 7];
            return (baseOctave_ + oct) * 12 + semitone;
        }
        return -1;
    }

    void drawKeyboard(juce::Graphics& g) const
    {
        const int W = getWidth();
        const float whiteW = static_cast<float>(W) / static_cast<float>(kWhiteKeysTotal);
        const float blackW = whiteW * 0.55f;
        const float blackH = static_cast<float>(keyboardH_) * 0.60f;
        const float ky = static_cast<float>(keyboardY_);
        const float kh = static_cast<float>(keyboardH_);

        // ── White keys ────────────────────────────────────────────────────────
        for (int oct = 0; oct < kNumOctaves; ++oct)
        {
            for (int wi = 0; wi < 7; ++wi)
            {
                const int semitone = kWhiteSemitones[wi];
                const int midiNote = (baseOctave_ + oct) * 12 + semitone;
                const int pitchClass = midiNote % 12;
                const bool inScale  = scaleNotes_[pitchClass];
                const bool pressed  = (midiNote == pressedNote_);
                const float kx = (oct * 7 + wi) * whiteW;

                juce::Colour fill;
                if (pressed)
                    fill = juce::Colour(0xFF4CDFA8).brighter(0.3f);
                else if (inScale)
                    fill = juce::Colour(0xFF4CDFA8).withAlpha(0.55f);
                else
                    fill = juce::Colour(0xFFDDDDCC);

                g.setColour(fill);
                g.fillRoundedRectangle(kx + 1.f, ky, whiteW - 2.f, kh, 3.f);

                // Key border
                g.setColour(juce::Colour(0xFF333333));
                g.drawRoundedRectangle(kx + 1.f, ky, whiteW - 2.f, kh, 3.f, 0.8f);

                // Note name at bottom
                if (wi == 0)  // C notes
                {
                    const juce::String noteName = juce::MidiMessage::getMidiNoteName(
                        midiNote, true, true, 4);
                    g.setColour(inScale ? juce::Colour(0xFF131314) : juce::Colour(0xFF666666));
                    g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.f)));
                    g.drawFittedText(noteName,
                        juce::Rectangle<int>(static_cast<int>(kx), static_cast<int>(ky + kh - 14),
                                             static_cast<int>(whiteW), 12),
                        juce::Justification::centred, 1);
                }
            }
        }

        // ── Black keys (drawn on top) ─────────────────────────────────────────
        static constexpr float kBlackGaps[5] = { 0.7f, 1.7f, 3.7f, 4.7f, 5.7f };
        for (int oct = 0; oct < kNumOctaves; ++oct)
        {
            for (int bi = 0; bi < 5; ++bi)
            {
                const int semitone   = kBlackSemitones[bi];
                const int midiNote   = (baseOctave_ + oct) * 12 + semitone;
                const int pitchClass = midiNote % 12;
                const bool inScale   = scaleNotes_[pitchClass];
                const bool pressed   = (midiNote == pressedNote_);
                const float cx = (oct * 7 + kBlackGaps[bi]) * whiteW;
                const float kx = cx - blackW * 0.5f;

                juce::Colour fill;
                if (pressed)
                    fill = juce::Colour(0xFF4CDFA8).brighter(0.1f);
                else if (inScale)
                    fill = juce::Colour(0xFF4CDFA8).withAlpha(0.80f);
                else
                    fill = juce::Colour(0xFF111111);

                g.setColour(fill);
                g.fillRoundedRectangle(kx, ky, blackW, blackH, 2.f);

                g.setColour(juce::Colour(0xFF000000).withAlpha(0.5f));
                g.drawRoundedRectangle(kx, ky, blackW, blackH, 2.f, 0.8f);
            }
        }
    }

    // ── Members ───────────────────────────────────────────────────────────────

    juce::ComboBox   scaleCombo_;
    juce::ComboBox   presetCombo_;
    juce::TextButton octaveDownBtn_, octaveUpBtn_;
    juce::Label      octaveLabel_;
    juce::Slider     volumeSlider_;
    juce::Label      volumeLabel_;

    std::array<juce::Slider, kNumParams> paramSliders_;
    std::array<juce::Label,  kNumParams> paramLabels_;

    int         masterKey_   = 0;       // 0=C .. 11=B
    bool        masterMajor_ = true;
    ScaleType   scaleType_   = ScaleType::Major;
    int         baseOctave_  = 4;       // MIDI octave number of left C key
    int         pressedNote_ = -1;      // currently held MIDI note, -1 = none
    int         keyboardY_   = 32;
    int         keyboardH_   = 120;

    std::array<bool, 12> scaleNotes_ {};  // which pitch classes are in the scale

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoKeyboardPanel)
};

} // namespace ui
