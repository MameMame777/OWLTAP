#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jtag::ila {

enum class IlaGeneratorFormat {
    HEX = 0,
    DEC = 1,
    BIN = 2,
};

struct IlaGeneratorLane {
    std::string        name;
    int                hi = 0;
    int                lo = 0;
    IlaGeneratorFormat format = IlaGeneratorFormat::HEX;
};

struct IlaGeneratorConfig {
    std::filesystem::path output_subdir = "hdl/ila/generated/ila_generated";
    std::string project_name = "ila_generated";
    std::string top_module = "ila_generated_top";
    std::string fpga_part = "xc7z020clg400-1";
    std::string clock_port_name = "sample_clk";
    std::string reset_port_name = "sample_rst_n";
    std::string data_port_name = "data_in";
    std::string valid_port_name = "data_valid";
    uint32_t sample_clock_hz = 125000000;
    int data_width = 32;
    int depth = 1024;
    uint32_t idcode = 0xA17A0001u;
    std::vector<IlaGeneratorLane> lanes;
};

struct IlaGeneratedFiles {
    int addr_width = 0;
    std::filesystem::path output_dir;
    std::string wrapper_file_name;
    std::string wrapper_sv;
    std::vector<std::string> bundled_rtl_file_names;
    std::string create_project_tcl;
    std::string build_bitstream_tcl;
    std::string xdc;
    std::string readme_md;
    std::vector<std::filesystem::path> written_paths;
};

bool validateIlaGeneratorConfig(const IlaGeneratorConfig& config,
                                std::string& error);

bool renderIlaGeneratedFiles(const IlaGeneratorConfig& config,
                             const std::filesystem::path& repo_root,
                             IlaGeneratedFiles& out,
                             std::string& error);

bool writeIlaGeneratedFiles(const IlaGeneratorConfig& config,
                            const std::filesystem::path& repo_root,
                            IlaGeneratedFiles& out,
                            std::string& error);

} // namespace jtag::ila
