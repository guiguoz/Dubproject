#include "MainComponent.h"

#include "dsp/BpmDetector.h"
#include "dsp/DelayEffect.h"
#include "dsp/FeatureExtractor.h"
#include "dsp/FlangerEffect.h"
#include "dsp/HarmonizerEffect.h"
#include "dsp/KeyDetector.h"
#include "dsp/OctaverEffect.h"
#include "dsp/SynthEffect.h"
#include "dsp/WsolaShifter.h"

#include <cmath>
#include <future>
#include <memory>

//==============================================================================
MainComponent::MainComponent()
{
    // Step 2: apply SAX-OS neon LookAndFeel if available
    saxOsLookAndFeel_ = std::make_unique<ui::SaxOsLookAndFeel>();
    if (saxOsLookAndFeel_)
        setLookAndFeel(saxOsLookAndFeel_.get());

    // Charger le logo depuis les ressources embarquées
    logoImage_ = juce::ImageCache::getFromMemory(
        BinaryData::logo_png, BinaryData::logo_pngSize);

    // ── Audio settings button ─────────────────────────────────────────────────
    audioSettingsButton_.setButtonText("AUDIO SETTINGS");
    audioSettingsButton_.onClick = [this]
    {
        auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager, 0, 1, 0, 2, true,
                                                                false, false, false);
        selector->setSize(500, 400);

        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(selector);
        options.dialogTitle = "Audio Device Settings";
        options.dialogBackgroundColour = juce::Colours::darkgrey;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    };
    addAndMakeVisible(audioSettingsButton_);

    // ── Status / info labels ──────────────────────────────────────────────────
    infoLabel_.setJustificationType(juce::Justification::centred);
    infoLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(9.5f)));
    infoLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible(infoLabel_);

    pitchLabel_.setJustificationType(juce::Justification::centred);
    pitchLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(16.0f).withStyle("Bold")));
    pitchLabel_.setColour(juce::Label::textColourId, juce::Colours::cyan);
    pitchLabel_.setText("--", juce::dontSendNotification);
    addAndMakeVisible(pitchLabel_);

    // ── Main mix slider (output gain) ─────────────────────────────────────────
    mainMixSlider_.setSliderStyle(juce::Slider::LinearVertical);
    mainMixSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    mainMixSlider_.setRange(0.0, 1.5, 0.01);
    mainMixSlider_.setValue(1.0, juce::dontSendNotification);
    mainMixSlider_.onValueChange = [this]
    {
        outputGain_.store(static_cast<float>(mainMixSlider_.getValue()),
                          std::memory_order_relaxed);
    };
    addAndMakeVisible(mainMixSlider_);

    mainMixLabel_.setText("MAIN MIX", juce::dontSendNotification);
    mainMixLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(9.5f).withStyle("Bold")));
    mainMixLabel_.setJustificationType(juce::Justification::centred);
    mainMixLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(mainMixLabel_);

    // ── Effect chain ──────────────────────────────────────────────────────────
    fxLabel_.setText("EFFECT CHAIN", juce::dontSendNotification);
    fxLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(10.0f).withStyle("Bold")));
    fxLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFFE5E2E3).withAlpha(0.85f));
    addAndMakeVisible(fxLabel_);
    addAndMakeVisible(effectChainEditor_);

    auto& chain = dspPipeline_.getEffectChain();
    {
        auto harm = std::make_unique<::dsp::HarmonizerEffect>();
        harm->setParam(2, 0.15f);  // Mix: 50% → 15% (live-safe)
        chain.addEffect(std::move(harm));
    }
    {
        auto flan = std::make_unique<::dsp::FlangerEffect>();
        flan->setParam(3, 0.15f);  // Mix: 50% → 15% (live-safe)
        chain.addEffect(std::move(flan));
    }
    {
        auto del = std::make_unique<::dsp::DelayEffect>();
        del->enabled.store(false);  // Delay OFF par défaut (500ms = injouable en live)
        chain.addEffect(std::move(del));
    }
    {
        auto oct = std::make_unique<::dsp::OctaverEffect>();
        oct->enabled.store(false);  // Octaver OFF par défaut (réduit latence WSOLA)
        chain.addEffect(std::move(oct));
    }
    {
        auto synth = std::make_unique<::dsp::SynthEffect>();
        synth->enabled.store(false, std::memory_order_relaxed);  // OFF — activé par clavier
        synth->setParam(7, 1.0f);   // 100% wet : full synth sans dry
        chain.addEffect(std::move(synth));
    }

    // ── Step Sequencer label ──────────────────────────────────────────────────
    samplerLabel_.setText("RHYTHM MATRIX", juce::dontSendNotification);
    samplerLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(10.0f).withStyle("Bold")));
    samplerLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFFE5E2E3).withAlpha(0.85f));
    addAndMakeVisible(samplerLabel_);

    // ── Sidebar lower: scene navigation ───────────────────────────────────────
    // No explicit colour override → SaxOsLookAndFeel handles styling uniformly
    auto styleSideBtn = [](juce::TextButton& b, const juce::String& text)
    {
        b.setButtonText(text);
    };

    styleSideBtn(sceneUpBtn_,    juce::CharPointer_UTF8("\xe2\x96\xb2"));  // ▲
    styleSideBtn(sceneDownBtn_,  juce::CharPointer_UTF8("\xe2\x96\xbc"));  // ▼
    styleSideBtn(sceneResetBtn_,      "RST SCN");
    styleSideBtn(sceneTrackResetBtn_, "RST TRK");
    styleSideBtn(sceneCopyBtn_,       "COPY");

    sceneUpBtn_  .onClick = [this] { navigateScene(-1); };
    sceneDownBtn_.onClick = [this] { navigateScene(+1); };
    sceneCopyBtn_.onClick = [this] { copyCurrentSceneToNext(); };

    sceneResetBtn_.onClick = [this]
    {
        juce::AlertWindow::showOkCancelBox(
            juce::AlertWindow::WarningIcon,
            "Reset Scene",
            "Effacer les patterns de la scene " + juce::String(currentScene_ + 1) + " ?\n"
            "(les samples restent charges)",
            "Reset", "Annuler", nullptr,
            juce::ModalCallbackFunction::create([this](int result)
            {
                if (result == 1) resetCurrentScene();
            }));
    };

    sceneTrackResetBtn_.onClick = [this]
    {
        juce::AlertWindow::showOkCancelBox(
            juce::AlertWindow::WarningIcon,
            "Full Reset",
            "Effacer patterns ET samples de la scene " + juce::String(currentScene_ + 1) + " ?\n"
            "Cette action est irreversible.",
            "Reset complet", "Annuler", nullptr,
            juce::ModalCallbackFunction::create([this](int result)
            {
                if (result == 1) resetCurrentSceneFull();
            }));
    };

    sceneNumLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(13.f).withStyle("Bold")));
    sceneNumLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFFFFCC44));
    sceneNumLabel_.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(sceneUpBtn_);
    addAndMakeVisible(sceneNumLabel_);
    addAndMakeVisible(sceneDownBtn_);
    addAndMakeVisible(sceneResetBtn_);
    addAndMakeVisible(sceneTrackResetBtn_);
    addAndMakeVisible(sceneCopyBtn_);
    updateSceneLabel();

    // ── Sidebar lower: transport (▶/■, TAP, BPM, ⚡) ─────────────────────────
    sidebarPlayBtn_.setButtonText(juce::CharPointer_UTF8("\xe2\x96\xb6"));  // ▶
    // Keep accent-green text for semantic play identity, LAF handles background
    sidebarPlayBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF4CDFA8));
    sidebarPlayBtn_.setColour(juce::TextButton::textColourOnId,  juce::Colour(0xFF4CDFA8));
    sidebarPlayBtn_.onClick = [this] { stepSeqPanel_.triggerPlay(); };

    sidebarTapBtn_.setButtonText("TAP TEMPO");
    // LAF background, accent text to indicate tempo function
    sidebarTapBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF4CDFA8));
    sidebarTapBtn_.onClick = [this] { stepSeqPanel_.triggerTap(); };

    sidebarBpmLabel_.setText("120.00", juce::dontSendNotification);
    sidebarBpmLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(34.f).withStyle("Bold")));
    sidebarBpmLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFFE5E2E3));
    sidebarBpmLabel_.setJustificationType(juce::Justification::centred);
    sidebarBpmLabel_.setEditable(false, true, false);
    sidebarBpmLabel_.onEditorHide = [this]
    {
        const juce::String raw = sidebarBpmLabel_.getText().retainCharacters("0123456789.");
        const float bpm = raw.getFloatValue();
        if (bpm >= 40.f && bpm <= 240.f)
        {
            stepSequencer_.setBpm(bpm);
            dspPipeline_.setBpm(bpm);
            stepSeqPanel_.setBpm(bpm);
            updateSidebarBpm(bpm);
        }
        else
        {
            updateSidebarBpm(stepSeqPanel_.getBpm());
        }
    };

    sidebarMagicBtn_.setButtonText(juce::CharPointer_UTF8("\xe2\x9a\xa1"));  // ⚡
    sidebarMagicBtn_.setTooltip("Re-analyser le mix IA (re-clic = annuler)");
    sidebarMagicBtn_.setClickingTogglesState(true);
    sidebarMagicBtn_.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF2A1C00));
    sidebarMagicBtn_.setColour(juce::TextButton::buttonOnColourId,  juce::Colour(0xFF886600));
    sidebarMagicBtn_.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFFFCC44));
    sidebarMagicBtn_.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFFFFEE88));
    sidebarMagicBtn_.setAccentColour(juce::Colour(0xFFFFAA00));  // amber glow
    sidebarMagicBtn_.onClick = [this] { samplerEngine_.toggleMagicMix(); };

    // AI status indicator
    aiStatusLabel_.setText("", juce::dontSendNotification);
    aiStatusLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(10.f).withStyle("Bold")));
    aiStatusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFF4CDFA8));
    aiStatusLabel_.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(sidebarPlayBtn_);
    addAndMakeVisible(sidebarTapBtn_);
    addAndMakeVisible(sidebarBpmLabel_);
    addAndMakeVisible(sidebarMagicBtn_);
    addAndMakeVisible(aiStatusLabel_);

    // ── Project buttons ───────────────────────────────────────────────────────
    saveProjectButton_.setButtonText("SAVE PROJECT");
    saveProjectButton_.onClick = [this] { saveProject(); };
    addAndMakeVisible(saveProjectButton_);

    loadProjectButton_.setButtonText("LOAD PROJECT");
    loadProjectButton_.onClick = [this]
    {
        auto chooser =
            std::make_shared<juce::FileChooser>("Open SaxFX Project", juce::File{}, "*.saxfx");

        chooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, chooser](const juce::FileChooser& fc)
            {
                const auto results = fc.getResults();
                if (results.isEmpty()) return;

                auto data =
                    project::ProjectLoader::load(results[0].getFullPathName().toStdString());
                if (data.has_value())
                    applyProjectData(*data);
                else
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        "Load failed", "Could not parse the .saxfx file.");
            });
    };
    addAndMakeVisible(loadProjectButton_);

    // ── Step Sequencer panel ──────────────────────────────────────────────────

    // File loaded: read PCM, pitch-match if needed, push to Sampler
    stepSeqPanel_.onSlotFileLoaded = [this](int slot, std::string path)
    {
        // Read PCM
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();
        const juce::File file { juce::String(path) };
        std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(file));
        if (!reader) return;

        const int numSamples = static_cast<int>(reader->lengthInSamples);
        if (numSamples <= 0) return;

        juce::AudioBuffer<float> buf(1, numSamples);
        reader->read(&buf, 0, numSamples, 0, true, false);
        std::vector<float> pcm(buf.getReadPointer(0),
                               buf.getReadPointer(0) + numSamples);
        const double fileSr = static_cast<double>(reader->sampleRate);

        samplerEngine_.setSlotFilePath(slot, path);
        autoMatchSampleAsync(slot, std::move(pcm), fileSr);

        // Auto-trigger AI mix analysis as soon as a sample is loaded
        samplerEngine_.applyMagicMix();
        aiStatusLabel_.setText("IA...", juce::dontSendNotification);
    };

    // Step toggled: stop sample immediately if the track has no more active steps
    stepSeqPanel_.onStepChanged = [this](int track, int /*step*/, bool active)
    {
        if (active) return;
        const int nSteps = stepSequencer_.getTrackStepCount(track);
        for (int s = 0; s < nSteps; ++s)
            if (stepSequencer_.getStep(track, s)) return;  // at least one step still on
        dspPipeline_.getSampler().stop(track);
    };

    // Slot cleared: unload PCM and clear engine path
    stepSeqPanel_.onSlotCleared = [this](int slot)
    {
        dspPipeline_.getSampler().clearSlot(slot);
        samplerEngine_.setSlotFilePath(slot, "");
    };

    // BPM changed: update sequencer + DSP + sidebar label
    stepSeqPanel_.onBpmChanged = [this](float bpm)
    {
        stepSequencer_.setBpm(bpm);
        dspPipeline_.setBpm(bpm);
        auto ctx = effectChainEditor_.getMusicContext();
        ctx.bpm  = bpm;
        effectChainEditor_.setMusicContext(ctx);
        updateSidebarBpm(bpm);
    };

    // Play/stop — StepSequencerPanel already calls seq_.setPlaying(); stop samples immediately
    stepSeqPanel_.onPlayChanged = [this](bool playing)
    {
        if (!playing)
        {
            dspPipeline_.getSampler().stopAllSlots();
            // Appliquer toute transition en attente immédiatement au stop
            const int pending = pendingScene_.exchange(-1, std::memory_order_relaxed);
            if (pending >= 0)
            {
                captureCurrentScene();
                currentScene_ = pending;
                applyScene(currentScene_);
                updateSceneLabel();
                stepSequencer_.setPendingTransitionLen(0);
            }
        }
    };

    // Volume per slot
    stepSeqPanel_.onVolumeChanged = [this](int slot, float gain)
    {
        dspPipeline_.getSampler().setSlotGain(slot, gain);
    };

    // Mute per slot — re-run magic mix if active so gain/pan adapt to new mute state
    stepSeqPanel_.onMutedChanged = [this](int slot, bool muted)
    {
        dspPipeline_.getSampler().setSlotMuted(slot, muted);
        if (samplerEngine_.isMagicActive())
            samplerEngine_.applyMagicMix();
    };

    // Magic Mix ⚡ — le callback du panel (backup, au cas où) n'est plus utilisé pour le toggle
    stepSeqPanel_.onMagicButtonPressed = nullptr;

    // Mise à jour des tags de type dans l'UI quand la détection est terminée
    samplerEngine_.onTypesDetected = [this]
    {
        // Per-slot content-type accent colours (mirrors StepSequencerPanel::trackColour)
        static const juce::Colour kSlotColours[8] = {
            juce::Colour { 0xFF4CDFA8 }, juce::Colour { 0xFF06B6D4 },
            juce::Colour { 0xFFC8C7C7 }, juce::Colour { 0xFF8B5CF6 },
            juce::Colour { 0xFFF97316 }, juce::Colour { 0xFFF43F5E },
            juce::Colour { 0xFFEAB308 }, juce::Colour { 0xFF38BDF8 },
        };

        for (int i = 0; i < 8; ++i)
        {
            const auto type = samplerEngine_.getDetectedType(i);
            const bool loaded = dspPipeline_.getSampler().isLoaded(i);
            if (loaded)
                stepSeqPanel_.setSlotContentType(
                    i, ::dsp::SmartSamplerEngine::contentTypeName(type));

            // Spatial visualization — pan/width/depth are set by SmartSamplerEngine
            // after applyNeutronMix(); we just update the visual state here.
            // Note: pan/width/depth are heuristic values from computeSpatialization().
            // We reconstruct them from the sampler's stored gains for display.
            // Simple approach: use content-type defaults (matches what was applied).
            const ::dsp::SmartSamplerEngine::ContentType ct =
                samplerEngine_.getDetectedType(i);
            const auto sp = ::dsp::SmartSamplerEngine::spatialForType(i, ct);
            spatialViz_.setSlotState(i, sp.pan, sp.width, sp.depth,
                                     loaded, kSlotColours[i]);
        }
        spatialViz_.setSaxActive(true);
        stepSeqPanel_.setMagicActive(true);
        sidebarMagicBtn_.setToggleState(true, juce::dontSendNotification);
        aiStatusLabel_.setText(juce::CharPointer_UTF8("IA \xe2\x9c\x93"), juce::dontSendNotification);  // "IA ✓"
    };

    // onDone : si revert → effacer les tags et désactiver les toggles visuels
    samplerEngine_.onDone = [this]
    {
        if (!samplerEngine_.isMagicActive())
        {
            for (int i = 0; i < 8; ++i)
                stepSeqPanel_.setSlotContentType(i, "");
            stepSeqPanel_.setMagicActive(false);
            sidebarMagicBtn_.setToggleState(false, juce::dontSendNotification);
            aiStatusLabel_.setText("", juce::dontSendNotification);
            spatialViz_.resetAll();
        }
    };

    // Override manuel du type par slot (right-click sur l'indicateur)
    stepSeqPanel_.onTypeOverrideChanged = [this](int slot, int typeIndex)
    {
        if (typeIndex < 0)
            samplerEngine_.clearTypeOverride(slot);
        else
            samplerEngine_.setTypeOverride(
                slot, static_cast<::dsp::SmartSamplerEngine::ContentType>(typeIndex));
    };

    // Éditeur de sample (bouton [ED])
    stepSeqPanel_.onEditPressed = [this](int slot) { openSampleEditor(slot); };

    // Changement de longueur de pattern via le menu pageLabel_
    stepSeqPanel_.onTrackBarCountChanged = [this](int track, int bars)
    {
        stepSequencer_.setTrackBarCount(track, bars);
    };

    // Playhead ratio for waveform animation (approx — audio thread value, GUI read)
    stepSeqPanel_.getSlotPlayhead = [this](int slot) -> float
    {
        return dspPipeline_.getSampler().getSlotPlayheadRatio(slot);
    };

    // VU meter — real per-slot output peak from audio thread (reflects gain/mute)
    stepSeqPanel_.getSlotLevel = [this](int slot) -> float
    {
        return dspPipeline_.getSampler().getSlotOutputPeak(slot);
    };

    // Solo per slot
    stepSeqPanel_.onSoloChanged = [this](int slot, bool soloed)
    {
        if (soloed)
            dspPipeline_.getSampler().setSoloSlot(slot);
        else
            dspPipeline_.getSampler().clearSolo();
    };

    // Provide the ducking gain to the UI (1.0 = normal, 0.5 = -6dB)
    stepSeqPanel_.getDuckingGain = [this]() -> float
    {
        return dspPipeline_.getCurrentDuckingGain();
    };

    addAndMakeVisible(stepSeqPanel_);

    // ── Master key selector (sidebar) ─────────────────────────────────────────
    static const char* kNoteNames[12] = {
        "C","C#","D","Eb","E","F","F#","G","Ab","A","Bb","B"
    };
    for (int i = 0; i < 12; ++i)
        masterKeyCombo_.addItem(kNoteNames[i], i + 1);
    masterKeyCombo_.setSelectedId(1, juce::dontSendNotification);
    masterKeyCombo_.onChange = [this] {
        masterKeyRoot_ = masterKeyCombo_.getSelectedId() - 1;
        applyMasterKey();
    };
    addAndMakeVisible(masterKeyCombo_);

    masterKeyModeCombo_.addItem("Major", 1);
    masterKeyModeCombo_.addItem("Minor", 2);
    masterKeyModeCombo_.setSelectedId(1, juce::dontSendNotification);
    masterKeyModeCombo_.onChange = [this] {
        masterKeyMajor_ = (masterKeyModeCombo_.getSelectedId() == 1);
        applyMasterKey();
    };
    addAndMakeVisible(masterKeyModeCombo_);

    // ── Global scale selector (sidebar) ──────────────────────────────────────
    globalScaleCombo_.addItem("Major",            1);
    globalScaleCombo_.addItem("Minor",            2);
    globalScaleCombo_.addItem("Pentatonique Maj", 3);
    globalScaleCombo_.addItem("Pentatonique Min", 4);
    globalScaleCombo_.addItem("Blues",            5);
    globalScaleCombo_.addItem("Dorian",           6);
    globalScaleCombo_.setSelectedId(1, juce::dontSendNotification);
    globalScaleCombo_.onChange = [this] {
        const auto st = static_cast<ui::PianoKeyboardPanel::ScaleType>(
                            globalScaleCombo_.getSelectedId() - 1);
        pianoKeyboardPanel_.setScaleType(st);
        saxStaffPanel_     .setScaleType(st);
    };
    addAndMakeVisible(globalScaleCombo_);

    // ── View switch buttons ───────────────────────────────────────────────────
    viewKeyboardBtn_.setButtonText(juce::CharPointer_UTF8("\xf0\x9f\x8e\xb9"));  // 🎹
    viewKeyboardBtn_.setClickingTogglesState(false);
    viewKeyboardBtn_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF131314));
    viewKeyboardBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF4CDFA8));
    viewKeyboardBtn_.onClick = [this] {
        currentViewMode_ = (currentViewMode_ == ViewMode::Keyboard)
                           ? ViewMode::Effects : ViewMode::Keyboard;
        updateViewVisibility();
    };
    addAndMakeVisible(viewKeyboardBtn_);

    viewStaffBtn_.setButtonText(juce::CharPointer_UTF8("\xf0\x9f\x8e\xbc"));  // 🎼
    viewStaffBtn_.setClickingTogglesState(false);
    viewStaffBtn_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF131314));
    viewStaffBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF4CDFA8));
    viewStaffBtn_.onClick = [this] {
        currentViewMode_ = (currentViewMode_ == ViewMode::Staff)
                           ? ViewMode::Effects : ViewMode::Staff;
        updateViewVisibility();
    };
    addAndMakeVisible(viewStaffBtn_);

    // ── Piano keyboard panel ──────────────────────────────────────────────────
    pianoKeyboardPanel_.setMasterKey(masterKeyRoot_, masterKeyMajor_);
    pianoKeyboardPanel_.onNoteOn = [this](float hz) {
        dspPipeline_.setForcedPitch(hz);
        if (auto* se = findSynthEffect()) {
            se->enabled.store(true, std::memory_order_relaxed);
            se->setKeyGate(0.75f);
        }
    };
    pianoKeyboardPanel_.onNoteOff = [this]() {
        dspPipeline_.clearForcedPitch();
        if (auto* se = findSynthEffect())
            se->setKeyGate(0.f);  // relâche l'enveloppe normalement
    };
    pianoKeyboardPanel_.onSynthParam = [this](int paramIdx, float value) {
        if (auto* se = findSynthEffect())
            se->setParam(paramIdx, value);
    };
    pianoKeyboardPanel_.setVisible(false);
    addAndMakeVisible(pianoKeyboardPanel_);

    // ── Sax staff panel ───────────────────────────────────────────────────────
    saxStaffPanel_.setMasterKey(masterKeyRoot_, masterKeyMajor_);
    saxStaffPanel_.setVisible(false);
    addAndMakeVisible(saxStaffPanel_);

    // ── Spatial visualization ─────────────────────────────────────────────────
    addAndMakeVisible(spatialViz_);

    // Initial BPM
    stepSequencer_.setBpm(120.f);
    dspPipeline_.setBpm(120.f);

    setSize(1280, 900);
    setAudioChannels(1, 2);
    midiManager_.start(deviceManager);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    // Signal background workers to abort as soon as possible
    shutdownFlag_.store(true, std::memory_order_release);

    // Wait for all pending background tasks (max ~5 s to avoid hanging)
    for (auto& f : backgroundTasks_)
    {
        if (f.valid())
            f.wait();
    }
    backgroundTasks_.clear();

    // Restore default look and feel and release SAX-OS look
    setLookAndFeel(nullptr);
    saxOsLookAndFeel_.reset();
    stopTimer();
    midiManager_.stop();
    shutdownAudio();
}

