#include "platform/windows/win_replay_recorder.hpp"

#include "platform/windows/d3d11_converter.hpp"
#include "platform/windows/dxgi_capture.hpp"
#include "platform/windows/mf_replay_encoder.hpp"
#include "platform/windows/win_audio.hpp"
#include "platform/windows/win_raii.hpp"
#include "shine/core/clip_library.hpp"

#include <algorithm>
#include <chrono>

namespace shine::platform::win {

WinReplayRecorder::~WinReplayRecorder() {
    (void)stop();
}

core::Result<void> WinReplayRecorder::start(const core::ReplayConfig& config) {
    auto validRes = config.validate();
    if (!validRes) {
        return validRes;
    }

    if (running_.load(std::memory_order_acquire)) {
        return core::Result<void>::make_err(
            core::ErrorCode::AlreadyRunning, "Replay buffer is already running");
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    defaultClipSeconds_ = config.clipLengthSeconds;
    outputDir_ = config.outputDirectory.empty()
                     ? core::default_recordings_dir()
                     : config.outputDirectory;

    ringBuffer_.clear();
    ringBuffer_.configure(config.clipLengthSeconds);

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_ = core::RecordingStats{};
        stats_.state = core::RecorderState::Idle;
        stats_.replayActive = true;
        stats_.memoryUsageMb = core::current_process_memory_mb();
    }

    stopRequested_.store(false, std::memory_order_release);
    std::promise<core::Result<void>> initPromise;
    auto initFuture = initPromise.get_future();

    workerThread_ = std::thread(&WinReplayRecorder::worker_main, this, config, std::move(initPromise));

    core::Result<void> initRes = initFuture.get();
    if (!initRes) {
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        running_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.replayActive = false;
        return initRes;
    }

    return core::Result<void>::make_ok();
}

core::Result<void> WinReplayRecorder::stop() {
    if (!running_.load(std::memory_order_acquire) && !workerThread_.joinable()) {
        return core::Result<void>::make_ok();
    }

    stopRequested_.store(true, std::memory_order_release);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    running_.store(false, std::memory_order_release);
    ringBuffer_.clear();

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.replayActive = false;
    stats_.replayBufferedSec = 0U;
    stats_.voiceActive = false;
    stats_.memoryUsageMb = core::current_process_memory_mb();
    return core::Result<void>::make_ok();
}

core::Result<std::filesystem::path> WinReplayRecorder::save_clip(
    const std::filesystem::path& outputPath,
    std::uint32_t seconds) {
    if (!running_.load(std::memory_order_acquire)) {
        return core::Result<std::filesystem::path>::make_err(
            core::ErrorCode::NotRunning, "Replay buffer is not running");
    }

    std::lock_guard<std::mutex> saveLock(saveMutex_);
    const std::uint32_t targetSec = seconds > 0U ? seconds : defaultClipSeconds_;
    const core::ClipSnapshot snap = ringBuffer_.snapshot_last(targetSec);
    if (snap.videoPackets.empty()) {
        return core::Result<std::filesystem::path>::make_err(
            core::ErrorCode::NotRunning, "Replay buffer does not have a complete GOP yet");
    }

    const std::filesystem::path dest =
        outputPath.empty() ? core::generate_clip_filename(outputDir_) : outputPath;

    ComScope comScope;
    if (!comScope.ok()) {
        return core::Result<std::filesystem::path>::make_err(
            core::ErrorCode::InternalError,
            "CoInitializeEx failed during clip save: " + hr_to_string(comScope.hr()));
    }
    MfScope mfScope;
    if (!mfScope.ok()) {
        return core::Result<std::filesystem::path>::make_err(
            core::ErrorCode::EncoderInitFailed,
            "MFStartup failed during clip save: " + hr_to_string(mfScope.hr()));
    }

    auto muxRes = save_clip_mp4_windows(snap, dest);
    if (!muxRes) {
        return core::Result<std::filesystem::path>::make_err(muxRes.error());
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.lastClipPath = dest;
        ++stats_.clipsSavedCount;
    }
    return core::Result<std::filesystem::path>::make_ok(dest);
}

core::RecordingStats WinReplayRecorder::stats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    core::RecordingStats copy = stats_;
    copy.replayBufferedSec = ringBuffer_.buffered_seconds();
    copy.memoryUsageMb = core::current_process_memory_mb();
    return copy;
}

