#include "scanner.h"

#include <set>

namespace jtag {

ScanResult decodeBoundaryScan(const bsdl::BSDLDevice& device,
                              std::vector<uint8_t> raw_bsr) {
    ScanResult result;
    result.raw_bsr = std::move(raw_bsr);
    if (result.raw_bsr.empty()) {
        return result;
    }

    // First pass: INPUT/BIDIR/CLOCK cells — these capture the actual pad state.
    for (const auto& cell : device.boundary_cells) {
        if (!cell.hasPin()) continue;
        if (!cell.isInput()) continue;
        const bool bit_val = result.getBit(cell.position);
        result.pin_states[cell.pin_name] = bit_val ? PinState::HIGH : PinState::LOW;
    }

    // Second pass: OUTPUT2/OUTPUT3 cells for pins that have no INPUT cell.
    // (e.g. dedicated config pins that are output-only in the BSR)
    for (const auto& cell : device.boundary_cells) {
        if (!cell.hasPin()) continue;
        if (!cell.isOutput()) continue;
        if (result.pin_states.count(cell.pin_name) != 0) continue;
        const bool bit_val = result.getBit(cell.position);
        result.pin_states[cell.pin_name] = bit_val ? PinState::HIGH : PinState::LOW;
    }

    return result;
}

Scanner::Scanner(JtagChain& chain, int device_index)
    : chain_(chain), device_index_(device_index) {}

bool Scanner::isReady() const {
    if (device_index_ < 0 ||
        device_index_ >= chain_.deviceCount()) {
        return false;
    }
    return chain_.devices()[device_index_].bsdl != nullptr;
}

const bsdl::BSDLDevice* Scanner::bsdlDevice() const {
    if (!isReady()) return nullptr;
    return chain_.devices()[device_index_].bsdl.get();
}

bool Scanner::readIdCode(uint32_t& idcode) {
    const auto* dev = bsdlDevice();
    if (!dev) {
        last_error_ = "BSDL not loaded";
        return false;
    }

    auto idcode_opcode = dev->idcodeOpcode();
    if (!idcode_opcode) {
        // Try default IDCODE (after reset, DR contains IDCODE)
        idcode_opcode = 0x09;  // Common Xilinx IDCODE
    }

    if (!chain_.selectInstruction(device_index_,
                                   static_cast<uint32_t>(*idcode_opcode))) {
        last_error_ = chain_.lastError();
        return false;
    }

    // IDCODE DR is always 32 bits (not BSR length).
    // Calculate total DR: 32 bits for target + 1-bit BYPASS per other device.
    int total_dr = 32;
    for (int i = 0; i < chain_.deviceCount(); i++) {
        if (i != device_index_) total_dr += 1;
    }

    std::vector<uint8_t> dr_data;
    if (!chain_.tap().readDR(dr_data, total_dr)) {
        last_error_ = "Failed to read IDCODE DR";
        return false;
    }

    // Extract 32-bit IDCODE, skipping BYPASS bits for devices before target
    int skip_bits = 0;
    for (int i = 0; i < device_index_; i++) {
        skip_bits += 1;  // BYPASS = 1 bit
    }

    idcode = 0;
    for (int b = 0; b < 32; b++) {
        int src_bit = skip_bits + b;
        if (src_bit / 8 < static_cast<int>(dr_data.size())) {
            if ((dr_data[src_bit / 8] >> (src_bit % 8)) & 1) {
                idcode |= (1u << b);
            }
        }
    }

    return true;
}

bool Scanner::verifyIdCode() {
    const auto* dev = bsdlDevice();
    if (!dev) return false;

    uint32_t actual_idcode;
    if (!readIdCode(actual_idcode)) return false;

    return dev->idcode.matches(actual_idcode);
}

ScanResult Scanner::sample() {
    ScanResult result;

    const auto* dev = bsdlDevice();
    if (!dev) {
        last_error_ = "BSDL not loaded";
        return result;
    }

    auto sample_opcode = dev->sampleOpcode();
    if (!sample_opcode) {
        last_error_ = "SAMPLE instruction not found in BSDL";
        return result;
    }

    // Load SAMPLE instruction
    if (!chain_.selectInstruction(device_index_,
                                   static_cast<uint32_t>(*sample_opcode))) {
        last_error_ = chain_.lastError();
        return result;
    }

    // Some Xilinx/ARM devices need at least 1 RTI cycle after UPDATE-IR
    // before the new DR path is available.
    chain_.tap().clkIdle(2);

    // Read BSR
    if (!chain_.readBSR(device_index_, result.raw_bsr)) {
        last_error_ = chain_.lastError();
        return result;
    }

    auto scan = decodeBoundaryScan(*dev, std::move(result.raw_bsr));

    // Filter pin_states to only requested pins (reduces per-sample memory).
    if (!decode_filter_.empty()) {
        std::map<std::string, PinState> filtered;
        for (const auto& name : decode_filter_) {
            auto it = scan.pin_states.find(name);
            if (it != scan.pin_states.end()) filtered.emplace(*it);
        }
        scan.pin_states = std::move(filtered);
    }

    return scan;
}

void Scanner::setDecodeFilter(const std::vector<std::string>& names) {
    decode_filter_.clear();
    decode_filter_.insert(names.begin(), names.end());
}

std::vector<std::string> Scanner::getObservablePins() const {
    std::vector<std::string> pins;
    const auto* dev = bsdlDevice();
    if (!dev) return pins;

    std::set<std::string> seen;
    // INPUT/BIDIR/CLOCK cells first (prefer these)
    for (const auto& cell : dev->boundary_cells) {
        if (cell.hasPin() && cell.isInput()) {
            if (seen.insert(cell.pin_name).second)
                pins.push_back(cell.pin_name);
        }
    }
    // Then OUTPUT2/OUTPUT3 for pins with no INPUT cell
    for (const auto& cell : dev->boundary_cells) {
        if (cell.hasPin() && cell.isOutput()) {
            if (seen.insert(cell.pin_name).second)
                pins.push_back(cell.pin_name);
        }
    }
    return pins;
}

std::vector<std::string> Scanner::getDrivablePins() const {
    std::vector<std::string> pins;
    const auto* dev = bsdlDevice();
    if (!dev) return pins;

    std::set<std::string> seen;
    for (const auto& cell : dev->boundary_cells) {
        if (cell.hasPin() && cell.isOutput()) {
            if (seen.insert(cell.pin_name).second) {
                pins.push_back(cell.pin_name);
            }
        }
    }
    return pins;
}

} // namespace jtag
