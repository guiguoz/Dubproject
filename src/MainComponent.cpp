#include "MainComponent.h"

#include "dsp/DelayEffect.h"
#include "dsp/FeatureExtractor.h"
#include "dsp/FlangerEffect.h"
#include "dsp/HarmonizerEffect.h"
#include "dsp/KeyDetector.h"
#include "dsp/OctaverEffect.h"
#include "dsp/SynthEffect.h"
#include "dsp/WsolaShifter.h"

#include <cmath>
#include <memory>

//==============================================================================
MainComponent::MainComponent()
{
    // Step 2: apply SAX-OS neon LookAndFeel if available
    saxOsLookAndFeel_ = std::make_unique<ui::SaxOsLookAndFeel>();
    if (saxOsLookAndFeel_)
        setLookAndFeel(saxOsLookAndFeel_.get());
    // Fallback to default if needed
    // setLookAndFeel(&laf_);

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
    auto styleSideBtn = [](juce::TextButton& b, const juce::String& text)
    {
        b.setButtonText(text);
        b.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF1A1A2E));
        b.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCCCCCC));
    };

    styleSideBtn(sceneUpBtn_,    juce::CharPointer_UTF8("\xe2\x96\xb2"));  // ▲
    styleSideBtn(sceneDownBtn_,  juce::CharPointer_UTF8("\xe2\x96\xbc"));  // ▼
    styleSideBtn(sceneResetBtn_, "RESET");
    styleSideBtn(sceneCopyBtn_,  "COPY");

    sceneUpBtn_  .onClick = [this] { navigateScene(-1); };
    sceneDownBtn_.onClick = [this] { navigateScene(+1); };
    sceneResetBtn_.onClick = [this] { resetCurrentScene(); };
    sceneCopyBtn_ .onClick = [this] { copyCurrentSceneToNext(); };

    sceneNumLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(13.f).withStyle("Bold")));
    sceneNumLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFFFFCC44));
    sceneNumLabel_.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(sceneUpBtn_);
    addAndMakeVisible(sceneNumLabel_);
    addAndMakeVisible(sceneDownBtn_);
    addAndMakeVisible(sceneResetBtn_);
    addAndMakeVisible(sceneCopyBtn_);
    updateSceneLabel();

    // ── Sidebar lower: transport (▶/■, TAP, BPM, ⚡) ─────────────────────────
    sidebarPlayBtn_.setButtonText(juce::CharPointer_UTF8("\xe2\x96\xb6"));  // ▶
    sidebarPlayBtn_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF1A2E1A));
    sidebarPlayBtn_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF2A4A2A));
    sidebarPlayBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF44FF44));
    sidebarPlayBtn_.onClick = [this] { stepSeqPanel_.triggerPlay(); };

    sidebarTapBtn_.setButtonText("TAP TEMPO");
    sidebarTapBtn_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4CDFA8).withAlpha(0.10f));
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
    sidebarMagicBtn_.setTooltip("Smart mix Neutron: actif = EQ+sidechain, re-clic = annuler");
    sidebarMagicBtn_.setClickingTogglesState(true);
    sidebarMagicBtn_.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF2A1C00));
    sidebarMagicBtn_.setColour(juce::TextButton::buttonOnColourId,  juce::Colour(0xFF886600));
    sidebarMagicBtn_.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFFFCC44));
    sidebarMagicBtn_.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFFFFEE88));
    sidebarMagicBtn_.onClick = [this] { samplerEngine_.toggleMagicMix(); };

    addAndMakeVisible(sidebarPlayBtn_);
    addAndMakeVisible(sidebarTapBtn_);
    addAndMakeVisible(sidebarBpmLabel_);
    addAndMakeVisible(sidebarMagicBtn_);

    // ── Project buttons ───────────────────────────────────────────────────────
    saveProjectButton_.setButtonText("SAVE PROJECT");
    saveProjectButton_.onClick = [this] { saveProject(); };
    addAndMakeVisible(saveProjectButton_);

    loadProjectButton_.setButtonText("LOAD PRESET");
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
        pitchMatchSampleAsync(slot, std::move(pcm), fileSr);
    };

    // Step toggled: already forwarded to stepSequencer_ inside the panel
    stepSeqPanel_.onStepChanged = [](int, int, bool) {};  // no extra action needed

    // Slot cleared: unload PCM and clear engine path
    stepSeqPanel_.onSlotCleared = [this](int slot)
    {
        dspPipeline_.getSampler().clearSlot(slot);
        samplerEngine_.setSlotFilePath(slot, "");
    };

    // BPM changed: update DSP + sidebar label
    stepSeqPanel_.onBpmChanged = [this](float bpm)
    {
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
            dspPipeline_.getSampler().stopAllSlots();
    };

    // Volume per slot
    stepSeqPanel_.onVolumeChanged = [this](int slot, float gain)
    {
        dspPipeline_.getSampler().setSlotGain(slot, gain);
    };

    // Mute per slot
    stepSeqPanel_.onMutedChanged = [this](int slot, bool muted)
    {
        dspPipeline_.getSampler().setSlotMuted(slot, muted);
    };

    // Magic Mix ⚡ — le callback du panel (backup, au cas où) n'est plus utilisé pour le toggle
    stepSeqPanel_.onMagicButtonPressed = nullptr;

    // Mise à jour des tags de type dans l'UI quand la détection est terminée
    samplerEngine_.onTypesDetected = [this]
    {
        for (int i = 0; i < 8; ++i)
        {
            const auto type = samplerEngine_.getDetectedType(i);
            if (dspPipeline_.getSampler().isLoaded(i))
                stepSeqPanel_.setSlotContentType(
                    i, ::dsp::SmartSamplerEngine::contentTypeName(type));
        }
        stepSeqPanel_.setMagicActive(true);
        sidebarMagicBtn_.setToggleState(true, juce::dontSendNotification);
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

    // VU meter — real per-slot output peak from audio thread (reflects gain/mute)
    stepSeqPanel_.getSlotLevel = [this](int slot) -> float
    {
        return dspPipeline_.getSampler().getSlotOutputPeak(slot);
    };

    // Variable step count per track
    stepSeqPanel_.onStepCountChanged = [this](int track, int count)
    {
        stepSequencer_.setTrackStepCount(track, count);
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
// pitchMatchSampleAsync — key detection + WSOLA shift, then load into Sampler
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::pitchMatchSampleAsync(int slot, std::vector<float> raw, double sr)
{
    // Charger le sample brut immédiatement — slot jouable dès maintenant
    {
        auto& sampler = dspPipeline_.getSampler();
        sampler.loadSample(slot, raw.data(), static_cast<int>(raw.size()), sr);
        sampler.setSlotOneShot(slot, true);
        stepSeqPanel_.setSlotLoaded(slot, true);
    }

    // Slot 0 = référence, pas de shift
    if (slot == 0)
        return;

    // Thread de fond : détection de tonalité + WSOLA, swap si shift ≠ 0
    std::thread([this, slot, raw = std::move(raw), sr]() mutable
    {
        // 1. Type de contenu — skip pitch-shift pour percussions
        auto features = ::dsp::FeatureExtractor::extract(raw, sr);
        const bool isPercussive =
            (features.contentType == ::dsp::ContentCategory::KICK   ||
             features.contentType == ::dsp::ContentCategory::SNARE  ||
             features.contentType == ::dsp::ContentCategory::HIHAT  ||
             features.contentType == ::dsp::ContentCategory::PERC);

        if (isPercussive) return;  // raw déjà chargé, on garde

        // 2. Détection de la tonalité du sample
        ::dsp::KeyDetector kd;
        kd.process(raw.data(), static_cast<int>(raw.size()), sr);
        const auto result = kd.getResult();
        if (result.key < 0) return;  // détection échouée, raw déjà chargé

        int delta = masterKeyRoot_ - result.key;
        while (delta >  6) delta -= 12;
        while (delta < -6) delta += 12;
        const float shiftSemitones = static_cast<float>(delta);

        if (std::abs(shiftSemitones) <= 0.05f) return;  // déjà dans la bonne tonalité

        // 3. WSOLA pitch-shift (batch)
        std::vector<float> shifted(raw.size());
        ::dsp::WsolaShifter ws;
        ws.prepare(sr, 512);
        ws.setShiftSemitones(shiftSemitones);
        constexpr int kBlock = 512;
        for (int i = 0; i < static_cast<int>(raw.size()); i += kBlock)
        {
            const int n = std::min(kBlock, static_cast<int>(raw.size()) - i);
            ws.process(raw.data() + i, shifted.data() + i, n, 0.f);
        }

        // 4. Swap sur le thread GUI (raw toujours en fallback si WSOLA échoue)
        juce::MessageManager::callAsync(
            [this, slot, shifted = std::move(shifted)]() mutable
            {
                dspPipeline_.getSampler().reloadSlotData(slot, std::move(shifted));
            });
    }).detach();
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
                sc.gain     = 1.0f;
                sc.loop     = false;
                sc.oneShot  = true;
                sc.muted    = sampler.isSlotMuted(i);
                sc.gridDiv  = 0;
                for (int s = 0; s < 16; ++s)
                    sc.stepPattern[s] = stepSequencer_.getStep(i, s);
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

    float* monoChannel = bufferToFill.buffer->getWritePointer(0, bufferToFill.startSample);
    const int numSamples = bufferToFill.numSamples;

    // Compute RMS before processing (for VU meter) — exponential smoothing
    // Attack fast (0.3) so the meter reacts quickly to transients,
    // release slow (0.05) so it doesn't jump back to zero between notes.
    {
        float sumSquares = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            sumSquares += monoChannel[i] * monoChannel[i];
        const float rawRms = std::sqrt(sumSquares / static_cast<float>(numSamples));
        const float prev   = currentRmsLevel_.load(std::memory_order_relaxed);
        const float coeff  = (rawRms > prev) ? 0.3f : 0.05f;
        currentRmsLevel_.store(prev + coeff * (rawRms - prev),
                               std::memory_order_relaxed);
    }

    // Step sequencer — triggers sampler slots at step boundaries (before DSP mix)
    stepSequencer_.process(numSamples, dspPipeline_.getSampler());

    // DSP pipeline (effects + sampler mix)
    dspPipeline_.process(monoChannel, numSamples);

    // Apply master output gain
    const float outGain = outputGain_.load(std::memory_order_relaxed);
    if (outGain != 1.0f)
        for (int i = 0; i < numSamples; ++i)
            monoChannel[i] *= outGain;

    // Compute Output RMS (for VU meter)
    {
        float sumSquares = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            sumSquares += monoChannel[i] * monoChannel[i];
        const float rawRms = std::sqrt(sumSquares / static_cast<float>(numSamples));
        const float prev   = currentOutputRmsLevel_.load(std::memory_order_relaxed);
        const float coeff  = (rawRms > prev) ? 0.3f : 0.05f;
        currentOutputRmsLevel_.store(prev + coeff * (rawRms - prev),
                               std::memory_order_relaxed);
    }

    // Copy mono to all output channels
    for (int ch = 1; ch < bufferToFill.buffer->getNumChannels(); ++ch)
    {
        bufferToFill.buffer->copyFrom(ch, bufferToFill.startSample, *bufferToFill.buffer, 0,
                                      bufferToFill.startSample, numSamples);
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

        // Title
        g.setColour(ui::SaxFXColours::vuLow); // primary #4cdfa8
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(15.f).withStyle("Bold")));
        g.drawText("SONIC MONOLITH | SAX-OS", 20, 0, 280, kHeaderH,
                   juce::Justification::centredLeft);

        // Nav tabs (effects)
        const juce::String tabs[] = { "HARMONIZER", "FLANGER", "DELAY", "OCTAVER" };
        int tabX = 320;
        for (int i = 0; i < 4; ++i)
        {
            const bool active = (i == 0);
            g.setColour(active ? ui::SaxFXColours::vuLow
                               : ui::SaxFXColours::textPrimary.withAlpha(0.35f));
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(9.f).withStyle("Bold")));
            g.drawText(tabs[i], tabX, 0, 90, kHeaderH - (active ? 2 : 0),
                       juce::Justification::centredLeft);
            if (active)
            {
                g.setColour(ui::SaxFXColours::vuLow);
                g.fillRect(tabX, kHeaderH - 2, 80, 2);
            }
            tabX += 96;
        }

        // Pitch display (top-right of header area, before sidebar)
        g.setColour(juce::Colours::cyan.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(13.f).withStyle("Bold")));
        const auto pitchStr = pitchLabel_.getText();
        g.drawText(pitchStr, sidebarX - 180, 0, 170, kHeaderH,
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

        // "KEY" micro-label above master key combos (drawn inline — position mirrors resized())
        // kPad=8, kStatusH=24 → magicY=H-58, playY=H-120
        const int playY_p      = H - 24 - 8 - 26 - 8 - 54;  // H - 120
        const int viewBtnY_p   = playY_p - 8 - 28;           // H - 156
        const int scaleY_p     = viewBtnY_p - 4 - 26;        // H - 186
        const int masterKeyY_p = scaleY_p - 4 - 24;          // H - 214
        g.drawText("KEY", sidebarX + 12, masterKeyY_p - 11, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);
        g.drawText("SCALE", sidebarX + 12, scaleY_p - 11, kSidebarW - 24, 10,
                   juce::Justification::centredLeft);
        g.drawText("VIEW", sidebarX + 12, viewBtnY_p - 11, kSidebarW - 24, 10,
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
        g.drawText("SONIC MONOLITH SAX-OS V0.1.0", statusW - 260, statusY, 250, kStatusH,
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

        // ── TRANSPORT section ────────────────────────────────────────────
        // Separator + label drawn in paint()
        y = kHeaderH + 512;
        sceneUpBtn_   .setBounds(sbBtnX, y, sbBtnW, 26); y += 28;
        sceneNumLabel_.setBounds(sbBtnX, y, sbBtnW, 18); y += 20;
        sceneDownBtn_ .setBounds(sbBtnX, y, sbBtnW, 26); y += 30;
        sceneResetBtn_.setBounds(sbBtnX, y, sbBtnW, 22); y += 25;
        sceneCopyBtn_ .setBounds(sbBtnX, y, sbBtnW, 22);

        // PLAY button (large) + magic — pinned near bottom
        const int magicY = H - kStatusH - kPad - 26;
        const int playY  = magicY - kPad - 54;
        sidebarPlayBtn_ .setBounds(sbBtnX, playY,  sbBtnW, 54);
        sidebarMagicBtn_.setBounds(sbBtnX, magicY, sbBtnW, 26);

        // ── VIEW SWITCH + MASTER KEY (above play button) ──────────────
        const int viewBtnY   = playY - kPad - 28;     // 28px view buttons
        const int scaleY     = viewBtnY - 4 - 26;     // 26px scale combo
        const int masterKeyY = scaleY - 4 - 24;       // 24px key combos

        const int halfW = sbBtnW / 2 - 2;
        viewKeyboardBtn_.setBounds(sbBtnX,               viewBtnY, halfW, 28);
        viewStaffBtn_   .setBounds(sbBtnX + halfW + 4,   viewBtnY, halfW, 28);

        globalScaleCombo_.setBounds(sbBtnX, scaleY, sbBtnW, 26);

        masterKeyCombo_    .setBounds(sbBtnX,               masterKeyY, halfW, 24);
        masterKeyModeCombo_.setBounds(sbBtnX + halfW + 4,   masterKeyY, halfW, 24);
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
    dspPipeline_.getEffectChain().collectGarbage();

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
    for (int i = 0; i < 8; ++i)
    {
        sc.filePaths[i] = stepSeqPanel_.getSlotFilePath(i);
        sc.mutes[i]     = dspPipeline_.getSampler().isSlotMuted(i);
        for (int s = 0; s < 16; ++s)
            sc.steps[i][static_cast<std::size_t>(s)] = stepSequencer_.getStep(i, s);
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

    // Restore BPM
    stepSequencer_.setBpm(sc.bpm);
    dspPipeline_.setBpm(sc.bpm);
    stepSeqPanel_.setBpm(sc.bpm);
    updateSidebarBpm(sc.bpm);

    // Restore samples and step patterns
    for (int i = 0; i < 8; ++i)
    {
        if (!sc.filePaths[i].empty())
        {
            loadSampleIntoSlot(i, sc.filePaths[i]);
            stepSeqPanel_.setSlotFilePath(i, sc.filePaths[i]);
            samplerEngine_.setSlotFilePath(i, sc.filePaths[i]);
        }
        dspPipeline_.getSampler().setSlotMuted(i, sc.mutes[i]);
        stepSeqPanel_.setSlotMuted(i, sc.mutes[i]);
        for (int s = 0; s < 16; ++s)
        {
            const bool active = sc.steps[i][static_cast<std::size_t>(s)];
            stepSequencer_.setStep(i, s, active);
            stepSeqPanel_.setStepState(i, s, active);
        }
    }
}

void MainComponent::navigateScene(int delta)
{
    captureCurrentScene();
    currentScene_ = juce::jlimit(0, kMaxScenes - 1, currentScene_ + delta);
    applyScene(currentScene_);
    updateSceneLabel();
}

void MainComponent::resetCurrentScene()
{
    // Clear step patterns only — keep samples loaded
    for (int i = 0; i < 8; ++i)
        for (int s = 0; s < 16; ++s)
        {
            stepSequencer_.setStep(i, s, false);
            stepSeqPanel_.setStepState(i, s, false);
        }
    // Mark scene as used (but empty)
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
