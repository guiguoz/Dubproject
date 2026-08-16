#include "MainComponent.h"



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

    // ── Audio settings button (masqué — accessible via menu FILES) ───────────
    audioSettingsButton_.setButtonText("AUDIO SETTINGS");
    audioSettingsButton_.onClick = [this] { openAudioSettings(); };

    // ── Status / info labels ──────────────────────────────────────────────────
    infoLabel_.setJustificationType(juce::Justification::centred);
    infoLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(9.5f)));
    infoLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible(infoLabel_);

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
            "Effacer les patterns de la scene " + juce::String(sceneManager_.currentIdx() + 1) + " ?\n"
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
            "Effacer patterns ET samples de la scene " + juce::String(sceneManager_.currentIdx() + 1) + " ?\n"
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
            serumHost_.setBpm(bpm);
            looperEngine_.setBpm(bpm);
            stepSeqPanel_.setBpm(bpm);
            updateSidebarBpm(bpm);
        }
        else
        {
            updateSidebarBpm(stepSeqPanel_.getBpm());
        }
    };

    addAndMakeVisible(sidebarPlayBtn_);
    addAndMakeVisible(sidebarTapBtn_);
    addAndMakeVisible(sidebarBpmLabel_);
    addAndMakeVisible(aiCloud_);

    // ── EWI Synth section ─────────────────────────────────────────────────────
    styleSideBtn(serumLoadBtn_, "SERUM");
    styleSideBtn(serumShowUiBtn_, "SHOW");
    serumShowUiBtn_.onClick = [this] { openSerumEditor(); };
    addAndMakeVisible(serumShowUiBtn_);

    serumLoadBtn_.onClick = [this]
    {
        static const juce::String kSerumPath {
            "C:\\Program Files\\Common Files\\VST3\\Serum2.vst3\\Contents\\x86_64-win\\Serum2.vst3"
        };
        loadSerumPlugin(kSerumPath);
    };
    addAndMakeVisible(serumLoadBtn_);

    styleSideBtn(swamLoadBtn_, "SWAM");
    swamLoadBtn_.onClick = [this]
    {
        static const juce::String kSwamPath {
            "C:\\Program Files\\Common Files\\VST3\\SWAM\\Trumpets\\SWAM Trumpet.vst3"
        };
        loadSerumPlugin(kSwamPath);
    };
    addAndMakeVisible(swamLoadBtn_);

    serumStatusLabel_.setText("not loaded", juce::dontSendNotification);
    serumStatusLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(9.f)));
    serumStatusLabel_.setColour(juce::Label::textColourId,
                                juce::Colour(0xFF888888));
    serumStatusLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(serumStatusLabel_);

    ewiDeviceEditor_.setText("EWI");
    ewiDeviceEditor_.setFont(juce::Font(juce::FontOptions{}.withHeight(11.f)));
    ewiDeviceEditor_.setColour(juce::TextEditor::textColourId,
                               juce::Colour(0xFFE5E2E3));
    ewiDeviceEditor_.setColour(juce::TextEditor::backgroundColourId,
                               juce::Colour(0xFF2A2A2A));
    ewiDeviceEditor_.setColour(juce::TextEditor::outlineColourId,
                               juce::Colour(0xFF4CDFA8));
    ewiDeviceEditor_.onReturnKey = [this]
        { midiManager_.setEwiDeviceName(ewiDeviceEditor_.getText()); };
    ewiDeviceEditor_.onFocusLost = [this]
        { midiManager_.setEwiDeviceName(ewiDeviceEditor_.getText()); };
    addAndMakeVisible(ewiDeviceEditor_);

    // ── MIDI Learn panel ──────────────────────────────────────────────────────
    midiLearnBindings_.resize(midi::kNumTargets);
    for (int i = 0; i < midi::kNumTargets; ++i)
        midiLearnBindings_[static_cast<std::size_t>(i)].target =
            static_cast<midi::MappingTarget>(i);

    for (int i = 0; i < midi::kNumTargets; ++i)
    {
        const std::size_t si = static_cast<std::size_t>(i);
        const auto t = static_cast<midi::MappingTarget>(i);

        mlTargetLabels_[si].setText(midi::mappingTargetName(t), juce::dontSendNotification);
        mlTargetLabels_[si].setFont(juce::Font(juce::FontOptions{}.withHeight(10.f)));
        mlTargetLabels_[si].setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        midiLearnPanel_.addAndMakeVisible(mlTargetLabels_[si]);

        mlCcLabels_[si].setText("--", juce::dontSendNotification);
        mlCcLabels_[si].setFont(juce::Font(juce::FontOptions{}.withHeight(10.f)));
        mlCcLabels_[si].setColour(juce::Label::textColourId, juce::Colour(0xFF4CDFA8));
        mlCcLabels_[si].setJustificationType(juce::Justification::centred);
        midiLearnPanel_.addAndMakeVisible(mlCcLabels_[si]);

        mlLearnBtns_[si].setButtonText("LEARN");
        mlLearnBtns_[si].onClick = [this, i] { startLearning(i); };
        midiLearnPanel_.addAndMakeVisible(mlLearnBtns_[si]);

        mlClearBtns_[si].setButtonText("X");
        mlClearBtns_[si].onClick = [this, i] { clearMapping(i); };
        midiLearnPanel_.addAndMakeVisible(mlClearBtns_[si]);
    }

    addChildComponent(midiLearnPanel_);  // invisible par défaut

    midiLearnBtn_.setButtonText("MIDI LEARN");
    midiLearnBtn_.setClickingTogglesState(true);
    midiLearnBtn_.onClick = [this] {
        midiLearnVisible_ = midiLearnBtn_.getToggleState();
        midiLearnPanel_.setVisible(midiLearnVisible_);
        resized();
        repaint();
    };
    addAndMakeVisible(midiLearnBtn_);

    // ── Project buttons (masqués — accessibles via menu FILES) ───────────────
    saveProjectButton_.setButtonText("SAVE PROJECT");
    saveProjectButton_.onClick = [this] { saveProject(); };

    loadProjectButton_.setButtonText("LOAD PROJECT");
    loadProjectButton_.onClick = [this] { doLoadProject(); };

    // ── Menu FILES (barre de titre) ───────────────────────────────────────────
    filesMenuButton_.setButtonText("FILES");
    filesMenuButton_.onClick = [this] { showFilesMenu(); };
    addAndMakeVisible(filesMenuButton_);

    // ── Step Sequencer panel ──────────────────────────────────────────────────

    // File loaded: read PCM, pitch-match if needed, push to Sampler
    stepSeqPanel_.onSlotFileLoaded = [this](int slot, std::string path)
    {
        // Moteur V2 : l'import lit/analyse le fichier (rôles, BPM, tonalité)
        // et charge le PCM dans SlotPlayer. L'UI est notifiée une fois l'analyse prête.
        facade_.importSampleAsync(slot, path, [this](int s, const engine::AnalysisResult&) {
            juce::MessageManager::callAsync([this, s] {
                stepSeqPanel_.setSlotLoaded(s, true);
                stepSeqPanel_.setSlotWaveform(s, computeEnvelope(facade_.getSlotPcmSnapshot(s)));
            });
        });
    };

    // Step toggled: stop sample immediately if the track has no more active steps
    stepSeqPanel_.onStepChanged = [this](int track, int step, bool active)
    {
        stepSequencer_.setStep(track, step, active);  // sync pour le project save
        facade_.setStep(track, step, active);
        facade_.flipPatternBuffer();
    };

    // Slot cleared: unload PCM and clear engine state
    stepSeqPanel_.onSlotCleared = [this](int slot)
    {
        facade_.clearSlot(slot);
        stepSeqPanel_.setSlotLoaded(slot, false);
    };

    // BPM changed: update sequencer + DSP + sidebar label
    stepSeqPanel_.onBpmChanged = [this](float bpm)
    {
        stepSequencer_.setBpm(bpm);
        serumHost_.setBpm(bpm);
        updateSidebarBpm(bpm);
        facade_.setBpm(bpm);
    };

    // Play/stop — StepSequencerPanel already calls seq_.setPlaying(); stop samples immediately
    stepSeqPanel_.onPlayChanged = [this](bool playing)
    {
        if (playing) {
            facade_.flipPatternBuffer();
            facade_.play();
        } else {
            facade_.stop();
        }
        serumHost_.setIsPlaying(playing);
        if (playing)
            serumHost_.resetPlaybackPosition();
        else
            serumHost_.sendAllNotesOff();
    };

    // Volume per slot — user fader, multiplied on top of AI normalization gain
    stepSeqPanel_.onVolumeChanged = [this](int slot, float userGain)
    {
        auto& sc = sceneManager_.scene(sceneManager_.currentIdx());
        sc.userGains[static_cast<std::size_t>(slot)] = userGain;
        facade_.setSlotGain(slot, userGain);
    };

    // Mute per slot — relancer le magic mix ici causait une perte de gain sur les autres
    // slots (l'IA recalcule l'équilibre avec un slot de plus/moins et peut produire gain≈0
    // sur la basse ou un autre slot). Le mute est appliqué directement ; le prochain
    // triggerAI() (bouton ⚡) reprendra l'état de mute courant pour recalibrer.
    stepSeqPanel_.onMutedChanged = [this](int slot, bool muted)
    {
        const bool quantize = !muted && stepSequencer_.isPlaying();
        facade_.setSlotMuted(slot, muted, quantize);
    };

    // Magic Mix ⚡ — le callback du panel (backup, au cas où) n'est plus utilisé pour le toggle
    stepSeqPanel_.onMagicButtonPressed = nullptr;

    // ── Looper ────────────────────────────────────────────────────────────────
    stepSeqPanel_.onLooperPress        = [this] { looperEngine_.pressButton(); };
    stepSeqPanel_.onLooperClear        = [this] { looperEngine_.clear(); };
    stepSeqPanel_.onLooperModeChanged  = [this](bool tape)
    {
        looperEngine_.setOverdubMode(tape ? ::dsp::LooperEngine::OverdubMode::Tape
                                          : ::dsp::LooperEngine::OverdubMode::Replace);
    };
    stepSeqPanel_.getLooperState  = [this] { return static_cast<int>(looperEngine_.getState()); };
    stepSeqPanel_.getLooperBars   = [this] { return looperEngine_.getLoopBars(); };
    stepSeqPanel_.getLooperIsTape = [this]
    {
        return looperEngine_.getOverdubMode() == ::dsp::LooperEngine::OverdubMode::Tape;
    };

    // ── Swing ─────────────────────────────────────────────────────────────────
    stepSeqPanel_.onSwingChanged = [this](float v) { stepSequencer_.setSwing(v); };

    // ── Magic mix V2 (worker EngineFacade) — callback unique de fin ────────────
    // Le worker V2 détecte les types + applique le mix d'un bloc (aucun rechargement
    // PCM — invariant transparence) puis appelle ce callback sur le message thread.
    // Distinction apply/revert via facade_.isMagicActive().
    facade_.setMagicMixDoneCallback([this]
    {
        // Per-slot content-type accent colours (mirrors StepSequencerPanel::trackColour)
        static const juce::Colour kSlotColours[9] = {
            juce::Colour { 0xFF4CDFA8 }, juce::Colour { 0xFF06B6D4 },
            juce::Colour { 0xFFC8C7C7 }, juce::Colour { 0xFF8B5CF6 },
            juce::Colour { 0xFFF97316 }, juce::Colour { 0xFFF43F5E },
            juce::Colour { 0xFFEAB308 }, juce::Colour { 0xFF38BDF8 },
            juce::Colour { 0xFFFF6B35 },  // deep-orange (DRM)
        };

        if (!facade_.isMagicActive())
        {
            // Revert : effacer les tags + reset spatial viz. Le PCM n'est jamais
            // modifié en V2 → aucun re-trim / re-reload nécessaire.
            for (int i = 0; i < 9; ++i)
                stepSeqPanel_.setSlotContentType(i, "");
            stepSeqPanel_.setMagicActive(false);
            spatialViz_.resetAll();
        }
        else
        {
            // Apply : tags de type + spatial viz + dub delay ON.
            for (int i = 0; i < 9; ++i)
            {
                const auto type   = facade_.getDetectedType(i);
                const bool loaded = !facade_.slotFilePath(i).empty();
                if (loaded)
                    stepSeqPanel_.setSlotContentType(
                        i, engine::mix::contentTypeName(type));

                // pan/width/depth sont appliqués au runtime par le worker V2
                // (setSlotMixState) ; on ne fait que refléter l'état ici.
                const auto sp = engine::mix::spatialForType(i, type);
                spatialViz_.setSlotState(i, sp.pan, sp.width, sp.depth,
                                         loaded, kSlotColours[i]);
            }
            spatialViz_.setSaxActive(true);
            stepSeqPanel_.setMagicActive(true);

            // Sync visuel du bouton ON (cohérent avec le comportement V1)
            juce::MessageManager::callAsync([this]
            {
                dubDelayEnableBtn_.setToggleState(true, juce::dontSendNotification);
            });

            // Appliquer les gains IA comme point de départ des faders + scènes
            // (persistance projet). L'application runtime a déjà été faite par le
            // worker (setSlotMixState → SlotPlayer).
            auto& sc = sceneManager_.scene(sceneManager_.currentIdx());
            for (int i = 0; i < 9; ++i)
            {
                const auto ms = facade_.getSlotMixState(i);
                if (!ms.applied) continue;
                if (ms.gain <= 0.f) continue;   // gain 0 → garder le gain existant
                const std::size_t idx = static_cast<std::size_t>(i);
                sc.gains[idx]     = ms.gain;    // référence/diagnostique uniquement
                sc.userGains[idx] = ms.gain;    // le gain IA devient le point de départ du fader
                stepSeqPanel_.setSlotVolume(i, ms.gain);   // le slider se cale sur la suggestion IA
            }

            // Normalise le gain Serum vers un niveau RMS cible (≈ −14 dBFS).
            // Déclenché une seule fois par run magic mix — n'écrase pas les réglages manuels.
            if (serumHost_.isLoaded() && serumMixFeatures_.rms > 0.01f)
            {
                constexpr float kSerumTargetRms = 0.20f;
                const float normGain = juce::jlimit(0.2f, 3.0f,
                    kSerumTargetRms / (serumMixFeatures_.rms + 1e-6f));
                serumUserGain_.store(normGain, std::memory_order_relaxed);
                sc.serumGain = normGain;
            }
        }

        mixStateDirty_ = true;
    });

    // Override manuel du type par slot (right-click sur l'indicateur)
    stepSeqPanel_.onTypeOverrideChanged = [this](int slot, int typeIndex)
    {
        facade_.setManualTypeOverride(slot, typeIndex);
        // Re-lancer l'IA avec le nouveau type
        triggerAI();
    };

    // Éditeur de sample (bouton [ED])
    stepSeqPanel_.onEditPressed = [this](int slot) { openSampleEditor(slot); };

    // ── Clipboard track ───────────────────────────────────────────────────────
    stepSeqPanel_.onTrackCopyRequest = [this](int slot)
    {
        auto& cb = trackClipboard_;
        cb.steps    = {};
        const int numSteps = stepSequencer_.getTrackStepCount(slot);
        for (int s = 0; s < numSteps; ++s)
            cb.steps[static_cast<std::size_t>(s)] = stepSequencer_.getStep(slot, s);
        cb.barCount = stepSequencer_.getTrackBarCount(slot);
        cb.filePath = stepSeqPanel_.getSlotFilePath(slot);
        cb.gain     = dspPipeline_.getSampler().getSlotGain(slot);
        cb.muted    = dspPipeline_.getSampler().isSlotMuted(slot);
        cb.valid    = true;
        stepSeqPanel_.hasPasteData = true;
    };

    stepSeqPanel_.onTrackPasteRequest = [this](int slot)
    {
        if (!trackClipboard_.valid) return;
        captureCurrentScene();
        const auto& cb = trackClipboard_;

        // Pattern
        stepSequencer_.setTrackBarCount(slot, cb.barCount);
        stepSeqPanel_.setTrackStepCount(slot, cb.barCount * 16);
        const int numSteps = cb.barCount * 16;
        facade_.setTrackBarCount(slot, cb.barCount);
        for (int s = 0; s < numSteps; ++s)
        {
            stepSequencer_.setStep(slot, s, cb.steps[static_cast<std::size_t>(s)]);
            stepSeqPanel_.setStepState(slot, s, cb.steps[static_cast<std::size_t>(s)]);
            facade_.setStep(slot, s, cb.steps[static_cast<std::size_t>(s)]);
        }
        facade_.flipPatternBuffer();

        // Gain + mute
        dspPipeline_.getSampler().setSlotGain(slot, cb.gain);
        dspPipeline_.getSampler().setSlotMuted(slot, cb.muted);
        stepSeqPanel_.setSlotMuted(slot, cb.muted);

        // Sample (si différent)
        if (!cb.filePath.empty() && cb.filePath != stepSeqPanel_.getSlotFilePath(slot))
        {
            loadSampleIntoSlot(slot, cb.filePath);
            stepSeqPanel_.setSlotFilePath(slot, cb.filePath);
        }

        // Mettre à jour la scène courante en mémoire
        auto& sc = sceneManager_.scene(sceneManager_.currentIdx());
        for (int s = 0; s < numSteps; ++s)
            sc.steps[static_cast<std::size_t>(slot)][static_cast<std::size_t>(s)] =
                cb.steps[static_cast<std::size_t>(s)];
        sc.gains          [static_cast<std::size_t>(slot)] = cb.gain;
        sc.mutes          [static_cast<std::size_t>(slot)] = cb.muted;
        sc.trackBarCounts [static_cast<std::size_t>(slot)] = cb.barCount;
        if (!cb.filePath.empty())
            sc.filePaths  [static_cast<std::size_t>(slot)] = cb.filePath;
    };

    stepSeqPanel_.onPitchOffsetChanged = [this](int slot, float semitones)
    {
        onPitchOffsetChanged(slot, semitones);
    };

    // Changement de longueur de pattern via le menu pageLabel_
    stepSeqPanel_.onTrackBarCountChanged = [this](int track, int bars)
    {
        stepSequencer_.setTrackBarCount(track, bars);   // sync pour le project save
        facade_.setTrackBarCount(track, bars);
        facade_.flipPatternBuffer();
    };

    // Override manuel du mode de lecture (clic droit → "Mode de lecture...")
    stepSeqPanel_.onSlotModeChanged = [this](int slot, int mode)
    {
        const engine::PlayMode pm = (mode == 1) ? engine::PlayMode::Free
                                  : (mode == 2) ? engine::PlayMode::LoopSync
                                  :               engine::PlayMode::OneShot;
        facade_.setSlotMode(slot, pm);
    };

    // Playhead ratio for waveform animation (approx — audio thread value, GUI read)
    stepSeqPanel_.getSlotPlayhead = [this](int slot) -> float
    {
        return facade_.getSlotPlayheadRatio(slot);
    };

    // VU meter — real per-slot output peak from audio thread (reflects gain/mute)
    stepSeqPanel_.getSlotLevel = [this](int slot) -> float
    {
        return facade_.getSlotOutputPeak(slot);
    };

    // Solo per slot
    stepSeqPanel_.onSoloChanged = [this](int slot, bool soloed)
    {
        facade_.setSlotSolo(slot, soloed);
    };

    // Ducking display — le ducking V2 est géré en interne par AutoMix (sidechain)
    // et n'est pas exposé à l'UI ; on renvoie 1.0 (pas de duck externe visible).
    stepSeqPanel_.getDuckingGain = [this]() -> float
    {
        return 1.0f;
    };

    // Input RMS — RMS maître du graphe V2.
    stepSeqPanel_.getInputRms = [this]() -> float
    {
        return facade_.getMasterRms();
    };


    addAndMakeVisible(stepSeqPanel_);

    // ── Master key selector (sidebar) ─────────────────────────────────────────
    // ── Dub Delay global bus
    dubDelayLabel_.setText("DUB DELAY", juce::dontSendNotification);
    dubDelayLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(9.5f).withStyle("Bold")));
    dubDelayLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFF4CDFA8));
    dubDelayLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(dubDelayLabel_);

    dubDelayEnableBtn_.setButtonText("ON");
    dubDelayEnableBtn_.setClickingTogglesState(true);
    dubDelayEnableBtn_.onStateChange = [this] {
        facade_.delay().setEnabled(dubDelayEnableBtn_.getToggleState());
    };
    addAndMakeVisible(dubDelayEnableBtn_);

    dubDelayFreezeBtn_.setButtonText("FREEZE");
    dubDelayFreezeBtn_.setClickingTogglesState(true);
    dubDelayFreezeBtn_.onStateChange = [this] {
        facade_.delay().setFreeze(dubDelayFreezeBtn_.getToggleState());
    };
    addAndMakeVisible(dubDelayFreezeBtn_);

    auto setupDubSlider = [](juce::Slider& s, double lo, double hi, double def) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setRange(lo, hi, 0.01);
        s.setValue(def, juce::dontSendNotification);
    };
    setupDubSlider(dubDelaySendSlider_,     0.0, 1.0,  0.20);
    setupDubSlider(dubDelayWetSlider_,      0.0, 1.0,  0.28);
    setupDubSlider(dubDelayFeedbackSlider_, 0.0, 0.95, 0.48);
    setupDubSlider(dubDelayToneSlider_,     0.0, 1.0,  0.55);
    setupDubSlider(dubDelayDriveSlider_,    0.0, 1.0,  0.15);

    dubDelaySendSlider_    .onValueChange = [this] {
        facade_.delay().setSend(static_cast<float>(dubDelaySendSlider_.getValue()));
    };
    dubDelayWetSlider_     .onValueChange = [this] {
        facade_.delay().setWet(static_cast<float>(dubDelayWetSlider_.getValue()));
    };
    dubDelayFeedbackSlider_.onValueChange = [this] {
        facade_.delay().setFeedback(static_cast<float>(dubDelayFeedbackSlider_.getValue()));
    };
    dubDelayToneSlider_    .onValueChange = [this] {
        facade_.delay().setTone(static_cast<float>(dubDelayToneSlider_.getValue()));
    };
    dubDelayDriveSlider_   .onValueChange = [this] {
        facade_.delay().setDrive(static_cast<float>(dubDelayDriveSlider_.getValue()));
    };

    addAndMakeVisible(dubDelaySendSlider_);
    addAndMakeVisible(dubDelayWetSlider_);
    addAndMakeVisible(dubDelayFeedbackSlider_);
    addAndMakeVisible(dubDelayToneSlider_);
    addAndMakeVisible(dubDelayDriveSlider_);

    dubDelayDivCombo_.addItem("1/8",   1);
    dubDelayDivCombo_.addItem("1/4",   2);
    dubDelayDivCombo_.addItem("1/2",   3);
    dubDelayDivCombo_.addItem("1 bar", 4);
    dubDelayDivCombo_.setSelectedId(2, juce::dontSendNotification);
    dubDelayDivCombo_.onChange = [this] {
        facade_.delay().setDiv(dubDelayDivCombo_.getSelectedId() - 1);
    };
    addAndMakeVisible(dubDelayDivCombo_);

    static const char* kNoteNames[12] = {
        "C","C#","D","Eb","E","F","F#","G","Ab","A","Bb","B"
    };
    // ID 100 = sentinelle "Aucune" (pas de tonalité définie) ; IDs 1-12 = notes C-B.
    masterKeyCombo_.addItem("Aucune", 100);
    for (int i = 0; i < 12; ++i)
        masterKeyCombo_.addItem(kNoteNames[i], i + 1);
    masterKeyCombo_.setSelectedId(100, juce::dontSendNotification);
    masterKeyCombo_.onChange = [this] {
        const int sid = masterKeyCombo_.getSelectedId();
        if (sid == 100) {
            masterKeyRoot_      = -1;
            masterKeySetByUser_ = false;  // retour à "Aucune" → désarme key-match
        } else {
            const int newRoot = sid - 1;
            // Arme uniquement sur un vrai changement vers une tonalité réelle.
            // Protège contre onChange fantôme (JUCE peut le déclencher sans vrai changement).
            if (newRoot != masterKeyRoot_)
                masterKeySetByUser_ = true;
            masterKeyRoot_ = newRoot;
        }
        // Mode combo n'a de sens que si une tonique réelle est définie.
        masterKeyModeCombo_.setEnabled(masterKeyRoot_ >= 0);
        applyMasterKey();
    };
    addAndMakeVisible(masterKeyCombo_);

    masterKeyModeCombo_.addItem("Major", 1);
    masterKeyModeCombo_.addItem("Minor", 2);
    masterKeyModeCombo_.setSelectedId(1, juce::dontSendNotification);
    masterKeyModeCombo_.setEnabled(false);  // désactivé tant que la tonique est "Aucune"
    masterKeyModeCombo_.onChange = [this] {
        masterKeyMajor_ = (masterKeyModeCombo_.getSelectedId() == 1);
        applyMasterKey();
    };
    addAndMakeVisible(masterKeyModeCombo_);

    // ── Portée musicale + gamme ───────────────────────────────────────────────
    scaleTypeCombo_.addItem("Major",          1);
    scaleTypeCombo_.addItem("Minor",          2);
    scaleTypeCombo_.addItem("Pentatonic Maj", 3);
    scaleTypeCombo_.addItem("Pentatonic Min", 4);
    scaleTypeCombo_.addItem("Blues",          5);
    scaleTypeCombo_.addItem("Dorian",         6);
    scaleTypeCombo_.setSelectedId(1, juce::dontSendNotification);
    scaleTypeCombo_.onChange = [this] {
        const auto t = static_cast<ui::ScaleType>(scaleTypeCombo_.getSelectedId() - 1);
        scaleStaff_.setKey(masterKeyRoot_, t);
    };
    addAndMakeVisible(scaleTypeCombo_);
    addAndMakeVisible(scaleStaff_);
    scaleStaff_.setKey(masterKeyRoot_, ui::ScaleType::Major);

    // ── Spatial visualization ─────────────────────────────────────────────────
    addAndMakeVisible(spatialViz_);

    // Initial BPM
    stepSequencer_.setBpm(120.f);
    dspPipeline_.setBpm(120.f);
    serumHost_.setBpm(120.f);
    looperEngine_.setBpm(120.f);

    // Auto-lancement IA au démarrage (après init audio)
    juce::MessageManager::callAsync([this] { triggerAI(); });

    setSize(1280, 900);
    setAudioChannels(1, 2);
    midiManager_.start(deviceManager);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    // Restore default look and feel and release SAX-OS look
    setLookAndFeel(nullptr);
    saxOsLookAndFeel_.reset();
    stopTimer();
    midiManager_.stop();
    shutdownAudio();
}

