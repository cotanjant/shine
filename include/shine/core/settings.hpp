#pragma once

#include "shine/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace shine::core {

struct AppSettings {
    bool firstRunCompleted{false};
    std::string clipHotkey{"F8"};
    std::uint32_t clipLengthSeconds{30};
    bool replayEnabled{true};
    bool voiceEnabled{false};
    bool voicePttEnabled{false};
    std::string voicePttHotkey{"V"};
    std::uint32_t displayIndex{0};
    std::uint32_t fps{60};
    std::uint32_t bitrateKbps{8000};
    bool captureCursor{true};
    std::filesystem::path outputDirectory;
};

[[nodiscard]] std::filesystem::path default_settings_path();
[[nodiscard]] AppSettings load_settings(const std::filesystem::path& path = default_settings_path());
[[nodiscard]] Result<void> save_settings(
    const AppSettings& settings,
    const std::filesystem::path& path = default_settings_path());

} // namespace shine::core
