#include "ila_generator.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace jtag::ila {

std::string renderIlaConfigJson(const IlaGeneratorConfig& cfg) {
    std::ostringstream o;
    o << "{\n  \"lanes\": [\n";
    for (size_t i = 0; i < cfg.lanes.size(); ++i) {
        const auto& lane = cfg.lanes[i];
        const char* fmt =
            (lane.format == IlaGeneratorFormat::DEC) ? "DEC" :
            (lane.format == IlaGeneratorFormat::BIN) ? "BIN" : "HEX";
        o << "    {\"name\":\"" << lane.name << "\","
          << "\"hi\":" << lane.hi << ","
          << "\"lo\":" << lane.lo << ","
          << "\"fmt\":\"" << fmt << "\"}";
        if (i + 1 < cfg.lanes.size()) o << ",";
        o << "\n";
    }
    o << "  ]\n}\n";
    return o.str();
}

namespace {

constexpr int kMaxLaneCount = 15;
constexpr const char* kBundledRtlFiles[] = {
    "ila_trigger.sv",
    "ila_capture_fsm.sv",
    "ila_bram.sv",
    "ila_bscane2_top.sv",
};

bool isPowerOfTwo(int value) {
    return value > 0 && (value & (value - 1)) == 0;
}

int log2Exact(int value) {
    int out = 0;
    while (value > 1) {
        value >>= 1;
        ++out;
    }
    return out;
}

bool isIdentifier(const std::string& text) {
    if (text.empty()) return false;
    const char first = text[0];
    if (!((first >= 'A' && first <= 'Z') ||
          (first >= 'a' && first <= 'z') ||
          first == '_')) {
        return false;
    }
    for (char c : text) {
        const bool ok = ((c >= 'A' && c <= 'Z') ||
                         (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') ||
                         c == '_');
        if (!ok) return false;
    }
    return true;
}

bool isSafeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

std::string formatEnumLiteral(IlaGeneratorFormat fmt) {
    switch (fmt) {
        case IlaGeneratorFormat::DEC:
            return "4'd1";
        case IlaGeneratorFormat::BIN:
            return "4'd2";
        case IlaGeneratorFormat::HEX:
        default:
            return "4'd0";
    }
}

std::string formatEnumName(IlaGeneratorFormat fmt) {
    switch (fmt) {
        case IlaGeneratorFormat::DEC:
            return "DEC";
        case IlaGeneratorFormat::BIN:
            return "BIN";
        case IlaGeneratorFormat::HEX:
        default:
            return "HEX";
    }
}

std::string toForwardSlash(const std::filesystem::path& path) {
    return path.generic_string();
}

std::string joinSv8Array(const IlaGeneratorConfig& config, bool hi_array) {
    std::ostringstream oss;
    oss << "'{";
    for (int index = 0; index < kMaxLaneCount; ++index) {
        if (index != 0) oss << ", ";
        if (index < static_cast<int>(config.lanes.size())) {
            const int value = hi_array ? config.lanes[index].hi
                                       : config.lanes[index].lo;
            oss << "8'd" << value;
        } else {
            oss << "8'd0";
        }
    }
    oss << "}";
    return oss.str();
}

std::string joinSvFmtArray(const IlaGeneratorConfig& config) {
    std::ostringstream oss;
    oss << "'{";
    for (int index = 0; index < kMaxLaneCount; ++index) {
        if (index != 0) oss << ", ";
        if (index < static_cast<int>(config.lanes.size())) {
            oss << formatEnumLiteral(config.lanes[index].format);
        } else {
            oss << "4'd0";
        }
    }
    oss << "}";
    return oss.str();
}

std::string hex32(uint32_t value) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
        << value;
    return oss.str();
}

bool writeTextFile(const std::filesystem::path& path,
                   const std::string& content,
                   std::string& error) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        error = "failed to open output file: " + path.string();
        return false;
    }
    out << content;
    if (!out.good()) {
        error = "failed to write output file: " + path.string();
        return false;
    }
    return true;
}

