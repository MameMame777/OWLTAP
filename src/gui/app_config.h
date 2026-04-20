#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jtag::gui {

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

    /// Save config to a JSON file. Returns true on success.
    bool save(const std::string& path) const;

    /// Load config from a JSON file. Returns default config on failure.
    static AppConfig load(const std::string& path);
};

} // namespace jtag::gui
