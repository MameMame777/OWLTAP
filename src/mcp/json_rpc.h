#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace jtag::mcp {

// Standard JSON-RPC 2.0 error codes.
enum class RpcErrorCode : int {
    kParseError     = -32700,
    kInvalidRequest = -32600,
    kMethodNotFound = -32601,
    kInvalidParams  = -32602,
    kInternalError  = -32603,
    // Server-defined errors (-32000 to -32099).
    kHardwareBusy   = -32000,
    kHardwareError  = -32001,
    kJobNotFound    = -32002,
    kTimeout        = -32003,
};

// A parsed JSON-RPC 2.0 request.
struct RpcRequest {
    nlohmann::json id;      // null for notifications, otherwise string or int
    std::string    method;
    nlohmann::json params;  // null if omitted

    bool is_notification() const { return id.is_null(); }
};

// Try to parse a JSON-RPC request from a JSON object.
// Returns nullopt and sets error_out on failure.
std::optional<RpcRequest> parseRequest(const nlohmann::json& j,
                                        std::string& error_out);

// Build a success response object.
nlohmann::json makeResult(const nlohmann::json& id, nlohmann::json result);

// Build an error response object (raw integer code).
nlohmann::json makeError(const nlohmann::json& id, int code,
                          std::string message,
                          nlohmann::json data = nullptr);

// Build an error response object (typed code).
nlohmann::json makeError(const nlohmann::json& id, RpcErrorCode code,
                          std::string message,
                          nlohmann::json data = nullptr);

// Frame a JSON message for the Content-Length stdio transport:
//   "Content-Length: N\r\n\r\n<body>"
std::string frameMessage(const nlohmann::json& msg);

// Try to read one complete JSON-RPC message from buf, starting at *pos.
// Supports Content-Length framing and newline-delimited fallback.
// On success, advances *pos past the consumed bytes and returns the parsed JSON.
// Returns nullopt if no complete message is available yet.
// Returns a JSON null value if a message was framed but could not be parsed.
std::optional<nlohmann::json> tryReadMessage(const std::string& buf,
                                              size_t& pos);

}  // namespace jtag::mcp
