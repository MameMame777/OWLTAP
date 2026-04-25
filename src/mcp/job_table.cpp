#include "job_table.h"

namespace jtag::mcp {

void JobTable::add(JobHandle handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(mu_);
    jobs_.emplace(handle->id(), std::move(handle));
}

std::optional<JobHandle> JobTable::get(const std::string& job_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) return std::nullopt;
    return it->second;
}

void JobTable::remove(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(mu_);
    jobs_.erase(job_id);
}

void JobTable::cleanup() {
    using S = hardware::JobState;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = jobs_.begin(); it != jobs_.end(); ) {
        const S s = it->second->state();
        if (s == S::kComplete || s == S::kFailed || s == S::kCancelled) {
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

std::size_t JobTable::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return jobs_.size();
}

}  // namespace jtag::mcp
