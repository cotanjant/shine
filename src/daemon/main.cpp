#include "daemon/daemon_app.hpp"

#include "shine/ipc/ipc_transport.hpp"
#include "shine/service/autostart.hpp"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32

#include "platform/windows/win_raii.hpp"

#include <shellapi.h>

namespace {

struct DaemonCliOptions {
    std::string endpoint;
    bool customEndpoint{false};
    bool installAutostart{false};
    bool uninstallAutostart{false};
};

[[nodiscard]] DaemonCliOptions parse_windows_args() {
    DaemonCliOptions opts{};
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return opts;
    }

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--endpoint" && i + 1 < argc) {
            opts.endpoint = shine::platform::win::wide_to_utf8(argv[++i]);
            opts.customEndpoint = !opts.endpoint.empty();
        } else if (arg == L"--install-autostart") {
            opts.installAutostart = true;
        } else if (arg == L"--uninstall-autostart") {
            opts.uninstallAutostart = true;
        }
    }

    ::LocalFree(argv);
    return opts;
}

[[nodiscard]] std::filesystem::path current_exe_path() {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(std::wstring(buf, len));
    }
    return {};
}

LRESULT CALLBACK daemon_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam != FALSE) {
            ::PostQuitMessage(0);
        }
        return 0;
    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    const DaemonCliOptions opts = parse_windows_args();

    if (opts.installAutostart) {
        return shine::service::install_autostart(current_exe_path()).ok() ? 0 : 1;
    }
    if (opts.uninstallAutostart) {
        return shine::service::uninstall_autostart().ok() ? 0 : 1;
    }

    shine::platform::win::UniqueHandle instanceMutex;
    if (!opts.customEndpoint) {
        HANDLE mtx = ::CreateMutexW(nullptr, TRUE, L"Local\\ShineRecorderDaemonV1");
        if (mtx != nullptr) {
            const DWORD lastErr = ::GetLastError();
            instanceMutex.reset(mtx);
            if (lastErr == ERROR_ALREADY_EXISTS) {
                return 0;
            }
        }
    }

    const wchar_t* kClassName = L"ShineDaemonMessageWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = daemon_wnd_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    ::RegisterClassExW(&wc);

    HWND msgHwnd = ::CreateWindowExW(
        0,
        kClassName,
        L"ShineDaemon",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        hInstance,
        nullptr);

    const std::string endpoint = opts.customEndpoint
                                     ? opts.endpoint
                                     : shine::ipc::default_ipc_endpoint();

    std::atomic<bool> shutdownPosted{false};
    shine::daemon::DaemonApp app(
        endpoint,
        [msgHwnd, &shutdownPosted]() {
            if (!shutdownPosted.exchange(true, std::memory_order_acq_rel) && msgHwnd != nullptr) {
                ::PostMessageW(msgHwnd, WM_CLOSE, 0, 0);
            }
        },
        !opts.customEndpoint);

    auto startRes = app.start();
    if (!startRes) {
        if (msgHwnd != nullptr) {
            ::DestroyWindow(msgHwnd);
        }
        return 1;
    }

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    app.stop();
    return 0;
}

#else

#include "platform/linux/linux_raii.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

int g_sigWriteFd = -1;

void handle_signal(int) noexcept {
    if (g_sigWriteFd >= 0) {
        const char b = 1;
        const ssize_t ignored = ::write(g_sigWriteFd, &b, 1);
        (void)ignored;
    }
}

[[nodiscard]] std::filesystem::path current_exe_path_linux(const char* argv0) {
    char buf[4096]{};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return std::filesystem::path(buf);
    }
    return argv0 != nullptr ? std::filesystem::weakly_canonical(argv0) : std::filesystem::path{};
}

} // namespace

int main(int argc, char** argv) {
    std::string endpoint;
    bool customEndpoint = false;
    bool installAutostart = false;
    bool uninstallAutostart = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--endpoint" && i + 1 < argc) {
            endpoint = argv[++i];
            customEndpoint = !endpoint.empty();
        } else if (arg == "--install-autostart") {
            installAutostart = true;
        } else if (arg == "--uninstall-autostart") {
            uninstallAutostart = true;
        }
    }

    if (installAutostart) {
        return shine::service::install_autostart(current_exe_path_linux(argv[0])).ok() ? 0 : 1;
    }
    if (uninstallAutostart) {
        return shine::service::uninstall_autostart().ok() ? 0 : 1;
    }

    const std::string activeEndpoint = customEndpoint
                                           ? endpoint
                                           : shine::ipc::default_ipc_endpoint();

    shine::platform::linux_impl::UniqueFd lockFd;
    if (!customEndpoint) {
        const std::string lockPath = activeEndpoint + ".lock";
        lockFd.reset(::open(lockPath.c_str(), O_CREAT | O_RDWR, 0600));
        if (lockFd.valid() && ::flock(lockFd.get(), LOCK_EX | LOCK_NB) != 0) {
            return 0;
        }
    }

    int wakePipe[2]{-1, -1};
    if (::pipe(wakePipe) != 0) {
        return 1;
    }
    shine::platform::linux_impl::UniqueFd wakeRead(wakePipe[0]);
    shine::platform::linux_impl::UniqueFd wakeWrite(wakePipe[1]);
    ::fcntl(wakeRead.get(), F_SETFL, O_NONBLOCK);
    ::fcntl(wakeWrite.get(), F_SETFL, O_NONBLOCK);

    g_sigWriteFd = wakeWrite.get();
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    shine::daemon::DaemonApp app(
        activeEndpoint,
        [&wakeWrite]() {
            if (wakeWrite.valid()) {
                const char b = 1;
                const ssize_t ignored = ::write(wakeWrite.get(), &b, 1);
                (void)ignored;
            }
        },
        !customEndpoint);

    auto startRes = app.start();
    if (!startRes) {
        g_sigWriteFd = -1;
        return 1;
    }

    while (true) {
        pollfd pfd{};
        pfd.fd = wakeRead.get();
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, -1);
        if (pr < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    g_sigWriteFd = -1;
    app.stop();
    return 0;
}

#endif
