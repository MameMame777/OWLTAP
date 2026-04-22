#pragma once

#include <string>
#include <vector>

#include "protocol.h"

namespace jtag::protocol {

/// I2C decoder configuration.
struct I2cConfig {
    std::string scl_pin;  ///< SCL signal name
    std::string sda_pin;  ///< SDA signal name
};

/// I2C frame types encoded in DecodedFrame::label.
/// Label format: "START", "ADDR 0x4A W", "ADDR 0x4A R", "DATA 0x42",
///               "ACK", "NAK", "STOP"

/// Decode I2C frames from a captured sample stream.
/// Returns one DecodedFrame per I2C bus event (START, address, data byte, ACK/NAK, STOP).
std::vector<DecodedFrame> decodeI2c(const std::vector<jtag::SampleFrame>& samples,
                                    const I2cConfig& config);

}  // namespace jtag::protocol