//==============================================================================

void MainComponent::loadSampleIntoSlot(int slot, const std::string& path,
                                        int trimStart, int trimEnd,
                                        double* outFileSr)
{
    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();

    const juce::File file { juce::String(path) };
    std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(file));
    if (!reader) return;

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    if (numSamples <= 0) return;

    const int numCh = std::max(1, static_cast<int>(reader->numChannels));
    juce::AudioBuffer<float> buf(numCh, numSamples);
    reader->read(&buf, 0, numSamples, 0, true, numCh > 1);
    std::vector<float> pcm(numSamples);
    if (numCh > 1) {
        const float* left = buf.getReadPointer(0);
        const float* right = buf.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i) pcm[i] = (left[i] + right[i]) * 0.5f;
    } else {
        std::copy(buf.getReadPointer(0), buf.getReadPointer(0) + numSamples, pcm.begin());
    }

    const double fileSr = reader->sampleRate;
    if (outFileSr) *outFileSr = fileSr;

    // Apply trim inline so that only one loadSample() call is made per scene
    // transition — avoids a second reloadSlotData() write that would race with
    // a voice still reading the background buffer during its fadeOut.
    const int start = juce::jlimit(0, numSamples - 1, trimStart);
    const int end   = (trimEnd >= 0)
                      ? juce::jlimit(start + 1, numSamples, trimEnd)
                      : numSamples;

    static constexpr bool kSlotLoop[9] = { true, true, false, false, false, true, true, false, true };
    auto& sampler = dspPipeline_.getSampler();
    sampler.loadSample(slot, pcm.data() + start, end - start, fileSr);
    sampler.setSlotOneShot(slot, true);
    if (slot >= 0 && slot < 9)
        sampler.setSlotLoop(slot, kSlotLoop[slot]);

    stepSeqPanel_.setSlotLoaded(slot, true);

    {
        auto snap = sampler.getSlotPcmSnapshot(slot);
        stepSeqPanel_.setSlotWaveform(slot, computeEnvelope(snap));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// applyMasterKey — propagate master key to MusicContext
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::applyMasterKey()
{
    {
        const auto t = static_cast<ui::ScaleType>(scaleTypeCombo_.getSelectedId() - 1);
        scaleStaff_.setKey(masterKeyRoot_, t);  // -1 = Aucune → rebuildNoteInfos() efface la portée
    }
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
    auto pcm = facade_.getSlotPcmSnapshot(slot);
    if (pcm.empty()) return;
    const float slotSr = facade_.getSlotPcmSampleRate(slot);
    const float sr     = (slotSr > 0.f) ? slotSr : static_cast<float>(currentSampleRate_);

    // Arrêter le morceau pour permettre d'écouter le sample en isolation
    if (stepSequencer_.isPlaying())
        stepSeqPanel_.triggerPlay();

    const juce::String fileName =
        juce::File(stepSeqPanel_.getSlotFilePath(slot))
            .getFileNameWithoutExtension();
    const juce::String title =
        "Edit Sample  —  " + (fileName.isEmpty() ? "S" + juce::String(slot + 1) : fileName)
        + "  [S" + juce::String(slot + 1) + "]";

    // Offset du snapshot dans le fichier d'origine (coordonnées fichier).
    // Le snapshot du sampler est déjà tronqué par applyScene — les indices retournés
    // par l'éditeur sont relatifs à ce snapshot, pas au fichier complet.
    // On additionne cet offset pour que trimStart/trimEnd restent toujours en
    // coordonnées fichier, quel que soit le nombre d'éditions successives.
    const int fileTrimOffset =
        sceneManager_.scene(sceneManager_.currentIdx())
                     .trimStart[static_cast<std::size_t>(slot)];

    auto* editor = new ui::SampleEditorComponent(std::move(pcm), sr);

    editor->onApply = [this, slot, fileTrimOffset](int startSample, int endSample)
    {
        auto snapshot = facade_.getSlotPcmSnapshot(slot);
        const int total = static_cast<int>(snapshot.size());
        const int s = juce::jlimit(0, total - 1, startSample);
        const int e = juce::jlimit(s + 1, total,  endSample);
        std::vector<float> trimmed(snapshot.begin() + s, snapshot.begin() + e);
        const float sr = facade_.getSlotPcmSampleRate(slot);

        // Appliquer au slot courant (moteur V2 : reload PCM trimé)
        facade_.reloadSlotPcm(slot, trimmed, sr);

        // Convertir en coordonnées fichier : ajouter l'offset du snapshot courant.
        // Sans cette correction, chaque réouverture de projet décale le trim du
        // nombre de samples correspondant au trimStart de la scène précédente.
        const int fileS = fileTrimOffset + s;
        const int fileE = fileTrimOffset + e;

        const std::string filePath = stepSeqPanel_.getSlotFilePath(slot);
        for (int si = 0; si < kMaxScenes; ++si)
            for (int t = 0; t < 9; ++t)
                if (sceneManager_.scene(si).filePaths[t] == filePath)
                {
                    sceneManager_.scene(si).trimStart[t] = fileS;
                    sceneManager_.scene(si).trimEnd  [t] = fileE;
                }

        // Config V2 : re-sync la configuration scène (trim stocké dans SceneStore).
        for (int si = 0; si < kMaxScenes; ++si)
            for (int t = 0; t < 9; ++t)
                if (sceneManager_.scene(si).filePaths[t] == filePath)
                {
                    // Mettre à jour le SceneStore pour que les futures transitions
                    // voient le bon trim (Replace) et le re-trigger cohérent.
                    syncV2Scene(si);
                }

        // Appliquer aux autres slots actuellement chargés avec le même fichier
        for (int t = 0; t < 9; ++t)
            if (t != slot && stepSeqPanel_.getSlotFilePath(t) == filePath)
                facade_.reloadSlotPcm(t, trimmed, sr);
    };

    editor->onPlayRequested = [this, slot]()
    {
        facade_.triggerSlot(slot);
    };

    editor->onStopRequested = [this, slot]()
    {
        facade_.stopSlot(slot, true);
    };

    editor->getPlayheadRatio = [this, slot]() -> float
    {
        return facade_.getSlotPlayheadRatio(slot);
    };

    editor->isSlotPlaying = [this, slot]() -> bool
    {
        return facade_.isSlotPlaying(slot);
    };

    editor->onClose = [this]() { sampleEditorWindow_.reset(); };

    // Fix 1 — fenêtre non-modale : le morceau continue de jouer
    sampleEditorWindow_.reset();
    sampleEditorWindow_ = std::make_unique<SampleEditorWindow>(title, juce::Colour(0xFF0A0A0A));
    sampleEditorWindow_->onClose = [this]() { sampleEditorWindow_.reset(); };
    sampleEditorWindow_->setUsingNativeTitleBar(false);
    sampleEditorWindow_->setContentOwned(editor, true);
    sampleEditorWindow_->setResizable(false, false);
    sampleEditorWindow_->centreWithSize(720, 300);
    sampleEditorWindow_->setVisible(true);
}

void MainComponent::saveProjectToFile(const juce::File& f)
{
    auto path = f.getFullPathName();
    if (!path.endsWithIgnoreCase(".saxfx"))
        path += ".saxfx";

    project::ProjectData data;
    data.projectName = "SaxFX Project";
    data.bpm         = stepSequencer_.getBpm();

    // ── Music context ─────────────────────────────────────────────────────
    data.musicContext.keyRoot = masterKeyRoot_;
    data.musicContext.isMajor = masterKeyMajor_;

    // ── Sampler slots + step patterns ─────────────────────────────────────
    auto& sampler = dspPipeline_.getSampler();
    for (int i = 0; i < 9; ++i)
    {
        auto& sc    = data.samples[static_cast<std::size_t>(i)];
        sc.filePath = stepSeqPanel_.getSlotFilePath(i);
        sc.gain     = sampler.getSlotGain(i);
        sc.loop     = false;
        sc.oneShot  = true;
        sc.muted    = sampler.isSlotMuted(i);
        sc.gridDiv  = 0;
        for (int s = 0; s < 16; ++s)
            sc.stepPattern[s] = stepSequencer_.getStep(i, s);
    }

    // ── AI mix states ─────────────────────────────────────────────────────
    for (int i = 0; i < 9; ++i)
    {
        const auto ms = facade_.getSlotMixState(i);
        auto& sm      = data.slotMix[static_cast<std::size_t>(i)];
        sm.gain    = ms.gain;
        sm.pan     = ms.pan;
        sm.width   = ms.width;
        sm.depth   = ms.depth;
        sm.applied = ms.applied;
    }

    // ── Master key ────────────────────────────────────────────────────────
    data.masterKeyRoot      = masterKeyRoot_;
    data.masterKeyMajor     = masterKeyMajor_;
    data.masterKeySetByUser = masterKeySetByUser_;

    // ── Scenes ────────────────────────────────────────────────────────────
    captureCurrentScene();
    data.currentScene = sceneManager_.currentIdx();
    for (int si = 0; si < kMaxScenes; ++si)
    {
        const auto& src = sceneManager_.scene(si);
        auto& dst       = data.scenes[static_cast<std::size_t>(si)];
        dst.used           = src.used;
        dst.bpm            = src.bpm;
        dst.filePaths      = src.filePaths;
        dst.mutes          = src.mutes;
        dst.gains          = src.gains;
        dst.userGains      = src.userGains;
        dst.trackBarCounts = src.trackBarCounts;
        dst.trimStart      = src.trimStart;
        dst.trimEnd        = src.trimEnd;
        dst.delaySends     = src.delaySends;
        dst.serumGain        = src.serumGain;
        dst.serumState       = src.serumState;
        dst.serumPresetName  = src.serumPresetName;
        dst.dubDelayFeedback = src.dubDelayFeedback;
        dst.dubDelayWet      = src.dubDelayWet;
        dst.dubDelayTone     = src.dubDelayTone;
        dst.dubDelayDrive    = src.dubDelayDrive;
        dst.pitchOffsets     = src.pitchOffsets;
        for (int t = 0; t < 9; ++t)
        {
            const int numSteps = src.trackBarCounts[static_cast<std::size_t>(t)] * 16;
            for (int s = 0; s < numSteps; ++s)
                dst.steps[static_cast<std::size_t>(t)][static_cast<std::size_t>(s)] =
                    src.steps[static_cast<std::size_t>(t)][static_cast<std::size_t>(s)];
        }
    }

    // ── v11 — dub delay global bus ────────────────────────────────────────────
    data.dubDelayEnabled  = dubDelayEnableBtn_.getToggleState();
    data.dubDelaySend     = static_cast<float>(dubDelaySendSlider_    .getValue());
    data.dubDelayWet      = static_cast<float>(dubDelayWetSlider_     .getValue());
    data.dubDelayFeedback = static_cast<float>(dubDelayFeedbackSlider_.getValue());
    data.dubDelayTone     = static_cast<float>(dubDelayToneSlider_    .getValue());
    data.dubDelayDrive    = static_cast<float>(dubDelayDriveSlider_   .getValue());
    data.dubDelayDiv      = dubDelayDivCombo_.getSelectedId() - 1;

    // ── v12 — MIDI learn bindings ─────────────────────────────────────────────
    data.midiLearnEntries.clear();
    for (const auto& b : midiLearnBindings_)
    {
        if (b.cc < 0) continue;
        project::MidiLearnEntry e;
        e.target = static_cast<int>(b.target);
        e.cc     = b.cc;
        e.min    = b.min;
        e.max    = b.max;
        data.midiLearnEntries.push_back(e);
    }

    // ── Swing global ──────────────────────────────────────────────────────────
    data.swing = stepSequencer_.getSwing();

    // Serum preset state + name are captured per-scene in captureCurrentScene().

    if (project::ProjectLoader::save(data, path.toStdString()))
        juce::Logger::writeToLog("Project saved: " + path);
    else
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Save failed",
            "Could not write to " + path + ".");
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
            saveProjectToFile(results[0]);
        });
}

