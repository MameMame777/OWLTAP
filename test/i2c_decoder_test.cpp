#include <gtest/gtest.h>

#include "src/protocol/i2c_decoder.h"
#include "src/protocol/protocol.h"
#include "src/capture/capture_engine.h"

#include <chrono>

namespace {

// Build a synthetic I2C transaction: START, ADDR write, ACK, DATA, ACK, STOP
// Timing: 1 sample per transition
static std::vector<jtag::SampleFrame> buildI2cFrames(
        const std::string& scl_pin,
        const std::string& sda_pin,
        uint8_t addr_7bit,  // 7-bit address
        bool read,
        uint8_t data_byte) {
    std::vector<jtag::SampleFrame> frames;
    auto t0 = std::chrono::steady_clock::now();

    auto addSample = [&](bool scl, bool sda) {
        jtag::SampleFrame sf;
        sf.timestamp = t0 + std::chrono::microseconds(frames.size());
        sf.data.pin_states[scl_pin] = scl ? jtag::PinState::HIGH : jtag::PinState::LOW;
        sf.data.pin_states[sda_pin] = sda ? jtag::PinState::HIGH : jtag::PinState::LOW;
        sf.trigger_point = false;
        frames.push_back(sf);
    };

    // Idle: SCL=1, SDA=1
    for (int i = 0; i < 5; i++) addSample(true, true);

    // START: SDA falls while SCL=1
    addSample(true, true);
    addSample(true, false);  // START condition
    addSample(false, false);

    // Transmit addr (7 bits) + R/W bit, then ACK (9 bits total)
    uint8_t addr_byte = (addr_7bit << 1) | (read ? 1 : 0);
    for (int b = 7; b >= 0; b--) {
        bool bit = (addr_byte >> b) & 1;
        addSample(false, bit);
        addSample(true, bit);   // rising edge — data valid
        addSample(false, bit);
    }
    // ACK from slave (SDA pulled low by slave)
    addSample(false, false);
    addSample(true, false);   // SCL high during ACK
    addSample(false, false);

    // Transmit data byte + ACK
    for (int b = 7; b >= 0; b--) {
        bool bit = (data_byte >> b) & 1;
        addSample(false, bit);
        addSample(true, bit);
        addSample(false, bit);
    }
    // ACK
    addSample(false, false);
    addSample(true, false);
    addSample(false, false);

    // STOP: SCL=1, then SDA rises while SCL=1
    addSample(false, false);
    addSample(true, false);
    addSample(true, true);  // STOP condition

    // Idle
    for (int i = 0; i < 5; i++) addSample(true, true);

    return frames;
}

TEST(I2cDecoderTest, DecodeWriteTransaction) {
    const std::string scl = "SCL";
    const std::string sda = "SDA";
    const uint8_t addr = 0x4A;
    const uint8_t data = 0x55;

    auto frames = buildI2cFrames(scl, sda, addr, false, data);

    jtag::protocol::I2cConfig cfg;
    cfg.scl_pin = scl;
    cfg.sda_pin = sda;

    auto decoded = jtag::protocol::decodeI2c(frames, cfg);
    ASSERT_FALSE(decoded.empty());

    // Find a frame with address info
    bool found_addr = false;
    bool found_data = false;
    for (const auto& f : decoded) {
        if (f.label.find("4A") != std::string::npos ||
            f.label.find("0x4A") != std::string::npos) {
            found_addr = true;
        }
        if (f.label.find("55") != std::string::npos ||
            f.data.find("55") != std::string::npos) {
            found_data = true;
        }
    }
    EXPECT_TRUE(found_addr) << "Expected address 0x4A in decoded frames";
    EXPECT_TRUE(found_data) << "Expected data byte 0x55 in decoded frames";
}

TEST(I2cDecoderTest, EmptyFrames) {
    jtag::protocol::I2cConfig cfg;
    cfg.scl_pin = "SCL";
    cfg.sda_pin = "SDA";
    auto decoded = jtag::protocol::decodeI2c({}, cfg);
    EXPECT_TRUE(decoded.empty());
}

}  // namespace
