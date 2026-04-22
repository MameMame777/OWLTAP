#pragma once

#include <string>
#include <vector>

#include "protocol.h"

namespace jtag::protocol {

/// UART decoder configuration.
struct UartConfig {
    std::string rx_pin;         ///< Signal name for RX data line
    uint32_t baud_rate  = 9600; ///< Baud rate in bits/s
    int data_bits       = 8;    ///< 5..8
    int stop_bits       = 1;    ///< 1 or 2
    bool parity_enable  = false;
    bool parity_odd     = false; ///< true=odd parity, false=even parity
};

/// Decode UART frames from a captured sample stream.
/// Samples must be ordered by timestamp.
/// Returns one DecodedFrame per received data byte (or framing error).
std::vector<DecodedFrame> decodeUart(const std::vector<jtag::SampleFrame>& samples,
                                     const UartConfig& config);

}  // namespace jtag::protocol
