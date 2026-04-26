#pragma once

#include "dsp/DspPipeline.h"
#include "dsp/SynthEffect.h"
#include "dsp/SmartSamplerEngine.h"
#include "dsp/StepSequencer.h"
#include "midi/MidiManager.h"
#include "project/ProjectLoader.h"
#include "ui/EffectChainEditor.h"
#include "ui/PianoKeyboardPanel.h"
#include "ui/NeonButton.h"
#include "ui/SaxOsLookAndFeel.h"
#include "ui/SaxFXLookAndFeel.h"
#include "ui/SaxStaffPanel.h"
#include "ui/PixelCloudComponent.h"
#include "ui/SampleEditorComponent.h"
#include "ui/SpatialVisualization.h"
#include "ui/StepSequencerPanel.h"

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <future>
#include <vector>

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
    std::unique_ptr<ui::SaxOsLookAndFeel>  saxOsLookAndFeel_;

    // Fenêtre non-modale pour l'éditeur de sample (Fix 1)
    struct SampleEditorWindow : public juce::DocumentWindow
    {
        std::function<void()> onClose;
        SampleEditorWindow(const juce::String& title, juce::Colour bg)
            : juce::DocumentWindow(title, bg, juce::DocumentWindow::closeButton) {}
        void closeButtonPressed() override { if (onClose) onClose(); }
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SampleEditorWindow)
    };
    std::unique_ptr<SampleEditorWindow> sampleEditorWindow_;
    juce::Image logoImage_;  // DubEngine logo (embarqué via BinaryData)
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
    ui::SaxStaffPanel          saxStaffPanel_;
    ui::SpatialVisualization   spatialViz_;
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
    juce::TextButton filesMenuButton_;
    ui::StepSequencerPanel stepSeqPanel_ { stepSequencer_ };

    // ── Sidebar upper: already covered by audioSettingsButton_, mainMixSlider_ ──

    // ── Sidebar lower — transport (moved from StepSequencerPanel) ─────────────
    juce::TextButton        sidebarPlayBtn_;
    juce::TextButton        sidebarTapBtn_;
    juce::Label             sidebarBpmLabel_;
    ui::PixelCloudComponent aiCloud_;
    void                    triggerAI();
    bool                    reloadPending_ { false };       // sample chargé pendant mix actif
    std::array<bool, 9>     manualTypeOverride_ {};         // clic droit → type prioritaire sur rôle fixe

    // ── Sidebar lower — scene navigation ──────────────────────────────────────
    juce::TextButton sceneUpBtn_;
    juce::Label      sceneNumLabel_;
    juce::TextButton sceneDownBtn_;
    juce::TextButton sceneResetBtn_;      // efface les patterns de la scène courante
    juce::TextButton sceneTrackResetBtn_; // full reset : patterns + samples de la scène courante
    juce::TextButton sceneCopyBtn_;

    // ── Scene data ─────────────────────────────────────────────────────────────
    static constexpr int kMaxScenes = 8;

    struct SceneData
    {
        float                                 bpm           { 120.f };
        std::array<std::string, 9>            filePaths     {};
        std::array<std::array<bool, 512>, 9>  steps         {};
        std::array<float, 9>                  gains         { 1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f };
        std::array<bool, 9>                   mutes         {};
        std::array<int, 9>                    trackBarCounts{ 1,1,1,1,1,1,1,1,1 };
        std::array<int, 9>                    trimStart     { 0,0,0,0,0,0,0,0,0 };
        std::array<int, 9>                    trimEnd       { -1,-1,-1,-1,-1,-1,-1,-1,-1 };
        bool                                  used          { false };
    };

    std::array<SceneData, kMaxScenes> scenes_;
    int currentScene_ { 0 };
    std::atomic<int>  pendingScene_ { -1 };  // -1 = aucune transition en attente

    //==========================================================================
    // Background task management
    //==========================================================================
    std::atomic<bool>                               shutdownFlag_{ false };
    std::array<std::atomic<bool>, 9>                processingSlot_{};
    std::vector<std::future<void>>                  backgroundTasks_;

    // Réserve #3 — BPM override + raw-PCM retry storage (GUI thread only)
    float                           overrideBpm_{ 0.f };          // consumed by next async run
    std::array<std::vector<float>, 9> rawPcmForRetry_{};
    std::array<double,             9> rawSrForRetry_{};           // value-initialised to 0

    //==========================================================================
    // Clipboard (copy/paste track)
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
    // Crossfade entre scènes
    //==========================================================================
    struct CrossfadeState {
        bool  active      { false };
        int   elapsedMs   { 0 };
        int   durationMs  { 300 };
        std::array<float, 9> startGains  {};
        std::array<float, 9> targetGains {};
    };
    CrossfadeState crossfade_;

    //==========================================================================
    // State
    //==========================================================================
    int  autosaveTick_     { 0 };
    int  autosaveFadeTimer_{ 0 };
    std::atomic<float> currentRmsLevel_{0.0f};
    std::atomic<float> currentOutputRmsLevel_{0.0f};
    std::atomic<float> outputGain_     {1.0f};
    double             currentSampleRate_{0.0};
    int                currentBufferSize_{0};

    //==========================================================================
    // Helpers
    //==========================================================================
    void loadSampleIntoSlot(int slot, const std::string& path);
    void autoMatchSampleAsync(int slot, std::vector<float> rawPcm, double fileSr);
    void showBpmConfidencePopup(int slot, float detectedBpm);
    void openSampleEditor(int slot);
    static std::vector<float> computeEnvelope(const std::vector<float>& pcm, int bins = 200);
    ::dsp::SynthEffect* findSynthEffect() noexcept;
    void applyProjectData(const project::ProjectData& data);
    void saveProject();
    void saveProjectToFile(const juce::File& f);
    void showFilesMenu();
    void newProject();
    void doLoadProject();
    void openAudioSettings();
    void doAutosave();
    static juce::String frequencyToNoteName(float hz);

    // Scene management
    ::dsp::SmartSamplerEngine::SceneSnapshot buildSceneSnapshot(int si) const;
    void captureCurrentScene();
    void applyScene(int idx);
    void navigateScene(int delta);
    void resetCurrentScene();       // patterns only
    void resetCurrentSceneFull();   // patterns + samples
    void copyCurrentSceneToNext();
    void updateSceneLabel();
    void updateSidebarBpm(float bpm);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
