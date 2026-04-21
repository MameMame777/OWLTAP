#include "src/config/pl_config.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// bitReverseByte
// ---------------------------------------------------------------------------

TEST(PlConfigBitReverseTest, KnownValues) {
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0x00), 0x00);
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0xFF), 0xFF);
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0x01), 0x80);  // LSB -> MSB
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0x80), 0x01);  // MSB -> LSB
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0xAA), 0x55);
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0x55), 0xAA);
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0xF0), 0x0F);
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0x0F), 0xF0);
}

TEST(PlConfigBitReverseTest, SyncWordBytesConvertCorrectly) {
    // Sync word as raw .bit bytes: 0xAA 0x99 0x55 0x66
    // After bit-reversal (what FPGA receives MSB-first via MPSSE LSB-first):
    //   reverse(0xAA)=0x55, reverse(0x99)=0x99, reverse(0x55)=0xAA, reverse(0x66)=0x66
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0xAA), 0x55);
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0x99), 0x99);
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0x55), 0xAA);
    EXPECT_EQ(jtag::PlConfig::bitReverseByte(0x66), 0x66);
}

// ---------------------------------------------------------------------------
// decodeStatus
// ---------------------------------------------------------------------------

TEST(PlConfigStatusTest, DoneOnly) {
    // DONE = bit14
    uint32_t raw = 1u << 14;
    auto s = jtag::PlConfig::decodeStatus(raw);
    EXPECT_TRUE(s.done);
    EXPECT_FALSE(s.release_done);
    EXPECT_FALSE(s.init_b);
    EXPECT_FALSE(s.crc_error);
    EXPECT_EQ(s.raw, raw);
}

TEST(PlConfigStatusTest, InitBAndEos) {
    // INIT_B = bit12, EOS = bit4
    uint32_t raw = (1u << 12) | (1u << 4);
    auto s = jtag::PlConfig::decodeStatus(raw);
    EXPECT_TRUE(s.init_b);
    EXPECT_TRUE(s.eos);
    EXPECT_FALSE(s.done);
    EXPECT_FALSE(s.crc_error);
}

TEST(PlConfigStatusTest, CrcError) {
    uint32_t raw = 1u << 0;
    auto s = jtag::PlConfig::decodeStatus(raw);
    EXPECT_TRUE(s.crc_error);
    EXPECT_FALSE(s.done);
}

TEST(PlConfigStatusTest, AllFieldsSet) {
    uint32_t raw = (1u << 14) | (1u << 13) | (1u << 12) | (1u << 11) |
                   (1u << 4)  | (1u << 0);
    auto s = jtag::PlConfig::decodeStatus(raw);
    EXPECT_TRUE(s.done);
    EXPECT_TRUE(s.release_done);
    EXPECT_TRUE(s.init_b);
    EXPECT_TRUE(s.init_complete);
    EXPECT_TRUE(s.eos);
    EXPECT_TRUE(s.crc_error);
}

TEST(PlConfigStatusTest, ZeroRaw) {
    auto s = jtag::PlConfig::decodeStatus(0);
    EXPECT_FALSE(s.done);
    EXPECT_FALSE(s.release_done);
    EXPECT_FALSE(s.init_b);
    EXPECT_FALSE(s.init_complete);
    EXPECT_FALSE(s.eos);
    EXPECT_FALSE(s.crc_error);
    EXPECT_EQ(s.raw, 0u);
}

// ---------------------------------------------------------------------------
// loadBitstream — .bit header stripping and bit-reversal
// ---------------------------------------------------------------------------

// Helper: write a temp file with given bytes and return its path
static std::string writeTempFile(const std::string& name,
                                  const std::vector<uint8_t>& data) {
    std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp") +
                       "/" + name;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return path;
}

TEST(PlConfigLoadBitstreamTest, StripHeaderAtOffset0) {
    // Sync word at byte 0 — header length = 0, body = sync + 2 payload bytes
    const uint8_t payload = 0xA5;
    std::vector<uint8_t> file = {0xAA, 0x99, 0x55, 0x66, payload, 0x00};
    std::string path = writeTempFile("test_sync0.bit", file);

    std::vector<uint8_t> body;
    ASSERT_TRUE(jtag::PlConfig::loadBitstream(path, body));
    ASSERT_EQ(body.size(), 6u);
    // First byte: bitReverse(0xAA) = 0x55
    EXPECT_EQ(body[0], jtag::PlConfig::bitReverseByte(0xAA));
    // Payload byte: bitReverse(0xA5)
    EXPECT_EQ(body[4], jtag::PlConfig::bitReverseByte(payload));
}

TEST(PlConfigLoadBitstreamTest, StripHeaderAtOffset16) {
    // Sync word at offset 16 (typical .bit file header is ~96 bytes,
    // but test with minimal 16-byte header)
    std::vector<uint8_t> file(16, 0x00);
    file.push_back(0xAA); file.push_back(0x99);
    file.push_back(0x55); file.push_back(0x66);
    file.push_back(0xBE); file.push_back(0xEF);
    std::string path = writeTempFile("test_sync16.bit", file);

    std::vector<uint8_t> body;
    ASSERT_TRUE(jtag::PlConfig::loadBitstream(path, body));
    // Body starts from sync word: 4 sync bytes + 2 payload bytes
    ASSERT_EQ(body.size(), 6u);
    EXPECT_EQ(body[0], jtag::PlConfig::bitReverseByte(0xAA));
    EXPECT_EQ(body[4], jtag::PlConfig::bitReverseByte(0xBE));
}

TEST(PlConfigLoadBitstreamTest, NoSyncWordUsesFullFile) {
    // .bin file — no sync word; loadBitstream keeps all bytes
    std::vector<uint8_t> file = {0x01, 0x02, 0x03, 0x04};
    std::string path = writeTempFile("test_noheader.bin", file);

    std::vector<uint8_t> body;
    ASSERT_TRUE(jtag::PlConfig::loadBitstream(path, body));
    ASSERT_EQ(body.size(), 4u);
    EXPECT_EQ(body[0], jtag::PlConfig::bitReverseByte(0x01));
    EXPECT_EQ(body[3], jtag::PlConfig::bitReverseByte(0x04));
}

TEST(PlConfigLoadBitstreamTest, AllBytesReversed) {
    // Verify every byte in output is bit-reversed from input
    std::vector<uint8_t> file;
    for (int i = 0; i < 256; i++) file.push_back(static_cast<uint8_t>(i));
    std::string path = writeTempFile("test_allbytes.bin", file);

    std::vector<uint8_t> body;
    ASSERT_TRUE(jtag::PlConfig::loadBitstream(path, body));
    for (int i = 0; i < 256; i++) {
        EXPECT_EQ(body[i], jtag::PlConfig::bitReverseByte(static_cast<uint8_t>(i)))
            << "mismatch at byte " << i;
    }
}
