#include "capture_session.h"

#include <stdexcept>

namespace jtag::hardware {

CaptureSession::CaptureSession(Scanner& scanner)
    : scanner_(scanner) {}

void CaptureSession::setBufferDepth(std::size_t depth) {
    buffer_depth_ = (depth < 1) ? 1 : depth;
}

void CaptureSession::setSampleInterval(uint32_t interval_us) {
    sample_interval_us_ = (interval_us < 100) ? 100 : interval_us;
}

void CaptureSession::start() {
    buffer_.resize(buffer_depth_);
    write_pos_ = 0;
    count_     = 0;
    has_prev_  = false;
    last_error_.clear();

    trigger_.arm();

    const auto mode = trigger_.mode();
    if (mode == TriggerMode::FREE_RUN || mode == TriggerMode::SINGLE) {
        state_ = CaptureState::RUNNING;
    } else {
        state_ = CaptureState::WAITING_TRIGGER;
    }

    next_sample_time_ = std::chrono::steady_clock::now();
}

void CaptureSession::stop() {
    state_ = CaptureState::STOPPED;
}

void CaptureSession::clearSamples() {
    buffer_.clear();
    write_pos_ = 0;
    count_     = 0;
    has_prev_  = false;
}

bool CaptureSession::tick() {
    if (state_ == CaptureState::STOPPED || state_ == CaptureState::COMPLETE) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < next_sample_time_) {
        return false;
    }

    next_sample_time_ = now + std::chrono::microseconds(sample_interval_us_);

    ScanResult result = scanner_.sample();
    if (!scanner_.lastError().empty()) {
        last_error_ = scanner_.lastError();
        state_ = CaptureState::STOPPED;
        return false;
    }

    bool trig_point = false;

    switch (state_) {
        case CaptureState::RUNNING: {
            const auto mode = trigger_.mode();
            if (mode == TriggerMode::SINGLE) {
                appendSample(result, false);
                state_ = CaptureState::COMPLETE;
            } else {
                appendSample(result, false);
                // Free run: just keep filling buffer.
                if (count_ >= buffer_depth_) {
                    state_ = CaptureState::COMPLETE;
                }
            }
            break;
        }
        case CaptureState::WAITING_TRIGGER: {
            bool fired = false;
            if (has_prev_) {
                fired = trigger_.evaluate(prev_sample_, result);
            }
            if (fired) {
                trig_point = true;
                // Compute how many samples to collect post-trigger.
                const float ratio = trigger_.preTriggerRatio();
                const std::size_t pre = static_cast<std::size_t>(
                    ratio * static_cast<float>(buffer_depth_));
                post_trigger_remaining_ = buffer_depth_ - pre;
                appendSample(result, true);
                --post_trigger_remaining_;
                state_ = (post_trigger_remaining_ > 0)
                             ? CaptureState::TRIGGERED
                             : CaptureState::COMPLETE;
            } else {
                appendSample(result, false);
            }
            break;
        }
        case CaptureState::TRIGGERED: {
            appendSample(result, false);
            if (post_trigger_remaining_ > 0) --post_trigger_remaining_;
            if (post_trigger_remaining_ == 0) {
                state_ = CaptureState::COMPLETE;
            }
            break;
        }
        default:
            break;
    }

    prev_sample_ = result;
    has_prev_    = true;
    return true;
}

void CaptureSession::appendSample(ScanResult result, bool trigger_point) {
    if (buffer_.empty()) buffer_.resize(buffer_depth_);

    SampleFrame frame;
    frame.timestamp    = std::chrono::steady_clock::now();
    frame.data         = std::move(result);
    frame.trigger_point = trigger_point;

    buffer_[write_pos_] = std::move(frame);
    write_pos_ = (write_pos_ + 1) % buffer_depth_;
    if (count_ < buffer_depth_) ++count_;
}

std::vector<SampleFrame> CaptureSession::getSamples() const {
    if (count_ == 0) return {};
    if (count_ < buffer_depth_) {
        return std::vector<SampleFrame>(buffer_.begin(),
                                        buffer_.begin() +
                                            static_cast<std::ptrdiff_t>(count_));
    }
    // Ring buffer: oldest sample is at write_pos_.
    std::vector<SampleFrame> out;
    out.reserve(buffer_depth_);
    for (std::size_t i = 0; i < buffer_depth_; ++i) {
        out.push_back(buffer_[(write_pos_ + i) % buffer_depth_]);
    }
    return out;
}

}  // namespace jtag::hardware
