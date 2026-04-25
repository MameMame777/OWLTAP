// Tests for JSON-RPC 2.0 framing and request parsing (no hardware).
#include <gtest/gtest.h>

#include "src/mcp/json_rpc.h"

namespace jtag::mcp {
namespace {

// ── parseRequest ─────────────────────────────────────────────────────────────

TEST(JsonRpcTest, ParseValidRequest) {
    auto j = nlohmann::json{
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};
    std::string err;
    auto req = parseRequest(j, err);
    ASSERT_TRUE(req.has_value()) << err;
    EXPECT_EQ(req->method, "tools/list");
    EXPECT_FALSE(req->is_notification());
    EXPECT_EQ(req->id, 1);
}

TEST(JsonRpcTest, ParseNotification) {
    auto j = nlohmann::json{{"jsonrpc", "2.0"}, {"method", "ping"}};
    std::string err;
    auto req = parseRequest(j, err);
    ASSERT_TRUE(req.has_value()) << err;
    EXPECT_EQ(req->method, "ping");
    EXPECT_TRUE(req->is_notification());
}

TEST(JsonRpcTest, ParseRequestWithStringId) {
    auto j = nlohmann::json{
        {"jsonrpc", "2.0"}, {"id", "abc"}, {"method", "foo"}};
    std::string err;
    auto req = parseRequest(j, err);
    ASSERT_TRUE(req.has_value()) << err;
    EXPECT_EQ(req->id, "abc");
}

TEST(JsonRpcTest, ParseRequestWithParams) {
    auto j = nlohmann::json{
        {"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
        {"params", {{"name", "read_idcode"}, {"arguments", nullptr}}}};
    std::string err;
    auto req = parseRequest(j, err);
    ASSERT_TRUE(req.has_value()) << err;
    EXPECT_EQ(req->params["name"], "read_idcode");
}

TEST(JsonRpcTest, ParseMissingMethodReturnsNullopt) {
    auto j = nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}};
    std::string err;
    auto req = parseRequest(j, err);
    EXPECT_FALSE(req.has_value());
    EXPECT_FALSE(err.empty());
}

TEST(JsonRpcTest, ParseNonObjectReturnsNullopt) {
    std::string err;
    auto req = parseRequest(nlohmann::json::array(), err);
    EXPECT_FALSE(req.has_value());
}

// ── makeResult / makeError ───────────────────────────────────────────────────

TEST(JsonRpcTest, MakeResultShape) {
    auto j = makeResult(1, nlohmann::json{{"answer", 42}});
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["id"], 1);
    EXPECT_EQ(j["result"]["answer"], 42);
    EXPECT_FALSE(j.contains("error"));
}

TEST(JsonRpcTest, MakeErrorShape) {
    auto j = makeError(2, RpcErrorCode::kMethodNotFound, "not found");
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["id"], 2);
    EXPECT_EQ(j["error"]["code"], static_cast<int>(RpcErrorCode::kMethodNotFound));
    EXPECT_EQ(j["error"]["message"], "not found");
    EXPECT_FALSE(j.contains("result"));
}

TEST(JsonRpcTest, MakeErrorWithNullId) {
    auto j = makeError(nullptr, RpcErrorCode::kParseError, "bad json");
    EXPECT_TRUE(j["id"].is_null());
    EXPECT_EQ(j["error"]["code"], -32700);
}

TEST(JsonRpcTest, MakeErrorWithData) {
    auto j = makeError(1, RpcErrorCode::kInvalidParams, "bad",
                       nlohmann::json{{"field", "x"}});
    EXPECT_EQ(j["error"]["data"]["field"], "x");
}

// ── frameMessage / tryReadMessage ────────────────────────────────────────────

TEST(JsonRpcTest, FrameAndReadRoundTrip) {
    auto original = nlohmann::json{{"hello", "world"}};
    const std::string framed = frameMessage(original);
    EXPECT_NE(framed.find("Content-Length:"), std::string::npos);

    std::size_t pos = 0;
    auto parsed = tryReadMessage(framed, pos);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ((*parsed)["hello"], "world");
    EXPECT_EQ(pos, framed.size());
}

TEST(JsonRpcTest, PartialMessageReturnsNullopt) {
    const std::string partial = "Content-Length: 100\r\n\r\n{";
    std::size_t pos = 0;
    auto parsed = tryReadMessage(partial, pos);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(pos, 0u);  // Position should not advance on partial.
}

TEST(JsonRpcTest, MultipleMessagesCanBeRead) {
    auto m1 = nlohmann::json{{"a", 1}};
    auto m2 = nlohmann::json{{"b", 2}};
    const std::string buf = frameMessage(m1) + frameMessage(m2);

    std::size_t pos = 0;
    auto r1 = tryReadMessage(buf, pos);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ((*r1)["a"], 1);

    auto r2 = tryReadMessage(buf, pos);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ((*r2)["b"], 2);

    auto r3 = tryReadMessage(buf, pos);
    EXPECT_FALSE(r3.has_value());
}

}  // namespace
}  // namespace jtag::mcp
