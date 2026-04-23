// Unit tests for the Intel HEX / Xilinx MCS file parser.
// Tests use in-memory MCS content written to temp files on disk.

#include "src/flash/mcs_parser.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using jtag::flash::internal::parseMcsToImage;

// Write a string to a temp file and return its path.
std::string writeTempMcs(const std::string& content) {
    char buf[L_tmpnam];
    std::tmpnam(buf);
    std::string path = std::string(buf) + ".mcs";
    std::ofstream f(path);
    f << content;
    return path;
}

// Minimal 2-record MCS: one 4-byte data record at address 0x0000, then EOF.
//   :04000000DEADBEEF4B  (data)
//   :00000001FF          (EOF)
// Checksum for data record: 0x100 - ((0x04+0x00+0x00+0x00+0xDE+0xAD+0xBE+0xEF) & 0xFF)
// sum = 04+DE+AD+BE+EF = 0x3B4; lower byte = 0xB4; two's complement = 0x4C... let me compute:
// 04+00+00+00+DE+AD+BE+EF = 0x2B4 -> lower byte 0xB4 -> ~0xB4+1 = 0x4C -> 0x4C
static const char kSimpleMcs[] =
    ":04000000DEADBEEFC4\r\n"
    ":00000001FF\r\n";

TEST(McsParserTest, SimpleDataRecord) {
    auto path = writeTempMcs(kSimpleMcs);
    std::vector<uint8_t> out;
    std::string err;
    ASSERT_TRUE(parseMcsToImage(path, out, err)) << err;
    std::remove(path.c_str());

    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], 0xDE);
    EXPECT_EQ(out[1], 0xAD);
    EXPECT_EQ(out[2], 0xBE);
    EXPECT_EQ(out[3], 0xEF);
}

// Two data records with a gap: addr 0x0000 (4 bytes) and addr 0x0008 (2 bytes).
// The gap bytes [4..7] should be 0xFF.
// Record 1: :04 0000 00 DEADBEEF CK
//   CK = 0x100 - ((04+00+00+00+DE+AD+BE+EF) & 0xFF) = 0x100 - 0xB4 = 0x4C
// Record 2: :02 0008 00 1234 CK
//   CK = 0x100 - ((02+00+08+00+12+34) & 0xFF) = 0x100 - 0x50 = 0xB0
static const char kGapMcs[] =
    ":04000000DEADBEEFC4\r\n"
    ":020008001234B0\r\n"
    ":00000001FF\r\n";

TEST(McsParserTest, GapFilledWithFF) {
    auto path = writeTempMcs(kGapMcs);
    std::vector<uint8_t> out;
    std::string err;
    ASSERT_TRUE(parseMcsToImage(path, out, err)) << err;
    std::remove(path.c_str());

    ASSERT_EQ(out.size(), 10u);
    EXPECT_EQ(out[0], 0xDE);
    EXPECT_EQ(out[3], 0xEF);
    EXPECT_EQ(out[4], 0xFF);  // gap
    EXPECT_EQ(out[7], 0xFF);  // gap
    EXPECT_EQ(out[8], 0x12);
    EXPECT_EQ(out[9], 0x34);
}

// Extended Linear Address (type 04) shifts data to upper 16 bits.
// ELA record: :02 0000 04 0001 CK
//   sum = 2+0+0+4+0+1 = 7 -> CK=0xF9
// Data at 0x00010000: :04 0000 00 AABBCCDD CK
//   sum = 4+0+0+0+0xAA+0xBB+0xCC+0xDD = 786 = 0x312 -> LSB 0x12 -> CK=0xEE
static const char kEla[] =
    ":020000040001F9\r\n"
    ":04000000AABBCCDDEE\r\n"
    ":00000001FF\r\n";

TEST(McsParserTest, ExtendedLinearAddress) {
    auto path = writeTempMcs(kEla);
    std::vector<uint8_t> out;
    std::string err;
    ASSERT_TRUE(parseMcsToImage(path, out, err)) << err;
    std::remove(path.c_str());

    ASSERT_GE(out.size(), static_cast<size_t>(0x00010004u));
    // Gap before 0x00010000 must be 0xFF.
    EXPECT_EQ(out[0], 0xFF);
    EXPECT_EQ(out[0x00010000], 0xAA);
    EXPECT_EQ(out[0x00010001], 0xBB);
    EXPECT_EQ(out[0x00010002], 0xCC);
    EXPECT_EQ(out[0x00010003], 0xDD);
}

TEST(McsParserTest, ChecksumMismatchReturnsError) {
    // Corrupt checksum: last byte wrong
    const std::string bad =
        ":04000000DEADBEEF00\r\n"  // 00 instead of C4
        ":00000001FF\r\n";
    auto path = writeTempMcs(bad);
    std::vector<uint8_t> out;
    std::string err;
    EXPECT_FALSE(parseMcsToImage(path, out, err));
    EXPECT_FALSE(err.empty());
    std::remove(path.c_str());
}

TEST(McsParserTest, EmptyFileReturnsError) {
    auto path = writeTempMcs(":00000001FF\r\n");  // only EOF, no data
    std::vector<uint8_t> out;
    std::string err;
    EXPECT_FALSE(parseMcsToImage(path, out, err));
    std::remove(path.c_str());
}

TEST(McsParserTest, NonexistentFileReturnsError) {
    std::vector<uint8_t> out;
    std::string err;
    EXPECT_FALSE(parseMcsToImage("__no_such_file__.mcs", out, err));
    EXPECT_FALSE(err.empty());
}

TEST(McsParserTest, WindowsCRLFAndUnixLF) {
    // Same content but LF-only
    const std::string lf_only =
        ":04000000DEADBEEFC4\n"
        ":00000001FF\n";
    auto path = writeTempMcs(lf_only);
    std::vector<uint8_t> out;
    std::string err;
    ASSERT_TRUE(parseMcsToImage(path, out, err)) << err;
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], 0xDE);
    std::remove(path.c_str());
}

}  // namespace
