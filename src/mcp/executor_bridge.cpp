#include "executor_bridge.h"

#include <stdexcept>
#include <thread>

#include "src/hardware/hardware_job.h"

namespace jtag::mcp {

ExecutorBridge::ExecutorBridge(hardware::HardwareExecutor& executor)
    : executor_(executor) {}

// ---------------------------------------------------------------------------
// submitSync
// ---------------------------------------------------------------------------

nlohmann::json ExecutorBridge::submitSync(hardware::HardwareTask task,
                                          std::string label,
                                          std::chrono::milliseconds timeout) {
    auto handle = executor_.submit(std::move(task), std::move(label));
    job_table_.add(handle);

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        const auto snap = handle->snapshot();
        switch (snap.state) {
            case hardware::JobState::kComplete:
                return snap.result.is_null() ? nlohmann::json::object()
                                             : snap.result;
            case hardware::JobState::kFailed:
                throw std::runtime_error(
                    snap.error.empty() ? "hardware task failed" : snap.error);
            case hardware::JobState::kCancelled:
                throw std::runtime_error("job was cancelled");
            default:
                break;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            // Return job_id so the client can poll asynchronously.
            return nlohmann::json{{"job_id", handle->id()}};
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ---------------------------------------------------------------------------
// submitAsync
// ---------------------------------------------------------------------------

nlohmann::json ExecutorBridge::submitAsync(hardware::HardwareTask task,
                                           std::string label) {
    auto handle = executor_.submit(std::move(task), std::move(label));
    job_table_.add(handle);
    return nlohmann::json{{"job_id", handle->id()}};
}

// ---------------------------------------------------------------------------
// pollJob
// ---------------------------------------------------------------------------

nlohmann::json ExecutorBridge::pollJob(const std::string& job_id) const {
    auto opt = job_table_.get(job_id);
    if (!opt) {
        return nlohmann::json{{"error", "not_found"},
                              {"job_id", job_id}};
    }
    return hardware::snapshotToJson((*opt)->snapshot());
}

// ---------------------------------------------------------------------------
// cancelJob
// ---------------------------------------------------------------------------

nlohmann::json ExecutorBridge::cancelJob(const std::string& job_id) {
    auto opt = job_table_.get(job_id);
    if (!opt) {
        return nlohmann::json{{"error", "not_found"},
                              {"job_id", job_id}};
    }
    (*opt)->requestCancel();
    executor_.tryCancel(job_id);
    return nlohmann::json{{"cancelled", true}, {"job_id", job_id}};
}

}  // namespace jtag::mcp
