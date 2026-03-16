#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

//==============================================================================
// Tests placeholder — Sprint 1
//
// Ces tests valident uniquement que la CI fonctionne.
// Sprint 2+ : remplacer par de vrais tests unitaires DSP :
//   - test_pitch_tracker.cpp  → YIN/aubio sur notes synthétiques
//   - test_reverb.cpp         → validation filtre de réverbération
//   - test_harmonizer.cpp     → sortie harmoniseur (2 voix, intervalles)
//==============================================================================

TEST_CASE("Build system is working", "[scaffold]")
{
    REQUIRE(1 + 1 == 2);
}

TEST_CASE("Basic DSP math", "[dsp][placeholder]")
{
    // Valider que les opérations flottantes de base fonctionnent correctement
    const float sampleRate = 44100.0f;
    const int   bufferSize = 128;
    const float latencyMs  = 1000.0f * static_cast<float>(bufferSize) / sampleRate;

    // Latence d'un buffer de 128 échantillons @ 44.1 kHz ≈ 2.9 ms
    REQUIRE(latencyMs == Catch::Approx(2.902f).epsilon(0.01f));
}

TEST_CASE("Decibel conversion", "[dsp][placeholder]")
{
    // 0 dB = gain 1.0
    const float gainOf1dB0 = std::pow(10.0f, 0.0f / 20.0f);
    REQUIRE(gainOf1dB0 == Catch::Approx(1.0f));

    // -6 dB ≈ gain 0.5
    const float gainOfMinus6dB = std::pow(10.0f, -6.0f / 20.0f);
    REQUIRE(gainOfMinus6dB == Catch::Approx(0.501f).epsilon(0.01f));

    // -inf → gain 0 (silence)
    const float gainOf0 = std::pow(10.0f, -120.0f / 20.0f);
    REQUIRE(gainOf0 == Catch::Approx(0.0f).margin(1e-4f));
}
