#ifdef _WIN32
// winsock2.h must precede windows.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static inline void closeSocket(socket_t s) { closesocket(s); }
static inline int lastSocketError() { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static inline void closeSocket(socket_t s) { close(s); }
static inline int lastSocketError() { return errno; }
#endif

#include "mcp_transport.h"
#include "json_rpc.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace jtag::mcp {

// ---------------------------------------------------------------------------
// StdioTransport
// ---------------------------------------------------------------------------

struct StdioTransport::Impl {
    ReceiveHandler handler;
    std::atomic<bool> stop_flag{false};
    std::thread reader;
    std::mutex write_mu;
};

StdioTransport::StdioTransport() : impl_(std::make_unique<Impl>()) {}

StdioTransport::~StdioTransport() {
    stop();
}

void StdioTransport::setReceiveHandler(ReceiveHandler h) {
    impl_->handler = std::move(h);
}

void StdioTransport::start() {
#ifdef _WIN32
    // Prevent CRLF translation on stdin/stdout.
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    impl_->stop_flag.store(false);
    impl_->reader = std::thread([this] {
        std::string buf;
        buf.reserve(4096);
        char chunk[4096];
        size_t pos = 0;
        while (!impl_->stop_flag.load()) {
            const int n = static_cast<int>(fread(chunk, 1, sizeof(chunk), stdin));
            if (n <= 0) break;  // EOF or error
            buf.append(chunk, static_cast<size_t>(n));
            while (pos < buf.size()) {
                auto msg = tryReadMessage(buf, pos);
                if (!msg) break;
                if (!msg->is_null() && impl_->handler) {
                    impl_->handler(std::move(*msg));
                }
            }
            // Discard consumed bytes to keep the buffer small.
            if (pos > 0) {
                buf.erase(0, pos);
                pos = 0;
            }
        }
    });
}

void StdioTransport::stop() {
    impl_->stop_flag.store(true);
    if (impl_->reader.joinable()) {
        // We cannot interrupt a blocking fread() portably; on Windows,
        // posting a dummy byte to stdin is awkward. Accept that the thread
        // will exit on its own once the MCP client closes stdin.
        impl_->reader.detach();
    }
}

bool StdioTransport::send(const nlohmann::json& msg) {
    if (impl_->stop_flag.load()) return false;
    const std::string frame = frameMessage(msg);
    std::lock_guard<std::mutex> lk(impl_->write_mu);
    const size_t written = fwrite(frame.data(), 1, frame.size(), stdout);
    fflush(stdout);
    return written == frame.size();
}

// ---------------------------------------------------------------------------
// TcpTransport
// ---------------------------------------------------------------------------

struct TcpTransport::Impl {
    uint16_t port;
    ReceiveHandler handler;
    std::atomic<bool> stop_flag{false};
    socket_t server_sock{kInvalidSocket};
    socket_t client_sock{kInvalidSocket};
    std::thread accept_thread;
    std::mutex write_mu;
#ifdef _WIN32
    bool wsa_ok{false};
#endif
};

TcpTransport::TcpTransport(uint16_t port) : impl_(std::make_unique<Impl>()) {
    impl_->port = port;
#ifdef _WIN32
    WSADATA wsa;
    impl_->wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
}

TcpTransport::~TcpTransport() {
    stop();
#ifdef _WIN32
    if (impl_->wsa_ok) {
        WSACleanup();
        impl_->wsa_ok = false;
    }
#endif
}

void TcpTransport::setReceiveHandler(ReceiveHandler h) {
    impl_->handler = std::move(h);
}

static void tcpReaderLoop(socket_t sock, std::atomic<bool>& stop_flag,
                           ITransport::ReceiveHandler& handler) {
    std::string buf;
    buf.reserve(4096);
    char chunk[4096];
    size_t pos = 0;
    while (!stop_flag.load()) {
        const int n = static_cast<int>(recv(sock, chunk, sizeof(chunk), 0));
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
        while (pos < buf.size()) {
            auto msg = tryReadMessage(buf, pos);
            if (!msg) break;
            if (!msg->is_null() && handler) {
                handler(std::move(*msg));
            }
        }
        if (pos > 0) {
            buf.erase(0, pos);
            pos = 0;
        }
    }
}

void TcpTransport::start() {
    impl_->stop_flag.store(false);

    impl_->server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->server_sock == kInvalidSocket) return;

    // Allow port reuse to avoid TIME_WAIT issues during rapid restarts.
    int opt = 1;
    setsockopt(impl_->server_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(impl_->port);
    // Bind only to loopback — never expose on a network interface.
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(impl_->server_sock, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        closeSocket(impl_->server_sock);
        impl_->server_sock = kInvalidSocket;
        return;
    }

    // Read back the actual port (useful when port==0).
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (getsockname(impl_->server_sock, reinterpret_cast<sockaddr*>(&bound),
                    &bound_len) == 0) {
        impl_->port = ntohs(bound.sin_port);
    }

    listen(impl_->server_sock, 1);

    impl_->accept_thread = std::thread([this] {
        while (!impl_->stop_flag.load()) {
            socket_t client = accept(impl_->server_sock, nullptr, nullptr);
            if (client == kInvalidSocket) break;
            impl_->client_sock = client;
            tcpReaderLoop(client, impl_->stop_flag, impl_->handler);
            {
                std::lock_guard<std::mutex> lk(impl_->write_mu);
                closeSocket(impl_->client_sock);
                impl_->client_sock = kInvalidSocket;
            }
        }
    });
}

void TcpTransport::stop() {
    impl_->stop_flag.store(true);
    if (impl_->server_sock != kInvalidSocket) {
        closeSocket(impl_->server_sock);
        impl_->server_sock = kInvalidSocket;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->write_mu);
        if (impl_->client_sock != kInvalidSocket) {
            closeSocket(impl_->client_sock);
            impl_->client_sock = kInvalidSocket;
        }
    }
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
}

bool TcpTransport::send(const nlohmann::json& msg) {
    std::lock_guard<std::mutex> lk(impl_->write_mu);
    if (impl_->client_sock == kInvalidSocket) return false;
    const std::string frame = frameMessage(msg);
    size_t sent = 0;
    while (sent < frame.size()) {
        const int n = static_cast<int>(
            ::send(impl_->client_sock,
                   frame.data() + sent,
                   static_cast<int>(frame.size() - sent), 0));
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

uint16_t TcpTransport::port() const { return impl_->port; }

}  // namespace jtag::mcp
