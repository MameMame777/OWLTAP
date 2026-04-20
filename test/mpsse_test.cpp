#include "gtest/gtest.h"

#include "src/ftdi/mpsse.h"

namespace jtag {
namespace {

TEST(MpsseCommandBufferTest, InitialState) {
    MpsseCommandBuffer buf;
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_EQ(buf.expectedReadBytes(), 0u);
}

TEST(MpsseCommandBufferTest, Clear) {
    MpsseCommandBuffer buf;
    buf.sendImmediate();
    EXPECT_FALSE(buf.empty());
    buf.clear();
    EXPECT_TRUE(buf.empty());
}

TEST(MpsseCommandBufferTest, ClockTms) {
    MpsseCommandBuffer buf;
    // Clock 3 TMS bits: TMS = 0b101, TDI = 0
    buf.clockTms(0x05, 3, false, false);

    auto& data = buf.data();
    ASSERT_GE(data.size(), 3u);
    EXPECT_EQ(data[0], mpsse_cmd::MPSSE_TMS_OUT);
    EXPECT_EQ(data[1], 2);  // bit_count - 1
    EXPECT_EQ(data[2], 0x05);  // TMS pattern
}

TEST(MpsseCommandBufferTest, ClockTmsWithTdi) {
    MpsseCommandBuffer buf;
    buf.clockTms(0x01, 1, true, false);

    auto& data = buf.data();
    ASSERT_GE(data.size(), 3u);
    EXPECT_EQ(data[0], mpsse_cmd::MPSSE_TMS_OUT);
    EXPECT_EQ(data[2], 0x01 | 0x80);  // TMS pattern | TDI=1 in bit 7
}

TEST(MpsseCommandBufferTest, ShiftOut) {
    MpsseCommandBuffer buf;
    uint8_t data_out[] = {0xAA, 0x55};
    buf.shiftOut(data_out, 16);

    auto& data = buf.data();
    // Should have: command byte, length_lo, length_hi, data[0], data[1]
    ASSERT_GE(data.size(), 5u);
    EXPECT_EQ(data[0], mpsse_cmd::MPSSE_WRITE_NEG_LSB);
    EXPECT_EQ(data[3], 0xAA);
    EXPECT_EQ(data[4], 0x55);
    EXPECT_EQ(buf.expectedReadBytes(), 0u);
}

TEST(MpsseCommandBufferTest, ShiftIn) {
    MpsseCommandBuffer buf;
    buf.shiftIn(16);

    ASSERT_GE(buf.size(), 3u);
    auto& data = buf.data();
    EXPECT_EQ(data[0], mpsse_cmd::MPSSE_READ_POS_LSB);
    EXPECT_EQ(buf.expectedReadBytes(), 2u);
}

TEST(MpsseCommandBufferTest, ShiftInOut) {
    MpsseCommandBuffer buf;
    uint8_t tdi[] = {0xFF};
    buf.shiftInOut(tdi, 8);

    ASSERT_GE(buf.size(), 4u);
    auto& data = buf.data();
    EXPECT_EQ(data[0], mpsse_cmd::MPSSE_RDWR_LSB);
    EXPECT_EQ(buf.expectedReadBytes(), 1u);
}

TEST(MpsseCommandBufferTest, SetLowBits) {
    MpsseCommandBuffer buf;
    buf.setLowBits(0x08, 0x0B);

    auto& data = buf.data();
    ASSERT_EQ(data.size(), 3u);
    EXPECT_EQ(data[0], mpsse_cmd::SET_BITS_LOW);
    EXPECT_EQ(data[1], 0x08);  // value
    EXPECT_EQ(data[2], 0x0B);  // direction
}

TEST(MpsseCommandBufferTest, GetLowBits) {
    MpsseCommandBuffer buf;
    buf.getLowBits();

    auto& data = buf.data();
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0], mpsse_cmd::GET_BITS_LOW);
    EXPECT_EQ(buf.expectedReadBytes(), 1u);
}

TEST(MpsseCommandBufferTest, SetClockDivisor) {
    MpsseCommandBuffer buf;
    buf.setClockDivisor(5);

    auto& data = buf.data();
    ASSERT_EQ(data.size(), 3u);
    EXPECT_EQ(data[0], mpsse_cmd::TCK_DIVISOR);
    EXPECT_EQ(data[1], 5);  // Low byte
    EXPECT_EQ(data[2], 0);  // High byte
}

TEST(MpsseCommandBufferTest, DisableClockDivide5) {
    MpsseCommandBuffer buf;
    buf.disableClockDivide5();
    ASSERT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf.data()[0], mpsse_cmd::DIS_DIV_5);
}

TEST(MpsseCommandBufferTest, SendImmediate) {
    MpsseCommandBuffer buf;
    buf.sendImmediate();
    ASSERT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf.data()[0], mpsse_cmd::SEND_IMMEDIATE);
}

TEST(MpsseCommandBufferTest, MultipleCommands) {
    MpsseCommandBuffer buf;
    buf.disableClockDivide5();
    buf.enable3PhaseClocking();
    buf.disableLoopback();
    buf.sendImmediate();

    EXPECT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf.data()[0], mpsse_cmd::DIS_DIV_5);
    EXPECT_EQ(buf.data()[1], mpsse_cmd::EN_3_PHASE);
    EXPECT_EQ(buf.data()[2], mpsse_cmd::LOOPBACK_END);
    EXPECT_EQ(buf.data()[3], mpsse_cmd::SEND_IMMEDIATE);
}

} // namespace
} // namespace jtag
