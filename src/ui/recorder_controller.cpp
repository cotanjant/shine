#include "ui/recorder_controller.hpp"

#include "shine/core/recorder.hpp"
#include "shine/service/autostart.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace shine::ui {

namespace {

[[nodiscard]] QString format_elapsed(std::uint64_t ms) {
    const std::uint64_t totalSeconds = ms / 1000U;
    const std::uint64_t hours = totalSeconds / 3600U;
    const std::uint64_t mins = (totalSeconds % 3600U) / 60U;
    const std::uint64_t secs = totalSeconds % 60U;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(mins, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(mins, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QString format_gpu_badge(bool gpuAccelerated, core::GpuBackend backend) {
    if (!gpuAccelerated) {
        return QStringLiteral("CPU");
    }
    switch (backend) {
    case core::GpuBackend::WindowsD3D11MediaFoundation:
        return QStringLiteral("GPU · D3D11");
    case core::GpuBackend::LinuxVaApi:
        return QStringLiteral("GPU · VA-API");
    case core::GpuBackend::None:
        return QStringLiteral("GPU");
    }
    return QStringLiteral("GPU");
}

} // namespace

RecorderController::RecorderController(QObject* parent)
    : QObject(parent),
      client_(ipc::create_ipc_client(ipc::default_ipc_endpoint())) {
    const auto recDir = core::default_recordings_dir();
    outputDirectory_ = QString::fromStdString(recDir.string());
    autostartEnabled_ = service::is_autostart_installed();

    connect(&pollTimer_, &QTimer::timeout, this, &RecorderController::poll_status);
    pollTimer_.setInterval(2000);
    pollTimer_.start();

    (void)ensure_daemon_running();
    refreshDisplays();
    poll_status();
}

std::filesystem::path RecorderController::resolve_daemon_exe() const {
    const QString appDir = QCoreApplication::applicationDirPath();
#ifdef _WIN32
    const QString candidate = QDir(appDir).filePath(QStringLiteral("shine_daemon.exe"));
#else
    const QString candidate = QDir(appDir).filePath(QStringLiteral("shine_daemon"));
#endif
    return std::filesystem::path(candidate.toStdString());
}

bool RecorderController::ensure_daemon_running() {
    ipc::IpcRequest pingReq{};
    pingReq.command = ipc::IpcCommand::Ping;
    if (auto res = client_->send(pingReq, 400); res && res.value().ok) {
        if (!daemonConnected_) {
            daemonConnected_ = true;
            emit stateChanged();
        }
        return true;
    }

    const auto daemonPath = resolve_daemon_exe();
    auto launchRes = service::launch_daemon_detached(daemonPath);
    if (!launchRes) {
        lastError_ = QString::fromStdString(launchRes.error().message);
        daemonConnected_ = false;
        emit stateChanged();
        return false;
    }

    for (int attempt = 0; attempt < 30; ++attempt) {
        QThread::msleep(50);
        if (auto res = client_->send(pingReq, 300); res && res.value().ok) {
            daemonConnected_ = true;
            lastError_.clear();
            emit stateChanged();
            return true;
        }
    }

    daemonConnected_ = false;
    lastError_ = QStringLiteral("Background service did not respond");
    emit stateChanged();
    return false;
}

void RecorderController::setFps(int fps) {
    const int clamped = std::clamp(fps, 1, 240);
    if (fps_ != clamped) {
        fps_ = clamped;
        emit configChanged();
    }
}

void RecorderController::setBitrateKbps(int bitrateKbps) {
    const int clamped = std::clamp(bitrateKbps, 500, 100000);
    if (bitrateKbps_ != clamped) {
        bitrateKbps_ = clamped;
        emit configChanged();
    }
}

void RecorderController::setSelectedDisplay(int displayIndex) {
    const int clamped = std::max(0, displayIndex);
    if (selectedDisplay_ != clamped) {
        selectedDisplay_ = clamped;
        emit configChanged();
    }
}

void RecorderController::setCaptureCursor(bool enabled) {
    if (captureCursor_ != enabled) {
        captureCursor_ = enabled;
        emit configChanged();
    }
}

void RecorderController::toggleRecording() {
    if (busy_) {
        return;
    }

    busy_ = true;
    lastError_.clear();
    emit stateChanged();

    if (!ensure_daemon_running()) {
        busy_ = false;
        emit stateChanged();
        return;
    }

    if (!recording_) {
        core::RecordingConfig cfg{};
        cfg.outputPath = core::generate_recording_filename();
        cfg.displayIndex = static_cast<std::uint32_t>(selectedDisplay_);
        cfg.fps = static_cast<std::uint32_t>(fps_);
        cfg.bitrateKbps = static_cast<std::uint32_t>(bitrateKbps_);
        cfg.captureCursor = captureCursor_;
        cfg.preferGpu = true;

        ipc::IpcRequest req{};
        req.command = ipc::IpcCommand::Start;
        req.startConfig = std::move(cfg);

        auto res = client_->send(req, 5000);
        if (!res) {
            lastError_ = QString::fromStdString(res.error().message);
        } else if (!res.value().ok) {
            lastError_ = QString::fromStdString(res.value().message);
        } else if (res.value().stats.has_value()) {
            apply_stats(*res.value().stats);
        }
    } else {
        ipc::IpcRequest req{};
        req.command = ipc::IpcCommand::Stop;

        auto res = client_->send(req, 10000);
        if (!res) {
            lastError_ = QString::fromStdString(res.error().message);
        } else if (!res.value().ok) {
            lastError_ = QString::fromStdString(res.value().message);
        } else if (res.value().stats.has_value()) {
            apply_stats(*res.value().stats);
        }
    }

    busy_ = false;
    pollTimer_.setInterval(recording_ ? 250 : 2000);
    emit stateChanged();
}

void RecorderController::refreshDisplays() {
    if (!ensure_daemon_running()) {
        return;
    }

    ipc::IpcRequest req{};
    req.command = ipc::IpcCommand::ListDisplays;
    auto res = client_->send(req, 3000);
    if (res && res.value().ok) {
        const int count = static_cast<int>(res.value().displays.size());
        displayModel_.set_displays(res.value().displays);
        if (count > 0 && selectedDisplay_ >= count) {
            selectedDisplay_ = 0;
            emit configChanged();
        }
    }
}

void RecorderController::openRecordingsFolder() {
    const QString dirPath = outputDirectory_;
    QDir().mkpath(dirPath);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dirPath));
}

void RecorderController::openLastRecording() {
    if (!lastRecordingPath_.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(lastRecordingPath_));
    }
}

