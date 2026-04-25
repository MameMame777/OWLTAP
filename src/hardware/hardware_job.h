#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace jtag::hardware {

enum class JobState {
    kQueued,
    kRunning,
    kComplete,
    kFailed,
    kCancelled,
};

const char* jobStateName(JobState s);

// Thread-safe job state container.
// Worker thread mutates state/result/progress; any thread may poll or cancel.
class HardwareJob {
public:
    explicit HardwareJob(std::string id, std::string label = "");

    // Immutable after construction.
    const std::string& id()    const { return id_; }
    const std::string& label() const { return label_; }

    // Thread-safe read accessors.
    JobState           state()    const;
    nlohmann::json     progress() const;
    nlohmann::json     result()   const;
    std::string        error()    const;
    bool               isCancelRequested() const;

    // Worker-thread-only write methods (caller must hold no locks).
    void setState(JobState s);
    void setProgress(nlohmann::json p);
    void setResult(nlohmann::json r);
    void setError(std::string e);

    // Any-thread: request cancellation.
    void requestCancel();

    // Snapshot for MCP job_poll response (thread-safe).
    struct Snapshot {
        std::string    id;
        JobState       state;
        nlohmann::json progress;
        nlohmann::json result;
        std::string    error;
    };
    Snapshot snapshot() const;

private:
    const std::string id_;
    const std::string label_;

    mutable std::mutex      mu_;
    JobState                state_{JobState::kQueued};
    nlohmann::json          progress_{nullptr};
    nlohmann::json          result_{nullptr};
    std::string             error_;
    std::atomic<bool>       cancel_requested_{false};
};

// Build a JSON object from a job snapshot for job_poll responses.
nlohmann::json snapshotToJson(const HardwareJob::Snapshot& snap);

}  // namespace jtag::hardware
