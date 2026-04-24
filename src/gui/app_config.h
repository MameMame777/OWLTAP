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
    int         hi  = 0;
    int         lo  = 0;
    BusFormat   fmt = BusFormat::HEX;
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

    // XDC pin alias file (optional)
    std::string xdc_path;

    /// Save config to a JSON file. Returns true on success.
    bool save(const std::string& path) const;

    /// Load config from a JSON file. Returns default config on failure.
    static AppConfig load(const std::string& path);
};

} // namespace jtag::gui
