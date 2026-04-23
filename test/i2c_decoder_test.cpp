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

// ---------------------------------------------------------------------------
// Helper: shift one byte + ACK into a frames vector (modifies in place)
// ---------------------------------------------------------------------------
static void i2cShiftByteAck(
        std::vector<jtag::SampleFrame>& frames,
        const std::string& scl_pin, const std::string& sda_pin,
        uint8_t byte_val) {
    auto t0 = frames.empty() ? std::chrono::steady_clock::now()
                              : frames.front().timestamp;
    auto addSample = [&](bool scl, bool sda) {
        jtag::SampleFrame sf;
        sf.timestamp = t0 + std::chrono::microseconds(frames.size());
        sf.data.pin_states[scl_pin] = scl ? jtag::PinState::HIGH : jtag::PinState::LOW;
        sf.data.pin_states[sda_pin] = sda ? jtag::PinState::HIGH : jtag::PinState::LOW;
        frames.push_back(sf);
    };
    for (int b = 7; b >= 0; b--) {
        bool bit = (byte_val >> b) & 1;
        addSample(false, bit);
        addSample(true,  bit);
        addSample(false, bit);
    }
    // ACK (SDA LOW)
    addSample(false, false);
    addSample(true,  false);
    addSample(false, false);
}

// Build I2C frames ending with a NAK after the address (slave not present).
static std::vector<jtag::SampleFrame> buildI2cNakFrames(
        const std::string& scl_pin, const std::string& sda_pin,
        uint8_t addr_7bit) {
    std::vector<jtag::SampleFrame> frames;
    auto t0 = std::chrono::steady_clock::now();
    auto addSample = [&](bool scl, bool sda) {
        jtag::SampleFrame sf;
        sf.timestamp = t0 + std::chrono::microseconds(frames.size());
        sf.data.pin_states[scl_pin] = scl ? jtag::PinState::HIGH : jtag::PinState::LOW;
        sf.data.pin_states[sda_pin] = sda ? jtag::PinState::HIGH : jtag::PinState::LOW;
        frames.push_back(sf);
    };
    for (int i = 0; i < 5; i++) addSample(true, true);
    addSample(true, true);
    addSample(true, false);   // START
    addSample(false, false);
    // Address + W
    uint8_t addr_byte = static_cast<uint8_t>((addr_7bit << 1) | 0);
    for (int b = 7; b >= 0; b--) {
        bool bit = (addr_byte >> b) & 1;
        addSample(false, bit);
        addSample(true,  bit);
        addSample(false, bit);
    }
    // NAK: SDA stays HIGH
    addSample(false, true);
    addSample(true,  true);   // rising SCL with SDA=1 → NAK sampled
    addSample(false, true);
    // STOP
    addSample(false, false);
    addSample(true,  false);
    addSample(true,  true);   // SDA rises while SCL high → STOP
    for (int i = 0; i < 5; i++) addSample(true, true);
    return frames;
}

// Build I2C frames with a repeated START between two segments.
static std::vector<jtag::SampleFrame> buildI2cRepeatedStartFrames(
        const std::string& scl_pin, const std::string& sda_pin) {
    std::vector<jtag::SampleFrame> frames;
    auto t0 = std::chrono::steady_clock::now();
    auto addSample = [&](bool scl, bool sda) {
        jtag::SampleFrame sf;
        sf.timestamp = t0 + std::chrono::microseconds(frames.size());
        sf.data.pin_states[scl_pin] = scl ? jtag::PinState::HIGH : jtag::PinState::LOW;
        sf.data.pin_states[sda_pin] = sda ? jtag::PinState::HIGH : jtag::PinState::LOW;
        frames.push_back(sf);
    };
    for (int i = 0; i < 5; i++) addSample(true, true);
    addSample(true, true);
    addSample(true, false);   // START
    addSample(false, false);
    // First segment: ADDR(0x4A W), ACK, DATA(0x55), ACK
    i2cShiftByteAck(frames, scl_pin, sda_pin,
                    static_cast<uint8_t>((0x4A << 1) | 0));
    i2cShiftByteAck(frames, scl_pin, sda_pin, 0x55);
    // Repeated START: SDA rises, SCL high, SDA falls
    addSample(false, true);
    addSample(true,  true);
    addSample(true,  false);   // SDA falls while SCL=1 → repeated START
    addSample(false, false);
    // Second segment: ADDR(0x4B R), ACK, DATA(0xAA), ACK
    i2cShiftByteAck(frames, scl_pin, sda_pin,
                    static_cast<uint8_t>((0x4B << 1) | 1));
    i2cShiftByteAck(frames, scl_pin, sda_pin, 0xAA);
    // STOP
    addSample(false, false);
    addSample(true,  false);
    addSample(true,  true);
    for (int i = 0; i < 5; i++) addSample(true, true);
    return frames;
}

