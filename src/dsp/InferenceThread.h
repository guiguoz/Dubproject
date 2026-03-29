#pragma once

#ifdef SAXFX_HAS_ONNX

#include "LockFreeQueue.h"
#include "OnnxInference.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// InferenceRequest / InferenceResult
//
// Fixed-size messages for the lock-free queues between the audio thread
// and the inference thread.  MaxSamples must be large enough for one
// audio block (typically 512 or 1024).
// ─────────────────────────────────────────────────────────────────────────────
template <int MaxSamples = 1024>
struct InferenceRequest
{
    float data[MaxSamples] {};
    int   numSamples { 0 };
    int   id         { 0 };   // monotonic ID for matching request ↔ result
};

template <int MaxSamples = 1024>
struct InferenceResult
{
    float data[MaxSamples] {};
    int   numSamples { 0 };
    int   id         { 0 };
    bool  valid      { false };
};

// ─────────────────────────────────────────────────────────────────────────────
// InferenceThread
//
// Runs an ONNX model on a background thread.  The audio thread pushes
// requests via a lock-free queue and polls results from another.
//
// Lifecycle:
//   1. Construct with a loaded OnnxInference reference.
//   2. Call start() — spawns the background thread.
//   3. Audio thread: submitRequest() to enqueue, pollResult() to dequeue.
//   4. Call stop() — joins the thread (destructor also calls stop).
//
// The inference thread spins on the request queue (low latency, burns CPU).
// For power-saving, a condition variable could be used instead.
// ─────────────────────────────────────────────────────────────────────────────
template <int MaxSamples = 1024, std::size_t QueueCapacity = 16>
class InferenceThread
{
public:
    using Request = InferenceRequest<MaxSamples>;
    using Result  = InferenceResult<MaxSamples>;

    explicit InferenceThread(OnnxInference& model) noexcept
        : model_(model) {}

    ~InferenceThread() { stop(); }

    void start()
    {
        if (running_.load(std::memory_order_acquire)) return;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
    }

    void stop()
    {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();
    }

    bool isRunning() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    /// Called from the audio thread.  Returns false if queue is full.
    bool submitRequest(const float* buf, int numSamples, int id) noexcept
    {
        if (numSamples <= 0 || numSamples > MaxSamples) return false;
        Request req;
        req.numSamples = numSamples;
        req.id = id;
        std::memcpy(req.data, buf, static_cast<size_t>(numSamples) * sizeof(float));
        return requestQueue_.tryPush(req);
    }

    /// Called from the audio thread.  Returns true if a result was available.
    bool pollResult(Result& out) noexcept
    {
        return resultQueue_.tryPop(out);
    }

    /// Number of pending results available.
    std::size_t resultCount() const noexcept { return resultQueue_.size(); }

private:
    void run()
    {
        while (running_.load(std::memory_order_acquire))
        {
            Request req;
            if (requestQueue_.tryPop(req))
            {
                // Run inference
                std::vector<float> input(req.data,
                    req.data + req.numSamples);
                auto output = model_.run(input);

                Result res;
                res.id = req.id;
                res.numSamples = static_cast<int>(output.size());
                res.valid = true;
                if (res.numSamples > MaxSamples)
                    res.numSamples = MaxSamples;
                std::memcpy(res.data, output.data(),
                    static_cast<size_t>(res.numSamples) * sizeof(float));

                resultQueue_.tryPush(res);
            }
            else
            {
                // Yield to avoid busy-spinning at 100% CPU
                std::this_thread::yield();
            }
        }
    }

    OnnxInference& model_;
    std::atomic<bool> running_ { false };
    std::thread thread_;

    LockFreeQueue<Request, QueueCapacity> requestQueue_;
    LockFreeQueue<Result,  QueueCapacity> resultQueue_;
};

} // namespace dsp

#endif // SAXFX_HAS_ONNX
