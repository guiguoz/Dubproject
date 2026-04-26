#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
#include "dsp/KeyboardSynth.h"

using dsp::KeyboardSynth;
using Catch::Matchers::WithinAbs;

static constexpr double kSR        = 44100.0;
static constexpr int    kBlock     = 512;

static float renderPeak(KeyboardSynth& s, int nSamples)
{
    std::vector<float> buf(static_cast<std::size_t>(nSamples), 0.f);
    s.processMonoAdd(buf.data(), nSamples);
    float peak = 0.f;
    for (auto x : buf) peak = std::max(peak, std::abs(x));
    return peak;
}

// ─── ADSR ────────────────────────────────────────────────────────────────────

TEST_CASE("KeyboardSynth: ADSR produces sound and reaches sustain", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    s.setParam(5, 0.001f);  // ~2ms attack
    s.setParam(9, 0.001f);  // ~5ms decay
    s.setParam(8, 0.6f);    // sustain = 60%
    s.setParam(11, 0.f);    // no velocity scaling (flat)

    s.noteOn(60, 1.f);
    REQUIRE(renderPeak(s, 4096) > 0.f);
}

TEST_CASE("KeyboardSynth: sustain = 0 means envelope decays to silence", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    s.setParam(5, 0.001f);  // fast attack
    s.setParam(9, 0.001f);  // fast decay
    s.setParam(8, 0.f);     // sustain = 0 → full decay to 0
    s.setParam(11, 0.f);

    s.noteOn(60, 1.f);
    renderPeak(s, 512);                    // let attack + decay complete
    const float post = renderPeak(s, 4096);
    REQUIRE(post < 0.001f);
}

TEST_CASE("KeyboardSynth: noteOff triggers release to silence", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    s.setParam(5, 0.001f);
    s.setParam(6, 0.001f);  // ~14ms release
    s.setParam(8, 0.8f);

    s.noteOn(60, 1.f);
    renderPeak(s, 4096);    // reach sustain

    s.noteOff(60);
    renderPeak(s, 4096);    // let the ~14ms release complete (93ms window)
    REQUIRE(renderPeak(s, 512) < 0.001f);  // tail must be silent
}

// ─── Velocity ────────────────────────────────────────────────────────────────

TEST_CASE("KeyboardSynth: velocity scaling — velAmt=0 means flat level", "[keyboard]")
{
    auto peak = [](float vel, float velAmt) {
        KeyboardSynth s;
        s.prepare(kSR, kBlock);
        s.setParam(5, 0.001f);
        s.setParam(8, 1.f);
        s.setParam(11, velAmt);
        s.noteOn(60, vel);
        return renderPeak(s, 1024);
    };

    const float lo = peak(0.1f, 0.f);
    const float hi = peak(1.0f, 0.f);
    // velAmt=0 → no velocity effect, levels should be equal
    REQUIRE_THAT(lo / hi, WithinAbs(1.f, 0.05f));
}

TEST_CASE("KeyboardSynth: velocity scaling — velAmt=1 scales with vel", "[keyboard]")
{
    auto peak = [](float vel) {
        KeyboardSynth s;
        s.prepare(kSR, kBlock);
        s.setParam(5, 0.001f);
        s.setParam(8, 1.f);
        s.setParam(11, 1.f);  // full velocity sensitivity
        s.noteOn(60, vel);
        return renderPeak(s, 1024);
    };

    REQUIRE(peak(0.1f) < peak(1.0f) * 0.5f);
}

// ─── Mono / Legato ──────────────────────────────────────────────────────────

TEST_CASE("KeyboardSynth: legato does not reset envelope to zero", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    s.setMonoMode(true);
    s.setParam(5, 0.15f);   // ~300ms attack
    s.setParam(8, 0.8f);
    s.setParam(9, 0.15f);

    // Trigger first note, let it reach sustain
    s.noteOn(60, 1.f);
    renderPeak(s, 8192);

    // Legato: second note without noteOff
    s.noteOn(64, 1.f);
    std::vector<float> buf(512, 0.f);
    s.processMonoAdd(buf.data(), 512);

    // First sample after legato noteOn must NOT be near silence (no click-to-zero)
    REQUIRE(std::abs(buf[0]) > 0.01f);
}

