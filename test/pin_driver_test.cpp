#include <gtest/gtest.h>

#include <string>

#include "src/boundary_scan/pin_driver.h"

namespace jtag {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Build a minimal BSDLDevice for BSR staging tests.
/// Layout (boundary_cells index == bit position):
///   bit 0 : OUTPUT3  "LED0"    disabled_by=1, disable_value=1
///   bit 1 : CONTROL  "*"
///   bit 2 : INPUT    "BTN0"
///   bit 3 : OUTPUT2  "CLK_OUT" no control cell
///   bit 4 : OUTPUT3  "IO0"     disabled_by=5, disable_value=0  (active-low enable)
///   bit 5 : CONTROL  "*"
bsdl::BSDLDevice makeTestDevice() {
    bsdl::BSDLDevice dev;
    dev.boundary_length = 6;
    dev.boundary_cells.resize(6);

    // bit 0 - OUTPUT3 LED0, enable=HIGH (disable_value=1 → enable when ctrl=0)
    dev.boundary_cells[0] = {0, "BC_4", "LED0", bsdl::CellFunction::OUTPUT3,
                              0, 1, 1, bsdl::DisableResult::HIGHZ};
    // bit 1 - CONTROL for LED0
    dev.boundary_cells[1] = {1, "BC_4", "*", bsdl::CellFunction::CONTROL,
                              1, -1, -1, bsdl::DisableResult::NONE};
    // bit 2 - INPUT BTN0 (not drivable)
    dev.boundary_cells[2] = {2, "BC_1", "BTN0", bsdl::CellFunction::INPUT,
                              0, -1, -1, bsdl::DisableResult::NONE};
    // bit 3 - OUTPUT2 CLK_OUT (no tri-state)
    dev.boundary_cells[3] = {3, "BC_2", "CLK_OUT", bsdl::CellFunction::OUTPUT2,
                              0, -1, -1, bsdl::DisableResult::NONE};
    // bit 4 - OUTPUT3 IO0, enable=LOW  (disable_value=0 → enable when ctrl=1)
    dev.boundary_cells[4] = {4, "BC_4", "IO0", bsdl::CellFunction::OUTPUT3,
                              0, 5, 0, bsdl::DisableResult::HIGHZ};
    // bit 5 - CONTROL for IO0
    dev.boundary_cells[5] = {5, "BC_4", "*", bsdl::CellFunction::CONTROL,
                              0, -1, -1, bsdl::DisableResult::NONE};
    return dev;
}

// ---------------------------------------------------------------------------
// deviceAllowsLiveExtest
// ---------------------------------------------------------------------------

TEST(PinDriverTest, BlocksLiveExtestOnZynqProcessingSystemPins) {
    bsdl::BSDLDevice device;
    device.pins["PS_DDR_A0"].name = "PS_DDR_A0";
    device.pins["PS_MIO0"].name = "PS_MIO0";

    std::string reason;
    EXPECT_FALSE(deviceAllowsLiveExtest(device, &reason));
    EXPECT_NE(reason.find("PS/DDR/MIO"), std::string::npos);
}

TEST(PinDriverTest, AllowsLiveExtestWhenPsPinsAreAbsent) {
    bsdl::BSDLDevice device;
    device.pins["LED0"].name = "LED0";
    device.pins["DONE_T12"].name = "DONE_T12";

    std::string reason;
    EXPECT_TRUE(deviceAllowsLiveExtest(device, &reason));
    EXPECT_TRUE(reason.empty());
}

// ---------------------------------------------------------------------------
// stagePinOutput
// ---------------------------------------------------------------------------

TEST(PinDriverTest, StagePinOutputSetsOutputBitAndEnablesControl) {
    // LED0: disable_value=1 → enable_val = (1==0 ? true : false) = false → ctrl=0
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0xFF);  // start all-high to verify clearing

    EXPECT_TRUE(stagePinOutput(bsr, dev, "LED0", 1));

    // output bit 0 should be 1
    EXPECT_EQ((bsr[0] >> 0) & 1, 1);
    // control bit 1 should be 0 (enable: opposite of disable_value=1)
    EXPECT_EQ((bsr[0] >> 1) & 1, 0);
}

TEST(PinDriverTest, StagePinOutputClearsOutputBit) {
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0xFF);

    EXPECT_TRUE(stagePinOutput(bsr, dev, "LED0", 0));

    EXPECT_EQ((bsr[0] >> 0) & 1, 0);  // output LOW
    EXPECT_EQ((bsr[0] >> 1) & 1, 0);  // control enabled (ctrl=0 != disable=1)
}

