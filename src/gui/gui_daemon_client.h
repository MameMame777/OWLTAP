#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace jtag::gui {

/// Synchronous JSON-RPC 2.0 TCP client for the daemon GUI endpoint.
///
/// All public methods are thread-safe. The `call()` helpers block the caller
/// until a response arrives or the timeout expires.
///
/// Usage:
///   GuiDaemonClient c;
///   std::string err = c.connect(gui_port);
///   if (!err.empty()) { /* handle */ }
///   auto devs = c.detectDevices();
///   c.disconnect();
class GuiDaemonClient {
public:
    GuiDaemonClient();
    ~GuiDaemonClient();

    // Non-copyable.
    GuiDaemonClient(const GuiDaemonClient&)            = delete;
    GuiDaemonClient& operator=(const GuiDaemonClient&) = delete;

    /// Connect to 127.0.0.1:port. Returns "" on success, error string on fail.
    std::string connect(uint16_t port, int timeout_ms = 3000);

    /// Disconnect and stop reader thread.
    void disconnect();

    bool isConnected() const;

    // -----------------------------------------------------------------------
    // RPC methods — throw std::runtime_error on network error or RPC error.
    // -----------------------------------------------------------------------
    nlohmann::json daemonStatus(int timeout_ms = 2000);
    nlohmann::json detectDevices(int timeout_ms = 5000);
    nlohmann::json loadBsdl(int device_index, const std::string& bsdl_path,
                             int timeout_ms = 5000);
    nlohmann::json listPins(int device_index, int timeout_ms = 5000);
    nlohmann::json readPin(int device_index, const std::string& pin_name,
                           int timeout_ms = 5000);

private:
    /// Send a JSON-RPC request and block until the matching response arrives.
    nlohmann::json call(const std::string& method,
                        const nlohmann::json& params,
                        int timeout_ms);

    /// Background thread that feeds received bytes into the framing parser.
    void readerLoop();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace jtag::gui