void MainComponent::showFilesMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "New Project");
    menu.addItem(2, "Load Project");
    menu.addItem(3, "Save Project");
    menu.addSeparator();
    menu.addItem(4, "Audio Settings");

    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(filesMenuButton_)
            .withMinimumWidth(180),
        [this](int result)
        {
            switch (result)
            {
                case 1: newProject();        break;
                case 2: doLoadProject();     break;
                case 3: saveProject();       break;
                case 4: openAudioSettings(); break;
                default: break;
            }
        });
}

void MainComponent::newProject()
{
    juce::AlertWindow::showOkCancelBox(
        juce::AlertWindow::WarningIcon,
        "New Project",
        "Creer un nouveau projet ?\nToutes les scenes seront effacees.",
        "Nouveau projet", "Annuler", nullptr,
        juce::ModalCallbackFunction::create([this](int result)
        {
            if (result == 1)
            {
                project::ProjectData empty{};
                applyProjectData(empty);
            }
        }));
}

void MainComponent::doLoadProject()
{
    auto chooser =
        std::make_shared<juce::FileChooser>("Open SaxFX Project", juce::File{}, "*.saxfx");

    chooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser](const juce::FileChooser& fc)
        {
            const auto results = fc.getResults();
            if (results.isEmpty()) return;

            auto data = project::ProjectLoader::load(results[0].getFullPathName().toStdString());
            if (data.has_value())
                applyProjectData(*data);
            else
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon,
                    "Load failed", "Could not parse the .saxfx file.");
        });
}

