#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "dsp/EffectChain.h"
#include "dsp/FlangerEffect.h"
#include "dsp/HarmonizerEffect.h"
#include "dsp/EnvelopeFilterEffect.h"

#include <array>
#include <memory>
#include <numeric>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal test effect: multiplies every sample by a fixed gain.
// ─────────────────────────────────────────────────────────────────────────────
class GainEffect : public dsp::IEffect
{
public:
    explicit GainEffect(float gain) : gain_(gain) {}

    dsp::EffectType type() const noexcept override { return dsp::EffectType::Flanger; }
    void prepare(double, int) noexcept override {}
    void process(float* buf, int n, float) noexcept override
    {
        for (int i = 0; i < n; ++i) buf[i] *= gain_;
    }
    void reset() noexcept override {}
    int              paramCount()           const noexcept override { return 0; }
    dsp::ParamDescriptor paramDescriptor(int) const noexcept override { return {}; }
    float            getParam(int)          const noexcept override { return 0.0f; }
    void             setParam(int, float)         noexcept override {}

    float gain() const noexcept { return gain_; }
private:
    float gain_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::array<float, 64> makeSine(float amplitude = 0.5f)
{
    std::array<float, 64> buf;
    buf.fill(amplitude);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("EffectChain: empty chain passes signal through unchanged")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 64);

    auto buf = makeSine();
    chain.process(buf.data(), 64, 440.0f);

    // All samples unchanged
    for (float s : buf)
        REQUIRE(s == Catch::Approx(0.5f));
}

TEST_CASE("EffectChain: added effect is called")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 64);

    REQUIRE(chain.addEffect(std::make_unique<GainEffect>(2.0f)));

    auto buf = makeSine();
    chain.process(buf.data(), 64, 440.0f);

    // GainEffect x2 -> 1.0
    for (float s : buf)
        REQUIRE(s == Catch::Approx(1.0f));
}

TEST_CASE("EffectChain: effects applied in order")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 64);

    chain.addEffect(std::make_unique<GainEffect>(2.0f));   // x2 -> 1.0
    chain.addEffect(std::make_unique<GainEffect>(0.25f));  // x0.25 -> 0.25

    auto buf = makeSine();
    chain.process(buf.data(), 64, 440.0f);

    for (float s : buf)
        REQUIRE(s == Catch::Approx(0.25f));
}

TEST_CASE("EffectChain: effectCount returns correct value")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 64);

    REQUIRE(chain.effectCount() == 0);
    chain.addEffect(std::make_unique<GainEffect>(1.0f));
    REQUIRE(chain.effectCount() == 1);
    chain.addEffect(std::make_unique<GainEffect>(1.0f));
    REQUIRE(chain.effectCount() == 2);
}

TEST_CASE("EffectChain: getEffect returns correct pointer")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 64);

    chain.addEffect(std::make_unique<GainEffect>(3.0f));
    auto* eff = static_cast<GainEffect*>(chain.getEffect(0));
    REQUIRE(eff != nullptr);
    REQUIRE(eff->gain() == Catch::Approx(3.0f));

    REQUIRE(chain.getEffect(-1) == nullptr);
    REQUIRE(chain.getEffect(1)  == nullptr);
}

TEST_CASE("EffectChain: removed effect no longer processes after drain")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 64);

    chain.addEffect(std::make_unique<GainEffect>(10.0f));
    chain.removeEffect(0);
    REQUIRE(chain.effectCount() == 0);

    // Simulate audio thread draining the command
    chain.drainCommands();
    chain.collectGarbage();

    auto buf = makeSine();
    chain.process(buf.data(), 64, 440.0f);

    // Gain x10 must NOT have been applied
    for (float s : buf)
        REQUIRE(s == Catch::Approx(0.5f));
}