//==============================================================================

void MainComponent::loadSampleIntoSlot(int slot, const std::string& path)
{
    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();

    const juce::File file { juce::String(path) };
    std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(file));
    if (!reader) return;

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    if (numSamples <= 0) return;

    juce::AudioBuffer<float> buf(1, numSamples);
    reader->read(&buf, 0, numSamples, 0, true, false);

    auto& sampler = dspPipeline_.getSampler();
    sampler.loadSample(slot, buf.getReadPointer(0), numSamples,
                       static_cast<double>(reader->sampleRate));
    sampler.setSlotOneShot(slot, true);

    stepSeqPanel_.setSlotLoaded(slot, true);

    // Update waveform preview for this slot
    {
        auto pcm = sampler.getSlotPcmSnapshot(slot);
        stepSeqPanel_.setSlotWaveform(slot, computeEnvelope(pcm));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// findSynthEffect — returns first SynthEffect in chain, or nullptr
// ─────────────────────────────────────────────────────────────────────────────
::dsp::SynthEffect* MainComponent::findSynthEffect() noexcept
{
    auto& chain = dspPipeline_.getEffectChain();
    for (int i = 0; i < chain.effectCount(); ++i)
        if (auto* se = dynamic_cast<::dsp::SynthEffect*>(chain.getEffect(i)))
            return se;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// autoMatchSampleAsync — BPM + key detection, time-stretch + pitch-shift, then
// reload into Sampler.
//
// Thread safety:
//   - Raw sample loaded immediately on the GUI thread (playable at once).
//   - Background worker: std::async (stored in backgroundTasks_).
//   - processingSlot_[slot] prevents overlapping jobs on the same slot.
//   - shutdownFlag_ checked periodically; lambda exits cleanly on destruction.
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::autoMatchSampleAsync(int slot, std::vector<float> raw, double sr)
{
    // Load the raw sample immediately — slot is playable right away
    {
        auto& sampler = dspPipeline_.getSampler();
        sampler.loadSample(slot, raw.data(), static_cast<int>(raw.size()), sr);
        sampler.setSlotOneShot(slot, true);
        stepSeqPanel_.setSlotLoaded(slot, true);
        stepSeqPanel_.setSlotWaveform(slot, computeEnvelope(raw));
    }

    // Slot 0 is the reference master — never auto-shift it
    if (slot == 0)
        return;

    // Only one background job per slot at a time
    if (slot < 0 || slot >= static_cast<int>(processingSlot_.size()))
        return;
    bool expected = false;
    if (!processingSlot_[static_cast<std::size_t>(slot)].compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return;  // job already running for this slot

    // Prune completed futures to avoid unbounded growth
    backgroundTasks_.erase(
        std::remove_if(backgroundTasks_.begin(), backgroundTasks_.end(),
            [](const std::future<void>& f) {
                return f.valid() &&
                       f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            }),
        backgroundTasks_.end());

    // Snapshot project SR on GUI thread before handing off (Réserve #2)
    const double snapProjectSr = currentSampleRate_;

    // Save raw copy for potential BPM-retry (Réserve #3)
    rawPcmForRetry_[static_cast<std::size_t>(slot)] = raw;
    rawSrForRetry_ [static_cast<std::size_t>(slot)] = sr;

    backgroundTasks_.push_back(
        std::async(std::launch::async,
            [this, slot, raw = std::move(raw), sr, snapProjectSr]() mutable
            {
                // RAII guard: clear processingSlot on exit
                struct SlotGuard {
                    std::atomic<bool>& flag;
                    ~SlotGuard() { flag.store(false, std::memory_order_release); }
                } guard{ processingSlot_[static_cast<std::size_t>(slot)] };

                if (shutdownFlag_.load(std::memory_order_acquire)) return;

                // ── Réserve #2 : SR normalisation ────────────────────────────────
                // If the file SR differs from the project SR, resample the raw PCM
                // so that all analysis and processing runs at the project rate.
                if (snapProjectSr > 0.0 && std::abs(sr - snapProjectSr) > 1.0)
                {
                    const float srRatio = static_cast<float>(sr / snapProjectSr);
                    auto resampled = ::dsp::WsolaShifter::resampleLinear(raw, srRatio);
                    if (!resampled.empty())
                    {
                        raw = std::move(resampled);
                        sr  = snapProjectSr;
                        // Push SR-corrected version immediately (replaces initial load)
                        juce::MessageManager::callAsync(
                            [this, slot, srFixed = raw]() mutable
                            {
                                dspPipeline_.getSampler().reloadSlotData(
                                    slot, std::move(srFixed));
                            });
                    }
                }

                if (shutdownFlag_.load(std::memory_order_acquire)) return;

                // 1. Skip pitch/tempo correction for pure percussion
                auto features = ::dsp::FeatureExtractor::extract(raw, sr);
                const bool isPercussive =
                    (features.contentType == ::dsp::ContentCategory::KICK  ||
                     features.contentType == ::dsp::ContentCategory::SNARE ||
                     features.contentType == ::dsp::ContentCategory::HIHAT ||
                     features.contentType == ::dsp::ContentCategory::PERC);
                if (isPercussive) return;

                if (shutdownFlag_.load(std::memory_order_acquire)) return;

                // 2. BPM detection with confidence (3-method fallback)
                ::dsp::BpmDetector::BpmDetectionResult bpmResult;
                if (overrideBpm_ > 0.f)
                {
                    // Manual BPM from popup override (Réserve #3)
                    bpmResult.bpm        = overrideBpm_;
                    bpmResult.confidence = 1.0f;
                    overrideBpm_ = 0.f;  // consume it
                }
                else
                {
                    bpmResult = ::dsp::BpmDetector::detectOfflineRobust(
                        raw.data(), static_cast<int>(raw.size()), sr);
                }

                // Snapshot project BPM (atomic read from BPM detector)
                const float masterBpm = dspPipeline_.getBpmDetector().getBpm();

                // ── Réserve #3 : low-confidence popup ────────────────────────────
                if (bpmResult.confidence < 0.5f &&
                    bpmResult.bpm >= ::dsp::BpmDetector::kMinBpm)
                {
                    const float detected = bpmResult.bpm;
                    juce::MessageManager::callAsync(
                        [this, slot, detected]()
                        { showBpmConfidencePopup(slot, detected); });
                    return;  // async processing will restart if user confirms
                }

                float stretchRatio      = 1.f;
                float pitchFixSemitones = 0.f;
                bool  doStretch         = false;

                if (bpmResult.confidence > 0.5f &&
                    bpmResult.bpm >= ::dsp::BpmDetector::kMinBpm &&
                    bpmResult.bpm <= ::dsp::BpmDetector::kMaxBpm &&
                    masterBpm    >= ::dsp::BpmDetector::kMinBpm)
                {
                    stretchRatio = masterBpm / bpmResult.bpm;
                    doStretch    = (std::abs(stretchRatio - 1.f) >= 0.05f) &&
                                   stretchRatio > 0.33f && stretchRatio < 3.f;
                    if (doStretch)
                        pitchFixSemitones = -12.f * std::log2(stretchRatio);
                }

                if (shutdownFlag_.load(std::memory_order_acquire)) return;

                // 3. Key detection
                ::dsp::KeyDetector kd;
                kd.process(raw.data(), static_cast<int>(raw.size()), sr);
                const auto keyResult = kd.getResult();
                float keyShiftSemitones = 0.f;
                if (keyResult.key >= 0)
                {
                    int delta = masterKeyRoot_ - keyResult.key;
                    while (delta >  6) delta -= 12;
                    while (delta < -6) delta += 12;
                    keyShiftSemitones = static_cast<float>(delta);
                }

                // totalSemitones: key correction + pre-compensation for the pitch
                // drop that Hermite resample will introduce in Pass 2.
                // pitchFix = -12*log2(stretchRatio)  →  cancelled by resample.
                const float totalSemitones = keyShiftSemitones + pitchFixSemitones;
                const bool  doShift        = std::abs(totalSemitones) > 0.25f;

                if (!doStretch && !doShift) return;  // nothing to do

                if (shutdownFlag_.load(std::memory_order_acquire)) return;

                // ── Pitch-first dual-pass ─────────────────────────────────────────
                // Pass 1: WSOLA pitch shift on the pristine (or SR-normalised) signal.
                //   totalSemitones includes pitchFix to pre-compensate for Pass 2.
                std::vector<float> processed = raw;
                if (doShift)
                {
                    std::vector<float> shifted(processed.size(), 0.f);
                    ::dsp::WsolaShifter ws;
                    ws.prepare(sr, 1024);
                    ws.setShiftSemitones(totalSemitones);
                    constexpr int kBlock = 1024;
                    for (int i = 0; i < static_cast<int>(processed.size()); i += kBlock)
                    {
                        if (shutdownFlag_.load(std::memory_order_acquire)) return;
                        const int n = std::min(kBlock,
                                               static_cast<int>(processed.size()) - i);
                        ws.process(processed.data() + i, shifted.data() + i, n, 0.f);
                    }
                    processed = std::move(shifted);
                }

                if (shutdownFlag_.load(std::memory_order_acquire)) return;

                // Pass 2: Hermite resample for tempo alignment.
                //   Also lowers pitch by -pitchFix semitones — exactly cancelled by Pass 1.
                if (doStretch)
                {
                    auto stretched = ::dsp::WsolaShifter::resampleHermite(processed,
                                                                            stretchRatio);
                    if (!stretched.empty()) processed = std::move(stretched);
                }

                if (shutdownFlag_.load(std::memory_order_acquire)) return;

                // 6. Reload on the GUI thread.
                //    Capture bpm/confidence as individual floats (Réserve #1 —
                //    avoids dangling reference to stack-local bpmResult struct).
                juce::MessageManager::callAsync(
                    [this, slot,
                     processed  = std::move(processed),
                     bpm        = bpmResult.bpm,
                     confidence = bpmResult.confidence]() mutable
                    {
                        // Compute envelope before moving processed data
                        auto envelope = computeEnvelope(processed);
                        dspPipeline_.getSampler().reloadSlotData(slot,
                                                                  std::move(processed));
                        stepSeqPanel_.setSlotBpm(slot, bpm, confidence);
                        stepSeqPanel_.setSlotWaveform(slot, std::move(envelope));
                    });
            }));
}

// ─────────────────────────────────────────────────────────────────────────────
// showBpmConfidencePopup — shown when detectOfflineRobust confidence < 0.5.
// Lets the user confirm the detected BPM or enter one manually.
// Called on the GUI thread via MessageManager::callAsync.
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::showBpmConfidencePopup(int slot, float detectedBpm)
{
    const juce::String msg =
        "BPM detected: " + juce::String(juce::roundToInt(detectedBpm)) +
        "\n(low confidence — may be inaccurate for melodic/ambient samples)\n\n"
        "What would you like to do?";

    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions{}
            .withTitle("BPM Uncertain")
            .withMessage(msg)
            .withButton("Use " + juce::String(juce::roundToInt(detectedBpm)) + " BPM")
            .withButton("Enter Manually")
            .withButton("Cancel"),
        [this, slot, detectedBpm](int result)
        {
            const auto idx = static_cast<std::size_t>(slot);
            if (result == 1)  // "Use detected BPM"
            {
                auto  raw = rawPcmForRetry_[idx];
                double sr = rawSrForRetry_ [idx];
                rawPcmForRetry_[idx].clear();
                overrideBpm_ = detectedBpm;
                autoMatchSampleAsync(slot, std::move(raw), sr);
            }
            else if (result == 2)  // "Enter Manually"
            {
                auto* aw = new juce::AlertWindow("Manual BPM",
                                                 "Enter the sample BPM:",
                                                 juce::MessageBoxIconType::QuestionIcon);
                aw->addTextEditor("bpm",
                    juce::String(juce::roundToInt(detectedBpm)), "BPM:");
                aw->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
                aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                aw->enterModalState(true,
                    juce::ModalCallbackFunction::create(
                        [this, slot, aw](int r)
                        {
                            const auto i = static_cast<std::size_t>(slot);
                            if (r == 1)
                            {
                                const float bpm =
                                    aw->getTextEditorContents("bpm").getFloatValue();
                                if (bpm >= 40.f && bpm <= 300.f)
                                {
                                    auto   raw = rawPcmForRetry_[i];
                                    double sr  = rawSrForRetry_ [i];
                                    rawPcmForRetry_[i].clear();
                                    overrideBpm_ = bpm;
                                    autoMatchSampleAsync(slot, std::move(raw), sr);
                                }
                            }
                            else
                            {
                                rawPcmForRetry_[i].clear();
                            }
                        }),
                    true);  // deleteWhenDismissed
            }
            else  // "Cancel"
            {
                rawPcmForRetry_[idx].clear();
            }
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// updateViewVisibility — show/hide effects / keyboard / staff panels
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::updateViewVisibility()
{
    effectChainEditor_  .setVisible(currentViewMode_ == ViewMode::Effects);
    pianoKeyboardPanel_ .setVisible(currentViewMode_ == ViewMode::Keyboard);
    saxStaffPanel_      .setVisible(currentViewMode_ == ViewMode::Staff);
    fxLabel_            .setVisible(currentViewMode_ == ViewMode::Effects);

    // Update button highlight colours
    viewKeyboardBtn_.setColour(juce::TextButton::buttonColourId,
        currentViewMode_ == ViewMode::Keyboard
            ? juce::Colour(0xFF4CDFA8).withAlpha(0.20f)
            : juce::Colour(0xFF131314));
    viewStaffBtn_.setColour(juce::TextButton::buttonColourId,
        currentViewMode_ == ViewMode::Staff
            ? juce::Colour(0xFF4CDFA8).withAlpha(0.20f)
            : juce::Colour(0xFF131314));
}

// ─────────────────────────────────────────────────────────────────────────────
// applyMasterKey — propagate master key to panels + MusicContext
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::applyMasterKey()
{
    pianoKeyboardPanel_.setMasterKey(masterKeyRoot_, masterKeyMajor_);
    saxStaffPanel_     .setMasterKey(masterKeyRoot_, masterKeyMajor_);

    // Update the existing MusicContext used by HarmonizerEffect / SmartMixEngine
    auto ctx     = effectChainEditor_.getMusicContext();
    ctx.keyRoot  = masterKeyRoot_;
    ctx.isMajor  = masterKeyMajor_;
    effectChainEditor_.setMusicContext(ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// computeEnvelope — builds a peak-amplitude envelope (bins values, 0..1)
// Called on the GUI thread after sample load; safe and fast (<1ms for 200 bins).
// ─────────────────────────────────────────────────────────────────────────────
std::vector<float> MainComponent::computeEnvelope(const std::vector<float>& pcm, int bins)
{
    if (pcm.empty()) return {};
    std::vector<float> env(static_cast<std::size_t>(bins), 0.f);
    const int total = static_cast<int>(pcm.size());
    for (int b = 0; b < bins; ++b)
    {
        const int first = b * total / bins;
        const int last  = std::min(total, (b + 1) * total / bins);
        float peak = 0.f;
        for (int i = first; i < last; ++i)
            peak = std::max(peak, std::abs(pcm[static_cast<std::size_t>(i)]));
        env[static_cast<std::size_t>(b)] = peak;
    }
    return env;
}

// ─────────────────────────────────────────────────────────────────────────────
// openSampleEditor — ouvre la fenêtre waveform + trim pour un slot
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::openSampleEditor(int slot)
{
    auto pcm = dspPipeline_.getSampler().getSlotPcmSnapshot(slot);
    if (pcm.empty()) return;

    const juce::String fileName =
        juce::File(stepSeqPanel_.getSlotFilePath(slot))
            .getFileNameWithoutExtension();
    const juce::String title =
        "Edit Sample  —  " + (fileName.isEmpty() ? "S" + juce::String(slot + 1) : fileName)
        + "  [S" + juce::String(slot + 1) + "]";

    auto* editor = new ui::SampleEditorComponent(std::move(pcm), currentSampleRate_);

    editor->onApply = [this, slot](int startSample, int endSample)
    {
        auto snapshot = dspPipeline_.getSampler().getSlotPcmSnapshot(slot);
        const int total = static_cast<int>(snapshot.size());
        const int s = juce::jlimit(0, total - 1, startSample);
        const int e = juce::jlimit(s + 1, total,  endSample);
        std::vector<float> trimmed(snapshot.begin() + s, snapshot.begin() + e);
        dspPipeline_.getSampler().reloadSlotData(slot, std::move(trimmed));
    };

    editor->onPlayRequested = [this, slot]()
    {
        dspPipeline_.getSampler().trigger(slot);
    };

    editor->onStopRequested = [this, slot]()
    {
        dspPipeline_.getSampler().stop(slot);
    };

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(editor);
    opts.dialogTitle             = title;
    opts.componentToCentreAround = this;
    opts.useNativeTitleBar       = false;
    opts.resizable               = false;
    opts.dialogBackgroundColour  = juce::Colour(0xFF0A0A0A);
    opts.content->setSize(720, 280);
    opts.launchAsync();
}

void MainComponent::saveProject()
{
    auto chooser = std::make_shared<juce::FileChooser>(
        "Save SaxFX Project", juce::File{}, "*.saxfx");

    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser](const juce::FileChooser& fc)
        {
            const auto results = fc.getResults();
            if (results.isEmpty()) return;

            project::ProjectData data;
            data.projectName = "SaxFX Project";
            data.bpm         = stepSequencer_.getBpm();

            // ── Effect chain ──────────────────────────────────────────────
            const auto aiFlags = effectChainEditor_.captureAiManagedFlags();
            project::ProjectLoader::captureChain(
                dspPipeline_.getEffectChain(), data, aiFlags);

            // ── Music context ─────────────────────────────────────────────
            const auto& ctx  = effectChainEditor_.getMusicContext();
            data.musicContext.bpm     = ctx.bpm;
            data.musicContext.keyRoot = ctx.keyRoot;
            data.musicContext.isMajor = ctx.isMajor;
            data.musicContext.style   = static_cast<int>(ctx.style);

            // ── Sampler slots + step patterns ─────────────────────────────
            auto& sampler = dspPipeline_.getSampler();
            for (int i = 0; i < 8; ++i)
            {
                auto& sc    = data.samples[static_cast<std::size_t>(i)];
                sc.filePath = stepSeqPanel_.getSlotFilePath(i);
                sc.gain     = sampler.getSlotGain(i);   // actual runtime gain
                sc.loop     = false;
                sc.oneShot  = true;
                sc.muted    = sampler.isSlotMuted(i);
                sc.gridDiv  = 0;
                for (int s = 0; s < 16; ++s)
                    sc.stepPattern[s] = stepSequencer_.getStep(i, s);
            }

            // ── AI mix states (gain, pan, width, depth) ───────────────────
            for (int i = 0; i < 8; ++i)
            {
                const auto ms = samplerEngine_.getSlotMixState(i);
                auto& sm      = data.slotMix[static_cast<std::size_t>(i)];
                sm.gain    = ms.gain;
                sm.pan     = ms.pan;
                sm.width   = ms.width;
                sm.depth   = ms.depth;
                sm.applied = ms.active;
            }

            // ── Master key ────────────────────────────────────────────────
            data.masterKeyRoot  = masterKeyRoot_;
            data.masterKeyMajor = masterKeyMajor_;

            // ── Scenes ────────────────────────────────────────────────────
            captureCurrentScene();  // make sure current scene is up to date
            data.currentScene = currentScene_;
            for (int si = 0; si < kMaxScenes; ++si)
            {
                const auto& src = scenes_[static_cast<std::size_t>(si)];
                auto& dst       = data.scenes[static_cast<std::size_t>(si)];
                dst.used          = src.used;
                dst.bpm           = src.bpm;
                dst.filePaths     = src.filePaths;
                dst.mutes         = src.mutes;
                dst.gains         = src.gains;
                dst.trackBarCounts = src.trackBarCounts;
                for (int t = 0; t < 8; ++t)
                {
                    const int numSteps = src.trackBarCounts[static_cast<std::size_t>(t)] * 16;
                    for (int s = 0; s < numSteps; ++s)
                        dst.steps[static_cast<std::size_t>(t)]
                                 [static_cast<std::size_t>(s)] =
                            src.steps[static_cast<std::size_t>(t)]
                                     [static_cast<std::size_t>(s)];
                }
            }

            auto path = results[0].getFullPathName();
            if (!path.endsWithIgnoreCase(".saxfx"))
                path += ".saxfx";

            if (project::ProjectLoader::save(data, path.toStdString()))
                juce::Logger::writeToLog("Project saved: " + path);
            else
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon, "Save failed",
                    "Could not write to " + path + ".");
        });
}

void MainComponent::applyProjectData(const project::ProjectData& data)
{
    // Restore BPM
    const float bpm = (data.bpm > 0.f) ? data.bpm : 120.f;
    stepSequencer_.setBpm(bpm);
    dspPipeline_.setBpm(bpm);
    stepSeqPanel_.setBpm(bpm);

    // Load samples and restore step patterns
    for (int i = 0; i < 8; ++i)
    {
        const auto& sc = data.samples[static_cast<std::size_t>(i)];

        if (!sc.filePath.empty())
        {
            loadSampleIntoSlot(i, sc.filePath);
            stepSeqPanel_.setSlotFilePath(i, sc.filePath);   // updates LCD name
            samplerEngine_.setSlotFilePath(i, sc.filePath);
        }

        for (int s = 0; s < 16; ++s)
        {
            const bool active = sc.stepPattern[s];
            stepSequencer_.setStep(i, s, active);
            stepSeqPanel_.setStepState(i, s, active);
        }

        dspPipeline_.getSampler().setSlotMuted(i, sc.muted);
        stepSeqPanel_.setSlotMuted(i, sc.muted);
    }

    // Apply effect chain — rebuild UI *immediately* so EffectRackUnit cards
    // never reference deleted IEffect objects (fixes crash on Load Project).
    project::ProjectLoader::applyChain(data, dspPipeline_.getEffectChain());
    effectChainEditor_.forceRebuild();

    {
        std::vector<bool> aiFlags;
        aiFlags.reserve(data.effectChain.size());
        for (const auto& slot : data.effectChain)
            aiFlags.push_back(slot.aiManaged);
        effectChainEditor_.applyAiManagedFlags(aiFlags);
    }

    if (data.musicContext.bpm > 0.f)
    {
        ::dsp::MusicContext ctx;
        ctx.bpm     = data.musicContext.bpm;
        ctx.keyRoot = data.musicContext.keyRoot;
        ctx.isMajor = data.musicContext.isMajor;
        ctx.style   = static_cast<::dsp::MusicContext::Style>(data.musicContext.style);
        effectChainEditor_.setMusicContext(ctx);
    }

    // Restore MIDI mappings
    midiManager_.clearMappings();
    for (const auto& m : data.midiMappings)
        midiManager_.setNoteMapping(m.midiNote, m.slotIndex);

    // ── v5 — master key ───────────────────────────────────────────────────────
    if (data.version >= 5)
    {
        masterKeyRoot_  = data.masterKeyRoot;
        masterKeyMajor_ = data.masterKeyMajor;
        masterKeyCombo_    .setSelectedId(masterKeyRoot_ + 1,          juce::dontSendNotification);
        masterKeyModeCombo_.setSelectedId(masterKeyMajor_ ? 1 : 2,     juce::dontSendNotification);
        applyMasterKey();
    }

    // ── v5 — AI mix states ────────────────────────────────────────────────────
    if (data.version >= 5)
    {
        auto& sampler = dspPipeline_.getSampler();
        for (int i = 0; i < 8; ++i)
        {
            const auto& sm = data.slotMix[static_cast<std::size_t>(i)];
            if (!sm.applied) continue;
            sampler.setSlotGain(i, sm.gain);
            sampler.setSlotPan(i, sm.pan);
            const int haasSamples = static_cast<int>(
                sm.width * 0.025 * currentSampleRate_);
            sampler.setSlotHaasDelay(i, haasSamples);
            samplerEngine_.restoreSlotMixState(i, sm.gain, sm.pan, sm.width, sm.depth);
        }
    }

    // ── v5 — scenes ───────────────────────────────────────────────────────────
    if (data.version >= 5)
    {
        currentScene_ = std::clamp(data.currentScene, 0, kMaxScenes - 1);
        for (int si = 0; si < kMaxScenes; ++si)
        {
            const auto& src = data.scenes[static_cast<std::size_t>(si)];
            auto& dst       = scenes_    [static_cast<std::size_t>(si)];
            dst.used          = src.used;
            dst.bpm           = src.bpm;
            dst.filePaths     = src.filePaths;
            dst.mutes         = src.mutes;
            dst.gains         = src.gains;
            dst.trackBarCounts = src.trackBarCounts;
            for (int t = 0; t < 8; ++t)
            {
                const int numSteps = src.trackBarCounts[static_cast<std::size_t>(t)] * 16;
                for (int s = 0; s < numSteps; ++s)
                    dst.steps[static_cast<std::size_t>(t)]
                             [static_cast<std::size_t>(s)] =
                        src.steps[static_cast<std::size_t>(t)]
                                 [static_cast<std::size_t>(s)];
            }
        }
        // Restore bar counts, step patterns and sample paths for the current scene
        applyScene(currentScene_);
        updateSceneLabel();
    }

    juce::Logger::writeToLog("Project loaded: " + juce::String(data.projectName));
}

//==============================================================================
// Audio callbacks
//==============================================================================

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate_ = sampleRate;
    currentBufferSize_ = samplesPerBlockExpected;

    stepSequencer_.prepare(sampleRate);
    samplerEngine_.setSampleRate(sampleRate);
    dspPipeline_.prepare(sampleRate, samplesPerBlockExpected);

    juce::Logger::writeToLog(juce::String("Audio prepared: ") + juce::String(sampleRate) +
                             " Hz, buffer " + juce::String(samplesPerBlockExpected) + " samples (" +
                             juce::String(1000.0 * samplesPerBlockExpected / sampleRate, 1) +
                             " ms)");
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    const int numSamples = bufferToFill.numSamples;
    const int numCh      = bufferToFill.buffer->getNumChannels();

    float* left = bufferToFill.buffer->getWritePointer(0, bufferToFill.startSample);

    // Compute input RMS before processing (for VU meter) — exponential smoothing
    {
        float sumSquares = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            sumSquares += left[i] * left[i];
        const float rawRms = std::sqrt(sumSquares / static_cast<float>(numSamples));
        const float prev   = currentRmsLevel_.load(std::memory_order_relaxed);
        const float coeff  = (rawRms > prev) ? 0.3f : 0.05f;
        currentRmsLevel_.store(prev + coeff * (rawRms - prev),
                               std::memory_order_relaxed);
    }

    // Step sequencer — triggers sampler slots at step boundaries (before DSP mix)
    stepSequencer_.process(numSamples, dspPipeline_.getSampler());

    if (numCh >= 2)
    {
        // ── Stereo path ───────────────────────────────────────────────────────
        float* right = bufferToFill.buffer->getWritePointer(1, bufferToFill.startSample);
        // Seed right channel with sax input (same as left before effects)
        std::copy(left, left + numSamples, right);

        dspPipeline_.processStereo(left, right, numSamples);

        // Apply master output gain to both channels
        const float outGain = outputGain_.load(std::memory_order_relaxed);
        if (outGain != 1.0f)
            for (int i = 0; i < numSamples; ++i)
            {
                left [i] *= outGain;
                right[i] *= outGain;
            }

        // Copy right to any additional channels (surround)
        for (int ch = 2; ch < numCh; ++ch)
            bufferToFill.buffer->copyFrom(ch, bufferToFill.startSample,
                                          *bufferToFill.buffer, 1,
                                          bufferToFill.startSample, numSamples);

        // Output RMS on left channel
        {
            float sumSquares = 0.0f;
            for (int i = 0; i < numSamples; ++i)
                sumSquares += left[i] * left[i];
            const float rawRms = std::sqrt(sumSquares / static_cast<float>(numSamples));
            const float prev   = currentOutputRmsLevel_.load(std::memory_order_relaxed);
            const float coeff  = (rawRms > prev) ? 0.3f : 0.05f;
            currentOutputRmsLevel_.store(prev + coeff * (rawRms - prev),
                                         std::memory_order_relaxed);
        }
    }
    else
    {
        // ── Mono fallback path (unchanged) ───────────────────────────────────
        dspPipeline_.process(left, numSamples);

        const float outGain = outputGain_.load(std::memory_order_relaxed);
        if (outGain != 1.0f)
            for (int i = 0; i < numSamples; ++i)
                left[i] *= outGain;

        // Compute Output RMS
        {
            float sumSquares = 0.0f;
            for (int i = 0; i < numSamples; ++i)
                sumSquares += left[i] * left[i];
            const float rawRms = std::sqrt(sumSquares / static_cast<float>(numSamples));
            const float prev   = currentOutputRmsLevel_.load(std::memory_order_relaxed);
            const float coeff  = (rawRms > prev) ? 0.3f : 0.05f;
            currentOutputRmsLevel_.store(prev + coeff * (rawRms - prev),
                                         std::memory_order_relaxed);
        }
    }
}

void MainComponent::releaseResources()
{
    dspPipeline_.reset();
    juce::Logger::writeToLog("Audio resources released.");
}

//==============================================================================
// GUI
//==============================================================================

void MainComponent::paint(juce::Graphics& g)
{
    const int W = getWidth();
    const int H = getHeight();
    static constexpr int kSidebarW = 264;
    static constexpr int kHeaderH  = 48;
    static constexpr int kStatusH  = 24;
    const int sidebarX = W - kSidebarW;

    g.fillAll(ui::SaxFXColours::background);

    // ── Top header bar ────────────────────────────────────────────────────────
    {
        g.setColour(juce::Colour(0xFF131314));
        g.fillRect(0, 0, W, kHeaderH);
        g.setColour(ui::SaxFXColours::cardBorder);
        g.fillRect(0, kHeaderH - 1, W, 1);

        // Logo
        if (logoImage_.isValid())
        {
            const int logoSize = kHeaderH - 10;
            g.drawImageWithin(logoImage_, 10, 5, logoSize, logoSize,
                juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
        }

        // Titre "DubEngine"
        const int titleX = logoImage_.isValid() ? kHeaderH + 6 : 16;
        g.setColour(ui::SaxFXColours::vuLow);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(15.f).withStyle("Bold")));
        g.drawText("DubEngine", titleX, 0, 160, kHeaderH, juce::Justification::centredLeft);

        // Pitch display
        g.setColour(juce::Colours::cyan.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(13.f).withStyle("Bold")));
        g.drawText(pitchLabel_.getText(), sidebarX - 180, 0, 170, kHeaderH,
                   juce::Justification::centredRight);
    }

    // ── Sidebar background ────────────────────────────────────────────────────
    g.setColour(juce::Colour(0xFF0E0E0F));
    g.fillRect(sidebarX, 0, kSidebarW, H);
    g.setColour(ui::SaxFXColours::cardBorder);
    g.fillRect(sidebarX, 0, 1, H);

    // ── Sidebar: "MASTER CLOCK" label ─────────────────────────────────────────
    {
        g.setColour(ui::SaxFXColours::textSecondary);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.5f)));
        g.drawText("MASTER CLOCK", sidebarX, kHeaderH + 10, kSidebarW, 12,
                   juce::Justification::centred);
    }

    // ── Sidebar: VU meters (L / R segmented bars) ────────────────────────────
    {
        const float rmsIn  = currentRmsLevel_.load(std::memory_order_relaxed);
        const float rmsOut = currentOutputRmsLevel_.load(std::memory_order_relaxed);
        const float dbIn   = juce::Decibels::gainToDecibels(rmsIn, -60.0f);
        const float dbOut  = juce::Decibels::gainToDecibels(rmsOut, -60.0f);
        const float normIn  = juce::jlimit(0.f, 1.f, juce::jmap(dbIn,  -60.f, 0.f, 0.f, 1.f));
        const float normOut = juce::jlimit(0.f, 1.f, juce::jmap(dbOut, -60.f, 0.f, 0.f, 1.f));

        // Centered pair of meters
        static constexpr int vuW = 16;
        static constexpr int vuH = 160;
        static constexpr int vuGap = 28;
        const int vuY  = kHeaderH + 140;  // below BPM display + TAP button
        const int vuLX = sidebarX + (kSidebarW - vuW * 2 - vuGap) / 2;
        const int vuRX = vuLX + vuW + vuGap;

        const int meters[2][3] = { {vuLX, vuH, 0}, {vuRX, vuH, 1} };
        const float norms[2]   = { normIn, normOut };
        const juce::String labels[2] = { "LEFT", "RIGHT" };

        for (int m = 0; m < 2; ++m)
        {
            const int mx    = meters[m][0];
            const float norm = norms[m];

            // Track background
            g.setColour(juce::Colour(0xFF1C1B1C));
            g.fillRoundedRectangle(static_cast<float>(mx), static_cast<float>(vuY),
                                   static_cast<float>(vuW), static_cast<float>(vuH), 2.f);

            const float filled  = static_cast<float>(vuH) * norm;
            const float seg1End = static_cast<float>(vuH) * 0.60f;
            const float seg2End = static_cast<float>(vuH) * 0.85f;

            if (filled > 0.f)
            {
                const float h1 = juce::jmin(filled, seg1End);
                g.setColour(ui::SaxFXColours::vuLow.withAlpha(0.90f));
                g.fillRoundedRectangle(static_cast<float>(mx),
                                       static_cast<float>(vuY + vuH) - h1,
                                       static_cast<float>(vuW), h1, 3.f);
            }
            if (filled > seg1End)
            {
                const float h2 = juce::jmin(filled - seg1End, seg2End - seg1End);
                g.setColour(ui::SaxFXColours::vuMid.withAlpha(0.94f));
                g.fillRoundedRectangle(static_cast<float>(mx),
                                       static_cast<float>(vuY + vuH) - seg1End - h2,
                                       static_cast<float>(vuW), h2, 3.f);
            }
            if (filled > seg2End)
            {
                const float h3 = filled - seg2End;
                g.setColour(ui::SaxFXColours::vuHigh.withAlpha(0.97f));
                g.fillRoundedRectangle(static_cast<float>(mx),
                                       static_cast<float>(vuY + vuH) - seg2End - h3,
                                       static_cast<float>(vuW), h3, 3.f);
            }
            // Label
            g.setColour(ui::SaxFXColours::textSecondary.withAlpha(0.60f));
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.f)));
            g.drawText(labels[m], mx - 4, vuY + vuH + 4, vuW + 8, 10,
                       juce::Justification::centred);
        }
    }

    // ── Sidebar separator line before buttons ─────────────────────────────────
    {
        const int sepY = kHeaderH + 320;
        g.setColour(ui::SaxFXColours::cardBorder);
        g.fillRect(sidebarX + 12, sepY, kSidebarW - 24, 1);
    }

    // ── Sidebar separator before transport ────────────────────────────────────
    {
        const int sepY = kHeaderH + 490;
        g.setColour(ui::SaxFXColours::cardBorder);
        g.fillRect(sidebarX + 12, sepY, kSidebarW - 24, 1);
    }

    // ── Section headers in sidebar ────────────────────────────────────────────
    {
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.f)));
        g.setColour(ui::SaxFXColours::textSecondary.withAlpha(0.50f));
        g.drawText("PROJECT", sidebarX + 12, kHeaderH + 328, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);
        g.drawText("TRANSPORT", sidebarX + 12, kHeaderH + 498, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);

        // Labels mirroring resized() bottom-up layout:
        // magicY=H-58, aiStatus=H-76, play=H-144, viewBtn=H-176, masterKey=H-210
        const int masterKeyY_p = H - 210;
        const int viewBtnY_p   = H - 176;
        g.drawText("KEY",  sidebarX + 12, masterKeyY_p - 11, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);
        g.drawText("VIEW", sidebarX + 12, viewBtnY_p   - 11, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);
    }

    // ── Main area section separators ─────────────────────────────────────────
    {
        g.setColour(ui::SaxFXColours::cardBorder);
        const int mainContentW = sidebarX - 20;
        const int seqLabelY    = kHeaderH + 440;
        g.fillRect(16, seqLabelY, mainContentW, 1);
    }

    // ── Bottom status bar ─────────────────────────────────────────────────────
    {
        const int statusY = H - kStatusH;
        const int statusW = sidebarX;

        g.setColour(juce::Colour(0xFF0E0E0F));
        g.fillRect(0, statusY, statusW, kStatusH);
        g.setColour(ui::SaxFXColours::cardBorder);
        g.fillRect(0, statusY, statusW, 1);

        // Status LED
        const juce::Colour ledCol = pipelineActive_ ? ui::SaxFXColours::vuLow
                                                    : juce::Colours::orange;
        g.setColour(ledCol.withAlpha(0.35f));
        g.fillEllipse(10.f, static_cast<float>(statusY + kStatusH / 2 - 5), 10.f, 10.f);
        g.setColour(ledCol);
        g.fillEllipse(12.f, static_cast<float>(statusY + kStatusH / 2 - 3), 6.f, 6.f);

        g.setFont(juce::Font(juce::FontOptions{}.withHeight(9.f).withStyle("Bold")));
        g.setColour(ui::SaxFXColours::textPrimary.withAlpha(0.40f));
        g.drawText("INPUT SIGNAL: STABLE", 24, statusY, 200, kStatusH,
                   juce::Justification::centredLeft);

        const auto bufStr = juce::String("BUFFER: ")
                          + juce::String(currentBufferSize_) + " SAMPLES";
        g.drawText(bufStr, 230, statusY, 200, kStatusH,
                   juce::Justification::centredLeft);

        g.setColour(ui::SaxFXColours::textPrimary.withAlpha(0.20f));
        g.drawText("DubEngine v0.1.0", statusW - 260, statusY, 250, kStatusH,
                   juce::Justification::centredRight);
    }

    // ── Grain noise overlay ───────────────────────────────────────────────────
    {
        juce::Random rng(42);
        g.setColour(juce::Colours::white.withAlpha(0.012f));
        for (int i = 0; i < 800; ++i)
        {
            const float gx = rng.nextFloat() * static_cast<float>(W);
            const float gy = rng.nextFloat() * static_cast<float>(H);
            g.fillRect(gx, gy, 1.5f, 1.5f);
        }
    }
}