void MainComponent::openAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(
        deviceManager, 0, 1, 0, 2, true, false, false, false);
    selector->setSize(500, 400);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle            = "Audio Device Settings";
    options.dialogBackgroundColour = juce::Colours::darkgrey;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::triggerPanic() noexcept
{
    dspPipeline_.getSampler().stopAllSlots(::dsp::Sampler::StopMode::Instant);
    dspPipeline_.resetAllDelays();
    stepSequencer_.setPlaying(false);
}

void MainComponent::doAutosave()
{
    const auto dir = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory).getChildFile("DubEngine");
    dir.createDirectory();
    saveProjectToFile(dir.getChildFile(
        "autosave." + juce::String(autosaveSlot_) + ".saxfx"));
    autosaveSlot_     = (autosaveSlot_ + 1) % 3;
    autosaveFadeTimer_ = 120;
}

void MainComponent::applyProjectData(const project::ProjectData& data)
{
    // Restore BPM
    const float bpm = (data.bpm > 0.f) ? data.bpm : 120.f;
    stepSequencer_.setBpm(bpm);
    serumHost_.setBpm(bpm);
    stepSeqPanel_.setBpm(bpm);
    updateSidebarBpm(bpm);
    facade_.setBpm(bpm);

    // Load samples and restore step patterns
    for (int i = 0; i < 9; ++i)
    {
        const auto& sc = data.samples[static_cast<std::size_t>(i)];

        if (!sc.filePath.empty())
        {
            // Moteur V2 : l'import lit/analyse le fichier et charge le PCM
            // dans SlotPlayer (rôles, BPM, tonalité, mode de lecture).
            facade_.importSampleAsync(i, sc.filePath, [this](int s, const engine::AnalysisResult&) {
                juce::MessageManager::callAsync([this, s] {
                    stepSeqPanel_.setSlotLoaded(s, true);
                    stepSeqPanel_.setSlotWaveform(s, computeEnvelope(facade_.getSlotPcmSnapshot(s)));
                });
            });

            stepSeqPanel_.setSlotFilePath(i, sc.filePath);   // updates LCD name

            // Restore saved gain (fallback to unity if not yet set)
            const float savedGain = sc.gain > 0.f ? sc.gain : 1.0f;
            facade_.setSlotGain(i, savedGain);
        }
        else
        {
            // Vide explicitement le slot s'il n'y a pas de sample configuré
            facade_.clearSlot(i);
            stepSeqPanel_.setSlotFilePath(i, "");
            stepSeqPanel_.setSlotLoaded(i, false);
            stepSeqPanel_.setSlotWaveform(i, {});
        }

        for (int s = 0; s < 16; ++s)
        {
            const bool active = sc.stepPattern[s];
            stepSequencer_.setStep(i, s, active);
            stepSeqPanel_.setStepState(i, s, active);
            facade_.setStep(i, s, active);
        }
        facade_.setTrackBarCount(i, stepSequencer_.getTrackBarCount(i));

        stepSeqPanel_.setSlotMuted(i, sc.muted);
        facade_.setSlotMuted(i, sc.muted);
    }

    if (data.musicContext.keyRoot >= 0)
    {
        masterKeyRoot_  = data.musicContext.keyRoot;
        masterKeyMajor_ = data.musicContext.isMajor;
    }

    // Restore MIDI mappings
    midiManager_.clearMappings();
    for (const auto& m : data.midiMappings)
        midiManager_.setNoteMapping(m.midiNote, m.slotIndex);

    // ── v5 — master key ───────────────────────────────────────────────────────
    if (data.version >= 5)
    {
        masterKeyRoot_      = data.masterKeyRoot;
        masterKeyMajor_     = data.masterKeyMajor;
        masterKeySetByUser_ = data.masterKeySetByUser;  // lu directement, pas déduit
        masterKeyCombo_    .setSelectedId(masterKeyRoot_ >= 0 ? masterKeyRoot_ + 1 : 100,
                                          juce::dontSendNotification);
        masterKeyModeCombo_.setSelectedId(masterKeyMajor_ ? 1 : 2, juce::dontSendNotification);
        masterKeyModeCombo_.setEnabled(masterKeyRoot_ >= 0);
        applyMasterKey();
    }

    // ── v5 — AI mix states ────────────────────────────────────────────────────
    if (data.version >= 5)
    {
        auto& sampler = dspPipeline_.getSampler();
        for (int i = 0; i < 9; ++i)
        {
            const auto& sm = data.slotMix[static_cast<std::size_t>(i)];
            if (!sm.applied) continue;
            sampler.setSlotGain(i, sm.gain);
            sampler.setSlotPan(i, sm.pan);
            const int haasSamples = static_cast<int>(
                sm.width * 0.025 * currentSampleRate_);
            sampler.setSlotHaasDelay(i, haasSamples);
            facade_.setSlotMixState(i, sm.gain, sm.pan, sm.width, sm.depth);
        }
    }

    // ── v5 — scenes ───────────────────────────────────────────────────────────
    if (data.version >= 5)
    {
        sceneManager_.setCurrentIdx(std::clamp(data.currentScene, 0, kMaxScenes - 1));
        for (int si = 0; si < kMaxScenes; ++si)
        {
            const auto& src = data.scenes[static_cast<std::size_t>(si)];
            auto& dst       = sceneManager_.scene(si);
            dst.used           = src.used;
            dst.bpm            = src.bpm;
            dst.filePaths      = src.filePaths;
            dst.mutes          = src.mutes;
            dst.gains          = src.gains;
            dst.userGains      = src.userGains;
            dst.trackBarCounts = src.trackBarCounts;
            dst.trimStart      = src.trimStart;
            dst.trimEnd        = src.trimEnd;
            dst.delaySends     = src.delaySends;
            dst.serumGain        = src.serumGain;
            dst.serumState       = src.serumState;
            dst.serumPresetName  = src.serumPresetName;
            dst.dubDelayFeedback = src.dubDelayFeedback;
            dst.dubDelayWet      = src.dubDelayWet;
            dst.dubDelayTone     = src.dubDelayTone;
            dst.dubDelayDrive    = src.dubDelayDrive;
            dst.pitchOffsets     = src.pitchOffsets;
            for (int t = 0; t < 9; ++t)
            {
                const int numSteps = src.trackBarCounts[static_cast<std::size_t>(t)] * 16;
                for (int s = 0; s < numSteps; ++s)
                    dst.steps[static_cast<std::size_t>(t)]
                             [static_cast<std::size_t>(s)] =
                        src.steps[static_cast<std::size_t>(t)]
                                 [static_cast<std::size_t>(s)];
            }
        }
        // Pré-calcul de l'énergie pour toutes les scènes (depuis les patterns chargés).
        for (int si = 0; si < kMaxScenes; ++si)
            sceneManager_.setSceneEnergy(si,
                ::dsp::SceneManager::computeSceneEnergy(sceneManager_.scene(si)));

        // Restore bar counts, step patterns and sample paths for the current scene
        applyScene(sceneManager_.currentIdx());
        updateSceneLabel();

        // Seed toutes les scènes V1 → SceneStore moteur (pour transitions/scènes).
        syncV2Scenes();
    }

    // ── v11 — dub delay global bus ────────────────────────────────────────────
    if (data.version >= 11)
    {
        dubDelayEnableBtn_  .setToggleState(data.dubDelayEnabled,                        juce::dontSendNotification);
        dubDelaySendSlider_ .setValue      (static_cast<double>(data.dubDelaySend),      juce::dontSendNotification);
        dubDelayWetSlider_  .setValue      (static_cast<double>(data.dubDelayWet),       juce::dontSendNotification);
        dubDelayFeedbackSlider_.setValue   (static_cast<double>(data.dubDelayFeedback),  juce::dontSendNotification);
        dubDelayToneSlider_ .setValue      (static_cast<double>(data.dubDelayTone),      juce::dontSendNotification);
        dubDelayDriveSlider_.setValue      (static_cast<double>(data.dubDelayDrive),     juce::dontSendNotification);
        dubDelayDivCombo_   .setSelectedId (data.dubDelayDiv + 1,                        juce::dontSendNotification);

        facade_.delay().setEnabled (data.dubDelayEnabled);
        facade_.delay().setSend    (data.dubDelaySend);
        facade_.delay().setWet     (data.dubDelayWet);
        facade_.delay().setFeedback(data.dubDelayFeedback);
        facade_.delay().setTone    (data.dubDelayTone);
        facade_.delay().setDrive   (data.dubDelayDrive);
        facade_.delay().setDiv     (data.dubDelayDiv);
    }

    // ── v12 — MIDI learn bindings ─────────────────────────────────────────────
    if (data.version >= 12)
    {
        for (auto& b : midiLearnBindings_)
            b.cc = -1;

        for (const auto& e : data.midiLearnEntries)
        {
            if (e.target < 0 || e.target >= midi::kNumTargets) continue;
            if (e.cc < 0 || e.cc >= 128) continue;
            auto& b = midiLearnBindings_[static_cast<std::size_t>(e.target)];
            b.cc  = e.cc;
            b.min = e.min;
            b.max = e.max;
            // Initialiser le smoothing à la valeur CC courante
            targetSmoothed_[static_cast<std::size_t>(e.target)] =
                e.min + midiManager_.getCcValue(e.cc) * (e.max - e.min);
        }
        updateMidiLearnUI();
    }

    // Serum preset state + name are now per-scene — restored by applyScene().

    // ── Swing (absent dans anciens projets → 0 = straight) ───────────────────
    stepSequencer_.setSwing(data.swing);
    stepSeqPanel_.setSwing(data.swing);

    juce::Logger::writeToLog("Project loaded: " + juce::String(data.projectName));
}

//==============================================================================
void MainComponent::triggerAI()
{
    if (facade_.isMagicBusy()) return;

    // Le moteur V2 résout les types depuis les rôles du graphe (AudioGraph::slotRole)
    // + overrides manuels (setManualTypeOverride). Le contexte Serum (EWI/VST) est
    // poussé avant le lancement pour la compensation de masquage.
    facade_.setSerumContext(serumMixFeatures_.rms, serumMixFeatures_.spectralCentroid,
                            serumMixFeatures_.midFrac, serumMixFeatures_.highFrac,
                            serumContentTypeForMix(), serumHost_.isLoaded());
    facade_.triggerMagicMix();
}

