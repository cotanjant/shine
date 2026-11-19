#pragma once

#include "shine/core/result.hpp"
#include "shine/core/types.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace shine::core {

class IScreenRecorder {
public:
    virtual ~IScreenRecorder() = default;

    IScreenRecorder(const IScreenRecorder&) = delete;
    IScreenRecorder& operator=(const IScreenRecorder&) = delete;
    IScreenRecorder(IScreenRecorder&&) = delete;
    IScreenRecorder& operator=(IScreenRecorder&&) = delete;

    [[nodiscard]] virtual Result<void> start(const RecordingConfig& config) = 0;
    [[nodiscard]] virtual Result<RecordingStats> stop() = 0;
    [[nodiscard]] virtual RecordingStats stats() const = 0;
    [[nodiscard]] virtual bool is_recording() const noexcept = 0;

protected:
    IScreenRecorder() = default;
};

class IReplayRecorder {
public:
    virtual ~IReplayRecorder() = default;

    IReplayRecorder(const IReplayRecorder&) = delete;
    IReplayRecorder& operator=(const IReplayRecorder&) = delete;
    IReplayRecorder(IReplayRecorder&&) = delete;
    IReplayRecorder& operator=(IReplayRecorder&&) = delete;

    [[nodiscard]] virtual Result<void> start(const ReplayConfig& config) = 0;
    [[nodiscard]] virtual Result<void> stop() = 0;
    [[nodiscard]] virtual Result<std::filesystem::path> save_clip(
        const std::filesystem::path& outputPath,
        std::uint32_t seconds) = 0;
    [[nodiscard]] virtual RecordingStats stats() const = 0;
    [[nodiscard]] virtual bool is_running() const noexcept = 0;

protected:
    IReplayRecorder() = default;
};

[[nodiscard]] std::unique_ptr<IScreenRecorder> create_screen_recorder();
[[nodiscard]] std::unique_ptr<IReplayRecorder> create_replay_recorder();
[[nodiscard]] Result<std::vector<DisplayInfo>> enumerate_displays();
[[nodiscard]] std::filesystem::path default_recordings_dir();
[[nodiscard]] std::filesystem::path generate_recording_filename();
[[nodiscard]] double current_process_memory_mb() noexcept;

} // namespace shine::core
