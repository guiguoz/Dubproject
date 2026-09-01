#pragma once

#include "engine/EngineFacade.h"

#include "engine/Analysis/FeatureExtractor.h"
#include "engine/SceneStore.h"
#include "dsp/SerumHost.h"
#include "midi/MidiManager.h"
#include "midi/MidiLearnMap.h"
#include "project/ProjectLoader.h"
#include "ui/SaxOsLookAndFeel.h"
#include "ui/SampleEditorComponent.h"
#include "ui/ScaleStaffComponent.h"
#include "ui/SpatialVisualization.h"
#include "ui/StepSequencerPanel.h"
#include "ui/PixelCloudComponent.h"

#include <JuceHeader.h>
#include <atomic>
#include <array>
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
    void openSerumEditor();

    // ── Panic : coupe tous les slots + delays instantanément ─────────────────
    void triggerPanic() noexcept;

    //==========================================================================
    // Component
    //==========================================================================
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
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
    // Moteur V2
    //==========================================================================
    engine::EngineFacade facade_;
    // Buffers de capture de l'entrée dry (EWI/sax) — le rendu V2 écrase left/right.
    std::vector<float>       v2InputScratchL_, v2InputScratchR_;
    std::atomic<float>       v2InputGain_ { 0.8f };

    // Sync sceneStore_ → EngineFacade sceneStore (gains, semitones, rôles, actifs)
    // puis application au graphe (setCurrentScene) pour les scènes/transitions.
    engine::SlotRole v2RoleForSlot(int slot) const noexcept;
    // Rôle effectif d'un slot : analysé V2 si fiable, sinon fallback par défaut.
    engine::SlotRole activeRoleForSlot(int slot) const noexcept;
    void             syncV2Scene(int idx) noexcept;
    void             syncV2Scenes() noexcept;

    //==========================================================================
    // DSP + MIDI
    //==========================================================================
    ::dsp::SerumHost          serumHost_;
    midi::MidiManager         midiManager_{ [this](int slot, bool noteOn) {
        if (noteOn) facade_.triggerSlot(slot);
        else        facade_.stopSlot(slot);
    } };
    juce::MidiBuffer          ewiMidiBuffer_;

    // ── Serum gain rider (audio thread only) ─────────────────────────────────
    std::vector<float>       serumSnapBuf_;
    std::vector<float>       serumSnapCopy_;        // GUI thread only — buffer réutilisé pour éviter alloc par tick
    juce::SpinLock           serumSnapLock_;
    bool                     serumSnapReady_       { false };
    std::atomic<float>       serumGainSmooth_      { 1.0f };  // audio thread r/w + GUI r
    std::atomic<float>       serumUserGain_        { 1.0f };  // MIDI learn multiplier (GUI w, audio r)
    std::atomic<engine::analysis::ContentCategory> serumContentType_ { engine::analysis::ContentCategory::SYNTH };
    engine::analysis::MixFeatures                  serumMixFeatures_ {};   // GUI thread only
    int                      serumAnalysisCounter_ { 0 };

    // ── EWI Synth UI ─────────────────────────────────────────────────────────
    juce::TextButton                    serumLoadBtn_;
    juce::TextButton                    swamLoadBtn_;
    juce::TextButton                    serumShowUiBtn_;
    juce::Label                         serumStatusLabel_;
    juce::TextEditor                    ewiDeviceEditor_;
    std::unique_ptr<SampleEditorWindow> serumEditorWindow_;

    //==========================================================================
    // GUI — contrôles audio
    //==========================================================================
    juce::Label      infoLabel_;
    juce::Slider     mainMixSlider_;
    juce::Label      mainMixLabel_;
    bool             pipelineActive_ = false;

    // ── LookAndFeel ───────────────────────────────────────────────────────────

    // ── Spatial visualization ─────────────────────────────────────────────────
    ui::SpatialVisualization   spatialViz_;

    // ── Master key ────────────────────────────────────────────────────────────
    int              masterKeyRoot_      { -1 }; // -1 = "Aucune" (sentinelle — aucune tonalité définie)
    bool             masterKeyMajor_     { true };
    bool             masterKeySetByUser_ { false };  // true only after explicit UI selection
    juce::ComboBox   masterKeyCombo_;
    juce::ComboBox   masterKeyModeCombo_;
    void             applyMasterKey();

    // ── Sampler / Step Sequencer ──────────────────────────────────────────────
    juce::Label samplerLabel_;
    juce::TextButton filesMenuButton_;
    std::unique_ptr<ui::StepSequencerPanel> stepSeqPanel_;

    // ── Sidebar transport ─────────────────────────────────────────────────────
    juce::TextButton        sidebarPlayBtn_;
    juce::TextButton        sidebarTapBtn_;
    juce::Label             sidebarBpmLabel_;
    ui::PixelCloudComponent aiCloud_;
    void                    triggerAI();

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
    static constexpr int kMaxScenes = ::engine::kMaxScenes;
    using SceneData = ::engine::SceneData;

    ::engine::SceneStore sceneStore_;

    // ── Info musicale (preset Serum + portée gammes) ──────────────────────────
    ui::ScaleStaffComponent scaleStaff_;
    juce::ComboBox          scaleTypeCombo_;
    juce::String            currentPresetName_;
    juce::Rectangle<int>    serumZone_;   // bounds du panel Serum, pour repaint ciblé
    int                     presetNameTick_ { 0 };

    //==========================================================================
    // Mini-rampe Serum post-purge (rampait les gains sampler avant).
    struct SerumGainRamp {
        bool  active     = false;
        float from       = 1.f;
        float to         = 1.f;
        int   elapsedMs  = 0;
        int   durationMs = 250;
    };
    SerumGainRamp serumGainRamp_;
    // Trim appliqué en coordonnées fichier par slot (évite le double-trim en scène identique)
    std::array<int,  9>         appliedTrimStart_ {};
    std::array<int,  9>         appliedTrimEnd_   {};

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
    int  autosaveSlot_     { 0 };
    bool panicArmed_       { false };
    uint8_t panicCC_       { 64 };   // CC#64 (sustain) par défaut — FCB1010 footswitch

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

    // ── Performance: grain noise pre-rendered image (U1) ─────────────────────
    juce::Image grainNoiseImage_;

    //==========================================================================
    // MIDI Learn
    //==========================================================================
    std::vector<midi::MidiLearnBinding>                midiLearnBindings_;
    int                                                 learningTarget_   { -1 };
    bool                                                midiLearnVisible_ { false };
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
    void loadSampleIntoSlot(int slot, const std::string& path);
    void updateSpatialSlot(int slot);
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
    void captureCurrentScene();
    void applyScene(int idx, int fromIdx = -1);
    void navigateScene(int delta);
    void resetCurrentScene();
    void resetCurrentSceneFull();
    void copyCurrentSceneToNext();
    void updateSceneLabel();
    void updateSidebarBpm(float bpm);
    void applyDubDelayMorph(float t);
    void onPitchOffsetChanged(int slot, float semitones);

    // Mapping ContentCategory (Serum) → MixContentType (mix V2) pour le contexte
    // de compensation de masquage (étape 9 / M9).
    engine::mix::MixContentType serumContentTypeForMix() const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
