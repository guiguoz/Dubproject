#pragma once

#include "dsp/DspPipeline.h"
#include "dsp/FeatureExtractor.h"
#include "dsp/LooperEngine.h"
#include "dsp/SceneManager.h"
#include "dsp/SerumHost.h"
#include "dsp/SmartSamplerEngine.h"
#include "dsp/StepSequencer.h"
#include "midi/MidiManager.h"
#include "midi/MidiLearnMap.h"
#include "project/ProjectLoader.h"
#include "ui/NeonButton.h"
#include "ui/SaxOsLookAndFeel.h"
#include "ui/SaxFXLookAndFeel.h"
#include "ui/SampleEditorComponent.h"
#include "ui/ScaleStaffComponent.h"
#include "ui/SpatialVisualization.h"
#include "ui/StepSequencerPanel.h"
#include "ui/PixelCloudComponent.h"

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <future>
#include <vector>

//==============================================================================
class MainComponent : public juce::AudioAppComponent, private juce::Timer
{
  public:
    MainComponent();
    ~MainComponent() override;

    //==========================================================================
    // AudioAppComponent
    //==========================================================================
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    // ── EWI synth (VST3 host) ─────────────────────────────────────────────────
    void loadSerumPlugin(const juce::String& vst3Path);
    void unloadSerumPlugin();
    void openSerumEditor();

    //==========================================================================
    // Component
    //==========================================================================
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    std::unique_ptr<ui::SaxOsLookAndFeel>  saxOsLookAndFeel_;

