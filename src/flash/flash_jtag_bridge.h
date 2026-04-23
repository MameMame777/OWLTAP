#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/jtag/jtag_chain.h"

namespace jtag::flash {

/// Abstract SPI transport for a flash device reached through a JTAG BSCAN
/// bridge.  Allows unit tests to substitute a recording fake.
class IFlashBridge {
 public:
    virtual ~IFlashBridge() = default;

    /// Perform a single SPI transaction (CS asserted for the whole call).
    ///
    /// @param tx  Bytes to clock out on MOSI (MSB-first on the wire).
    /// @param rx  Bytes captured from MISO; resized to tx.size().
    ///            On return, rx[i] contains the byte received while tx[i] was
    ///            being shifted out.
    /// @return true on success.
    virtual bool transfer(const std::vector<uint8_t>& tx,
                          std::vector<uint8_t>& rx) = 0;

    /// Diagnostic message for the last failed operation.
    virtual const std::string& lastError() const = 0;
};

/// Drives SPI flash through the quartiq BSCAN SPI bridge bitstream loaded into
/// the Zynq PL TAP.  The bridge exposes a shifter on USER1 DR: entering
/// SHIFT-DR asserts CS_n low, exiting releases CS_n.
///
/// SPI data on the wire is MSB-first, MPSSE shifts LSB-first — this class
/// bit-reverses every byte in both directions so callers work in natural
/// MSB-first byte values.
class FlashJtagBridge : public IFlashBridge {
 public:
    /// @param chain        Detected JTAG chain.
    /// @param device_index Index of the PL TAP (the device whose USER1 DR
    ///                     routes to the flash controller in the bridge
    ///                     bitstream).
    FlashJtagBridge(JtagChain& chain, int device_index);

    bool transfer(const std::vector<uint8_t>& tx,
                  std::vector<uint8_t>& rx) override;

    const std::string& lastError() const override { return last_error_; }

    /// IR opcode of the BSCAN USER1 register on 7-series / Zynq TAPs.
    static constexpr uint32_t kUser1Ir = 0x02;

 private:
    JtagChain& chain_;
    int device_index_;
    bool user1_selected_ = false;  ///< Avoid re-loading IR between transfers.
    std::string last_error_;
};

}  // namespace jtag::flash
