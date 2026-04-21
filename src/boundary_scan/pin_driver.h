#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "scanner.h"
#include "src/bsdl/bsdl_model.h"
#include "src/jtag/jtag_chain.h"

namespace jtag {

/// Returns false for devices whose boundary register covers a live PS/DDR/MIO
/// subsystem that would be commandeered by EXTEST.
bool deviceAllowsLiveExtest(const bsdl::BSDLDevice& device,
                            std::string* reason = nullptr);

/// Set a pin's output value in a BSR staging buffer.
/// Sets both the output cell bit and the control cell enable bit.
/// Returns false if the pin is not drivable; fills *error when non-null.
bool stagePinOutput(std::vector<uint8_t>& bsr_data,
                    const bsdl::BSDLDevice& device,
                    const std::string& pin_name,
                    int value,
                    std::string* error = nullptr);

/// Set a pin to high impedance in a BSR staging buffer.
/// Returns false if the pin doesn't support tri-state; fills *error when non-null.
bool stagePinHighZ(std::vector<uint8_t>& bsr_data,
                   const bsdl::BSDLDevice& device,
                   const std::string& pin_name,
                   std::string* error = nullptr);

/// Read the staged output value for a pin from a BSR buffer.
/// Returns 0 (LOW), 1 (HIGH), or -1 (high-Z / pin not present).
int getStagedPinValue(const std::vector<uint8_t>& bsr_data,
                      const bsdl::BSDLDevice& device,
                      const std::string& pin_name);

/// Copy raw_bsr into bsr_data, sized and masked to match device.boundary_length.
/// Excess high bits in the last byte are zeroed. Short input is zero-padded.
void applyBsrSnapshot(std::vector<uint8_t>& bsr_data,
                      const bsdl::BSDLDevice& device,
                      const std::vector<uint8_t>& raw_bsr);

/// Fill bsr_data from BSDL safe_value fields.
/// Cells with safe_value==1 set their bit; all others clear it.
void initBsrFromSafeValues(std::vector<uint8_t>& bsr_data,
                            const bsdl::BSDLDevice& device);

/// Drives FPGA pins using EXTEST instruction and boundary scan register.
/// WARNING: EXTEST overrides normal pin function. Use with caution.
class PinDriver {
public:
    /// @param chain        JTAG chain manager
    /// @param device_index Target device index in chain
    PinDriver(JtagChain& chain, int device_index);

    /// Check if driver is properly configured.
    bool isReady() const;

    /// Whether EXTEST-based pin driving is allowed for the loaded device.
    bool extestAllowed() const;

    /// Human-readable reason why EXTEST driving is blocked.
    std::string extestBlockedReason() const;

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

    /// Capture the current device boundary state using SAMPLE and use it as
    /// the staging baseline for subsequent EXTEST writes.
    bool captureCurrentState();

    /// Replace the internal staged BSR snapshot with a raw BSR image.
    void loadSnapshot(const std::vector<uint8_t>& raw_bsr);

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

    void initBsrFromSafe();
};

} // namespace jtag
