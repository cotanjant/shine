#pragma once

#include "shine/core/result.hpp"
#include "shine/ipc/protocol.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace shine::ipc {

using IpcHandler = std::function<IpcResponse(const IpcRequest&)>;

class IpcServer {
public:
    virtual ~IpcServer() = default;

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) = delete;
    IpcServer& operator=(IpcServer&&) = delete;

    [[nodiscard]] virtual core::Result<void> start(IpcHandler handler) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual const std::string& endpoint() const noexcept = 0;

protected:
    IpcServer() = default;
};

class IpcClient {
public:
    virtual ~IpcClient() = default;

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;
    IpcClient(IpcClient&&) = delete;
    IpcClient& operator=(IpcClient&&) = delete;

    [[nodiscard]] virtual core::Result<IpcResponse> send(const IpcRequest& req, std::uint32_t timeoutMs = 5000) = 0;
    [[nodiscard]] virtual const std::string& endpoint() const noexcept = 0;

protected:
    IpcClient() = default;
};

[[nodiscard]] std::unique_ptr<IpcServer> create_ipc_server(std::string endpoint);
[[nodiscard]] std::unique_ptr<IpcClient> create_ipc_client(std::string endpoint);
[[nodiscard]] std::string default_ipc_endpoint();

} // namespace shine::ipc
