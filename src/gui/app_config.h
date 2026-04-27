#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jtag::gui {

/// Display format for multi-bit bus values.
enum class BusFormat {
    HEX = 0,
    DEC = 1,
    BIN = 2,
};

/// Persistent definition of a multi-bit bus (group of signals).
/// signals[0] = MSB, signals.back() = LSB.
struct BusDefinition {
    std::string name;
    std::vector<std::string> signals;  ///< MSB first
    BusFormat format = BusFormat::HEX;
};

/// Persistent definition of an ILA signal lane (bit-field slice of the
/// captured data word).  hi/lo are inclusive 0-based bit indices.
struct IlaSignalConfig {
    std::string name;
    int         hi          = 0;
    int         lo          = 0;
    BusFormat   fmt         = BusFormat::HEX;
    int         trig_cond   = 0;   ///< TriggerCond cast to int (0 = None); Group A
    uint32_t    trig_value  = 0;   ///< only used when trig_cond is Eq or Neq; Group A
    int         trig_cond_b = 0;   ///< TriggerCond for Group B (None/Eq/Neq only)
    uint32_t    trig_value_b = 0;  ///< Group B match value
};

/// Persistent application settings saved to/loaded from cfg.json.
struct AppConfig {
    // Device connection
    uint16_t vendor_id      = 0x0403;
    uint16_t product_id     = 0x6010;
    std::string serial;
    int interface_channel   = 0;
    uint32_t clock_freq_hz  = 1000000;

    // BSDL
    std::string bsdl_path;
    int bsdl_device_index   = 0;

    // Signal selection
    std::vector<std::string> selected_pins;

    // Bus definitions
    std::vector<BusDefinition> buses;

    // ILA signal lane definitions (per-lane bit-field slicing)
    std::vector<IlaSignalConfig> ila_signals;
    bool ila_or_mode = false;  ///< Trigger OR mode (false = AND)

    // ILA generator wizard defaults
    std::string ila_generator_output_subdir = "hdl/ila/generated/ila_generated";
    std::string ila_generator_project_name = "ila_generated";
    std::string ila_generator_top_module = "ila_generated_top";
    std::string ila_generator_fpga_part = "xc7z020clg400-1";
    std::string ila_generator_clock_port_name = "sample_clk";
    std::string ila_generator_reset_port_name = "sample_rst_n";
    std::string ila_generator_data_port_name = "data_in";
    std::string ila_generator_valid_port_name = "data_valid";
    uint32_t ila_generator_sample_clock_hz = 125000000;
    int ila_generator_data_width = 32;
    int ila_generator_depth = 1024;
    uint32_t ila_generator_idcode = 0xA17A0001u;
    std::vector<IlaSignalConfig> ila_generator_lanes;

    // XDC pin alias file (optional)
    std::string xdc_path;

    /// Save config to a JSON file. Returns true on success.
    bool save(const std::string& path) const;

    /// Load config from a JSON file. Returns default config on failure.
    static AppConfig load(const std::string& path);
};

} // namespace jtag::gui
