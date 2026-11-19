#include "shine/ipc/ipc_transport.hpp"

#include "platform/linux/linux_raii.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace shine::ipc {

namespace {

[[nodiscard]] std::string normalize_socket_path(std::string endpoint) {
    if (endpoint.find('/') == std::string::npos) {
        return "/tmp/" + endpoint + ".sock";
    }
    return endpoint;
}

class LinuxUnixServer final : public IpcServer {
public:
    explicit LinuxUnixServer(std::string endpoint)
        : endpoint_(normalize_socket_path(std::move(endpoint))) {}

    ~LinuxUnixServer() override {
        stop();
    }

    core::Result<void> start(IpcHandler handler) override {
        if (!handler) {
            return core::Result<void>::make_err(
                core::ErrorCode::InvalidArgument, "IPC handler must not be empty");
        }
        if (running_.load(std::memory_order_acquire)) {
            return core::Result<void>::make_err(
                core::ErrorCode::AlreadyRunning, "IPC server already running");
        }

        if (endpoint_.size() >= sizeof(sockaddr_un{}.sun_path)) {
            return core::Result<void>::make_err(
                core::ErrorCode::InvalidArgument, "Unix socket path too long");
        }

        int pipeFds[2]{-1, -1};
        if (::pipe(pipeFds) != 0) {
            return core::Result<void>::make_err(
                core::ErrorCode::IpcError, "Failed to create wake pipe");
        }
        wakeReadFd_.reset(pipeFds[0]);
        wakeWriteFd_.reset(pipeFds[1]);
        ::fcntl(wakeReadFd_.get(), F_SETFL, O_NONBLOCK);
        ::fcntl(wakeWriteFd_.get(), F_SETFL, O_NONBLOCK);

        std::error_code ec;
        std::filesystem::remove(endpoint_, ec);

        listenFd_.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (!listenFd_.valid()) {
            return core::Result<void>::make_err(
                core::ErrorCode::IpcError, "socket(AF_UNIX) failed");
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, endpoint_.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(listenFd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            listenFd_.reset();
            return core::Result<void>::make_err(
                core::ErrorCode::IpcError,
                std::string("bind() failed: ") + std::strerror(errno));
        }

        if (::listen(listenFd_.get(), 16) != 0) {
            listenFd_.reset();
            return core::Result<void>::make_err(
                core::ErrorCode::IpcError,
                std::string("listen() failed: ") + std::strerror(errno));
        }

        handler_ = std::move(handler);
        stopRequested_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        serverThread_ = std::thread(&LinuxUnixServer::serve_loop, this);
        return core::Result<void>::make_ok();
    }

    void stop() override {
        stopRequested_.store(true, std::memory_order_release);
        if (wakeWriteFd_.valid()) {
            const char byte = 1;
            const ssize_t ignored = ::write(wakeWriteFd_.get(), &byte, 1);
            (void)ignored;
        }
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
        running_.store(false, std::memory_order_release);
        listenFd_.reset();
        wakeReadFd_.reset();
        wakeWriteFd_.reset();

        std::error_code ec;
        std::filesystem::remove(endpoint_, ec);
    }

    const std::string& endpoint() const noexcept override {
        return endpoint_;
    }

private:
    void serve_loop() {
        while (!stopRequested_.load(std::memory_order_acquire)) {
            pollfd fds[2]{};
            fds[0].fd = wakeReadFd_.get();
            fds[0].events = POLLIN;
            fds[1].fd = listenFd_.get();
            fds[1].events = POLLIN;

            const int pr = ::poll(fds, 2, -1);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            if ((fds[0].revents & POLLIN) != 0 || stopRequested_.load(std::memory_order_acquire)) {
                break;
            }

            if ((fds[1].revents & POLLIN) != 0) {
                platform::linux_impl::UniqueFd clientFd(::accept(listenFd_.get(), nullptr, nullptr));
                if (!clientFd.valid()) {
                    continue;
                }
                handle_client(clientFd.get());
            }
        }
        running_.store(false, std::memory_order_release);
    }

    void handle_client(int clientFd) {
        std::string line;
        line.reserve(512);
        while (line.size() < kMaxIpcMessageBytes) {
            char chunk[1024]{};
            const ssize_t n = ::recv(clientFd, chunk, sizeof(chunk), 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if (n == 0) {
                break;
            }
            line.append(chunk, static_cast<std::size_t>(n));
            if (line.find('\n') != std::string::npos) {
                break;
            }
        }

        auto reqRes = parse_request(line);
        IpcResponse resp = reqRes
            ? handler_(reqRes.value())
            : IpcResponse::make_error(reqRes.error().code, reqRes.error().message);

        const std::string out = serialize_response(resp);
        std::size_t sentTotal = 0;
        while (sentTotal < out.size()) {
            const ssize_t n = ::send(
                clientFd, out.data() + sentTotal, out.size() - sentTotal, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if (n == 0) {
                return;
            }
            sentTotal += static_cast<std::size_t>(n);
        }
    }

    std::string endpoint_;
    IpcHandler handler_;
    platform::linux_impl::UniqueFd listenFd_;
    platform::linux_impl::UniqueFd wakeReadFd_;
    platform::linux_impl::UniqueFd wakeWriteFd_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread serverThread_;
};

class LinuxUnixClient final : public IpcClient {
public:
    explicit LinuxUnixClient(std::string endpoint)
        : endpoint_(normalize_socket_path(std::move(endpoint))) {}

    core::Result<IpcResponse> send(const IpcRequest& req, std::uint32_t timeoutMs) override {
        if (endpoint_.size() >= sizeof(sockaddr_un{}.sun_path)) {
            return core::Result<IpcResponse>::make_err(
                core::ErrorCode::InvalidArgument, "Unix socket path too long");
        }

        platform::linux_impl::UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (!fd.valid()) {
            return core::Result<IpcResponse>::make_err(
                core::ErrorCode::IpcError, "socket(AF_UNIX) failed");
        }

        timeval tv{};
        tv.tv_sec = static_cast<time_t>(timeoutMs / 1000U);
        tv.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000U) * 1000U);
        ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, endpoint_.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            return core::Result<IpcResponse>::make_err(
                core::ErrorCode::IpcError,
                std::string("Failed to connect to daemon socket: ") + std::strerror(errno));
        }

        const std::string payload = serialize_request(req);
        std::size_t sentTotal = 0;
        while (sentTotal < payload.size()) {
            const ssize_t n = ::send(
                fd.get(), payload.data() + sentTotal, payload.size() - sentTotal, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return core::Result<IpcResponse>::make_err(
                    core::ErrorCode::IpcError, "Failed to write IPC request");
            }
            if (n == 0) {
                return core::Result<IpcResponse>::make_err(
                    core::ErrorCode::IpcError, "Socket closed while writing IPC request");
            }
            sentTotal += static_cast<std::size_t>(n);
        }

        std::string responseLine;
        responseLine.reserve(512);
        while (responseLine.size() < kMaxIpcMessageBytes) {
            char buf[1024]{};
            const ssize_t n = ::recv(fd.get(), buf, sizeof(buf), 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return core::Result<IpcResponse>::make_err(
                    core::ErrorCode::IpcError, "Failed to read IPC response");
            }
            if (n == 0) {
                break;
            }
            responseLine.append(buf, static_cast<std::size_t>(n));
            if (responseLine.find('\n') != std::string::npos) {
                break;
            }
        }

        return parse_response(responseLine);
    }

    const std::string& endpoint() const noexcept override {
        return endpoint_;
    }

private:
    std::string endpoint_;
};

} // namespace

std::unique_ptr<IpcServer> create_ipc_server(std::string endpoint) {
    return std::make_unique<LinuxUnixServer>(std::move(endpoint));
}

std::unique_ptr<IpcClient> create_ipc_client(std::string endpoint) {
    return std::make_unique<LinuxUnixClient>(std::move(endpoint));
}

std::string default_ipc_endpoint() {
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir != nullptr && runtimeDir[0] != '\0') {
        return (std::filesystem::path(runtimeDir) / "shine.sock").string();
    }
    return "/tmp/shine-" + std::to_string(static_cast<unsigned long>(::geteuid())) + ".sock";
}

} // namespace shine::ipc