void RecorderController::setAutostart(bool enabled) {
    const core::Result<void> res = enabled
        ? service::install_autostart(resolve_daemon_exe())
        : service::uninstall_autostart();

    if (!res) {
        lastError_ = QString::fromStdString(res.error().message);
        emit stateChanged();
        return;
    }

    autostartEnabled_ = service::is_autostart_installed();
    emit configChanged();
}

void RecorderController::poll_status() {
    ipc::IpcRequest req{};
    req.command = ipc::IpcCommand::Status;

    auto res = client_->send(req, 1000);
    if (!res) {
        if (daemonConnected_) {
            daemonConnected_ = false;
            recording_ = false;
            statusText_ = QStringLiteral("Offline");
            emit stateChanged();
        }
        return;
    }

    daemonConnected_ = true;
    if (res.value().ok && res.value().stats.has_value()) {
        apply_stats(*res.value().stats);
    } else if (!res.value().ok) {
        lastError_ = QString::fromStdString(res.value().message);
        emit stateChanged();
    }
}

void RecorderController::apply_stats(const core::RecordingStats& stats) {
    const bool isRec = (stats.state == core::RecorderState::Recording ||
                        stats.state == core::RecorderState::Starting ||
                        stats.state == core::RecorderState::Stopping);
    const bool stateChangedFlag = (recording_ != isRec);
    recording_ = isRec;
    pollTimer_.setInterval(recording_ ? 250 : 2000);

    switch (stats.state) {
    case core::RecorderState::Idle:
        statusText_ = QStringLiteral("Ready");
        break;
    case core::RecorderState::Starting:
        statusText_ = QStringLiteral("Starting");
        break;
    case core::RecorderState::Recording:
        statusText_ = QStringLiteral("Recording");
        break;
    case core::RecorderState::Stopping:
        statusText_ = QStringLiteral("Saving");
        break;
    case core::RecorderState::Error:
        statusText_ = QStringLiteral("Error");
        break;
    }

    elapsedText_ = isRec ? format_elapsed(stats.elapsedMs) : QStringLiteral("00:00");
    gpuAccelerated_ = stats.gpuAccelerated;
    gpuBadgeText_ = format_gpu_badge(stats.gpuAccelerated, stats.gpuBackend);
    memoryText_ = QStringLiteral("%1 MB").arg(static_cast<int>(std::llround(stats.memoryUsageMb)));

    if (!stats.outputPath.empty() && !isRec) {
        lastRecordingPath_ = QString::fromStdString(stats.outputPath.string());
    }
    if (!stats.lastError.empty()) {
        lastError_ = QString::fromStdString(stats.lastError);
    }

    emit statsChanged();
    if (stateChangedFlag || !stats.lastError.empty()) {
        emit stateChanged();
    }
}

} // namespace shine::ui
