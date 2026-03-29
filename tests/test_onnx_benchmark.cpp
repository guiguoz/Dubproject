#include <catch2/catch_test_macros.hpp>

#ifdef SAXFX_HAS_ONNX

#include "dsp/OnnxInference.h"
#include <chrono>
#include <numeric>
#include <vector>

static const char* kIdentityModelPath =
#ifdef IDENTITY_MODEL_PATH
    IDENTITY_MODEL_PATH;
#else
    "../tests/models/identity.onnx";
#endif

TEST_CASE("OnnxBenchmark -- single inference under 2 ms (512 samples)", "[onnx][benchmark]")
{
    dsp::OnnxInference model(kIdentityModelPath);

    std::vector<float> input(512, 0.5f);

    // Warm-up run (first run is always slower due to ORT internals)
    model.run(input);

    // Measure 50 runs
    const int N = 50;
    std::vector<double> timings(N);

    for (int i = 0; i < N; ++i)
    {
        auto t0 = std::chrono::steady_clock::now();
        auto output = model.run(input);
        auto t1 = std::chrono::steady_clock::now();

        timings[i] = std::chrono::duration<double, std::micro>(t1 - t0).count();
        REQUIRE(output.size() == 512);
    }

    // Median
    std::sort(timings.begin(), timings.end());
    double median = timings[N / 2];

    // Mean
    double mean = std::accumulate(timings.begin(), timings.end(), 0.0) / N;

    // P95
    double p95 = timings[static_cast<size_t>(N * 0.95)];

    INFO("Inference median: " << median << " µs");
    INFO("Inference mean:   " << mean << " µs");
    INFO("Inference P95:    " << p95 << " µs");

    // Assert: median under 2 ms (2000 µs)
    REQUIRE(median < 2000.0);

    // Assert: P95 under 2 ms
    REQUIRE(p95 < 2000.0);
}

TEST_CASE("OnnxBenchmark -- 100 sequential inferences throughput", "[onnx][benchmark]")
{
    dsp::OnnxInference model(kIdentityModelPath);

    std::vector<float> input(512, 0.5f);

    // Warm-up
    model.run(input);

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i)
        model.run(input);
    auto t1 = std::chrono::steady_clock::now();

    double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double avgMs   = totalMs / 100.0;

    INFO("100 inferences in " << totalMs << " ms (avg " << avgMs << " ms)");

    // Average should be well under 2 ms for identity model
    REQUIRE(avgMs < 2.0);
}

#else

TEST_CASE("OnnxBenchmark -- skipped (ONNX not enabled)", "[onnx][benchmark]")
{
    SUCCEED("ONNX Runtime not available");
}

#endif