bool copyFileContents(const std::filesystem::path& source,
                      const std::filesystem::path& destination,
                      std::string& error) {
    std::ifstream in(source, std::ios::binary);
    if (!in.is_open()) {
        error = "failed to open source file: " + source.string();
        return false;
    }
    std::ofstream out(destination, std::ios::binary);
    if (!out.is_open()) {
        error = "failed to open output file: " + destination.string();
        return false;
    }
    out << in.rdbuf();
    if (!in.good() && !in.eof()) {
        error = "failed to read source file: " + source.string();
        return false;
    }
    if (!out.good()) {
        error = "failed to write output file: " + destination.string();
        return false;
    }
    return true;
}

} // namespace

bool validateIlaGeneratorConfig(const IlaGeneratorConfig& config,
                                std::string& error) {
    if (!isSafeRelativePath(config.output_subdir)) {
        error = "output_subdir must be a safe relative path inside the repository";
        return false;
    }
    if (!isIdentifier(config.project_name)) {
        error = "project_name must be a valid identifier";
        return false;
    }
    if (!isIdentifier(config.top_module)) {
        error = "top_module must be a valid identifier";
        return false;
    }
    if (!isIdentifier(config.clock_port_name) ||
        !isIdentifier(config.reset_port_name) ||
        !isIdentifier(config.data_port_name) ||
        !isIdentifier(config.valid_port_name)) {
        error = "clock/reset/data/valid port names must be valid identifiers";
        return false;
    }
    if (config.fpga_part.empty()) {
        error = "fpga_part must not be empty";
        return false;
    }
    if (config.sample_clock_hz == 0) {
        error = "sample_clock_hz must be non-zero";
        return false;
    }
    if (config.data_width < 1 || config.data_width > 32) {
        error = "data_width must be in the range 1..32";
        return false;
    }
    if (!isPowerOfTwo(config.depth)) {
        error = "depth must be a power of two";
        return false;
    }
    if (config.lanes.empty()) {
        error = "at least one signal lane is required";
        return false;
    }
    if (static_cast<int>(config.lanes.size()) > kMaxLaneCount) {
        error = "no more than 15 signal lanes are supported";
        return false;
    }
    for (const auto& lane : config.lanes) {
        if (!isIdentifier(lane.name)) {
            error = "lane names must be valid identifiers";
            return false;
        }
        if (lane.lo < 0 || lane.hi < lane.lo || lane.hi >= config.data_width) {
            error = "lane bit range is out of bounds for the selected data width";
            return false;
        }
    }
    error.clear();
    return true;
}