// Build I2C multi-byte write: START, ADDR, ACK, DATA..., ACK, STOP.
static std::vector<jtag::SampleFrame> buildI2cMultiByteFrames(
        const std::string& scl_pin, const std::string& sda_pin,
        uint8_t addr_7bit, const std::vector<uint8_t>& data) {
    std::vector<jtag::SampleFrame> frames;
    auto t0 = std::chrono::steady_clock::now();
    auto addSample = [&](bool scl, bool sda) {
        jtag::SampleFrame sf;
        sf.timestamp = t0 + std::chrono::microseconds(frames.size());
        sf.data.pin_states[scl_pin] = scl ? jtag::PinState::HIGH : jtag::PinState::LOW;
        sf.data.pin_states[sda_pin] = sda ? jtag::PinState::HIGH : jtag::PinState::LOW;
        frames.push_back(sf);
    };
    for (int i = 0; i < 5; i++) addSample(true, true);
    addSample(true, true);
    addSample(true, false);
    addSample(false, false);
    i2cShiftByteAck(frames, scl_pin, sda_pin,
                    static_cast<uint8_t>((addr_7bit << 1) | 0));
    for (uint8_t d : data)
        i2cShiftByteAck(frames, scl_pin, sda_pin, d);
    addSample(false, false);
    addSample(true,  false);
    addSample(true,  true);
    for (int i = 0; i < 5; i++) addSample(true, true);
    return frames;
}

// ---------------------------------------------------------------------------
// New tests
// ---------------------------------------------------------------------------

TEST(I2cDecoderTest, ReadTransaction) {
    auto frames = buildI2cFrames("SCL", "SDA", 0x50, /*read=*/true, 0x42);
    jtag::protocol::I2cConfig cfg{"SCL", "SDA"};
    auto decoded = jtag::protocol::decodeI2c(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    bool found_read = false;
    for (const auto& f : decoded) {
        if (f.label.find("50") != std::string::npos &&
            f.label.find('R')  != std::string::npos) {
            found_read = true;
        }
    }
    EXPECT_TRUE(found_read) << "Expected 'ADDR 0x50 R' frame";
}

TEST(I2cDecoderTest, NakAfterAddress) {
    auto frames = buildI2cNakFrames("SCL", "SDA", 0x3C);
    jtag::protocol::I2cConfig cfg{"SCL", "SDA"};
    auto decoded = jtag::protocol::decodeI2c(frames, cfg);
    ASSERT_FALSE(decoded.empty());
    bool found_nak = false;
    for (const auto& f : decoded) {
        if (f.label == "NAK") {
            found_nak = true;
            EXPECT_TRUE(f.error_flag);
        }
    }
    EXPECT_TRUE(found_nak) << "Expected NAK frame with error_flag";
}

TEST(I2cDecoderTest, RepeatedStart) {
    auto frames = buildI2cRepeatedStartFrames("SCL", "SDA");
    jtag::protocol::I2cConfig cfg{"SCL", "SDA"};
    auto decoded = jtag::protocol::decodeI2c(frames, cfg);
    int start_count = 0;
    for (const auto& f : decoded) {
        if (f.label == "START") start_count++;
    }
    EXPECT_EQ(start_count, 2) << "Expected initial START + repeated START";
}

TEST(I2cDecoderTest, MultiByteWrite) {
    auto frames = buildI2cMultiByteFrames("SCL", "SDA", 0x4A, {0x12, 0x34});
    jtag::protocol::I2cConfig cfg{"SCL", "SDA"};
    auto decoded = jtag::protocol::decodeI2c(frames, cfg);
    bool found_12 = false, found_34 = false;
    for (const auto& f : decoded) {
        if (f.label.find("DATA") != std::string::npos) {
            if (f.label.find("12") != std::string::npos) found_12 = true;
            if (f.label.find("34") != std::string::npos) found_34 = true;
        }
    }
    EXPECT_TRUE(found_12) << "Expected DATA 0x12";
    EXPECT_TRUE(found_34) << "Expected DATA 0x34";
}

}  // namespace
