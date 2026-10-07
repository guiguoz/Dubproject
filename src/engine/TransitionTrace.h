#pragma once
// TransitionTrace — instrumentation optionnelle, ring-buffer préallouée/lock-free.
// Audio thread (writer) et message thread (writer) poussent via fetch_add, sans
// printf/log/string/allocation. Consommation message-thread via drain().
#include <atomic>
#include <array>
#include <cstdint>

namespace engine {

struct TraceEntry {
    enum class Type : uint8_t {
        Prepared = 0, Ready, Armed, Keep, Morph, Leave, Enter, PcmFlip, GainRamp, Release, Done,
        Cancelled
    };
    Type    type = Type::Prepared;
    int64_t atSample = 0; // absolu
    int32_t fromScene = -1;
    int32_t toScene = -1;
    uint8_t slot = 255;
    float   a = 0.f, b = 0.f; // pour GainRamp etc.
};

class TransitionTrace {
public:
    static constexpr int kCapacity = 256;

    void push(const TraceEntry& e) noexcept {
        const int idx = head_.fetch_add(1, std::memory_order_relaxed) % kCapacity;
        buffer_[idx] = e;
        // مرئي للreader
        count_.fetch_add(1, std::memory_order_release);
    }

    // Drain tout le buffer dans out (message thread). Retourne le nombre d'entrées.
    int drain(TraceEntry* out, int maxOut) noexcept {
        int c = count_.load(std::memory_order_acquire);
        if (c <= 0) return 0;
        int n = (c > maxOut) ? maxOut : c;
        int start = (head_.load(std::memory_order_acquire) - c + kCapacity) % kCapacity;
        for (int i = 0; i < n; ++i) {
            out[i] = buffer_[(start + i) % kCapacity];
        }
        count_.fetch_sub(n, std::memory_order_release);
        return n;
    }

    void clear() noexcept {
        head_.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_release);
    }

private:
    std::array<TraceEntry, kCapacity> buffer_{};
    std::atomic<int> head_{0};
    std::atomic<int> count_{0};
};

} // namespace engine
