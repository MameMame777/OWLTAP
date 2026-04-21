#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "src/jtag/jtag_chain.h"

namespace jtag {

/// Progress callback: invoked periodically during bitstream loading.
/// @param bytes_sent  Bytes sent so far
/// @param total_bytes Total payload bytes
using PlProgressCallback =
    std::function<void(size_t bytes_sent, size_t total_bytes)>;

// ---------------------------------------------------------------------------
// PL TAP instruction opcodes (6-bit, UG470 Table 10-2)
// ---------------------------------------------------------------------------
namespace pl_ir {
    constexpr uint32_t CFG_OUT  = 0x04;  // Read configuration data
    constexpr uint32_t CFG_IN   = 0x05;  // Write configuration data
    constexpr uint32_t JPROGRAM = 0x0B;  // Assert PROGRAM_B (clears PL)
    constexpr uint32_t JSTART   = 0x0C;  // Clock startup sequence
    constexpr uint32_t ISC_NOOP = 0x14;  // No-operation (idles TAP)
    constexpr uint32_t BYPASS   = 0x3F;  // Bypass register (1-bit)
}  // namespace pl_ir

// ---------------------------------------------------------------------------
// STAT register bit positions (UG470 Table 5-29)
// ---------------------------------------------------------------------------
namespace pl_stat {
    constexpr int DONE         = 14;  // DONE pin value
    constexpr int RELEASE_DONE = 13;  // Internal DONE released
    constexpr int INIT_B       = 12;  // INIT_B pin value
    constexpr int INIT_COMPLETE = 11; // Initialization complete
    constexpr int EOS          = 4;   // End of Startup
    constexpr int CRC_ERROR    = 0;   // CRC error during configuration
}  // namespace pl_stat

// ---------------------------------------------------------------------------
// Configuration packet constants (UG470 Chapter 5)
// ---------------------------------------------------------------------------
namespace pl_cfg_pkt {
    constexpr uint32_t SYNC_WORD = 0xAA995566;  // Sync word (MSB first)
    constexpr uint32_t NOOP      = 0x20000000;  // Type 1 NOP
    // Type 1 read packet header: [31:29]=001 [28:27]=01 addr[26:13] reserved[12:11] count[10:0]
    // STAT register address = 0x07, read 1 word: 0x28000000|(7<<13)|1 = 0x2800E001
    constexpr uint32_t READ_STAT = 0x2800E001;
}  // namespace pl_cfg_pkt

/// Zynq/7-series PL JTAG configuration engine.
///
/// Implements the JTAG configuration sequence from UG470 Table 10-4:
///   JPROGRAM → wait INIT_B → CFG_IN + bitstream → JSTART → verify DONE
///
/// Requires existing JtagChain with devices already detected.
/// The PL TAP must be identified by device_index in the chain.
class PlConfig {
 public:
    /// @param chain        Detected JTAG chain
    /// @param device_index Index of the PL TAP (0 = TDO-closest device)
    explicit PlConfig(JtagChain& chain, int device_index);

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    /// Program the PL from a .bit or .bin file.
    ///
    /// Executes the full sequence:
    ///   1. JPROGRAM  — clear PL configuration memory
    ///   2. wait      — clock RTI until INIT_B asserts (or fixed cycles)
    ///   3. CFG_IN    — load configuration instruction
    ///   4. DR shift  — push bit-reversed bitstream body
    ///   5. JSTART    — clock startup sequence (≥ 2000 RTI cycles)
    ///   6. verify    — read STAT register, check DONE
    ///
    /// @param bitstream_path  Path to .bit or .bin file
    /// @param cb              Optional progress callback (may be nullptr)
    /// @return true if DONE is asserted after configuration
    bool program(const std::string& bitstream_path,
                 PlProgressCallback cb = nullptr);

    // -----------------------------------------------------------------------
    // Status and polling
    // -----------------------------------------------------------------------

    /// Decoded STAT register fields.
    struct Status {
        bool done;          ///< STAT[14]: DONE pin
        bool release_done;  ///< STAT[13]: internal DONE released
        bool init_b;        ///< STAT[12]: INIT_B pin
        bool init_complete; ///< STAT[11]: initialization complete
        bool eos;           ///< STAT[4]:  End of Startup
        bool crc_error;     ///< STAT[0]:  CRC error
        uint32_t raw;       ///< Raw 32-bit STAT value
    };

    /// Read and decode the PL STAT register via JTAG (CFG_IN/CFG_OUT).
    bool readStatus(Status& out);

    /// Poll INIT_B until asserted or cycles exhausted.
    /// Used after JPROGRAM to confirm PL internal clear is done.
    /// @param timeout_cycles  Max RTI clock cycles to wait
    bool waitInitB(int timeout_cycles = 50000);

    // -----------------------------------------------------------------------
    // Bitstream loading
    // -----------------------------------------------------------------------

    /// Load a .bit or .bin bitstream file and prepare it for JTAG shifting.
    ///
    /// For .bit files: locates the sync word 0xAA995566 and discards the
    /// header preceding it.  For .bin files: uses the file as-is.
    ///
    /// Every byte is then bit-reversed so that the data can be sent via
    /// MPSSE's LSB-first shift while the PL configuration engine receives
    /// it MSB-first (as required by UG470 §6).
    ///
    /// @param path  File path (.bit or .bin)
    /// @param body  Output: processed bytes, ready to pass to shiftDR
    static bool loadBitstream(const std::string& path,
                              std::vector<uint8_t>& body);

    /// Reverse the bit order within a single byte (e.g., 0b10110000 → 0b00001101).
    static uint8_t bitReverseByte(uint8_t b);

    /// Decode raw STAT register value into Status struct.
    static Status decodeStatus(uint32_t raw);

    const std::string& lastError() const { return last_error_; }

 private:
    /// Load a PL instruction while putting other devices in BYPASS.
    bool loadInstruction(uint32_t opcode);

    JtagChain& chain_;
    int device_index_;
    std::string last_error_;
};

}  // namespace jtag
