#pragma once

#include "shine/core/recorder.hpp"
#include "shine/core/result.hpp"
#include "shine/ipc/ipc_transport.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace shine::daemon {

class DaemonApp {
public:
    DaemonApp(std::string endpoint, std::function<void()> onShutdownRequested);
    ~DaemonApp();

    DaemonApp(const DaemonApp&) = delete;
    DaemonApp& operator=(const DaemonApp&) = delete;

    [[nodiscard]] core::Result<void> start();
    void stop();

    [[nodiscard]] const std::string& endpoint() const noexcept;

private:
    [[nodiscard]] ipc::IpcResponse handle_request(const ipc::IpcRequest& req);
    [[nodiscard]] core::RecordingStats current_stats_locked() const;

    std::string endpoint_;
    std::function<void()> onShutdownRequested_;
    std::unique_ptr<ipc::IpcServer> server_;

    mutable std::mutex mutex_;
    std::unique_ptr<core::IScreenRecorder> recorder_;
    core::RecordingStats lastStats_{};
    std::atomic<bool> stopping_{false};
};

} // namespace shine::daemon
