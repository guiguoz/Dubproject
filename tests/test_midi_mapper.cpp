#include <catch2/catch_test_macros.hpp>
#include "midi/MidiNoteMapper.h"

using midi::MidiNoteMapper;

// ─────────────────────────────────────────────────────────────────────────────
// MidiNoteMapper tests (pure C++, no JUCE dependency)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("MidiMapper -- default: all notes unmapped", "[midi]")
{
    MidiNoteMapper m;
    for (int n = 0; n < 128; ++n)
        REQUIRE(m.getSlot(n) == MidiNoteMapper::kUnmapped);
}

TEST_CASE("MidiMapper -- set and get mapping", "[midi]")
{
    MidiNoteMapper m;
    m.setMapping(60, 0);
    m.setMapping(62, 3);
    REQUIRE(m.getSlot(60) == 0);
    REQUIRE(m.getSlot(62) == 3);
    REQUIRE(m.getSlot(61) == MidiNoteMapper::kUnmapped);
}

TEST_CASE("MidiMapper -- out-of-range note is ignored", "[midi]")
{
    MidiNoteMapper m;
    REQUIRE_NOTHROW(m.setMapping(-1,  0));
    REQUIRE_NOTHROW(m.setMapping(128, 0));
    REQUIRE(m.getSlot(-1)  == MidiNoteMapper::kUnmapped);
    REQUIRE(m.getSlot(128) == MidiNoteMapper::kUnmapped);
}

TEST_CASE("MidiMapper -- clearMappings resets all", "[midi]")
{
    MidiNoteMapper m;
    m.setMapping(60, 0);
    m.setMapping(64, 2);
    m.clearMappings();
    REQUIRE(m.getSlot(60) == MidiNoteMapper::kUnmapped);
    REQUIRE(m.getSlot(64) == MidiNoteMapper::kUnmapped);
}
