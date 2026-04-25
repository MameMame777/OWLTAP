#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "hardware_context.h"
#include "hardware_job.h"
#include "src/gui/app_config.h"

namespace jtag::hardware {

using JobHandle  = std::shared_ptr<HardwareJob>;
// Task function signature: receives context and job (for progress/cancel).
// Must return a JSON result value.  Throw std::exception on failure.
using HardwareTask =
    std::function<nlohmann::json(HardwareContext&, HardwareJob&)>;

// Single-threaded hardware execution queue.
//
// One worker thread serialises all FTDI/JTAG operations.  Tasks are submitted
// from any thread and executed in FIFO order.  The caller receives a JobHandle
// (shared_ptr<HardwareJob>) that can be polled or used to request cancellation.
//
// Lifecycle: start() → submit() … → shutdown().
class HardwareExecutor {
public:
    explicit HardwareExecutor(gui::AppConfig cfg);
    ~HardwareExecutor();

    // Non-copyable, non-movable.
    HardwareExecutor(const HardwareExecutor&)            = delete;
    HardwareExecutor& operator=(const HardwareExecutor&) = delete;
    HardwareExecutor(HardwareExecutor&&)                 = delete;
    HardwareExecutor& operator=(HardwareExecutor&&)      = delete;

    // Open hardware and start the worker thread.
    // Returns empty string on success, or a human-readable error on failure.
    std::string start();

    // True after start() and before shutdown().
    bool isRunning() const;

    // Submit a task.  Returns the JobHandle immediately; execution is async.
    // Throws std::runtime_error if the executor is not running.
    // label is an optional human-readable name stored in the job.
    JobHandle submit(HardwareTask task, std::string label = "");

    // Request cancellation of the job with the given ID.
    // No-op if the ID is unknown or the job is already terminal.
    void tryCancel(const std::string& job_id);

    // Return a snapshot of the job state, or nullopt if the ID is unknown.
    std::optional<HardwareJob::Snapshot> poll(const std::string& job_id) const;

    // Stop accepting new jobs, wait for the current job to finish, close
    // hardware, and join the worker thread.
    void shutdown();

    // Direct access to hardware context (worker-thread use only).
    HardwareContext& context() { return *context_; }

private:
    void workerLoop();

    static std::string generateJobId();

    std::unique_ptr<HardwareContext> context_;
    std::atomic<bool>                running_{false};
    std::atomic<bool>                stop_flag_{false};

    mutable std::mutex                           queue_mu_;
    std::condition_variable                      queue_cv_;
    std::deque<std::pair<JobHandle, HardwareTask>> queue_;

    mutable std::mutex                               registry_mu_;
    std::unordered_map<std::string,
                       std::weak_ptr<HardwareJob>>   job_registry_;

    std::thread worker_;
};

}  // namespace jtag::hardware