//==============================================================================
// Audio callbacks
//==============================================================================

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate_ = sampleRate;
    currentBufferSize_ = samplesPerBlockExpected;

    stepSequencer_.prepare(sampleRate);

    facade_.prepare(sampleRate, samplesPerBlockExpected);
    facade_.setBpm(stepSeqPanel_.getBpm());
    facade_.setSerumHost(&serumHost_);
    facade_.setInputGain(v2InputGain_.load(std::memory_order_relaxed));
    facade_.startMixThread();
    v2InputScratchL_.assign(static_cast<size_t>(samplesPerBlockExpected), 0.f);
    v2InputScratchR_.assign(static_cast<size_t>(samplesPerBlockExpected), 0.f);

    serumSnapBuf_.assign(samplesPerBlockExpected, 0.f);

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

    // Drain EWI MIDI for this block and forward to Serum synth
    ewiMidiBuffer_.clear();
    midiManager_.consumeEwiMidi(ewiMidiBuffer_, numSamples);

    // Capture l'entrée dry (EWI/sax) avant que le rendu V2 n'écrase le buffer.
    // Utilisée par facade_.processBlock comme bus d'entrée externe.
    if (static_cast<int>(v2InputScratchL_.size()) < numSamples)
    {
        v2InputScratchL_.assign(static_cast<size_t>(numSamples), 0.f);
        v2InputScratchR_.assign(static_cast<size_t>(numSamples), 0.f);
    }
    std::copy(left, left + numSamples, v2InputScratchL_.begin());
    if (numCh >= 2)
        std::copy(bufferToFill.buffer->getWritePointer(1, bufferToFill.startSample),
                  bufferToFill.buffer->getWritePointer(1, bufferToFill.startSample) + numSamples,
                  v2InputScratchR_.begin());
    else
        std::copy(left, left + numSamples, v2InputScratchR_.begin());

    // Capture beat phase BEFORE step sequencer advances it (used by looper for bar detection)
    const double looperBeatPhase = stepSequencer_.getCurrentPhase();

    serumHost_.processBlock(ewiMidiBuffer_);

    // Looper: process Serum output in-place (mixes loop playback, records input)
    if (serumHost_.isLoaded())
    {
        auto& sb = serumHost_.getOutputBuffer();
        float* sLw = sb.getWritePointer(0);
        float* sRw = sb.getWritePointer(1);
        looperEngine_.process(sLw, sRw, sLw, sRw, numSamples, looperBeatPhase);
    }

    // Step sequencer — the V2 facade's sequencer handles audio triggering, but we
    // still advance the V1 sequencer to keep getCurrentStep() alive for the UI.
    // No sound comes from the V1 sampler in V2 mode (PCM chargé côté SlotPlayer).
    stepSequencer_.process(numSamples, dspPipeline_.getSampler());

    if (numCh >= 2)
    {
        // ── Stereo path ───────────────────────────────────────────────────────
        float* right = bufferToFill.buffer->getWritePointer(1, bufferToFill.startSample);

        // ── Serum gain rider (avant processStereo — Serum injecté dans la chaîne FX)
        const float* serumMixL = nullptr;
        const float* serumMixR = nullptr;
        float        serumMixGain = 0.f;
        if (serumHost_.isLoaded())
        {
            const auto& sb = serumHost_.getOutputBuffer();
            const float* sL = sb.getReadPointer(0);
            const float* sR = sb.getReadPointer(1);

            // Snapshot every 100 blocks for off-RT classification (timerCallback reads)
            if (++serumAnalysisCounter_ >= 100)
            {
                serumAnalysisCounter_ = 0;
                if (numSamples <= static_cast<int>(serumSnapBuf_.size()))
                {
                    juce::SpinLock::ScopedTryLockType sl(serumSnapLock_);
                    if (sl.isLocked())
                    {
                        for (int i = 0; i < numSamples; ++i)
                            serumSnapBuf_[i] = (sL[i] + sR[i]) * 0.5f;
                        serumSnapReady_ = true;
                    }
                }
            }

            // Target RMS — volontairement en dessous du sampler pour ne pas masquer
            float targetRms;
            switch (serumContentType_.load(std::memory_order_relaxed))
            {
                case engine::analysis::ContentCategory::BASS:  targetRms = 0.055f; break; // ~-25 dBFS
                case engine::analysis::ContentCategory::PAD:   targetRms = 0.063f; break; // ~-24 dBFS
                default:                            targetRms = 0.075f; break; // ~-22 dBFS (SYNTH/lead)
            }

            // Measure Serum RMS this block
            float sumSq = 0.f;
            for (int i = 0; i < numSamples; ++i)
                sumSq += (sL[i] * sL[i] + sR[i] * sR[i]) * 0.5f;
            const float serumRms = std::sqrt(sumSq / static_cast<float>(numSamples));

            // Gain rider: drive toward targetRms, duck when sampler is loud
            float targetGain = (serumRms > 0.001f) ? targetRms / serumRms : 1.f;
            const float mixRms = facade_.getMasterRms();
            if (mixRms > 0.08f)
                targetGain *= std::max(0.5f, 1.f - (mixRms - 0.08f) * 2.5f);
            // Clamp serré : pas de boost gratuit au-dessus de l'unité
            targetGain = std::clamp(targetGain, 0.25f, 1.0f);

            // EMA smooth (~150 ms) — prevents gain pumping
            static constexpr float kAlpha = 0.997f;
            const float prevG = serumGainSmooth_.load(std::memory_order_relaxed);
            const float newG  = kAlpha * prevG + (1.f - kAlpha) * targetGain;
            serumGainSmooth_.store(newG, std::memory_order_relaxed);
            const float g = newG * serumUserGain_.load(std::memory_order_relaxed);

            // Garder les pointeurs pour le bus Serum du moteur V2.
            serumMixL = sL;
            serumMixR = sR;
            serumMixGain = g;
        }

        facade_.processBlock(left, right, numSamples,
                             v2InputScratchL_.data(), v2InputScratchR_.data(),
                             serumMixL, serumMixR, serumMixGain);

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
        // ── Mono fallback path ────────────────────────────────────────────────
        // Bus Serum (même gain rider que le chemin stéréo) → rendu par le graphe,
        // qui applique aussi le MasterLimiter en interne.
        const float* serumL = nullptr;
        const float* serumR = nullptr;
        float        serumGain = 0.f;
        if (serumHost_.isLoaded())
        {
            const auto& sb = serumHost_.getOutputBuffer();
            serumL = sb.getReadPointer(0);
            serumR = sb.getReadPointer(1);
            serumGain = serumGainSmooth_.load(std::memory_order_relaxed)
                       * serumUserGain_.load(std::memory_order_relaxed);
        }
        facade_.processBlock(left, left, numSamples,
                             v2InputScratchL_.data(), v2InputScratchR_.data(),
                             serumL, serumR, serumGain);

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
    serumHost_.setProcessingEnabled(false);
    facade_.stopMixThread();
    facade_.releaseResources();
    juce::Logger::writeToLog("Audio resources released.");
}

void MainComponent::loadSerumPlugin(const juce::String& vst3Path)
{
    jassert(currentSampleRate_ > 0 && currentBufferSize_ > 0);
    if (currentSampleRate_ == 0.0)
    {
        juce::NativeMessageBox::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Serum",
            "Démarre le moteur audio avant de charger un VST3.", this);
        return;
    }
    juce::String err;
    if (serumHost_.load(vst3Path, currentSampleRate_, currentBufferSize_, &err))
    {
        serumStatusLabel_.setText(serumHost_.getPluginName(), juce::dontSendNotification);
        const int latency = serumHost_.getLatencySamples();
        juce::Logger::writeToLog("Serum latency: " + juce::String(latency) + " samples ("
            + juce::String(latency * 1000.0 / currentSampleRate_, 1) + " ms)"
            + (latency > 0 ? " — no compensation" : ""));
        openSerumEditor();
    }
    else
    {
        const juce::String msg = err.isEmpty() ? "Echec inconnu" : err;
        serumStatusLabel_.setText(msg, juce::dontSendNotification);
        juce::NativeMessageBox::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Serum — echec chargement", msg, this);
    }
}

void MainComponent::unloadSerumPlugin()
{
    serumEditorWindow_.reset();
    serumHost_.unload();
}

void MainComponent::openSerumEditor()
{
    if (!serumHost_.isLoaded()) return;

    if (serumEditorWindow_ != nullptr)
    {
        serumEditorWindow_->toFront(true);
        return;
    }

    auto* editor = serumHost_.createEditor();
    if (editor == nullptr) return;

    serumEditorWindow_ = std::make_unique<SampleEditorWindow>(
        serumHost_.getPluginName(), juce::Colours::black);
    serumEditorWindow_->onClose = [this] { serumEditorWindow_.reset(); };
    serumEditorWindow_->setContentOwned(editor, true);
    serumEditorWindow_->setResizable(true, false);
    serumEditorWindow_->setUsingNativeTitleBar(true);
    serumEditorWindow_->centreWithSize(editor->getWidth(), editor->getHeight());
    serumEditorWindow_->setVisible(true);
    serumEditorWindow_->toFront(true);
}

//==============================================================================
// GUI
//==============================================================================

void MainComponent::ensureGrainNoise(int w, int h)
{
    if (grainNoiseImage_.isValid()
        && grainNoiseImage_.getWidth()  == w
        && grainNoiseImage_.getHeight() == h)
        return;

    grainNoiseImage_ = juce::Image(juce::Image::ARGB, w, h, true);
    juce::Graphics gi(grainNoiseImage_);
    juce::Random rng(42);
    gi.setColour(juce::Colours::white.withAlpha(0.012f));
    for (int i = 0; i < 800; ++i)
    {
        const float gx = rng.nextFloat() * static_cast<float>(w);
        const float gy = rng.nextFloat() * static_cast<float>(h);
        gi.fillRect(gx, gy, 1.5f, 1.5f);
    }
}

void MainComponent::mouseDown(const juce::MouseEvent& e)
{
    if (serumZone_.isEmpty() || !serumZone_.contains(e.getPosition()) || !serumHost_.isLoaded())
        return;

    auto* aw = new juce::AlertWindow("Nom du preset Serum",
                                     "Entrez le nom du preset :",
                                     juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor("name", currentPresetName_, "Nom :");
    aw->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Annuler", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    aw->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [this, aw](int result)
            {
                if (result == 1)
                {
                    const auto name = aw->getTextEditorContents("name").trim();
                    if (name.isNotEmpty())
                        serumHost_.setPresetNameManual(name);
                    else
                        serumHost_.clearManualPresetName();
                    currentPresetName_ = name;
                    sceneManager_.scene(sceneManager_.currentIdx()).serumPresetName =
                        name.toStdString();
                    if (!serumZone_.isEmpty()) repaint(serumZone_);
                }
                delete aw;
            }));
}

void MainComponent::mouseDoubleClick(const juce::MouseEvent& /*e*/)
{
    if (auto* peer = getPeer())
        peer->setFullScreen(!peer->isFullScreen());
}

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

        // Indicateur autosave (fade sur 4 s)
        if (autosaveFadeTimer_ > 0)
        {
            const float alpha = juce::jmin(1.f, autosaveFadeTimer_ / 30.f) * 0.60f;
            g.setColour(ui::SaxFXColours::vuLow.withAlpha(alpha));
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(9.f)));
            g.drawText("AUTOSAVED", 292, 0, 90, kHeaderH, juce::Justification::centredLeft);
        }
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
        const float normIn  = juce::jlimit(0.f, 1.f, juce::jmap(cachedDbIn_,  -60.f, 0.f, 0.f, 1.f));
        const float normOut = juce::jlimit(0.f, 1.f, juce::jmap(cachedDbOut_, -60.f, 0.f, 0.f, 1.f));

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

    // ── Sidebar separator before transport ────────────────────────────────────
    {
        const int sepY = kHeaderH + 424;
        g.setColour(ui::SaxFXColours::cardBorder);
        g.fillRect(sidebarX + 12, sepY, kSidebarW - 24, 1);
    }

    // ── Section headers in sidebar ────────────────────────────────────────────
    {
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.f)));
        g.setColour(ui::SaxFXColours::textSecondary.withAlpha(0.50f));

        g.drawText("SPATIAL", sidebarX + 12, kHeaderH + 310, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);
        g.drawText("TRANSPORT", sidebarX + 12, kHeaderH + 427, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);

        // Labels KEY et VIEW — miroir des positions dans resized() (yFlow = kHeaderH+561)
        const int masterKeyY_p = kHeaderH + 561;
        const int viewBtnY_p   = kHeaderH + 591;
        g.drawText("KEY",  sidebarX + 12, masterKeyY_p - 11, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);
        g.drawText("VIEW", sidebarX + 12, viewBtnY_p   - 11, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);

        // EWI SYNTH section (yFlow after PLAY = kHeaderH+679)
        g.drawText("EWI SYNTH",  sidebarX + 12, kHeaderH + 679, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);
        g.drawText("EWI DEVICE", sidebarX + 12, kHeaderH + 733, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);
    }

    // ── Panel preset Serum (moitié gauche de la zone info musicale) ──────────
    if (!serumZone_.isEmpty())
    {
        using namespace ui::SaxFXColours;
        const auto zone = serumZone_.toFloat();

        // Card dark
        g.setColour(juce::Colour(0xFF141416));
        g.fillRoundedRectangle(zone, 6.f);
        g.setColour(cardBorder);
        g.drawRoundedRectangle(zone.reduced(0.5f), 6.f, 1.f);

        // Label "SERUM"
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.f)));
        g.setColour(textSecondary.withAlpha(0.50f));
        g.drawText("SERUM",
                   serumZone_.getX() + 10, serumZone_.getY() + 6,
                   serumZone_.getWidth() - 20, 10,
                   juce::Justification::centredLeft);

        const bool pluginLoaded = serumHost_.isLoaded();

        // Nom du preset centré verticalement dans la zone
        const int presetAreaY  = serumZone_.getY() + 22;
        const int presetAreaH  = serumZone_.getHeight() - 28;
        const int presetAreaX  = serumZone_.getX() + 16;
        const int presetAreaW  = serumZone_.getWidth() - 32;

        if (!pluginLoaded)
        {
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(11.f).withStyle("Italic")));
            g.setColour(textSecondary.withAlpha(0.35f));
            g.drawText("— charger Serum —",
                       presetAreaX, presetAreaY, presetAreaW, presetAreaH,
                       juce::Justification::centred);
        }
        else
        {
            const bool isFallback = currentPresetName_ == serumHost_.getPluginName()
                                    || currentPresetName_.isEmpty();
            if (isFallback)
            {
                g.setFont(juce::Font(juce::FontOptions{}.withHeight(11.f).withStyle("Italic")));
                g.setColour(textSecondary.withAlpha(0.45f));
                g.drawText("cliquer pour nommer\nce preset",
                           presetAreaX, presetAreaY, presetAreaW, presetAreaH,
                           juce::Justification::centred);
            }
            else
            {
                g.setFont(juce::Font(juce::FontOptions{}.withHeight(22.f).withStyle("Bold")));
                g.setColour(aiBadge);
                g.drawFittedText(currentPresetName_,
                                 presetAreaX, presetAreaY, presetAreaW, presetAreaH,
                                 juce::Justification::centred, 3, 0.8f);
            }
        }

        // Séparateur entre les deux panels
        g.setColour(cardBorder);
        g.fillRect(serumZone_.getRight() + 4, serumZone_.getY() + 8, 1, serumZone_.getHeight() - 16);
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
        g.drawText("DubEngine v0.1.0  fmt v" + juce::String(project::ProjectLoader::kFormatVersion),
                   statusW - 260, statusY, 250, kStatusH,
                   juce::Justification::centredRight);
    }

    // ── Grain noise overlay ───────────────────────────────────────────────────
    ensureGrainNoise(W, H);
    g.drawImageAt(grainNoiseImage_, 0, 0);

    // ── MIDI Learn panel background (drawn under children) ───────────────────
    if (midiLearnVisible_)
    {
        const auto pb = midiLearnPanel_.getBounds().toFloat();
        g.setColour(juce::Colour(0xF2141414));
        g.fillRoundedRectangle(pb, 6.f);
        g.setColour(juce::Colour(0xFF4CDFA8));
        g.drawRoundedRectangle(pb, 6.f, 1.f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(10.f).withStyle("Bold")));
        g.drawText("MIDI LEARN", pb.withHeight(28.f), juce::Justification::centred);
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

    // ── Bouton FILES dans le header ──────────────────────────────────────────
    filesMenuButton_.setBounds(222, (kHeaderH - 22) / 2, 64, 22);

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

        // Boutons projet déplacés dans le menu FILES — masqués ici
        saveProjectButton_  .setBounds(0, 0, 0, 0);
        loadProjectButton_  .setBounds(0, 0, 0, 0);
        audioSettingsButton_.setBounds(0, 0, 0, 0);
        sceneTrackResetBtn_ .setBounds(0, 0, 0, 0);

        // Info label
        infoLabel_.setBounds(sidebarX, kHeaderH + 308, kSidebarW, 10);

        // ── SPATIAL pad ─────────────────────────────────────────────────
        spatialViz_.setBounds(sbBtnX, kHeaderH + 320, sbBtnW, 100);

        // ── TRANSPORT section ────────────────────────────────────────────
        // Separator + label drawn in paint() at kHeaderH+424/427
        int y = kHeaderH + 436;
        sceneUpBtn_   .setBounds(sbBtnX, y, sbBtnW, 26); y += 28;
        sceneNumLabel_.setBounds(sbBtnX, y, sbBtnW, 18); y += 20;
        sceneDownBtn_ .setBounds(sbBtnX, y, sbBtnW, 26); y += 30;
        sceneResetBtn_.setBounds(sbBtnX, y, sbBtnW, 22); // pleine largeur
        y += 25;
        sceneCopyBtn_.setBounds(sbBtnX, y, sbBtnW, 22);

        // ── Section sous COPY : KEY/MODE, clavier/portée, PLAY, nuage IA ──
        // yFlow = kHeaderH + 436 + 26+28+18+20+26+30+22+25+22 + 8 = kHeaderH + 561
        int yFlow = kHeaderH + 561;

        const int halfW = sbBtnW / 2 - 2;

        // KEY + MODE
        masterKeyCombo_    .setBounds(sbBtnX,             yFlow, halfW, 26);
        masterKeyModeCombo_.setBounds(sbBtnX + halfW + 4, yFlow, halfW, 26);
        yFlow += 30;  // 26 + 4 gap

        // PLAY (50px)
        sidebarPlayBtn_.setBounds(sbBtnX, yFlow, sbBtnW, 50);
        yFlow += 58;  // 50 + 8 gap

        // ── EWI SYNTH section (labels drawn in paint()) ──────────────────────
        // "EWI SYNTH" header at yFlow (10px), then buttons 12px below
        {
            const int thirdB = (sbBtnW - 8) / 3;
            serumLoadBtn_  .setBounds(sbBtnX,                    yFlow + 12, thirdB, 22);
            swamLoadBtn_   .setBounds(sbBtnX + thirdB + 4,       yFlow + 12, thirdB, 22);
            serumShowUiBtn_.setBounds(sbBtnX + 2 * (thirdB + 4), yFlow + 12, thirdB, 22);
        }
        yFlow += 38; // 12+22+4
        serumStatusLabel_.setBounds(sbBtnX, yFlow,      sbBtnW, 12); yFlow += 16; // 12+4
        // "EWI DEVICE" header at yFlow (10px), then editor 10px below
        ewiDeviceEditor_ .setBounds(sbBtnX, yFlow + 10, sbBtnW, 20); yFlow += 38; // 10+20+8

        // MIDI Learn toggle button
        midiLearnBtn_.setBounds(sbBtnX, yFlow, sbBtnW, 22); yFlow += 26;

        // Nuage IA — remplit l'espace restant jusqu'en bas de la sidebar
        const int cloudBottom = H - kStatusH - kPad;
        const int cloudH      = juce::jmax(40, cloudBottom - yFlow);
        aiCloud_.setBounds(sidebarX, yFlow, kSidebarW, cloudH);

    }

    // ════════════════════════════════════════════════════════════════════════
    // MAIN CONTENT AREA
    // ════════════════════════════════════════════════════════════════════════

    // ── Zone info musicale (preset Serum + portée gammes) ────────────────────
    static constexpr int kInfoZoneH = 180;
    static constexpr int kInfoZoneTop = kHeaderH + 8;
    const int halfW = (mainW - 8) / 2;

    // Panel gauche — preset Serum (dessiné dans paint(), bounds cachés ici)
    serumZone_ = { 16, kInfoZoneTop, halfW, kInfoZoneH };

    // Panel droit — portée musicale
    const int staffLeft = 16 + halfW + 8;
    scaleTypeCombo_.setBounds(staffLeft, kInfoZoneTop, halfW, 22);
    scaleStaff_.setBounds(staffLeft, kInfoZoneTop + 26, halfW, kInfoZoneH - 26);

    // Step sequencer poussé sous la zone info
    const int seqTop = kInfoZoneTop + kInfoZoneH + 8;
    samplerLabel_.setBounds(16, seqTop - 20, 200, 16);
    stepSeqPanel_.setBounds(16, seqTop, mainW, H - seqTop - kStatusH - kPad);

    // MIDI Learn overlay panel (se superpose au step sequencer)
    {
        constexpr int kPanelW = 284;
        const int panelH = juce::jmin(H - seqTop - kStatusH - kPad,
                                      32 + midi::kNumTargets * 27 + 8);
        midiLearnPanel_.setBounds(16, seqTop, kPanelW, panelH);

        // Positionner les enfants (coordonnées locales au panneau)
        for (int i = 0; i < midi::kNumTargets; ++i)
        {
            const std::size_t si = static_cast<std::size_t>(i);
            const int y = 32 + i * 27;
            mlTargetLabels_[si].setBounds(8,   y, 114, 22);
            mlCcLabels_    [si].setBounds(124, y,  52, 22);
            mlLearnBtns_   [si].setBounds(178, y,  54, 22);
            mlClearBtns_   [si].setBounds(235, y,  26, 22);
        }
    }
}

