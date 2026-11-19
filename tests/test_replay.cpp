#include "shine/core/clip_library.hpp"
#include "shine/core/mp4_validator.hpp"
#include "shine/core/recorder.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {

[[nodiscard]] bool is_github_actions() noexcept {
#ifdef _WIN32
    char buf[8]{};
    return GetEnvironmentVariableA("GITHUB_ACTIONS", buf, sizeof(buf)) > 0;
#else
    return false;
#endif
}

[[nodiscard]] bool verify_clip(const std::filesystem::path& path) {
    const auto report = shine::core::inspect_mp4(path);
    if (!report.valid || !report.hasFtyp || !report.hasMoov || !report.hasMdat) {
        std::cerr << "FAIL: invalid clip MP4 container (" << path.string()
                  << "): " << report.diagnostic << '\n';
        return false;
    }
    if (report.videoFrameCount < 5U) {
        std::cerr << "FAIL: clip has too few video frames (" << report.videoFrameCount << ")\n";
        return false;
    }
    if (report.durationSeconds < 0.3) {
        std::cerr << "FAIL: clip duration too short (" << report.durationSeconds << " s)\n";
        return false;
    }
    std::cout << "  Clip verified: " << path.filename().string()
              << " frames=" << report.videoFrameCount
              << " duration=" << report.durationSeconds << "s"
              << " audio=" << (report.hasAudioTrack ? report.audioCodecFourCC : "none")
              << '\n';
    return true;
}

} // namespace

int main() {
    const auto displaysRes = shine::core::enumerate_displays();
    if (!displaysRes) {
        std::cerr << "FAIL: enumerate_displays failed: " << displaysRes.error().message << '\n';
        return 1;
    }

    if (displaysRes.value().empty()) {
        std::cout << "Note: no active displays found in test environment; skipping live replay test.\n";
        return 0;
    }

    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() / "shine_test_replay";
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    auto replay = shine::core::create_replay_recorder();
    if (!replay) {
        std::cerr << "FAIL: create_replay_recorder returned null\n";
        return 1;
    }

    shine::core::ReplayConfig cfg{};
    cfg.displayIndex = 0;
    cfg.fps = 30;
    cfg.bitrateKbps = 4000;
    cfg.clipLengthSeconds = 5;
    cfg.captureCursor = true;
    cfg.preferGpu = true;
    cfg.voiceEnabled = true;
    cfg.voicePttEnabled = false;
    cfg.outputDirectory = tempDir;

    const auto startRes = replay->start(cfg);
    if (!startRes) {
        if (is_github_actions() && startRes.error().code == shine::core::ErrorCode::CaptureFailed) {
            std::cout << "Note: skipping live replay capture on headless Windows CI ("
                      << startRes.error().message << ")\n";
            std::filesystem::remove_all(tempDir, ec);
            return 0;
        }
        std::cerr << "FAIL: replay->start failed: " << startRes.error().message << '\n';
        std::filesystem::remove_all(tempDir, ec);
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    const auto midStats = replay->stats();
    std::cout << "Replay stats after warmup: frames=" << midStats.framesEncoded
              << " fps=" << midStats.currentFps
              << " mem=" << midStats.memoryUsageMb << " MB"
              << " gpu=" << (midStats.gpuAccelerated ? "yes" : "no")
              << " encoder=" << midStats.encoderName
              << " bufSec=" << midStats.replayBufferedSec
              << '\n';

    if (!midStats.replayActive) {
        std::cerr << "FAIL: replayActive is false while replay buffer is running\n";
        (void)replay->stop();
        std::filesystem::remove_all(tempDir, ec);
        return 1;
    }

    if (midStats.memoryUsageMb > 220.0) {
        std::cerr << "FAIL: memory usage exceeded 220 MB budget (" << midStats.memoryUsageMb << " MB)\n";
        (void)replay->stop();
        std::filesystem::remove_all(tempDir, ec);
        return 1;
    }

    const auto clip1Path = shine::core::generate_clip_filename(tempDir);
    const auto clip1Res = replay->save_clip(clip1Path, 5);
    if (!clip1Res) {
        std::cerr << "FAIL: first save_clip failed: " << clip1Res.error().message << '\n';
        (void)replay->stop();
        std::filesystem::remove_all(tempDir, ec);
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    const auto clip2Path = tempDir / "clip_second.mp4";
    const auto clip2Res = replay->save_clip(clip2Path, 3);
    if (!clip2Res) {
        std::cerr << "FAIL: second save_clip failed: " << clip2Res.error().message << '\n';
        (void)replay->stop();
        std::filesystem::remove_all(tempDir, ec);
        return 1;
    }

    const auto endStats = replay->stats();
    if (endStats.clipsSavedCount < 2U) {
        std::cerr << "FAIL: expected clipsSavedCount >= 2, got " << endStats.clipsSavedCount << '\n';
        (void)replay->stop();
        std::filesystem::remove_all(tempDir, ec);
        return 1;
    }

    const auto stopRes = replay->stop();
    if (!stopRes) {
        std::cerr << "FAIL: replay->stop failed: " << stopRes.error().message << '\n';
        std::filesystem::remove_all(tempDir, ec);
        return 1;
    }

    const bool ok1 = verify_clip(clip1Res.value());
    const bool ok2 = verify_clip(clip2Res.value());
    const auto scanned = shine::core::scan_clips(tempDir);

    std::filesystem::remove_all(tempDir, ec);

    if (!ok1 || !ok2) {
        return 1;
    }
    if (scanned.size() != 2U) {
        std::cerr << "FAIL: expected 2 scanned clips, found " << scanned.size() << '\n';
        return 1;
    }

    std::cout << "shine_test_replay: all checks passed.\n";
    return 0;
}
