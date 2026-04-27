#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tap_controller.h"
#include "src/bsdl/bsdl_model.h"

namespace jtag {

/// Represents a single device in a JTAG chain.
struct ChainDevice {
    int position;           // Position in chain (0 = closest to TDO)
    uint32_t idcode;        // Device IDCODE (0 if BYPASS)
    int ir_length;          // Instruction register length
    std::string bsdl_file;  // Path to BSDL file (optional)
    std::unique_ptr<bsdl::BSDLDevice> bsdl;  // Parsed BSDL data (optional)
};

/// Manages a JTAG chain with one or more devices.
/// Handles IR/DR operations accounting for other devices in the chain.
class JtagChain {
public:
    /// @param tap  Reference to initialized TapController
    explicit JtagChain(TapController& tap);

    /// Auto-detect devices in the chain by reading IDCODEs.
    /// @return Number of devices found
    int detectDevices();

    /// Get list of detected devices.
    const std::vector<ChainDevice>& devices() const { return devices_; }

    /// Get device count.
    int deviceCount() const { return static_cast<int>(devices_.size()); }

    /// Load BSDL file for a specific device in the chain.
    /// @param device_index  Index into devices() vector
    /// @param bsdl_path     Path to BSDL file
    /// @return true on success
    bool loadBsdl(int device_index, const std::string& bsdl_path);

    /// Shift an instruction into a specific device's IR.
    /// Other devices in the chain receive BYPASS instruction.
    /// @param device_index  Target device index
    /// @param instruction   Instruction opcode value
    /// @return true on success
    bool selectInstruction(int device_index, uint32_t instruction);

    /// Read the boundary scan register of a specific device.
    /// Assumes SAMPLE instruction is already loaded.
    /// @param device_index  Target device index
    /// @param bsr_data      Output: BSR data (LSB first)
    /// @param partial_bits  If > 0 and < full BSR length, only clock out this
    ///                      many bits (partial read for FPS optimisation).
    ///                      0 means read full BSR (default).
    /// @return true on success
    bool readBSR(int device_index, std::vector<uint8_t>& bsr_data,
                 int partial_bits = 0);

    /// Write boundary scan register of a specific device.
    /// Used with EXTEST to drive pins.
    /// @param device_index  Target device index
    /// @param bsr_data      BSR data to write (LSB first)
    /// @return true on success
    bool writeBSR(int device_index, const uint8_t* bsr_data, int bsr_bits);

    /// Read a raw Data Register from a specific device (BSDL not required).
    /// Assumes the appropriate instruction is already loaded via selectInstruction().
    /// Accounts for 1-bit BYPASS registers of other devices in the chain.
    /// @param device_index  Target device index
    /// @param dr_bits       Number of bits in the target device's DR
    /// @param tdo_data      Output: captured DR data (LSB first)
    /// @return true on success
    bool readDataDR(int device_index, int dr_bits, std::vector<uint8_t>& tdo_data);

    /// Get last error message.
    const std::string& lastError() const { return last_error_; }

    /// Get the underlying TAP controller.
    TapController& tap() { return tap_; }

private:
    /// Compute total IR length for all devices in chain.
    int totalIRLength() const;

    TapController& tap_;
    std::vector<ChainDevice> devices_;
    std::string last_error_;
};

} // namespace jtag
