#include <catch2/catch_test_macros.hpp>
#include "midi/MidiNoteMapper.h"
#include "dsp/LockFreeQueue.h"

using midi::MidiNoteMapper;
using dsp::LockFreeQueue;
using dsp::SamplerEvent;

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

// ─────────────────────────────────────────────────────────────────────────────
// LockFreeQueue tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LockFreeQueue -- empty on construction", "[lfq]")
{
    LockFreeQueue<SamplerEvent, 64> q;
    REQUIRE(q.empty());
    REQUIRE(q.size() == 0);
}

TEST_CASE("LockFreeQueue -- push and pop round-trip", "[lfq]")
{
    LockFreeQueue<SamplerEvent, 64> q;

    SamplerEvent pushed { 3, true };
    REQUIRE(q.tryPush(pushed));

    SamplerEvent popped;
    REQUIRE(q.tryPop(popped));
    REQUIRE(popped.slotIndex == 3);
    REQUIRE(popped.noteOn == true);
    REQUIRE(q.empty());
}

TEST_CASE("LockFreeQueue -- returns false when empty", "[lfq]")
{
    LockFreeQueue<SamplerEvent, 64> q;
    SamplerEvent e;
    REQUIRE_FALSE(q.tryPop(e));
}

TEST_CASE("LockFreeQueue -- returns false when full", "[lfq]")
{
    LockFreeQueue<SamplerEvent, 4> q; // capacity 4, max 3 items (ring buffer)
    SamplerEvent e { 0, true };
    REQUIRE(q.tryPush(e));
    REQUIRE(q.tryPush(e));
    REQUIRE(q.tryPush(e));
    REQUIRE_FALSE(q.tryPush(e)); // full
}

TEST_CASE("LockFreeQueue -- fifo ordering", "[lfq]")
{
    LockFreeQueue<SamplerEvent, 16> q;
    for (int i = 0; i < 5; ++i)
        q.tryPush({ i, true });

    for (int i = 0; i < 5; ++i)
    {
        SamplerEvent e;
        REQUIRE(q.tryPop(e));
        REQUIRE(e.slotIndex == i);
    }
}
