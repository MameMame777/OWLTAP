#include "pin_driver.h"

namespace jtag {

PinDriver::PinDriver(JtagChain& chain, int device_index)
    : chain_(chain), device_index_(device_index) {
    // Defer BSR init until BSDL is loaded; isReady() checks that.
    if (isReady()) {
        initBsrFromSafe();
    }
}

bool PinDriver::isReady() const {
    if (device_index_ < 0 || device_index_ >= chain_.deviceCount())
        return false;
    return chain_.devices()[device_index_].bsdl != nullptr;
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

bool PinDriver::setPin(const std::string& pin_name, int value) {
    if (!isReady()) {
        last_error_ = "Driver not ready";
        return false;
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
    if (control_cell) {
        setBit(control_cell->position, output_cell->disable_value != 0);
    }

    return true;
}

bool PinDriver::applyOutputs() {
    if (!isReady()) {
        last_error_ = "Driver not ready";
        return false;
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
