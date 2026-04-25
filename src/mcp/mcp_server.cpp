#include "mcp_server.h"
#include "json_rpc.h"

#include <atomic>
#include <stdexcept>

namespace jtag::mcp {

// MCP protocol version this server implements.
static constexpr const char* kProtocolVersion = "2024-11-05";

struct McpServer::Impl {
    ServerInfo info;
    std::unique_ptr<ITransport> transport;
    ExecutorBridge* bridge{nullptr};
    ToolRegistry registry;
    std::atomic<bool> running{false};
    std::atomic<bool> initialized{false};
};

McpServer::McpServer(ServerInfo info) : impl_(std::make_unique<Impl>()) {
    impl_->info = std::move(info);
}

McpServer::~McpServer() {
    stop();
}

void McpServer::setTransport(std::unique_ptr<ITransport> t) {
    impl_->transport = std::move(t);
    impl_->transport->setReceiveHandler(
        [this](nlohmann::json msg) { onMessage(std::move(msg)); });
}

void McpServer::setExecutorBridge(ExecutorBridge* bridge) {
    impl_->bridge = bridge;
}

ToolRegistry& McpServer::registry() { return impl_->registry; }
const ToolRegistry& McpServer::registry() const { return impl_->registry; }

void McpServer::start() {
    if (!impl_->transport) return;
    impl_->running.store(true);
    impl_->transport->start();
}

void McpServer::stop() {
    impl_->running.store(false);
    if (impl_->transport) {
        impl_->transport->stop();
    }
}

bool McpServer::isRunning() const { return impl_->running.load(); }

// ---------------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------------

void McpServer::onMessage(nlohmann::json msg) {
    if (!msg.is_object()) return;

    nlohmann::json response = dispatch(msg);

    // Notifications have a null id; do not send a response for them.
    if (!response.is_null()) {
        impl_->transport->send(response);
    }
}

nlohmann::json McpServer::dispatch(const nlohmann::json& req_j) {
    std::string parse_err;
    auto req_opt = parseRequest(req_j, parse_err);
    if (!req_opt) {
        // Return parse error only if we can extract an id.
        nlohmann::json id = req_j.contains("id") ? req_j["id"] : nlohmann::json(nullptr);
        return makeError(id, RpcErrorCode::kInvalidRequest, parse_err);
    }
    const RpcRequest& req = *req_opt;

    // Notifications: dispatch but never reply.
    if (req.is_notification()) {
        // Handle notifications/initialized silently.
        return nlohmann::json(nullptr);
    }

    try {
        if (req.method == "initialize") {
            return makeResult(req.id, handleInitialize(req.params));
        }
        if (req.method == "tools/list") {
            if (!impl_->initialized.load()) {
                return makeError(req.id, RpcErrorCode::kInvalidRequest,
                                 "server not yet initialized");
            }
            return makeResult(req.id, handleToolsList(req.params));
        }
        if (req.method == "tools/call") {
            if (!impl_->initialized.load()) {
                return makeError(req.id, RpcErrorCode::kInvalidRequest,
                                 "server not yet initialized");
            }
            return makeResult(req.id, handleToolsCall(req.params));
        }
        if (req.method == "shutdown") {
            // Graceful shutdown: respond then stop.
            impl_->running.store(false);
            return makeResult(req.id, nlohmann::json(nullptr));
        }
        return makeError(req.id, RpcErrorCode::kMethodNotFound,
                         "method not found: " + req.method);
    } catch (const std::invalid_argument& e) {
        return makeError(req.id, RpcErrorCode::kInvalidParams, e.what());
    } catch (const std::runtime_error& e) {
        return makeError(req.id, RpcErrorCode::kInternalError, e.what());
    } catch (...) {
        return makeError(req.id, RpcErrorCode::kInternalError,
                         "unexpected error");
    }
}

// ---------------------------------------------------------------------------
// Method handlers
// ---------------------------------------------------------------------------

nlohmann::json McpServer::handleInitialize(const nlohmann::json& /*params*/) {
    impl_->initialized.store(true);
    return {
        {"protocolVersion", kProtocolVersion},
        {"capabilities",    {{"tools", nlohmann::json::object()}}},
        {"serverInfo",      {{"name",    impl_->info.name},
                             {"version", impl_->info.version}}},
    };
}

nlohmann::json McpServer::handleToolsList(const nlohmann::json& /*params*/) {
    return impl_->registry.listTools();
}

nlohmann::json McpServer::handleToolsCall(const nlohmann::json& params) {
    if (!params.is_object()) {
        throw std::invalid_argument("tools/call params must be an object");
    }
    const auto name_it = params.find("name");
    if (name_it == params.end() || !name_it->is_string()) {
        throw std::invalid_argument("tools/call: missing string 'name'");
    }
    const std::string& tool_name = name_it->get_ref<const std::string&>();

    if (!impl_->registry.hasTool(tool_name)) {
        throw std::runtime_error("unknown tool: " + tool_name);
    }

    const nlohmann::json& tool_params =
        params.contains("arguments") ? params["arguments"] : nlohmann::json(nullptr);

    return impl_->registry.callTool(tool_name, tool_params);
}

}  // namespace jtag::mcp
