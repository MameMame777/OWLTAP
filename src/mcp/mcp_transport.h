#pragma once

#include <functional>
#include <memory>

#include <nlohmann/json.hpp>

namespace jtag::mcp {

// Transport interface.
// send() is thread-safe.
// start() spawns internal I/O thread(s); stop() terminates them.
// The receive handler is called from the transport's reader thread.
class ITransport {
public:
    using ReceiveHandler = std::function<void(nlohmann::json)>;

    virtual ~ITransport() = default;

    // Register the callback invoked for each received JSON-RPC message.
    // Must be called before start().
    virtual void setReceiveHandler(ReceiveHandler h) = 0;

    // Start accepting / reading messages.
    virtual void start() = 0;

    // Signal stop and block until all internal threads have joined.
    virtual void stop() = 0;

    // Send a JSON message to the connected peer.
    // Returns false if the transport is not connected or has been stopped.
    virtual bool send(const nlohmann::json& msg) = 0;
};

// Reads from stdin with Content-Length framing (plus newline-delimited fallback)
// and writes to stdout. On Windows, sets binary mode on both streams.
// The reader thread terminates when stdin reaches EOF or stop() is called.
class StdioTransport : public ITransport {
public:
    StdioTransport();
    ~StdioTransport() override;

    void setReceiveHandler(ReceiveHandler h) override;
    void start() override;
    void stop() override;
    bool send(const nlohmann::json& msg) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Binds a TCP server socket to 127.0.0.1:<port> and accepts one client at a
// time. The reader thread processes messages from that client.
// A new connection is accepted after the previous one is closed.
class TcpTransport : public ITransport {
public:
    explicit TcpTransport(uint16_t port);
    ~TcpTransport() override;

    void setReceiveHandler(ReceiveHandler h) override;
    void start() override;
    void stop() override;
    bool send(const nlohmann::json& msg) override;

    // Actual bound port (useful if 0 was passed to let the OS choose).
    uint16_t port() const;

    // True after start() successfully binds and listens.
    bool isListening() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace jtag::mcp
