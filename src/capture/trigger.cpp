#include "trigger.h"

namespace jtag {

void TriggerEngine::setMode(TriggerMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = mode;
}

TriggerMode TriggerEngine::mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_;
}

void TriggerEngine::setConditions(
    const std::vector<TriggerCondition>& conditions) {
    std::lock_guard<std::mutex> lock(mutex_);
    conditions_ = conditions;
}

std::vector<TriggerCondition> TriggerEngine::conditions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return conditions_;
}

void TriggerEngine::clearConditions() {
    std::lock_guard<std::mutex> lock(mutex_);
    conditions_.clear();
    triggered_ = false;
    armed_ = false;
}

void TriggerEngine::arm() {
    std::lock_guard<std::mutex> lock(mutex_);
    triggered_ = false;
    armed_ = true;
}

bool TriggerEngine::hasTriggered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return triggered_;
}

void TriggerEngine::setPreTriggerRatio(float ratio) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    pre_trigger_ratio_ = ratio;
}

float TriggerEngine::preTriggerRatio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pre_trigger_ratio_;
}

bool TriggerEngine::evaluate(const ScanResult& prev, const ScanResult& curr) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (mode_ == TriggerMode::FREE_RUN) return true;

    if (!armed_) return false;
    if (triggered_ && mode_ == TriggerMode::SINGLE) return false;

    if (conditions_.empty()) return true;

    // All conditions must be met simultaneously (AND logic)
    for (const auto& cond : conditions_) {
        bool met = false;
        switch (cond.type) {
            case TriggerType::EDGE:
                met = evaluateEdge(cond, prev, curr);
                break;
            case TriggerType::LEVEL:
                met = evaluateLevel(cond, curr);
                break;
        }
        if (!met) return false;
    }

    triggered_ = true;
    if (mode_ == TriggerMode::NORMAL) {
        // Re-arm for next trigger
        // (caller will reset armed_ after processing)
    }
    return true;
}

bool TriggerEngine::evaluateEdge(const TriggerCondition& cond,
                                  const ScanResult& prev,
                                  const ScanResult& curr) {
    PinState prev_state = prev.getPin(cond.pin_name);
    PinState curr_state = curr.getPin(cond.pin_name);

    if (prev_state == PinState::UNKNOWN || curr_state == PinState::UNKNOWN) {
        return false;
    }

    bool rising = (prev_state == PinState::LOW && curr_state == PinState::HIGH);
    bool falling = (prev_state == PinState::HIGH && curr_state == PinState::LOW);

    switch (cond.edge) {
        case TriggerEdge::RISING:  return rising;
        case TriggerEdge::FALLING: return falling;
        case TriggerEdge::EITHER:  return rising || falling;
    }
    return false;
}

bool TriggerEngine::evaluateLevel(const TriggerCondition& cond,
                                   const ScanResult& curr) {
    PinState state = curr.getPin(cond.pin_name);
    return state == cond.level;
}

} // namespace jtag