bool WinReplayRecorder::is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void WinReplayRecorder::worker_main(
    core::ReplayConfig config,
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
            stats_.replayActive = false;
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
            stats_.replayActive = false;
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
            stats_.replayActive = false;
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

    ringBuffer_.set_video_format(encodeW, encodeH, config.fps, config.bitrateKbps, {});

    MfH264StreamEncoder encoder;
    auto encRes = encoder.init(
        capture.device(),
        converterPtr,
        capture.width(),
        capture.height(),
        config.fps,
        config.bitrateKbps,
        config.preferGpu,
        [this](core::EncodedVideoPacket pkt) {
            ringBuffer_.push_video(std::move(pkt));
        },
        [this, encodeW, encodeH, config](std::vector<std::uint8_t> cfg) {
            ringBuffer_.set_video_format(
                encodeW, encodeH, config.fps, config.bitrateKbps, std::move(cfg));
        });

    if (!encRes) {
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.replayActive = false;
            stats_.lastError = encRes.error().message;
        }
        initPromise.set_value(std::move(encRes));
        return;
    }

    const auto startTp = std::chrono::steady_clock::now();

    WasapiMicCapture micCapture;
    bool micRunning = false;
    if (config.voiceEnabled) {
        auto micRes = micCapture.start(
            [this](core::AudioPcmChunk chunk) {
                ringBuffer_.push_audio(std::move(chunk));
            },
            startTp,
            config.voicePttEnabled,
            config.voicePttHotkey);
        micRunning = micRes.ok();
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.replayActive = true;
        stats_.voiceActive = micRunning;
        stats_.gpuAccelerated = encoder.is_gpu_accelerated();
        stats_.gpuBackend = encoder.is_gpu_accelerated()
                                ? core::GpuBackend::WindowsD3D11MediaFoundation
                                : core::GpuBackend::None;
        stats_.encoderName = encoder.encoder_name();
        stats_.memoryUsageMb = core::current_process_memory_mb();
    }

    running_.store(true, std::memory_order_release);
    initPromise.set_value(core::Result<void>::make_ok());

    const auto frameInterval = std::chrono::nanoseconds(1'000'000'000LL / config.fps);
    const std::int64_t sampleDuration100Ns = std::max<std::int64_t>(1, 10'000'000LL / config.fps);

    auto nextTick = startTp;
    std::int64_t lastSampleTime100Ns = -1;
    int consecutiveAccessLost = 0;

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
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.lastError = "DXGI duplication lost repeatedly";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            (void)inputDesktop.attach();
            (void)capture.recreate_duplication();
            continue;
        }
        if (acq.status == AcquireStatus::Error) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.lastError = acq.errorMessage;
            break;
        }
        consecutiveAccessLost = 0;

        now = std::chrono::steady_clock::now();
        if (now < nextTick) {
            std::this_thread::sleep_until(nextTick);
            const AcquireResult catchup = capture.acquire_frame(0);
            if (catchup.status == AcquireStatus::Error) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.lastError = catchup.errorMessage;
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

        core::Result<void> subRes = core::Result<void>::make_ok();
        if (encoder.uses_gpu_pipeline()) {
            subRes = encoder.submit_gpu_frame(converter, sampleTime100Ns, sampleDuration100Ns);
        } else {
            subRes = encoder.submit_cpu_frame(capture, sampleTime100Ns, sampleDuration100Ns);
        }
        if (!subRes) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.lastError = subRes.error().message;
            break;
        }
        lastSampleTime100Ns = sampleTime100Ns;

        nextTick += frameInterval;
        if (nextTick + frameInterval * 4 < now) {
            nextTick = now + frameInterval;
        }

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.framesEncoded = encoder.frames_encoded();
            stats_.replayBufferedSec = ringBuffer_.buffered_seconds();
            stats_.memoryUsageMb = core::current_process_memory_mb();
        }
    }

    micCapture.stop();
    encoder.shutdown();
    running_.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.replayActive = false;
        stats_.voiceActive = false;
        stats_.replayBufferedSec = 0U;
        stats_.memoryUsageMb = core::current_process_memory_mb();
    }
}

} // namespace shine::platform::win

namespace shine::core {

std::unique_ptr<IReplayRecorder> create_replay_recorder() {
    return std::make_unique<platform::win::WinReplayRecorder>();
}

} // namespace shine::core