void MainComponent::timerCallback()
{

    // ── MIDI Learn : consommer le dernier CC reçu ─────────────────────────────
    if (learningTarget_ >= 0)
    {
        const int cc = midiManager_.getLastCC();
        if (cc >= 0)
        {
            const std::size_t si = static_cast<std::size_t>(learningTarget_);
            auto& binding = midiLearnBindings_[si];
            binding.cc = cc;
            // Initialiser le smoothing à la valeur CC courante (évite le cold-start)
            targetSmoothed_[si] = binding.min
                + midiManager_.getCcValue(cc) * (binding.max - binding.min);
            stopLearning();
            updateMidiLearnUI();
        }
    }

    // ── Appliquer les mappings MIDI actifs ────────────────────────────────────
    applyMidiMappings();

    // ── Panic CC : footcontroller (CC configurable) + fallback CC#120/123 ────
    {
        const float ccVal = midiManager_.getCcValue(static_cast<int>(panicCC_));
        if (ccVal >= 0.5f && !panicArmed_)
        {
            panicArmed_ = true;
            triggerPanic();
        }
        else if (ccVal < 0.5f)
            panicArmed_ = false;

        if (midiManager_.getCcValue(120) >= 0.5f || midiManager_.getCcValue(123) >= 0.5f)
            triggerPanic();
    }

// ── Preset Serum : mise à jour 1×/s (~30 ticks) ──────────────────────────
    if (++presetNameTick_ >= 30)
    {
        presetNameTick_ = 0;
        const auto name = serumHost_.getCurrentPresetName();
        if (name.isNotEmpty() && !serumHost_.isPresetNameManual()
            && name != currentPresetName_)
        {
            currentPresetName_ = name;
            if (!serumZone_.isEmpty())
                repaint(serumZone_);
        }
    }

    // Autosave toutes les 5 min (30 fps × 300 s = 9000 ticks)
    if (++autosaveTick_ >= 9000)
    {
        autosaveTick_ = 0;
        doAutosave();
    }
    if (autosaveFadeTimer_ > 0)
        --autosaveFadeTimer_;

    // Transition de scène quantisée : appliquée à la fin du cycle du séquenceur
    if (sceneManager_.pendingIdx() >= 0
        && stepSequencer_.consumeSceneEnd())
    {
        const int target = sceneManager_.consumePendingScene();
        if (target >= 0)
        {
            // NE PAS appeler captureCurrentScene() ici : le step buffer est déjà flippé
            // vers la nouvelle scène par l'audio thread. La capture correcte a été faite
            // dans navigateScene() avant prepareStepBuffer().
            const int oldIdx = sceneManager_.currentIdx();
            sceneManager_.setCurrentIdx(target);
            applyScene(target, oldIdx);
            updateSceneLabel();
        }
    }


    // Serum content classification — lock tenu uniquement pour la copie, pas pour extract()
    if (serumHost_.isLoaded())
    {
        {
            juce::SpinLock::ScopedTryLockType sl(serumSnapLock_);
            if (sl.isLocked() && serumSnapReady_ && !serumSnapBuf_.empty())
            {
                serumSnapReady_ = false;
                serumSnapCopy_ = serumSnapBuf_;
            }
        }
        if (!serumSnapCopy_.empty())
        {
            const auto feat = engine::analysis::FeatureExtractor::extract(serumSnapCopy_, currentSampleRate_);
            serumContentType_.store(feat.contentType, std::memory_order_relaxed);
            serumMixFeatures_ = feat;
            serumSnapCopy_.clear();
        }
    }

    // Crossfade entre scènes : le moteur V2 gère les fades de slots (GainRamps) ;
    // ici on n'a plus qu'une mini-rampe Serum (serumUserGain_ lue par le bus V2).
    if (serumGainRamp_.active)
    {
        serumGainRamp_.elapsedMs += 33;
        const float t = std::clamp(
            static_cast<float>(serumGainRamp_.elapsedMs)
            / static_cast<float>(serumGainRamp_.durationMs), 0.f, 1.f);
        const float g = serumGainRamp_.from
            + (serumGainRamp_.to - serumGainRamp_.from) * t;
        serumUserGain_.store(g, std::memory_order_relaxed);
        if (t >= 1.f)
            serumGainRamp_.active = false;
    }

    // Morphing PingPongDelay entre scènes (4 secondes)
    sceneManager_.updateMorph();
    if (sceneManager_.isMorphing())
        applyDubDelayMorph(sceneManager_.getMorphProgress());

    // Mise à jour état nuage IA
    if (facade_.isMagicBusy())
        aiCloud_.setState(ui::PixelCloudComponent::State::Working);
    else if (facade_.isMagicActive())
        aiCloud_.setState(facade_.didLastMixUseFallback()
                              ? ui::PixelCloudComponent::State::Disabled   // rouge = fallback heuristique
                              : ui::PixelCloudComponent::State::Active);   // vert = IA réelle
    else
        aiCloud_.setState(ui::PixelCloudComponent::State::Disabled);

    // Sync play button text with sequencer state
    sidebarPlayBtn_.setButtonText(
        stepSequencer_.isPlaying()
            ? juce::CharPointer_UTF8("\xe2\x96\xa0")   // ■
            : juce::CharPointer_UTF8("\xe2\x96\xb6"));  // ▶

    // U2 — Précalcul des niveaux VU en dB (évite gainToDecibels dans paint())
    {
        const float rmsIn  = currentRmsLevel_.load(std::memory_order_relaxed);
        const float rmsOut = currentOutputRmsLevel_.load(std::memory_order_relaxed);
        cachedDbIn_  = juce::Decibels::gainToDecibels(rmsIn,  -60.f);
        cachedDbOut_ = juce::Decibels::gainToDecibels(rmsOut, -60.f);
        vuDirty_ = true;
    }

    // Repaint conditionnel
    if (vuDirty_ || mixStateDirty_)
    {
        vuDirty_ = mixStateDirty_ = false;
        repaint();
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    pipelineActive_ = (device != nullptr);

    if (device != nullptr)
    {
        infoLabel_.setText(juce::String(static_cast<int>(currentSampleRate_)) + " Hz / " +
                               juce::String(currentBufferSize_) + " smp",
                           juce::dontSendNotification);

    }
    else
    {
        infoLabel_.setText("No audio device", juce::dontSendNotification);
    }
}

//==============================================================================
// MIDI Learn
//==============================================================================

void MainComponent::applyMidiMappings()
{
    applyingMidi_ = true;
    for (int i = 0; i < midi::kNumTargets; ++i)
    {
        const auto& b = midiLearnBindings_[static_cast<std::size_t>(i)];
        if (b.cc < 0) continue;
        const float raw = midiManager_.getCcValue(b.cc);
        applyMappingValue(b.target, b.min + raw * (b.max - b.min));
    }
    applyingMidi_ = false;
}

void MainComponent::applyMappingValue(midi::MappingTarget t, float rawValue)
{
    const std::size_t idx = static_cast<std::size_t>(t);
    targetSmoothed_[idx] += (rawValue - targetSmoothed_[idx]) * 0.1f;
    const float v = targetSmoothed_[idx];

    using MT = midi::MappingTarget;
    switch (t)
    {
    case MT::MasterMix:
        mainMixSlider_.setValue(v, juce::dontSendNotification);
        outputGain_.store(v, std::memory_order_relaxed);
        break;
    case MT::DubDelaySend:
        dubDelaySendSlider_.setValue(v, juce::dontSendNotification);
        facade_.delay().setSend(v);
        break;
    case MT::DubDelayWet:
        dubDelayWetSlider_.setValue(v, juce::dontSendNotification);
        facade_.delay().setWet(v);
        break;
    case MT::DubDelayFeedback:
        dubDelayFeedbackSlider_.setValue(v * 0.95f, juce::dontSendNotification);
        facade_.delay().setFeedback(v * 0.95f);
        break;
    case MT::SerumGain:
        serumUserGain_.store(v, std::memory_order_relaxed);
        break;
    case MT::Slot0Gain: case MT::Slot1Gain: case MT::Slot2Gain: case MT::Slot3Gain:
    case MT::Slot4Gain: case MT::Slot5Gain: case MT::Slot6Gain: case MT::Slot7Gain:
    {
        const int slot = static_cast<int>(t) - static_cast<int>(MT::Slot0Gain);
        facade_.setSlotGain(slot, v);
        break;
    }
    default: break;
    }
}

void MainComponent::startLearning(int targetIdx)
{
    learningTarget_ = targetIdx;
    mlLearnBtns_[static_cast<std::size_t>(targetIdx)].setButtonText("...");
}

void MainComponent::stopLearning()
{
    if (learningTarget_ >= 0)
        mlLearnBtns_[static_cast<std::size_t>(learningTarget_)].setButtonText("LEARN");
    learningTarget_ = -1;
}

void MainComponent::clearMapping(int targetIdx)
{
    auto& b = midiLearnBindings_[static_cast<std::size_t>(targetIdx)];
    b.cc = -1;
    targetSmoothed_[static_cast<std::size_t>(targetIdx)] = 0.f;
    updateMidiLearnUI();
}

void MainComponent::updateMidiLearnUI()
{
    for (int i = 0; i < midi::kNumTargets; ++i)
    {
        const auto& b = midiLearnBindings_[static_cast<std::size_t>(i)];
        const juce::String label = (b.cc >= 0)
            ? ("CC " + juce::String(b.cc))
            : "--";
        mlCcLabels_[static_cast<std::size_t>(i)].setText(label, juce::dontSendNotification);
    }
}

//==============================================================================
// Scene management
//==============================================================================

void MainComponent::updateSidebarBpm(float bpm)
{
    sidebarBpmLabel_.setText(juce::String(bpm, 2),
                             juce::dontSendNotification);
}

void MainComponent::reApplyCurrentSceneTrims()
{
    const auto& sc = sceneManager_.scene(sceneManager_.currentIdx());
    for (int i = 0; i < 9; ++i)
    {
        const std::size_t sidx = static_cast<std::size_t>(i);
        const int ts = sc.trimStart[sidx];
        const int te = sc.trimEnd  [sidx];
        if (ts <= 0 && te < 0) continue;
        auto snap = facade_.getSlotPcmSnapshot(i);
        const int total = static_cast<int>(snap.size());
        if (total <= 0) continue;
        const int s2 = juce::jlimit(0, total - 1, ts);
        const int e2 = te >= 0 ? juce::jlimit(s2 + 1, total, te) : total;
        std::vector<float> trimmed(snap.begin() + s2, snap.begin() + e2);
        facade_.reloadSlotPcm(i, std::move(trimmed), facade_.getSlotPcmSampleRate(i));
    }
}

void MainComponent::updateSceneLabel()
{
    sceneNumLabel_.setText("Scene " + juce::String(sceneManager_.currentIdx() + 1) +
                           " / " + juce::String(kMaxScenes),
                           juce::dontSendNotification);
}

void MainComponent::captureCurrentScene()
{
    auto& sc = sceneManager_.scene(sceneManager_.currentIdx());
    sc.bpm  = stepSequencer_.getBpm();
    sc.used = true;
    auto& sampler = dspPipeline_.getSampler();
    for (int i = 0; i < 9; ++i)
    {
        const std::size_t idx  = static_cast<std::size_t>(i);
        const int numSteps     = stepSequencer_.getTrackStepCount(i);
        sc.filePaths    [idx]  = stepSeqPanel_.getSlotFilePath(i);
        sc.mutes        [idx]  = sampler.isSlotMuted(i);
        {
            const auto ms = facade_.getSlotMixState(i);
            sc.gains    [idx]  = ms.applied ? ms.gain : 1.0f;
        }
        // sc.userGains[idx] is maintained live by onVolumeChanged — do not overwrite here
        // delaySends (persisté .saxfx) : dérivé du rôle effectif V2 (les sends
        // temps réel sont pilotés par l'AutoMix V2 depuis les rôles).
        sc.delaySends[idx] =
            engine::AutoMixDub::roleStaticDelaySend(activeRoleForSlot(i));
        sc.trackBarCounts[idx] = stepSequencer_.getTrackBarCount(i);
        for (int s = 0; s < numSteps; ++s)
            sc.steps[idx][static_cast<std::size_t>(s)] = stepSequencer_.getStep(i, s);
    }
    sc.serumGain = serumUserGain_.load(std::memory_order_relaxed);

    // Capture Serum preset state and name for this scene
    if (serumHost_.isLoaded())
    {
        juce::MemoryBlock block;
        serumHost_.getState(block);
        if (!block.isEmpty())
            sc.serumState = block.toBase64Encoding().toStdString();
    }
    sc.serumPresetName = currentPresetName_.toStdString();

    {
        auto& dd = facade_.delay();
        sc.dubDelayFeedback = dd.getFeedback();
        sc.dubDelayWet      = dd.getWet();
        sc.dubDelayTone     = dd.getTone();
        sc.dubDelayDrive    = dd.getDrive();
    }

    // Rafraîchir le score d'énergie de la scène capturée.
    const int ci = sceneManager_.currentIdx();
    sceneManager_.setSceneEnergy(ci, ::dsp::SceneManager::computeSceneEnergy(sceneManager_.scene(ci)));
}

// ═══ Moteur V2 : sync des scènes (V1 SceneManager → engine::SceneStore) ═══════

engine::SlotRole MainComponent::activeRoleForSlot(int slot) const noexcept
{
    // Rôle analysé V2 (ImportPipeline, confiance ONNX ≥ 0.75) si fiable ;
    // sinon type effectif du mix V2 (dernier mix ou rôle courant du slot).
    return facade_.isSlotRoleReliable(slot) ? facade_.slotRole(slot)
                                            : v2RoleForSlot(slot);
}

engine::SlotRole MainComponent::v2RoleForSlot(int slot) const noexcept
{
    // Type effectif du mix V2 (dernier mix ou rôle courant du slot), puis
    // conversion vers SlotRole pour AutoMixDub / delay sends.
    using MT = engine::mix::MixContentType;
    static constexpr MT kDefaultTypes[9] = {
        MT::LOOP, MT::BASS, MT::KICK, MT::SNARE, MT::HIHAT,
        MT::PAD,  MT::SYNTH, MT::PERC, MT::LOOP,
    };
    const auto detected = (slot >= 0 && slot < 9)
        ? facade_.getDetectedType(slot) : MT::OTHER;
    const auto mt = (detected != MT::OTHER) ? detected : kDefaultTypes[slot];

    switch (mt) {
        case MT::KICK:  return engine::SlotRole::Kick;
        case MT::BASS:  return engine::SlotRole::Bass;
        case MT::SNARE: return engine::SlotRole::Snare;
        case MT::PAD:   return engine::SlotRole::Pad;
        case MT::SYNTH: return engine::SlotRole::Melodic;
        case MT::PERC:
        case MT::HIHAT: return engine::SlotRole::Perc;
        case MT::LOOP:
        case MT::OTHER:
        default:        return engine::SlotRole::Loop;
    }
}

void MainComponent::syncV2Scene(int idx) noexcept
{
    if (idx < 0 || idx >= kMaxScenes) return;
    const auto& sc  = sceneManager_.scene(idx);
    auto&       es  = facade_.scene(idx);

    for (int i = 0; i < 9; ++i)
    {
        auto& cfg = es.slots[i];

        cfg.filePath = sc.filePaths[static_cast<std::size_t>(i)];

        // userGains est le seul contrôle de volume en V1 ; repli sur sc.gains.
        const float ug = sc.userGains   [static_cast<std::size_t>(i)];
        const float sg = sc.gains       [static_cast<std::size_t>(i)];
        cfg.gain = (ug > 0.001f) ? ug : (sg > 0.001f ? sg : 1.0f);

        cfg.semitones  = sc.pitchOffsets[static_cast<std::size_t>(i)];
        cfg.trimStart  = sc.trimStart   [static_cast<std::size_t>(i)];
        cfg.trimEnd    = sc.trimEnd     [static_cast<std::size_t>(i)];
        // Role : priorité au rôle analysé par le moteur V2 quand fiable
        // (ImportPipeline, confiance ONNX ≥ 0.75) ; sinon type effectif du mix
        // V2 (→ position par défaut en dernier ressort).
        cfg.role       = activeRoleForSlot(i);

        // Actif si fichier + au moins un step dans le pattern.
        bool hasSteps = false;
        const int nSteps = std::min(512, sc.trackBarCounts[static_cast<std::size_t>(i)] * 16);
        for (int s = 0; s < nSteps && !hasSteps; ++s)
            hasSteps = sc.steps[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)];
        cfg.active = !cfg.filePath.empty() && hasSteps;
    }
}

