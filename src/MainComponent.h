#pragma once

#include <JuceHeader.h>
#include "dsp/DspPipeline.h"
#include "midi/MidiManager.h"
#include "project/ProjectLoader.h"

#include <array>

//==============================================================================
/**
 * MainComponent — composant principal de SaxFX Live
 *
 * Sprint 1 : pass-through pur (entrée Scarlett → sortie casque)
 * Sprint 2 : pipeline DSP (YIN pitch tracking, harmoniseur OLA, flanger)
 * Sprint 3 : sampler 8 slots + MIDI + chargement projet .saxfx
 */
class MainComponent : public juce::AudioAppComponent,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    //==========================================================================
    // AudioAppComponent — boucle audio temps réel
    //==========================================================================
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    //==========================================================================
    // Component — rendu GUI
    //==========================================================================
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    //==========================================================================
    // Timer — mise à jour du VU-mètre et affichage pitch (~30 fps)
    //==========================================================================
    void timerCallback() override;

    //==========================================================================
    // DSP + MIDI
    //==========================================================================
    ::dsp::DspPipeline dspPipeline_;
    midi::MidiManager  midiManager_ { dspPipeline_.getMidiEventQueue() };

    //==========================================================================
    // GUI — contrôles audio
    //==========================================================================
    juce::TextButton audioSettingsButton_;
    juce::Label      statusLabel_;
    juce::Label      infoLabel_;
    juce::Label      pitchLabel_;

    // ── Harmoniseur ───────────────────────────────────────────────────────────
    juce::ToggleButton harmonizerToggle_;
    juce::Slider       harmVoice1Slider_;
    juce::Slider       harmVoice2Slider_;
    juce::Slider       harmMixSlider_;
    juce::Label        harmLabel_;

    // ── Flanger ───────────────────────────────────────────────────────────────
    juce::ToggleButton flangerToggle_;
    juce::Slider       flangerRateSlider_;
    juce::Slider       flangerDepthSlider_;
    juce::Slider       flangerFeedbackSlider_;
    juce::Slider       flangerMixSlider_;
    juce::Label        flangerLabel_;

    // ── Sampler ───────────────────────────────────────────────────────────────
    juce::Label                              samplerLabel_;
    juce::TextButton                         loadProjectButton_;
    std::array<juce::TextButton, 8>          slotButtons_;
    std::array<juce::Label,      8>          slotLabels_;

    //==========================================================================
    // State (shared between audio and GUI threads via atomics)
    //==========================================================================
    std::atomic<float> currentRmsLevel_ { 0.0f };
    double currentSampleRate_ { 0.0 };
    int    currentBufferSize_ { 0 };

    //==========================================================================
    // Helpers
    //==========================================================================
    void setupHarmonizerControls();
    void setupFlangerControls();
    void setupSamplerControls();
    void applyProjectData(const project::ProjectData& data);
    static juce::String frequencyToNoteName(float hz);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