void MainComponent::resized()
{
    const int W = getWidth();
    const int H = getHeight();
    static constexpr int kSidebarW = 264;
    static constexpr int kHeaderH  = 48;
    static constexpr int kStatusH  = 24;
    static constexpr int kPad      = 8;
    const int sidebarX = W - kSidebarW;
    const int mainW    = sidebarX - 16;

    const int sbBtnW = kSidebarW - 24;
    const int sbBtnX = sidebarX + 12;

    // ════════════════════════════════════════════════════════════════════════
    // SIDEBAR — unified top to bottom
    // ════════════════════════════════════════════════════════════════════════
    {
        // ── MASTER CLOCK section ─────────────────────────────────────────
        // "MASTER CLOCK" label drawn in paint() at kHeaderH+10
        // BPM large number
        sidebarBpmLabel_.setBounds(sidebarX, kHeaderH + 24, kSidebarW, 52);

        // TAP TEMPO button
        sidebarTapBtn_.setBounds(sbBtnX, kHeaderH + 82, sbBtnW, 34);

        // Main mix slider (slim fader, alongside VU meters)
        mainMixLabel_.setBounds(sidebarX, kHeaderH + 120, kSidebarW, 12);
        mainMixSlider_.setBounds(sidebarX + kSidebarW - 20, kHeaderH + 140, 14, 160);

        // VU meters drawn in paint() at kHeaderH+140

        // ── PROJECT section ──────────────────────────────────────────────
        // Separator + label drawn in paint()
        int y = kHeaderH + 342;
        saveProjectButton_  .setBounds(sbBtnX, y, sbBtnW, 28); y += 32;
        loadProjectButton_  .setBounds(sbBtnX, y, sbBtnW, 28); y += 32;
        audioSettingsButton_.setBounds(sbBtnX, y, sbBtnW, 28);

        // Info label (tiny, below audio settings)
        infoLabel_.setBounds(sidebarX, kHeaderH + 436, kSidebarW, 10);

        // ── SPATIAL pad ─────────────────────────────────────────────────
        spatialViz_.setBounds(sbBtnX, kHeaderH + 450, sbBtnW, 58);

        // ── TRANSPORT section ────────────────────────────────────────────
        // Separator + label drawn in paint()
        y = kHeaderH + 512;
        sceneUpBtn_   .setBounds(sbBtnX, y, sbBtnW, 26); y += 28;
        sceneNumLabel_.setBounds(sbBtnX, y, sbBtnW, 18); y += 20;
        sceneDownBtn_ .setBounds(sbBtnX, y, sbBtnW, 26); y += 30;
        {
            const int half = (sbBtnW - 2) / 2;
            sceneResetBtn_     .setBounds(sbBtnX,          y, half,            22);
            sceneTrackResetBtn_.setBounds(sbBtnX + half + 2, y, sbBtnW - half - 2, 22);
        }
        y += 25;
        sceneCopyBtn_.setBounds(sbBtnX, y, sbBtnW, 22);

        // ── Bottom section (pinned, bottom-up) ──────────────────────────
        // magicY   = H-58 | aiStatus = H-76 | play = H-144
        // viewBtn  = H-176 | masterKey = H-210
        const int magicY     = H - kStatusH - kPad - 26;  // 26px ⚡
        const int aiStatusY  = magicY - 18;                // 16px AI label
        const int playY      = aiStatusY - kPad - 60;      // 60px PLAY
        const int viewBtnY   = playY - kPad - 24;          // 24px 🎹🎼
        const int masterKeyY = viewBtnY - kPad - 26;       // 26px KEY

        sidebarPlayBtn_ .setBounds(sbBtnX, playY,     sbBtnW, 60);
        aiStatusLabel_  .setBounds(sbBtnX, aiStatusY, sbBtnW, 16);
        sidebarMagicBtn_.setBounds(sbBtnX, magicY,    sbBtnW, 26);

        const int halfW = sbBtnW / 2 - 2;
        viewKeyboardBtn_.setBounds(sbBtnX,             viewBtnY, halfW, 24);
        viewStaffBtn_   .setBounds(sbBtnX + halfW + 4, viewBtnY, halfW, 24);

        globalScaleCombo_.setBounds(0, 0, 0, 0);           // supprimé — doublon
        masterKeyCombo_    .setBounds(sbBtnX,             masterKeyY, halfW, 26);
        masterKeyModeCombo_.setBounds(sbBtnX + halfW + 4, masterKeyY, halfW, 26);
    }

    // ════════════════════════════════════════════════════════════════════════
    // MAIN CONTENT AREA
    // ════════════════════════════════════════════════════════════════════════

    // Pitch label hidden from main area (shown in header via paint())
    pitchLabel_.setBounds(0, 0, 0, 0);

    // Effect chain section + swappable panels (same bounds)
    const int fxTop = kHeaderH + 16;
    fxLabel_.setBounds(16, fxTop, 180, 16);

    const auto panelBounds = juce::Rectangle<int>(16, fxTop + 20, mainW, 390);
    effectChainEditor_  .setBounds(panelBounds);
    pianoKeyboardPanel_ .setBounds(panelBounds);
    saxStaffPanel_      .setBounds(panelBounds);

    // Step sequencer section
    const int seqTop = fxTop + 20 + 390 + 28;
    samplerLabel_.setBounds(16, seqTop - 20, 200, 16);
    stepSeqPanel_.setBounds(16, seqTop, mainW, H - seqTop - kStatusH - kPad);
}

