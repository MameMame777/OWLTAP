#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "src/boundary_scan/scanner.h"

namespace jtag {

/// Edge type for trigger conditions
enum class TriggerEdge {
    RISING,   // Low -> High transition
    FALLING,  // High -> Low transition
    EITHER,   // Any transition
};

/// Trigger condition type
enum class TriggerType {
    EDGE,   // Trigger on edge transition
    LEVEL,  // Trigger on static level
};

/// Operating mode for trigger
enum class TriggerMode {
    FREE_RUN,  // No trigger; continuous capture
    SINGLE,    // Immediate snapshot: one frame, stop (ignores trigger conditions)
    NORMAL,    // Wait for trigger condition, fill buffer, re-arm
};

/// A single trigger condition
struct TriggerCondition {
    std::string pin_name;
    TriggerType type = TriggerType::EDGE;
    TriggerEdge edge = TriggerEdge::RISING;
    PinState level = PinState::HIGH;  // For level triggers
};

/// Trigger engine: evaluates trigger conditions against sample data.
/// Thread-safe: configuration methods acquire a mutex.
class TriggerEngine {
public:
    TriggerEngine() = default;

    /// Set trigger mode.
    void setMode(TriggerMode mode);
    TriggerMode mode() const;

    /// Set trigger conditions (all must be met simultaneously).
    void setConditions(const std::vector<TriggerCondition>& conditions);
    std::vector<TriggerCondition> conditions() const;

    /// Clear all trigger conditions.
    void clearConditions();

    /// Arm the trigger (reset state for new capture).
    void arm();

    /// Check if trigger has fired.
    bool hasTriggered() const;

    /// Evaluate trigger conditions against two consecutive scan results.
    /// @param prev  Previous sample (for edge detection)
    /// @param curr  Current sample
    /// @return true if trigger fires on this sample
    bool evaluate(const ScanResult& prev, const ScanResult& curr);

    /// Set pre-trigger buffer depth (as fraction of total buffer, 0.0 - 1.0).
    void setPreTriggerRatio(float ratio);
    float preTriggerRatio() const;

private:
    bool evaluateEdge(const TriggerCondition& cond,
                      const ScanResult& prev, const ScanResult& curr);
    bool evaluateLevel(const TriggerCondition& cond,
                       const ScanResult& curr);

    mutable std::mutex mutex_;
    TriggerMode mode_ = TriggerMode::FREE_RUN;
    std::vector<TriggerCondition> conditions_;
    bool triggered_ = false;
    bool armed_ = false;
    float pre_trigger_ratio_ = 0.5f;
};

} // namespace jtag
