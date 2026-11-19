#pragma once

#include "shine/core/recorder.hpp"
#include "shine/core/replay_buffer.hpp"

extern "C" {
#include <libavcodec/codec_id.h>
}

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace shine::platform::linux_impl {

class LinuxReplayRecorder final : public core::IReplayRecorder {
public:
    LinuxReplayRecorder() = default;
    ~LinuxReplayRecorder() override;

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
    AVCodecID videoCodecId_{AV_CODEC_ID_H264};
    std::uint32_t defaultClipSeconds_{30};
    std::filesystem::path outputDir_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread workerThread_;
};

} // namespace shine::platform::linux_impl
