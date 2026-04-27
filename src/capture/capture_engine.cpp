#include "capture_engine.h"

#include <algorithm>

namespace jtag {

CaptureEngine::CaptureEngine(Scanner& scanner) : scanner_(scanner) {}

CaptureEngine::~CaptureEngine() {
    stop();
}

void CaptureEngine::setBufferDepth(size_t depth) {
    if (state_.load() != CaptureState::STOPPED) return;
    buffer_depth_ = std::max(depth, size_t{100});
}

void CaptureEngine::setSampleInterval(uint32_t interval_us) {
    sample_interval_us_.store(std::max(interval_us, uint32_t{100}));
}

void CaptureEngine::setCallback(CaptureCallback callback) {
    if (state_.load() != CaptureState::STOPPED) return;
    callback_ = std::move(callback);
}

bool CaptureEngine::start() {
    if (state_.load() != CaptureState::STOPPED) {
        last_error_ = "Capture already running";
        return false;
    }

    if (!scanner_.isReady()) {
        last_error_ = "Scanner not ready";
        return false;
    }

    // Initialize buffer
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_.resize(buffer_depth_);
        write_pos_ = 0;
        count_ = 0;
        trigger_sample_index_ = -1;
    }

    // Arm trigger
    trigger_.arm();

    stop_requested_.store(false);

    // SINGLE: immediate snapshot, bypass trigger conditions entirely.
    // FREE_RUN: continuous capture without trigger.
    // NORMAL: wait for trigger condition, fill buffer, re-arm.
    if (trigger_.mode() == TriggerMode::FREE_RUN ||
        trigger_.mode() == TriggerMode::SINGLE) {
        state_.store(CaptureState::RUNNING);
    } else {
        state_.store(CaptureState::WAITING_TRIGGER);
    }

    capture_thread_ = std::thread(&CaptureEngine::captureLoop, this);
    return true;
}

void CaptureEngine::stop() {
    if (state_.load() == CaptureState::STOPPED) return;

    stop_requested_.store(true);
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        stop_cv_.notify_all();
    }

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    state_.store(CaptureState::STOPPED);
}

size_t CaptureEngine::sampleCount() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return count_;
}

std::vector<SampleFrame> CaptureEngine::getSamples() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    std::vector<SampleFrame> result;
    result.reserve(count_);

    if (count_ < buffer_.size()) {
        // Buffer not yet full
        for (size_t i = 0; i < count_; i++) {
            result.push_back(buffer_[i]);
        }
    } else {
        // Buffer full: read from write_pos (oldest) to write_pos-1 (newest)
        for (size_t i = 0; i < buffer_.size(); i++) {
            size_t idx = (write_pos_ + i) % buffer_.size();
            result.push_back(buffer_[idx]);
        }
    }

    return result;
}

void CaptureEngine::clearSamples() {
    if (state_.load() != CaptureState::STOPPED) return;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    write_pos_ = 0;
    count_ = 0;
    total_written_.store(0, std::memory_order_relaxed);
    trigger_sample_index_ = -1;
    post_trigger_remaining_ = 0;
    effective_rate_.store(0.0);
}

std::vector<SampleFrame> CaptureEngine::getNewSamples(
        size_t& inout_last_total) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    const size_t current = total_written_.load(std::memory_order_relaxed);
    if (current <= inout_last_total) {
        return {};  // nothing new
    }

    const size_t new_count = current - inout_last_total;

    // If ring has wrapped since last call, fall back to full copy.
    if (new_count > buffer_.size()) {
        inout_last_total = current;
        std::vector<SampleFrame> result;
        result.reserve(count_);
        for (size_t i = 0; i < buffer_.size(); i++) {
            size_t idx = (write_pos_ + i) % buffer_.size();
            result.push_back(buffer_[idx]);
        }
        return result;
    }

    // Return only the new frames (oldest-first).
    // They occupy physical indices [inout_last_total % size .. current % size).
    std::vector<SampleFrame> result;
    result.reserve(new_count);
    const size_t sz = buffer_.size();
    for (size_t i = 0; i < new_count; i++) {
        size_t idx = (inout_last_total + i) % sz;
        result.push_back(buffer_[idx]);
    }
    inout_last_total = current;
    return result;
}

SampleFrame CaptureEngine::getLatestSample() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (count_ == 0) return {};
    size_t idx = (write_pos_ == 0) ? buffer_.size() - 1 : write_pos_ - 1;
    return buffer_[idx];
}

double CaptureEngine::effectiveSampleRate() const {
    return effective_rate_.load();
}

void CaptureEngine::captureLoop() {
    using Clock = std::chrono::steady_clock;

    ScanResult prev_result;
    bool has_prev = false;
    auto interval = std::chrono::microseconds(sample_interval_us_.load());

    auto loop_start = Clock::now();
    size_t sample_count = 0;

    while (!stop_requested_.load()) {
        auto sample_start = Clock::now();

        // Perform boundary scan SAMPLE
        ScanResult result = scanner_.sample();

        SampleFrame frame;
        frame.timestamp = Clock::now();
        frame.data = result;
        frame.trigger_point = false;

        // Evaluate trigger
        CaptureState current_state = state_.load();
        if (current_state == CaptureState::WAITING_TRIGGER && has_prev) {
            if (trigger_.evaluate(prev_result, result)) {
                frame.trigger_point = true;
                state_.store(CaptureState::TRIGGERED);

                // Calculate post-trigger samples
                float post_ratio = 1.0f - trigger_.preTriggerRatio();
                post_trigger_remaining_ =
                    static_cast<size_t>(buffer_depth_ * post_ratio);

                current_state = CaptureState::TRIGGERED;
            }
        }

        if (current_state == CaptureState::TRIGGERED) {
            if (post_trigger_remaining_ == 0) {
                state_.store(CaptureState::COMPLETE);
                if (trigger_.mode() == TriggerMode::SINGLE) {
                    break;  // Stop capture
                } else {
                    // Re-arm for NORMAL mode
                    trigger_.arm();
                    state_.store(CaptureState::WAITING_TRIGGER);
                }
            } else {
                post_trigger_remaining_--;
            }
        }

        // Store in ring buffer
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_[write_pos_] = frame;
            write_pos_ = (write_pos_ + 1) % buffer_.size();
            if (count_ < buffer_.size()) count_++;
            total_written_.fetch_add(1, std::memory_order_relaxed);
        }

        sample_count++;
        prev_result = result;
        has_prev = true;

        // SINGLE mode: stop immediately after one frame (snapshot).
        if (trigger_.mode() == TriggerMode::SINGLE &&
            current_state == CaptureState::RUNNING) {
            state_.store(CaptureState::COMPLETE);
            break;
        }

        // Invoke callback
        if (callback_) {
            callback_(frame, state_.load());
        }

        // Update effective rate every 100 samples
        if (sample_count % 100 == 0) {
            auto elapsed = Clock::now() - loop_start;
            double secs = std::chrono::duration<double>(elapsed).count();
            if (secs > 0) {
                effective_rate_.store(sample_count / secs);
            }
        }

        // Wait for next sample interval
        auto elapsed = Clock::now() - sample_start;
        auto interval = std::chrono::microseconds(sample_interval_us_.load());
        if (elapsed < interval) {
            std::unique_lock<std::mutex> lock(stop_mutex_);
            stop_cv_.wait_for(lock, interval - elapsed,
                              [this] { return stop_requested_.load(); });
        }
    }
}

} // namespace jtag
