#include <catch2/catch_test_macros.hpp>

#ifdef SAXFX_HAS_ONNX

#include "dsp/InferenceThread.h"
#include <chrono>
#include <cmath>
#include <thread>

static const char* kIdentityModelPath =
#ifdef IDENTITY_MODEL_PATH
    IDENTITY_MODEL_PATH;
#else
    "../tests/models/identity.onnx";
#endif

TEST_CASE("InferenceThread -- processes single request", "[onnx][thread]")
{
    dsp::OnnxInference model(kIdentityModelPath);
    dsp::InferenceThread<512> thread(model);

    thread.start();
    REQUIRE(thread.isRunning());

    // Submit a ramp
    float input[512];
    for (int i = 0; i < 512; ++i)
        input[i] = static_cast<float>(i) / 512.0f;

    REQUIRE(thread.submitRequest(input, 512, 1));

    // Poll for result (with timeout)
    dsp::InferenceResult<512> result;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool got = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (thread.pollResult(result))
        {
            got = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(got);
    REQUIRE(result.valid);
    REQUIRE(result.id == 1);
    REQUIRE(result.numSamples == 512);

    // Identity: output == input
    for (int i = 0; i < 512; ++i)
        REQUIRE(result.data[i] == input[i]);

    thread.stop();
    REQUIRE_FALSE(thread.isRunning());
}

TEST_CASE("InferenceThread -- processes 100 requests without loss", "[onnx][thread]")
{
    dsp::OnnxInference model(kIdentityModelPath);
    dsp::InferenceThread<512, 128> thread(model); // queue capacity 128

    thread.start();

    const int N = 100;

    // Submit 100 requests
    for (int r = 0; r < N; ++r)
    {
        float buf[512];
        float val = static_cast<float>(r) / static_cast<float>(N);
        for (int i = 0; i < 512; ++i)
            buf[i] = val;

        // Retry if queue full
        while (!thread.submitRequest(buf, 512, r))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Collect all 100 results
    int received = 0;
    bool seen[100] = {};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (received < N && std::chrono::steady_clock::now() < deadline)
    {
        dsp::InferenceResult<512> result;
        if (thread.pollResult(result))
        {
            REQUIRE(result.valid);
            REQUIRE(result.id >= 0);
            REQUIRE(result.id < N);
            seen[result.id] = true;
            ++received;

            // Verify value
            float expected = static_cast<float>(result.id) / static_cast<float>(N);
            for (int i = 0; i < 512; ++i)
                REQUIRE(result.data[i] == expected);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    REQUIRE(received == N);

    // All IDs seen
    for (int i = 0; i < N; ++i)
        REQUIRE(seen[i]);

    thread.stop();
}

TEST_CASE("InferenceThread -- latency under 5 ms per inference", "[onnx][thread]")
{
    dsp::OnnxInference model(kIdentityModelPath);
    dsp::InferenceThread<512> thread(model);

    thread.start();

    float input[512] = {};
    for (int i = 0; i < 512; ++i)
        input[i] = 0.5f;

    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(thread.submitRequest(input, 512, 0));

    dsp::InferenceResult<512> result;
    while (!thread.pollResult(result))
        std::this_thread::yield();

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    REQUIRE(result.valid);
    // Must be under 5 ms (5000 µs)
    REQUIRE(ms < 5000);

    thread.stop();
}

#else

TEST_CASE("InferenceThread -- skipped (ONNX not enabled)", "[onnx][thread]")
{
    SUCCEED("ONNX Runtime not available");
}

#endif