TEST(PinDriverTest, StagePinOutputActiveLowEnablePolarity) {
    // IO0: disable_value=0 → enable_val = (0==0 ? true : false) = true → ctrl=1
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0x00);

    EXPECT_TRUE(stagePinOutput(bsr, dev, "IO0", 1));

    EXPECT_EQ((bsr[0] >> 4) & 1, 1);  // output HIGH
    EXPECT_EQ((bsr[0] >> 5) & 1, 1);  // control enabled (ctrl=1 != disable=0)
}

TEST(PinDriverTest, StagePinOutputReturnsFalseForInputPin) {
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0x00);
    std::string error;

    EXPECT_FALSE(stagePinOutput(bsr, dev, "BTN0", 1, &error));
    EXPECT_FALSE(error.empty());
}

TEST(PinDriverTest, StagePinOutputReturnsFalseForUnknownPin) {
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0x00);

    EXPECT_FALSE(stagePinOutput(bsr, dev, "NONEXISTENT", 1));
}

// ---------------------------------------------------------------------------
// stagePinHighZ
// ---------------------------------------------------------------------------

TEST(PinDriverTest, StagePinHighZSetsControlToDisableValue) {
    // LED0: disable_value=1 → ctrl bit = (1 != 0) = true = 1
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0x00);

    EXPECT_TRUE(stagePinHighZ(bsr, dev, "LED0"));

    EXPECT_EQ((bsr[0] >> 1) & 1, 1);  // control bit set to disable_value=1
}

TEST(PinDriverTest, StagePinHighZActiveLowControlPolarity) {
    // IO0: disable_value=0 → ctrl bit = (0 != 0) = false = 0
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0xFF);  // start high

    EXPECT_TRUE(stagePinHighZ(bsr, dev, "IO0"));

    EXPECT_EQ((bsr[0] >> 5) & 1, 0);  // control bit cleared to disable_value=0
}

TEST(PinDriverTest, StagePinHighZReturnsFalseForOutput2) {
    // CLK_OUT is OUTPUT2 (no tri-state support)
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0x00);
    std::string error;

    EXPECT_FALSE(stagePinHighZ(bsr, dev, "CLK_OUT", &error));
    EXPECT_FALSE(error.empty());
}

TEST(PinDriverTest, StagePinHighZReturnsFalseForInputPin) {
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0x00);

    EXPECT_FALSE(stagePinHighZ(bsr, dev, "BTN0"));
}

// ---------------------------------------------------------------------------
// getStagedPinValue
// ---------------------------------------------------------------------------

TEST(PinDriverTest, GetStagedPinValueReturnsHighAfterStageHigh) {
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0x00);

    stagePinOutput(bsr, dev, "LED0", 1);
    EXPECT_EQ(getStagedPinValue(bsr, dev, "LED0"), 1);
}

TEST(PinDriverTest, GetStagedPinValueReturnsLowAfterStageLow) {
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0xFF);

    stagePinOutput(bsr, dev, "LED0", 0);
    EXPECT_EQ(getStagedPinValue(bsr, dev, "LED0"), 0);
}

TEST(PinDriverTest, GetStagedPinValueReturnsHighZAfterStageHighZ) {
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0x00);

    stagePinHighZ(bsr, dev, "LED0");
    EXPECT_EQ(getStagedPinValue(bsr, dev, "LED0"), -1);
}

TEST(PinDriverTest, GetStagedPinValueReturnsMinusOneForInputPin) {
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0x00);

    EXPECT_EQ(getStagedPinValue(bsr, dev, "BTN0"), -1);
}

TEST(PinDriverTest, GetStagedPinValueRoundTripOutput2) {
    // OUTPUT2 has no control cell; value is always readable
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0x00);

    stagePinOutput(bsr, dev, "CLK_OUT", 1);
    EXPECT_EQ(getStagedPinValue(bsr, dev, "CLK_OUT"), 1);
}

// ---------------------------------------------------------------------------
// applyBsrSnapshot
// ---------------------------------------------------------------------------

