#include "ui/recorder_controller.hpp"

#include "shine/core/clip_library.hpp"
#include "shine/core/hotkey.hpp"
#include "shine/core/recorder.hpp"
#include "shine/service/autostart.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
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

[[nodiscard]] QString path_to_qstring(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(u8.data()), static_cast<qsizetype>(u8.size()));
}

[[nodiscard]] std::filesystem::path qstring_to_path(const QString& s) {
    const QByteArray utf8 = s.toUtf8();
    std::u8string u8(reinterpret_cast<const char8_t*>(utf8.constData()), static_cast<std::size_t>(utf8.size()));
    return std::filesystem::path(u8);
}

} // namespace

RecorderController::RecorderController(QObject* parent)
    : QObject(parent),
      client_(ipc::create_ipc_client(ipc::default_ipc_endpoint())),
      settings_(core::load_settings()) {
    outputDirectory_ = path_to_qstring(settings_.outputDirectory);
    autostartEnabled_ = service::is_autostart_installed();
    clipModel_.reload(settings_.outputDirectory);

    connect(&pollTimer_, &QTimer::timeout, this, &RecorderController::poll_status);
    pollTimer_.setInterval(2000);
    pollTimer_.start();

    if (ensure_daemon_running()) {
        refreshDisplays();
        if (settings_.firstRunCompleted) {
            sync_daemon_replay_and_hotkey();
        }
        poll_status();
    }
}

