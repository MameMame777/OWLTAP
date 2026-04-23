// Unit tests for Mt25qFlash command encoding using a recording fake bridge.
//
// These tests verify the SPI byte sequences sent for each command — they do
// not need real hardware.  The fake bridge captures every tx vector and
// allows the test to stage the rx bytes the flash should pretend to return.

#include "src/flash/spi_flash.h"

#include <deque>
#include <vector>

#include <gtest/gtest.h>

namespace {

using jtag::flash::IFlashBridge;
using jtag::flash::Mt25qFlash;

class RecordingBridge : public IFlashBridge {
 public:
    bool transfer(const std::vector<uint8_t>& tx,
                  std::vector<uint8_t>& rx) override {
        transfers.push_back(tx);
        rx.assign(tx.size(), 0);  // default MISO = 0
        if (!staged_rx.empty()) {
            const auto& next = staged_rx.front();
            for (std::size_t i = 0; i < rx.size() && i < next.size(); i++) {
                rx[i] = next[i];
            }
            staged_rx.pop_front();
        }
        return true;
    }
    const std::string& lastError() const override { return last_error; }

    std::vector<std::vector<uint8_t>> transfers;
    std::deque<std::vector<uint8_t>>  staged_rx;
    std::string                        last_error;
};

TEST(Mt25qFlashTest, ReadIdCommandEncoding) {
    RecordingBridge bridge;
    // Stage rx: [don't-care, 0x20, 0xBA, 0x18]
    bridge.staged_rx.push_back({0x00, 0x20, 0xBA, 0x18});

    Mt25qFlash flash(bridge);
    uint32_t id = 0;
    ASSERT_TRUE(flash.readId(id));
    EXPECT_EQ(id, Mt25qFlash::kJedecMt25ql128);

    ASSERT_EQ(bridge.transfers.size(), 1u);
    const auto& tx = bridge.transfers[0];
    ASSERT_EQ(tx.size(), 4u);
    EXPECT_EQ(tx[0], Mt25qFlash::kCmdReadId);
    EXPECT_EQ(tx[1], 0x00);
    EXPECT_EQ(tx[2], 0x00);
    EXPECT_EQ(tx[3], 0x00);
}

TEST(Mt25qFlashTest, ReadStatusCommandEncoding) {
    RecordingBridge bridge;
    bridge.staged_rx.push_back({0x00, 0x42});
    Mt25qFlash flash(bridge);
    uint8_t sr = 0;
    ASSERT_TRUE(flash.readStatus(sr));
    EXPECT_EQ(sr, 0x42);
    ASSERT_EQ(bridge.transfers.size(), 1u);
    EXPECT_EQ(bridge.transfers[0][0], Mt25qFlash::kCmdReadStatus);
}

TEST(Mt25qFlashTest, WriteEnableSingleByte) {
    RecordingBridge bridge;
    Mt25qFlash flash(bridge);
    ASSERT_TRUE(flash.writeEnable());
    ASSERT_EQ(bridge.transfers.size(), 1u);
    ASSERT_EQ(bridge.transfers[0].size(), 1u);
    EXPECT_EQ(bridge.transfers[0][0], Mt25qFlash::kCmdWriteEnable);
}

TEST(Mt25qFlashTest, PageProgramAddressEncoding) {
    RecordingBridge bridge;
    // writeEnable + pageProgram + readStatus (WIP clear immediately)
    Mt25qFlash flash(bridge);
    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_TRUE(flash.pageProgram(0x012345, payload.data(), payload.size(),
                                   std::chrono::milliseconds(100)));
    ASSERT_GE(bridge.transfers.size(), 2u);
    // [0] = writeEnable 0x06
    EXPECT_EQ(bridge.transfers[0][0], Mt25qFlash::kCmdWriteEnable);
    // [1] = page program 0x02 | addr 0x01 0x23 0x45 | data
    const auto& pp = bridge.transfers[1];
    ASSERT_EQ(pp.size(), 4u + payload.size());
    EXPECT_EQ(pp[0], Mt25qFlash::kCmdPageProgram);
    EXPECT_EQ(pp[1], 0x01);
    EXPECT_EQ(pp[2], 0x23);
    EXPECT_EQ(pp[3], 0x45);
    EXPECT_EQ(pp[4], 0xDE);
    EXPECT_EQ(pp[5], 0xAD);
    EXPECT_EQ(pp[6], 0xBE);
    EXPECT_EQ(pp[7], 0xEF);
}

TEST(Mt25qFlashTest, PageProgramRejectsOversize) {
    RecordingBridge bridge;
    Mt25qFlash flash(bridge);
    std::vector<uint8_t> payload(Mt25qFlash::kPageSize + 1, 0);
    EXPECT_FALSE(flash.pageProgram(0, payload.data(), payload.size(),
                                    std::chrono::milliseconds(100)));
    EXPECT_EQ(bridge.transfers.size(), 0u);
}

TEST(Mt25qFlashTest, PageProgramRejectsAddressOverflow) {
    RecordingBridge bridge;
    Mt25qFlash flash(bridge);
    uint8_t byte = 0;
    EXPECT_FALSE(flash.pageProgram(
        static_cast<uint32_t>(Mt25qFlash::kCapacityBytes), &byte, 1,
        std::chrono::milliseconds(100)));
}

TEST(Mt25qFlashTest, ReadAddressAndPayloadExtraction) {
    RecordingBridge bridge;
    // rx[0..3] = don't care; rx[4..] = actual data.
    bridge.staged_rx.push_back({0, 0, 0, 0, 0x11, 0x22, 0x33});
    Mt25qFlash flash(bridge);
    std::vector<uint8_t> out(3, 0);
    ASSERT_TRUE(flash.read(0xABCDEF, out.data(), out.size()));
    EXPECT_EQ(out[0], 0x11);
    EXPECT_EQ(out[1], 0x22);
    EXPECT_EQ(out[2], 0x33);
    ASSERT_EQ(bridge.transfers.size(), 1u);
    const auto& rd = bridge.transfers[0];
    ASSERT_EQ(rd.size(), 4u + 3u);
    EXPECT_EQ(rd[0], Mt25qFlash::kCmdRead);
    EXPECT_EQ(rd[1], 0xAB);
    EXPECT_EQ(rd[2], 0xCD);
    EXPECT_EQ(rd[3], 0xEF);
}

TEST(Mt25qFlashTest, BulkEraseIssuesWrenThenErase) {
    RecordingBridge bridge;
    Mt25qFlash flash(bridge);
    ASSERT_TRUE(flash.bulkErase(std::chrono::milliseconds(100)));
    ASSERT_GE(bridge.transfers.size(), 2u);
    EXPECT_EQ(bridge.transfers[0][0], Mt25qFlash::kCmdWriteEnable);
    EXPECT_EQ(bridge.transfers[1][0], Mt25qFlash::kCmdBulkErase);
}

}  // namespace
