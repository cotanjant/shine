#include "shine/service/autostart.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace shine::service {

namespace {

[[nodiscard]] std::filesystem::path user_service_path() {
    const char* configHome = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base;
    if (configHome != nullptr && configHome[0] != '\0') {
        base = configHome;
    } else {
        const char* home = std::getenv("HOME");
        base = (home != nullptr && home[0] != '\0')
                   ? (std::filesystem::path(home) / ".config")
                   : std::filesystem::path("/tmp");
    }
    return base / "systemd" / "user" / "shine-daemon.service";
}

} // namespace

core::Result<void> install_autostart(const std::filesystem::path& daemonExe) {
    if (daemonExe.empty()) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Daemon executable path is empty");
    }

    const auto unitPath = user_service_path();
    std::error_code ec;
    std::filesystem::create_directories(unitPath.parent_path(), ec);
    if (ec) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError,
            "Failed to create systemd user directory: " + ec.message());
    }

    std::ofstream out(unitPath, std::ios::trunc);
    if (!out.is_open()) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError, "Failed to write systemd user unit");
    }

    out << "[Unit]\n"
        << "Description=Shine Screen Recorder Background Service\n"
        << "After=graphical-session.target\n\n"
        << "[Service]\n"
        << "Type=simple\n"
        << "ExecStart=\"" << daemonExe.string() << "\"\n"
        << "Restart=on-failure\n"
        << "RestartSec=2\n\n"
        << "[Install]\n"
        << "WantedBy=graphical-session.target\n";
    out.flush();
    if (!out) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError, "Failed while writing systemd user unit");
    }

    return core::Result<void>::make_ok();
}

core::Result<void> uninstall_autostart() {
    const auto unitPath = user_service_path();
    std::error_code ec;
    std::filesystem::remove(unitPath, ec);
    if (ec) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError,
            "Failed to remove systemd user unit: " + ec.message());
    }
    return core::Result<void>::make_ok();
}

bool is_autostart_installed() {
    std::error_code ec;
    return std::filesystem::exists(user_service_path(), ec);
}

core::Result<void> launch_daemon_detached(
    const std::filesystem::path& daemonExe,
    std::string_view endpointOverride) {
    if (daemonExe.empty()) {
        return core::Result<void>::make_err(
            core::ErrorCode::InvalidArgument, "Daemon executable path is empty");
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        return core::Result<void>::make_err(
            core::ErrorCode::IoError, "fork() failed while launching daemon");
    }

    if (pid == 0) {
        ::setsid();
        const pid_t grandchild = ::fork();
        if (grandchild < 0) {
            ::_exit(127);
        }
        if (grandchild > 0) {
            ::_exit(0);
        }

        const int devNull = ::open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::dup2(devNull, STDOUT_FILENO);
            ::dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) {
                ::close(devNull);
            }
        }

        std::string exeStr = daemonExe.string();
        std::string flagStr = "--endpoint";
        std::string epStr(endpointOverride);
        std::vector<char*> argv;
        argv.push_back(exeStr.data());
        if (!epStr.empty()) {
            argv.push_back(flagStr.data());
            argv.push_back(epStr.data());
        }
        argv.push_back(nullptr);

        ::execv(exeStr.c_str(), argv.data());
        ::_exit(127);
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    return core::Result<void>::make_ok();
}

} // namespace shine::service
