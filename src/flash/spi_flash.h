#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "flash_jtag_bridge.h"

namespace jtag::flash {

/// Driver for Micron MT25QL128 128 Mbit SPI NOR flash (3-byte addressing,
/// bulk erase only, 256-byte page program).  Datasheet: Micron MT25QL128ABA.
///
/// All commands use 1-bit (standard) SPI.  No quad, no 4-byte addressing.
class Mt25qFlash {
 public:
    // Identification / geometry constants.
    static constexpr uint32_t kJedecMt25ql128 = 0x20BA18;  ///< Manufacturer 0x20 (Micron), mem-type 0xBA (3.3 V), density 0x18 (128 Mb).
    static constexpr size_t   kPageSize       = 256;
    static constexpr size_t   kCapacityBytes  = 16u * 1024u * 1024u;

    // SPI command opcodes used by this driver.
    static constexpr uint8_t kCmdReadId       = 0x9F;
    static constexpr uint8_t kCmdReadStatus   = 0x05;
    static constexpr uint8_t kCmdWriteEnable  = 0x06;
    static constexpr uint8_t kCmdRead         = 0x03;
    static constexpr uint8_t kCmdPageProgram  = 0x02;
    static constexpr uint8_t kCmdBulkErase    = 0xC7;

    // Status register bits.
    static constexpr uint8_t kStatusWip       = 0x01;  ///< Write In Progress
    static constexpr uint8_t kStatusWel       = 0x02;  ///< Write Enable Latch

    explicit Mt25qFlash(IFlashBridge& bridge);

    /// Read 3-byte JEDEC ID (0x9F command).
    /// @param jedec  Output: big-endian 24-bit value ((mfg<<16)|(type<<8)|cap).
    bool readId(uint32_t& jedec);

    /// Read the 8-bit status register (0x05).
    bool readStatus(uint8_t& sr);

    /// Latch WEL.  Required before every erase or program.
    bool writeEnable();

    /// Poll the status register until WIP clears.
    /// @param timeout  Maximum wall-clock wait before giving up.
    bool waitWipClear(std::chrono::milliseconds timeout);

    /// Bulk chip erase (0xC7).  Blocks until WIP clears.
    /// MT25QL128 typical 250 s; caller should pass a generous timeout.
    bool bulkErase(std::chrono::milliseconds timeout);

    /// Program one page (≤ 256 bytes).  Address must be page-aligned or the
    /// flash wraps within the page — the caller must respect page alignment.
    /// @param addr 24-bit byte address (0..kCapacityBytes-1).
    /// @param data Source bytes.
    /// @param n    Byte count (1..kPageSize).
    bool pageProgram(uint32_t addr, const uint8_t* data, size_t n,
                     std::chrono::milliseconds timeout);

    /// Read @p n bytes starting at @p addr into @p out.
    bool read(uint32_t addr, uint8_t* out, size_t n);

    const std::string& lastError() const { return last_error_; }

 private:
    IFlashBridge& bridge_;
    std::string   last_error_;
};

}  // namespace jtag::flash
