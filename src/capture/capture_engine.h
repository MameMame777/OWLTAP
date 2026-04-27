#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "trigger.h"
#include "src/boundary_scan/scanner.h"

namespace jtag {

/// A single captured sample frame
struct SampleFrame {
    std::chrono::steady_clock::time_point timestamp;
    ScanResult data;
    bool trigger_point = false;  // True if trigger fired on this sample
};

/// Capture engine state
enum class CaptureState {
    STOPPED,          // Not capturing
    RUNNING,          // Actively capturing
    WAITING_TRIGGER,  // Capturing but waiting for trigger
    TRIGGERED,        // Trigger fired, filling post-trigger buffer
    COMPLETE,         // Capture complete (single trigger filled buffer)
};

/// Callback for capture events
using CaptureCallback = std::function<void(const SampleFrame& frame,
                                            CaptureState state)>;

/// Threaded capture engine that periodically reads pin states via JTAG
/// boundary scan and stores them in a ring buffer.
class CaptureEngine {
public:
    /// @param scanner  Reference to initialized Scanner
    explicit CaptureEngine(Scanner& scanner);
    ~CaptureEngine();

    // Non-copyable, non-movable
    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    /// Set the capture buffer depth (number of samples).
    /// Must be called before start().
    void setBufferDepth(size_t depth);
    size_t bufferDepth() const { return buffer_depth_; }

    /// Set the sample interval.
    /// @param interval_us  Microseconds between samples (minimum ~100us)
    void setSampleInterval(uint32_t interval_us);
    uint32_t sampleInterval() const { return sample_interval_us_.load(); }

    /// Set capture callback (called from capture thread).
    void setCallback(CaptureCallback callback);

    /// Get trigger engine for configuration.
    TriggerEngine& trigger() { return trigger_; }
    const TriggerEngine& trigger() const { return trigger_; }

    /// Start capturing.
    /// @return true if started successfully
    bool start();

    /// Stop capturing.
    void stop();

    /// Get current capture state.
    CaptureState state() const { return state_.load(); }

    /// Get current number of samples in buffer.
    size_t sampleCount() const;

    /// Get all captured samples (thread-safe copy).
    std::vector<SampleFrame> getSamples() const;

    /// Get only samples written since last call (incremental, low-overhead).
    /// @param inout_last_total  In: total written at last call (0 = first call).
    ///                         Out: updated to current total_written_.
    /// Returns newly-added frames only. Falls back to full copy if ring wrapped.
    std::vector<SampleFrame> getNewSamples(size_t& inout_last_total) const;

    /// Clear all captured samples while stopped.
    void clearSamples();

    /// Get the most recent sample.
    SampleFrame getLatestSample() const;

    /// Get the effective sample rate (measured, in Hz).
    double effectiveSampleRate() const;

    /// Get last error message.
    const std::string& lastError() const { return last_error_; }

private:
    void captureLoop();

    Scanner& scanner_;
    TriggerEngine trigger_;

    // Configuration
    size_t buffer_depth_ = 10000;
    std::atomic<uint32_t> sample_interval_us_{1000};  // 1ms default
    CaptureCallback callback_;

    // Ring buffer
    mutable std::mutex buffer_mutex_;
    std::vector<SampleFrame> buffer_;
    size_t write_pos_ = 0;
    size_t count_ = 0;
    std::atomic<size_t> total_written_{0};  // monotonic write counter
    int trigger_sample_index_ = -1;
    size_t post_trigger_remaining_ = 0;

    // Thread control
    std::thread capture_thread_;
    std::atomic<CaptureState> state_{CaptureState::STOPPED};
    std::atomic<bool> stop_requested_{false};
    std::condition_variable stop_cv_;
    std::mutex stop_mutex_;

    // Metrics
    std::atomic<double> effective_rate_{0.0};
    std::string last_error_;
};

} // namespace jtag
