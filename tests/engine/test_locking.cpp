#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <cmath>
#include "engine/OfflineRender.h"

using namespace engine;

// ─────────────────────────────────────────────────────────────────────────────
// T-LOCK2 : Position dérivée stable sur longue durée (1 min test)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-LOCK2: loop 126 BPM vs project 120 BPM — 1 min stability", "[locking]") {
    // Résumé : Une loop de 4 beats à 126 BPM importée dans un projet à 120 BPM.
    // La loop doit être time-stretchée et rester collée à la grille projet.
    // Sur 1 minute, les triggers du kick (référence régulière) doivent être parfaitement espacés.

    OfflineSession sess;
    sess.bpm        = 120.0;
    sess.sampleRate = 44100.0;
    sess.maxBlockSize = 512;
    
    // Calculs temporels
    const double samplesPerBeat_120 = 44100.0 * 60.0 / 120.0; // 22050 samples/beat
    const double loopBeatDuration = 4.0; // 4 beats
    const double loopSampleDuration_126 = loopBeatDuration * 44100.0 * 60.0 / 126.0;
    const double timeRatio = 120.0 / 126.0; // ≈ 0.952381

    // === Slot 0 : Loop 126 BPM en LoopSync ===
    // Créer un PCM synthétique : une onde simple 4 beats de durée
    const int loopNumFrames = static_cast<int>(loopSampleDuration_126 + 0.5);
    sess.pcm[0].numChannels = 1;
    sess.pcm[0].sampleRate  = 44100.0f;
    sess.pcm[0].data.resize(loopNumFrames);
    
    // Remplir avec une onde : simple sine pour identifier facilement
    for (int i = 0; i < loopNumFrames; ++i) {
        sess.pcm[0].data[i] = 0.1f * std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
    }
    
    // Config LOOP SYNC
    sess.loopSync[0].enabled   = true;
    sess.loopSync[0].loopBeats = 4;
    sess.loopSync[0].timeRatio = static_cast<float>(timeRatio);
    
    // === Slot 1 : Kick reference (tous les 4 beats = 88200 samples à 120 BPM) ===
    const int kickNumFrames = static_cast<int>(samplesPerBeat_120 + 0.5); // ~22050
    sess.pcm[1].numChannels = 1;
    sess.pcm[1].sampleRate  = 44100.0f;
    sess.pcm[1].data.resize(kickNumFrames);
    
    // Kick : impulsion synthétique (simple click)
    sess.pcm[1].data[0] = 1.0f; // click au frame 0
    for (int i = 1; i < kickNumFrames; ++i) {
        sess.pcm[1].data[i] = 0.1f * std::exp(-i / 2205.0f); // decay rapide
    }
    
    // === Pattern : Kick tous les 4 steps (4 steps = 1 beat à 120 BPM) ===
    // 4 beats × 4 steps/beat = 16 steps par cycle de kick
    sess.patterns[0].numSteps = 16;
    for (int i = 0; i < 16; ++i)
        sess.patterns[0].steps[i] = true; // loop continue
    
    sess.patterns[1].numSteps = 16;
    for (int i = 0; i < 16; ++i)
        sess.patterns[1].steps[i] = (i % 4 == 0); // kick tous les 4 steps
    
    // === Scène unique ===
    sess.scenes[0].slots[0].filePath = "loop_126";
    sess.scenes[0].slots[0].mode     = PlayMode::Free;
    sess.scenes[0].slots[0].gain     = 1.0f;
    sess.scenes[0].slots[0].role     = SlotRole::Loop;
    
    sess.scenes[0].slots[1].filePath = "kick_120";
    sess.scenes[0].slots[1].mode     = PlayMode::OneShot;
    sess.scenes[0].slots[1].gain     = 1.0f;
    sess.scenes[0].slots[1].role     = SlotRole::Kick;
    
    // === Rendu offline : 1 minute ===
    const int64_t totalSamples = static_cast<int64_t>(60.0 * 44100.0); // 1 min
    std::vector<float> audioOut;
    
    try {
        audioOut = renderOffline(sess, totalSamples);
    } catch (const std::exception& e) {
        FAIL("renderOffline threw: " << e.what());
        return;
    }
    
    REQUIRE(audioOut.size() == static_cast<size_t>(totalSamples * 2));
    
    // === Analyse : extraire les pics de kick ===
    // Le kick est dans le canal 1 (slot 1). On cherche les pics > 0.8
    const float kickThreshold = 0.5f;
    std::vector<int64_t> kickPeaks;
    
    for (int64_t i = 1; i < totalSamples - 1; ++i) {
        // Interleaved stéréo : sample [slot0] à 2*i, sample [slot1] à 2*i+1
        // (Ou mono dépend de la structure — adapter selon OfflineRender)
        // Ici on suppose le canal du kick est identifiable par amplitude pic
        float left  = audioOut[2 * i];
        float right = audioOut[2 * i + 1];
        float maxSamp = std::max(std::abs(left), std::abs(right));
        
        // Pic local
        if (maxSamp > kickThreshold && 
            std::abs(audioOut[2 * (i - 1)]) < kickThreshold &&
            std::abs(audioOut[2 * (i + 1)]) < kickThreshold) {
            kickPeaks.push_back(i);
        }
    }
    
    // === Vérification : espacements réguliers ===
    // Kick tous les 4 steps = 4 steps/beat = 1 beat = 22050 samples (à 120 BPM)
    const int64_t expectedGap = static_cast<int64_t>(samplesPerBeat_120 + 0.5);
    
    REQUIRE(kickPeaks.size() > 10); // Au moins 10 kicks en 1 min (1 kick/beat = 60 kicks)
    
    // Mesurer la dérive
    double maxDrift = 0.0;
    double sumDrift = 0.0;
    
    for (size_t i = 1; i < kickPeaks.size(); ++i) {
        int64_t gap = kickPeaks[i] - kickPeaks[i - 1];
        double drift = std::abs(gap - expectedGap);
        maxDrift = std::max(maxDrift, drift);
        sumDrift += drift;
    }
    
    double avgDrift = sumDrift / (kickPeaks.size() - 1);
    
    // === Critère de validation ===
    // La dérive moyenne doit être < 0.5 sample (très stable)
    // La dérive max doit être < 2 samples (acceptable)
    REQUIRE(avgDrift < 0.5);  // Moyenne < 0.5 sample
    REQUIRE(maxDrift < 2.0);  // Max < 2 samples
    
    // === Log pour diagnostic ===
    INFO("Kicks detected: " << kickPeaks.size());
    INFO("Expected gap: " << expectedGap << " samples");
    INFO("Average drift: " << avgDrift << " samples");
    INFO("Max drift: " << maxDrift << " samples");
}