void MainComponent::timerCallback()
{
    // Transition de scène quantisée : appliquée à la fin du cycle du séquenceur
    if (pendingScene_.load(std::memory_order_relaxed) >= 0
        && stepSequencer_.consumeSceneEnd())
    {
        const int target = pendingScene_.exchange(-1, std::memory_order_relaxed);
        if (target >= 0)
        {
            captureCurrentScene();
            currentScene_ = target;
            applyScene(currentScene_);
            updateSceneLabel();
        }
    }

    dspPipeline_.getEffectChain().collectGarbage();

    // AI status animation while busy
    if (samplerEngine_.isBusy())
    {
        static const char* kFrames[] = { "IA \xe2\x97\x8f", "IA \xe2\x97\x8f\xe2\x97\x8f", "IA \xe2\x97\x8f\xe2\x97\x8f\xe2\x97\x8f", "IA \xe2\x97\x8f\xe2\x97\x8f" };
        aiStatusLabel_.setText(juce::CharPointer_UTF8(kFrames[aiAnimFrame_ % 4]),
                               juce::dontSendNotification);
        ++aiAnimFrame_;
    }

    // Sync play button text with sequencer state
    sidebarPlayBtn_.setButtonText(
        stepSequencer_.isPlaying()
            ? juce::CharPointer_UTF8("\xe2\x96\xa0")   // ■
            : juce::CharPointer_UTF8("\xe2\x96\xb6"));  // ▶

    repaint();

    auto* device = deviceManager.getCurrentAudioDevice();
    pipelineActive_ = (device != nullptr);

    if (device != nullptr)
    {
        infoLabel_.setText(juce::String(static_cast<int>(currentSampleRate_)) + " Hz / " +
                               juce::String(currentBufferSize_) + " smp",
                           juce::dontSendNotification);

        const auto pitch = dspPipeline_.getLastPitch();
        if (pitch.frequencyHz > 0.0f && pitch.confidence > 0.3f)
            pitchLabel_.setText(frequencyToNoteName(pitch.frequencyHz) + "  " +
                                    juce::String(pitch.frequencyHz, 0) + " Hz",
                                juce::dontSendNotification);
        else
            pitchLabel_.setText("--", juce::dontSendNotification);
    }
    else
    {
        infoLabel_.setText("No audio device", juce::dontSendNotification);
        pitchLabel_.setText("--", juce::dontSendNotification);
    }
}

