// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "src/ila/ila_generator.h"

namespace jtag::ila {
namespace {

class TempRepoRoot {
public:
    TempRepoRoot() {
        const auto base = std::filesystem::temp_directory_path();
        path_ = base / std::filesystem::path("owltap_ila_generator_test_" + std::to_string(std::rand()));
        std::filesystem::create_directories(path_ / "hdl" / "ila" / "rtl");
        std::ofstream(path_ / "hdl" / "ila" / "rtl" / "ila_bscane2_top.sv") << "module ila_bscane2_top; endmodule\n";
        std::ofstream(path_ / "hdl" / "ila" / "rtl" / "ila_trigger.sv") << "module ila_trigger; endmodule\n";
        std::ofstream(path_ / "hdl" / "ila" / "rtl" / "ila_capture_fsm.sv") << "module ila_capture_fsm; endmodule\n";
        std::ofstream(path_ / "hdl" / "ila" / "rtl" / "ila_bram.sv") << "module ila_bram; endmodule\n";
    }

    ~TempRepoRoot() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

IlaGeneratorConfig makeValidConfig() {
    IlaGeneratorConfig config;
    config.output_subdir = "hdl/ila/generated/demo";
    config.project_name = "ila_demo";
    config.top_module = "ila_demo_top";
    config.fpga_part = "xc7z020clg400-1";
    config.clock_port_name = "sample_clk";
    config.reset_port_name = "sample_rst_n";
    config.data_port_name = "data_in";
    config.valid_port_name = "data_valid";
    config.sample_clock_hz = 125000000;
    config.data_width = 32;
    config.depth = 1024;
    config.idcode = 0xA17A0001u;
    config.lanes = {
        {"upper_half", 31, 16, IlaGeneratorFormat::HEX},
        {"lower_half", 15, 0, IlaGeneratorFormat::BIN},
    };
    return config;
}

TEST(IlaGeneratorTest, ValidateRejectsBadDepth) {
    IlaGeneratorConfig config = makeValidConfig();
    config.depth = 1000;
    std::string error;
    EXPECT_FALSE(validateIlaGeneratorConfig(config, error));
    EXPECT_NE(error.find("power of two"), std::string::npos);
}

TEST(IlaGeneratorTest, ValidateRejectsWideDataBus) {
    IlaGeneratorConfig config = makeValidConfig();
    config.data_width = 64;
    std::string error;
    EXPECT_FALSE(validateIlaGeneratorConfig(config, error));
    EXPECT_NE(error.find("1..32"), std::string::npos);
}

TEST(IlaGeneratorTest, ValidateRejectsAbsoluteOutputPath) {
    IlaGeneratorConfig config = makeValidConfig();
    config.output_subdir = std::filesystem::temp_directory_path() / "bad";
    std::string error;
    EXPECT_FALSE(validateIlaGeneratorConfig(config, error));
    EXPECT_NE(error.find("safe relative path"), std::string::npos);
}

TEST(IlaGeneratorTest, RenderGeneratesExpectedStrings) {
    TempRepoRoot repo_root;
    IlaGeneratorConfig config = makeValidConfig();
    IlaGeneratedFiles files;
    std::string error;

    ASSERT_TRUE(renderIlaGeneratedFiles(config, repo_root.path(), files, error)) << error;
    EXPECT_EQ(files.addr_width, 10);
    EXPECT_EQ(files.wrapper_file_name, "ila_demo_top.sv");
    EXPECT_NE(files.wrapper_sv.find("module ila_demo_top"), std::string::npos);
    EXPECT_NE(files.wrapper_sv.find(".DATA_W    (32)"), std::string::npos);
    EXPECT_NE(files.wrapper_sv.find(".DEPTH     (1024)"), std::string::npos);
    EXPECT_NE(files.wrapper_sv.find(".ADDR_W    (10)"), std::string::npos);
    EXPECT_NE(files.wrapper_sv.find(".SIG_COUNT (2)"), std::string::npos);
    EXPECT_NE(files.wrapper_sv.find("8'd31, 8'd15"), std::string::npos);
    EXPECT_NE(files.wrapper_sv.find("4'd0, 4'd2"), std::string::npos);
    EXPECT_NE(files.create_project_tcl.find("ila_bscane2_top.sv"), std::string::npos);
    EXPECT_EQ(files.create_project_tcl.find("hdl ila rtl"), std::string::npos);
    EXPECT_EQ(files.create_project_tcl.find(repo_root.path().string()), std::string::npos);
    EXPECT_NE(files.build_bitstream_tcl.find("source create_project.tcl"), std::string::npos);
    EXPECT_NE(files.build_bitstream_tcl.find("if {[get_property PROGRESS [get_runs synth_1]] != \"100%\"} {"), std::string::npos);
    EXPECT_NE(files.build_bitstream_tcl.find("if {[get_property PROGRESS [get_runs impl_1]] != \"100%\"} {"), std::string::npos);
    EXPECT_NE(files.xdc.find("create_clock"), std::string::npos);
    EXPECT_NE(files.readme_md.find("upper_half"), std::string::npos);
    EXPECT_NE(files.readme_md.find("ila_bscane2_top.sv"), std::string::npos);
}

TEST(IlaGeneratorTest, WriteCreatesExpectedFiles) {
    TempRepoRoot repo_root;
    IlaGeneratorConfig config = makeValidConfig();
    IlaGeneratedFiles files;
    std::string error;

    ASSERT_TRUE(writeIlaGeneratedFiles(config, repo_root.path(), files, error)) << error;
    ASSERT_EQ(files.written_paths.size(), 9u);
    for (const auto& path : files.written_paths) {
        EXPECT_TRUE(std::filesystem::exists(path)) << path.string();
    }
    EXPECT_TRUE(std::filesystem::exists(files.output_dir / "ila_demo_top.sv"));
    EXPECT_TRUE(std::filesystem::exists(files.output_dir / "create_project.tcl"));
    EXPECT_TRUE(std::filesystem::exists(files.output_dir / "ila_bscane2_top.sv"));
    EXPECT_TRUE(std::filesystem::exists(files.output_dir / "ila_trigger.sv"));
}

TEST(IlaGeneratorTest, ValidateRejectsOutOfRangeLane) {
    IlaGeneratorConfig config = makeValidConfig();
    config.lanes[0].hi = 40;
    std::string error;
    EXPECT_FALSE(validateIlaGeneratorConfig(config, error));
    EXPECT_NE(error.find("out of bounds"), std::string::npos);
}

} // namespace
} // namespace jtag::ila
