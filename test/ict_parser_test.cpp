#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "src/boundary_scan/interconnect_test.h"

namespace jtag {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string writeTempFile(const std::string& stem,
                                 const std::string& content) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto path = (dir / (stem + ".tmp")).string();
    std::ofstream f(path, std::ios::trunc);
    f << content;
    return path;
}

// ---------------------------------------------------------------------------
// parseIctFile
// ---------------------------------------------------------------------------

TEST(IctParserTest, MissingFileReturnsError) {
    std::string err;
    const auto nets = parseIctFile("/no/such/file.ict", err);
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(nets.empty());
}

TEST(IctParserTest, EmptyFileReturnsEmptyList) {
    const auto path = writeTempFile("ict_empty", "");
    std::string err;
    const auto nets = parseIctFile(path, err);
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(nets.empty());
}

TEST(IctParserTest, CommentsAndBlanksIgnored) {
    const auto path = writeTempFile("ict_comments",
        "# this is a comment\n"
        "\n"
        "  # another\n");
    std::string err;
    const auto nets = parseIctFile(path, err);
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(nets.empty());
}

TEST(IctParserTest, ParseSingleNet) {
    const auto path = writeTempFile("ict_single",
        "CLK  0:IO_L1P  1:IO_L2N\n");
    std::string err;
    const auto nets = parseIctFile(path, err);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(nets.size(), 1u);
    EXPECT_EQ(nets[0].name, "CLK");
    EXPECT_EQ(nets[0].driver.device_index, 0);
    EXPECT_EQ(nets[0].driver.pin, "IO_L1P");
    ASSERT_EQ(nets[0].receivers.size(), 1u);
    EXPECT_EQ(nets[0].receivers[0].device_index, 1);
    EXPECT_EQ(nets[0].receivers[0].pin, "IO_L2N");
}

TEST(IctParserTest, ParseMultipleNetsAndMultipleReceivers) {
    const auto path = writeTempFile("ict_multi",
        "# name  driver       receivers\n"
        "CLK   0:IO_L1P  1:IO_L2N\n"
        "DATA  0:IO_L2P  1:IO_L3P  1:IO_L4P\n");
    std::string err;
    const auto nets = parseIctFile(path, err);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(nets.size(), 2u);
    EXPECT_EQ(nets[1].name, "DATA");
    ASSERT_EQ(nets[1].receivers.size(), 2u);
    EXPECT_EQ(nets[1].receivers[0].pin, "IO_L3P");
    EXPECT_EQ(nets[1].receivers[1].pin, "IO_L4P");
}

TEST(IctParserTest, PinNamesConvertedToUpperCase) {
    const auto path = writeTempFile("ict_case",
        "NET  0:io_lower  1:io_mixed\n");
    std::string err;
    const auto nets = parseIctFile(path, err);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(nets.size(), 1u);
    EXPECT_EQ(nets[0].driver.pin, "IO_LOWER");
    EXPECT_EQ(nets[0].receivers[0].pin, "IO_MIXED");
}

TEST(IctParserTest, DeviceIndexParsedCorrectly) {
    const auto path = writeTempFile("ict_devidx",
        "SIG  2:IO_A  5:IO_B\n");
    std::string err;
    const auto nets = parseIctFile(path, err);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(nets.size(), 1u);
    EXPECT_EQ(nets[0].driver.device_index, 2);
    EXPECT_EQ(nets[0].receivers[0].device_index, 5);
}

TEST(IctParserTest, MalformedDevPinMissingColonReturnsError) {
    const auto path = writeTempFile("ict_baddevpin",
        "CLK  nocoion  1:IO_L2N\n");
    std::string err;
    const auto nets = parseIctFile(path, err);
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(nets.empty());
}

TEST(IctParserTest, TooFewColumnsReturnsError) {
    const auto path = writeTempFile("ict_toofew",
        "CLK  0:IO_L1P\n");  // missing at least one receiver
    std::string err;
    const auto nets = parseIctFile(path, err);
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(nets.empty());
}

