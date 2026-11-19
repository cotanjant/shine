#include "shine/ipc/protocol.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <unordered_map>

namespace shine::ipc {

namespace {

[[nodiscard]] bool needs_encoding(unsigned char c) noexcept {
    if (c < 0x20U || c >= 0x7FU) {
        return true;
    }
    switch (c) {
    case '%':
    case ' ':
    case '=':
    case ',':
    case ';':
    case '|':
        return true;
    default:
        return false;
    }
}

[[nodiscard]] int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

[[nodiscard]] std::filesystem::path utf8_to_path(std::string_view s) {
    std::u8string u8(reinterpret_cast<const char8_t*>(s.data()), s.size());
    return std::filesystem::path(u8);
}

[[nodiscard]] std::string strip_line_ending(std::string_view line) noexcept {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return std::string(line);
}

[[nodiscard]] std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        if (i >= s.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            ++i;
        }
        tokens.push_back(s.substr(start, i - start));
    }
    return tokens;
}

[[nodiscard]] core::Result<std::unordered_map<std::string_view, std::string_view>>
parse_kv_tokens(const std::vector<std::string_view>& tokens, std::size_t startIdx) {
    std::unordered_map<std::string_view, std::string_view> kv;
    for (std::size_t i = startIdx; i < tokens.size(); ++i) {
        const std::size_t eq = tokens[i].find('=');
        if (eq == std::string_view::npos || eq == 0) {
            return core::Result<std::unordered_map<std::string_view, std::string_view>>::make_err(
                core::ErrorCode::IpcError, "Malformed key=value token in IPC message");
        }
        kv[tokens[i].substr(0, eq)] = tokens[i].substr(eq + 1);
    }
    return core::Result<std::unordered_map<std::string_view, std::string_view>>::make_ok(std::move(kv));
}

template <typename IntT>
[[nodiscard]] core::Result<IntT> parse_int(std::string_view sv) {
    if (sv.empty()) {
        return core::Result<IntT>::make_err(core::ErrorCode::IpcError, "Empty integer field");
    }
    IntT val{};
    const char* first = sv.data();
    const char* last = sv.data() + sv.size();
    const auto [ptr, ec] = std::from_chars(first, last, val);
    if (ec != std::errc{} || ptr != last) {
        return core::Result<IntT>::make_err(core::ErrorCode::IpcError, "Invalid integer field");
    }
    return core::Result<IntT>::make_ok(val);
}

[[nodiscard]] core::Result<double> parse_double(std::string_view sv) {
    if (sv.empty()) {
        return core::Result<double>::make_err(core::ErrorCode::IpcError, "Empty floating-point field");
    }
    double val = 0.0;
    const char* first = sv.data();
    const char* last = sv.data() + sv.size();
    const auto [ptr, ec] = std::from_chars(first, last, val);
    if (ec != std::errc{} || ptr != last) {
        return core::Result<double>::make_err(core::ErrorCode::IpcError, "Invalid floating-point field");
    }
    return core::Result<double>::make_ok(val);
}

[[nodiscard]] std::string format_double(double v) {
    std::array<char, 64> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%.3f", v);
    if (n <= 0) {
        return "0.000";
    }
    return std::string(buf.data(), static_cast<std::size_t>(n));
}

[[nodiscard]] std::vector<std::string_view> split_char(std::string_view sv, char delim) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = sv.find(delim, start);
        if (pos == std::string_view::npos) {
            parts.push_back(sv.substr(start));
            break;
        }
        parts.push_back(sv.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

} // namespace

