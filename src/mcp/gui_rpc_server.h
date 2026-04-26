#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace jtag::mcp {

/// Minimal JSON-RPC 2.0 server for the GUI endpoint.
///
/// Uses TcpTransport (TCP loopback, one client at a time).
/// Does NOT implement the MCP protocol envelope; callers send plain
/// JSON-RPC 2.0 request objects and receive plain result/error objects.
class GuiRpcServer {
public:
    using Handler = std::function<nlohmann::json(const nlohmann::json& params)>;

    /// @param port  TCP port to listen on. 0 = OS-chosen ephemeral port.
    explicit GuiRpcServer(uint16_t port);
    ~GuiRpcServer();

    // Non-copyable, non-movable.
    GuiRpcServer(const GuiRpcServer&)            = delete;
    GuiRpcServer& operator=(const GuiRpcServer&) = delete;

    /// Register a method handler. Call before start().
    void registerMethod(std::string method, Handler handler);

    /// Bind the TCP socket and start the accept/dispatch loop.
    void start();

    /// Stop accepting new connections and shut down the current session.
    void stop();

    /// True after start() succeeds (socket is bound and listening).
    bool isListening() const;

    /// True while the dispatch loop thread is alive.
    bool isRunning() const;

    /// Actual bound port (valid after start()).
    uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace jtag::mcp
