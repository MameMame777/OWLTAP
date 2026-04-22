#include <gtest/gtest.h>

#include "src/protocol/spi_decoder.h"
#include "src/protocol/protocol.h"
#include "src/capture/capture_engine.h"

#include <chrono>

namespace {

// Build synthetic SPI frames encoding one byte on MOSI.
// Mode 0: CPOL=0, CPHA=0 — data captured on rising edge.
static std::vector<jtag::SampleFrame> buildSpiFrames(
        const std::string& clk_pin,
        const std::string& mosi_pin,
        const std::string& cs_pin,
        uint8_t data_byte,
        int samples_per_half_bit = 5) {
    std::vector<jtag::SampleFrame> frames;
    auto t0 = std::chrono::steady_clock::now();

    auto addSample = [&](bool clk, bool mosi, bool cs_n) {
        jtag::SampleFrame sf;
        sf.timestamp = t0 + std::chrono::microseconds(frames.size());
        sf.data.pin_states[clk_pin]  = clk   ? jtag::PinState::HIGH : jtag::PinState::LOW;
        sf.data.pin_states[mosi_pin] = mosi  ? jtag::PinState::HIGH : jtag::PinState::LOW;
        sf.data.pin_states[cs_pin]   = cs_n  ? jtag::PinState::HIGH : jtag::PinState::LOW;
        sf.trigger_point = false;
        frames.push_back(sf);
    };

    auto addSamples = [&](int n, bool clk, bool mosi, bool cs_n) {
        for (int i = 0; i < n; i++) addSample(clk, mosi, cs_n);
    };

    // Idle: CS=1, CLK=0
    addSamples(samples_per_half_bit, false, false, true);

    // CS assert (CS_N=0)
    addSamples(samples_per_half_bit, false, false, false);

    // 8 bits MSB first
    for (int b = 7; b >= 0; b--) {
        bool bit = (data_byte >> b) & 1;
        // Setup data with CLK low
        addSamples(samples_per_half_bit, false, bit, false);
        // Rising edge → sample point (CPHA=0)
        addSamples(samples_per_half_bit, true, bit, false);
    }

    // Deassert CS
    addSamples(samples_per_half_bit, false, false, true);

    return frames;
}

TEST(SpiDecoderTest, DecodeA5) {
    const std::string clk  = "CLK";
    const std::string mosi = "MOSI";
    const std::string cs   = "CS_N";

    auto frames = buildSpiFrames(clk, mosi, cs, 0xA5);

    jtag::protocol::SpiConfig cfg;
    cfg.clk_pin       = clk;
    cfg.mosi_pin      = mosi;
    cfg.cs_pin        = cs;
    cfg.cpol          = false;
    cfg.cpha          = false;
    cfg.lsb_first     = false;
    cfg.bits_per_word = 8;

    auto decoded = jtag::protocol::decodeSpi(frames, cfg);

    ASSERT_FALSE(decoded.empty());
    // Label encodes decoded value
    EXPECT_NE(decoded[0].label.find("A5"), std::string::npos);
}

TEST(SpiDecoderTest, DecodeByte00) {
    const std::string clk  = "CLK";
    const std::string mosi = "MOSI";
    const std::string cs   = "CS_N";

    auto frames = buildSpiFrames(clk, mosi, cs, 0x00);

    jtag::protocol::SpiConfig cfg;
    cfg.clk_pin  = clk;
    cfg.mosi_pin = mosi;
    cfg.cs_pin   = cs;

    auto decoded = jtag::protocol::decodeSpi(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    // 0x00 — label should contain all zeros
    EXPECT_NE(decoded[0].label.find("00"), std::string::npos);
}

TEST(SpiDecoderTest, EmptyFrames) {
    jtag::protocol::SpiConfig cfg;
    cfg.clk_pin  = "CLK";
    cfg.mosi_pin = "MOSI";
    auto decoded = jtag::protocol::decodeSpi({}, cfg);
    EXPECT_TRUE(decoded.empty());
}

}  // namespace
