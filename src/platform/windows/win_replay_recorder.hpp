#pragma once

#include "shine/core/recorder.hpp"
#include "shine/core/replay_buffer.hpp"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace shine::platform::win {

class WinReplayRecorder final : public core::IReplayRecorder {
public:
    WinReplayRecorder() = default;
    ~WinReplayRecorder() override;

    [[nodiscard]] core::Result<void> start(const core::ReplayConfig& config) override;
    [[nodiscard]] core::Result<void> stop() override;
    [[nodiscard]] core::Result<std::filesystem::path> save_clip(
        const std::filesystem::path& outputPath,
        std::uint32_t seconds) override;
    [[nodiscard]] core::RecordingStats stats() const override;
    [[nodiscard]] bool is_running() const noexcept override;

private:
    void worker_main(core::ReplayConfig config, std::promise<core::Result<void>> initPromise);

    mutable std::mutex statsMutex_;
    core::RecordingStats stats_{};

    std::mutex saveMutex_;
    core::ReplayRingBuffer ringBuffer_;
    std::uint32_t defaultClipSeconds_{30};
    std::filesystem::path outputDir_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread workerThread_;
};

} // namespace shine::platform::win
