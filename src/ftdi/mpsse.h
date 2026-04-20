#pragma once

#include <cstdint>
#include <vector>

namespace jtag {

// MPSSE command bytes for JTAG operations
namespace mpsse_cmd {
    // Clock data bytes out on -ve clock edge, MSB first
    constexpr uint8_t MPSSE_WRITE_NEG = 0x11;
    // Clock data bytes in on +ve clock edge, MSB first
    constexpr uint8_t MPSSE_READ_POS = 0x20;
    // Clock data bytes in/out on +/-ve clock edge, MSB first
    constexpr uint8_t MPSSE_RDWR = 0x31;

    // Clock data bits out on -ve clock edge, LSB first
    constexpr uint8_t MPSSE_WRITE_BITS_NEG_LSB = 0x1B;
    // Clock data bits in on +ve clock edge, LSB first
    constexpr uint8_t MPSSE_READ_BITS_POS_LSB = 0x2A;
    // Clock data bits in/out, LSB first
    constexpr uint8_t MPSSE_RDWR_BITS_LSB = 0x3B;

    // Clock data bytes out on -ve edge, LSB first
    constexpr uint8_t MPSSE_WRITE_NEG_LSB = 0x19;
    // Clock data bytes in on +ve edge, LSB first
    constexpr uint8_t MPSSE_READ_POS_LSB = 0x28;
    // Clock data bytes in/out, LSB first
    constexpr uint8_t MPSSE_RDWR_LSB = 0x39;

    // Clock TMS bits out on -ve edge (bit mode, LSB first)
    constexpr uint8_t MPSSE_TMS_OUT = 0x4B;
    // Clock TMS bits out + read TDO, -ve edge (bit mode)
    constexpr uint8_t MPSSE_TMS_RDWR = 0x6B;

    // Set data bits low byte (ADBUS)
    constexpr uint8_t SET_BITS_LOW = 0x80;
    // Set data bits high byte (ACBUS)
    constexpr uint8_t SET_BITS_HIGH = 0x82;
    // Read data bits low byte
    constexpr uint8_t GET_BITS_LOW = 0x81;
    // Read data bits high byte
    constexpr uint8_t GET_BITS_HIGH = 0x83;

    // Set TCK divisor
    constexpr uint8_t TCK_DIVISOR = 0x86;
    // Disable clock divide by 5 (FT2232H/FT4232H/FT232H)
    constexpr uint8_t DIS_DIV_5 = 0x8A;
    // Enable clock divide by 5
    constexpr uint8_t EN_DIV_5 = 0x8B;
    // Enable 3-phase data clocking
    constexpr uint8_t EN_3_PHASE = 0x8C;
    // Disable 3-phase data clocking
    constexpr uint8_t DIS_3_PHASE = 0x8D;
    // Disconnect TDI/DO loopback
    constexpr uint8_t LOOPBACK_END = 0x85;
    // Send immediate (flush USB buffer)
    constexpr uint8_t SEND_IMMEDIATE = 0x87;
    // Bad command (for sync detection)
    constexpr uint8_t BAD_COMMAND = 0xAB;
}

// JTAG pin assignments on FTDI ADBUS
namespace jtag_pins {
    constexpr uint8_t TCK = 0x01; // ADBUS0
    constexpr uint8_t TDI = 0x02; // ADBUS1
    constexpr uint8_t TDO = 0x04; // ADBUS2
    constexpr uint8_t TMS = 0x08; // ADBUS3
    // ADBUS7: output-enable for Digilent board JTAG buffers.
    // Must be driven HIGH or TCK/TDI/TMS never reach the FPGA.
    // (OpenOCD Zybo config: ftdi_layout_init 0x0088 0x008b)
    constexpr uint8_t DIGILENT_OE = 0x80; // ADBUS7
    // Output pins mask (TCK, TDI, TMS, DIGILENT_OE are outputs; TDO is input)
    constexpr uint8_t OUTPUT_MASK = TCK | TDI | TMS | DIGILENT_OE; // 0x8B
    // Initial pin values: TMS=1 (idle), DIGILENT_OE=1 (buffers enabled)
    constexpr uint8_t INIT_VALUE  = TMS | DIGILENT_OE;             // 0x88
}

/// Command buffer for building MPSSE command sequences.
/// Commands are accumulated in memory and flushed to the device together
/// for efficient USB transfers.
class MpsseCommandBuffer {
public:
    MpsseCommandBuffer();

    /// Reset the buffer
    void clear();

    /// Get the raw command data
    const std::vector<uint8_t>& data() const { return buffer_; }
    size_t size() const { return buffer_.size(); }
    bool empty() const { return buffer_.empty(); }

    /// Number of bytes expected to be read back from device
    size_t expectedReadBytes() const { return expected_read_bytes_; }

    /// Clock TMS bits (1-7 bits per call).
    /// Used for TAP state machine transitions.
    /// @param tms_bits  TMS bit pattern, LSB clocked first
    /// @param bit_count Number of bits to clock (1-7)
    /// @param tdi_value TDI value to hold during TMS clocking (0 or 1)
    /// @param read_tdo  If true, capture TDO during last bit
    void clockTms(uint8_t tms_bits, int bit_count, bool tdi_value = false,
                  bool read_tdo = false);

    /// Shift data out on TDI (bytes), LSB first, no TDO read.
    /// @param data     Pointer to data bytes
    /// @param bit_count Total number of bits to shift
    void shiftOut(const uint8_t* data, int bit_count);

    /// Shift data in from TDO (bytes), LSB first, no TDI drive.
    /// @param bit_count Total number of bits to shift in
    void shiftIn(int bit_count);

    /// Shift data in/out simultaneously (TDI out, TDO in), LSB first.
    /// @param tdi_data  Pointer to TDI data bytes
    /// @param bit_count Total number of bits to shift
    void shiftInOut(const uint8_t* tdi_data, int bit_count);

    /// Set ADBUS pin values and direction.
    /// @param value Pin output values
    /// @param direction Pin directions (1 = output, 0 = input)
    void setLowBits(uint8_t value, uint8_t direction);

    /// Read ADBUS pin values.
    void getLowBits();

    /// Set TCK frequency.
    /// @param divisor  Clock divisor: freq = 60MHz / ((1 + divisor) * 2)
    ///                 For 6MHz: divisor = 4
    ///                 For 1MHz: divisor = 29
    void setClockDivisor(uint16_t divisor);

    /// Disable /5 clock divider (use 60MHz base clock).
    void disableClockDivide5();

    /// Enable /5 clock divider (use 12MHz base clock).
    void enableClockDivide5();

    /// Enable adaptive clocking (RTCK).
    void enable3PhaseClocking();

    /// Disable adaptive clocking.
    void disable3PhaseClocking();

    /// Disconnect internal TDI/TDO loopback.
    void disableLoopback();

    /// Request immediate USB flush.
    void sendImmediate();

private:
    void appendByte(uint8_t b);
    void appendBytes(const uint8_t* data, size_t len);

    std::vector<uint8_t> buffer_;
    size_t expected_read_bytes_ = 0;
};

} // namespace jtag