std::filesystem::path RecorderController::resolve_daemon_exe() const {
    const QString appDir = QCoreApplication::applicationDirPath();
#ifdef _WIN32
    const QString candidate = QDir(appDir).filePath(QStringLiteral("shine_daemon.exe"));
#else
    const QString candidate = QDir(appDir).filePath(QStringLiteral("shine_daemon"));
#endif
    return qstring_to_path(candidate);
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

void RecorderController::persist_settings() {
    auto saveRes = core::save_settings(settings_);
    if (!saveRes) {
        lastError_ = QString::fromStdString(saveRes.error().message);
        emit stateChanged();
    }
}

void RecorderController::sync_daemon_replay_and_hotkey() {
    if (!ensure_daemon_running()) {
        return;
    }

    ipc::IpcRequest hkReq{};
    hkReq.command = ipc::IpcCommand::SetHotkey;
    hkReq.hotkey = settings_.replayEnabled ? settings_.clipHotkey : std::string{};
    if (auto hkRes = client_->send(hkReq, 2000); hkRes && !hkRes.value().ok) {
        lastError_ = QString::fromStdString(hkRes.value().message);
        emit stateChanged();
    }

    if (settings_.replayEnabled) {
        core::ReplayConfig rcfg{};
        rcfg.displayIndex = settings_.displayIndex;
        rcfg.fps = settings_.fps;
        rcfg.bitrateKbps = settings_.bitrateKbps;
        rcfg.clipLengthSeconds = settings_.clipLengthSeconds;
        rcfg.captureCursor = settings_.captureCursor;
        rcfg.preferGpu = true;
        rcfg.voiceEnabled = settings_.voiceEnabled;
        rcfg.voicePttEnabled = settings_.voicePttEnabled;
        rcfg.voicePttHotkey = settings_.voicePttHotkey;
        rcfg.outputDirectory = settings_.outputDirectory;

        ipc::IpcRequest repReq{};
        repReq.command = ipc::IpcCommand::ReplayStart;
        repReq.replayConfig = std::move(rcfg);
        auto repRes = client_->send(repReq, 5000);
        if (repRes && repRes.value().ok && repRes.value().stats.has_value()) {
            apply_stats(*repRes.value().stats);
        } else if (repRes && !repRes.value().ok) {
            lastError_ = QString::fromStdString(repRes.value().message);
            emit stateChanged();
        }
    } else {
        ipc::IpcRequest stopReq{};
        stopReq.command = ipc::IpcCommand::ReplayStop;
        auto stopRes = client_->send(stopReq, 3000);
        if (stopRes && stopRes.value().ok && stopRes.value().stats.has_value()) {
            apply_stats(*stopRes.value().stats);
        }
    }
}

void RecorderController::setFps(int fps) {
    const auto clamped = static_cast<std::uint32_t>(std::clamp(fps, 1, 240));
    if (settings_.fps != clamped) {
        settings_.fps = clamped;
        persist_settings();
        emit configChanged();
        if (settings_.firstRunCompleted && settings_.replayEnabled) {
            sync_daemon_replay_and_hotkey();
        }
    }
}

void RecorderController::setBitrateKbps(int bitrateKbps) {
    const auto clamped = static_cast<std::uint32_t>(std::clamp(bitrateKbps, 500, 100000));
    if (settings_.bitrateKbps != clamped) {
        settings_.bitrateKbps = clamped;
        persist_settings();
        emit configChanged();
        if (settings_.firstRunCompleted && settings_.replayEnabled) {
            sync_daemon_replay_and_hotkey();
        }
    }
}

void RecorderController::setSelectedDisplay(int displayIndex) {
    const auto clamped = static_cast<std::uint32_t>(std::max(0, displayIndex));
    if (settings_.displayIndex != clamped) {
        settings_.displayIndex = clamped;
        persist_settings();
        emit configChanged();
        if (settings_.firstRunCompleted && settings_.replayEnabled) {
            sync_daemon_replay_and_hotkey();
        }
    }
}

void RecorderController::setCaptureCursor(bool enabled) {
    if (settings_.captureCursor != enabled) {
        settings_.captureCursor = enabled;
        persist_settings();
        emit configChanged();
        if (settings_.firstRunCompleted && settings_.replayEnabled) {
            sync_daemon_replay_and_hotkey();
        }
    }
}

void RecorderController::setClipHotkey(const QString& hotkey) {
    const auto parsed = core::parse_hotkey(hotkey.toStdString());
    if (!parsed || !parsed->valid()) {
        return;
    }
    const std::string normalized = parsed->to_string();
    if (settings_.clipHotkey != normalized) {
        settings_.clipHotkey = normalized;
        persist_settings();
        emit configChanged();
        if (settings_.firstRunCompleted && settings_.replayEnabled && ensure_daemon_running()) {
            ipc::IpcRequest hkReq{};
            hkReq.command = ipc::IpcCommand::SetHotkey;
            hkReq.hotkey = settings_.clipHotkey;
            auto res = client_->send(hkReq, 2000);
            if (res && !res.value().ok) {
                lastError_ = QString::fromStdString(res.value().message);
                emit stateChanged();
            }
        }
    }
}

void RecorderController::setClipLengthSeconds(int seconds) {
    const auto clamped = static_cast<std::uint32_t>(std::clamp(seconds, 5, 300));
    if (settings_.clipLengthSeconds != clamped) {
        settings_.clipLengthSeconds = clamped;
        persist_settings();
        emit configChanged();
        if (settings_.firstRunCompleted && settings_.replayEnabled) {
            sync_daemon_replay_and_hotkey();
        }
    }
}

void RecorderController::setReplayEnabled(bool enabled) {
    if (settings_.replayEnabled != enabled) {
        settings_.replayEnabled = enabled;
        persist_settings();
        emit configChanged();
        if (settings_.firstRunCompleted) {
            sync_daemon_replay_and_hotkey();
        }
    }
}

void RecorderController::setVoiceEnabled(bool enabled) {
    if (settings_.voiceEnabled != enabled) {
        settings_.voiceEnabled = enabled;
        persist_settings();
        emit configChanged();
        if (settings_.firstRunCompleted && settings_.replayEnabled) {
            sync_daemon_replay_and_hotkey();
        }
    }
}

void RecorderController::setVoicePttEnabled(bool enabled) {
    if (settings_.voicePttEnabled != enabled) {
        settings_.voicePttEnabled = enabled;
        persist_settings();
        emit configChanged();
        if (settings_.firstRunCompleted && settings_.replayEnabled && settings_.voiceEnabled) {
            sync_daemon_replay_and_hotkey();
        }
    }
}

void RecorderController::setVoicePttHotkey(const QString& hotkey) {
    const auto parsed = core::parse_hotkey(hotkey.toStdString());
    if (!parsed || !parsed->valid()) {
        return;
    }
    const std::string normalized = parsed->to_string();
    if (settings_.voicePttHotkey != normalized) {
        settings_.voicePttHotkey = normalized;
        persist_settings();
        emit configChanged();
        if (settings_.firstRunCompleted && settings_.replayEnabled &&
            settings_.voiceEnabled && settings_.voicePttEnabled) {
            sync_daemon_replay_and_hotkey();
        }
    }
}

void RecorderController::completeFirstRun(
    const QString& hotkey,
    int clipLengthSeconds,
    bool voiceEnabled,
    bool pttEnabled,
    const QString& pttHotkey) {
    if (const auto parsed = core::parse_hotkey(hotkey.toStdString()); parsed && parsed->valid()) {
        settings_.clipHotkey = parsed->to_string();
    }
    if (const auto parsedPtt = core::parse_hotkey(pttHotkey.toStdString()); parsedPtt && parsedPtt->valid()) {
        settings_.voicePttHotkey = parsedPtt->to_string();
    }
    settings_.clipLengthSeconds = static_cast<std::uint32_t>(std::clamp(clipLengthSeconds, 5, 300));
    settings_.voiceEnabled = voiceEnabled;
    settings_.voicePttEnabled = pttEnabled;
    settings_.replayEnabled = true;
    settings_.firstRunCompleted = true;

    persist_settings();
    emit configChanged();
    sync_daemon_replay_and_hotkey();
}

void RecorderController::saveClipNow() {
    if (!ensure_daemon_running()) {
        return;
    }

    ipc::IpcRequest req{};
    req.command = ipc::IpcCommand::SaveClip;
    req.clipSeconds = settings_.clipLengthSeconds;

    auto res = client_->send(req, 10000);
    if (!res) {
        lastError_ = QString::fromStdString(res.error().message);
        emit stateChanged();
        return;
    }
    if (!res.value().ok) {
        lastError_ = QString::fromStdString(res.value().message);
        emit stateChanged();
        return;
    }

    lastError_.clear();
    if (res.value().stats.has_value()) {
        apply_stats(*res.value().stats);
    }
    refreshClips();
    emit clipSaved();
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
        cfg.displayIndex = settings_.displayIndex;
        cfg.fps = settings_.fps;
        cfg.bitrateKbps = settings_.bitrateKbps;
        cfg.captureCursor = settings_.captureCursor;
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
        refreshClips();
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
        const auto count = static_cast<std::uint32_t>(res.value().displays.size());
        displayModel_.set_displays(res.value().displays);
        if (count > 0U && settings_.displayIndex >= count) {
            settings_.displayIndex = 0U;
            persist_settings();
            emit configChanged();
        }
    }
}

