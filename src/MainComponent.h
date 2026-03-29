#pragma once

#include "dsp/DspPipeline.h"
#include "dsp/SynthEffect.h"
#include "dsp/SmartSamplerEngine.h"
#include "dsp/StepSequencer.h"
#include "midi/MidiManager.h"
#include "project/ProjectLoader.h"
#include "ui/EffectChainEditor.h"
#include "ui/PianoKeyboardPanel.h"
#include "ui/SaxOsLookAndFeel.h"
#include "ui/SaxFXLookAndFeel.h"
#include "ui/SaxStaffPanel.h"
#include "ui/StepSequencerPanel.h"

#include <JuceHeader.h>

//==============================================================================
/**
 * MainComponent — composant principal de SaxFX Live
 *
 * Sprint 7 : séquenceur à steps (8 tracks × 16 steps) inspiré Novation Circuit Tracks.
 *            Remplace SamplerPanel + MasterSampleSelector + SamplerMagicButton.
 */
class MainComponent : public juce::AudioAppComponent, private juce::Timer
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
    std::unique_ptr<ui::SaxOsLookAndFeel> saxOsLookAndFeel_;
    //==========================================================================
    // Timer — mise à jour du VU-mètre et affichage pitch (~30 fps)
    //==========================================================================
    void timerCallback() override;

    //==========================================================================
    // DSP + MIDI
    //==========================================================================
    ::dsp::DspPipeline       dspPipeline_;
    ::dsp::SmartSamplerEngine samplerEngine_ { dspPipeline_.getSampler() };
    ::dsp::StepSequencer     stepSequencer_;
    midi::MidiManager        midiManager_{dspPipeline_.getMidiEventQueue()};

    //==========================================================================
    // GUI — contrôles audio
    //==========================================================================
    juce::TextButton audioSettingsButton_;
    juce::Label      infoLabel_;
    juce::Label      pitchLabel_;
    juce::Slider     mainMixSlider_;
    juce::Label      mainMixLabel_;
    bool             pipelineActive_ = false;

    // ── LookAndFeel ───────────────────────────────────────────────────────────
    ui::SaxFXLookAndFeel laf_;

    // ── Modular Effects ──────────────────────────────────────────────────────
    ui::EffectChainEditor  effectChainEditor_{dspPipeline_.getEffectChain()};
    ui::PianoKeyboardPanel pianoKeyboardPanel_;
    ui::SaxStaffPanel      saxStaffPanel_;
    juce::Label fxLabel_;

    // ── View switching ────────────────────────────────────────────────────────
    enum class ViewMode { Effects, Keyboard, Staff };
    ViewMode         currentViewMode_ { ViewMode::Effects };
    juce::TextButton viewKeyboardBtn_;  // 🎹
    juce::TextButton viewStaffBtn_;     // 🎼
    void             updateViewVisibility();

    // ── Master key (tonalité de référence) ───────────────────────────────────
    int              masterKeyRoot_  { 0 };     // 0=C .. 11=B
    bool             masterKeyMajor_ { true };
    juce::ComboBox   masterKeyCombo_;
    juce::ComboBox   masterKeyModeCombo_;
    juce::ComboBox   globalScaleCombo_;         // gamme (partagé clavier + portée)
    void             applyMasterKey();          // propagates to panels + MusicContext

    // ── Sampler / Step Sequencer ──────────────────────────────────────────────
    juce::Label samplerLabel_;
    juce::TextButton loadProjectButton_;
    juce::TextButton saveProjectButton_;
    ui::StepSequencerPanel stepSeqPanel_ { stepSequencer_ };

    // ── Sidebar upper: already covered by audioSettingsButton_, mainMixSlider_ ──

    // ── Sidebar lower — transport (moved from StepSequencerPanel) ─────────────
    juce::TextButton sidebarPlayBtn_;
    juce::TextButton sidebarTapBtn_;
    juce::Label      sidebarBpmLabel_;
    juce::TextButton sidebarMagicBtn_;

    // ── Sidebar lower — scene navigation ──────────────────────────────────────
    juce::TextButton sceneUpBtn_;
    juce::Label      sceneNumLabel_;
    juce::TextButton sceneDownBtn_;
    juce::TextButton sceneResetBtn_;
    juce::TextButton sceneCopyBtn_;

    // ── Scene data ─────────────────────────────────────────────────────────────
    static constexpr int kMaxScenes = 8;

    struct SceneData
    {
        float                                bpm      { 120.f };
        std::array<std::string, 8>           filePaths {};
        std::array<std::array<bool, 16>, 8>  steps    {};
        std::array<float, 8>                 gains    { 1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f };
        std::array<bool, 8>                  mutes    {};
        bool                                 used     { false };
    };

    std::array<SceneData, kMaxScenes> scenes_;
    int currentScene_ { 0 };

    //==========================================================================
    // State
    //==========================================================================
    std::atomic<float> currentRmsLevel_{0.0f};
    std::atomic<float> currentOutputRmsLevel_{0.0f};
    std::atomic<float> outputGain_     {1.0f};
    double             currentSampleRate_{0.0};
    int                currentBufferSize_{0};

    //==========================================================================
    // Helpers
    //==========================================================================
    void loadSampleIntoSlot(int slot, const std::string& path);
    void pitchMatchSampleAsync(int slot, std::vector<float> rawPcm, double fileSr);
    ::dsp::SynthEffect* findSynthEffect() noexcept;
    void applyProjectData(const project::ProjectData& data);
    void saveProject();
    static juce::String frequencyToNoteName(float hz);

    // Scene management
    void captureCurrentScene();
    void applyScene(int idx);
    void navigateScene(int delta);
    void resetCurrentScene();
    void copyCurrentSceneToNext();
    void updateSceneLabel();
    void updateSidebarBpm(float bpm);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
