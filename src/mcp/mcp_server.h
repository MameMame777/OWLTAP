#pragma once

#include "mcp_transport.h"
#include "tool_registry.h"

#include <memory>
#include <string>

namespace jtag::mcp {

class ExecutorBridge;  // defined in Phase 2

struct ServerInfo {
    std::string name;
    std::string version;
};

// MCP JSON-RPC 2.0 server.
//
// Lifecycle:
//   McpServer srv(info);
//   srv.setTransport(std::make_unique<StdioTransport>());
//   srv.registry().registerTool(...);  // register tools before start
//   srv.start();
//   ...
//   srv.stop();
//
// The server is single-instance per transport; the transport's reader thread
// calls onMessage() which dispatches synchronously.  For async tools, Phase 2
// will add setExecutorBridge() so that handlers can submit hardware jobs.
class McpServer {
public:
    explicit McpServer(ServerInfo info);
    ~McpServer();

    // Must be called before start().
    void setTransport(std::unique_ptr<ITransport> transport);

    // Optional; enables hardware job submission from tool handlers (Phase 2).
    void setExecutorBridge(ExecutorBridge* bridge);

    ToolRegistry& registry();
    const ToolRegistry& registry() const;

    // Start the transport and begin processing messages.
    void start();

    // Stop the transport and wait for all threads.
    void stop();

    bool isRunning() const;

private:
    void onMessage(nlohmann::json msg);
    nlohmann::json dispatch(const nlohmann::json& req_j);
    nlohmann::json handleInitialize(const nlohmann::json& params);
    nlohmann::json handleToolsList(const nlohmann::json& params);
    nlohmann::json handleToolsCall(const nlohmann::json& params);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace jtag::mcp