    struct SampleEditorWindow : public juce::DocumentWindow
    {
        std::function<void()> onClose;
        SampleEditorWindow(const juce::String& title, juce::Colour bg)
            : juce::DocumentWindow(title, bg, juce::DocumentWindow::closeButton) {}
        void closeButtonPressed() override { if (onClose) onClose(); }
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SampleEditorWindow)
    };
    std::unique_ptr<SampleEditorWindow> sampleEditorWindow_;
    juce::Image logoImage_;

    //==========================================================================
    // Timer
    //==========================================================================
    void timerCallback() override;

    //==========================================================================
    // DSP + MIDI
    //==========================================================================
    ::dsp::DspPipeline        dspPipeline_;
    ::dsp::SerumHost          serumHost_;
    ::dsp::SmartSamplerEngine samplerEngine_ { dspPipeline_.getSampler() };
    ::dsp::StepSequencer      stepSequencer_;
    ::dsp::LooperEngine       looperEngine_;
    midi::MidiManager         midiManager_{dspPipeline_.getMidiEventQueue()};
    juce::MidiBuffer          ewiMidiBuffer_;

    // ── Serum gain rider (audio thread only) ─────────────────────────────────
    std::vector<float>       serumSnapBuf_;
    std::vector<float>       serumSnapCopy_;        // GUI thread only — buffer réutilisé pour éviter alloc par tick
    juce::SpinLock           serumSnapLock_;
    bool                     serumSnapReady_       { false };
    std::atomic<float>       serumGainSmooth_      { 1.0f };  // audio thread r/w + GUI r
    std::atomic<float>       serumUserGain_        { 1.0f };  // MIDI learn multiplier (GUI w, audio r)
    std::atomic<::dsp::ContentCategory> serumContentType_ { ::dsp::ContentCategory::SYNTH };
    ::dsp::MixFeatures       serumMixFeatures_ {};   // GUI thread only
    int                      serumAnalysisCounter_ { 0 };

    // ── EWI Synth UI ─────────────────────────────────────────────────────────
    juce::TextButton                    serumLoadBtn_;
    juce::TextButton                    serumShowUiBtn_;
    juce::Label                         serumStatusLabel_;
    juce::TextEditor                    ewiDeviceEditor_;
    std::unique_ptr<juce::FileChooser>  serumFileChooser_;
    std::unique_ptr<SampleEditorWindow> serumEditorWindow_;

    //==========================================================================
    // GUI — contrôles audio
    //==========================================================================
    juce::TextButton audioSettingsButton_;
    juce::Label      infoLabel_;
    juce::Slider     mainMixSlider_;
    juce::Label      mainMixLabel_;
    bool             pipelineActive_ = false;

    // ── LookAndFeel ───────────────────────────────────────────────────────────
    ui::SaxFXLookAndFeel laf_;

    // ── Spatial visualization ─────────────────────────────────────────────────
    ui::SpatialVisualization   spatialViz_;

    // ── Master key ────────────────────────────────────────────────────────────
    int              masterKeyRoot_  { 0 };
    bool             masterKeyMajor_ { true };
    juce::ComboBox   masterKeyCombo_;
    juce::ComboBox   masterKeyModeCombo_;
    void             applyMasterKey();
    void             reApplyCurrentSceneTrims();

    // ── Sampler / Step Sequencer ──────────────────────────────────────────────
    juce::Label samplerLabel_;
    juce::TextButton loadProjectButton_;
    juce::TextButton saveProjectButton_;
    juce::TextButton filesMenuButton_;
    ui::StepSequencerPanel stepSeqPanel_ { stepSequencer_ };

    // ── Sidebar transport ─────────────────────────────────────────────────────
    juce::TextButton        sidebarPlayBtn_;
    juce::TextButton        sidebarTapBtn_;
    juce::Label             sidebarBpmLabel_;
    ui::PixelCloudComponent aiCloud_;
    void                    triggerAI();
    bool                    reloadPending_       { false };
    bool                    trimAfterMixPending_ { false };
    std::array<bool, 9>     manualTypeOverride_ {};

    // ── Dub Delay global bus ───────────────────────────────────────────────────
    juce::ToggleButton dubDelayEnableBtn_;
    juce::Slider       dubDelaySendSlider_;
    juce::Slider       dubDelayWetSlider_;
    juce::Slider       dubDelayFeedbackSlider_;
    juce::Slider       dubDelayToneSlider_;
    juce::Slider       dubDelayDriveSlider_;
    juce::ComboBox     dubDelayDivCombo_;
    juce::TextButton   dubDelayFreezeBtn_;
    juce::Label        dubDelayLabel_;

    // ── Scene navigation ──────────────────────────────────────────────────────
    juce::TextButton sceneUpBtn_;
    juce::Label      sceneNumLabel_;
    juce::TextButton sceneDownBtn_;
    juce::TextButton sceneResetBtn_;
    juce::TextButton sceneTrackResetBtn_;
    juce::TextButton sceneCopyBtn_;

    // ── Scene data ─────────────────────────────────────────────────────────────
    static constexpr int kMaxScenes = ::dsp::SceneManager::kMaxScenes;
    using SceneData = ::dsp::SceneData;

    ::dsp::SceneManager sceneManager_;

    // ── Info musicale (preset Serum + portée gammes) ──────────────────────────
    ui::ScaleStaffComponent scaleStaff_;
    juce::ComboBox          scaleTypeCombo_;
    juce::String            currentPresetName_;
    juce::Rectangle<int>    serumZone_;   // bounds du panel Serum, pour repaint ciblé
    int                     presetNameTick_ { 0 };

    //==========================================================================
    // Background task management
    //==========================================================================
    std::atomic<bool>                               shutdownFlag_{ false };
    std::array<std::atomic<bool>, 9>                processingSlot_{};
    std::vector<std::future<void>>                  backgroundTasks_;

    // ── Loader thread persistant (étape 5 SceneManager) ──────────────────────
    struct PreloadCache {
        std::string        path;
        std::vector<float> pcm;
        double             sampleRate { 44100.0 };
        std::atomic<bool>  ready      { false };
    };
    std::array<PreloadCache, 9> preloadCache_;
    std::atomic<int>            preloadTargetScene_ { -1 };
    void preloadSceneAsync(int targetScene);

    float                           overrideBpm_{ 0.f };
    std::array<std::vector<float>, 9> rawPcmForRetry_{};
    std::array<double,             9> rawSrForRetry_{};

    //==========================================================================
    // Clipboard
    //==========================================================================
    struct TrackClipboard {
        std::array<bool, 512> steps {};
        int         barCount { 1 };
        std::string filePath;
        float       gain     { 1.f };
        bool        muted    { false };
        bool        valid    { false };
    };
    TrackClipboard trackClipboard_;


    //==========================================================================
    // State
    //==========================================================================
    int  autosaveTick_     { 0 };
    int  autosaveFadeTimer_{ 0 };

    // ── Sidechain config cache (évite rebuilds répétés dans onTypesDetected) ───
    int                lastSidechainKick_    { -1 };
    std::array<int, 4> lastSidechainTargets_ {};
    int                lastSidechainCount_   { 0 };
    std::atomic<float> currentRmsLevel_{0.0f};
    std::atomic<float> currentOutputRmsLevel_{0.0f};
    std::atomic<float> outputGain_     {1.0f};
    double             currentSampleRate_{0.0};
    int                currentBufferSize_{0};

    // ── Performance: VU dB cache (U2) ────────────────────────────────────────
    float cachedDbIn_  = -60.f;
    float cachedDbOut_ = -60.f;

    // ── Performance: dirty flags repaint (U3) ────────────────────────────────
    bool vuDirty_        = true;
    bool mixStateDirty_  = true;
    bool crossfadeDirty_ = false;

    // ── Performance: grain noise pre-rendered image (U1) ─────────────────────
    juce::Image grainNoiseImage_;

    //==========================================================================
    // MIDI Learn
    //==========================================================================
    std::vector<midi::MidiLearnBinding>                midiLearnBindings_;
    int                                                 learningTarget_   { -1 };
    bool                                                midiLearnVisible_ { false };
    bool                                                applyingMidi_     { false };
    std::array<float, midi::kNumTargets>                targetSmoothed_   {};

    // UI panel + controls
    juce::Component                                     midiLearnPanel_;
    juce::TextButton                                    midiLearnBtn_;
    std::array<juce::Label,      midi::kNumTargets>     mlTargetLabels_;
    std::array<juce::Label,      midi::kNumTargets>     mlCcLabels_;
    std::array<juce::TextButton, midi::kNumTargets>     mlLearnBtns_;
    std::array<juce::TextButton, midi::kNumTargets>     mlClearBtns_;

    void applyMidiMappings();
    void startLearning(int targetIdx);
    void stopLearning();
    void clearMapping(int targetIdx);
    void updateMidiLearnUI();
    void applyMappingValue(midi::MappingTarget t, float rawValue);

    //==========================================================================
    // Helpers
    //==========================================================================
    void ensureGrainNoise(int w, int h);
    void loadSampleIntoSlot(int slot, const std::string& path,
                            int trimStart = 0, int trimEnd = -1);
    void autoMatchSampleAsync(int slot, std::vector<float> rawPcm, double fileSr);
    void showBpmConfidencePopup(int slot, float detectedBpm);
    void openSampleEditor(int slot);
    static std::vector<float> computeEnvelope(const std::vector<float>& pcm, int bins = 200);
    void applyProjectData(const project::ProjectData& data);
    void saveProject();
    void saveProjectToFile(const juce::File& f);
    void showFilesMenu();
    void newProject();
    void doLoadProject();
    void openAudioSettings();
    void doAutosave();

    // Scene management
    ::dsp::SmartSamplerEngine::SceneSnapshot buildSceneSnapshot(int si) const;
    void captureCurrentScene();
    void applyScene(int idx, int fromIdx = -1);
    void navigateScene(int delta);
    void resetCurrentScene();
    void resetCurrentSceneFull();
    void copyCurrentSceneToNext();
    void updateSceneLabel();
    void updateSidebarBpm(float bpm);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
