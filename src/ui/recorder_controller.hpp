#pragma once

#include "shine/core/types.hpp"
#include "shine/ipc/ipc_transport.hpp"
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
    [[nodiscard]] int fps() const noexcept { return fps_; }
    [[nodiscard]] int bitrateKbps() const noexcept { return bitrateKbps_; }
    [[nodiscard]] int selectedDisplay() const noexcept { return selectedDisplay_; }
    [[nodiscard]] bool captureCursor() const noexcept { return captureCursor_; }
    [[nodiscard]] bool autostartEnabled() const noexcept { return autostartEnabled_; }
    [[nodiscard]] QString outputDirectory() const { return outputDirectory_; }
    [[nodiscard]] QString lastRecordingPath() const { return lastRecordingPath_; }
    [[nodiscard]] QString lastError() const { return lastError_; }
    [[nodiscard]] DisplayModel* displays() noexcept { return &displayModel_; }

    void setFps(int fps);
    void setBitrateKbps(int bitrateKbps);
    void setSelectedDisplay(int displayIndex);
    void setCaptureCursor(bool enabled);

    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void refreshDisplays();
    Q_INVOKABLE void openRecordingsFolder();
    Q_INVOKABLE void openLastRecording();
    Q_INVOKABLE void setAutostart(bool enabled);

signals:
    void stateChanged();
    void statsChanged();
    void configChanged();

private:
    [[nodiscard]] bool ensure_daemon_running();
    [[nodiscard]] std::filesystem::path resolve_daemon_exe() const;
    void poll_status();
    void apply_stats(const core::RecordingStats& stats);

    std::unique_ptr<ipc::IpcClient> client_;
    DisplayModel displayModel_;
    QTimer pollTimer_;

    bool recording_{false};
    bool busy_{false};
    bool daemonConnected_{false};
    bool gpuAccelerated_{false};

    QString elapsedText_{QStringLiteral("00:00")};
    QString statusText_{QStringLiteral("Ready")};
    QString gpuBadgeText_{QStringLiteral("GPU")};
    QString memoryText_{QStringLiteral("-- MB")};
    QString outputDirectory_;
    QString lastRecordingPath_;
    QString lastError_;

    int fps_{60};
    int bitrateKbps_{8000};
    int selectedDisplay_{0};
    bool captureCursor_{true};
    bool autostartEnabled_{false};
};

} // namespace shine::ui
