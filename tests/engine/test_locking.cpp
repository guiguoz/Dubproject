#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include <cmath>
#include <algorithm>
#include "engine/Transport.h"
#include "engine/EventScheduler.h"
#include "engine/Sequencer.h"

using namespace engine;

// ─────────────────────────────────────────────────────────────────────────────
// Helper : calcul sample pour un step
// ─────────────────────────────────────────────────────────────────────────────
static int64_t sampleOfStep(const TransportState& ts, int step) noexcept {
    return static_cast<int64_t>(std::ceil(ts.samplesPerStep * step));
}

// ─────────────────────────────────────────────────────────────────────────────
// T-LOCK2 : Position dérivée stable sur longue durée
// ─────────────────────────────────────────────────────────────────────────────
// Objectif : Valider que les triggers du kick sont parfaitement réguliers
// sur 60 secondes à 120 BPM. Si les triggers ont une dérive < 1 sample,
// la position dérivée du Transport est stable.
//
// Setup :
// - Projet 120 BPM
// - Pattern : tous les 4 steps activés (= kick toutes les mesures)
// - 60 secondes de rendu
// - Mesurer la régularité des espacements de triggers
//
TEST_CASE("T-LOCK2: trigger timing stability over 60 seconds", "[locking]") {
    const double sr  = 44100.0;
    const double bpm = 120.0;
    
    TransportState ts;
    ts.sampleRate     = sr;
    ts.bpm            = bpm;
    ts.samplesPerBeat = sr * 60.0 / bpm;  // 22050 samples
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;  // 5512.5 samples
    ts.samplePos      = 0;
    ts.playing        = true;
    
    // Setup séquenceur : pattern avec triggers tous les 4 steps
    Sequencer seq;
    {
        TrackPattern* pat = seq.patterns().writeBuffer(0);
        pat->numSteps = 16;
        // Triggers sur steps 0, 4, 8, 12 (tous les 4 steps = tous les beats)
        for (int i = 0; i < 16; ++i)
            pat->steps[i] = (i % 4 == 0);
        seq.patterns().flip();
    }
    
    // === Générer les événements sur 60 secondes ===
    const int64_t totalSamples = static_cast<int64_t>(60.0 * sr);  // 2,646,000 samples
    const int32_t blockSize = 512;
    std::vector<int64_t> triggerTimes;
    
    int64_t blockStart = 0;
    while (blockStart < totalSamples) {
        int32_t numSamples = static_cast<int32_t>(
            std::min(static_cast<int64_t>(blockSize), totalSamples - blockStart));
        
        // Update transport position for this block
        ts.samplePos = blockStart;
        
        // Generate events
        EventScheduler sched;
        seq.generateEvents(ts, blockStart, numSamples, 0.0f, sched);
        
        // Collect all trigger times (slot 0)
        for (size_t i = 0; i < sched.size(); ++i) {
            const EngineEvent& ev = sched.at(i);
            if (ev.type == EventType::Trigger && ev.slot == 0) {
                triggerTimes.push_back(ev.time);
            }
        }
        
        blockStart += numSamples;
    }
    
    // === Analyse : vérifier la régularité ===
    // Les triggers doivent être espacés régulièrement : tous les 4 steps = 22050 samples
    const int64_t expectedGap = static_cast<int64_t>(ts.samplesPerBeat + 0.5);
    
    // Au moins 50 triggers en 60 sec (à 120 BPM : 1 trigger/beat = 60 triggers)
    REQUIRE(triggerTimes.size() > 50);
    
    // Mesurer l'espacement entre chaque trigger consécutif
    std::vector<double> gaps;
    std::vector<double> drifts;
    double maxDrift = 0.0;
    double sumDrift = 0.0;
    
    for (size_t i = 1; i < triggerTimes.size(); ++i) {
        int64_t gap = triggerTimes[i] - triggerTimes[i - 1];
        gaps.push_back(static_cast<double>(gap));
        
        double drift = std::abs(gap - expectedGap);
        drifts.push_back(drift);
        maxDrift = std::max(maxDrift, drift);
        sumDrift += drift;
    }
    
    double avgDrift = sumDrift / (triggerTimes.size() - 1);
    double minGap = *std::min_element(gaps.begin(), gaps.end());
    double maxGap = *std::max_element(gaps.begin(), gaps.end());
    
    // === Critères de validation ===
    // La position dérivée doit être très stable
    REQUIRE(avgDrift < 0.1);   // Dérive moyenne < 0.1 sample
    REQUIRE(maxDrift < 1.0);   // Dérive max < 1 sample
    REQUIRE(minGap >= expectedGap - 1.0);  // Aucun gap trop court
    REQUIRE(maxGap <= expectedGap + 1.0);  // Aucun gap trop long
    
    // === Log pour diagnostic ===
    INFO("Triggers collected: " << triggerTimes.size());
    INFO("Expected gap: " << expectedGap << " samples (~" << (expectedGap / sr) << " sec)");
    INFO("Min gap: " << minGap << " samples");
    INFO("Max gap: " << maxGap << " samples");
    INFO("Average drift: " << avgDrift << " samples");
    INFO("Max drift: " << maxDrift << " samples");
}
