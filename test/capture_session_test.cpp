// Tests for CaptureSession state machine (no hardware tick).
//
// Scanner / JtagChain / TapController / FtdiDevice are constructed in-process
// without opening hardware.  All tests avoid calling tick() (which would
// invoke Scanner::sample() and crash without real hardware).
#include <gtest/gtest.h>

#include "src/boundary_scan/scanner.h"
#include "src/capture/capture_engine.h"
#include "src/capture/trigger.h"
#include "src/ftdi/ftdi_device.h"
#include "src/hardware/capture_session.h"
#include "src/jtag/jtag_chain.h"
#include "src/jtag/tap_controller.h"

namespace jtag::hardware {
namespace {

// Builds a dummy Scanner that is syntactically valid but has no real hardware.
struct HwFixture {
    FtdiDevice   ftdi;
    TapController tap{ftdi};
    JtagChain    chain{tap};
    Scanner      scanner{chain, 0};
};

// ── Initial state ─────────────────────────────────────────────────────────────

TEST(CaptureSessionTest, InitialStateIsStopped) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    EXPECT_EQ(cs.state(), CaptureState::STOPPED);
    EXPECT_EQ(cs.sampleCount(), 0u);
}

TEST(CaptureSessionTest, DefaultParameters) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    EXPECT_GT(cs.bufferDepth(), 0u);
    EXPECT_GT(cs.sampleInterval(), 0u);
}

// ── Configuration setters ─────────────────────────────────────────────────────

TEST(CaptureSessionTest, SetBufferDepth) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    cs.setBufferDepth(512);
    EXPECT_EQ(cs.bufferDepth(), 512u);
}

TEST(CaptureSessionTest, SetSampleInterval) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    cs.setSampleInterval(500);
    EXPECT_EQ(cs.sampleInterval(), 500u);
}

// ── start() / stop() state transitions ───────────────────────────────────────

TEST(CaptureSessionTest, StartTransitionsToRunningForFreeRun) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    cs.trigger().setMode(TriggerMode::FREE_RUN);
    cs.start();
    EXPECT_NE(cs.state(), CaptureState::STOPPED);
}

TEST(CaptureSessionTest, StartTransitionsToWaitingTriggerForNormal) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    cs.trigger().setMode(TriggerMode::NORMAL);
    cs.start();
    EXPECT_EQ(cs.state(), CaptureState::WAITING_TRIGGER);
}

TEST(CaptureSessionTest, StartSingleModeNotStopped) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    cs.trigger().setMode(TriggerMode::SINGLE);
    cs.start();
    EXPECT_NE(cs.state(), CaptureState::STOPPED);
}

TEST(CaptureSessionTest, StopReturnsStopped) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    cs.trigger().setMode(TriggerMode::FREE_RUN);
    cs.start();
    cs.stop();
    EXPECT_EQ(cs.state(), CaptureState::STOPPED);
}

TEST(CaptureSessionTest, DoubleStopIsHarmless) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    cs.stop();  // Already stopped — should be a no-op.
    EXPECT_EQ(cs.state(), CaptureState::STOPPED);
}

// ── clearSamples ─────────────────────────────────────────────────────────────

TEST(CaptureSessionTest, ClearSamplesWhenStopped) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    // getSamples() should return empty when no samples taken.
    EXPECT_TRUE(cs.getSamples().empty());
    cs.clearSamples();
    EXPECT_EQ(cs.sampleCount(), 0u);
    EXPECT_TRUE(cs.getSamples().empty());
}

// ── TriggerEngine accessor ────────────────────────────────────────────────────

TEST(CaptureSessionTest, TriggerAccessorRoundTrip) {
    HwFixture hw;
    CaptureSession cs(hw.scanner);
    cs.trigger().setMode(TriggerMode::NORMAL);
    EXPECT_EQ(cs.trigger().mode(), TriggerMode::NORMAL);
    cs.trigger().setMode(TriggerMode::FREE_RUN);
    EXPECT_EQ(cs.trigger().mode(), TriggerMode::FREE_RUN);
}

}  // namespace
}  // namespace jtag::hardware
