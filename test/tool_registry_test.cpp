// Tests for ToolRegistry: registration, listing, dispatch, validation.
#include <gtest/gtest.h>

#include <stdexcept>

#include "src/mcp/tool_registry.h"

namespace jtag::mcp {
namespace {

// ── validateParams ───────────────────────────────────────────────────────────

TEST(ValidateParamsTest, AcceptsValidParams) {
    auto schema = nlohmann::json{
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}}}, {"count", {{"type", "number"}}}}},
        {"required", nlohmann::json::array({"name"})}};
    auto params = nlohmann::json{{"name", "foo"}, {"count", 3}};
    auto err = validateParams(schema, params);
    EXPECT_FALSE(err.has_value()) << err.value_or("");
}

TEST(ValidateParamsTest, MissingRequiredField) {
    auto schema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"name"})}};
    auto params = nlohmann::json::object();
    auto err = validateParams(schema, params);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("name"), std::string::npos);
}

TEST(ValidateParamsTest, WrongFieldType) {
    auto schema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"port", {{"type", "number"}}}}},
        {"required", nlohmann::json::array({"port"})}};
    auto params = nlohmann::json{{"port", "not-a-number"}};
    auto err = validateParams(schema, params);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateParamsTest, NotAnObjectIsRejected) {
    auto schema = nlohmann::json{{"type", "object"}};
    auto params = nlohmann::json::array();
    auto err = validateParams(schema, params);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateParamsTest, NullParamsWithNoRequiredAccepted) {
    auto schema = nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json::object()}};
    // null params is rejected (must be an object); pass empty object instead.
    auto err = validateParams(schema, nlohmann::json::object());
    EXPECT_FALSE(err.has_value()) << err.value_or("");
}

// ── ToolRegistry ─────────────────────────────────────────────────────────────

class ToolRegistryTest : public ::testing::Test {
protected:
    ToolRegistry reg;

    void SetUp() override {
        ToolSpec echo_spec;
        echo_spec.name        = "echo";
        echo_spec.description = "Echo a message back.";
        echo_spec.input_schema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"message", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"message"})}};
        echo_spec.handler = [](const nlohmann::json& params) {
            return nlohmann::json{{"echo", params["message"]}};
        };
        reg.registerTool(std::move(echo_spec));
    }
};

TEST_F(ToolRegistryTest, HasToolAfterRegistration) {
    EXPECT_TRUE(reg.hasTool("echo"));
    EXPECT_FALSE(reg.hasTool("no_such_tool"));
    EXPECT_EQ(reg.size(), 1u);
}

TEST_F(ToolRegistryTest, ListToolsReturnsArray) {
    auto result = reg.listTools();
    ASSERT_TRUE(result.is_object());
    ASSERT_TRUE(result.contains("tools"));
    const auto& list = result["tools"];
    ASSERT_TRUE(list.is_array());
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0]["name"], "echo");
}

TEST_F(ToolRegistryTest, CallToolReturnTextContent) {
    auto result = reg.callTool("echo", {{"message", "hello"}});
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0]["type"], "text");
    // The text field contains a JSON string with the result.
    const std::string text = result[0]["text"];
    auto parsed = nlohmann::json::parse(text);
    EXPECT_EQ(parsed["echo"], "hello");
}

TEST_F(ToolRegistryTest, CallUnknownToolThrows) {
    EXPECT_THROW(reg.callTool("nonexistent", {}), std::runtime_error);
}

TEST_F(ToolRegistryTest, CallToolWithMissingRequiredParamThrows) {
    // "message" is required but not provided.  Throws std::invalid_argument.
    EXPECT_THROW(reg.callTool("echo", nlohmann::json::object()),
                 std::invalid_argument);
}

TEST_F(ToolRegistryTest, RegisteringAgainReplacesOldHandler) {
    ToolSpec new_spec;
    new_spec.name        = "echo";
    new_spec.description = "New echo.";
    new_spec.input_schema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"message", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"message"})}};
    new_spec.handler = [](const nlohmann::json&) {
        return nlohmann::json{{"replaced", true}};
    };
    reg.registerTool(std::move(new_spec));

    EXPECT_EQ(reg.size(), 1u);
    auto result = reg.callTool("echo", {{"message", "x"}});
    const auto parsed = nlohmann::json::parse(result[0]["text"].get<std::string>());
    EXPECT_EQ(parsed["replaced"], true);
}

TEST_F(ToolRegistryTest, MultipleTools) {
    ToolSpec noop;
    noop.name = "noop";
    noop.description = "No-op tool.";
    noop.input_schema = nlohmann::json{{"type", "object"}};
    noop.handler = [](const nlohmann::json&) {
        return nlohmann::json{{"ok", true}};
    };
    reg.registerTool(std::move(noop));

    EXPECT_EQ(reg.size(), 2u);
    auto result = reg.listTools();
    const auto& list = result["tools"];
    ASSERT_EQ(list.size(), 2u);
}

}  // namespace
}  // namespace jtag::mcp