//==============================================================================
juce::String MainComponent::frequencyToNoteName(float hz)
{
    static const char* noteNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                      "F#", "G",  "G#", "A",  "A#", "B"};
    if (hz <= 0.0f) return "--";

    const float midi    = 69.0f + 12.0f * std::log2(hz / 440.0f);
    const int midiInt   = static_cast<int>(std::round(midi));
    if (midiInt < 0 || midiInt > 127) return "--";

    const int octave  = midiInt / 12 - 1;
    const int noteIdx = midiInt % 12;
    return juce::String(noteNames[noteIdx]) + juce::String(octave);
}

//==============================================================================
// Scene management
//==============================================================================

void MainComponent::updateSidebarBpm(float bpm)
{
    sidebarBpmLabel_.setText(juce::String(bpm, 2),
                             juce::dontSendNotification);
}

void MainComponent::updateSceneLabel()
{
    sceneNumLabel_.setText("Scene " + juce::String(currentScene_ + 1) +
                           " / " + juce::String(kMaxScenes),
                           juce::dontSendNotification);
}

void MainComponent::captureCurrentScene()
{
    auto& sc = scenes_[static_cast<std::size_t>(currentScene_)];
    sc.bpm  = stepSequencer_.getBpm();
    sc.used = true;
    auto& sampler = dspPipeline_.getSampler();
    for (int i = 0; i < 8; ++i)
    {
        const std::size_t idx  = static_cast<std::size_t>(i);
        const int numSteps     = stepSequencer_.getTrackStepCount(i);
        sc.filePaths    [idx]  = stepSeqPanel_.getSlotFilePath(i);
        sc.mutes        [idx]  = sampler.isSlotMuted(i);
        sc.gains        [idx]  = sampler.getSlotGain(i);
        sc.trackBarCounts[idx] = stepSequencer_.getTrackBarCount(i);
        for (int s = 0; s < numSteps; ++s)
            sc.steps[idx][static_cast<std::size_t>(s)] = stepSequencer_.getStep(i, s);
    }
}

