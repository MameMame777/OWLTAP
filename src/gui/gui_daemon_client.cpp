#include "gui_daemon_client.h"

#include "../mcp/json_rpc.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace jtag::gui {

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------
#ifdef _WIN32
using SocketHandle = SOCKET;
static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
static void closeSocket(SocketHandle s) { ::closesocket(s); }
#else
using SocketHandle = int;
static constexpr SocketHandle kInvalidSocket = -1;
static void closeSocket(SocketHandle s) { ::close(s); }
#endif

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct GuiDaemonClient::Impl {
    SocketHandle              sock{kInvalidSocket};
    std::atomic<bool>         connected{false};

    std::thread               reader_thread;
    std::atomic<bool>         stop_flag{false};

    std::mutex                send_mu;      // serialise writes to socket
    std::mutex                pending_mu;
    std::unordered_map<int, std::promise<nlohmann::json>> pending;

    std::atomic<int>          next_id{1};

#ifdef _WIN32
    bool wsa_init{false};
#endif

    void failAllPending(const std::string& reason) {
        std::lock_guard<std::mutex> lk(pending_mu);
        for (auto& [id, p] : pending) {
            try {
                p.set_exception(std::make_exception_ptr(
                    std::runtime_error(reason)));
            } catch (...) {}
        }
        pending.clear();
    }
};

// ---------------------------------------------------------------------------
// Ctor / dtor
// ---------------------------------------------------------------------------
GuiDaemonClient::GuiDaemonClient() : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0) {
        impl_->wsa_init = true;
    }
#endif
}

GuiDaemonClient::~GuiDaemonClient() {
    disconnect();
#ifdef _WIN32
    if (impl_->wsa_init) {
        WSACleanup();
    }
#endif
}

// ---------------------------------------------------------------------------
// connect / disconnect / isConnected
// ---------------------------------------------------------------------------
std::string GuiDaemonClient::connect(uint16_t port, int timeout_ms) {
    if (impl_->connected.load()) disconnect();

    SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) {
        return "socket() failed";
    }

    // Set a connect timeout via SO_RCVTIMEO / SO_SNDTIMEO after connect.
    // For the connect() call itself we use a non-blocking approach.
#ifdef _WIN32
    // Set socket to non-blocking for the connect attempt.
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            closeSocket(s);
            return "connect() failed (immediate error)";
        }
        // Wait for the socket to become writable.
        fd_set wfds{};
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        TIMEVAL tv{};
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        rc = ::select(0, nullptr, &wfds, nullptr, &tv);
        if (rc <= 0) {
            closeSocket(s);
            return "connect() timed out";
        }
        // Verify no error on the socket.
        int err2  = 0;
        int optlen = sizeof(err2);
        getsockopt(s, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&err2), &optlen);
        if (err2 != 0) {
            closeSocket(s);
            return "connect() failed (async error)";
        }
    }
    // Restore blocking mode.
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);

    // Set receive timeout for subsequent reads.
    DWORD recv_to = static_cast<DWORD>(timeout_ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_to), sizeof(recv_to));

#else   // POSIX
    // Set non-blocking for connect.
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        closeSocket(s);
        return "connect() failed";
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    rc = ::select(s + 1, nullptr, &wfds, nullptr, &tv);
    if (rc <= 0) {
        closeSocket(s);
        return "connect() timed out";
    }
    int err2 = 0;
    socklen_t optlen = sizeof(err2);
    getsockopt(s, SOL_SOCKET, SO_ERROR, &err2, &optlen);
    if (err2 != 0) {
        closeSocket(s);
        return "connect() failed (async error)";
    }

    // Restore blocking mode.
    fcntl(s, F_SETFL, flags);

    // Set receive timeout.
    timeval rtv{};
    rtv.tv_sec  = timeout_ms / 1000;
    rtv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
#endif

    impl_->sock = s;
    impl_->stop_flag.store(false);
    impl_->connected.store(true);

    impl_->reader_thread = std::thread(&GuiDaemonClient::readerLoop, this);

    return "";  // success
}

void GuiDaemonClient::disconnect() {
    if (!impl_->connected.load() && impl_->sock == kInvalidSocket) return;

    impl_->stop_flag.store(true);
    impl_->connected.store(false);

    if (impl_->sock != kInvalidSocket) {
#ifdef _WIN32
        ::shutdown(impl_->sock, SD_BOTH);
#else
        ::shutdown(impl_->sock, SHUT_RDWR);
#endif
        closeSocket(impl_->sock);
        impl_->sock = kInvalidSocket;
    }

    if (impl_->reader_thread.joinable()) {
        impl_->reader_thread.join();
    }

    impl_->failAllPending("disconnected");
}

bool GuiDaemonClient::isConnected() const {
    return impl_->connected.load();
}

