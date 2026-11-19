#include "shine/core/mp4_validator.hpp"
#include "shine/core/recorder.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

int main() {
    auto dispRes = shine::core::enumerate_displays();
    if (!dispRes || dispRes.value().empty()) {
        std::cout << "SKIP shine_test_recording: no attached display available ("
                  << (dispRes ? "0 displays" : dispRes.error().message) << ")\n";
        return 0;
    }

    const auto& displays = dispRes.value();
    std::uint32_t chosenDisplay = displays[0].index;
    for (const auto& d : displays) {
        std::cout << "Display[" << d.index << "]: " << d.name
                  << " " << d.width << "x" << d.height
                  << " @" << d.refreshHz << "Hz"
                  << (d.isPrimary ? " [primary]" : "") << '\n';
        if (d.isPrimary) {
            chosenDisplay = d.index;
        }
    }

    const auto tempDir = std::filesystem::temp_directory_path() / "shine_test_recording";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    const auto outMp4 = tempDir / "live_capture_2s.mp4";
    std::filesystem::remove(outMp4, ec);

    shine::core::RecordingConfig cfg{};
    cfg.outputPath = outMp4;
    cfg.displayIndex = chosenDisplay;
    cfg.fps = 60;
    cfg.bitrateKbps = 8000;
    cfg.captureCursor = true;
    cfg.preferGpu = true;

    auto recorder = shine::core::create_screen_recorder();
    auto startRes = recorder->start(cfg);
    if (!startRes) {
        std::cerr << "FAIL: start() failed: " << startRes.error().message << '\n';
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    auto stopRes = recorder->stop();
    if (!stopRes) {
        std::cerr << "FAIL: stop() failed: " << stopRes.error().message << '\n';
        return 1;
    }

    const auto& stats = stopRes.value();
    const auto report = shine::core::inspect_mp4(outMp4);

    std::cout << "Stats:\n"
              << "  state          : " << shine::core::to_string(stats.state) << '\n'
              << "  elapsedMs      : " << stats.elapsedMs << '\n'
              << "  framesEncoded  : " << stats.framesEncoded << '\n'
              << "  framesDropped  : " << stats.framesDropped << '\n'
              << "  currentFps     : " << stats.currentFps << '\n'
              << "  memoryUsageMb  : " << stats.memoryUsageMb << '\n'
              << "  gpuAccelerated : " << (stats.gpuAccelerated ? "true" : "false") << '\n'
              << "  gpuBackend     : " << shine::core::to_string(stats.gpuBackend) << '\n'
              << "  encoderName    : " << stats.encoderName << '\n'
              << "MP4 Report:\n"
              << "  valid          : " << (report.valid ? "true" : "false") << " (" << report.diagnostic << ")\n"
              << "  fileSizeBytes  : " << report.fileSizeBytes << '\n'
              << "  mdatBytes      : " << report.mdatPayloadBytes << '\n'
              << "  codec          : " << report.videoCodecFourCC << '\n'
              << "  videoFrames    : " << report.videoFrameCount << '\n'
              << "  durationSec    : " << report.durationSeconds << '\n';

    bool ok = true;
    if (stats.framesEncoded < 30) {
        std::cerr << "FAIL: expected >= 30 encoded frames, got " << stats.framesEncoded << '\n';
        ok = false;
    }
    if (stats.memoryUsageMb <= 0.0 || stats.memoryUsageMb >= 150.0) {
        std::cerr << "FAIL: memory usage out of expected range (< 150 MB): "
                  << stats.memoryUsageMb << " MB\n";
        ok = false;
    }
    if (!report.valid || !report.hasFtyp || !report.hasMoov || !report.hasMdat) {
        std::cerr << "FAIL: invalid MP4 output: " << report.diagnostic << '\n';
        ok = false;
    }
    if (report.videoFrameCount < 30) {
        std::cerr << "FAIL: MP4 stsz frame count too low: " << report.videoFrameCount << '\n';
        ok = false;
    }
    if (report.durationSeconds < 1.0) {
        std::cerr << "FAIL: MP4 duration too short: " << report.durationSeconds << "s\n";
        ok = false;
    }

    recorder.reset();
    std::filesystem::remove_all(tempDir, ec);

    return ok ? 0 : 1;
}
