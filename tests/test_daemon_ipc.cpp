#include "shine/core/mp4_validator.hpp"
#include "shine/ipc/ipc_transport.hpp"
#include "shine/service/autostart.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifndef SHINE_DAEMON_PATH
#error "SHINE_DAEMON_PATH must be defined by CMake"
#endif

int main() {
    const auto nowTicks = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const std::string endpoint = R"(\\.\pipe\shine_test_daemon_)" + std::to_string(nowTicks);
#else
    const std::string endpoint = "/tmp/shine_test_daemon_" + std::to_string(nowTicks) + ".sock";
#endif

    const std::filesystem::path daemonExe(SHINE_DAEMON_PATH);
    auto launchRes = shine::service::launch_daemon_detached(daemonExe, endpoint);
    if (!launchRes) {
        std::cerr << "FAIL: launch_daemon_detached failed: " << launchRes.error().message << '\n';
        return 1;
    }

    auto client = shine::ipc::create_ipc_client(endpoint);

    bool pingOk = false;
    for (int i = 0; i < 60; ++i) {
        shine::ipc::IpcRequest pingReq{};
        pingReq.command = shine::ipc::IpcCommand::Ping;
        auto res = client->send(pingReq, 300);
        if (res && res.value().ok && res.value().message == "pong") {
            pingOk = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!pingOk) {
        std::cerr << "FAIL: daemon did not respond to PING\n";
        return 1;
    }

    const auto send_checked = [&](const shine::ipc::IpcRequest& req, shine::ipc::IpcResponse& out) -> bool {
        auto res = client->send(req, 5000);
        if (!res) {
            std::cerr << "FAIL: IPC send error: " << res.error().message << '\n';
            return false;
        }
        if (!res.value().ok) {
            std::cerr << "FAIL: daemon returned error: " << res.value().message << '\n';
            return false;
        }
        out = std::move(res.value());
        return true;
    };

    bool allOk = true;

    shine::ipc::IpcResponse statusResp{};
    if (!send_checked({shine::ipc::IpcCommand::Status, std::nullopt}, statusResp) ||
        !statusResp.stats.has_value() ||
        statusResp.stats->state != shine::core::RecorderState::Idle ||
        statusResp.stats->memoryUsageMb >= 150.0) {
        std::cerr << "FAIL: initial STATUS check failed\n";
        allOk = false;
    } else {
        std::cout << "Daemon idle memory: " << statusResp.stats->memoryUsageMb << " MB\n";
    }

    shine::ipc::IpcRequest listReq{shine::ipc::IpcCommand::ListDisplays, std::nullopt};
    auto listRes = client->send(listReq, 5000);
    const bool hasDisplays = listRes && listRes.value().ok && !listRes.value().displays.empty();

    const auto tempDir = std::filesystem::temp_directory_path() / "shine_test_daemon_ipc";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    const auto outMp4 = tempDir / "daemon_capture.mp4";
    std::filesystem::remove(outMp4, ec);

    if (allOk && hasDisplays) {
        shine::core::RecordingConfig cfg{};
        cfg.outputPath = outMp4;
        cfg.displayIndex = listRes.value().displays[0].index;
        cfg.fps = 60;
        cfg.bitrateKbps = 8000;
        cfg.captureCursor = true;
        cfg.preferGpu = true;

        shine::ipc::IpcResponse startResp{};
        if (!send_checked({shine::ipc::IpcCommand::Start, cfg}, startResp)) {
            allOk = false;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));

            shine::ipc::IpcResponse midStatus{};
            if (!send_checked({shine::ipc::IpcCommand::Status, std::nullopt}, midStatus) ||
                !midStatus.stats.has_value() ||
                midStatus.stats->state != shine::core::RecorderState::Recording ||
                midStatus.stats->memoryUsageMb >= 150.0) {
                std::cerr << "FAIL: active recording STATUS check failed\n";
                allOk = false;
            } else {
                std::cout << "Daemon recording memory: " << midStatus.stats->memoryUsageMb
                          << " MB, gpu=" << (midStatus.stats->gpuAccelerated ? "true" : "false")
                          << ", encoder=" << midStatus.stats->encoderName << '\n';
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1100));

            shine::ipc::IpcResponse stopResp{};
            if (!send_checked({shine::ipc::IpcCommand::Stop, std::nullopt}, stopResp) ||
                !stopResp.stats.has_value()) {
                allOk = false;
            } else {
                std::cout << "Daemon stopped: frames=" << stopResp.stats->framesEncoded << '\n';
                if (stopResp.stats->framesEncoded < 20) {
                    std::cerr << "FAIL: daemon encoded too few frames\n";
                    allOk = false;
                }
                const auto report = shine::core::inspect_mp4(outMp4);
                if (!report.valid || report.videoFrameCount < 20) {
                    std::cerr << "FAIL: daemon MP4 invalid: " << report.diagnostic << '\n';
                    allOk = false;
                }
            }

            shine::ipc::IpcResponse afterStatus{};
            if (!send_checked({shine::ipc::IpcCommand::Status, std::nullopt}, afterStatus) ||
                !afterStatus.stats.has_value() ||
                afterStatus.stats->state != shine::core::RecorderState::Idle) {
                std::cerr << "FAIL: daemon did not return to idle after STOP\n";
                allOk = false;
            }
        }
    } else {
        std::cout << "Note: no display attached; verified daemon IPC lifecycle without video capture.\n";
    }

    shine::ipc::IpcResponse shutResp{};
    if (!send_checked({shine::ipc::IpcCommand::Shutdown, std::nullopt}, shutResp)) {
        allOk = false;
    }

    bool exited = false;
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto postPing = client->send({shine::ipc::IpcCommand::Ping, std::nullopt}, 150);
        if (!postPing) {
            exited = true;
            break;
        }
    }
    if (!exited) {
        std::cerr << "FAIL: daemon remained reachable after SHUTDOWN\n";
        allOk = false;
    }

    std::filesystem::remove_all(tempDir, ec);
    return allOk ? 0 : 1;
}
