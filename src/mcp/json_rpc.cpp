#include "json_rpc.h"

#include <string_view>

namespace jtag::mcp {

std::optional<RpcRequest> parseRequest(const nlohmann::json& j,
                                        std::string& error_out) {
    if (!j.is_object()) {
        error_out = "request is not an object";
        return std::nullopt;
    }

    // jsonrpc field must be "2.0".
    auto jrpc_it = j.find("jsonrpc");
    if (jrpc_it == j.end() || *jrpc_it != "2.0") {
        error_out = "missing or invalid jsonrpc field";
        return std::nullopt;
    }

    // method must be a string.
    auto method_it = j.find("method");
    if (method_it == j.end() || !method_it->is_string()) {
        error_out = "missing or non-string method";
        return std::nullopt;
    }

    RpcRequest req;
    req.method = method_it->get<std::string>();
    req.id     = j.contains("id") ? j["id"] : nlohmann::json(nullptr);
    req.params = j.contains("params") ? j["params"] : nlohmann::json(nullptr);

    return req;
}

nlohmann::json makeResult(const nlohmann::json& id, nlohmann::json result) {
    return {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"result",  std::move(result)},
    };
}

nlohmann::json makeError(const nlohmann::json& id, int code,
                          std::string message, nlohmann::json data) {
    nlohmann::json err = {
        {"code",    code},
        {"message", std::move(message)},
    };
    if (!data.is_null()) {
        err["data"] = std::move(data);
    }
    return {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"error",   std::move(err)},
    };
}

nlohmann::json makeError(const nlohmann::json& id, RpcErrorCode code,
                          std::string message, nlohmann::json data) {
    return makeError(id, static_cast<int>(code), std::move(message),
                     std::move(data));
}

std::string frameMessage(const nlohmann::json& msg) {
    const std::string body = msg.dump();
    std::string out;
    out.reserve(body.size() + 32);
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n\r\n";
    out += body;
    return out;
}

// Returns the position just past the first newline in [buf, buf+len), or
// std::string::npos if not found.
static size_t findNewline(const std::string& buf, size_t from) {
    const size_t pos = buf.find('\n', from);
    return pos;
}

std::optional<nlohmann::json> tryReadMessage(const std::string& buf,
                                              size_t& pos) {
    if (pos >= buf.size()) return std::nullopt;

    // Try Content-Length framing first.
    static constexpr std::string_view kHeader = "Content-Length: ";
    if (buf.compare(pos, kHeader.size(), kHeader) == 0) {
        // Parse the length value.
        const size_t val_start = pos + kHeader.size();
        const size_t crlf2 = buf.find("\r\n\r\n", val_start);
        if (crlf2 == std::string::npos) return std::nullopt;

        const std::string len_str = buf.substr(val_start, crlf2 - val_start);
        size_t body_len = 0;
        try {
            body_len = std::stoul(len_str);
        } catch (...) {
            // Skip the malformed header by advancing past the \r\n\r\n.
            pos = crlf2 + 4;
            return nlohmann::json(nullptr);
        }

        const size_t body_start = crlf2 + 4;
        if (buf.size() - body_start < body_len) return std::nullopt;  // incomplete

        const std::string body = buf.substr(body_start, body_len);
        pos = body_start + body_len;

        try {
            return nlohmann::json::parse(body);
        } catch (...) {
            return nlohmann::json(nullptr);
        }
    }

    // Newline-delimited fallback: look for a complete line starting with '{'.
    if (pos < buf.size() && buf[pos] == '{') {
        const size_t nl = findNewline(buf, pos);
        if (nl == std::string::npos) return std::nullopt;

        const std::string line = buf.substr(pos, nl - pos);
        pos = nl + 1;

        try {
            return nlohmann::json::parse(line);
        } catch (...) {
            return nlohmann::json(nullptr);
        }
    }

    // Skip unrecognised leading bytes until we find a recognisable start.
    const size_t cl_pos = buf.find("Content-Length:", pos);
    const size_t brace  = buf.find('{', pos);
    const size_t next   = std::min(cl_pos, brace);
    if (next == std::string::npos) {
        pos = buf.size();
        return std::nullopt;
    }
    pos = next;
    return std::nullopt;
}

}  // namespace jtag::mcp
