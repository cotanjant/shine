#pragma once

#include "platform/windows/d3d11_converter.hpp"
#include "platform/windows/dxgi_capture.hpp"
#include "shine/core/replay_buffer.hpp"
#include "shine/core/result.hpp"

#include <d3d11.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace shine::platform::win {

struct PoolState;

class MfH264StreamEncoder {
public:
    MfH264StreamEncoder() = default;
    ~MfH264StreamEncoder();

    MfH264StreamEncoder(const MfH264StreamEncoder&) = delete;
    MfH264StreamEncoder& operator=(const MfH264StreamEncoder&) = delete;

    [[nodiscard]] core::Result<void> init(
        ID3D11Device* d3dDevice,
        D3D11BgraToNv12Converter* converter,
        std::uint32_t captureWidth,
        std::uint32_t captureHeight,
        std::uint32_t fps,
        std::uint32_t bitrateKbps,
        bool preferGpu,
        std::function<void(core::EncodedVideoPacket)> onPacket,
        std::function<void(std::vector<std::uint8_t>)> onCodecConfig);

    [[nodiscard]] core::Result<void> submit_gpu_frame(
        D3D11BgraToNv12Converter& converter,
        std::int64_t sampleTime100Ns,
        std::int64_t sampleDuration100Ns);

    [[nodiscard]] core::Result<void> submit_cpu_frame(
        const DxgiCapture& capture,
        std::int64_t sampleTime100Ns,
        std::int64_t sampleDuration100Ns);

    void shutdown();

    [[nodiscard]] bool is_gpu_accelerated() const noexcept { return gpuAccelerated_; }
    [[nodiscard]] bool uses_gpu_pipeline() const noexcept { return usingGpuPipeline_; }
    [[nodiscard]] const std::string& encoder_name() const noexcept { return encoderName_; }
    [[nodiscard]] std::uint64_t frames_encoded() const noexcept {
        return framesEncoded_.load(std::memory_order_acquire);
    }

private:
    [[nodiscard]] core::Result<void> configure_mft(
        ID3D11Device* d3dDevice,
        D3D11BgraToNv12Converter* converter,
        bool tryHardware);

    [[nodiscard]] core::Result<void> submit_sample(IMFSample* sample);
    void async_event_loop();
    void drain_sync_outputs();
    void drain_one_output();
    void maybe_capture_codec_config(const std::vector<std::uint8_t>& annexB);

    std::uint32_t captureWidth_{0};
    std::uint32_t captureHeight_{0};
    std::uint32_t encodeWidth_{0};
    std::uint32_t encodeHeight_{0};
    std::uint32_t fps_{60};
    std::uint32_t bitrateKbps_{8000};

    bool usingGpuPipeline_{false};
    bool gpuAccelerated_{false};
    bool isAsyncMft_{false};
    bool mftProvidesOutputSamples_{false};
    DWORD outputBufferSize_{0};
    std::string encoderName_;

    std::function<void(core::EncodedVideoPacket)> onPacket_;
    std::function<void(std::vector<std::uint8_t>)> onCodecConfig_;
    std::vector<std::uint8_t> codecConfig_;

    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> eventGen_;
    std::shared_ptr<PoolState> surfacePool_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> swStagingTex_;
    std::vector<std::uint8_t> swNv12Buffer_;

    std::mutex mftMutex_;
    std::mutex inputCvMutex_;
    std::condition_variable inputCv_;
    std::atomic<int> needInputCount_{0};
    std::atomic<bool> stopEventPump_{false};
    std::atomic<std::uint64_t> framesEncoded_{0};
    std::thread eventThread_;
};

[[nodiscard]] core::Result<void> save_clip_mp4_windows(
    const core::ClipSnapshot& snapshot,
    const std::filesystem::path& outputPath);

} // namespace shine::platform::win
