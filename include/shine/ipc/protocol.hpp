#pragma once

#include "shine/core/result.hpp"
#include "shine/core/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shine::ipc {

inline constexpr std::size_t kMaxIpcMessageBytes = 16384;

enum class IpcCommand {
    Ping,
    Status,
    ListDisplays,
    Start,
    Stop,
    Shutdown,
    ReplayStart,
    ReplayStop,
    SaveClip,
    SetHotkey
};

struct IpcRequest {
    IpcCommand command{IpcCommand::Ping};
    std::optional<core::RecordingConfig> startConfig;
    std::optional<core::ReplayConfig> replayConfig;
    std::optional<std::string> hotkey;
    std::optional<std::filesystem::path> clipOutputPath;
    std::optional<std::uint32_t> clipSeconds;
};

struct IpcResponse {
    bool ok{false};
    core::ErrorCode errorCode{core::ErrorCode::Ok};
    std::string message;
    std::optional<core::RecordingStats> stats;
    std::vector<core::DisplayInfo> displays;

    [[nodiscard]] static IpcResponse make_error(core::ErrorCode code, std::string msg) {
        IpcResponse resp;
        resp.ok = false;
        resp.errorCode = code;
        resp.message = std::move(msg);
        return resp;
    }
};

[[nodiscard]] std::string encode_token(std::string_view input);
[[nodiscard]] core::Result<std::string> decode_token(std::string_view encoded);

[[nodiscard]] std::string serialize_request(const IpcRequest& req);
[[nodiscard]] core::Result<IpcRequest> parse_request(std::string_view line);

[[nodiscard]] std::string serialize_response(const IpcResponse& resp);
[[nodiscard]] core::Result<IpcResponse> parse_response(std::string_view line);

} // namespace shine::ipc