void MainComponent::syncV2Scenes() noexcept
{
    for (int i = 0; i < kMaxScenes; ++i)
        syncV2Scene(i);
}

// ═══ applyScene ══════════════════════════════════════════════════════════════

void MainComponent::applyScene(int idx, int fromIdx)
{
    // Capturer les gains cibles courants avant changement — point de départ du crossfade.
    // Lit slot.gain (target), pas gainSmoothed_ (audio-only) : précision suffisante.
    std::array<float, 9> gainsBeforeScene {};
    for (int i = 0; i < 9; ++i)
        gainsBeforeScene[i] = dspPipeline_.getSampler().getSlotGain(i);
    const float serumGainBefore = serumUserGain_.load(std::memory_order_relaxed);

    const auto& sc = sceneManager_.scene(idx);

    // Config V2 : sync SceneData V1 → SceneStore moteur puis application au graphe
    // (gains, modes, semitones, rôles). setCurrentScene sert aussi de point
    // de départ pour requestTransition() lors des navigations en lecture.
    syncV2Scene(idx);
    facade_.setCurrentScene(idx);

    if (!sc.used)
    {
        // Empty scene: clear step patterns and reset all gains to unity
        for (int i = 0; i < 9; ++i)
            for (int s = 0; s < 16; ++s)
                stepSeqPanel_.setStepState(i, s, false);
        if (!stepSequencer_.hasPendingTransition())
            stepSequencer_.prepareStepBuffer(::dsp::StepSequencer::StepBuf{});
        for (int i = 0; i < 9; ++i)
            dspPipeline_.getSampler().setSlotGain(i, 1.0f);
        return;
    }

    // BPM is global (master clock) — not overridden by scene data

    // Track which slots got a new file — trim is already integrated in that case.
    std::array<bool, 9> loadedNewFile {};

    // Build the step buffer for the new scene (préparé atomiquement via double-buffer).
    ::dsp::StepSequencer::StepBuf nextStepBuf;

    // Reset scroll so setStepState() calls below are not filtered out by a stale offset
    stepSeqPanel_.resetViewToStart();

    // Restore samples, bar counts and step patterns
    for (int i = 0; i < 9; ++i)
    {
        const std::size_t sidx    = static_cast<std::size_t>(i);
        const int barCount        = sc.trackBarCounts[sidx];
        const int numSteps        = barCount * 16;
        const std::string& newPath     = sc.filePaths[i];
        const std::string  currentPath = stepSeqPanel_.getSlotFilePath(i);

        // Mettre à jour le step buffer local (sera préparé atomiquement après la boucle).
        nextStepBuf.trackStepCount[i] = numSteps;
        for (int s = 0; s < ::dsp::StepSequencer::kMaxSteps; ++s)
            nextStepBuf.steps[i][s] = sc.steps[sidx][static_cast<std::size_t>(s)];

        stepSeqPanel_.setTrackStepCount(i, numSteps);

        if (!newPath.empty() && newPath != currentPath)
        {
            // Nouveau fichier pour ce slot : chargement synchrone direct.
            // Le sampler V1 n'est plus audible (chemin V2 exclusif) mais reste
            // alimenté pour l'IA V1 (détection de rôles / spaceviz depuis
            // getSlotPcmView). Le V2, lui, est servi par facade_.importSampleAsync
            // (async) — voir l'alignement en fin de boucle.
            loadSampleIntoSlot(i, newPath, sc.trimStart[sidx], sc.trimEnd[sidx]);
            loadedNewFile[static_cast<std::size_t>(i)] = true;
            stepSeqPanel_.setSlotFilePath(i, newPath);
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
        }

        facade_.setSlotMuted(i, sc.mutes[i]);
        // userGains est le seul contrôle de volume : sc.gains (calibration IA) y est déjà intégré
        // dès qu'onDone tourne (onDone écrit sc.userGains = ms.gain). On ne multiplie plus les deux
        // pour éviter que les gains IA très faibles (hihat 0.09, snare 0.22) n'écrasent le fader.
        // Fallback : si userGains est nul (projet pré-IA), garder le gain courant ou 0.5.
        const float targetGain = sc.userGains[sidx] > 0.001f
            ? sc.userGains[sidx]
            : (gainsBeforeScene[static_cast<std::size_t>(i)] > 0.001f
                ? gainsBeforeScene[static_cast<std::size_t>(i)]
                : 0.5f);
        dspPipeline_.getSampler().setSlotGain     (i, targetGain);
        stepSeqPanel_.setSlotMuted (i, sc.mutes[i]);
        stepSeqPanel_.setSlotVolume(i, sc.userGains[sidx]);
        for (int s = 0; s < numSteps; ++s)
            stepSeqPanel_.setStepState(i, s, sc.steps[sidx][static_cast<std::size_t>(s)]);

        // v7 — re-appliquer le trim si défini pour ce slot/scène.
        // Seulement si même fichier : trim déjà intégré lors du chargement ci-dessus.
        if (!loadedNewFile[static_cast<std::size_t>(i)])
        {
            const int ts = sc.trimStart[sidx];
            const int te = sc.trimEnd  [sidx];
            if (ts > 0 || te >= 0)
            {
                // Ne recharger que si le trim a changé depuis la dernière application.
                // Le snapshot peut être déjà tronqué : appliquer des coordonnées fichier
                // dessus produit un double-trim (le sample rétrécit à chaque navigation).
                // On relit depuis le fichier d'origine pour garantir l'exactitude.
                if (ts != appliedTrimStart_[static_cast<std::size_t>(i)]
                    || te != appliedTrimEnd_[static_cast<std::size_t>(i)])
                {
                    if (!newPath.empty())
                        loadSampleIntoSlot(i, newPath, ts, te);
                    appliedTrimStart_[static_cast<std::size_t>(i)] = ts;
                    appliedTrimEnd_  [static_cast<std::size_t>(i)] = te;
                }
            }
        }
        else
        {
            // Nouveau fichier chargé : trim intégré dans loadSampleIntoSlot / cache.
            appliedTrimStart_[static_cast<std::size_t>(i)] = sc.trimStart[sidx];
            appliedTrimEnd_  [static_cast<std::size_t>(i)] = sc.trimEnd  [sidx];
        }

        // ── Alignement SlotPlayer V2 sur la scène (fichier + trim) ─────────────
        // importSampleAsync enregistre slotPath_* immédiatement, donc les appels
        // suivants avec la même (fichier, trim) sont des no-ops. Un fichier
        // nouveau pour cette scène (ou un trim différent de celui chargé) relance
        // un import V2 — le SlotPlayer joue le bon PCM, exactement comme le V1.
        if (!newPath.empty())
        {
            if (facade_.slotFilePath(i) != newPath
                || facade_.slotTrimStart(i) != sc.trimStart[sidx]
                || facade_.slotTrimEnd(i)   != sc.trimEnd[sidx])
            {
                facade_.importSampleAsync(i, newPath, nullptr,
                                          sc.trimStart[sidx], sc.trimEnd[sidx]);
            }
        }
        else
        {
            facade_.clearSlot(i);
        }
    }

    // Second pass: re-apply step states directly from scene data.
    // The setTrackStepCount() calls inside the loop above invoke refreshStepButtons(),
    // which reads seq_.getStep() — still returning old-scene data because the double-
    // buffer has not been flipped yet. This overwrites the setStepState() results for
    // all but the last track. Reading sc.steps[] directly avoids the stale seq_ data.
    for (int i = 0; i < 9; ++i)
    {
        const std::size_t sidx2   = static_cast<std::size_t>(i);
        const int         nSteps2 = sc.trackBarCounts[sidx2] * 16;
        for (int s = 0; s < nSteps2; ++s)
            stepSeqPanel_.setStepState(i, s, sc.steps[sidx2][static_cast<std::size_t>(s)]);
    }

    // When a bar-quantized transition is armed (navigateScene pre-armed the buffer),
    // skip: the audio thread will flip at step 0. Otherwise (sequencer stopped or
    // direct applyScene call), prepare immediately for an instant flip.
    if (!stepSequencer_.hasPendingTransition())
        stepSequencer_.prepareStepBuffer(nextStepBuf);

    // Appliquer le gain Serum de la nouvelle scène — petite rampe de 250 ms pour
    // éviter un saut audible (le crossfade de gains des slots sampler est géré
    // par le moteur V2 via TransitionEngine/GainRamps ; le bus Serum lui n'est
    // pas rampe côté moteur, on garde ici une mini-rampe UI sur serumUserGain_).
    const float serumGainTarget = sc.serumGain > 0.f ? sc.serumGain : serumGainBefore;
    if (stepSequencer_.isPlaying())
    {
        serumUserGain_.store(serumGainBefore, std::memory_order_relaxed);
        serumGainRamp_ = { true, serumGainBefore, serumGainTarget, 0, 250 };
        const int resolvedFrom = (fromIdx >= 0) ? fromIdx : idx;
        sceneManager_.startDubDelayMorph(resolvedFrom, idx, 4000.f);
    }
    else
    {
        serumUserGain_.store(serumGainTarget, std::memory_order_relaxed);
    }

    // Restore Serum preset for this scene
    if (!sc.serumState.empty() && serumHost_.isLoaded())
    {
        juce::MemoryBlock block;
        if (block.fromBase64Encoding(juce::String(sc.serumState)))
            serumHost_.setState(block.getData(), static_cast<int>(block.getSize()));
        else
            juce::Logger::writeToLog("applyScene: Serum state decode failed for scene " + juce::String(idx));
    }
    currentPresetName_ = juce::String::fromUTF8(sc.serumPresetName.c_str());
    if (currentPresetName_.isNotEmpty())
        serumHost_.setPresetNameManual(currentPresetName_);
    else
        serumHost_.clearManualPresetName();
    if (!serumZone_.isEmpty()) repaint(serumZone_);

    // Populate spatial visualization (mirrors onTypesDetected).
    {
        static const juce::Colour kSlotColours[9] = {
            juce::Colour { 0xFF4CDFA8 }, juce::Colour { 0xFF06B6D4 },
            juce::Colour { 0xFFC8C7C7 }, juce::Colour { 0xFF8B5CF6 },
            juce::Colour { 0xFFF97316 }, juce::Colour { 0xFFF43F5E },
            juce::Colour { 0xFFEAB308 }, juce::Colour { 0xFF38BDF8 },
            juce::Colour { 0xFFFF6B35 },
        };
        using MT = engine::mix::MixContentType;
        static constexpr MT kDefaultTypes[9] = {
            MT::LOOP, MT::BASS, MT::KICK, MT::SNARE, MT::HIHAT,
            MT::PAD,  MT::SYNTH, MT::PERC, MT::LOOP,
        };
        for (int i = 0; i < 9; ++i)
        {
            const auto detected = facade_.getDetectedType(i);
            const auto mt       = (detected != MT::OTHER) ? detected : kDefaultTypes[i];
            const auto sp       = engine::mix::spatialForType(i, mt);
            const bool loaded   = !facade_.slotFilePath(i).empty();
            spatialViz_.setSlotState(i, sp.pan, sp.width, sp.depth, loaded, kSlotColours[i]);
        }
        spatialViz_.setSaxActive(true);
    }
}