void MainComponent::applyScene(int idx)
{
    const auto& sc = scenes_[static_cast<std::size_t>(idx)];

    if (!sc.used)
    {
        // Empty scene: clear all step patterns, keep BPM
        for (int i = 0; i < 8; ++i)
            for (int s = 0; s < 16; ++s)
            {
                stepSequencer_.setStep(i, s, false);
                stepSeqPanel_.setStepState(i, s, false);
            }
        return;
    }

    // BPM is global (master clock) — not overridden by scene data

    // Remettre le séquenceur au step 0 → nouvelle scène part toujours du début
    if (stepSequencer_.isPlaying())
        stepSequencer_.resetPhase();

    // Restore samples, bar counts and step patterns
    for (int i = 0; i < 8; ++i)
    {
        const std::size_t sidx    = static_cast<std::size_t>(i);
        const int barCount        = sc.trackBarCounts[sidx];
        const int numSteps        = barCount * 16;
        const std::string& newPath     = sc.filePaths[i];
        const std::string  currentPath = stepSeqPanel_.getSlotFilePath(i);

        stepSequencer_.setTrackBarCount(i, barCount);
        stepSeqPanel_.setTrackStepCount(i, numSteps);

        if (!newPath.empty() && newPath != currentPath)
        {
            // Sample différent : rechargement nécessaire
            loadSampleIntoSlot(i, newPath);
            stepSeqPanel_.setSlotFilePath(i, newPath);
            samplerEngine_.setSlotFilePath(i, newPath);
        }
        else if (newPath.empty() && !currentPath.empty())
        {
            // Slot doit être vidé
            dspPipeline_.getSampler().clearSlot(i);
            stepSeqPanel_.setSlotFilePath(i, "");
        }
        else if (!newPath.empty())
        {
            // Même fichier déjà chargé : skip reload → pas de coupure audio
            samplerEngine_.setSlotFilePath(i, newPath);
        }

        dspPipeline_.getSampler().setSlotMuted(i, sc.mutes[i]);
        dspPipeline_.getSampler().setSlotGain (i, sc.gains[sidx]);
        stepSeqPanel_.setSlotMuted(i, sc.mutes[i]);
        for (int s = 0; s < numSteps; ++s)
        {
            const bool active = sc.steps[sidx][static_cast<std::size_t>(s)];
            stepSequencer_.setStep(i, s, active);
            stepSeqPanel_.setStepState(i, s, active);
        }
    }
}

