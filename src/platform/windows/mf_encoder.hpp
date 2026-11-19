#pragma once

#include "platform/windows/d3d11_converter.hpp"
#include "platform/windows/dxgi_capture.hpp"
#include "shine/core/result.hpp"

#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace shine::platform::win {

struct PoolState;

class MfH264Encoder {
public:
    MfH264Encoder() = default;
    ~MfH264Encoder();

    MfH264Encoder(const MfH264Encoder&) = delete;
    MfH264Encoder& operator=(const MfH264Encoder&) = delete;
    MfH264Encoder(MfH264Encoder&&) = delete;
    MfH264Encoder& operator=(MfH264Encoder&&) = delete;

    [[nodiscard]] core::Result<void> init(
        const std::filesystem::path& outputPath,
        ID3D11Device* d3dDevice,
        D3D11BgraToNv12Converter* converter,
        std::uint32_t captureWidth,
        std::uint32_t captureHeight,
        std::uint32_t fps,
        std::uint32_t bitrateKbps,
        bool preferGpu);

    [[nodiscard]] core::Result<void> encode_gpu_frame(
        D3D11BgraToNv12Converter& converter,
        std::int64_t sampleTime100Ns,
        std::int64_t sampleDuration100Ns);

    [[nodiscard]] core::Result<void> encode_cpu_frame(
        const DxgiCapture& capture,
        std::int64_t sampleTime100Ns,
        std::int64_t sampleDuration100Ns);

    [[nodiscard]] core::Result<void> finalize();

    [[nodiscard]] bool is_gpu_accelerated() const noexcept { return gpuAccelerated_; }
    [[nodiscard]] bool uses_gpu_pipeline() const noexcept { return usingGpuPipeline_; }
    [[nodiscard]] const std::string& encoder_name() const noexcept { return encoderName_; }
    [[nodiscard]] std::uint64_t frames_written() const noexcept { return framesWritten_; }

private:
    [[nodiscard]] core::Result<void> init_gpu_pipeline(
        const std::filesystem::path& outputPath,
        ID3D11Device* d3dDevice,
        D3D11BgraToNv12Converter& converter);

    [[nodiscard]] core::Result<void> init_software_pipeline(
        const std::filesystem::path& outputPath,
        ID3D11Device* d3dDevice);

    [[nodiscard]] core::Result<void> configure_sink_writer(
        const std::filesystem::path& outputPath,
        bool enableHardware);

    std::uint32_t captureWidth_{0};
    std::uint32_t captureHeight_{0};
    std::uint32_t encodeWidth_{0};
    std::uint32_t encodeHeight_{0};
    std::uint32_t fps_{60};
    std::uint32_t bitrateKbps_{8000};

    bool usingGpuPipeline_{false};
    bool gpuAccelerated_{false};
    bool finalized_{false};
    std::uint64_t framesWritten_{0};
    DWORD videoStreamIndex_{0};
    std::string encoderName_;

    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    Microsoft::WRL::ComPtr<IMFSinkWriter> sinkWriter_;
    std::shared_ptr<PoolState> surfacePool_;

    // Software fallback reusable resources
    Microsoft::WRL::ComPtr<ID3D11Texture2D> swStagingTex_;
    std::vector<std::uint8_t> swNv12Buffer_;
};

} // namespace shine::platform::win