bool renderIlaGeneratedFiles(const IlaGeneratorConfig& config,
                             const std::filesystem::path& repo_root,
                             IlaGeneratedFiles& out,
                             std::string& error) {
    if (!validateIlaGeneratorConfig(config, error)) {
        return false;
    }

    const auto rtl_root = repo_root / "hdl" / "ila" / "rtl";
    if (!std::filesystem::exists(rtl_root / "ila_bscane2_top.sv")) {
        error = "repo_root does not point to an OWLTAP repository with hdl/ila/rtl";
        return false;
    }

    out = {};
    out.addr_width = log2Exact(config.depth);
    out.output_dir = repo_root / config.output_subdir;
    out.wrapper_file_name = config.top_module + ".sv";
    out.bundled_rtl_file_names.assign(std::begin(kBundledRtlFiles),
                                      std::end(kBundledRtlFiles));
    for (const auto* file_name : kBundledRtlFiles) {
        if (!std::filesystem::exists(rtl_root / file_name)) {
            error = "missing bundled RTL source file: " + (rtl_root / file_name).string();
            return false;
        }
    }

    std::ostringstream wrapper;
    wrapper << "// Auto-generated by OWLTAP ILA Generator.\n";
    wrapper << "// Specification: hdl/ila/doc/generated_ip_spec.md\n";
    wrapper << "`timescale 1ns/1ps\n";
    wrapper << "`default_nettype none\n\n";
    wrapper << "module " << config.top_module << " (\n";
    wrapper << "    input  wire " << config.clock_port_name << ",\n";
    wrapper << "    input  wire " << config.reset_port_name << ",\n";
    wrapper << "    input  wire [" << (config.data_width - 1) << ":0] "
            << config.data_port_name << ",\n";
    wrapper << "    input  wire " << config.valid_port_name << "\n";
    wrapper << ");\n\n";
    wrapper << "    ila_bscane2_top #(\n";
    wrapper << "        .DATA_W    (" << config.data_width << "),\n";
    wrapper << "        .DEPTH     (" << config.depth << "),\n";
    wrapper << "        .ADDR_W    (" << out.addr_width << "),\n";
    wrapper << "        .NUM_CH    (1),\n";
    wrapper << "        .IDCODE_VAL(32'h" << hex32(config.idcode) << "),\n";
    wrapper << "        .SIG_COUNT (" << config.lanes.size() << "),\n";
    wrapper << "        .SIG_HI    (" << joinSv8Array(config, true) << "),\n";
    wrapper << "        .SIG_LO    (" << joinSv8Array(config, false) << "),\n";
    wrapper << "        .SIG_FMT   (" << joinSvFmtArray(config) << ")\n";
    wrapper << "    ) u_ila (\n";
    wrapper << "        .sample_clk   (" << config.clock_port_name << "),\n";
    wrapper << "        .sample_rst_n (" << config.reset_port_name << "),\n";
    wrapper << "        .data_in      (" << config.data_port_name << "),\n";
    wrapper << "        .data_valid   (" << config.valid_port_name << ")\n";
    wrapper << "    );\n\n";
    wrapper << "endmodule\n\n";
    wrapper << "`default_nettype wire\n";
    out.wrapper_sv = wrapper.str();

    std::ostringstream create_project;
    create_project << "# Auto-generated by OWLTAP ILA Generator\n";
    create_project << "set script_dir [file normalize [file dirname [info script]]]\n";
    create_project << "set out_dir $script_dir\n";
    create_project << "set proj_dir [file join $script_dir vivado_proj]\n\n";
    create_project << "create_project " << config.project_name
                   << " $proj_dir -part " << config.fpga_part << " -force\n";
    create_project << "set_property target_language Verilog [current_project]\n";
    create_project << "set_property simulator_language Mixed [current_project]\n\n";
    create_project << "add_files -norecurse [list \\\n";
    for (size_t index = 0; index < out.bundled_rtl_file_names.size(); ++index) {
        create_project << "    [file join $out_dir " << out.bundled_rtl_file_names[index] << "] \\\n";
    }
    create_project << "    [file join $out_dir " << out.wrapper_file_name << "]\\\n";
    create_project << "]\n";
    create_project << "foreach f [get_files *.sv] {\n";
    create_project << "    set_property file_type SystemVerilog $f\n";
    create_project << "}\n\n";
    create_project << "add_files -fileset constrs_1 -norecurse [file join $out_dir ila_generated.xdc]\n";
    create_project << "set_property top " << config.top_module << " [current_fileset]\n";
    create_project << "update_compile_order -fileset sources_1\n\n";
    create_project << "puts \"Project created: [get_property DIRECTORY [current_project]]\"\n";
    create_project << "puts \"Top module: " << config.top_module << "\"\n";
    create_project << "puts \"Part: " << config.fpga_part << "\"\n";
    out.create_project_tcl = create_project.str();

    std::ostringstream build_bitstream;
    build_bitstream << "# Auto-generated by OWLTAP ILA Generator\n";
    build_bitstream << "source create_project.tcl\n\n";
    build_bitstream << "puts \"=== Synthesis ===\"\n";
    build_bitstream << "launch_runs synth_1 -jobs 4\n";
    build_bitstream << "wait_on_run synth_1\n";
    build_bitstream << "if {[get_property PROGRESS [get_runs synth_1]] != \"100%\"} {\n";
    build_bitstream << "    puts \"ERROR: Synthesis failed\"\n";
    build_bitstream << "    exit 1\n";
    build_bitstream << "}\n\n";
    build_bitstream << "puts \"=== Implementation + Write Bitstream ===\"\n";
    build_bitstream << "launch_runs impl_1 -to_step write_bitstream -jobs 4\n";
    build_bitstream << "wait_on_run impl_1\n";
    build_bitstream << "if {[get_property PROGRESS [get_runs impl_1]] != \"100%\"} {\n";
    build_bitstream << "    puts \"ERROR: Implementation failed\"\n";
    build_bitstream << "    exit 1\n";
    build_bitstream << "}\n\n";
    build_bitstream << "set bit_file [file join $script_dir vivado_proj "
                    << config.project_name << ".runs impl_1 " << config.top_module
                    << ".bit]\n";
    build_bitstream << "puts \"Bitstream ready: $bit_file\"\n";
    out.build_bitstream_tcl = build_bitstream.str();

    std::ostringstream xdc;
    xdc << "# Auto-generated by OWLTAP ILA Generator\n";
    xdc << "create_clock -name sample_clk -period "
        << std::fixed << std::setprecision(3)
        << (1000000000.0 / static_cast<double>(config.sample_clock_hz))
        << " [get_ports " << config.clock_port_name << "]\n";
    xdc << "# Add any board-specific constraints for surrounding user logic separately.\n";
    out.xdc = xdc.str();

    std::ostringstream readme;
    readme << "# Generated OwlTAP ILA Package\n\n";
    readme << "This directory was generated by OWLTAP.\n\n";
    readme << "## Parameters\n\n";
    readme << "- Project: `" << config.project_name << "`\n";
    readme << "- Top module: `" << config.top_module << "`\n";
    readme << "- FPGA part: `" << config.fpga_part << "`\n";
    readme << "- DATA_W: `" << config.data_width << "`\n";
    readme << "- DEPTH: `" << config.depth << "`\n";
    readme << "- ADDR_W: `" << out.addr_width << "`\n";
    readme << "- IDCODE: `32'h" << hex32(config.idcode) << "`\n\n";
    readme << "## Signal lanes\n\n";
    for (const auto& lane : config.lanes) {
        readme << "- `" << lane.name << "`: [" << lane.hi << ":" << lane.lo
               << "] `" << formatEnumName(lane.format) << "`\n";
    }
    readme << "\n## Files\n\n";
    for (const auto& file_name : out.bundled_rtl_file_names) {
        readme << "- `" << file_name << "`\n";
    }
    readme << "- `" << out.wrapper_file_name << "`\n";
    readme << "- `ila_generated.xdc`\n";
    readme << "- `create_project.tcl`\n";
    readme << "- `build_bitstream.tcl`\n\n";
    readme << "## Usage\n\n";
    readme << "Run in this directory:\n\n";
    readme << "```tcl\n";
    readme << "vivado -mode batch -source build_bitstream.tcl\n";
    readme << "```\n\n";
    readme << "This package is self-contained: required OwlTAP RTL files are copied into this directory.\n";
    out.readme_md = readme.str();

    error.clear();
    return true;
}

