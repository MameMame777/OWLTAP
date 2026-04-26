#include "gui_rpc_server.h"

#include "json_rpc.h"
#include "mcp_transport.h"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace jtag::mcp {

struct GuiRpcServer::Impl {
    uint16_t port_hint;  // requested port (0 = any)
    std::unordered_map<std::string, Handler> handlers;
    std::unique_ptr<TcpTransport> transport;
    std::atomic<bool> running{false};
};

GuiRpcServer::GuiRpcServer(uint16_t port) : impl_(std::make_unique<Impl>()) {
    impl_->port_hint = port;
}

GuiRpcServer::~GuiRpcServer() { stop(); }

void GuiRpcServer::registerMethod(std::string name, Handler handler) {
    impl_->handlers[std::move(name)] = std::move(handler);
}

void GuiRpcServer::start() {
    impl_->transport = std::make_unique<TcpTransport>(impl_->port_hint);

    impl_->transport->setReceiveHandler([this](nlohmann::json msg) {
        std::string err;
        auto req = parseRequest(msg, err);
        if (!req) {
            impl_->transport->send(
                makeError(nullptr,
                          static_cast<int>(RpcErrorCode::kInvalidRequest),
                          std::move(err)));
            return;
        }

        auto it = impl_->handlers.find(req->method);
        if (it == impl_->handlers.end()) {
            if (!req->is_notification()) {
                impl_->transport->send(
                    makeError(req->id,
                              static_cast<int>(RpcErrorCode::kMethodNotFound),
                              "unknown method: " + req->method));
            }
            return;
        }

        if (!req->is_notification()) {
            const nlohmann::json params = req->params.is_null()
                                              ? nlohmann::json::object()
                                              : req->params;
            try {
                auto result = it->second(params);
                impl_->transport->send(makeResult(req->id, std::move(result)));
            } catch (const std::exception& e) {
                impl_->transport->send(
                    makeError(req->id,
                              static_cast<int>(RpcErrorCode::kInternalError),
                              e.what()));
            }
        }
    });

    impl_->transport->start();
    impl_->running.store(impl_->transport->isListening(),
                         std::memory_order_release);
}

void GuiRpcServer::stop() {
    impl_->running.store(false, std::memory_order_release);
    if (impl_->transport) {
        impl_->transport->stop();
        impl_->transport.reset();
    }
}

bool GuiRpcServer::isListening() const {
    return impl_->transport && impl_->transport->isListening();
}

bool GuiRpcServer::isRunning() const {
    return impl_->running.load(std::memory_order_acquire);
}

uint16_t GuiRpcServer::port() const {
    if (impl_->transport) return impl_->transport->port();
    return impl_->port_hint;
}

}  // namespace jtag::mcp
