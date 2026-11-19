#pragma once

#include "shine/core/result.hpp"

#include <filesystem>
#include <string_view>

namespace shine::service {

[[nodiscard]] core::Result<void> install_autostart(const std::filesystem::path& daemonExe);
[[nodiscard]] core::Result<void> uninstall_autostart();
[[nodiscard]] bool is_autostart_installed();
[[nodiscard]] core::Result<void> launch_daemon_detached(
    const std::filesystem::path& daemonExe,
    std::string_view endpointOverride = {});

} // namespace shine::service
