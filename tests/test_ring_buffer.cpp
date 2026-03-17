#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/RingBuffer.h"

using dsp::RingBuffer;
using Catch::Matchers::WithinAbs;

TEST_CASE("RingBuffer -- basic push and read", "[ring_buffer]")
{
    RingBuffer<16> rb;
    rb.setSize(8);

    // Push 3 samples
    rb.push(1.0f);
    rb.push(2.0f);
    rb.push(3.0f);

    // Most recent sample = delay 0
    REQUIRE_THAT(rb.read(0.0f), WithinAbs(3.0f, 1e-5f));
    // One sample ago
    REQUIRE_THAT(rb.read(1.0f), WithinAbs(2.0f, 1e-5f));
    // Two samples ago
    REQUIRE_THAT(rb.read(2.0f), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("RingBuffer -- wrap-around", "[ring_buffer]")
{
    RingBuffer<8> rb;
    rb.setSize(8);

    // Fill exactly one buffer size
    for (int i = 0; i < 8; ++i)
        rb.push(static_cast<float>(i));

    // Most recent = 7
    REQUIRE_THAT(rb.read(0.0f), WithinAbs(7.0f, 1e-5f));
    // Push one more — wrap occurs
    rb.push(100.0f);
    REQUIRE_THAT(rb.read(0.0f), WithinAbs(100.0f, 1e-5f));
    REQUIRE_THAT(rb.read(1.0f), WithinAbs(7.0f,   1e-5f));
}

TEST_CASE("RingBuffer -- fractional delay (linear interpolation)", "[ring_buffer]")
{
    RingBuffer<16> rb;
    rb.setSize(16);

    rb.push(0.0f);
    rb.push(1.0f); // delay 0 → 1.0
                   // delay 1 → 0.0
    // delay 0.5 should interpolate to 0.5
    REQUIRE_THAT(rb.read(0.5f), WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("RingBuffer -- reset clears all samples", "[ring_buffer]")
{
    RingBuffer<16> rb;
    rb.setSize(8);
    rb.push(5.0f);
    rb.push(6.0f);
    rb.reset();

    REQUIRE_THAT(rb.read(0.0f), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(rb.read(1.0f), WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("RingBuffer -- setSize clamps to MaxSize", "[ring_buffer]")
{
    RingBuffer<8> rb;
    rb.setSize(100); // larger than MaxSize
    REQUIRE(rb.getSize() == 8);
}
