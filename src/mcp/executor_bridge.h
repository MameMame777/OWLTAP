#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "job_table.h"
#include "src/hardware/hardware_context.h"
#include "src/hardware/hardware_executor.h"
#include "src/hardware/hardware_job.h"

namespace jtag::mcp {

// Bridges MCP tool handlers to the HardwareExecutor.
//
// Two submission modes:
//   submitSync()  – submit + wait up to timeout_ms; returns result JSON
//                   directly if the job completes in time, or throws on error,
//                   or returns {"job_id":"..."} if it times out.
//   submitAsync() – submit without waiting; stores the job in JobTable;
//                   returns {"job_id":"..."} immediately.
//
// MCP job_poll / job_cancel are implemented by pollJob() / cancelJob().
class ExecutorBridge {
public:
    // Constructs a bridge that submits to the given executor.
    // The executor must outlive this bridge.
    explicit ExecutorBridge(hardware::HardwareExecutor& executor);

    // -----------------------------------------------------------------------
    // Submission
    // -----------------------------------------------------------------------

    // Submit a task and wait up to timeout_ms milliseconds.
    // - On success within timeout: returns result JSON.
    // - On failure within timeout: throws std::runtime_error.
    // - On timeout:               returns {"job_id": "<uuid>"}.
    //   The caller must relay the job_id to the client for async polling.
    //
    // The job is always stored in the JobTable regardless of outcome.
    nlohmann::json submitSync(hardware::HardwareTask task,
                              std::string label,
                              std::chrono::milliseconds timeout);

    // Submit a task without waiting.  Returns {"job_id": "<uuid>"} immediately.
    // The job is stored in the JobTable and can be polled with pollJob().
    nlohmann::json submitAsync(hardware::HardwareTask task,
                               std::string label);

    // -----------------------------------------------------------------------
    // MCP job management
    // -----------------------------------------------------------------------

    // Return JSON snapshot of a job, or {"error":"not_found"} if unknown.
    nlohmann::json pollJob(const std::string& job_id) const;

    // Request cancellation.  Returns {"cancelled":true} or {"error":"..."}.
    nlohmann::json cancelJob(const std::string& job_id);

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    hardware::HardwareExecutor& executor() { return executor_; }
    JobTable& jobTable() { return job_table_; }

private:
    hardware::HardwareExecutor& executor_;
    JobTable                    job_table_;
};

}  // namespace jtag::mcp
