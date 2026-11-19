#pragma once

#include "shine/core/replay_buffer.hpp"
#include "shine/core/result.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>

namespace shine::platform::linux_impl {

class AlsaMicCapture {
public:
    AlsaMicCapture() = default;
    ~AlsaMicCapture();

    AlsaMicCapture(const AlsaMicCapture&) = delete;
    AlsaMicCapture& operator=(const AlsaMicCapture&) = delete;

    [[nodiscard]] core::Result<void> start(
        std::function<void(core::AudioPcmChunk)> onChunk,
        std::chrono::steady_clock::time_point clockEpoch,
        bool pttEnabled,
        std::string pttKey);

    void stop();
    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    void worker_main(
        std::function<void(core::AudioPcmChunk)> onChunk,
        std::chrono::steady_clock::time_point clockEpoch,
        bool pttEnabled,
        std::string pttKey,
        std::promise<core::Result<void>> initPromise);

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace shine::platform::linux_impl
