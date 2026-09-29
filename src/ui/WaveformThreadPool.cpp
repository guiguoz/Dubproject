#include "WaveformThreadPool.h"
#include <algorithm>
#include <cmath>

WaveformThreadPool::WaveformThreadPool(int numWorkers)
{
    workers_.reserve(static_cast<std::size_t>(numWorkers));
    for (int i = 0; i < numWorkers; ++i)
        workers_.emplace_back([this] { workerLoop(); });
}

WaveformThreadPool::~WaveformThreadPool()
{
    {
        std::lock_guard<std::mutex> lk(mx_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_)
        t.join();
}

void WaveformThreadPool::enqueue(StopToken token, Compute compute, OnResult onResult) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (stopping_) return;
        queue_.push({ std::move(compute), std::move(onResult), std::move(token), 0 });
    }
    catch (...) { return; }
    cv_.notify_one();
}

bool WaveformThreadPool::enqueueUnique(engine::AssetId assetId,
                                        Compute compute, OnResult onResult) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (stopping_) return false;
        if (pendingUnique_.count(assetId)) return false;
        pendingUnique_.insert(assetId);
        queue_.push({ std::move(compute), std::move(onResult), nullptr, assetId });
    }
    catch (...) { return false; }
    cv_.notify_one();
    return true;
}

void WaveformThreadPool::waitForIdle()
{
    std::unique_lock<std::mutex> lk(mx_);
    idle_.wait(lk, [this] { return stopping_ || (queue_.empty() && inFlight_ == 0); });
}

void WaveformThreadPool::workerLoop()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop();
            ++inFlight_;
        }

        // Check cancellation before IO
        bool cancelled = job.token && job.token->load(std::memory_order_relaxed);

        std::vector<float> envelope;
        if (!cancelled)
        {
            auto pcm = job.compute();
            // Check cancellation before compute
            cancelled = job.token && job.token->load(std::memory_order_relaxed);
            if (!cancelled && !pcm.empty())
                envelope = computeEnvelope(pcm);
        }

        // Check cancellation before delivering result
        if (!cancelled && job.onResult && !envelope.empty())
        {
            cancelled = job.token && job.token->load(std::memory_order_relaxed);
            if (!cancelled)
                job.onResult(std::move(envelope));
        }

        {
            std::lock_guard<std::mutex> lk(mx_);
            if (job.uniqueId != 0)
                pendingUnique_.erase(job.uniqueId);
            --inFlight_;
        }
        idle_.notify_all();
    }
}

std::vector<float> WaveformThreadPool::computeEnvelope(const std::vector<float>& pcm, int bins)
{
    if (pcm.empty() || bins <= 0) return {};
    std::vector<float> env(static_cast<std::size_t>(bins), 0.f);
    const int total = static_cast<int>(pcm.size());
    for (int b = 0; b < bins; ++b)
    {
        const int first = b * total / bins;
        const int last  = std::min(total, (b + 1) * total / bins);
        float peak = 0.f;
        for (int i = first; i < last; ++i)
            peak = std::max(peak, std::abs(pcm[static_cast<std::size_t>(i)]));
        env[static_cast<std::size_t>(b)] = peak;
    }
    return env;
}