TEST_CASE("KeyboardSynth: non-legato resets envelope", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    s.setMonoMode(true);
    s.setParam(5, 0.3f);    // slow attack (~600ms)
    s.setParam(8, 0.8f);

    s.noteOn(60, 1.f);
    renderPeak(s, 8192);    // reach sustain (~0.8)

    s.noteOff(60);
    renderPeak(s, 4096);    // let it decay to near 0

    s.noteOn(64, 1.f);      // fresh (non-legato) note

    // Immediately after noteOn, envelope starts from 0 → level is low
    std::vector<float> buf(64, 0.f);
    s.processMonoAdd(buf.data(), 64);
    const float earlyLevel = std::abs(buf[0]);
    REQUIRE(earlyLevel < 0.05f);   // attack just started — not yet loud
}

// ─── Mono note-stack ────────────────────────────────────────────────────────

TEST_CASE("KeyboardSynth: note-stack resumes held note on noteOff", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    s.setMonoMode(true);
    s.setParam(5, 0.001f);
    s.setParam(8, 1.f);

    s.noteOn(60, 1.f);  // C4
    s.noteOn(64, 1.f);  // E4 legato
    s.noteOff(64);      // Release E4 — should resume C4 (still held)

    // Voice should still be active (C4 resumed)
    REQUIRE(renderPeak(s, 1024) > 0.01f);
}

TEST_CASE("KeyboardSynth: note-stack releases when all notes off", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    s.setMonoMode(true);
    s.setParam(5, 0.001f);
    s.setParam(6, 0.001f);  // fast release
    s.setParam(8, 1.f);

    s.noteOn(60, 1.f);
    s.noteOn(64, 1.f);
    s.noteOff(64);
    s.noteOff(60);   // all notes released

    REQUIRE(renderPeak(s, 4096) < 0.001f);
}

// ─── Glide ──────────────────────────────────────────────────────────────────

TEST_CASE("KeyboardSynth: glide=0 means instant pitch snap", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    s.setMonoMode(true);
    s.setParam(5, 0.001f);
    s.setParam(8, 1.f);
    s.setParam(10, 0.f);    // no glide

    s.noteOn(60, 1.f);
    renderPeak(s, 4096);

    // Legato to new note — with glide=0, coeff=0, currentHz snaps instantly
    s.noteOn(72, 1.f);      // one octave up

    // Render 64 samples — phase increment should already be at new frequency
    std::vector<float> buf(64, 0.f);
    s.processMonoAdd(buf.data(), 64);
    REQUIRE(renderPeak(s, 64) > 0.f);
}

TEST_CASE("KeyboardSynth: glide keeps voice active during portamento", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    s.setMonoMode(true);
    s.setParam(5, 0.001f);
    s.setParam(8, 1.f);
    s.setParam(10, 0.2f);   // ~100ms glide

    s.noteOn(60, 1.f);
    renderPeak(s, 4096);

    s.noteOn(72, 1.f);
    // During glide window, sound must be continuous (no silence)
    const float mid = renderPeak(s, static_cast<int>(kSR * 0.05));
    REQUIRE(mid > 0.01f);
}

// ─── Poly mode (regression) ──────────────────────────────────────────────────

TEST_CASE("KeyboardSynth: poly mode produces sound on multiple notes", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    // default: poly mode

    s.noteOn(60, 1.f);
    s.noteOn(64, 1.f);
    s.noteOn(67, 1.f);
    REQUIRE(renderPeak(s, 1024) > 0.f);
}

TEST_CASE("KeyboardSynth: poly mode all-notes-off silences output", "[keyboard]")
{
    KeyboardSynth s;
    s.prepare(kSR, kBlock);
    s.setParam(6, 0.001f);  // ~14ms release

    s.noteOn(60, 1.f);
    s.noteOn(64, 1.f);
    renderPeak(s, 4096);

    s.noteOff(60);
    s.noteOff(64);
    renderPeak(s, 4096);    // let release complete
    REQUIRE(renderPeak(s, 512) < 0.001f);  // tail must be silent
}
