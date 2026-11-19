#include "daemon/daemon_app.hpp"

#include <utility>

namespace shine::daemon {

DaemonApp::DaemonApp(std::string endpoint, std::function<void()> onShutdownRequested)
    : endpoint_(std::move(endpoint)),
      onShutdownRequested_(std::move(onShutdownRequested)) {
    lastStats_.state = core::RecorderState::Idle;
    lastStats_.memoryUsageMb = core::current_process_memory_mb();
}

DaemonApp::~DaemonApp() {
    stop();
}

core::Result<void> DaemonApp::start() {
    server_ = ipc::create_ipc_server(endpoint_);
    return server_->start([this](const ipc::IpcRequest& req) {
        return handle_request(req);
    });
}

void DaemonApp::stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (server_) {
        server_->stop();
        server_.reset();
    }

    std::unique_ptr<core::IScreenRecorder> toStop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        toStop = std::move(recorder_);
    }

    if (toStop) {
        if (toStop->is_recording()) {
            auto stopRes = toStop->stop();
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRes) {
                lastStats_ = stopRes.value();
            }
        }
        toStop.reset();
    }
}

const std::string& DaemonApp::endpoint() const noexcept {
    return server_ ? server_->endpoint() : endpoint_;
}

core::RecordingStats DaemonApp::current_stats_locked() const {
    if (recorder_) {
        return recorder_->stats();
    }
    core::RecordingStats copy = lastStats_;
    copy.memoryUsageMb = core::current_process_memory_mb();
    return copy;
}

ipc::IpcResponse DaemonApp::handle_request(const ipc::IpcRequest& req) {
    switch (req.command) {
    case ipc::IpcCommand::Ping: {
        ipc::IpcResponse resp;
        resp.ok = true;
        resp.message = "pong";
        return resp;
    }

    case ipc::IpcCommand::Status: {
        std::lock_guard<std::mutex> lock(mutex_);
        ipc::IpcResponse resp;
        resp.ok = true;
        resp.stats = current_stats_locked();
        return resp;
    }

    case ipc::IpcCommand::ListDisplays: {
        auto dispRes = core::enumerate_displays();
        if (!dispRes) {
            return ipc::IpcResponse::make_error(dispRes.error().code, dispRes.error().message);
        }
        ipc::IpcResponse resp;
        resp.ok = true;
        resp.message = "displays";
        resp.displays = dispRes.take();
        return resp;
    }

    case ipc::IpcCommand::Start: {
        if (!req.startConfig.has_value()) {
            return ipc::IpcResponse::make_error(
                core::ErrorCode::InvalidArgument, "START missing recording configuration");
        }

        core::RecordingConfig cfg = *req.startConfig;
        if (cfg.outputPath.empty()) {
            cfg.outputPath = core::generate_recording_filename();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (recorder_ && recorder_->is_recording()) {
            return ipc::IpcResponse::make_error(
                core::ErrorCode::AlreadyRunning, "A recording is already active");
        }

        auto newRecorder = core::create_screen_recorder();
        auto startRes = newRecorder->start(cfg);
        if (!startRes) {
            lastStats_.state = core::RecorderState::Error;
            lastStats_.lastError = startRes.error().message;
            return ipc::IpcResponse::make_error(startRes.error().code, startRes.error().message);
        }

        recorder_ = std::move(newRecorder);
        lastStats_ = recorder_->stats();

        ipc::IpcResponse resp;
        resp.ok = true;
        resp.stats = lastStats_;
        return resp;
    }

    case ipc::IpcCommand::Stop: {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recorder_ || !recorder_->is_recording()) {
            return ipc::IpcResponse::make_error(
                core::ErrorCode::NotRunning, "No active recording");
        }

        auto stopRes = recorder_->stop();
        recorder_.reset();

        if (!stopRes) {
            lastStats_.state = core::RecorderState::Error;
            lastStats_.lastError = stopRes.error().message;
            return ipc::IpcResponse::make_error(stopRes.error().code, stopRes.error().message);
        }

        lastStats_ = stopRes.value();
        lastStats_.memoryUsageMb = core::current_process_memory_mb();

        ipc::IpcResponse resp;
        resp.ok = true;
        resp.stats = lastStats_;
        return resp;
    }

    case ipc::IpcCommand::Shutdown: {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (recorder_ && recorder_->is_recording()) {
                auto stopRes = recorder_->stop();
                if (stopRes) {
                    lastStats_ = stopRes.value();
                }
            }
            recorder_.reset();
        }
        if (onShutdownRequested_) {
            onShutdownRequested_();
        }
        ipc::IpcResponse resp;
        resp.ok = true;
        resp.message = "shutting_down";
        return resp;
    }
    }

    return ipc::IpcResponse::make_error(core::ErrorCode::IpcError, "Unhandled command");
}

} // namespace shine::daemon