std::string encode_token(std::string_view input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() + 8);
    for (const char ch : input) {
        const auto uch = static_cast<unsigned char>(ch);
        if (needs_encoding(uch)) {
            out.push_back('%');
            out.push_back(kHex[(uch >> 4U) & 0x0FU]);
            out.push_back(kHex[uch & 0x0FU]);
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

core::Result<std::string> decode_token(std::string_view encoded) {
    std::string out;
    out.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%') {
            if (i + 2 >= encoded.size()) {
                return core::Result<std::string>::make_err(
                    core::ErrorCode::IpcError, "Truncated percent-escape in IPC token");
            }
            const int hi = hex_digit(encoded[i + 1]);
            const int lo = hex_digit(encoded[i + 2]);
            if (hi < 0 || lo < 0) {
                return core::Result<std::string>::make_err(
                    core::ErrorCode::IpcError, "Invalid hex digit in IPC token");
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(encoded[i]);
        }
    }
    return core::Result<std::string>::make_ok(std::move(out));
}

std::string serialize_request(const IpcRequest& req) {
    std::ostringstream oss;
    switch (req.command) {
    case IpcCommand::Ping:
        oss << "PING";
        break;
    case IpcCommand::Status:
        oss << "STATUS";
        break;
    case IpcCommand::ListDisplays:
        oss << "LIST_DISPLAYS";
        break;
    case IpcCommand::Start: {
        const core::RecordingConfig cfg = req.startConfig.value_or(core::RecordingConfig{});
        oss << "START"
            << " output=" << encode_token(path_to_utf8(cfg.outputPath))
            << " display=" << cfg.displayIndex
            << " fps=" << cfg.fps
            << " bitrate=" << cfg.bitrateKbps
            << " cursor=" << (cfg.captureCursor ? 1 : 0)
            << " gpu=" << (cfg.preferGpu ? 1 : 0);
        break;
    }
    case IpcCommand::Stop:
        oss << "STOP";
        break;
    case IpcCommand::Shutdown:
        oss << "SHUTDOWN";
        break;
    }
    oss << '\n';
    return oss.str();
}

core::Result<IpcRequest> parse_request(std::string_view line) {
    const std::string clean = strip_line_ending(line);
    if (clean.empty() || clean.size() > kMaxIpcMessageBytes) {
        return core::Result<IpcRequest>::make_err(
            core::ErrorCode::IpcError, "Empty or oversized IPC request");
    }

    const auto tokens = split_ws(clean);
    if (tokens.empty()) {
        return core::Result<IpcRequest>::make_err(
            core::ErrorCode::IpcError, "Empty IPC request");
    }

    IpcRequest req{};
    const std::string_view cmd = tokens[0];
    if (cmd == "PING") {
        req.command = IpcCommand::Ping;
        return core::Result<IpcRequest>::make_ok(std::move(req));
    }
    if (cmd == "STATUS") {
        req.command = IpcCommand::Status;
        return core::Result<IpcRequest>::make_ok(std::move(req));
    }
    if (cmd == "LIST_DISPLAYS") {
        req.command = IpcCommand::ListDisplays;
        return core::Result<IpcRequest>::make_ok(std::move(req));
    }
    if (cmd == "STOP") {
        req.command = IpcCommand::Stop;
        return core::Result<IpcRequest>::make_ok(std::move(req));
    }
    if (cmd == "SHUTDOWN") {
        req.command = IpcCommand::Shutdown;
        return core::Result<IpcRequest>::make_ok(std::move(req));
    }
    if (cmd == "START") {
        auto kvRes = parse_kv_tokens(tokens, 1);
        if (!kvRes) {
            return core::Result<IpcRequest>::make_err(kvRes.error());
        }
        const auto& kv = kvRes.value();

        core::RecordingConfig cfg{};
        auto itOut = kv.find("output");
        if (itOut == kv.end()) {
            return core::Result<IpcRequest>::make_err(
                core::ErrorCode::InvalidArgument, "START missing output");
        }
        auto outDec = decode_token(itOut->second);
        if (!outDec) {
            return core::Result<IpcRequest>::make_err(outDec.error());
        }
        cfg.outputPath = utf8_to_path(outDec.value());

        if (auto it = kv.find("display"); it != kv.end()) {
            auto r = parse_int<std::uint32_t>(it->second);
            if (!r) {
                return core::Result<IpcRequest>::make_err(r.error());
            }
            cfg.displayIndex = r.value();
        }
        if (auto it = kv.find("fps"); it != kv.end()) {
            auto r = parse_int<std::uint32_t>(it->second);
            if (!r) {
                return core::Result<IpcRequest>::make_err(r.error());
            }
            cfg.fps = r.value();
        }
        if (auto it = kv.find("bitrate"); it != kv.end()) {
            auto r = parse_int<std::uint32_t>(it->second);
            if (!r) {
                return core::Result<IpcRequest>::make_err(r.error());
            }
            cfg.bitrateKbps = r.value();
        }
        if (auto it = kv.find("cursor"); it != kv.end()) {
            cfg.captureCursor = (it->second != "0");
        }
        if (auto it = kv.find("gpu"); it != kv.end()) {
            cfg.preferGpu = (it->second != "0");
        }

        req.command = IpcCommand::Start;
        req.startConfig = std::move(cfg);
        return core::Result<IpcRequest>::make_ok(std::move(req));
    }

    return core::Result<IpcRequest>::make_err(
        core::ErrorCode::IpcError, "Unknown IPC command");
}

std::string serialize_response(const IpcResponse& resp) {
    std::ostringstream oss;
    if (!resp.ok) {
        oss << "ERR code=" << static_cast<int>(resp.errorCode)
            << " msg=" << encode_token(resp.message)
            << '\n';
        return oss.str();
    }

    oss << "OK";
    if (!resp.displays.empty() || (!resp.stats.has_value() && resp.message == "displays")) {
        oss << " kind=displays count=" << resp.displays.size();
        for (std::size_t i = 0; i < resp.displays.size(); ++i) {
            const auto& d = resp.displays[i];
            oss << " d" << i << '='
                << d.index << ','
                << d.width << ','
                << d.height << ','
                << d.refreshHz << ','
                << (d.isPrimary ? 1 : 0) << ','
                << encode_token(d.name);
        }
        oss << '\n';
        return oss.str();
    }

    if (resp.stats.has_value()) {
        const auto& st = *resp.stats;
        oss << " state=" << core::to_string(st.state)
            << " elapsed_ms=" << st.elapsedMs
            << " frames=" << st.framesEncoded
            << " dropped=" << st.framesDropped
            << " fps=" << format_double(st.currentFps)
            << " memory_mb=" << format_double(st.memoryUsageMb)
            << " gpu=" << (st.gpuAccelerated ? 1 : 0)
            << " backend=" << core::to_string(st.gpuBackend)
            << " encoder=" << encode_token(st.encoderName)
            << " output=" << encode_token(path_to_utf8(st.outputPath))
            << " last_error=" << encode_token(st.lastError);
        oss << '\n';
        return oss.str();
    }

    if (!resp.message.empty()) {
        oss << " msg=" << encode_token(resp.message);
    }
    oss << '\n';
    return oss.str();
}

core::Result<IpcResponse> parse_response(std::string_view line) {
    const std::string clean = strip_line_ending(line);
    if (clean.empty() || clean.size() > kMaxIpcMessageBytes) {
        return core::Result<IpcResponse>::make_err(
            core::ErrorCode::IpcError, "Empty or oversized IPC response");
    }

    const auto tokens = split_ws(clean);
    if (tokens.empty()) {
        return core::Result<IpcResponse>::make_err(
            core::ErrorCode::IpcError, "Empty IPC response");
    }

    auto kvRes = parse_kv_tokens(tokens, 1);
    if (!kvRes) {
        return core::Result<IpcResponse>::make_err(kvRes.error());
    }
    const auto& kv = kvRes.value();

    IpcResponse resp{};
    if (tokens[0] == "ERR") {
        resp.ok = false;
        if (auto it = kv.find("code"); it != kv.end()) {
            auto r = parse_int<int>(it->second);
            if (r) {
                resp.errorCode = static_cast<core::ErrorCode>(r.value());
            }
        }
        if (auto it = kv.find("msg"); it != kv.end()) {
            auto m = decode_token(it->second);
            resp.message = m ? m.take() : std::string(it->second);
        }
        return core::Result<IpcResponse>::make_ok(std::move(resp));
    }

    if (tokens[0] != "OK") {
        return core::Result<IpcResponse>::make_err(
            core::ErrorCode::IpcError, "IPC response must begin with OK or ERR");
    }

    resp.ok = true;
    resp.errorCode = core::ErrorCode::Ok;

    if (auto it = kv.find("msg"); it != kv.end()) {
        auto dec = decode_token(it->second);
        if (!dec) {
            return core::Result<IpcResponse>::make_err(dec.error());
        }
        resp.message = dec.take();
    }

    if (auto itKind = kv.find("kind"); itKind != kv.end() && itKind->second == "displays") {
        resp.message = "displays";
        auto itCount = kv.find("count");
        if (itCount == kv.end()) {
            return core::Result<IpcResponse>::make_err(
                core::ErrorCode::IpcError, "Displays response missing count");
        }
        auto countRes = parse_int<std::size_t>(itCount->second);
        if (!countRes) {
            return core::Result<IpcResponse>::make_err(countRes.error());
        }
        const std::size_t count = countRes.value();
        resp.displays.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::string key = "d" + std::to_string(i);
            auto itD = kv.find(key);
            if (itD == kv.end()) {
                return core::Result<IpcResponse>::make_err(
                    core::ErrorCode::IpcError, "Displays response missing display entry");
            }
            const auto parts = split_char(itD->second, ',');
            if (parts.size() != 6) {
                return core::Result<IpcResponse>::make_err(
                    core::ErrorCode::IpcError, "Malformed display entry");
            }
            core::DisplayInfo info{};
            auto idxRes = parse_int<std::uint32_t>(parts[0]);
            auto wRes = parse_int<std::uint32_t>(parts[1]);
            auto hRes = parse_int<std::uint32_t>(parts[2]);
            auto hzRes = parse_int<std::uint32_t>(parts[3]);
            auto nameRes = decode_token(parts[5]);
            if (!idxRes || !wRes || !hRes || !hzRes || !nameRes) {
                return core::Result<IpcResponse>::make_err(
                    core::ErrorCode::IpcError, "Invalid display entry field");
            }
            info.index = idxRes.value();
            info.width = wRes.value();
            info.height = hRes.value();
            info.refreshHz = hzRes.value();
            info.isPrimary = (parts[4] != "0");
            info.name = nameRes.take();
            resp.displays.push_back(std::move(info));
        }
        return core::Result<IpcResponse>::make_ok(std::move(resp));
    }

    if (auto itState = kv.find("state"); itState != kv.end()) {
        core::RecordingStats st{};
        const auto parsedState = core::parse_recorder_state(itState->second);
        if (!parsedState) {
            return core::Result<IpcResponse>::make_err(
                core::ErrorCode::IpcError, "Unknown recorder state in IPC response");
        }
        st.state = *parsedState;

        if (auto it = kv.find("elapsed_ms"); it != kv.end()) {
            if (auto r = parse_int<std::uint64_t>(it->second); r) {
                st.elapsedMs = r.value();
            }
        }
        if (auto it = kv.find("frames"); it != kv.end()) {
            if (auto r = parse_int<std::uint64_t>(it->second); r) {
                st.framesEncoded = r.value();
            }
        }
        if (auto it = kv.find("dropped"); it != kv.end()) {
            if (auto r = parse_int<std::uint64_t>(it->second); r) {
                st.framesDropped = r.value();
            }
        }
        if (auto it = kv.find("fps"); it != kv.end()) {
            if (auto r = parse_double(it->second); r) {
                st.currentFps = r.value();
            }
        }
        if (auto it = kv.find("memory_mb"); it != kv.end()) {
            if (auto r = parse_double(it->second); r) {
                st.memoryUsageMb = r.value();
            }
        }
        if (auto it = kv.find("gpu"); it != kv.end()) {
            st.gpuAccelerated = (it->second != "0");
        }
        if (auto it = kv.find("backend"); it != kv.end()) {
            if (auto b = core::parse_gpu_backend(it->second); b) {
                st.gpuBackend = *b;
            }
        }
        if (auto it = kv.find("encoder"); it != kv.end()) {
            if (auto d = decode_token(it->second); d) {
                st.encoderName = d.take();
            }
        }
        if (auto it = kv.find("output"); it != kv.end()) {
            if (auto d = decode_token(it->second); d) {
                st.outputPath = utf8_to_path(d.value());
            }
        }
        if (auto it = kv.find("last_error"); it != kv.end()) {
            if (auto d = decode_token(it->second); d) {
                st.lastError = d.take();
            }
        }

        resp.stats = std::move(st);
    }

    return core::Result<IpcResponse>::make_ok(std::move(resp));
}

} // namespace shine::ipc
