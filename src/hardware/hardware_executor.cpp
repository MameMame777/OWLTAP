#include "hardware_executor.h"

#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace jtag::hardware {

// ---------------------------------------------------------------------------
// UUID v4 generation
// ---------------------------------------------------------------------------
std::string HardwareExecutor::generateJobId() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist32;
    std::uniform_int_distribution<uint16_t> dist16;

    auto r32 = [&] { return dist32(rng); };

    // UUID layout: 8-4-4-4-12 hex digits.
    // v4: variant bits 10xx in octet 8; version 0100 in octet 6.
    uint32_t a  = r32();
    uint32_t b  = r32();
    uint32_t c  = r32();
    uint32_t d  = r32();

    // Set version = 4 (bits [7:4] of octet 6 = 0100).
    b = (b & 0xFFFF0FFF) | 0x00004000;
    // Set variant = 10xx (bits [7:6] of octet 8).
    c = (c & 0x3FFFFFFF) | 0x80000000;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(8) << a       << '-';
    oss << std::setw(4) << (b >> 16) << '-';
    oss << std::setw(4) << (b & 0xFFFF) << '-';
    oss << std::setw(4) << (c >> 16) << '-';
    oss << std::setw(4) << (c & 0xFFFF);
    oss << std::setw(8) << d;
    return oss.str();
}

// ---------------------------------------------------------------------------
// HardwareExecutor
// ---------------------------------------------------------------------------

HardwareExecutor::HardwareExecutor(gui::AppConfig cfg)
    : context_(std::make_unique<HardwareContext>(std::move(cfg))) {}

HardwareExecutor::~HardwareExecutor() {
    if (running_.load()) shutdown();
}

std::string HardwareExecutor::start() {
    if (running_.load()) return {};

    std::string err = context_->open();
    if (!err.empty()) return err;

    stop_flag_.store(false);
    worker_ = std::thread(&HardwareExecutor::workerLoop, this);
    running_.store(true);
    return {};
}

void HardwareExecutor::startWorker() {
    if (running_.load()) return;

    // Do NOT open hardware here — deferred until first task execution.
    stop_flag_.store(false);
    worker_ = std::thread(&HardwareExecutor::workerLoop, this);
    running_.store(true);
}

bool HardwareExecutor::isRunning() const {
    return running_.load(std::memory_order_acquire);
}

JobHandle HardwareExecutor::submit(HardwareTask task, std::string label) {
    if (!running_.load()) {
        throw std::runtime_error("HardwareExecutor is not running");
    }

    std::string id = generateJobId();
    auto handle = std::make_shared<HardwareJob>(id, std::move(label));

    {
        std::lock_guard<std::mutex> lk(registry_mu_);
        job_registry_[id] = handle;
    }

    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        queue_.emplace_back(handle, std::move(task));
    }
    queue_cv_.notify_one();
    return handle;
}

void HardwareExecutor::tryCancel(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(registry_mu_);
    auto it = job_registry_.find(job_id);
    if (it != job_registry_.end()) {
        auto sp = it->second.lock();
        if (sp) sp->requestCancel();
    }
}

std::optional<HardwareJob::Snapshot>
HardwareExecutor::poll(const std::string& job_id) const {
    std::lock_guard<std::mutex> lk(registry_mu_);
    auto it = job_registry_.find(job_id);
    if (it == job_registry_.end()) return std::nullopt;
    auto sp = it->second.lock();
    if (!sp) return std::nullopt;
    return sp->snapshot();
}

void HardwareExecutor::shutdown() {
    if (!running_.load()) return;

    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        stop_flag_.store(true);
    }
    queue_cv_.notify_all();

    if (worker_.joinable()) worker_.join();
    running_.store(false);
    context_->close();
}

// ---------------------------------------------------------------------------
// Worker loop
// ---------------------------------------------------------------------------

void HardwareExecutor::workerLoop() {
    while (true) {
        std::pair<JobHandle, HardwareTask> entry;

        {
            std::unique_lock<std::mutex> lk(queue_mu_);
            queue_cv_.wait(lk, [this] {
                return !queue_.empty() || stop_flag_.load();
            });

            if (stop_flag_.load() && queue_.empty()) break;

            entry = std::move(queue_.front());
            queue_.pop_front();
        }

        auto& [handle, task] = entry;

        // Skip jobs cancelled before they started.
        if (handle->isCancelRequested()) {
            handle->setState(JobState::kCancelled);
            continue;
        }

        handle->setState(JobState::kRunning);
        try {
            // Auto-open hardware if not already open (lazy-start path).
            if (!context_->isOpen()) {
                std::string open_err = context_->open();
                if (!open_err.empty()) {
                    throw std::runtime_error("hardware open failed: " + open_err);
                }
            }
            nlohmann::json result = task(*context_, *handle);
            if (handle->isCancelRequested()) {
                handle->setState(JobState::kCancelled);
            } else {
                handle->setResult(std::move(result));
                handle->setState(JobState::kComplete);
            }
        } catch (const std::exception& e) {
            handle->setError(e.what());
            handle->setState(JobState::kFailed);
        } catch (...) {
            handle->setError("unknown exception in hardware task");
            handle->setState(JobState::kFailed);
        }
    }
}

}  // namespace jtag::hardware