TEST_CASE("EffectChain: remove middle effect preserves others")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 64);

    chain.addEffect(std::make_unique<GainEffect>(2.0f));   // index 0
    chain.addEffect(std::make_unique<GainEffect>(100.0f)); // index 1 <- remove this
    chain.addEffect(std::make_unique<GainEffect>(0.5f));   // index 2

    chain.removeEffect(1);
    chain.drainCommands();
    chain.collectGarbage();

    REQUIRE(chain.effectCount() == 2);

    auto buf = makeSine();
    chain.process(buf.data(), 64, 440.0f);

    // 0.5 * 2.0 * 0.5 = 0.5 (x100 removed)
    for (float s : buf)
        REQUIRE(s == Catch::Approx(0.5f));
}

TEST_CASE("EffectChain: moveEffect reorders chain")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 64);

    // Start: [x2, x0.1]  applied in order -> 0.5 * 2 * 0.1 = 0.1
    chain.addEffect(std::make_unique<GainEffect>(2.0f));
    chain.addEffect(std::make_unique<GainEffect>(0.1f));

    {
        auto buf = makeSine();
        chain.process(buf.data(), 64, 440.0f);
        for (float s : buf)
            REQUIRE(s == Catch::Approx(0.1f));
    }

    // After move(0->1): [x0.1, x2] -> same result (multiplication is commutative)
    chain.moveEffect(0, 1);

    {
        auto buf = makeSine();
        chain.process(buf.data(), 64, 440.0f);
        for (float s : buf)
            REQUIRE(s == Catch::Approx(0.1f));
    }

    // Order check: first effect is now GainEffect(0.1)
    auto* first = static_cast<GainEffect*>(chain.getEffect(0));
    REQUIRE(first->gain() == Catch::Approx(0.1f));
}

TEST_CASE("EffectChain: disabled effect is skipped")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 64);

    chain.addEffect(std::make_unique<GainEffect>(10.0f));
    chain.getEffect(0)->enabled.store(false, std::memory_order_release);

    auto buf = makeSine();
    chain.process(buf.data(), 64, 440.0f);

    // Gain x10 skipped -> unchanged
    for (float s : buf)
        REQUIRE(s == Catch::Approx(0.5f));
}

TEST_CASE("EffectChain: respects max chain length")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 64);

    for (int i = 0; i < dsp::EffectChain::kMaxEffects; ++i)
        REQUIRE(chain.addEffect(std::make_unique<GainEffect>(1.0f)));

    // Next add must fail
    REQUIRE_FALSE(chain.addEffect(std::make_unique<GainEffect>(1.0f)));
    REQUIRE(chain.effectCount() == dsp::EffectChain::kMaxEffects);
}

TEST_CASE("EffectChain: FlangerEffect integrates without crash")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 512);
    chain.addEffect(std::make_unique<dsp::FlangerEffect>());

    std::array<float, 512> buf;
    buf.fill(0.3f);
    chain.process(buf.data(), 512, 440.0f);

    // Flanger must keep output in [-1, 1]
    for (float s : buf)
    {
        REQUIRE(s >= -1.0f);
        REQUIRE(s <=  1.0f);
    }
}

TEST_CASE("EffectChain: HarmonizerEffect integrates without crash")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 512);
    chain.addEffect(std::make_unique<dsp::HarmonizerEffect>());

    std::array<float, 512> buf;
    buf.fill(0.3f);
    chain.process(buf.data(), 512, 440.0f);

    for (float s : buf)
    {
        REQUIRE(s >= -1.0f);
        REQUIRE(s <=  1.0f);
    }
}

TEST_CASE("EffectChain: EnvelopeFilterEffect integrates without crash")
{
    dsp::EffectChain chain;
    chain.prepare(44100.0, 512);
    chain.addEffect(std::make_unique<dsp::EnvelopeFilterEffect>());

    std::array<float, 512> buf;
    buf.fill(0.3f);
    chain.process(buf.data(), 512, 440.0f);

    for (float s : buf)
    {
        REQUIRE(s >= -1.0f);
        REQUIRE(s <=  1.0f);
    }
}
