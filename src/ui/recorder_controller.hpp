#pragma once

#include "shine/core/settings.hpp"
#include "shine/core/types.hpp"
#include "shine/ipc/ipc_transport.hpp"
#include "ui/clip_model.hpp"
#include "ui/display_model.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <filesystem>
#include <memory>

namespace shine::ui {

class RecorderController final : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool daemonConnected READ daemonConnected NOTIFY stateChanged)
    Q_PROPERTY(QString elapsedText READ elapsedText NOTIFY statsChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool gpuAccelerated READ gpuAccelerated NOTIFY statsChanged)
    Q_PROPERTY(QString gpuBadgeText READ gpuBadgeText NOTIFY statsChanged)
    Q_PROPERTY(QString memoryText READ memoryText NOTIFY statsChanged)
    Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY configChanged)
    Q_PROPERTY(int bitrateKbps READ bitrateKbps WRITE setBitrateKbps NOTIFY configChanged)
    Q_PROPERTY(int selectedDisplay READ selectedDisplay WRITE setSelectedDisplay NOTIFY configChanged)
    Q_PROPERTY(bool captureCursor READ captureCursor WRITE setCaptureCursor NOTIFY configChanged)
    Q_PROPERTY(bool autostartEnabled READ autostartEnabled NOTIFY configChanged)
    Q_PROPERTY(QString outputDirectory READ outputDirectory NOTIFY configChanged)
    Q_PROPERTY(QString lastRecordingPath READ lastRecordingPath NOTIFY statsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
    Q_PROPERTY(DisplayModel* displays READ displays CONSTANT)

    Q_PROPERTY(bool firstRunCompleted READ firstRunCompleted NOTIFY configChanged)
    Q_PROPERTY(QString clipHotkey READ clipHotkey WRITE setClipHotkey NOTIFY configChanged)
    Q_PROPERTY(int clipLengthSeconds READ clipLengthSeconds WRITE setClipLengthSeconds NOTIFY configChanged)
    Q_PROPERTY(bool replayEnabled READ replayEnabled WRITE setReplayEnabled NOTIFY configChanged)
    Q_PROPERTY(bool replayActive READ replayActive NOTIFY statsChanged)
    Q_PROPERTY(int replayBufferedSec READ replayBufferedSec NOTIFY statsChanged)
    Q_PROPERTY(bool voiceEnabled READ voiceEnabled WRITE setVoiceEnabled NOTIFY configChanged)
    Q_PROPERTY(bool voicePttEnabled READ voicePttEnabled WRITE setVoicePttEnabled NOTIFY configChanged)
    Q_PROPERTY(QString voicePttHotkey READ voicePttHotkey WRITE setVoicePttHotkey NOTIFY configChanged)
    Q_PROPERTY(QString lastClipPath READ lastClipPath NOTIFY statsChanged)
    Q_PROPERTY(int clipsSavedCount READ clipsSavedCount NOTIFY statsChanged)
    Q_PROPERTY(ClipModel* clips READ clips CONSTANT)

public:
    explicit RecorderController(QObject* parent = nullptr);

    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool daemonConnected() const noexcept { return daemonConnected_; }
    [[nodiscard]] QString elapsedText() const { return elapsedText_; }
    [[nodiscard]] QString statusText() const { return statusText_; }
    [[nodiscard]] bool gpuAccelerated() const noexcept { return gpuAccelerated_; }
    [[nodiscard]] QString gpuBadgeText() const { return gpuBadgeText_; }
    [[nodiscard]] QString memoryText() const { return memoryText_; }
    [[nodiscard]] int fps() const noexcept { return static_cast<int>(settings_.fps); }
    [[nodiscard]] int bitrateKbps() const noexcept { return static_cast<int>(settings_.bitrateKbps); }
    [[nodiscard]] int selectedDisplay() const noexcept { return static_cast<int>(settings_.displayIndex); }
    [[nodiscard]] bool captureCursor() const noexcept { return settings_.captureCursor; }
    [[nodiscard]] bool autostartEnabled() const noexcept { return autostartEnabled_; }
    [[nodiscard]] QString outputDirectory() const { return outputDirectory_; }
    [[nodiscard]] QString lastRecordingPath() const { return lastRecordingPath_; }
    [[nodiscard]] QString lastError() const { return lastError_; }
    [[nodiscard]] DisplayModel* displays() noexcept { return &displayModel_; }

    [[nodiscard]] bool firstRunCompleted() const noexcept { return settings_.firstRunCompleted; }
    [[nodiscard]] QString clipHotkey() const { return QString::fromStdString(settings_.clipHotkey); }
    [[nodiscard]] int clipLengthSeconds() const noexcept { return static_cast<int>(settings_.clipLengthSeconds); }
    [[nodiscard]] bool replayEnabled() const noexcept { return settings_.replayEnabled; }
    [[nodiscard]] bool replayActive() const noexcept { return replayActive_; }
    [[nodiscard]] int replayBufferedSec() const noexcept { return replayBufferedSec_; }
    [[nodiscard]] bool voiceEnabled() const noexcept { return settings_.voiceEnabled; }
    [[nodiscard]] bool voicePttEnabled() const noexcept { return settings_.voicePttEnabled; }
    [[nodiscard]] QString voicePttHotkey() const { return QString::fromStdString(settings_.voicePttHotkey); }
    [[nodiscard]] QString lastClipPath() const { return lastClipPath_; }
    [[nodiscard]] int clipsSavedCount() const noexcept { return static_cast<int>(clipsSavedCount_); }
    [[nodiscard]] ClipModel* clips() noexcept { return &clipModel_; }

    void setFps(int fps);
    void setBitrateKbps(int bitrateKbps);
    void setSelectedDisplay(int displayIndex);
    void setCaptureCursor(bool enabled);

    void setClipHotkey(const QString& hotkey);
    void setClipLengthSeconds(int seconds);
    void setReplayEnabled(bool enabled);
    void setVoiceEnabled(bool enabled);
    void setVoicePttEnabled(bool enabled);
    void setVoicePttHotkey(const QString& hotkey);

    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void refreshDisplays();
    Q_INVOKABLE void openRecordingsFolder();
    Q_INVOKABLE void openLastRecording();
    Q_INVOKABLE void setAutostart(bool enabled);

    Q_INVOKABLE void completeFirstRun(
        const QString& hotkey,
        int clipLengthSeconds,
        bool voiceEnabled,
        bool pttEnabled,
        const QString& pttHotkey);
    Q_INVOKABLE void saveClipNow();
    Q_INVOKABLE void refreshClips();
    Q_INVOKABLE void openClip(const QString& filePath);
    Q_INVOKABLE void revealClip(const QString& filePath);
    Q_INVOKABLE void deleteClip(const QString& filePath);

signals:
    void stateChanged();
    void statsChanged();
    void configChanged();
    void clipSaved();

private:
    [[nodiscard]] bool ensure_daemon_running();
    [[nodiscard]] std::filesystem::path resolve_daemon_exe() const;
    void persist_settings();
    void sync_daemon_replay_and_hotkey();
    void poll_status();
    void apply_stats(const core::RecordingStats& stats);

    std::unique_ptr<ipc::IpcClient> client_;
    DisplayModel displayModel_;
    ClipModel clipModel_;
    QTimer pollTimer_;
    core::AppSettings settings_{};

    bool recording_{false};
    bool busy_{false};
    bool daemonConnected_{false};
    bool gpuAccelerated_{false};
    bool replayActive_{false};
    int replayBufferedSec_{0};

    QString elapsedText_{QStringLiteral("00:00")};
    QString statusText_{QStringLiteral("Ready")};
    QString gpuBadgeText_{QStringLiteral("GPU")};
    QString memoryText_{QStringLiteral("-- MB")};
    QString outputDirectory_;
    QString lastRecordingPath_;
    QString lastClipPath_;
    QString lastError_;

    std::uint64_t clipsSavedCount_{0};
    bool autostartEnabled_{false};
};

} // namespace shine::ui