bool writeIlaGeneratedFiles(const IlaGeneratorConfig& config,
                            const std::filesystem::path& repo_root,
                            IlaGeneratedFiles& out,
                            std::string& error) {
    if (!renderIlaGeneratedFiles(config, repo_root, out, error)) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(out.output_dir, ec);
    if (ec) {
        error = "failed to create output directory: " + out.output_dir.string();
        return false;
    }

    out.written_paths.clear();
    const auto wrapper_path = out.output_dir / out.wrapper_file_name;
    const auto xdc_path = out.output_dir / "ila_generated.xdc";
    const auto create_project_path = out.output_dir / "create_project.tcl";
    const auto build_bitstream_path = out.output_dir / "build_bitstream.tcl";
    const auto readme_path = out.output_dir / "README.md";
    const auto rtl_root = repo_root / "hdl" / "ila" / "rtl";

    if (!writeTextFile(wrapper_path, out.wrapper_sv, error)) return false;
    out.written_paths.push_back(wrapper_path);
    for (const auto& file_name : out.bundled_rtl_file_names) {
        const auto source_path = rtl_root / file_name;
        const auto destination_path = out.output_dir / file_name;
        if (!copyFileContents(source_path, destination_path, error)) return false;
        out.written_paths.push_back(destination_path);
    }
    if (!writeTextFile(xdc_path, out.xdc, error)) return false;
    out.written_paths.push_back(xdc_path);
    if (!writeTextFile(create_project_path, out.create_project_tcl, error)) return false;
    out.written_paths.push_back(create_project_path);
    if (!writeTextFile(build_bitstream_path, out.build_bitstream_tcl, error)) return false;
    out.written_paths.push_back(build_bitstream_path);
    if (!writeTextFile(readme_path, out.readme_md, error)) return false;
    out.written_paths.push_back(readme_path);

    const auto config_json_path =
        out.output_dir / (config.project_name + "_config.json");
    if (!writeTextFile(config_json_path, renderIlaConfigJson(config), error))
        return false;
    out.written_paths.push_back(config_json_path);
    out.config_json_path = config_json_path;

    error.clear();
    return true;
}

} // namespace jtag::ila