// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <vector>

#include "src/ila/ila_driver.h"
#include "test/support/mock_ila_tap.h"

namespace jtag::ila {
namespace {

using test::MockIlaTap;

TEST(IlaDriverTest, ReadIdcodeReturnsDefault) {
    MockIlaTap tap;
    IlaDriver drv(tap);

    uint32_t id = 0;
    ASSERT_TRUE(drv.readIdcode(id));
    EXPECT_EQ(id, IlaDriver::kDefaultIdcode);
}

TEST(IlaDriverTest, ConfigureTriggerLatchesMaskValuePreSamples) {
    MockIlaTap tap;
    IlaDriver drv(tap);

    ASSERT_TRUE(drv.configureTrigger(0xDEAD'BEEF, 0x1234'5678, 64));
    EXPECT_EQ(tap.trigMask(),   0xDEAD'BEEFu);
    EXPECT_EQ(tap.trigValue(),  0x1234'5678u);
    EXPECT_EQ(tap.preSamples(), 64u);
}

TEST(IlaDriverTest, ConfigureTriggerRejectsPreSamplesTooLarge) {
    MockIlaTap tap;
    IlaDriver drv(tap);

    EXPECT_FALSE(drv.configureTrigger(0, 0, IlaDriver::kDepth));
    EXPECT_FALSE(drv.lastError().empty());
}

TEST(IlaDriverTest, CtrlPulsesProduceCorrectBits) {
    MockIlaTap tap;
    IlaDriver drv(tap);

    ASSERT_TRUE(drv.arm());
    EXPECT_TRUE(tap.armRequested());
    EXPECT_FALSE(tap.stopRequested());
    EXPECT_FALSE(tap.resetRequested());
    EXPECT_FALSE(tap.forceRequested());

    ASSERT_TRUE(drv.stop());
    EXPECT_TRUE(tap.stopRequested());
    EXPECT_FALSE(tap.armRequested());

    ASSERT_TRUE(drv.resetCapture());
    EXPECT_TRUE(tap.resetRequested());

    ASSERT_TRUE(drv.forceTrigger());
    EXPECT_TRUE(tap.forceRequested());
}

TEST(IlaDriverTest, ReadStatusDecodesBits) {
    MockIlaTap tap;
    IlaDriver drv(tap);

    tap.setStatus(/*armed=*/true, /*triggered=*/false, /*full=*/true);
    IlaStatus st{};
    ASSERT_TRUE(drv.readStatus(st));
    EXPECT_TRUE(st.armed);
    EXPECT_FALSE(st.triggered);
    EXPECT_TRUE(st.full);

    tap.setStatus(false, true, false);
    ASSERT_TRUE(drv.readStatus(st));
    EXPECT_FALSE(st.armed);
    EXPECT_TRUE(st.triggered);
    EXPECT_FALSE(st.full);
}

TEST(IlaDriverTest, SetReadAddrRejectsOutOfRange) {
    MockIlaTap tap;
    IlaDriver drv(tap);

    ASSERT_TRUE(drv.setReadAddr(0));
    EXPECT_EQ(tap.readAddr(), 0u);
    ASSERT_TRUE(drv.setReadAddr(IlaDriver::kDepth - 1));
    EXPECT_EQ(tap.readAddr(), IlaDriver::kDepth - 1);
    EXPECT_FALSE(drv.setReadAddr(IlaDriver::kDepth));
}

TEST(IlaDriverTest, ReadSamplesReturnsBramContentsWithAutoInc) {
    MockIlaTap tap;
    for (int i = 0; i < IlaDriver::kDepth; i++) {
        tap.setBramWord(i, 0x1000'0000u + static_cast<uint32_t>(i));
    }
    IlaDriver drv(tap);

    ASSERT_TRUE(drv.setReadAddr(0));
    std::vector<uint32_t> samples;
    ASSERT_TRUE(drv.readSamples(samples));
    ASSERT_EQ(samples.size(), static_cast<size_t>(IlaDriver::kDepth));
    for (int i = 0; i < IlaDriver::kDepth; i++) {
        EXPECT_EQ(samples[i], 0x1000'0000u + static_cast<uint32_t>(i));
    }
    // READ_ADDR wrapped back to 0 after DEPTH auto-increments.
    EXPECT_EQ(tap.readAddr(), 0u);
}

TEST(IlaDriverTest, ReadSamplesPartialCount) {
    MockIlaTap tap;
    for (int i = 0; i < IlaDriver::kDepth; i++) {
        tap.setBramWord(i, static_cast<uint32_t>(0xA000u + i));
    }
    IlaDriver drv(tap);

    ASSERT_TRUE(drv.setReadAddr(10));
    std::vector<uint32_t> samples;
    ASSERT_TRUE(drv.readSamples(samples, 4));
    ASSERT_EQ(samples.size(), 4u);
    EXPECT_EQ(samples[0], 0xA00Au);
    EXPECT_EQ(samples[1], 0xA00Bu);
    EXPECT_EQ(samples[2], 0xA00Cu);
    EXPECT_EQ(samples[3], 0xA00Du);
    EXPECT_EQ(tap.readAddr(), 14u);
}

TEST(IlaDriverTest, FullCaptureFlowSmoke) {
    MockIlaTap tap;
    IlaDriver drv(tap);

    ASSERT_TRUE(drv.configureTrigger(0xFFFF'0000, 0xABCD'0000, 128));
    ASSERT_TRUE(drv.arm());

    tap.setStatus(true, false, false);
    IlaStatus s{};
    ASSERT_TRUE(drv.readStatus(s));
    EXPECT_TRUE(s.armed);

    tap.setStatus(false, true, true);
    ASSERT_TRUE(drv.readStatus(s));
    EXPECT_TRUE(s.full);

    for (int i = 0; i < IlaDriver::kDepth; i++) {
        tap.setBramWord(i, static_cast<uint32_t>(i));
    }
    ASSERT_TRUE(drv.setReadAddr(0));
    std::vector<uint32_t> samples;
    ASSERT_TRUE(drv.readSamples(samples));
    EXPECT_EQ(samples.back(), static_cast<uint32_t>(IlaDriver::kDepth - 1));
}

TEST(IlaDriverTest, ProbeReadsConfigRegister) {
    MockIlaTap tap;
    IlaDriver drv(tap);

    IlaCaps caps{};
    ASSERT_TRUE(drv.probe(caps));
    EXPECT_EQ(caps.raw,     0x0110'7C0Au);
    EXPECT_EQ(caps.version, 0x01u);
    EXPECT_EQ(caps.num_ch,  1u);
    EXPECT_EQ(caps.data_w,  32u);
    EXPECT_EQ(caps.addr_w,  10u);
    EXPECT_EQ(caps.depth,   1024u);
    EXPECT_TRUE(caps.probed);
    EXPECT_TRUE(drv.probed());
    EXPECT_EQ(drv.dataWidth(), 32);
    EXPECT_EQ(drv.addrWidth(), 10);
    EXPECT_EQ(drv.depth(),     1024);
}

TEST(IlaDriverTest, DefaultCapsWhenProbeNotCalled) {
    MockIlaTap tap;
    IlaDriver drv(tap);
    EXPECT_FALSE(drv.probed());
    EXPECT_EQ(drv.dataWidth(), IlaDriver::kDataWidth);
    EXPECT_EQ(drv.addrWidth(), IlaDriver::kAddrWidth);
    EXPECT_EQ(drv.depth(),     IlaDriver::kDepth);
}

TEST(IlaDriverTest, ProbeFailsWhenVersionIsZero) {
    MockIlaTap tap;
    tap.setConfigValue(0);
    IlaDriver drv(tap);

    IlaCaps caps{};
    EXPECT_FALSE(drv.probe(caps));
    EXPECT_FALSE(drv.probed());
    // Driver must still report compile-time defaults.
    EXPECT_EQ(drv.depth(), IlaDriver::kDepth);
}

TEST(IlaDriverTest, DepthReflectsProbedAddrW) {
    MockIlaTap tap;
    // Pretend the fabric was compiled with DATA_W=16, ADDR_W=8 (depth=256).
    // Layout: {0x01, num_ch=1, rsvd=0, data_w_m1=15, rsvd=0, addr_w=8}
    //       = 0x0110_3C08
    tap.setConfigValue(0x0110'3C08u);
    IlaDriver drv(tap);
    ASSERT_TRUE(drv.probe());
    EXPECT_EQ(drv.dataWidth(), 16);
    EXPECT_EQ(drv.addrWidth(), 8);
    EXPECT_EQ(drv.depth(),     256);
}

TEST(IlaDriverTest, ConfigureTriggerUsesRuntimeDepth) {
    MockIlaTap tap;
    tap.setConfigValue(0x0110'3C08u);  // depth=256
    IlaDriver drv(tap);
    ASSERT_TRUE(drv.probe());

    // 200 < 256 is fine
    EXPECT_TRUE(drv.configureTrigger(0, 0, 200));
    // 256 is out of range for the probed depth
    EXPECT_FALSE(drv.configureTrigger(0, 0, 256));
}

}  // namespace
}  // namespace jtag::ila