void MainComponent::navigateScene(int delta)
{
    const int target = juce::jlimit(0, kMaxScenes - 1, currentScene_ + delta);
    if (target == currentScene_) return;

    if (!stepSequencer_.isPlaying())
    {
        // Séquenceur arrêté : changement immédiat, sans risque de coupure
        captureCurrentScene();
        currentScene_ = target;
        applyScene(currentScene_);
        updateSceneLabel();
        return;
    }

    // Séquenceur en lecture : on met la cible en attente.
    // Figer la longueur de la scène courante AVANT de stocker pendingScene_,
    // pour que la détection de fin de cycle soit stable dans le thread audio.
    int sceneLen = 1;
    for (int i = 0; i < 8; ++i)
        sceneLen = std::max(sceneLen, stepSequencer_.getTrackStepCount(i));
    stepSequencer_.setPendingTransitionLen(sceneLen);

    pendingScene_.store(target, std::memory_order_relaxed);
    sceneNumLabel_.setText("Scene " + juce::String(currentScene_ + 1) +
                           " \xe2\x86\x92 " + juce::String(target + 1),  // →
                           juce::dontSendNotification);
}

void MainComponent::resetCurrentScene()
{
    // Clear step patterns only — keep samples loaded
    for (int i = 0; i < 8; ++i)
    {
        const int numSteps = stepSequencer_.getTrackStepCount(i);
        for (int s = 0; s < numSteps; ++s)
        {
            stepSequencer_.setStep(i, s, false);
            stepSeqPanel_.setStepState(i, s, false);
        }
    }
    scenes_[static_cast<std::size_t>(currentScene_)].used = false;
}

