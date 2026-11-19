#include "daemon/daemon_app.hpp"

#include "shine/core/clip_library.hpp"
#include "shine/core/hotkey.hpp"
#include "shine/core/settings.hpp"

#include <utility>

namespace shine::daemon {

DaemonApp::DaemonApp(
    std::string endpoint,
    std::function<void()> onShutdownRequested,
    bool loadPersistedSettings)
    : endpoint_(std::move(endpoint)),
      onShutdownRequested_(std::move(onShutdownRequested)),
      loadPersistedSettings_(loadPersistedSettings) {
    lastStats_.state = core::RecorderState::Idle;
    lastStats_.memoryUsageMb = core::current_process_memory_mb();
    clipOutputDir_ = core::default_recordings_dir();
}

DaemonApp::~DaemonApp() {
    stop();
}

core::Result<void> DaemonApp::start() {
    hotkeyService_ = service::create_global_hotkey_service();

    {
        std::lock_guard<std::mutex> lk(triggerMutex_);
        triggerPending_ = false;
        triggerStop_ = false;
    }
    clipTriggerThread_ = std::thread(&DaemonApp::clip_trigger_loop, this);

    server_ = ipc::create_ipc_server(endpoint_);
    auto srvRes = server_->start([this](const ipc::IpcRequest& req) {
        return handle_request(req);
    });
    if (!srvRes) {
        stop();
        return srvRes;
    }

    if (loadPersistedSettings_) {
        const core::AppSettings settings = core::load_settings();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeClipSeconds_ = settings.clipLengthSeconds;
            clipOutputDir_ = settings.outputDirectory;
        }
        if (settings.firstRunCompleted) {
            (void)apply_hotkey(settings.clipHotkey);
            if (settings.replayEnabled) {
                core::ReplayConfig rcfg{};
                rcfg.displayIndex = settings.displayIndex;
                rcfg.fps = settings.fps;
                rcfg.bitrateKbps = settings.bitrateKbps;
                rcfg.clipLengthSeconds = settings.clipLengthSeconds;
                rcfg.captureCursor = settings.captureCursor;
                rcfg.preferGpu = true;
                rcfg.voiceEnabled = settings.voiceEnabled;
                rcfg.voicePttEnabled = settings.voicePttEnabled;
                rcfg.voicePttHotkey = settings.voicePttHotkey;
                rcfg.outputDirectory = settings.outputDirectory;

                auto replay = core::create_replay_recorder();
                if (replay->start(rcfg)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    replayRecorder_ = std::move(replay);
                }
            }
        }
    }

    return core::Result<void>::make_ok();
}

void DaemonApp::stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (hotkeyService_) {
        hotkeyService_->unregister_hotkey();
    }

    {
        std::lock_guard<std::mutex> lk(triggerMutex_);
        triggerStop_ = true;
    }
    triggerCv_.notify_all();
    if (clipTriggerThread_.joinable()) {
        clipTriggerThread_.join();
    }

    if (server_) {
        server_->stop();
        server_.reset();
    }

    hotkeyService_.reset();

    std::unique_ptr<core::IScreenRecorder> recToStop;
    std::unique_ptr<core::IReplayRecorder> replayToStop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recToStop = std::move(recorder_);
        replayToStop = std::move(replayRecorder_);
    }

    if (recToStop) {
        if (recToStop->is_recording()) {
            auto stopRes = recToStop->stop();
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRes) {
                lastStats_ = stopRes.value();
            }
        }
        recToStop.reset();
    }

    if (replayToStop) {
        if (replayToStop->is_running()) {
            (void)replayToStop->stop();
        }
        replayToStop.reset();
    }
}

const std::string& DaemonApp::endpoint() const noexcept {
    return server_ ? server_->endpoint() : endpoint_;
}

void DaemonApp::clip_trigger_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lk(triggerMutex_);
            triggerCv_.wait(lk, [&]() {
                return triggerPending_ || triggerStop_;
            });
            if (triggerStop_) {
                break;
            }
            triggerPending_ = false;
        }
        (void)execute_save_clip({}, 0U);
    }
}

core::Result<void> DaemonApp::apply_hotkey(const std::string& hotkeyText) {
    if (!hotkeyService_) {
        return core::Result<void>::make_err(
            core::ErrorCode::NotSupported, "Hotkey service not initialized");
    }

    if (hotkeyText.empty()) {
        hotkeyService_->unregister_hotkey();
        std::lock_guard<std::mutex> lock(mutex_);
        activeClipHotkey_.clear();
        return core::Result<void>::make_ok();
    }

    const auto parsed = core::parse_hotkey(hotkeyText);
    if (!parsed || !parsed->valid()) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Invalid hotkey: " + hotkeyText);
    }

    auto regRes = hotkeyService_->register_hotkey(*parsed, [this]() {
        {
            std::lock_guard<std::mutex> lk(triggerMutex_);
            triggerPending_ = true;
        }
        triggerCv_.notify_one();
    });
    if (!regRes) {
        return regRes;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    activeClipHotkey_ = parsed->to_string();
    return core::Result<void>::make_ok();
}

