#include "platform/linux/linux_recorder.hpp"

#include "platform/linux/vaapi_encoder.hpp"
#include "platform/linux/x11_capture.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace shine::platform::linux_impl {

LinuxRecorder::~LinuxRecorder() {
    if (workerThread_.joinable()) {
        stopRequested_.store(true, std::memory_order_release);
        workerThread_.join();
    }
    recording_.store(false, std::memory_order_release);
}

core::Result<void> LinuxRecorder::start(const core::RecordingConfig& config) {
    auto validRes = config.validate();
    if (!validRes) {
        return validRes;
    }

    if (recording_.load(std::memory_order_acquire)) {
        return core::Result<void>::make_err(
            core::ErrorCode::AlreadyRunning, "Recorder is already running");
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_ = core::RecordingStats{};
        stats_.state = core::RecorderState::Starting;
        stats_.outputPath = config.outputPath;
        stats_.memoryUsageMb = core::current_process_memory_mb();
    }

    stopRequested_.store(false, std::memory_order_release);
    std::promise<core::Result<void>> initPromise;
    auto initFuture = initPromise.get_future();

    workerThread_ = std::thread(&LinuxRecorder::worker_main, this, config, std::move(initPromise));

    core::Result<void> initRes = initFuture.get();
    if (!initRes) {
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        recording_.store(false, std::memory_order_release);
        return initRes;
    }

    return core::Result<void>::make_ok();
}

core::Result<core::RecordingStats> LinuxRecorder::stop() {
    if (!recording_.load(std::memory_order_acquire) && !workerThread_.joinable()) {
        return core::Result<core::RecordingStats>::make_err(
            core::ErrorCode::NotRunning, "Recorder is not currently running");
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        if (stats_.state == core::RecorderState::Recording) {
            stats_.state = core::RecorderState::Stopping;
        }
    }

    stopRequested_.store(true, std::memory_order_release);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    recording_.store(false, std::memory_order_release);
    return core::Result<core::RecordingStats>::make_ok(stats());
}

core::RecordingStats LinuxRecorder::stats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    core::RecordingStats copy = stats_;
    copy.memoryUsageMb = core::current_process_memory_mb();
    return copy;
}

bool LinuxRecorder::is_recording() const noexcept {
    return recording_.load(std::memory_order_acquire);
}

void LinuxRecorder::worker_main(
    core::RecordingConfig config,
    std::promise<core::Result<void>> initPromise) {
    X11Capture capture;
    auto capRes = capture.init(config.displayIndex, config.captureCursor);
    if (!capRes) {
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.state = core::RecorderState::Error;
            stats_.lastError = capRes.error().message;
        }
        initPromise.set_value(std::move(capRes));
        return;
    }

    VaapiH264Encoder encoder;
    auto encRes = encoder.init(
        config.outputPath,
        capture.width(),
        capture.height(),
        config.fps,
        config.bitrateKbps,
        config.preferGpu);
    if (!encRes) {
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.state = core::RecorderState::Error;
            stats_.lastError = encRes.error().message;
        }
        initPromise.set_value(std::move(encRes));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.state = core::RecorderState::Recording;
        stats_.gpuAccelerated = encoder.is_gpu_accelerated();
        stats_.gpuBackend = encoder.is_gpu_accelerated()
                                ? core::GpuBackend::LinuxVaApi
                                : core::GpuBackend::None;
        stats_.encoderName = encoder.encoder_name();
        stats_.outputPath = config.outputPath;
        stats_.memoryUsageMb = core::current_process_memory_mb();
    }

    recording_.store(true, std::memory_order_release);
    initPromise.set_value(core::Result<void>::make_ok());

    const auto frameInterval = std::chrono::nanoseconds(1'000'000'000LL / config.fps);
    const auto startTp = std::chrono::steady_clock::now();
    auto nextTick = startTp;
    std::int64_t ptsIndex = 0;
    std::uint64_t framesDropped = 0;
    std::string fatalError;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        if (now < nextTick) {
            std::this_thread::sleep_until(nextTick);
            if (stopRequested_.load(std::memory_order_acquire)) {
                break;
            }
            now = std::chrono::steady_clock::now();
        }

        auto capFrame = capture.capture_frame();
        if (!capFrame) {
            fatalError = capFrame.error().message;
            break;
        }

        const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - startTp).count();
        std::int64_t targetPts = (elapsedNs * static_cast<std::int64_t>(config.fps)) / 1'000'000'000LL;
        if (targetPts < ptsIndex) {
            targetPts = ptsIndex;
        }

        auto encFrame = encoder.encode_frame(capture.bgra_data(), capture.stride_bytes(), targetPts);
        if (!encFrame) {
            fatalError = encFrame.error().message;
            break;
        }
        ptsIndex = targetPts + 1;

        nextTick += frameInterval;
        if (nextTick + frameInterval * 4 < now) {
            const auto missedNs = (now - nextTick).count();
            framesDropped += static_cast<std::uint64_t>(missedNs / frameInterval.count());
            nextTick = now + frameInterval;
        }

        const std::uint64_t elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - startTp).count());
        const std::uint64_t encoded = encoder.frames_written();
        const double currentFps =
            elapsedMs > 0 ? (static_cast<double>(encoded) * 1000.0 / static_cast<double>(elapsedMs)) : 0.0;

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.elapsedMs = elapsedMs;
            stats_.framesEncoded = encoded;
            stats_.framesDropped = framesDropped;
            stats_.currentFps = currentFps;
            stats_.memoryUsageMb = core::current_process_memory_mb();
        }
    }

    auto finRes = encoder.finalize();
    if (!finRes && fatalError.empty()) {
        fatalError = finRes.error().message;
    }

    const auto endTp = std::chrono::steady_clock::now();
    const std::uint64_t totalMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(endTp - startTp).count());
    const std::uint64_t totalFrames = encoder.frames_written();
    const double avgFps =
        totalMs > 0 ? (static_cast<double>(totalFrames) * 1000.0 / static_cast<double>(totalMs)) : 0.0;

    recording_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.elapsedMs = totalMs;
        stats_.framesEncoded = totalFrames;
        stats_.framesDropped = framesDropped;
        stats_.currentFps = avgFps;
        stats_.memoryUsageMb = core::current_process_memory_mb();
        if (!fatalError.empty()) {
            stats_.state = core::RecorderState::Error;
            stats_.lastError = fatalError;
        } else {
            stats_.state = core::RecorderState::Idle;
        }
    }
}

} // namespace shine::platform::linux_impl

namespace shine::core {

std::unique_ptr<IScreenRecorder> create_screen_recorder() {
    return std::make_unique<platform::linux_impl::LinuxRecorder>();
}

std::filesystem::path default_recordings_dir() {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / "Videos" / "Shine";
    }
    return std::filesystem::current_path() / "Recordings";
}

std::filesystem::path generate_recording_filename() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmLocal{};
    localtime_r(&tt, &tmLocal);

    std::ostringstream oss;
    oss << "shine_" << std::put_time(&tmLocal, "%Y%m%d_%H%M%S") << ".mp4";
    return default_recordings_dir() / oss.str();
}

double current_process_memory_mb() noexcept {
    std::ifstream statm("/proc/self/statm");
    unsigned long sizePages = 0;
    unsigned long rssPages = 0;
    if (statm >> sizePages >> rssPages) {
        const long pageSize = ::sysconf(_SC_PAGESIZE);
        if (pageSize > 0) {
            return static_cast<double>(rssPages) * static_cast<double>(pageSize) / (1024.0 * 1024.0);
        }
    }
    return 0.0;
}

} // namespace shine::core
