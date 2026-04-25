#include "tool_registry.h"

#include <stdexcept>
#include <string_view>

namespace jtag::mcp {

// ---------------------------------------------------------------------------
// Schema validation (subset of JSON Schema draft-07)
// ---------------------------------------------------------------------------

// Map a JSON Schema type name to a predicate on nlohmann::json.
static bool checkType(std::string_view type_name, const nlohmann::json& v) {
    if (type_name == "string")  return v.is_string();
    if (type_name == "integer") return v.is_number_integer();
    if (type_name == "number")  return v.is_number();
    if (type_name == "boolean") return v.is_boolean();
    if (type_name == "null")    return v.is_null();
    if (type_name == "array")   return v.is_array();
    if (type_name == "object")  return v.is_object();
    return true;  // unknown type: pass through
}

std::optional<std::string> validateParams(const nlohmann::json& schema,
                                           const nlohmann::json& params) {
    if (!schema.is_object()) return std::nullopt;

    // Top-level type must be "object".
    if (auto it = schema.find("type"); it != schema.end()) {
        if (it->is_string() && *it != "object") {
            if (!checkType(it->get<std::string>(), params)) {
                return "params: expected type " + it->get<std::string>();
            }
        }
    }

    if (!params.is_object()) return "params must be an object";

    // Check required fields.
    if (auto req_it = schema.find("required"); req_it != schema.end() &&
                                               req_it->is_array()) {
        for (const auto& field : *req_it) {
            if (!field.is_string()) continue;
            const auto& name = field.get_ref<const std::string&>();
            if (!params.contains(name)) {
                return "missing required parameter: " + name;
            }
        }
    }

    // Check property types.
    if (auto props_it = schema.find("properties");
        props_it != schema.end() && props_it->is_object()) {
        for (const auto& [prop_name, prop_schema] : props_it->items()) {
            const auto val_it = params.find(prop_name);
            if (val_it == params.end()) continue;  // not present; required already checked

            if (prop_schema.is_object()) {
                if (auto type_it = prop_schema.find("type");
                    type_it != prop_schema.end() && type_it->is_string()) {
                    if (!checkType(type_it->get<std::string>(), *val_it)) {
                        return "parameter '" + prop_name + "': expected type " +
                               type_it->get<std::string>();
                    }
                }
                // Recurse into nested objects.
                if (val_it->is_object()) {
                    auto nested = validateParams(prop_schema, *val_it);
                    if (nested) return *nested;
                }
            }
        }
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

void ToolRegistry::registerTool(ToolSpec spec) {
    const auto it = index_.find(spec.name);
    if (it != index_.end()) {
        tools_[it->second] = std::move(spec);
    } else {
        index_[spec.name] = tools_.size();
        tools_.push_back(std::move(spec));
    }
}

nlohmann::json ToolRegistry::listTools() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tools_) {
        nlohmann::json entry = {
            {"name",        t.name},
            {"description", t.description},
            {"inputSchema", t.input_schema},
        };
        arr.push_back(std::move(entry));
    }
    return {{"tools", std::move(arr)}};
}

nlohmann::json ToolRegistry::callTool(const std::string& name,
                                       const nlohmann::json& params) const {
    const auto it = index_.find(name);
    if (it == index_.end()) {
        throw std::runtime_error("unknown tool: " + name);
    }
    const ToolSpec& spec = tools_[it->second];

    // Validate params against the tool's schema.
    const nlohmann::json effective_params =
        params.is_null() ? nlohmann::json::object() : params;
    if (auto err = validateParams(spec.input_schema, effective_params)) {
        throw std::invalid_argument(*err);
    }

    const nlohmann::json result = spec.handler(effective_params);

    // Wrap as MCP content array.
    return nlohmann::json::array({
        {{"type", "text"}, {"text", result.dump()}},
    });
}

bool ToolRegistry::hasTool(const std::string& name) const {
    return index_.count(name) > 0;
}

}  // namespace jtag::mcp
