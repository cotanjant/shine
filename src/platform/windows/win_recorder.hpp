#pragma once

#include "shine/core/recorder.hpp"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace shine::platform::win {

class WinRecorder final : public core::IScreenRecorder {
public:
    WinRecorder() = default;
    ~WinRecorder() override;

    [[nodiscard]] core::Result<void> start(const core::RecordingConfig& config) override;
    [[nodiscard]] core::Result<core::RecordingStats> stop() override;
    [[nodiscard]] core::RecordingStats stats() const override;
    [[nodiscard]] bool is_recording() const noexcept override;

private:
    void worker_main(core::RecordingConfig config, std::promise<core::Result<void>> initPromise);

    mutable std::mutex statsMutex_;
    core::RecordingStats stats_{};

    std::atomic<bool> recording_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread workerThread_;
};

} // namespace shine::platform::win