// ---------------------------------------------------------------------------
// Reader thread
// ---------------------------------------------------------------------------
void GuiDaemonClient::readerLoop() {
    std::string buf;
    buf.reserve(8192);
    char tmp[4096];
    size_t pos = 0;

    while (!impl_->stop_flag.load()) {
        // recv blocks until data or timeout (SO_RCVTIMEO set in connect()).
#ifdef _WIN32
        int n = ::recv(impl_->sock, tmp, static_cast<int>(sizeof(tmp)), 0);
#else
        ssize_t n = ::recv(impl_->sock, tmp, sizeof(tmp), 0);
#endif
        if (n <= 0) {
            // Distinguish a timeout (SO_RCVTIMEO) from a real disconnect.
            // On timeout we simply loop again; only break on actual close/error.
#ifdef _WIN32
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) continue;
#else
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
#endif
            // Connection closed or hard error — stop the reader.
            break;
        }

        buf.append(tmp, static_cast<size_t>(n));

        // Parse all complete messages in the buffer.
        while (pos < buf.size()) {
            auto msg = mcp::tryReadMessage(buf, pos);
            if (!msg) break;

            if (msg->is_null()) continue;  // malformed frame, skip

            // Match by integer id.
            if (msg->contains("id") && (*msg)["id"].is_number_integer()) {
                const int id = (*msg)["id"].get<int>();
                std::lock_guard<std::mutex> lk(impl_->pending_mu);
                auto it = impl_->pending.find(id);
                if (it != impl_->pending.end()) {
                    it->second.set_value(std::move(*msg));
                    impl_->pending.erase(it);
                }
            }
        }

        // Discard consumed prefix.
        if (pos > 0) {
            buf.erase(0, pos);
            pos = 0;
        }
    }

    impl_->connected.store(false);
    impl_->failAllPending("connection closed by server");
}

// ---------------------------------------------------------------------------
// call()
// ---------------------------------------------------------------------------
nlohmann::json GuiDaemonClient::call(const std::string& method,
                                      const nlohmann::json& params,
                                      int timeout_ms) {
    if (!impl_->connected.load()) {
        throw std::runtime_error("GuiDaemonClient: not connected");
    }

    const int id = impl_->next_id.fetch_add(1, std::memory_order_relaxed);

    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"method",  method},
        {"params",  params},
    };

    // Register promise before sending to avoid race with a fast response.
    std::future<nlohmann::json> future;
    {
        std::lock_guard<std::mutex> lk(impl_->pending_mu);
        auto& promise = impl_->pending[id];
        future = promise.get_future();
    }

    // Send (serialised to prevent interleaving).
    {
        const std::string frame = mcp::frameMessage(req);
        std::lock_guard<std::mutex> lk(impl_->send_mu);
        const char* data   = frame.data();
        size_t      remain = frame.size();
        while (remain > 0) {
#ifdef _WIN32
            int sent = ::send(impl_->sock, data,
                              static_cast<int>(remain), 0);
            if (sent == SOCKET_ERROR) {
#else
            ssize_t sent = ::send(impl_->sock, data, remain, 0);
            if (sent < 0) {
#endif
                std::lock_guard<std::mutex> lk2(impl_->pending_mu);
                impl_->pending.erase(id);
                throw std::runtime_error("send() failed: " + method);
            }
            data   += sent;
            remain -= static_cast<size_t>(sent);
        }
    }

    // Wait for response.
    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) ==
        std::future_status::timeout) {
        std::lock_guard<std::mutex> lk(impl_->pending_mu);
        impl_->pending.erase(id);
        throw std::runtime_error("timeout waiting for " + method);
    }

    nlohmann::json resp = future.get();

    // Unwrap JSON-RPC error.
    if (resp.contains("error")) {
        const auto& e = resp["error"];
        const std::string msg = e.value("message", "rpc error");
        throw std::runtime_error(msg);
    }

    return resp.value("result", nlohmann::json::object());
}

// ---------------------------------------------------------------------------
// Public RPC helpers
// ---------------------------------------------------------------------------
nlohmann::json GuiDaemonClient::daemonStatus(int timeout_ms) {
    return call("daemon/status", nlohmann::json::object(), timeout_ms);
}

nlohmann::json GuiDaemonClient::detectDevices(int timeout_ms) {
    return call("hardware/detect_devices", nlohmann::json::object(), timeout_ms);
}

nlohmann::json GuiDaemonClient::loadBsdl(int device_index,
                                          const std::string& bsdl_path,
                                          int timeout_ms) {
    return call("hardware/load_bsdl",
                {{"device_index", device_index}, {"bsdl_path", bsdl_path}},
                timeout_ms);
}

nlohmann::json GuiDaemonClient::listPins(int device_index, int timeout_ms) {
    return call("hardware/list_pins",
                {{"device_index", device_index}},
                timeout_ms);
}

