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

// ---------------------------------------------------------------------------
// Generic SPI frame builder: supports all 4 CPOL/CPHA modes, multi-byte,
// and LSB-first bit order.
//
// Derivation of clock phasing:
//   first_half_clk = cpol ^ cpha
//   - Mode 0 (cpol=0,cpha=0): first=LOW  → rising edge = sample  (CPHA=0,leading)
//   - Mode 1 (cpol=0,cpha=1): first=HIGH → falling edge = sample (CPHA=1,trailing)
//   - Mode 2 (cpol=1,cpha=0): first=HIGH → falling edge = sample (CPHA=0,leading)
//   - Mode 3 (cpol=1,cpha=1): first=LOW  → rising edge = sample  (CPHA=1,trailing)
// ---------------------------------------------------------------------------
static std::vector<jtag::SampleFrame> buildSpiFramesGeneric(
        const std::string& clk_pin, const std::string& mosi_pin,
        const std::string& cs_pin, const std::vector<uint8_t>& bytes,
        bool cpol, bool cpha, bool lsb_first = false, int spb = 5) {
    std::vector<jtag::SampleFrame> frames;
    auto t0 = std::chrono::steady_clock::now();
    auto addN = [&](int n, bool clk, bool mosi, bool cs_n) {
        for (int i = 0; i < n; i++) {
            jtag::SampleFrame sf;
            sf.timestamp = t0 + std::chrono::microseconds(frames.size());
            sf.data.pin_states[clk_pin]  = clk  ? jtag::PinState::HIGH : jtag::PinState::LOW;
            sf.data.pin_states[mosi_pin] = mosi ? jtag::PinState::HIGH : jtag::PinState::LOW;
            sf.data.pin_states[cs_pin]   = cs_n ? jtag::PinState::HIGH : jtag::PinState::LOW;
            frames.push_back(sf);
        }
    };
    const bool fh = cpol ^ cpha;   // clock level during data-setup half-period
    addN(spb, cpol, false, true);   // idle
    addN(spb, cpol, false, false);  // CS assert
    for (uint8_t byte : bytes) {
        for (int b = 0; b < 8; b++) {
            int bit_idx = lsb_first ? b : (7 - b);
            bool bit = (byte >> bit_idx) & 1;
            addN(spb,  fh, bit, false);   // setup half
            addN(spb, !fh, bit, false);   // sample edge
        }
    }
    addN(spb, cpol, false, true);   // return to idle, CS deassert
    return frames;
}

// ---------------------------------------------------------------------------
// New tests
// ---------------------------------------------------------------------------

TEST(SpiDecoderTest, Mode1_CPHA1) {
    // CPOL=0, CPHA=1: sample on falling edge
    const std::string clk="CLK", mosi="MOSI", cs="CS_N";
    auto frames = buildSpiFramesGeneric(clk, mosi, cs, {0xA5}, false, true);
    jtag::protocol::SpiConfig cfg;
    cfg.clk_pin  = clk; cfg.mosi_pin = mosi; cfg.cs_pin = cs;
    cfg.cpol = false;   cfg.cpha = true;
    auto decoded = jtag::protocol::decodeSpi(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    EXPECT_NE(decoded[0].label.find("A5"), std::string::npos);
}

TEST(SpiDecoderTest, Mode2_CPOL1) {
    // CPOL=1, CPHA=0: sample on falling (leading) edge
    const std::string clk="CLK", mosi="MOSI", cs="CS_N";
    auto frames = buildSpiFramesGeneric(clk, mosi, cs, {0x3C}, true, false);
    jtag::protocol::SpiConfig cfg;
    cfg.clk_pin  = clk; cfg.mosi_pin = mosi; cfg.cs_pin = cs;
    cfg.cpol = true;    cfg.cpha = false;
    auto decoded = jtag::protocol::decodeSpi(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    EXPECT_NE(decoded[0].label.find("3C"), std::string::npos);
}

TEST(SpiDecoderTest, Mode3_CPOL1_CPHA1) {
    // CPOL=1, CPHA=1: sample on rising (trailing) edge
    const std::string clk="CLK", mosi="MOSI", cs="CS_N";
    auto frames = buildSpiFramesGeneric(clk, mosi, cs, {0x69}, true, true);
    jtag::protocol::SpiConfig cfg;
    cfg.clk_pin  = clk; cfg.mosi_pin = mosi; cfg.cs_pin = cs;
    cfg.cpol = true;    cfg.cpha = true;
    auto decoded = jtag::protocol::decodeSpi(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    EXPECT_NE(decoded[0].label.find("69"), std::string::npos);
}

TEST(SpiDecoderTest, MultiByteTransfer) {
    // Two bytes clocked within a single CS assertion → two DecodedFrames
    const std::string clk="CLK", mosi="MOSI", cs="CS_N";
    auto frames = buildSpiFramesGeneric(clk, mosi, cs, {0x12, 0x34}, false, false);
    jtag::protocol::SpiConfig cfg;
    cfg.clk_pin  = clk; cfg.mosi_pin = mosi; cfg.cs_pin = cs;
    auto decoded = jtag::protocol::decodeSpi(frames, cfg);
    ASSERT_EQ(decoded.size(), 2u);
    EXPECT_NE(decoded[0].label.find("12"), std::string::npos);
    EXPECT_NE(decoded[1].label.find("34"), std::string::npos);
}

TEST(SpiDecoderTest, CsGapBetweenTransfers) {
    // Two separate CS assertions → two independent DecodedFrames
    const std::string clk="CLK", mosi="MOSI", cs="CS_N";
    auto f1 = buildSpiFramesGeneric(clk, mosi, cs, {0xAA}, false, false);
    auto f2 = buildSpiFramesGeneric(clk, mosi, cs, {0x55}, false, false);
    // Append f2 directly after f1 with timestamps continuing from f1's end
    auto f2_base = f2.front().timestamp;
    auto f1_end  = f1.back().timestamp + std::chrono::microseconds(10);
    for (auto& sf : f2) sf.timestamp = f1_end + (sf.timestamp - f2_base);
    f1.insert(f1.end(), f2.begin(), f2.end());

    jtag::protocol::SpiConfig cfg;
    cfg.clk_pin  = clk; cfg.mosi_pin = mosi; cfg.cs_pin = cs;
    auto decoded = jtag::protocol::decodeSpi(f1, cfg);
    ASSERT_EQ(decoded.size(), 2u);
    EXPECT_NE(decoded[0].label.find("AA"), std::string::npos);
    EXPECT_NE(decoded[1].label.find("55"), std::string::npos);
}

TEST(SpiDecoderTest, LSBFirst) {
    // 0x80 sent LSB-first: wire sequence bit0..bit7 = 0,0,0,0,0,0,0,1
    // Decoder reconstructs 0x80
    const std::string clk="CLK", mosi="MOSI", cs="CS_N";
    auto frames = buildSpiFramesGeneric(clk, mosi, cs, {0x80}, false, false,
                                        /*lsb_first=*/true);
    jtag::protocol::SpiConfig cfg;
    cfg.clk_pin   = clk; cfg.mosi_pin = mosi; cfg.cs_pin = cs;
    cfg.lsb_first = true;
    auto decoded = jtag::protocol::decodeSpi(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    EXPECT_NE(decoded[0].label.find("80"), std::string::npos);
}

}  // namespace
