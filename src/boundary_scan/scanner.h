#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "src/bsdl/bsdl_model.h"
#include "src/jtag/jtag_chain.h"

namespace jtag {

/// Observed state of a single pin
enum class PinState : uint8_t {
    LOW = 0,
    HIGH = 1,
    UNKNOWN = 2,
};

/// Result of a boundary scan sample operation
struct ScanResult {
    /// Raw BSR bit vector
    std::vector<uint8_t> raw_bsr;

    /// Pin states decoded from BSR, keyed by pin name
    std::map<std::string, PinState> pin_states;

    /// Get state of a specific pin
    PinState getPin(const std::string& name) const {
        auto it = pin_states.find(name);
        return (it != pin_states.end()) ? it->second : PinState::UNKNOWN;
    }

    /// Get a bit from the raw BSR
    bool getBit(int position) const {
        if (position < 0) return false;
        int byte_idx = position / 8;
        int bit_idx = position % 8;
        if (byte_idx >= static_cast<int>(raw_bsr.size())) return false;
        return (raw_bsr[byte_idx] >> bit_idx) & 1;
    }
};

/// Decode named pin states from a raw BSR snapshot using BSDL metadata.
/// INPUT/BIDIR/CLOCK cells take priority over output-only cells for the same pin.
ScanResult decodeBoundaryScan(const bsdl::BSDLDevice& device,
                              std::vector<uint8_t> raw_bsr);

/// Boundary scan operations: reads pin states using SAMPLE instruction.
class Scanner {
public:
    /// @param chain        JTAG chain manager
    /// @param device_index Target device index in chain
    Scanner(JtagChain& chain, int device_index);

    /// Check if scanner is properly configured (BSDL loaded, etc.)
    bool isReady() const;

    /// Read the device IDCODE.
    /// @param idcode  Output: 32-bit IDCODE value
    /// @return true on success
    bool readIdCode(uint32_t& idcode);

    /// Verify device IDCODE matches BSDL.
    /// @return true if IDCODE matches
    bool verifyIdCode();

    /// Perform a single SAMPLE operation.
    /// Loads SAMPLE instruction, reads BSR, decodes pin states.
    /// @return ScanResult with decoded pin states
    ScanResult sample();

    /// Get list of all observable (input) pin names.
    std::vector<std::string> getObservablePins() const;

    /// Get list of all drivable (output) pin names.
    std::vector<std::string> getDrivablePins() const;

    /// Get the BSDL device data.
    const bsdl::BSDLDevice* bsdlDevice() const;

    /// Get last error message.
    const std::string& lastError() const { return last_error_; }

private:
    JtagChain& chain_;
    int device_index_;
    std::string last_error_;
};

} // namespace jtag