TEST(PinDriverTest, ApplyBsrSnapshotCopiesBytes) {
    bsdl::BSDLDevice dev = makeTestDevice();  // boundary_length = 6 → 1 byte
    std::vector<uint8_t> bsr;

    applyBsrSnapshot(bsr, dev, {0b00101011});

    ASSERT_EQ(bsr.size(), 1u);
    // Only bits 0-5 should survive; bits 6-7 are extra and must be masked off.
    // Input 0b00101011 = bits 0,1,3,5 set; bits 6,7 also set but out of range.
    // keep_mask for 2 extra bits = 0xFF >> 2 = 0x3F
    EXPECT_EQ(bsr[0], 0b00101011u & 0x3Fu);
}

TEST(PinDriverTest, ApplyBsrSnapshotMasksExtraBits) {
    bsdl::BSDLDevice dev = makeTestDevice();  // boundary_length=6 → extra_bits=2
    std::vector<uint8_t> bsr;

    applyBsrSnapshot(bsr, dev, {0xFF});

    ASSERT_EQ(bsr.size(), 1u);
    EXPECT_EQ(bsr[0], 0x3Fu);  // top 2 bits zeroed
}

TEST(PinDriverTest, ApplyBsrSnapshotZeroPadsShortInput) {
    bsdl::BSDLDevice dev;
    dev.boundary_length = 16;  // 2 bytes required
    std::vector<uint8_t> bsr;

    applyBsrSnapshot(bsr, dev, {0xAB});  // only 1 byte supplied

    ASSERT_EQ(bsr.size(), 2u);
    EXPECT_EQ(bsr[0], 0xABu);
    EXPECT_EQ(bsr[1], 0x00u);  // zero-padded
}

TEST(PinDriverTest, ApplyBsrSnapshotTruncatesOversizedInput) {
    bsdl::BSDLDevice dev;
    dev.boundary_length = 8;  // exactly 1 byte
    std::vector<uint8_t> bsr;

    applyBsrSnapshot(bsr, dev, {0x12, 0x34, 0x56});  // 3 bytes supplied

    ASSERT_EQ(bsr.size(), 1u);  // only 1 byte kept
    EXPECT_EQ(bsr[0], 0x12u);
}

TEST(PinDriverTest, ApplyBsrSnapshotHandlesEmptyInput) {
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr(1, 0xFF);

    applyBsrSnapshot(bsr, dev, {});

    ASSERT_EQ(bsr.size(), 1u);
    EXPECT_EQ(bsr[0], 0x00u);  // zero-initialized
}

// ---------------------------------------------------------------------------
// initBsrFromSafeValues
// ---------------------------------------------------------------------------

TEST(PinDriverTest, InitBsrFromSafeValuesSetsOneBits) {
    // In makeTestDevice(): bit1 (CONTROL safe=1) is the only safe_value==1 cell
    bsdl::BSDLDevice dev = makeTestDevice();
    std::vector<uint8_t> bsr;

    initBsrFromSafeValues(bsr, dev);

    ASSERT_EQ(bsr.size(), 1u);
    EXPECT_EQ((bsr[0] >> 1) & 1, 1);  // bit 1 safe_value=1
    EXPECT_EQ((bsr[0] >> 0) & 1, 0);  // bit 0 safe_value=0
    EXPECT_EQ((bsr[0] >> 2) & 1, 0);  // bit 2 safe_value=0
}

TEST(PinDriverTest, InitBsrFromSafeValuesAllocatesCorrectSize) {
    bsdl::BSDLDevice dev;
    dev.boundary_length = 17;  // 3 bytes
    std::vector<uint8_t> bsr;

    initBsrFromSafeValues(bsr, dev);

    EXPECT_EQ(bsr.size(), 3u);
}

TEST(PinDriverTest, InitBsrFromSafeValuesIgnoresDontCare) {
    // safe_value == -1 means X (don't care); should not set the bit
    bsdl::BSDLDevice dev;
    dev.boundary_length = 8;
    dev.boundary_cells.resize(2);
    dev.boundary_cells[0] = {0, "BC_1", "P0", bsdl::CellFunction::INPUT,
                              -1, -1, -1, bsdl::DisableResult::NONE};  // safe=X
    dev.boundary_cells[1] = {1, "BC_1", "P1", bsdl::CellFunction::INPUT,
                               1, -1, -1, bsdl::DisableResult::NONE};  // safe=1
    std::vector<uint8_t> bsr;

    initBsrFromSafeValues(bsr, dev);

    EXPECT_EQ((bsr[0] >> 0) & 1, 0);  // X → not set
    EXPECT_EQ((bsr[0] >> 1) & 1, 1);  // 1 → set
}

} // namespace
} // namespace jtag
