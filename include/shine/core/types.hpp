#pragma once

#include "shine/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace shine::core {

enum class RecorderState {
    Idle,
    Starting,
    Recording,
    Stopping,
    Error
};

[[nodiscard]] inline constexpr std::string_view to_string(RecorderState state) noexcept {
    switch (state) {
    case RecorderState::Idle:
        return "idle";
    case RecorderState::Starting:
        return "starting";
    case RecorderState::Recording:
        return "recording";
    case RecorderState::Stopping:
        return "stopping";
    case RecorderState::Error:
        return "error";
    }
    return "error";
}

[[nodiscard]] inline std::optional<RecorderState> parse_recorder_state(std::string_view s) noexcept {
    if (s == "idle") {
        return RecorderState::Idle;
    }
    if (s == "starting") {
        return RecorderState::Starting;
    }
    if (s == "recording") {
        return RecorderState::Recording;
    }
    if (s == "stopping") {
        return RecorderState::Stopping;
    }
    if (s == "error") {
        return RecorderState::Error;
    }
    return std::nullopt;
}

enum class GpuBackend {
    None,
    WindowsD3D11MediaFoundation,
    LinuxVaApi
};

[[nodiscard]] inline constexpr std::string_view to_string(GpuBackend backend) noexcept {
    switch (backend) {
    case GpuBackend::None:
        return "none";
    case GpuBackend::WindowsD3D11MediaFoundation:
        return "d3d11_mf";
    case GpuBackend::LinuxVaApi:
        return "vaapi";
    }
    return "none";
}

[[nodiscard]] inline std::optional<GpuBackend> parse_gpu_backend(std::string_view s) noexcept {
    if (s == "none") {
        return GpuBackend::None;
    }
    if (s == "d3d11_mf") {
        return GpuBackend::WindowsD3D11MediaFoundation;
    }
    if (s == "vaapi") {
        return GpuBackend::LinuxVaApi;
    }
    return std::nullopt;
}

struct DisplayInfo {
    std::uint32_t index{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t refreshHz{};
    bool isPrimary{};
    std::string name;
};

struct RecordingConfig {
    std::filesystem::path outputPath;
    std::uint32_t displayIndex{0};
    std::uint32_t fps{60};
    std::uint32_t bitrateKbps{8000};
    bool captureCursor{true};
    bool preferGpu{true};

    [[nodiscard]] Result<void> validate() const {
        if (outputPath.empty()) {
            return Result<void>::make_err(ErrorCode::InvalidArgument, "Output path must not be empty");
        }
        if (fps < 1 || fps > 240) {
            return Result<void>::make_err(ErrorCode::InvalidArgument, "FPS must be between 1 and 240");
        }
        if (bitrateKbps < 500 || bitrateKbps > 100000) {
            return Result<void>::make_err(ErrorCode::InvalidArgument, "Bitrate must be between 500 and 100000 kbps");
        }
        return Result<void>::make_ok();
    }
};

struct RecordingStats {
    RecorderState state{RecorderState::Idle};
    std::uint64_t elapsedMs{0};
    std::uint64_t framesEncoded{0};
    std::uint64_t framesDropped{0};
    double currentFps{0.0};
    double memoryUsageMb{0.0};
    bool gpuAccelerated{false};
    GpuBackend gpuBackend{GpuBackend::None};
    std::string encoderName;
    std::filesystem::path outputPath;
    std::string lastError;
};

} // namespace shine::core
