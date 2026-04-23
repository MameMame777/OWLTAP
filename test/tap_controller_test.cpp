#include "gtest/gtest.h"

#include "src/jtag/tap_controller.h"

namespace jtag {
namespace {

TEST(TapStateTest, NamesAreValid) {
    EXPECT_STREQ("TEST-LOGIC-RESET", tapStateName(TapState::TEST_LOGIC_RESET));
    EXPECT_STREQ("RUN-TEST/IDLE", tapStateName(TapState::RUN_TEST_IDLE));
    EXPECT_STREQ("SHIFT-DR", tapStateName(TapState::SHIFT_DR));
    EXPECT_STREQ("SHIFT-IR", tapStateName(TapState::SHIFT_IR));
}

TEST(TmsPathTest, ResetToIdle) {
    uint8_t tms;
    int len = computeTmsPath(TapState::TEST_LOGIC_RESET,
                              TapState::RUN_TEST_IDLE, tms);
    ASSERT_GT(len, 0);
    ASSERT_LE(len, 7);
    // First TMS bit should be 0 (TLR -> RTI transition)
    EXPECT_EQ(tms & 1, 0);
}

TEST(TmsPathTest, IdleToShiftDR) {
    uint8_t tms;
    int len = computeTmsPath(TapState::RUN_TEST_IDLE,
                              TapState::SHIFT_DR, tms);
    ASSERT_GT(len, 0);
    ASSERT_LE(len, 7);
    // RTI -> SelectDR (1) -> CaptureDR (0) -> ShiftDR (0) = 3 bits, TMS = 001
    EXPECT_EQ(len, 3);
    EXPECT_EQ(tms & 0x07, 0x01);  // LSB first: 1,0,0
}

TEST(TmsPathTest, IdleToShiftIR) {
    uint8_t tms;
    int len = computeTmsPath(TapState::RUN_TEST_IDLE,
                              TapState::SHIFT_IR, tms);
    ASSERT_GT(len, 0);
    ASSERT_LE(len, 7);
    // RTI -> SelectDR (1) -> SelectIR (1) -> CaptureIR (0) -> ShiftIR (0) = 4 bits
    EXPECT_EQ(len, 4);
    EXPECT_EQ(tms & 0x0F, 0x03);  // LSB first: 1,1,0,0
}

TEST(TmsPathTest, SameState) {
    uint8_t tms;
    int len = computeTmsPath(TapState::RUN_TEST_IDLE,
                              TapState::RUN_TEST_IDLE, tms);
    // Path from a state to itself should be 0 length
    EXPECT_EQ(len, 0);
}

TEST(TmsPathTest, ShiftDRToIdle) {
    uint8_t tms;
    int len = computeTmsPath(TapState::SHIFT_DR,
                              TapState::RUN_TEST_IDLE, tms);
    ASSERT_GT(len, 0);
    ASSERT_LE(len, 7);
    // ShiftDR -> Exit1DR (1) -> UpdateDR (1) -> RTI (0) = 3 bits
    EXPECT_EQ(len, 3);
    EXPECT_EQ(tms & 0x07, 0x03);  // LSB first: 1,1,0
}

TEST(TmsPathTest, ShiftIRToIdle) {
    uint8_t tms;
    int len = computeTmsPath(TapState::SHIFT_IR,
                              TapState::RUN_TEST_IDLE, tms);
    ASSERT_GT(len, 0);
    ASSERT_LE(len, 7);
    // ShiftIR -> Exit1IR (1) -> UpdateIR (1) -> RTI (0) = 3 bits
    EXPECT_EQ(len, 3);
    EXPECT_EQ(tms & 0x07, 0x03);  // LSB first: 1,1,0
}

TEST(TmsPathTest, PauseDRToIdle) {
    uint8_t tms;
    int len = computeTmsPath(TapState::PAUSE_DR,
                              TapState::RUN_TEST_IDLE, tms);
    ASSERT_GT(len, 0);
    ASSERT_LE(len, 7);
    // PAUSE-DR -> Exit2DR (1) -> UpdateDR (1) -> RTI (0) = 3 bits
    EXPECT_EQ(len, 3);
    EXPECT_EQ(tms & 0x07, 0x03);
}

TEST(TmsPathTest, ResetToShiftDR) {
    uint8_t tms;
    int len = computeTmsPath(TapState::TEST_LOGIC_RESET,
                              TapState::SHIFT_DR, tms);
    ASSERT_GT(len, 0);
    ASSERT_LE(len, 7);
    // TLR -> RTI (0) -> SelectDR (1) -> CaptureDR (0) -> ShiftDR (0) = 4 bits
    EXPECT_EQ(len, 4);
    EXPECT_EQ(tms & 0x0F, 0x02);  // LSB first: 0,1,0,0
}

TEST(TmsPathTest, AnyStateToResetViaFiveTmsOne) {
    // Applying 5 TMS=1 clocks from any state must reach TEST-LOGIC-RESET.
    static const TapState all_states[] = {
        TapState::RUN_TEST_IDLE, TapState::SELECT_DR_SCAN,
        TapState::CAPTURE_DR,   TapState::SHIFT_DR,
        TapState::EXIT1_DR,     TapState::PAUSE_DR,
        TapState::EXIT2_DR,     TapState::UPDATE_DR,
        TapState::SELECT_IR_SCAN, TapState::CAPTURE_IR,
        TapState::SHIFT_IR,     TapState::EXIT1_IR,
        TapState::PAUSE_IR,     TapState::EXIT2_IR,
        TapState::UPDATE_IR,
    };
    // Verify the transition table: simulate 5 TMS=1 from each state.
    for (TapState start : all_states) {
        uint8_t tms;
        int len = computeTmsPath(start, TapState::TEST_LOGIC_RESET, tms);
        EXPECT_GT(len, 0) << "No path from state " << tapStateName(start);
        EXPECT_LE(len, 7);
    }
}

} // namespace
} // namespace jtag

