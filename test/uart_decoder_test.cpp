#include <gtest/gtest.h>

#include "src/protocol/uart_decoder.h"
#include "src/protocol/protocol.h"
#include "src/capture/capture_engine.h"

#include <chrono>

namespace {

// Build a synthetic SampleFrame vector encoding a UART byte.
// Sample rate: 1 MHz (1us per sample).
// Baud rate: 115200 → bit period = ~8.68 us, use 9 samples/bit.
static std::vector<jtag::SampleFrame> buildUartFrames(
        const std::string& pin_name, uint8_t data_byte,
        int samples_per_bit = 9) {
    std::vector<jtag::SampleFrame> frames;
    auto t0 = std::chrono::steady_clock::now();

    auto addBit = [&](bool high, int samples) {
        for (int i = 0; i < samples; i++) {
            jtag::SampleFrame sf;
            sf.timestamp = t0 + std::chrono::microseconds(frames.size());
            sf.data.pin_states[pin_name] = high ? jtag::PinState::HIGH : jtag::PinState::LOW;
            sf.trigger_point = false;
            frames.push_back(sf);
        }
    };

    // Idle HIGH
    addBit(true, samples_per_bit * 2);

    // Start bit LOW
    addBit(false, samples_per_bit);

    // 8 data bits, LSB first
    for (int b = 0; b < 8; b++) {
        addBit((data_byte >> b) & 1, samples_per_bit);
    }

    // Stop bit HIGH
    addBit(true, samples_per_bit * 2);

    return frames;
}

TEST(UartDecoderTest, DecodeByteA) {
    const std::string pin = "RX";
    const uint8_t byte_val = 0x41;  // 'A'
    const int spb = 9;
    // baud_rate * spb ≈ 1000000 (1 MHz sample rate)
    const uint32_t baud = 1000000 / spb;  // ~111111

    auto frames = buildUartFrames(pin, byte_val, spb);

    jtag::protocol::UartConfig cfg;
    cfg.rx_pin      = pin;
    cfg.baud_rate   = baud;
    cfg.data_bits   = 8;
    cfg.stop_bits   = 1;
    cfg.parity_enable = false;

    auto decoded = jtag::protocol::decodeUart(frames, cfg);

    ASSERT_FALSE(decoded.empty());
    EXPECT_FALSE(decoded[0].error_flag);
    // Label should contain hex representation
    EXPECT_NE(decoded[0].label.find("41"), std::string::npos);
    // data field is hex string
    EXPECT_EQ(decoded[0].data, "41");
}

TEST(UartDecoderTest, DecodeByteFF) {
    const std::string pin = "RX";
    const int spb = 9;
    const uint32_t baud = 1000000 / spb;

    auto frames = buildUartFrames(pin, 0xFF, spb);

    jtag::protocol::UartConfig cfg;
    cfg.rx_pin    = pin;
    cfg.baud_rate = baud;
    cfg.data_bits = 8;

    auto decoded = jtag::protocol::decodeUart(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    EXPECT_EQ(decoded[0].data, "FF");
}

TEST(UartDecoderTest, EmptyFrames) {
    jtag::protocol::UartConfig cfg;
    cfg.rx_pin    = "RX";
    cfg.baud_rate = 115200;
    auto decoded  = jtag::protocol::decodeUart({}, cfg);
    EXPECT_TRUE(decoded.empty());
}

}  // namespace