TEST(IctParserTest, InlineCommentStripped) {
    // "# comment" after the data tokens should be ignored.
    // The '#' appears right after whitespace so it strips the comment,
    // leaving only the three real tokens.
    const auto path = writeTempFile("ict_inline_comment",
        "SIG  0:IO_A  1:IO_B  # comment\n");
    std::string err;
    const auto nets = parseIctFile(path, err);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(nets.size(), 1u);
    EXPECT_EQ(nets[0].name, "SIG");
    ASSERT_EQ(nets[0].receivers.size(), 1u);
}

// ---------------------------------------------------------------------------
// runInterconnectTest — null driver/scanner paths (no hardware required)
// ---------------------------------------------------------------------------

TEST(InterconnectRunnerTest, EmptyNetListReturnsZeroCounts) {
    const InterconnectResult r = runInterconnectTest({}, {}, {});
    EXPECT_EQ(r.pass_count, 0);
    EXPECT_EQ(r.fail_count, 0);
    EXPECT_TRUE(r.nets.empty());
}

TEST(InterconnectRunnerTest, NullDriverMarksNetAsFailed) {
    NetDef nd;
    nd.name = "SIG";
    nd.driver = {0, "IO_DRV"};
    nd.receivers = {{0, "IO_RX"}};

    // Null PinDriver => cannot drive => UNKNOWN for all receivers => fail
    const InterconnectResult r =
        runInterconnectTest({nd}, {nullptr}, {nullptr});
    EXPECT_EQ(r.fail_count, 1);
    EXPECT_EQ(r.pass_count, 0);
    ASSERT_EQ(r.nets.size(), 1u);
    EXPECT_FALSE(r.nets[0].pass);
    // Each step (drive 0, drive 1) should record UNKNOWN for the receiver
    for (const auto& step : r.nets[0].steps) {
        ASSERT_EQ(step.observed.size(), 1u);
        EXPECT_EQ(step.observed[0], PinState::UNKNOWN);
        EXPECT_FALSE(step.pass);
    }
}

TEST(InterconnectRunnerTest, OutOfRangeDriverIndexTreatedAsNull) {
    NetDef nd;
    nd.name = "NET";
    nd.driver = {9, "IO_X"};   // device index beyond any scanners/drivers
    nd.receivers = {{9, "IO_Y"}};

    // Empty scanner/driver vectors => index 9 is out of range => null path
    const InterconnectResult r = runInterconnectTest({nd}, {}, {});
    EXPECT_EQ(r.fail_count, 1);
    ASSERT_EQ(r.nets.size(), 1u);
    EXPECT_FALSE(r.nets[0].pass);
}

TEST(InterconnectRunnerTest, MultipleNetsAllFailWithNullDrivers) {
    NetDef a;
    a.name = "NET_A";
    a.driver = {0, "IO_A"};
    a.receivers = {{1, "IO_B"}};

    NetDef b;
    b.name = "NET_B";
    b.driver = {0, "IO_C"};
    b.receivers = {{1, "IO_D"}};

    const InterconnectResult r =
        runInterconnectTest({a, b}, {nullptr, nullptr}, {nullptr, nullptr});
    EXPECT_EQ(r.fail_count, 2);
    EXPECT_EQ(r.pass_count, 0);
    EXPECT_EQ(r.nets.size(), 2u);
}

// ---------------------------------------------------------------------------
// formatInterconnectReport
// ---------------------------------------------------------------------------

TEST(InterconnectRunnerTest, FormatReportContainsSummaryAndNetName) {
    NetDef nd;
    nd.name = "MY_NET";
    nd.driver = {0, "DRV"};
    nd.receivers = {{1, "RX"}};

    NetResult nr;
    nr.name = "MY_NET";
    nr.pass = false;
    NetTestStep step;
    step.driven_value = 0;
    step.observed = {PinState::UNKNOWN};
    step.pass = false;
    nr.steps.push_back(step);

    InterconnectResult result;
    result.nets.push_back(nr);
    result.fail_count = 1;
    result.pass_count = 0;

    const std::string report = formatInterconnectReport(result, {nd});
    EXPECT_NE(report.find("MY_NET"), std::string::npos);
    EXPECT_NE(report.find("FAIL: 1"), std::string::npos);
    EXPECT_NE(report.find("UNK"), std::string::npos);
}

} // namespace
} // namespace jtag
