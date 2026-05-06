#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "midi/EwiDeviceFilter.h"

using midi::ewiDeviceMatches;
using midi::normalizeBreathCc;

// ─────────────────────────────────────────────────────────────────────────────
// ewiDeviceMatches — device name routing logic
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ewiDeviceMatches -- exact match", "[ewi]")
{
    REQUIRE(ewiDeviceMatches("EWI", "EWI"));
}

TEST_CASE("ewiDeviceMatches -- substring match", "[ewi]")
{
    REQUIRE(ewiDeviceMatches("AKAI EWI USB", "EWI"));
}

TEST_CASE("ewiDeviceMatches -- case insensitive", "[ewi]")
{
    REQUIRE(ewiDeviceMatches("Akai ewi usb", "EWI"));
    REQUIRE(ewiDeviceMatches("AKAI EWI USB", "ewi"));
    REQUIRE(ewiDeviceMatches("akai EWI usb", "Ewi"));
}

TEST_CASE("ewiDeviceMatches -- no match", "[ewi]")
{
    REQUIRE_FALSE(ewiDeviceMatches("Roland A-49", "EWI"));
    REQUIRE_FALSE(ewiDeviceMatches("", "EWI"));
}

TEST_CASE("ewiDeviceMatches -- empty filter matches nothing", "[ewi]")
{
    REQUIRE_FALSE(ewiDeviceMatches("AKAI EWI USB", ""));
    REQUIRE_FALSE(ewiDeviceMatches("", ""));
}

// ─────────────────────────────────────────────────────────────────────────────
// normalizeBreathCc — CC2 breath [0..127] → [0..1]
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("normalizeBreathCc -- zero is 0.0f", "[ewi]")
{
    REQUIRE(normalizeBreathCc(0) == Catch::Approx(0.f));
}

TEST_CASE("normalizeBreathCc -- full is 1.0f", "[ewi]")
{
    REQUIRE(normalizeBreathCc(127) == Catch::Approx(1.f));
}

TEST_CASE("normalizeBreathCc -- mid value", "[ewi]")
{
    REQUIRE(normalizeBreathCc(64) == Catch::Approx(64.f / 127.f));
}
