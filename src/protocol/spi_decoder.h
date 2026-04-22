#pragma once

#include <string>
#include <vector>

#include "protocol.h"

namespace jtag::protocol {

/// SPI decoder configuration.
struct SpiConfig {
    std::string clk_pin;   ///< Clock signal name
    std::string mosi_pin;  ///< MOSI signal name (may be empty to ignore)
    std::string miso_pin;  ///< MISO signal name (may be empty to ignore)
    std::string cs_pin;    ///< Chip select signal name (active-low; empty = always active)
    bool cpol   = false;   ///< Clock polarity: false=idle LOW, true=idle HIGH
    bool cpha   = false;   ///< Clock phase: false=sample on first edge, true=on second edge
    bool lsb_first = false; ///< Bit order: false=MSB first
    int bits_per_word = 8;  ///< Bits per transfer word (1..64)
};

/// Decode SPI frames from a captured sample stream.
/// Returns one DecodedFrame per transferred word (MOSI and MISO combined).
std::vector<DecodedFrame> decodeSpi(const std::vector<jtag::SampleFrame>& samples,
                                    const SpiConfig& config);

}  // namespace jtag::protocol