void MainComponent::navigateScene(int delta)
{
    const int target = juce::jlimit(0, kMaxScenes - 1, sceneManager_.currentIdx() + delta);
    if (target == sceneManager_.currentIdx()) return;

    if (!stepSequencer_.isPlaying())
    {
        // Séquenceur arrêté : changement immédiat, sans risque de coupure
        const int oldIdx = sceneManager_.currentIdx();
        captureCurrentScene();
        sceneManager_.setCurrentIdx(target);
        applyScene(target, oldIdx);
        updateSceneLabel();
        return;
    }

    // P0.3 — bloquer si une transition quantisée est déjà en cours
    if (stepSequencer_.hasPendingTransition())
    {
        return;
    }

    // Séquenceur en lecture : on met la cible en attente.
    // Capturer la scène courante MAINTENANT, pendant que activeBuf_ contient encore
    // l'ancien step buffer. Après prepareStepBuffer()/flipIfPrepared(), getStep()
    // retournera les données de la nouvelle scène, corrompant la capture.
    captureCurrentScene();

    // Moteur V2 : armer la transition vers la cible. Le diff est calculé depuis
    // la scène courante du moteur (mise à jour par applyScene / setCurrentScene).
    facade_.requestTransition(target);

    // Figer la longueur de la scène courante AVANT de stocker pendingScene_,
    // pour que la détection de fin de cycle soit stable dans le thread audio.
    int sceneLen = 1;
    for (int i = 0; i < 9; ++i)
        sceneLen = std::max(sceneLen, stepSequencer_.getTrackStepCount(i));
    stepSequencer_.setPendingTransitionLen(sceneLen);

    // Pre-arm the step buffer immediately so the audio thread can flip at exactly
    // step 0, independent of timer latency (fixes missed triggers on first step).
    {
        const auto& nextSc = sceneManager_.scene(target);
        ::dsp::StepSequencer::StepBuf nextBuf;
        for (int i = 0; i < 9; ++i)
        {
            const std::size_t sidx     = static_cast<std::size_t>(i);
            const int         numSteps = nextSc.trackBarCounts[sidx] * 16;
            nextBuf.trackStepCount[i]  = numSteps;
            for (int s = 0; s < ::dsp::StepSequencer::kMaxSteps; ++s)
                nextBuf.steps[i][s] = nextSc.steps[sidx][static_cast<std::size_t>(s)];
        }
        stepSequencer_.prepareStepBuffer(nextBuf);
    }

    sceneManager_.setPendingScene(target);
    sceneNumLabel_.setText("Scene " + juce::String(sceneManager_.currentIdx() + 1) +
                           " \xe2\x86\x92 " + juce::String(target + 1),  // →
                           juce::dontSendNotification);
}

void MainComponent::resetCurrentScene()
{
    // Clear step patterns only — keep samples loaded
    for (int i = 0; i < 9; ++i)
    {
        const int numSteps = stepSequencer_.getTrackStepCount(i);
        for (int s = 0; s < numSteps; ++s)
        {
            stepSequencer_.setStep(i, s, false);
            stepSeqPanel_.setStepState(i, s, false);
            facade_.setStep(i, s, false);
        }
    }
    facade_.flipPatternBuffer();
    sceneManager_.scene(sceneManager_.currentIdx()).used = false;
}

void MainComponent::resetCurrentSceneFull()
{
    // Clear patterns (all steps)
    for (int i = 0; i < 9; ++i)
    {
        const int numSteps = stepSequencer_.getTrackStepCount(i);
        for (int s = 0; s < numSteps; ++s)
        {
            stepSequencer_.setStep(i, s, false);
            stepSeqPanel_.setStepState(i, s, false);
            facade_.setStep(i, s, false);
        }
    }
    facade_.flipPatternBuffer();
    // Unload all samples
    auto& sampler = dspPipeline_.getSampler();
    for (int i = 0; i < 9; ++i)
    {
        sampler.clearSlot(i);
        facade_.clearSlot(i);
        stepSeqPanel_.setSlotFilePath(i, "");
        stepSeqPanel_.setSlotLoaded(i, false);
    }
    sceneManager_.scene(sceneManager_.currentIdx()).used = false;
}

void MainComponent::copyCurrentSceneToNext()
{
    // Scènes source disponibles : toutes les scènes utilisées sauf la courante
    juce::PopupMenu menu;
    int itemId = 1;
    for (int i = 0; i < kMaxScenes; ++i)
    {
        if (i == sceneManager_.currentIdx() || !sceneManager_.scene(i).used)
            { ++itemId; continue; }
        menu.addItem(itemId, "Scene " + juce::String(i + 1));
        ++itemId;
    }

    if (menu.getNumItems() == 0)
        return;

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&sceneCopyBtn_),
        [this](int result)
        {
            if (result <= 0) return;
            const int srcIdx = result - 1;  // itemId == sceneIndex + 1
            sceneManager_.scene(sceneManager_.currentIdx()) =
                sceneManager_.scene(srcIdx);
            sceneManager_.scene(sceneManager_.currentIdx()).used = true;
            applyScene(sceneManager_.currentIdx());
            juce::Logger::writeToLog("Scene " + juce::String(srcIdx + 1) +
                                     " copied into scene " + juce::String(sceneManager_.currentIdx() + 1));
        });
}

void MainComponent::applyDubDelayMorph(float t)
{
    const auto& from = sceneManager_.getScene(sceneManager_.getMorphFromScene());
    const auto& to   = sceneManager_.getScene(sceneManager_.getMorphToScene());
    auto& delay = facade_.delay();
    const auto lerp  = [](float a, float b, float x) { return a + (b - a) * x; };
    delay.setFeedback(lerp(from.dubDelayFeedback, to.dubDelayFeedback, t));
    delay.setWet     (lerp(from.dubDelayWet,      to.dubDelayWet,      t));
    delay.setTone    (lerp(from.dubDelayTone,     to.dubDelayTone,     t));
    delay.setDrive   (lerp(from.dubDelayDrive,    to.dubDelayDrive,    t));
}

void MainComponent::onPitchOffsetChanged(int slot, float semitones)
{
    // 1. Persist in current scene
    auto& sc = sceneManager_.scene(sceneManager_.currentIdx());
    sc.pitchOffsets[static_cast<std::size_t>(slot)] = semitones;

    // 2. Update panel checkmark
    stepSeqPanel_.setSlotPitchOffset(slot, semitones);

    // 3. Instant repitch
    facade_.setSlotTransposeSemitones(slot, semitones);
}

engine::mix::MixContentType MainComponent::serumContentTypeForMix() const noexcept
{
    switch (serumContentType_.load(std::memory_order_relaxed))
    {
        case engine::analysis::ContentCategory::KICK:  return engine::mix::MixContentType::KICK;
        case engine::analysis::ContentCategory::SNARE: return engine::mix::MixContentType::SNARE;
        case engine::analysis::ContentCategory::HIHAT: return engine::mix::MixContentType::HIHAT;
        case engine::analysis::ContentCategory::BASS:  return engine::mix::MixContentType::BASS;
        case engine::analysis::ContentCategory::SYNTH: return engine::mix::MixContentType::SYNTH;
        case engine::analysis::ContentCategory::PAD:   return engine::mix::MixContentType::PAD;
        case engine::analysis::ContentCategory::PERC:  return engine::mix::MixContentType::PERC;
        default:                                       return engine::mix::MixContentType::OTHER;
    }
}
