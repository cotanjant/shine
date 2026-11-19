#include "shine/ipc/ipc_transport.hpp"

#include "platform/windows/win_raii.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace shine::ipc {

namespace {

[[nodiscard]] std::string normalize_pipe_name(std::string endpoint) {
    constexpr std::string_view kPrefix = R"(\\.\pipe\)";
    if (endpoint.rfind(kPrefix, 0) != 0) {
        return std::string(kPrefix) + endpoint;
    }
    return endpoint;
}

[[nodiscard]] platform::win::UniqueHandle create_pipe_instance(const std::wstring& widePipe) {
    return platform::win::UniqueHandle(::CreateNamedPipeW(
        widePipe.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        32768,
        32768,
        0,
        nullptr));
}

class WinPipeServer final : public IpcServer {
public:
    explicit WinPipeServer(std::string endpoint)
        : endpoint_(normalize_pipe_name(std::move(endpoint))) {}

    ~WinPipeServer() override {
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

        stopEvent_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stopEvent_.valid()) {
            return core::Result<void>::make_err(
                core::ErrorCode::IpcError, "CreateEventW failed for IPC stop event");
        }

        handler_ = std::move(handler);
        stopRequested_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);

        serverThread_ = std::thread(&WinPipeServer::serve_loop, this);
        return core::Result<void>::make_ok();
    }

    void stop() override {
        stopRequested_.store(true, std::memory_order_release);
        if (stopEvent_.valid()) {
            ::SetEvent(stopEvent_.get());
        }
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
        running_.store(false, std::memory_order_release);
        stopEvent_.reset();
    }

    const std::string& endpoint() const noexcept override {
        return endpoint_;
    }

private:
    void serve_loop() {
        const std::wstring widePipe = platform::win::utf8_to_wide(endpoint_);
        platform::win::UniqueHandle connectEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!connectEvent.valid()) {
            running_.store(false, std::memory_order_release);
            return;
        }

        platform::win::UniqueHandle pipe = create_pipe_instance(widePipe);
        while (!stopRequested_.load(std::memory_order_acquire)) {
            if (!pipe.valid()) {
                pipe = create_pipe_instance(widePipe);
                if (!pipe.valid()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
            }

            ::ResetEvent(connectEvent.get());
            OVERLAPPED ov{};
            ov.hEvent = connectEvent.get();

            BOOL connected = ::ConnectNamedPipe(pipe.get(), &ov);
            if (!connected) {
                const DWORD err = ::GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE waitHandles[2] = {stopEvent_.get(), connectEvent.get()};
                    const DWORD waitRes = ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                    if (waitRes == WAIT_OBJECT_0) {
                        ::CancelIo(pipe.get());
                        break;
                    }
                    if (waitRes != WAIT_OBJECT_0 + 1) {
                        ::CancelIo(pipe.get());
                        pipe.reset();
                        continue;
                    }
                    DWORD dummy = 0;
                    if (!::GetOverlappedResult(pipe.get(), &ov, &dummy, FALSE)) {
                        pipe.reset();
                        continue;
                    }
                } else if (err != ERROR_PIPE_CONNECTED) {
                    pipe.reset();
                    continue;
                }
            }

            if (stopRequested_.load(std::memory_order_acquire)) {
                ::DisconnectNamedPipe(pipe.get());
                break;
            }

            platform::win::UniqueHandle nextPipe = create_pipe_instance(widePipe);
            handle_client(pipe.get());
            ::FlushFileBuffers(pipe.get());
            ::DisconnectNamedPipe(pipe.get());
            pipe = std::move(nextPipe);
        }

        running_.store(false, std::memory_order_release);
    }

    void handle_client(HANDLE pipe) {
        std::string line;
        line.reserve(512);

        platform::win::UniqueHandle ioEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!ioEvent.valid()) {
            return;
        }

        while (line.size() < kMaxIpcMessageBytes) {
            char chunk[1024]{};
            ::ResetEvent(ioEvent.get());
            OVERLAPPED ov{};
            ov.hEvent = ioEvent.get();

            DWORD bytesRead = 0;
            BOOL ok = ::ReadFile(pipe, chunk, sizeof(chunk), &bytesRead, &ov);
            if (!ok) {
                const DWORD err = ::GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE waitHandles[2] = {stopEvent_.get(), ioEvent.get()};
                    const DWORD w = ::WaitForMultipleObjects(2, waitHandles, FALSE, 5000);
                    if (w != WAIT_OBJECT_0 + 1) {
                        ::CancelIo(pipe);
                        return;
                    }
                    if (!::GetOverlappedResult(pipe, &ov, &bytesRead, FALSE)) {
                        return;
                    }
                } else if (err == ERROR_MORE_DATA) {
                    // Partial read; continue reading
                } else {
                    return;
                }
            }

            if (bytesRead == 0) {
                break;
            }

            line.append(chunk, bytesRead);
            if (line.find('\n') != std::string::npos) {
                break;
            }
        }

        auto reqRes = parse_request(line);
        IpcResponse resp = reqRes
            ? handler_(reqRes.value())
            : IpcResponse::make_error(reqRes.error().code, reqRes.error().message);

        const std::string out = serialize_response(resp);
        std::size_t writtenTotal = 0;
        while (writtenTotal < out.size()) {
            ::ResetEvent(ioEvent.get());
            OVERLAPPED ov{};
            ov.hEvent = ioEvent.get();

            DWORD writtenNow = 0;
            const DWORD toWrite = static_cast<DWORD>(out.size() - writtenTotal);
            BOOL ok = ::WriteFile(pipe, out.data() + writtenTotal, toWrite, &writtenNow, &ov);
            if (!ok) {
                const DWORD err = ::GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE waitHandles[2] = {stopEvent_.get(), ioEvent.get()};
                    const DWORD w = ::WaitForMultipleObjects(2, waitHandles, FALSE, 5000);
                    if (w != WAIT_OBJECT_0 + 1) {
                        ::CancelIo(pipe);
                        return;
                    }
                    if (!::GetOverlappedResult(pipe, &ov, &writtenNow, FALSE)) {
                        return;
                    }
                } else {
                    return;
                }
            }
            if (writtenNow == 0) {
                return;
            }
            writtenTotal += writtenNow;
        }
    }

    std::string endpoint_;
    IpcHandler handler_;
    platform::win::UniqueHandle stopEvent_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread serverThread_;
};

