#pragma once

#include <JuceHeader.h>

//==============================================================================
/**
 * MainComponent — composant principal de SaxFX Live (Sprint 1)
 *
 * Hérite de AudioAppComponent pour gérer la boucle audio JUCE.
 * Sprint 1 : pass-through pur (entrée Scarlett → sortie casque), zéro effet.
 * Les modules DSP (réverb, harmoniseur, sampler) seront ajoutés aux sprints suivants.
 */
class MainComponent : public juce::AudioAppComponent,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    //==========================================================================
    // AudioAppComponent overrides — boucle audio temps réel
    //==========================================================================

    /** Appelé avant le démarrage du flux audio. Préparer les buffers ici. */
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;

    /** Callback audio temps réel — REALTIME-SAFE : zéro allocation, zéro lock. */
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    /** Appelé à l'arrêt du flux audio. Libérer les ressources DSP. */
    void releaseResources() override;

    //==========================================================================
    // Component overrides — rendu GUI
    //==========================================================================
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    //==========================================================================
    // Timer — mise à jour du VU-mètre (~30 fps)
    //==========================================================================
    void timerCallback() override;

    //==========================================================================
    // GUI
    //==========================================================================
    juce::TextButton audioSettingsButton;
    juce::Label      statusLabel;
    juce::Label      infoLabel;

    // VU-mètre : niveau RMS courant (mis à jour dans le timer, pas dans le callback audio)
    std::atomic<float> currentRmsLevel { 0.0f };

    // Paramètres audio courants (lecture seule en dehors de prepareToPlay)
    double currentSampleRate  { 0.0 };
    int    currentBufferSize  { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
