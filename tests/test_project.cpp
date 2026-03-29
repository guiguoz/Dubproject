#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/EffectFactory.h"
#include "dsp/IEffect.h"
#include "dsp/FlangerEffect.h"
#include "dsp/HarmonizerEffect.h"
#include "dsp/DelayEffect.h"
#include "project/ProjectData.h"

using Catch::Matchers::WithinAbs;

// ─────────────────────────────────────────────────────────────────────────────
// EffectFactory tests (pure C++, no JUCE)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("EffectFactory -- effectTypeName round-trips all known types", "[factory]")
{
    using dsp::EffectType;
    const EffectType types[] = {
        EffectType::Flanger, EffectType::Harmonizer, EffectType::Reverb,
        EffectType::PitchFork, EffectType::EnvelopeFilter, EffectType::Delay,
        EffectType::Whammy, EffectType::Octaver, EffectType::Tuner,
        EffectType::Slicer, EffectType::AutoPitchCorrect
    };
    for (auto t : types)
    {
        const char* name = dsp::effectTypeName(t);
        REQUIRE(name != nullptr);
        REQUIRE(std::string(name) != "Unknown");
    }
}

TEST_CASE("EffectFactory -- createEffect(string) creates correct type", "[factory]")
{
    auto flanger = dsp::createEffect("Flanger");
    REQUIRE(flanger != nullptr);
    REQUIRE(flanger->type() == dsp::EffectType::Flanger);

    auto harm = dsp::createEffect("Harmonizer");
    REQUIRE(harm != nullptr);
    REQUIRE(harm->type() == dsp::EffectType::Harmonizer);

    auto delay = dsp::createEffect("Delay");
    REQUIRE(delay != nullptr);
    REQUIRE(delay->type() == dsp::EffectType::Delay);
}

TEST_CASE("EffectFactory -- createEffect unknown name returns nullptr", "[factory]")
{
    REQUIRE(dsp::createEffect("GigaDistortion9000") == nullptr);
    REQUIRE(dsp::createEffect("") == nullptr);
}

TEST_CASE("EffectFactory -- createEffect(EffectType) matches typeName", "[factory]")
{
    using dsp::EffectType;
    const EffectType types[] = {
        EffectType::Flanger, EffectType::Harmonizer, EffectType::Delay,
        EffectType::Octaver, EffectType::Slicer, EffectType::AutoPitchCorrect
    };
    for (auto t : types)
    {
        auto fx = dsp::createEffect(t);
        REQUIRE(fx != nullptr);
        REQUIRE(fx->type() == t);
    }
}

TEST_CASE("EffectFactory -- created effects have non-zero paramCount", "[factory]")
{
    auto flanger = dsp::createEffect("Flanger");
    REQUIRE(flanger->paramCount() > 0);

    auto harm = dsp::createEffect("Harmonizer");
    REQUIRE(harm->paramCount() > 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// ProjectData structure tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ProjectData -- default version is 4", "[project]")
{
    project::ProjectData data;
    REQUIRE(data.version == 4);
    REQUIRE(data.projectName == "Untitled");
    REQUIRE(data.effectChain.empty());
    REQUIRE(data.midiMappings.empty());
}

TEST_CASE("ProjectData -- EffectSlotData stores type and params", "[project]")
{
    project::EffectSlotData slot;
    slot.type    = "Harmonizer";
    slot.enabled = true;
    slot.params  = { 4.0f, 7.0f, 0.45f };

    REQUIRE(slot.type == "Harmonizer");
    REQUIRE(slot.params.size() == 3);
    REQUIRE_THAT(slot.params[0], WithinAbs(4.0f, 0.001f));
    REQUIRE_THAT(slot.params[2], WithinAbs(0.45f, 0.001f));
}

TEST_CASE("ProjectData -- effectChain can hold multiple effects", "[project]")
{
    project::ProjectData data;
    data.projectName = "Test Patch";

    {
        project::EffectSlotData s;
        s.type = "Flanger";
        s.enabled = true;
        s.params = { 0.5f, 0.7f, 0.3f, 0.5f };
        data.effectChain.push_back(s);
    }
    {
        project::EffectSlotData s;
        s.type = "Reverb";
        s.enabled = false;
        s.params = { 0.7f, 0.4f, 0.85f, 0.22f };
        data.effectChain.push_back(s);
    }

    REQUIRE(data.effectChain.size() == 2);
    REQUIRE(data.effectChain[0].type == "Flanger");
    REQUIRE(data.effectChain[1].type == "Reverb");
    REQUIRE_FALSE(data.effectChain[1].enabled);
}

// ─────────────────────────────────────────────────────────────────────────────
// Factory + setParam integration: create effect, apply params, verify
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("EffectFactory -- create Flanger and apply params from slot", "[factory][integration]")
{
    project::EffectSlotData slot;
    slot.type    = "Flanger";
    slot.enabled = true;
    slot.params  = { 1.5f, 0.6f, 0.4f, 0.3f };

    auto fx = dsp::createEffect(slot.type);
    REQUIRE(fx != nullptr);

    const int pc = fx->paramCount();
    for (int p = 0; p < pc && p < static_cast<int>(slot.params.size()); ++p)
        fx->setParam(p, slot.params[static_cast<std::size_t>(p)]);

    REQUIRE_THAT(fx->getParam(0), WithinAbs(1.5f, 0.001f));
    REQUIRE_THAT(fx->getParam(1), WithinAbs(0.6f, 0.001f));
}

TEST_CASE("EffectFactory -- create Harmonizer and verify type name", "[factory]")
{
    auto fx = dsp::createEffect("Harmonizer");
    REQUIRE(fx != nullptr);
    REQUIRE(std::string(dsp::effectTypeName(fx->type())) == "Harmonizer");
}
