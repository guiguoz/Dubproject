#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include <cmath>
#include <atomic>
#include <memory>
#include "engine/SceneTransitionPlan.h"
#include "ui/WaveformThreadPool.h"

using Catch::Approx;

// Réplique de WaveformCache::computeEnvelope (sans dépendance JUCE)
// Les tests d'intégration STOPPED_NAV_WAVEFORM et WAVE_CACHE_STALE_CALLBACK
// requièrent un runtime JUCE (MessageManager) et sont couverts manuellement.
static std::vector<float> computeEnvelope(const std::vector<float>& pcm, int bins = 200)
{
    if (pcm.empty() || bins <= 0) return {};
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

// ─── WAVEFORM-1 : computeEnvelope — cas de base ────────────────────────────
TEST_CASE("WAVEFORM-1: computeEnvelope returns empty for empty input", "[waveform]")
{
    CHECK(computeEnvelope({}).empty());
    CHECK(computeEnvelope({}, 0).empty());
}

TEST_CASE("WAVEFORM-2: computeEnvelope returns correct bin count", "[waveform]")
{
    std::vector<float> pcm(44100, 0.5f);
    auto env = computeEnvelope(pcm, 200);
    REQUIRE(env.size() == 200);
    for (auto v : env)
        CHECK(v == Approx(0.5f).epsilon(0.01f));
}

TEST_CASE("WAVEFORM-3: computeEnvelope silent signal → all zeros", "[waveform]")
{
    std::vector<float> pcm(1000, 0.f);
    auto env = computeEnvelope(pcm, 50);
    for (auto v : env)
        CHECK(v == Approx(0.f));
}

TEST_CASE("WAVEFORM-4: computeEnvelope peak detection", "[waveform]")
{
    // Sinus 440 Hz, amplitude 0.8
    std::vector<float> pcm(4410);
    for (int i = 0; i < 4410; ++i)
        pcm[i] = 0.8f * std::sin(2.f * 3.14159265f * 440.f * static_cast<float>(i) / 44100.f);

    auto env = computeEnvelope(pcm, 100);
    REQUIRE(env.size() == 100);
    // Peak attendu ~0.8
    for (auto v : env)
        CHECK(v <= 0.81f);
    // Au moins quelques bins proches du max
    float maxBin = *std::max_element(env.begin(), env.end());
    CHECK(maxBin > 0.75f);
}

// ─── WAVEFORM-5 : assetIdFor — déduplication par (path + trim) ─────────────
TEST_CASE("WAVEFORM-5: assetIdFor same path same trim → same id", "[waveform]")
{
    auto id1 = engine::assetIdFor("C:/samples/kick.wav", -1, -1);
    auto id2 = engine::assetIdFor("C:/samples/kick.wav", -1, -1);
    CHECK(id1 == id2);
    CHECK(id1 != 0);
}

TEST_CASE("WAVEFORM-6: assetIdFor same path different trim → different id", "[waveform]")
{
    auto id1 = engine::assetIdFor("C:/samples/kick.wav", 0,   -1);
    auto id2 = engine::assetIdFor("C:/samples/kick.wav", 100, -1);
    CHECK(id1 != id2);
}

TEST_CASE("WAVEFORM-7: assetIdFor different paths → different ids", "[waveform]")
{
    auto id1 = engine::assetIdFor("C:/samples/kick.wav",  -1, -1);
    auto id2 = engine::assetIdFor("C:/samples/snare.wav", -1, -1);
    CHECK(id1 != id2);
}

TEST_CASE("WAVEFORM-8: assetIdFor empty path → 0", "[waveform]")
{
    CHECK(engine::assetIdFor("", -1, -1) == 0);
}

// ─── WAVEFORM-9 : assetIdFor case-insensitive + backslash normalisation ─────
TEST_CASE("WAVEFORM-9: assetIdFor normalises case and separators", "[waveform]")
{
    auto id1 = engine::assetIdFor("C:/Samples/Kick.WAV", -1, -1);
    auto id2 = engine::assetIdFor("c:\\samples\\kick.wav", -1, -1);
    CHECK(id1 == id2);
}

// ─── Note : tests d'intégration STOPPED_NAV_WAVEFORM et WAVE_CACHE_STALE_CALLBACK
// nécessitent le runtime JUCE (MessageManager::callAsync) et des fichiers audio réels.
// Vérification manuelle :
//   1. Projet avec scène A (slot 0 = X.wav) et scène B (slot 0 = Y.wav)
//   2. Transport arrêté → nav A→B : waveform slot 0 doit changer (Y.wav)
//   3. Nav rapide A→B→A : waveform finale = celle de X.wav (job B annulé)

// ─── Tests WaveformThreadPool ─────────────────────────────────────────────────

// Le pool draine la queue complète avant que les workers quittent.
TEST_CASE("POOL_QUIT_WHILE_JOBS: destructor drains all jobs without deadlock", "[pool]")
{
    std::atomic<int> count{0};
    {
        WaveformThreadPool pool(2);
        for (int i = 0; i < 10; ++i)
        {
            auto token = std::make_shared<std::atomic<bool>>(false);
            pool.enqueue(token,
                []() -> std::vector<float> { return std::vector<float>(50, 0.5f); },
                [&count](std::vector<float>) { ++count; });
        }
        // pool destructor : stopping_=true, cv_.notify_all(), workers drainent puis joignent
    }
    CHECK(count.load() == 10);
}

// Token déjà à true avant enqueue → worker annule avant IO, onResult jamais appelé.
TEST_CASE("POOL_NAV_STRESS: pre-cancelled tokens suppress onResult", "[pool]")
{
    WaveformThreadPool pool(2);
    std::atomic<int> count{0};

    for (int i = 0; i < 20; ++i)
    {
        auto token = std::make_shared<std::atomic<bool>>(true);  // annulé avant enqueue
        pool.enqueue(token,
            []() -> std::vector<float> { return std::vector<float>(100, 0.1f); },
            [&count](std::vector<float>) { ++count; });
    }

    pool.waitForIdle();
    CHECK(count.load() == 0);
}

// Deux enqueueUnique avec le même assetId : second retourne false, résultat livré une seule fois.
TEST_CASE("POOL_DEDUP: enqueueUnique delivers result exactly once", "[pool]")
{
    WaveformThreadPool pool(1);  // un seul worker pour la déterminisme
    std::atomic<int> count{0};

    const engine::AssetId id = engine::assetIdFor("some/file.wav", -1, -1);
    auto compute  = []() -> std::vector<float> { return std::vector<float>(100, 0.5f); };
    auto onResult = [&count](std::vector<float>) { ++count; };

    const bool first  = pool.enqueueUnique(id, compute, onResult);
    const bool second = pool.enqueueUnique(id, compute, onResult);

    pool.waitForIdle();

    CHECK(first  == true);
    CHECK(second == false);
    CHECK(count.load() == 1);
}
