#include "hardware_job.h"

namespace jtag::hardware {

const char* jobStateName(JobState s) {
    switch (s) {
        case JobState::kQueued:    return "queued";
        case JobState::kRunning:   return "running";
        case JobState::kComplete:  return "complete";
        case JobState::kFailed:    return "failed";
        case JobState::kCancelled: return "cancelled";
    }
    return "unknown";
}

HardwareJob::HardwareJob(std::string id, std::string label)
    : id_(std::move(id)), label_(std::move(label)) {}

JobState HardwareJob::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

nlohmann::json HardwareJob::progress() const {
    std::lock_guard<std::mutex> lk(mu_);
    return progress_;
}

nlohmann::json HardwareJob::result() const {
    std::lock_guard<std::mutex> lk(mu_);
    return result_;
}

std::string HardwareJob::error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

bool HardwareJob::isCancelRequested() const {
    return cancel_requested_.load(std::memory_order_acquire);
}

void HardwareJob::setState(JobState s) {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = s;
}

void HardwareJob::setProgress(nlohmann::json p) {
    std::lock_guard<std::mutex> lk(mu_);
    progress_ = std::move(p);
}

void HardwareJob::setResult(nlohmann::json r) {
    std::lock_guard<std::mutex> lk(mu_);
    result_ = std::move(r);
}

void HardwareJob::setError(std::string e) {
    std::lock_guard<std::mutex> lk(mu_);
    error_ = std::move(e);
}

void HardwareJob::requestCancel() {
    cancel_requested_.store(true, std::memory_order_release);
}

HardwareJob::Snapshot HardwareJob::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return Snapshot{id_, state_, progress_, result_, error_};
}

nlohmann::json snapshotToJson(const HardwareJob::Snapshot& snap) {
    nlohmann::json j;
    j["job_id"] = snap.id;
    j["state"]  = jobStateName(snap.state);
    if (!snap.progress.is_null()) j["progress"] = snap.progress;
    if (!snap.result.is_null())   j["result"]   = snap.result;
    if (!snap.error.empty())      j["error"]    = snap.error;
    return j;
}

}  // namespace jtag::hardware
