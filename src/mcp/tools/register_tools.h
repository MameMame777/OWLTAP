#pragma once

#include "src/mcp/executor_bridge.h"
#include "src/mcp/tool_registry.h"

namespace jtag::mcp::tools {

// Register all 15 MCP hardware tool handlers into the given ToolRegistry.
// The ExecutorBridge is captured by pointer; it must outlive the registry.
void registerHardwareTools(ToolRegistry& registry, ExecutorBridge& bridge);

}  // namespace jtag::mcp::tools
