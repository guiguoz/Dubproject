#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "project/ProjectData.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("ProjectData -- default values", "[project]")
{
    project::ProjectData data;
    REQUIRE(data.version == 7);
    REQUIRE(data.projectName == "Untitled");
    REQUIRE_THAT(data.bpm, WithinAbs(120.f, 0.001f));
    REQUIRE(data.midiMappings.empty());
    REQUIRE(data.masterKeyRoot == 0);
    REQUIRE(data.masterKeyMajor == true);
    REQUIRE(data.currentScene == 0);
}

TEST_CASE("ProjectData -- dub delay defaults", "[project]")
{
    project::ProjectData data;
    REQUIRE(data.dubDelayEnabled == false);
    REQUIRE_THAT(data.dubDelaySend,     WithinAbs(0.20f, 0.001f));
    REQUIRE_THAT(data.dubDelayWet,      WithinAbs(0.28f, 0.001f));
    REQUIRE_THAT(data.dubDelayFeedback, WithinAbs(0.48f, 0.001f));
    REQUIRE_THAT(data.dubDelayTone,     WithinAbs(0.55f, 0.001f));
    REQUIRE_THAT(data.dubDelayDrive,    WithinAbs(0.15f, 0.001f));
    REQUIRE(data.dubDelayDiv == 1);
}

TEST_CASE("ProjectData -- SampleConfig defaults", "[project]")
{
    project::SampleConfig s;
    REQUIRE(s.filePath.empty());
    REQUIRE_THAT(s.gain, WithinAbs(1.f, 0.001f));
    REQUIRE(s.loop == false);
    REQUIRE(s.oneShot == true);
    REQUIRE(s.muted == false);
}

TEST_CASE("ProjectData -- MidiMapping", "[project]")
{
    project::MidiMapping m;
    m.midiNote  = 60;
    m.slotIndex = 2;
    REQUIRE(m.midiNote  == 60);
    REQUIRE(m.slotIndex == 2);
}

TEST_CASE("ProjectData -- SceneSaveData defaults", "[project]")
{
    project::SceneSaveData s;
    REQUIRE(s.used == false);
    REQUIRE_THAT(s.bpm, WithinAbs(120.f, 0.001f));
    for (int i = 0; i < 9; ++i)
    {
        REQUIRE(s.trackBarCounts[i] == 1);
        REQUIRE(s.trimStart[i] == 0);
        REQUIRE(s.trimEnd[i]   == -1);
        REQUIRE_THAT(s.delaySends[i], WithinAbs(0.f, 0.001f));
    }
}