core::Result<core::RecordingStats> DaemonApp::execute_save_clip(
    const std::filesystem::path& outputPath,
    std::uint32_t seconds) {
    std::lock_guard<std::mutex> saveLock(clipSaveMutex_);

    core::IReplayRecorder* replayPtr = nullptr;
    std::uint32_t defaultSec = 30U;
    std::filesystem::path outDir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        replayPtr = replayRecorder_.get();
        defaultSec = activeClipSeconds_;
        outDir = clipOutputDir_;
    }

    if (replayPtr == nullptr || !replayPtr->is_running()) {
        return core::Result<core::RecordingStats>::make_err(
            core::ErrorCode::NotRunning, "Replay buffer is not running");
    }

    const std::uint32_t targetSec = seconds > 0U ? seconds : defaultSec;
    const std::filesystem::path dest =
        outputPath.empty() ? core::generate_clip_filename(outDir) : outputPath;

    auto clipRes = replayPtr->save_clip(dest, targetSec);
    if (!clipRes) {
        return core::Result<core::RecordingStats>::make_err(clipRes.error());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    lastClipPath_ = clipRes.value();
    ++clipsSavedCount_;
    return core::Result<core::RecordingStats>::make_ok(current_stats_locked());
}

core::RecordingStats DaemonApp::current_stats_locked() const {
    core::RecordingStats copy = recorder_ ? recorder_->stats() : lastStats_;
    if (replayRecorder_ && replayRecorder_->is_running()) {
        const core::RecordingStats rstats = replayRecorder_->stats();
        copy.replayActive = true;
        copy.replayBufferedSec = rstats.replayBufferedSec;
        copy.voiceActive = rstats.voiceActive;
        if (!recorder_) {
            copy.gpuAccelerated = rstats.gpuAccelerated;
            copy.gpuBackend = rstats.gpuBackend;
            copy.encoderName = rstats.encoderName;
            copy.framesEncoded = rstats.framesEncoded;
        }
    } else {
        copy.replayActive = false;
        copy.replayBufferedSec = 0U;
        copy.voiceActive = false;
    }

    copy.activeClipHotkey = activeClipHotkey_;
    copy.lastClipPath = lastClipPath_;
    copy.clipsSavedCount = clipsSavedCount_;
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
        resp.stats = current_stats_locked();
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

        ipc::IpcResponse resp;
        resp.ok = true;
        resp.stats = current_stats_locked();
        return resp;
    }

    case ipc::IpcCommand::ReplayStart: {
        core::ReplayConfig rcfg = req.replayConfig.value_or(core::ReplayConfig{});
        if (rcfg.outputDirectory.empty()) {
            rcfg.outputDirectory = core::default_recordings_dir();
        }
        auto validRes = rcfg.validate();
        if (!validRes) {
            return ipc::IpcResponse::make_error(validRes.error().code, validRes.error().message);
        }

        std::unique_ptr<core::IReplayRecorder> oldReplay;
        {
            std::lock_guard<std::mutex> saveLock(clipSaveMutex_);
            std::lock_guard<std::mutex> lock(mutex_);
            oldReplay = std::move(replayRecorder_);
        }
        if (oldReplay) {
            (void)oldReplay->stop();
            oldReplay.reset();
        }

        auto nextReplay = core::create_replay_recorder();
        auto startRes = nextReplay->start(rcfg);
        if (!startRes) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastStats_.lastError = startRes.error().message;
            return ipc::IpcResponse::make_error(startRes.error().code, startRes.error().message);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        replayRecorder_ = std::move(nextReplay);
        activeClipSeconds_ = rcfg.clipLengthSeconds;
        clipOutputDir_ = rcfg.outputDirectory;

        ipc::IpcResponse resp;
        resp.ok = true;
        resp.stats = current_stats_locked();
        return resp;
    }

    case ipc::IpcCommand::ReplayStop: {
        std::unique_ptr<core::IReplayRecorder> oldReplay;
        {
            std::lock_guard<std::mutex> saveLock(clipSaveMutex_);
            std::lock_guard<std::mutex> lock(mutex_);
            oldReplay = std::move(replayRecorder_);
        }
        if (oldReplay) {
            (void)oldReplay->stop();
            oldReplay.reset();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        ipc::IpcResponse resp;
        resp.ok = true;
        resp.stats = current_stats_locked();
        return resp;
    }

    case ipc::IpcCommand::SaveClip: {
        auto saveRes = execute_save_clip(
            req.clipOutputPath.value_or(std::filesystem::path{}),
            req.clipSeconds.value_or(0U));
        if (!saveRes) {
            return ipc::IpcResponse::make_error(saveRes.error().code, saveRes.error().message);
        }
        ipc::IpcResponse resp;
        resp.ok = true;
        resp.stats = saveRes.take();
        return resp;
    }

    case ipc::IpcCommand::SetHotkey: {
        const std::string hk = req.hotkey.value_or(std::string{});
        auto hkRes = apply_hotkey(hk);
        if (!hkRes) {
            return ipc::IpcResponse::make_error(hkRes.error().code, hkRes.error().message);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ipc::IpcResponse resp;
        resp.ok = true;
        resp.stats = current_stats_locked();
        return resp;
    }

    case ipc::IpcCommand::Shutdown: {
        if (hotkeyService_) {
            hotkeyService_->unregister_hotkey();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (recorder_ && recorder_->is_recording()) {
                auto stopRes = recorder_->stop();
                if (stopRes) {
                    lastStats_ = stopRes.value();
                }
            }
            recorder_.reset();
            if (replayRecorder_ && replayRecorder_->is_running()) {
                (void)replayRecorder_->stop();
            }
            replayRecorder_.reset();
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
