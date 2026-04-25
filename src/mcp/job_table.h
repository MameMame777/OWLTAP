#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "src/hardware/hardware_job.h"

namespace jtag::mcp {

using JobHandle = std::shared_ptr<hardware::HardwareJob>;

// Thread-safe table that keeps JobHandles alive so MCP clients can poll
// terminal state even after the worker thread has finished.
//
// Jobs remain in the table until explicitly removed or cleanup() is called.
class JobTable {
public:
    // Store a handle, keyed by handle->id().
    void add(JobHandle handle);

    // Look up by job ID.  Returns nullopt if not present.
    std::optional<JobHandle> get(const std::string& job_id) const;

    // Remove a job entry.  No-op if not present.
    void remove(const std::string& job_id);

    // Remove all jobs that are in a terminal state (complete/failed/cancelled).
    void cleanup();

    // Number of tracked jobs.
    std::size_t size() const;

private:
    mutable std::mutex                           mu_;
    std::unordered_map<std::string, JobHandle>   jobs_;
};

}  // namespace jtag::mcp
