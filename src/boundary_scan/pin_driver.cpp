#include "pin_driver.h"

#include <algorithm>

namespace jtag {

namespace {

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

} // namespace

bool deviceAllowsLiveExtest(const bsdl::BSDLDevice& device,
                            std::string* reason) {
    for (const auto& [pin_name, pin] : device.pins) {
        (void)pin;
        if (startsWith(pin_name, "PS_DDR_") ||
            startsWith(pin_name, "PS_MIO") ||
            pin_name == "PS_SRST_B" ||
            pin_name == "PS_POR_B") {
            if (reason != nullptr) {
                *reason =
                    "EXTEST drive is blocked for this device: its boundary register includes live PS/DDR/MIO pins, so entering EXTEST can freeze or reset the running Zynq processing system.";
            }
            return false;
        }
    }

    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

PinDriver::PinDriver(JtagChain& chain, int device_index)
    : chain_(chain), device_index_(device_index) {
    // Prefer the current live boundary state over BSDL safe defaults so EXTEST
    // starts from what the device is already doing.
    if (isReady() && !captureCurrentState()) {
        initBsrFromSafe();
    }
}

bool PinDriver::isReady() const {
    if (device_index_ < 0 || device_index_ >= chain_.deviceCount())
        return false;
    return chain_.devices()[device_index_].bsdl != nullptr;
}

bool PinDriver::extestAllowed() const {
    if (!isReady()) {
        return false;
    }
    return deviceAllowsLiveExtest(*chain_.devices()[device_index_].bsdl);
}

std::string PinDriver::extestBlockedReason() const {
    if (!isReady()) {
        return "Driver not ready";
    }

    std::string reason;
    deviceAllowsLiveExtest(*chain_.devices()[device_index_].bsdl, &reason);
    return reason;
}

void PinDriver::setBit(int position, bool value) {
    if (position < 0) return;
    int byte_idx = position / 8;
    int bit_idx = position % 8;
    if (byte_idx >= static_cast<int>(bsr_data_.size())) {
        bsr_data_.resize(byte_idx + 1, 0);
    }
    if (value) {
        bsr_data_[byte_idx] |= (1 << bit_idx);
    } else {
        bsr_data_[byte_idx] &= ~(1 << bit_idx);
    }
}

bool PinDriver::getBit(int position) const {
    if (position < 0) return false;
    int byte_idx = position / 8;
    int bit_idx = position % 8;
    if (byte_idx >= static_cast<int>(bsr_data_.size())) return false;
    return (bsr_data_[byte_idx] >> bit_idx) & 1;
}

void PinDriver::initBsrFromSafe() {
    if (!isReady()) return;

    const auto* dev = chain_.devices()[device_index_].bsdl.get();
    bsr_data_.resize((dev->boundary_length + 7) / 8, 0);

    for (const auto& cell : dev->boundary_cells) {
        if (cell.safe_value == 1) {
            setBit(cell.position, true);
        } else {
            setBit(cell.position, false);
        }
    }
}

void PinDriver::loadSnapshot(const std::vector<uint8_t>& raw_bsr) {
    if (!isReady()) {
        return;
    }

    const auto* dev = chain_.devices()[device_index_].bsdl.get();
    const size_t required_bytes = static_cast<size_t>((dev->boundary_length + 7) / 8);
    bsr_data_.assign(required_bytes, 0);

    const size_t bytes_to_copy = std::min(required_bytes, raw_bsr.size());
    std::copy(raw_bsr.begin(), raw_bsr.begin() + bytes_to_copy, bsr_data_.begin());

    const int extra_bits = static_cast<int>(required_bytes * 8) - dev->boundary_length;
    if (extra_bits > 0 && !bsr_data_.empty()) {
        const uint8_t keep_mask = static_cast<uint8_t>(0xFFu >> extra_bits);
        bsr_data_.back() &= keep_mask;
    }
}

bool PinDriver::captureCurrentState() {
    if (!isReady()) {
        last_error_ = "Driver not ready";
        return false;
    }

    const auto* dev = chain_.devices()[device_index_].bsdl.get();
    auto sample_opcode = dev->sampleOpcode();
    if (!sample_opcode) {
        last_error_ = "SAMPLE instruction not found in BSDL";
        return false;
    }

    if (!chain_.selectInstruction(device_index_,
                                  static_cast<uint32_t>(*sample_opcode))) {
        last_error_ = chain_.lastError();
        return false;
    }

    chain_.tap().clkIdle(2);

    std::vector<uint8_t> raw_bsr;
    if (!chain_.readBSR(device_index_, raw_bsr)) {
        last_error_ = chain_.lastError();
        return false;
    }

    loadSnapshot(raw_bsr);
    return true;
}

bool PinDriver::setPin(const std::string& pin_name, int value) {
    if (!isReady()) {
        last_error_ = "Driver not ready";
        return false;
    }

    if (!extestAllowed()) {
        last_error_ = extestBlockedReason();
        return false;
    }

    if (bsr_data_.empty() && !captureCurrentState()) {
        initBsrFromSafe();
    }

    const auto* dev = chain_.devices()[device_index_].bsdl.get();

    // Find the output cell for this pin
    const auto* output_cell = dev->getOutputCellForPin(pin_name);
    if (!output_cell) {
        last_error_ = "Pin '" + pin_name + "' is not drivable (no output cell)";
        return false;
    }

    // Set the output value
    setBit(output_cell->position, value != 0);

    // Enable the output (set control cell to enable value)
    const auto* control_cell = dev->getControlCellFor(*output_cell);
    if (control_cell) {
        // Enable = opposite of disable_value
        bool enable_val = (output_cell->disable_value == 0) ? true : false;
        setBit(control_cell->position, enable_val);
    }

    return true;
}

bool PinDriver::setPinHighZ(const std::string& pin_name) {
    if (!isReady()) {
        last_error_ = "Driver not ready";
        return false;
    }

    if (!extestAllowed()) {
        last_error_ = extestBlockedReason();
        return false;
    }

    if (bsr_data_.empty() && !captureCurrentState()) {
        initBsrFromSafe();
    }

    const auto* dev = chain_.devices()[device_index_].bsdl.get();

    // Find the output cell
    const auto* output_cell = dev->getOutputCellForPin(pin_name);
    if (!output_cell) {
        last_error_ = "Pin '" + pin_name + "' is not drivable";
        return false;
    }

    if (output_cell->function != bsdl::CellFunction::OUTPUT3 &&
        output_cell->function != bsdl::CellFunction::BIDIR) {
        last_error_ = "Pin '" + pin_name + "' does not support tri-state";
        return false;
    }

    // Disable the output (set control cell to disable value)
    const auto* control_cell = dev->getControlCellFor(*output_cell);
    if (!control_cell) {
        last_error_ = "Pin '" + pin_name + "' has no control cell for high-Z";
        return false;
    }

    setBit(control_cell->position, output_cell->disable_value != 0);

    return true;
}

bool PinDriver::applyOutputs() {
    if (!isReady()) {
        last_error_ = "Driver not ready";
        return false;
    }

    if (!extestAllowed()) {
        last_error_ = extestBlockedReason();
        return false;
    }

    if (bsr_data_.empty() && !captureCurrentState()) {
        initBsrFromSafe();
    }

    const auto* dev = chain_.devices()[device_index_].bsdl.get();

    // Load EXTEST instruction
    auto extest_opcode = dev->extestOpcode();
    if (!extest_opcode) {
        last_error_ = "EXTEST instruction not found in BSDL";
        return false;
    }

    if (!chain_.selectInstruction(device_index_,
                                   static_cast<uint32_t>(*extest_opcode))) {
        last_error_ = chain_.lastError();
        return false;
    }

    // Shift out BSR data
    if (!chain_.writeBSR(device_index_, bsr_data_.data(),
                          dev->boundary_length)) {
        last_error_ = chain_.lastError();
        return false;
    }

    return true;
}

void PinDriver::resetToSafe() {
    initBsrFromSafe();
}

int PinDriver::getPinValue(const std::string& pin_name) const {
    if (!isReady()) return -1;

    const auto* dev = chain_.devices()[device_index_].bsdl.get();
    const auto* cell = dev->getOutputCellForPin(pin_name);
    if (!cell) return -1;

    // Check if output is disabled (high-Z)
    const auto* control = dev->getControlCellFor(*cell);
    if (control) {
        bool control_val = getBit(control->position);
        bool is_disabled = (static_cast<int>(control_val) == cell->disable_value);
        if (is_disabled) return -1;  // High-Z
    }

    return getBit(cell->position) ? 1 : 0;
}

} // namespace jtag