class WinPipeClient final : public IpcClient {
public:
    explicit WinPipeClient(std::string endpoint)
        : endpoint_(normalize_pipe_name(std::move(endpoint))) {}

    core::Result<IpcResponse> send(const IpcRequest& req, std::uint32_t timeoutMs) override {
        const std::wstring widePipe = platform::win::utf8_to_wide(endpoint_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

        platform::win::UniqueHandle pipe;
        while (true) {
            HANDLE h = ::CreateFileW(
                widePipe.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                pipe.reset(h);
                break;
            }

            const DWORD err = ::GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    return core::Result<IpcResponse>::make_err(
                        core::ErrorCode::IpcError, "Timed out waiting for busy daemon pipe");
                }
                const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                ::WaitNamedPipeW(widePipe.c_str(), static_cast<DWORD>(std::max<long long>(1, rem)));
                continue;
            }

            return core::Result<IpcResponse>::make_err(
                core::ErrorCode::IpcError,
                "Failed to connect to daemon pipe (" + std::to_string(err) + ")");
        }

        const std::string payload = serialize_request(req);
        std::size_t totalWritten = 0;
        while (totalWritten < payload.size()) {
            DWORD written = 0;
            const DWORD remaining = static_cast<DWORD>(payload.size() - totalWritten);
            if (!::WriteFile(pipe.get(), payload.data() + totalWritten, remaining, &written, nullptr) ||
                written == 0) {
                return core::Result<IpcResponse>::make_err(
                    core::ErrorCode::IpcError, "Failed to write IPC request to pipe");
            }
            totalWritten += written;
        }

        std::string responseLine;
        responseLine.reserve(512);
        while (responseLine.size() < kMaxIpcMessageBytes) {
            char buf[1024]{};
            DWORD bytesRead = 0;
            if (!::ReadFile(pipe.get(), buf, sizeof(buf), &bytesRead, nullptr)) {
                const DWORD err = ::GetLastError();
                if (err == ERROR_BROKEN_PIPE && !responseLine.empty()) {
                    break;
                }
                return core::Result<IpcResponse>::make_err(
                    core::ErrorCode::IpcError, "Failed to read IPC response from pipe");
            }
            if (bytesRead == 0) {
                break;
            }
            responseLine.append(buf, bytesRead);
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
    return std::make_unique<WinPipeServer>(std::move(endpoint));
}

std::unique_ptr<IpcClient> create_ipc_client(std::string endpoint) {
    return std::make_unique<WinPipeClient>(std::move(endpoint));
}

std::string default_ipc_endpoint() {
    return R"(\\.\pipe\shine_recorder_v1)";
}

} // namespace shine::ipc
