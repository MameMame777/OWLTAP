#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/capture/capture_engine.h"

namespace jtag::protocol {

/// Supported protocol kinds.
enum class ProtocolKind {
    UART,
    SPI,
    I2C,
};

/// A single decoded protocol frame.
struct DecodedFrame {
    double start_us = 0.0;  ///< Frame start time in microseconds from capture start
    double end_us   = 0.0;  ///< Frame end time in microseconds
    ProtocolKind kind = ProtocolKind::UART;
    std::string label;      ///< Short human-readable label (e.g. "0x41 'A'")
    std::string data;       ///< Raw data bytes as hex string
    bool error_flag = false; ///< True if framing/parity/NAK error detected
};

/// Return the pin value (true=HIGH) at a sample frame, treating UNKNOWN as false.
inline bool samplePinValue(const jtag::SampleFrame& frame,
                            const std::string& pin) {
    return frame.data.getPin(pin) == jtag::PinState::HIGH;
}

/// Convert a sample timestamp to microseconds from t0.
/// Uses nanosecond resolution to avoid truncation at sub-microsecond sample rates
/// (e.g. 8 ns/sample ILA captures at 125 MHz).
inline double sampleTimeUs(const jtag::SampleFrame& frame,
                            const jtag::SampleFrame& t0_frame) {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            frame.timestamp - t0_frame.timestamp).count()) / 1000.0;
}

}  // namespace jtag::protocol
