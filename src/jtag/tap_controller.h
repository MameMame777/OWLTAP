#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/ftdi/ftdi_device.h"
#include "src/ftdi/mpsse.h"

namespace jtag {

/// JTAG TAP controller states (IEEE 1149.1)
enum class TapState : int {
    TEST_LOGIC_RESET = 0,
    RUN_TEST_IDLE    = 1,
    SELECT_DR_SCAN   = 2,
    CAPTURE_DR       = 3,
    SHIFT_DR         = 4,
    EXIT1_DR         = 5,
    PAUSE_DR         = 6,
    EXIT2_DR         = 7,
    UPDATE_DR        = 8,
    SELECT_IR_SCAN   = 9,
    CAPTURE_IR       = 10,
    SHIFT_IR         = 11,
    EXIT1_IR         = 12,
    PAUSE_IR         = 13,
    EXIT2_IR         = 14,
    UPDATE_IR        = 15,
    NUM_STATES       = 16,
};

/// Get human-readable name for a TAP state
const char* tapStateName(TapState state);

/// Compute the TMS bit sequence to transition from one state to another.
/// Returns the number of bits in the path, and fills tms_bits (LSB first).
/// Maximum path length is 7 bits.
int computeTmsPath(TapState from, TapState to, uint8_t& tms_bits);

/// High-level TAP controller that manages state transitions and
/// IR/DR shift operations via an FtdiDevice.
class TapController {
public:
    /// @param device  Reference to an opened FtdiDevice in MPSSE mode
    explicit TapController(FtdiDevice& device);

    /// Get current TAP state
    TapState currentState() const { return state_; }

    /// Force TAP to TEST-LOGIC-RESET by clocking TMS=1 five times.
    /// @return true on success
    bool reset();

    /// Clock RUN-TEST-IDLE for N cycles (required before DR scan on Zynq).
    /// @param cycles  Number of TCK cycles to clock in RTI
    /// @return true on success
    bool clkIdle(int cycles);

    /// Transition TAP to specified state.
    /// @return true on success
    bool gotoState(TapState target);

    /// Shift data into the Instruction Register.
    /// Moves to SHIFT-IR, shifts data, then moves to RUN-TEST-IDLE.
    /// @param ir_data  Instruction data (LSB first)
    /// @param ir_bits  Number of bits in IR
    /// @return true on success
    bool shiftIR(const uint8_t* ir_data, int ir_bits);

    /// Shift data through the Data Register with readback.
    /// Moves to SHIFT-DR, shifts data in/out, then moves to RUN-TEST-IDLE.
    /// @param tdi_data  Data to shift in via TDI (LSB first)
    /// @param tdo_data  Output: data captured from TDO (LSB first)
    /// @param dr_bits   Number of bits to shift
    /// @return true on success
    bool shiftDR(const uint8_t* tdi_data, std::vector<uint8_t>& tdo_data,
                 int dr_bits);

    /// Shift data through the Data Register without readback.
    /// Moves to SHIFT-DR, shifts data out, then moves to RUN-TEST-IDLE.
    /// @param tdi_data  Data to shift in via TDI (LSB first)
    /// @param dr_bits   Number of bits to shift
    /// @return true on success
    bool shiftDR(const uint8_t* tdi_data, int dr_bits);

    /// Read data from Data Register (shift in zeros on TDI).
    /// Moves to SHIFT-DR, reads DR, then moves to RUN-TEST-IDLE.
    /// @param tdo_data  Output: data captured from TDO (LSB first)
    /// @param dr_bits   Number of bits to read
    /// @return true on success
    bool readDR(std::vector<uint8_t>& tdo_data, int dr_bits);

    /// Perform @p count identical DR scans of @p dr_bits bits (same TDI each
    /// scan), batched into a single MPSSE USB transfer per chunk. Significantly
    /// faster than calling shiftDR in a loop because USB round-trips are reduced
    /// from O(count) to O(count/kChunk).
    ///
    /// @param count     Number of scans to perform
    /// @param dr_bits   Bits per scan
    /// @param tdi       TDI data, ceil(dr_bits/8) bytes, same for every scan.
    ///                  May be nullptr (shifts zeros).
    /// @param flat_tdo  Output: count × ceil(dr_bits/8) bytes, TDO in order.
    /// @return true on success; TAP is left in RUN-TEST-IDLE.
    bool shiftDRRepeat(int count, int dr_bits, const uint8_t* tdi,
                       std::vector<uint8_t>& flat_tdo);

    /// Get last error message
    const std::string& lastError() const { return last_error_; }

private:
    /// Execute a shift operation in the current SHIFT state.
    /// Handles the last-bit TMS=1 transition for exit.
    bool doShift(const uint8_t* tdi_data, std::vector<uint8_t>* tdo_data,
                 int bit_count, bool read_back);

    FtdiDevice& device_;
    TapState state_ = TapState::TEST_LOGIC_RESET;
    std::string last_error_;
};

} // namespace jtag
