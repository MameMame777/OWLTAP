#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "src/jtag/jtag_chain.h"

namespace jtag::flash {

/// Phase of a flash programming operation (reported to the progress callback).
enum class FlashPhase {
    BRIDGE_LOAD,  ///< Loading BSCAN SPI bridge bitstream into PL
    ERASE,        ///< Bulk erase in progress
    PROGRAM,      ///< Page programming
    VERIFY,       ///< Read-back verify
};

/// Progress callback invoked periodically during programming.
/// @param phase    Current operation phase.
/// @param done     Bytes completed within the current phase (0 for ERASE).
/// @param total    Total bytes for the current phase (0 for ERASE).
using FlashProgressCallback =
    std::function<void(FlashPhase phase, std::size_t done, std::size_t total)>;

/// End-to-end programmer for a Zynq-7000 board equipped with a Micron
/// MT25QL128 SPI config ROM.  The caller supplies an already-detected
/// JtagChain; this class loads the BSCAN SPI bridge bitstream into the PL,
/// then drives erase/program/verify through that bridge.
class FlashProgrammer {
 public:
    /// @param chain            Detected JTAG chain.
    /// @param pl_device_index  Index of the PL TAP (Zynq PL typically 0).
    /// @param bridge_bit_path  Path to bscan_spi_xc7z020.bit (quartiq).
    FlashProgrammer(JtagChain& chain,
                    int pl_device_index,
                    std::string bridge_bit_path);

    /// Execute Erase → Program → Verify for @p image_path.
    ///
    /// @param image_path  Flash image file.  Supported formats:
    ///                    - `.bin`  Raw binary (Vivado `write_cfgmem -format BIN`)
    ///                    - `.mcs`  Intel HEX (Vivado `write_cfgmem -format MCS`)
    ///                    - `.hex`  Intel HEX (same format as .mcs)
    ///                    The format is detected by file extension.
    /// @param cb          Optional progress callback.
    /// @return true iff the verify pass succeeds.
    bool program(const std::string& image_path,
                 FlashProgressCallback cb = nullptr);

    const std::string& lastError() const { return last_error_; }

 private:
    JtagChain&   chain_;
    int          pl_device_index_;
    std::string  bridge_bit_path_;
    std::string  last_error_;
};

}  // namespace jtag::flash
