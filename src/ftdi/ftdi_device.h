#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mpsse.h"

// Forward declaration for libftdi
struct ftdi_context;

namespace jtag {

/// Information about a detected FTDI device
struct FtdiDeviceInfo {
    uint16_t vendor_id;
    uint16_t product_id;
    std::string manufacturer;
    std::string description;
    std::string serial;
};

/// FTDI interface/channel selection (for multi-channel chips)
enum class FtdiInterface : int {
    A = 0,  // INTERFACE_A
    B = 1,  // INTERFACE_B
    C = 2,  // INTERFACE_C
    D = 3,  // INTERFACE_D
};

/// RAII wrapper around libftdi ftdi_context.
/// Handles device opening, MPSSE configuration, and data transfer.
class FtdiDevice {
public:
    FtdiDevice();
    ~FtdiDevice();

    // Non-copyable
    FtdiDevice(const FtdiDevice&) = delete;
    FtdiDevice& operator=(const FtdiDevice&) = delete;

    // Movable
    FtdiDevice(FtdiDevice&& other) noexcept;
    FtdiDevice& operator=(FtdiDevice&& other) noexcept;

    /// Enumerate all connected FTDI devices.
    /// @param vendor_id  USB vendor ID (default 0x0403 = FTDI)
    /// @param product_id USB product ID (0 = any FTDI device)
    static std::vector<FtdiDeviceInfo> enumerate(
        uint16_t vendor_id = 0x0403, uint16_t product_id = 0);

    /// Open device by vendor/product ID and optional serial number.
    /// @param vendor_id   USB vendor ID
    /// @param product_id  USB product ID
    /// @param serial      Serial number filter (empty = first match)
    /// @param iface       Interface/channel for multi-channel chips
    /// @return true on success
    bool open(uint16_t vendor_id, uint16_t product_id,
              const std::string& serial = "",
              FtdiInterface iface = FtdiInterface::A);

    /// Close the device connection.
    void close();

    /// Check if device is currently open.
    bool isOpen() const { return is_open_; }

    /// Initialize MPSSE mode for JTAG operations.
    /// Must be called after open() and before any JTAG operations.
    /// @param clock_freq_hz  Desired JTAG TCK frequency in Hz
    /// @return true on success
    bool initMpsse(uint32_t clock_freq_hz = 6000000);

    /// Write MPSSE command buffer to device and read back response.
    /// @param cmd_buf  Command buffer to send
    /// @param read_buf Output buffer for response data (resized to fit)
    /// @return true on success
    bool transfer(const MpsseCommandBuffer& cmd_buf,
                  std::vector<uint8_t>& read_buf);

    /// Write MPSSE command buffer to device (no read expected).
    /// @param cmd_buf  Command buffer to send
    /// @return true on success
    bool write(const MpsseCommandBuffer& cmd_buf);

    /// Get last error message.
    const std::string& lastError() const { return last_error_; }

private:
    bool purgeBuffers();
    bool syncMpsse();

    ftdi_context* ftdi_ = nullptr;   // libftdi backend (non-Windows)
    void* d2xx_handle_  = nullptr;   // D2XX backend (Windows)
    bool is_open_ = false;
    std::string last_error_;
};

} // namespace jtag
