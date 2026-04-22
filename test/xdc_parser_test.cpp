#include <gtest/gtest.h>

#include "src/xdc/xdc_parser.h"

namespace {

// Minimal XDC content matching pynqaxi.xdc format.
constexpr const char* kXdcContent = R"(
# PYNQ-Z2 constraint file
set_property -dict {PACKAGE_PIN M14 IOSTANDARD LVCMOS33} [get_ports {led_out[0]}]
set_property -dict {PACKAGE_PIN M15 IOSTANDARD LVCMOS33} [get_ports {led_out[1]}]
set_property -dict {PACKAGE_PIN G15 IOSTANDARD LVCMOS33} [get_ports {sw_in[0]}]
set_property -dict {PACKAGE_PIN P15 IOSTANDARD LVCMOS33} [get_ports {sw_in[1]}]
set_property -dict {PACKAGE_PIN K18 IOSTANDARD LVCMOS18} [get_ports {btn_in[0]}]
set_property -dict {PACKAGE_PIN P16 IOSTANDARD LVCMOS18} [get_ports {btn_in[1]}]
# Timing constraint (should be ignored)
create_clock -period 8.000 -name sys_clk [get_ports {clk}]
)";

// Write content to a temp file and return its path.
static std::string writeTempXdc(const char* content) {
    const std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp") +
                             "/xdc_parser_test_tmp.xdc";
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
    return path;
}

TEST(XdcParser, ParsesLedPins) {
    const std::string path = writeTempXdc(kXdcContent);
    auto map = jtag::xdc::parseXdc(path);

    ASSERT_TRUE(map.count("M14"));
    EXPECT_EQ(map.at("M14"), "led_out[0]");

    ASSERT_TRUE(map.count("M15"));
    EXPECT_EQ(map.at("M15"), "led_out[1]");
}

TEST(XdcParser, ParsesSwitchPins) {
    const std::string path = writeTempXdc(kXdcContent);
    auto map = jtag::xdc::parseXdc(path);

    ASSERT_TRUE(map.count("G15"));
    EXPECT_EQ(map.at("G15"), "sw_in[0]");

    ASSERT_TRUE(map.count("P15"));
    EXPECT_EQ(map.at("P15"), "sw_in[1]");
}

TEST(XdcParser, ParsesButtonPins) {
    const std::string path = writeTempXdc(kXdcContent);
    auto map = jtag::xdc::parseXdc(path);

    ASSERT_TRUE(map.count("K18"));
    EXPECT_EQ(map.at("K18"), "btn_in[0]");

    ASSERT_TRUE(map.count("P16"));
    EXPECT_EQ(map.at("P16"), "btn_in[1]");
}

TEST(XdcParser, SkipsCommentAndNonSetPropertyLines) {
    const std::string path = writeTempXdc(kXdcContent);
    auto map = jtag::xdc::parseXdc(path);

    // create_clock line does not produce a mapping
    EXPECT_FALSE(map.count("clk"));

    // Total count: 6 set_property lines
    EXPECT_EQ(map.size(), 6u);
}

TEST(XdcParser, PinNameIsUppercase) {
    // Even if the XDC somehow uses lowercase (non-standard), the parser
    // should uppercase it to match BSDL pin names.
    constexpr const char* lower_xdc =
        "set_property -dict {PACKAGE_PIN m14 IOSTANDARD LVCMOS33} [get_ports {my_pin}]\n";
    const std::string path = writeTempXdc(lower_xdc);
    auto map = jtag::xdc::parseXdc(path);

    ASSERT_TRUE(map.count("M14"));
    EXPECT_EQ(map.at("M14"), "my_pin");
}

TEST(XdcParser, MissingFileReturnsEmptyMap) {
    auto map = jtag::xdc::parseXdc("/nonexistent/path/file.xdc");
    EXPECT_TRUE(map.empty());
}

TEST(XdcParser, EmptyMapOnEmptyFile) {
    const std::string path = writeTempXdc("");
    auto map = jtag::xdc::parseXdc(path);
    EXPECT_TRUE(map.empty());
}

}  // namespace
