#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/ftdi/ftdi_device.h"

namespace jtag::gui {

/// Device connection configuration
struct DeviceConfig {
    uint16_t vendor_id = 0x0403;
    uint16_t product_id = 0x6010;
    std::string serial;
    int interface_channel = 0;
    uint32_t clock_freq_hz = 1000000;  // 1 MHz default – safe for Zynq BSCAN
};

/// ImGui popup modal for selecting and configuring FTDI device connection.
class DeviceDialog {
public:
    /// Draw the dialog popup. Set *p_open to false to close.
    static void draw(bool* p_open);

    /// Get the last-confirmed config.
    static const DeviceConfig& config() { return config_; }

    /// Pre-fill the dialog with a saved config (call before opening).
    static void setConfig(const DeviceConfig& c) { config_ = c; }

    /// Returns true if user pressed OK since last check, then resets flag.
    static bool accepted();

private:
    static void refreshDevices();

    static DeviceConfig config_;
    static bool accepted_;
    static char vid_buf_[5];
    static char pid_buf_[5];
    static int interface_idx_;
    static int clock_hz_;
    static int device_idx_;

    struct DeviceEntry {
        std::string label;
        std::string serial;
        jtag::FtdiDeviceInfo info;  // full device info for detail display
    };
    static std::vector<DeviceEntry> devices_;
};

} // namespace jtag::gui
