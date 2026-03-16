#include "MainComponent.h"

//==============================================================================
MainComponent::MainComponent()
{
    // Sélectionner les canaux audio : 1 entrée (saxophone), 2 sorties (casque stéréo)
    setAudioChannels(1, 2);

    // Bouton d'accès aux paramètres audio (sélection périphérique ASIO/WASAPI)
    audioSettingsButton.setButtonText("Audio Settings");
    audioSettingsButton.onClick = [this]
    {
        juce::AudioDeviceSelectorComponent selector(
            deviceManager,
            0, 1,   // inputs min/max
            0, 2,   // outputs min/max
            true,   // show MIDI inputs
            false,  // show MIDI outputs
            false,  // stereo pairs
            false   // hide advanced
        );
        selector.setSize(500, 400);

        juce::DialogWindow::LaunchOptions options;
        options.content.setNonOwned(&selector);
        options.dialogTitle         = "Audio Device Settings";
        options.dialogBackgroundColour = juce::Colours::darkgrey;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar   = true;
        options.resizable           = false;
        options.launchAsync();
    };
    addAndMakeVisible(audioSettingsButton);

    // Labels d'information
    statusLabel.setJustificationType(juce::Justification::centred);
    statusLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    statusLabel.setText("Initialising...", juce::dontSendNotification);
    addAndMakeVisible(statusLabel);

    infoLabel.setJustificationType(juce::Justification::centred);
    infoLabel.setFont(juce::Font(13.0f));
    infoLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(infoLabel);

    setSize(800, 500);

    // Timer pour rafraîchir le VU-mètre sans bloquer le thread audio
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    // IMPORTANT : arrêter l'audio avant la destruction
    shutdownAudio();
}

//==============================================================================
// Boucle audio — callbacks
//==============================================================================

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    // Stocker les paramètres pour l'affichage (pas d'allocation ici)
    currentSampleRate = sampleRate;
    currentBufferSize = samplesPerBlockExpected;

    // Sprint 2+ : initialiser les buffers DSP ici (reverb, harmoniseur...)
    // Exemple : reverbBuffer.resize(samplesPerBlockExpected);

    juce::Logger::writeToLog(juce::String("Audio prepared: ")
        + juce::String(sampleRate) + " Hz, buffer "
        + juce::String(samplesPerBlockExpected) + " samples ("
        + juce::String(1000.0 * samplesPerBlockExpected / sampleRate, 1) + " ms)");
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    // ─────────────────────────────────────────────────────────────────────────
    // REALTIME-SAFE : pas d'allocation, pas de lock, pas d'I/O
    // ─────────────────────────────────────────────────────────────────────────

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    // Récupérer le canal d'entrée (microphone saxophone)
    const float* inputChannel = bufferToFill.buffer->getReadPointer(0, bufferToFill.startSample);

    // Calculer le niveau RMS pour le VU-mètre (atomic, thread-safe)
    float sumSquares = 0.0f;
    for (int i = 0; i < bufferToFill.numSamples; ++i)
        sumSquares += inputChannel[i] * inputChannel[i];

    const float rms = std::sqrt(sumSquares / static_cast<float>(bufferToFill.numSamples));
    currentRmsLevel.store(rms, std::memory_order_relaxed);

    // Pass-through : copier l'entrée mono vers les deux sorties stéréo
    // Sprint 2+ : insérer ici le pipeline d'effets DSP
    for (int channel = 0; channel < bufferToFill.buffer->getNumChannels(); ++channel)
    {
        bufferToFill.buffer->copyFrom(
            channel,
            bufferToFill.startSample,
            *bufferToFill.buffer,
            0,                         // source = canal 0 (entrée mono)
            bufferToFill.startSample,
            bufferToFill.numSamples
        );
    }
}

void MainComponent::releaseResources()
{
    // Sprint 2+ : libérer les ressources DSP allouées dans prepareToPlay
    juce::Logger::writeToLog("Audio resources released.");
}

//==============================================================================
// GUI
//==============================================================================

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1a2e));  // fond sombre bleu nuit

    // Titre
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(24.0f, juce::Font::bold));
    g.drawText("SaxFX Live", getLocalBounds().removeFromTop(60),
               juce::Justification::centred, false);

    // VU-mètre simple
    const float rms      = currentRmsLevel.load(std::memory_order_relaxed);
    const float dbLevel  = juce::Decibels::gainToDecibels(rms, -60.0f);
    const float normalised = juce::jmap(dbLevel, -60.0f, 0.0f, 0.0f, 1.0f);

    const int vuX = 50;
    const int vuY = 200;
    const int vuW = getWidth() - 100;
    const int vuH = 30;

    g.setColour(juce::Colours::darkgrey);
    g.fillRoundedRectangle(static_cast<float>(vuX), static_cast<float>(vuY),
                           static_cast<float>(vuW), static_cast<float>(vuH), 4.0f);

    // Couleur : vert → jaune → rouge selon le niveau
    const float filled = static_cast<float>(vuW) * juce::jlimit(0.0f, 1.0f, normalised);
    juce::Colour barColour = (normalised < 0.7f) ? juce::Colours::limegreen
                           : (normalised < 0.9f) ? juce::Colours::orange
                                                 : juce::Colours::red;
    g.setColour(barColour);
    g.fillRoundedRectangle(static_cast<float>(vuX), static_cast<float>(vuY),
                           filled, static_cast<float>(vuH), 4.0f);

    // Étiquette VU-mètre
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(12.0f));
    g.drawText("INPUT LEVEL", vuX, vuY - 20, vuW, 18, juce::Justification::left, false);
    g.drawText(juce::String(dbLevel, 1) + " dB", vuX, vuY + vuH + 4, vuW, 18,
               juce::Justification::right, false);
}

void MainComponent::resized()
{
    audioSettingsButton.setBounds(getWidth() / 2 - 80, 280, 160, 36);
    statusLabel.setBounds(0, 90, getWidth(), 30);
    infoLabel.setBounds(0, 125, getWidth(), 24);
}

void MainComponent::timerCallback()
{
    // Mise à jour de l'affichage (hors thread audio)
    repaint();

    // Mise à jour du label de statut
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device != nullptr)
    {
        statusLabel.setText("LIVE — Pass-through actif", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::limegreen);

        infoLabel.setText(
            juce::String(static_cast<int>(currentSampleRate)) + " Hz  |  buffer "
            + juce::String(currentBufferSize) + " samples  ("
            + juce::String(1000.0 * currentBufferSize / currentSampleRate, 1) + " ms)  |  "
            + device->getName(),
            juce::dontSendNotification
        );
    }
    else
    {
        statusLabel.setText("Aucun périphérique audio sélectionné", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
        infoLabel.setText("→ Cliquer sur 'Audio Settings'", juce::dontSendNotification);
    }
}
