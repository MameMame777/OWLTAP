#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "scanner.h"
#include "src/bsdl/bsdl_model.h"
#include "src/jtag/jtag_chain.h"

namespace jtag {

/// Drives FPGA pins using EXTEST instruction and boundary scan register.
/// WARNING: EXTEST overrides normal pin function. Use with caution.
class PinDriver {
public:
    /// @param chain        JTAG chain manager
    /// @param device_index Target device index in chain
    PinDriver(JtagChain& chain, int device_index);

    /// Check if driver is properly configured.
    bool isReady() const;

    /// Set a pin's output value.
    /// Does not take effect until applyOutputs() is called.
    /// @param pin_name  Name of the pin to drive
    /// @param value     0 (LOW) or 1 (HIGH)
    /// @return true if pin is drivable
    bool setPin(const std::string& pin_name, int value);

    /// Set a pin to high impedance (disable output).
    /// @param pin_name  Name of the pin
    /// @return true if pin supports tri-state
    bool setPinHighZ(const std::string& pin_name);

    /// Apply all pending pin outputs using EXTEST.
    /// This loads the EXTEST instruction and shifts out the BSR data.
    /// @return true on success
    bool applyOutputs();

    /// Reset all outputs to safe values (from BSDL safe_value field).
    void resetToSafe();

    /// Get the current configured value for a pin.
    /// @return 0, 1, or -1 (high-Z / not set)
    int getPinValue(const std::string& pin_name) const;

    /// Get last error message.
    const std::string& lastError() const { return last_error_; }

private:
    JtagChain& chain_;
    int device_index_;
    std::vector<uint8_t> bsr_data_;  // BSR bit buffer
    std::string last_error_;

    void setBit(int position, bool value);
    bool getBit(int position) const;
    void initBsrFromSafe();
};

} // namespace jtag
