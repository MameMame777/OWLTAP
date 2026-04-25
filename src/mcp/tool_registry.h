#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace jtag::mcp {

// Metadata and handler for one MCP tool.
struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json input_schema;  // JSON Schema object (type, properties, required)
    std::function<nlohmann::json(const nlohmann::json& params)> handler;
};

// Validate params against a JSON Schema subset.
// Checks: type=="object", required fields present, property types.
// Returns an error string on failure, nullopt on success.
std::optional<std::string> validateParams(const nlohmann::json& schema,
                                           const nlohmann::json& params);

class ToolRegistry {
public:
    // Register a tool. Replaces any existing registration with the same name.
    void registerTool(ToolSpec spec);

    // Produce the tools/list result payload.
    nlohmann::json listTools() const;

    // Invoke a tool.
    // Returns the content array: [{"type":"text","text":"<json>"}]
    // Throws std::runtime_error on schema validation failure.
    // Throws std::runtime_error if the tool is not found.
    nlohmann::json callTool(const std::string& name,
                             const nlohmann::json& params) const;

    bool hasTool(const std::string& name) const;
    size_t size() const { return tools_.size(); }

private:
    std::vector<ToolSpec> tools_;
    std::unordered_map<std::string, size_t> index_;
};

}  // namespace jtag::mcp