void RecorderController::refreshClips() {
    clipModel_.reload(settings_.outputDirectory);
}

void RecorderController::openClip(const QString& filePath) {
    if (!filePath.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    }
}

void RecorderController::revealClip(const QString& filePath) {
    const QFileInfo fi(filePath);
    if (fi.exists()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
    } else {
        openRecordingsFolder();
    }
}

void RecorderController::deleteClip(const QString& filePath) {
    if (filePath.isEmpty()) {
        return;
    }
    auto res = core::delete_clip(qstring_to_path(filePath));
    if (!res) {
        lastError_ = QString::fromStdString(res.error().message);
        emit stateChanged();
        return;
    }
    refreshClips();
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
            replayActive_ = false;
            statusText_ = QStringLiteral("Offline");
            emit stateChanged();
            emit statsChanged();
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
    replayActive_ = stats.replayActive;
    replayBufferedSec_ = static_cast<int>(stats.replayBufferedSec);

    if (!stats.outputPath.empty() && !isRec) {
        lastRecordingPath_ = path_to_qstring(stats.outputPath);
    }
    if (!stats.lastClipPath.empty()) {
        lastClipPath_ = path_to_qstring(stats.lastClipPath);
    }
    if (stats.clipsSavedCount > clipsSavedCount_) {
        clipsSavedCount_ = stats.clipsSavedCount;
        refreshClips();
        emit clipSaved();
    } else {
        clipsSavedCount_ = stats.clipsSavedCount;
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
