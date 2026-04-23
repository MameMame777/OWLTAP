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

// ---------------------------------------------------------------------------
// Extended frame builder: optional parity, bad stop bit, parity flip.
// Parity is computed correctly unless flip_parity=true (injects error).
// bad_stop=true drives the stop bit LOW (framing error).
// ---------------------------------------------------------------------------
static std::vector<jtag::SampleFrame> buildUartFramesEx(
        const std::string& pin, uint8_t data_byte, int spb,
        bool parity_enable, bool parity_odd,
        bool bad_stop = false, bool flip_parity = false) {
    std::vector<jtag::SampleFrame> frames;
    auto t0 = std::chrono::steady_clock::now();
    auto addBit = [&](bool high, int n) {
        for (int i = 0; i < n; i++) {
            jtag::SampleFrame sf;
            sf.timestamp = t0 + std::chrono::microseconds(frames.size());
            sf.data.pin_states[pin] = high ? jtag::PinState::HIGH : jtag::PinState::LOW;
            frames.push_back(sf);
        }
    };
    addBit(true, spb * 2);          // idle
    addBit(false, spb);             // start bit
    int ones = 0;
    for (int b = 0; b < 8; b++) {
        bool bit = (data_byte >> b) & 1;
        if (bit) ones++;
        addBit(bit, spb);
    }
    if (parity_enable) {
        // correct parity, then optionally flip to inject error
        bool pb = parity_odd ? ((ones % 2) == 0) : ((ones % 2) == 1);
        if (flip_parity) pb = !pb;
        addBit(pb, spb);
    }
    addBit(!bad_stop, spb * 2);     // stop bit (HIGH = OK, LOW = framing error)
    return frames;
}

// Two bytes back-to-back with a shared time base.
static std::vector<jtag::SampleFrame> buildTwoUartBytes(
        const std::string& pin, uint8_t b1, uint8_t b2, int spb) {
    std::vector<jtag::SampleFrame> frames;
    auto t0 = std::chrono::steady_clock::now();
    auto addBit = [&](bool high, int n) {
        for (int i = 0; i < n; i++) {
            jtag::SampleFrame sf;
            sf.timestamp = t0 + std::chrono::microseconds(frames.size());
            sf.data.pin_states[pin] = high ? jtag::PinState::HIGH : jtag::PinState::LOW;
            frames.push_back(sf);
        }
    };
    auto encodeFrame = [&](uint8_t byte) {
        addBit(false, spb);
        for (int b = 0; b < 8; b++) addBit((byte >> b) & 1, spb);
        addBit(true, spb);
    };
    addBit(true, spb * 2);
    encodeFrame(b1);
    addBit(true, spb);       // inter-frame gap
    encodeFrame(b2);
    addBit(true, spb * 2);
    return frames;
}

// ---------------------------------------------------------------------------
// New tests
// ---------------------------------------------------------------------------

TEST(UartDecoderTest, EvenParityCorrect) {
    // 0x41 = 01000001 → 2 ones → even parity bit = 0 → no error
    const std::string pin = "RX";
    const int spb = 9;
    const uint32_t baud = 1000000u / static_cast<uint32_t>(spb);
    auto frames = buildUartFramesEx(pin, 0x41, spb,
                                    /*parity_enable=*/true, /*parity_odd=*/false);
    jtag::protocol::UartConfig cfg;
    cfg.rx_pin = pin; cfg.baud_rate = baud; cfg.data_bits = 8;
    cfg.parity_enable = true; cfg.parity_odd = false;
    auto decoded = jtag::protocol::decodeUart(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    EXPECT_FALSE(decoded[0].error_flag);
    EXPECT_EQ(decoded[0].data, "41");
}

TEST(UartDecoderTest, ParityError) {
    // 0x55 = 01010101 → 4 ones → even parity bit = 0
    // We flip parity bit → mismatch → error_flag = true
    const std::string pin = "RX";
    const int spb = 9;
    const uint32_t baud = 1000000u / static_cast<uint32_t>(spb);
    auto frames = buildUartFramesEx(pin, 0x55, spb,
                                    /*parity_enable=*/true, /*parity_odd=*/false,
                                    /*bad_stop=*/false, /*flip_parity=*/true);
    jtag::protocol::UartConfig cfg;
    cfg.rx_pin = pin; cfg.baud_rate = baud; cfg.data_bits = 8;
    cfg.parity_enable = true; cfg.parity_odd = false;
    auto decoded = jtag::protocol::decodeUart(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    EXPECT_TRUE(decoded[0].error_flag);
}

TEST(UartDecoderTest, FramingError) {
    // Stop bit is LOW → framing error
    const std::string pin = "RX";
    const int spb = 9;
    const uint32_t baud = 1000000u / static_cast<uint32_t>(spb);
    auto frames = buildUartFramesEx(pin, 0x42, spb,
                                    /*parity_enable=*/false, /*parity_odd=*/false,
                                    /*bad_stop=*/true);
    jtag::protocol::UartConfig cfg;
    cfg.rx_pin = pin; cfg.baud_rate = baud; cfg.data_bits = 8;
    auto decoded = jtag::protocol::decodeUart(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    EXPECT_TRUE(decoded[0].error_flag);
}

TEST(UartDecoderTest, TwoConsecutiveBytes) {
    const std::string pin = "RX";
    const int spb = 9;
    const uint32_t baud = 1000000u / static_cast<uint32_t>(spb);
    auto frames = buildTwoUartBytes(pin, 0xAB, 0xCD, spb);
    jtag::protocol::UartConfig cfg;
    cfg.rx_pin = pin; cfg.baud_rate = baud; cfg.data_bits = 8;
    auto decoded = jtag::protocol::decodeUart(frames, cfg);
    ASSERT_EQ(decoded.size(), 2u);
    EXPECT_EQ(decoded[0].data, "AB");
    EXPECT_EQ(decoded[1].data, "CD");
}

}  // namespace
