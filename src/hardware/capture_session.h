#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "src/boundary_scan/scanner.h"
#include "src/capture/capture_engine.h"  // SampleFrame, CaptureState
#include "src/capture/trigger.h"

namespace jtag::hardware {

// Non-threaded capture state machine for use inside the HardwareExecutor
// worker thread.  Unlike CaptureEngine (which has its own thread), the
// caller must drive CaptureSession by calling tick() repeatedly.
//
// Lifecycle:
//   CaptureSession cs(scanner);
//   cs.setBufferDepth(4096);
//   cs.setSampleInterval(1000);           // 1 ms
//   cs.trigger().setMode(TriggerMode::FREE_RUN);
//   cs.start();
//   while (cs.state() != CaptureState::COMPLETE) {
//       cs.tick();
//       if (job.isCancelRequested()) { cs.stop(); break; }
//       std::this_thread::sleep_for(std::chrono::milliseconds(1));
//   }
//   auto samples = cs.getSamples();
class CaptureSession {
public:
    explicit CaptureSession(Scanner& scanner);

    // Non-copyable, non-movable.
    CaptureSession(const CaptureSession&)            = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;
    CaptureSession(CaptureSession&&)                 = delete;
    CaptureSession& operator=(CaptureSession&&)      = delete;

    // -----------------------------------------------------------------------
    // Configuration (must be set before start())
    // -----------------------------------------------------------------------

    void setBufferDepth(std::size_t depth);
    std::size_t bufferDepth() const { return buffer_depth_; }

    // Minimum interval between samples in microseconds.
    void setSampleInterval(uint32_t interval_us);
    uint32_t sampleInterval() const { return sample_interval_us_; }

    TriggerEngine& trigger() { return trigger_; }
    const TriggerEngine& trigger() const { return trigger_; }

    // -----------------------------------------------------------------------
    // Control
    // -----------------------------------------------------------------------

    // Arm and start capture.  Transitions from STOPPED → RUNNING or
    // WAITING_TRIGGER depending on trigger mode.
    void start();

    // Stop capture immediately.  Transitions to STOPPED.
    void stop();

    // Clear sample buffer.  Only valid when STOPPED.
    void clearSamples();

    // -----------------------------------------------------------------------
    // Tick (worker-thread only)
    // -----------------------------------------------------------------------

    // Perform at most one Scanner::sample() call if the sample interval has
    // elapsed.  Returns true if a sample was taken.
    // Must be called from the hardware worker thread.
    bool tick();

    // -----------------------------------------------------------------------
    // State accessors (worker-thread only)
    // -----------------------------------------------------------------------

    CaptureState state() const { return state_; }
    std::size_t  sampleCount() const { return count_; }

    // Return a copy of the sample buffer (may be called after stop/complete).
    std::vector<SampleFrame> getSamples() const;

    const std::string& lastError() const { return last_error_; }

private:
    void appendSample(ScanResult result, bool trigger_point);

    Scanner&      scanner_;
    TriggerEngine trigger_;

    std::size_t  buffer_depth_     = 10000;
    uint32_t     sample_interval_us_ = 1000;

    std::vector<SampleFrame>                 buffer_;
    std::size_t                              write_pos_ = 0;
    std::size_t                              count_     = 0;
    std::size_t                              post_trigger_remaining_ = 0;

    CaptureState state_ = CaptureState::STOPPED;

    std::chrono::steady_clock::time_point next_sample_time_{};
    ScanResult                            prev_sample_{};
    bool                                  has_prev_ = false;
    std::string                           last_error_;
};

}  // namespace jtag::hardware
