#pragma once

#include "shine/core/recorder.hpp"
#include "shine/core/result.hpp"
#include "shine/ipc/ipc_transport.hpp"
#include "shine/service/hotkey_service.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace shine::daemon {

class DaemonApp {
public:
    DaemonApp(
        std::string endpoint,
        std::function<void()> onShutdownRequested,
        bool loadPersistedSettings = true);
    ~DaemonApp();

    DaemonApp(const DaemonApp&) = delete;
    DaemonApp& operator=(const DaemonApp&) = delete;

    [[nodiscard]] core::Result<void> start();
    void stop();

    [[nodiscard]] const std::string& endpoint() const noexcept;

private:
    [[nodiscard]] ipc::IpcResponse handle_request(const ipc::IpcRequest& req);
    [[nodiscard]] core::RecordingStats current_stats_locked() const;
    [[nodiscard]] core::Result<void> apply_hotkey(const std::string& hotkeyText);
    [[nodiscard]] core::Result<core::RecordingStats> execute_save_clip(
        const std::filesystem::path& outputPath,
        std::uint32_t seconds);
    void clip_trigger_loop();

    std::string endpoint_;
    std::function<void()> onShutdownRequested_;
    bool loadPersistedSettings_{true};

    std::unique_ptr<ipc::IpcServer> server_;
    std::unique_ptr<service::IGlobalHotkeyService> hotkeyService_;

    mutable std::mutex mutex_;
    std::unique_ptr<core::IScreenRecorder> recorder_;
    std::unique_ptr<core::IReplayRecorder> replayRecorder_;
    core::RecordingStats lastStats_{};
    std::string activeClipHotkey_;
    std::uint32_t activeClipSeconds_{30};
    std::filesystem::path clipOutputDir_;
    std::filesystem::path lastClipPath_;
    std::uint64_t clipsSavedCount_{0};

    std::mutex clipSaveMutex_;
    std::mutex triggerMutex_;
    std::condition_variable triggerCv_;
    bool triggerPending_{false};
    bool triggerStop_{false};
    std::thread clipTriggerThread_;

    std::atomic<bool> stopping_{false};
};

} // namespace shine::daemon
