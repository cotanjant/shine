#include "platform/windows/win_recorder.hpp"

#include "platform/windows/d3d11_converter.hpp"
#include "platform/windows/dxgi_capture.hpp"
#include "platform/windows/mf_encoder.hpp"
#include "platform/windows/win_raii.hpp"

#include <psapi.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace shine::platform::win {

namespace {

struct CoTaskMemDeleter {
    void operator()(wchar_t* p) const noexcept {
        if (p != nullptr) {
            ::CoTaskMemFree(p);
        }
    }
};

} // namespace

WinRecorder::~WinRecorder() {
    if (workerThread_.joinable()) {
        stopRequested_.store(true, std::memory_order_release);
        workerThread_.join();
    }
    recording_.store(false, std::memory_order_release);
}

core::Result<void> WinRecorder::start(const core::RecordingConfig& config) {
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

    workerThread_ = std::thread(&WinRecorder::worker_main, this, config, std::move(initPromise));

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

core::Result<core::RecordingStats> WinRecorder::stop() {
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

core::RecordingStats WinRecorder::stats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    core::RecordingStats copy = stats_;
    copy.memoryUsageMb = core::current_process_memory_mb();
    return copy;
}

bool WinRecorder::is_recording() const noexcept {
    return recording_.load(std::memory_order_acquire);
}

void WinRecorder::worker_main(
    core::RecordingConfig config,
    std::promise<core::Result<void>> initPromise) {
    ScopedInputDesktop inputDesktop;
    (void)inputDesktop.attach();

    ComScope comScope;
    if (!comScope.ok()) {
        auto err = core::Result<void>::make_err(
            core::ErrorCode::InternalError,
            "CoInitializeEx failed: " + hr_to_string(comScope.hr()));
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.state = core::RecorderState::Error;
            stats_.lastError = err.error().message;
        }
        initPromise.set_value(std::move(err));
        return;
    }

    MfScope mfScope;
    if (!mfScope.ok()) {
        auto err = core::Result<void>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "MFStartup failed: " + hr_to_string(mfScope.hr()));
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.state = core::RecorderState::Error;
            stats_.lastError = err.error().message;
        }
        initPromise.set_value(std::move(err));
        return;
    }

    DxgiCapture capture;
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

    const std::uint32_t encodeW = capture.width() & ~1U;
    const std::uint32_t encodeH = capture.height() & ~1U;

    D3D11BgraToNv12Converter converter;
    D3D11BgraToNv12Converter* converterPtr = nullptr;
    if (config.preferGpu) {
        auto convRes = converter.init(
            capture.device(),
            capture.context(),
            capture.composite_texture(),
            capture.width(),
            capture.height(),
            encodeW,
            encodeH);
        if (convRes) {
            converterPtr = &converter;
        }
    }

    MfH264Encoder encoder;
    auto encRes = encoder.init(
        config.outputPath,
        capture.device(),
        converterPtr,
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
                                ? core::GpuBackend::WindowsD3D11MediaFoundation
                                : core::GpuBackend::None;
        stats_.encoderName = encoder.encoder_name();
        stats_.outputPath = config.outputPath;
        stats_.memoryUsageMb = core::current_process_memory_mb();
    }

    recording_.store(true, std::memory_order_release);
    initPromise.set_value(core::Result<void>::make_ok());

    const auto frameInterval = std::chrono::nanoseconds(1'000'000'000LL / config.fps);
    const std::int64_t sampleDuration100Ns = std::max<std::int64_t>(1, 10'000'000LL / config.fps);

    const auto startTp = std::chrono::steady_clock::now();
    auto nextTick = startTp;
    std::int64_t lastSampleTime100Ns = -1;
    std::uint64_t framesDropped = 0;
    int consecutiveAccessLost = 0;
    std::string fatalError;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        std::uint32_t waitMs = 0U;
        if (now < nextTick) {
            const auto remMs = std::chrono::duration_cast<std::chrono::milliseconds>(nextTick - now).count();
            waitMs = static_cast<std::uint32_t>(std::clamp<long long>(remMs, 1, 50));
        }

        const AcquireResult acq = capture.acquire_frame(waitMs);
        if (acq.status == AcquireStatus::AccessLost) {
            ++consecutiveAccessLost;
            if (consecutiveAccessLost > 10) {
                fatalError = "DXGI duplication lost repeatedly";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            (void)inputDesktop.attach();
            (void)capture.recreate_duplication();
            continue;
        }
        if (acq.status == AcquireStatus::Error) {
            fatalError = acq.errorMessage;
            break;
        }
        consecutiveAccessLost = 0;

        now = std::chrono::steady_clock::now();
        if (now < nextTick) {
            std::this_thread::sleep_until(nextTick);
            const AcquireResult catchup = capture.acquire_frame(0);
            if (catchup.status == AcquireStatus::Error) {
                fatalError = catchup.errorMessage;
                break;
            }
            now = std::chrono::steady_clock::now();
        }

        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        if (!capture.has_frame()) {
            nextTick += frameInterval;
            continue;
        }

        std::int64_t sampleTime100Ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - startTp).count() / 100LL;
        if (sampleTime100Ns <= lastSampleTime100Ns) {
            sampleTime100Ns = lastSampleTime100Ns + 1;
        }

        core::Result<void> writeRes = core::Result<void>::make_ok();
        if (encoder.uses_gpu_pipeline()) {
            writeRes = encoder.encode_gpu_frame(converter, sampleTime100Ns, sampleDuration100Ns);
        } else {
            writeRes = encoder.encode_cpu_frame(capture, sampleTime100Ns, sampleDuration100Ns);
        }

        if (!writeRes) {
            fatalError = writeRes.error().message;
            break;
        }
        lastSampleTime100Ns = sampleTime100Ns;

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

} // namespace shine::platform::win

namespace shine::core {

std::unique_ptr<IScreenRecorder> create_screen_recorder() {
    return std::make_unique<platform::win::WinRecorder>();
}

std::filesystem::path default_recordings_dir() {
    wchar_t* rawPath = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &rawPath)) &&
        rawPath != nullptr) {
        std::unique_ptr<wchar_t, platform::win::CoTaskMemDeleter> guard(rawPath);
        return std::filesystem::path(rawPath) / L"Shine";
    }

    wchar_t* userProfile = nullptr;
    std::size_t len = 0;
    if (_wdupenv_s(&userProfile, &len, L"USERPROFILE") == 0 && userProfile != nullptr) {
        std::unique_ptr<wchar_t, decltype(&std::free)> envGuard(userProfile, &std::free);
        return std::filesystem::path(userProfile) / L"Videos" / L"Shine";
    }

    return std::filesystem::current_path() / "Recordings";
}

std::filesystem::path generate_recording_filename() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmLocal{};
    localtime_s(&tmLocal, &tt);

    std::ostringstream oss;
    oss << "shine_" << std::put_time(&tmLocal, "%Y%m%d_%H%M%S") << ".mp4";
    return default_recordings_dir() / oss.str();
}

double current_process_memory_mb() noexcept {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
}

} // namespace shine::core