nlohmann::json GuiDaemonClient::readPin(int device_index,
                                         const std::string& pin_name,
                                         int timeout_ms) {
    return call("hardware/read_pin",
                {{"device_index", device_index}, {"pin_name", pin_name}},
                timeout_ms);
}

// ---------------------------------------------------------------------------
// Phase 4: capture and ILA helpers
// ---------------------------------------------------------------------------

nlohmann::json GuiDaemonClient::captureStart(int device_index,
                                              int buffer_depth,
                                              int interval_us,
                                              const std::string& trigger_mode,
                                              const std::vector<std::string>& pin_filter,
                                              int timeout_ms) {
    nlohmann::json params = {
        {"device_index", device_index},
        {"buffer_depth",  buffer_depth},
        {"interval_us",   interval_us},
        {"trigger_mode",  trigger_mode}
    };
    if (!pin_filter.empty()) {
        params["pin_filter"] = pin_filter;
    }
    return call("capture/start", params, timeout_ms);
}

nlohmann::json GuiDaemonClient::captureStop(const std::string& job_id,
                                             int timeout_ms) {
    return call("capture/stop", {{"job_id", job_id}}, timeout_ms);
}

nlohmann::json GuiDaemonClient::captureGetSamples(const std::string& job_id,
                                                   int timeout_ms) {
    return call("capture/get_samples", {{"job_id", job_id}}, timeout_ms);
}

nlohmann::json GuiDaemonClient::jobPoll(const std::string& job_id,
                                         int timeout_ms) {
    return call("job/poll", {{"job_id", job_id}}, timeout_ms);
}

nlohmann::json GuiDaemonClient::ilaStatus(int device_index, bool use_bscane,
                                           int timeout_ms) {
    return call("ila/status",
                {{"device_index", device_index}, {"use_bscane", use_bscane}},
                timeout_ms);
}

// ---------------------------------------------------------------------------
// Phase 5+: ILA high-level daemon operations
// ---------------------------------------------------------------------------

nlohmann::json GuiDaemonClient::ilaProbe(int device_index, bool use_bscane,
                                          int timeout_ms) {
    return call("ila/probe",
                {{"device_index", device_index}, {"use_bscane", use_bscane}},
                timeout_ms);
}

nlohmann::json GuiDaemonClient::ilaArm(int device_index, bool use_bscane,
                                        uint32_t mask, uint32_t value,
                                        uint32_t rise_mask, uint32_t fall_mask,
                                        uint32_t mask2, uint32_t val2,
                                        bool or_mode, int pre_samples,
                                        int timeout_ms) {
    return call("ila/arm",
                {{"device_index", device_index}, {"use_bscane", use_bscane},
                 {"mask",  mask},  {"value",     value},
                 {"rise_mask", rise_mask}, {"fall_mask", fall_mask},
                 {"mask2", mask2}, {"val2",      val2},
                 {"or_mode", or_mode}, {"pre_samples", pre_samples}},
                timeout_ms);
}

nlohmann::json GuiDaemonClient::ilaStop(int device_index, bool use_bscane,
                                         int timeout_ms) {
    return call("ila/stop",
                {{"device_index", device_index}, {"use_bscane", use_bscane}},
                timeout_ms);
}

nlohmann::json GuiDaemonClient::ilaForce(int device_index, bool use_bscane,
                                          int timeout_ms) {
    return call("ila/force_trigger",
                {{"device_index", device_index}, {"use_bscane", use_bscane}},
                timeout_ms);
}

nlohmann::json GuiDaemonClient::ilaReset(int device_index, bool use_bscane,
                                          int timeout_ms) {
    return call("ila/reset",
                {{"device_index", device_index}, {"use_bscane", use_bscane}},
                timeout_ms);
}

nlohmann::json GuiDaemonClient::ilaReadSamples(int device_index, bool use_bscane,
                                                int timeout_ms) {
    return call("ila/read_samples",
                {{"device_index", device_index}, {"use_bscane", use_bscane}},
                timeout_ms);
}

// ---------------------------------------------------------------------------
// Phase 5: BSR sample-all and script execution
// ---------------------------------------------------------------------------

nlohmann::json GuiDaemonClient::sampleBsr(int device_index, int timeout_ms) {
    return call("hardware/sample_bsr", {{"device_index", device_index}},
                timeout_ms);
}

nlohmann::json GuiDaemonClient::runScript(int device_index,
                                           const std::string& script_text,
                                           int timeout_ms) {
    return call("script/run",
                {{"device_index", device_index},
                 {"script_text",  script_text}},
                timeout_ms);
}

nlohmann::json GuiDaemonClient::programPl(int device_index,
                                           const std::string& bitstream_path,
                                           int timeout_ms) {
    return call("hardware/program_pl",
                {{"device_index",   device_index},
                 {"bitstream_path", bitstream_path}},
                timeout_ms);
}

}  // namespace jtag::gui
