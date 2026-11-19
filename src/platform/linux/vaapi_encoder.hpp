#pragma once

#include "platform/linux/linux_raii.hpp"
#include "shine/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace shine::platform::linux_impl {

class VaapiH264Encoder {
public:
    VaapiH264Encoder() = default;
    ~VaapiH264Encoder();

    VaapiH264Encoder(const VaapiH264Encoder&) = delete;
    VaapiH264Encoder& operator=(const VaapiH264Encoder&) = delete;

    [[nodiscard]] core::Result<void> init(
        const std::filesystem::path& outputPath,
        std::uint32_t captureWidth,
        std::uint32_t captureHeight,
        std::uint32_t fps,
        std::uint32_t bitrateKbps,
        bool preferGpu);

    [[nodiscard]] core::Result<void> encode_frame(
        const std::uint8_t* bgraData,
        std::uint32_t bgraStride,
        std::int64_t ptsIndex);

    [[nodiscard]] core::Result<void> finalize();

    [[nodiscard]] bool is_gpu_accelerated() const noexcept { return gpuAccelerated_; }
    [[nodiscard]] const std::string& encoder_name() const noexcept { return encoderName_; }
    [[nodiscard]] std::uint64_t frames_written() const noexcept { return framesWritten_; }

private:
    [[nodiscard]] core::Result<void> init_vaapi(
        const std::filesystem::path& outputPath,
        const std::string& drmNode,
        const std::string& vendor);

    [[nodiscard]] core::Result<void> init_software(const std::filesystem::path& outputPath);
    [[nodiscard]] core::Result<void> open_muxer(const std::filesystem::path& outputPath);
    [[nodiscard]] core::Result<void> drain_packets();

    std::uint32_t captureWidth_{0};
    std::uint32_t captureHeight_{0};
    std::uint32_t encodeWidth_{0};
    std::uint32_t encodeHeight_{0};
    std::uint32_t fps_{60};
    std::uint32_t bitrateKbps_{8000};

    bool gpuAccelerated_{false};
    bool finalized_{false};
    bool headerWritten_{false};
    std::uint64_t framesWritten_{0};
    std::string encoderName_;

    AvFormatOutPtr fmtCtx_;
    AVStream* videoStream_{nullptr};
    AvCodecCtxPtr codecCtx_;
    AvBufferRefPtr hwDeviceCtx_;
    AvFramePtr swFrame_;
    AvFramePtr hwFrame_;
    AvPacketPtr pkt_;
    SwsCtxPtr swsCtx_;
};

} // namespace shine::platform::linux_impl
