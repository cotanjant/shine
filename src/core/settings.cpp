#include "shine/core/settings.hpp"

#include "shine/core/hotkey.hpp"
#include "shine/core/recorder.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <shlobj.h>
#include <knownfolders.h>
#endif

namespace shine::core {

namespace {

#ifdef _WIN32
struct CoTaskMemDeleter {
    void operator()(wchar_t* p) const noexcept {
        if (p != nullptr) {
            ::CoTaskMemFree(p);
        }
    }
};
#endif

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

[[nodiscard]] std::filesystem::path utf8_to_path(std::string_view s) {
    std::u8string u8(reinterpret_cast<const char8_t*>(s.data()), s.size());
    return std::filesystem::path(u8);
}

[[nodiscard]] std::string_view trim_sv(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
        sv.remove_suffix(1);
    }
    return sv;
}

[[nodiscard]] bool parse_bool(std::string_view sv, bool fallback) noexcept {
    if (sv == "1" || sv == "true" || sv == "TRUE") {
        return true;
    }
    if (sv == "0" || sv == "false" || sv == "FALSE") {
        return false;
    }
    return fallback;
}

[[nodiscard]] std::uint32_t parse_u32(std::string_view sv, std::uint32_t fallback) noexcept {
    std::uint32_t val = 0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) {
        return fallback;
    }
    return val;
}

[[nodiscard]] AppSettings sanitize_settings(AppSettings s) {
    if (auto hk = parse_hotkey(s.clipHotkey); hk && hk->valid()) {
        s.clipHotkey = hk->to_string();
    } else {
        s.clipHotkey = "F8";
    }

    if (auto ptt = parse_hotkey(s.voicePttHotkey); ptt && ptt->valid()) {
        s.voicePttHotkey = ptt->to_string();
    } else {
        s.voicePttHotkey = "V";
    }

    s.clipLengthSeconds = std::clamp<std::uint32_t>(s.clipLengthSeconds, 5U, 300U);
    s.fps = std::clamp<std::uint32_t>(s.fps, 1U, 240U);
    s.bitrateKbps = std::clamp<std::uint32_t>(s.bitrateKbps, 500U, 100000U);
    if (s.outputDirectory.empty()) {
        s.outputDirectory = default_recordings_dir();
    }
    return s;
}

} // namespace

std::filesystem::path default_settings_path() {
#ifdef _WIN32
    wchar_t* rawPath = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &rawPath)) &&
        rawPath != nullptr) {
        std::unique_ptr<wchar_t, CoTaskMemDeleter> guard(rawPath);
        return std::filesystem::path(rawPath) / L"Shine" / L"settings.conf";
    }

    wchar_t* appData = nullptr;
    std::size_t len = 0;
    if (_wdupenv_s(&appData, &len, L"APPDATA") == 0 && appData != nullptr) {
        std::unique_ptr<wchar_t, decltype(&std::free)> envGuard(appData, &std::free);
        return std::filesystem::path(appData) / L"Shine" / L"settings.conf";
    }
    return default_recordings_dir() / L"settings.conf";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "shine" / "settings.conf";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "shine" / "settings.conf";
    }
    return default_recordings_dir() / "settings.conf";
#endif
}

AppSettings load_settings(const std::filesystem::path& path) {
    AppSettings settings{};
    settings.outputDirectory = default_recordings_dir();

    std::ifstream in(path);
    if (!in.is_open()) {
        return sanitize_settings(std::move(settings));
    }

    std::string line;
    while (std::getline(in, line)) {
        const std::string_view sv = trim_sv(line);
        if (sv.empty() || sv.front() == '#') {
            continue;
        }
        const std::size_t eq = sv.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            continue;
        }
        const std::string_view key = trim_sv(sv.substr(0, eq));
        const std::string_view val = trim_sv(sv.substr(eq + 1));

        if (key == "first_run_completed") {
            settings.firstRunCompleted = parse_bool(val, settings.firstRunCompleted);
        } else if (key == "clip_hotkey") {
            settings.clipHotkey = std::string(val);
        } else if (key == "clip_length_seconds") {
            settings.clipLengthSeconds = parse_u32(val, settings.clipLengthSeconds);
        } else if (key == "replay_enabled") {
            settings.replayEnabled = parse_bool(val, settings.replayEnabled);
        } else if (key == "voice_enabled") {
            settings.voiceEnabled = parse_bool(val, settings.voiceEnabled);
        } else if (key == "voice_ptt_enabled") {
            settings.voicePttEnabled = parse_bool(val, settings.voicePttEnabled);
        } else if (key == "voice_ptt_hotkey") {
            settings.voicePttHotkey = std::string(val);
        } else if (key == "display_index") {
            settings.displayIndex = parse_u32(val, settings.displayIndex);
        } else if (key == "fps") {
            settings.fps = parse_u32(val, settings.fps);
        } else if (key == "bitrate_kbps") {
            settings.bitrateKbps = parse_u32(val, settings.bitrateKbps);
        } else if (key == "capture_cursor") {
            settings.captureCursor = parse_bool(val, settings.captureCursor);
        } else if (key == "output_directory" && !val.empty()) {
            settings.outputDirectory = utf8_to_path(val);
        }
    }

    return sanitize_settings(std::move(settings));
}

Result<void> save_settings(const AppSettings& rawSettings, const std::filesystem::path& path) {
    const AppSettings settings = sanitize_settings(rawSettings);

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return Result<void>::make_err(
                ErrorCode::IoError, "Failed to create settings directory: " + ec.message());
        }
    }

    std::filesystem::path tmpPath = path;
    tmpPath += ".tmp";

    {
        std::ofstream out(tmpPath, std::ios::trunc | std::ios::binary);
        if (!out.is_open()) {
            return Result<void>::make_err(
                ErrorCode::IoError, "Failed to open temporary settings file for writing");
        }

        out << "first_run_completed=" << (settings.firstRunCompleted ? 1 : 0) << '\n'
            << "clip_hotkey=" << settings.clipHotkey << '\n'
            << "clip_length_seconds=" << settings.clipLengthSeconds << '\n'
            << "replay_enabled=" << (settings.replayEnabled ? 1 : 0) << '\n'
            << "voice_enabled=" << (settings.voiceEnabled ? 1 : 0) << '\n'
            << "voice_ptt_enabled=" << (settings.voicePttEnabled ? 1 : 0) << '\n'
            << "voice_ptt_hotkey=" << settings.voicePttHotkey << '\n'
            << "display_index=" << settings.displayIndex << '\n'
            << "fps=" << settings.fps << '\n'
            << "bitrate_kbps=" << settings.bitrateKbps << '\n'
            << "capture_cursor=" << (settings.captureCursor ? 1 : 0) << '\n'
            << "output_directory=" << path_to_utf8(settings.outputDirectory) << '\n';
        out.flush();
        if (!out) {
            std::filesystem::remove(tmpPath, ec);
            return Result<void>::make_err(
                ErrorCode::IoError, "Failed while writing settings file");
        }
    }

    std::filesystem::rename(tmpPath, path, ec);
    if (ec) {
        std::filesystem::remove(tmpPath, ec);
        return Result<void>::make_err(
            ErrorCode::IoError, "Failed to commit settings file: " + ec.message());
    }

    return Result<void>::make_ok();
}

} // namespace shine::core
