#pragma once

#include <cstdint>
#include <map>
#include <set>
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

    /// Set pin decode filter: only these names will be stored in ScanResult::pin_states.
    /// Pass empty vector to disable filtering (decode all pins, the default).
    void setDecodeFilter(const std::vector<std::string>& names);

    /// Returns the number of pins currently in the decode filter (0 = no filter).
    size_t decodeFilterSize() const { return decode_filter_.size(); }

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
    std::set<std::string> decode_filter_;  // empty = decode all pins
    std::string last_error_;

    /// Returns the minimum number of BSR bits that must be shifted out to cover
    /// all cells of the pins in decode_filter_.  Returns 0 when the filter is
    /// empty (meaning: read the full BSR).
    int maxNeededBsrBits() const;
};

} // namespace jtag