void MainComponent::resetCurrentSceneFull()
{
    // Clear patterns (all steps)
    for (int i = 0; i < 8; ++i)
    {
        const int numSteps = stepSequencer_.getTrackStepCount(i);
        for (int s = 0; s < numSteps; ++s)
        {
            stepSequencer_.setStep(i, s, false);
            stepSeqPanel_.setStepState(i, s, false);
        }
    }
    // Unload all samples
    auto& sampler = dspPipeline_.getSampler();
    for (int i = 0; i < 8; ++i)
    {
        sampler.clearSlot(i);
        stepSeqPanel_.setSlotFilePath(i, "");
        samplerEngine_.setSlotFilePath(i, "");
        stepSeqPanel_.setSlotLoaded(i, false);
    }
    scenes_[static_cast<std::size_t>(currentScene_)].used = false;
}

void MainComponent::copyCurrentSceneToNext()
{
    if (currentScene_ == 0) return;   // no previous scene
    const int prevIdx = currentScene_ - 1;
    captureCurrentScene();   // save current state first
    scenes_[static_cast<std::size_t>(currentScene_)] = scenes_[static_cast<std::size_t>(prevIdx)];
    scenes_[static_cast<std::size_t>(currentScene_)].used = true;
    applyScene(currentScene_);
    juce::Logger::writeToLog("Scene " + juce::String(prevIdx + 1) +
                             " copied into scene " + juce::String(currentScene_ + 1));
}
