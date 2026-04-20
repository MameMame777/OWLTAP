#include "jtag_chain.h"

#include <cstring>

#include "src/bsdl/bsdl_parser.h"

namespace jtag {

JtagChain::JtagChain(TapController& tap) : tap_(tap) {}

int JtagChain::detectDevices() {
    devices_.clear();

    // Reset TAP to ensure known state (5x TMS=1 from any state)
    if (!tap_.reset()) {
        last_error_ = "TAP reset failed: " + tap_.lastError();
        return 0;
    }

    // Clock at least 10 RTI cycles. Zynq-7020 ARM DAP requires idle time
    // after reset before it drives TDO correctly.
    if (!tap_.clkIdle(10)) {
        last_error_ = "RTI idle failed: " + tap_.lastError();
        return 0;
    }

    // After reset+idle, shift out zeros and capture IDCODEs from TDO.
    // Zynq JTAG chain: ARM DAP (IDCODE=0x4BA00477) + PL TAP (IDCODE=0x13722093)
    // We read 32*8 = 256 bits to cover up to 8 devices.
    const int MAX_DEVICES = 8;
    const int max_bits = MAX_DEVICES * 32;
    std::vector<uint8_t> tdo_data;

    if (!tap_.readDR(tdo_data, max_bits)) {
        last_error_ = "DR read failed: " + tap_.lastError();
        return 0;
    }

    // Diagnostic: record raw TDO bytes in error field (cleared if devices found)
    {
        char buf[256] = "Raw TDO: ";
        size_t off = strlen(buf);
        for (size_t i = 0; i < tdo_data.size() && i < 8; i++) {
            snprintf(buf + off, sizeof(buf) - off, "%02X ", tdo_data[i]);
            off += 3;
        }
        last_error_ = buf;
    }

    // Parse IDCODEs from the bit stream
    // IDCODE format: bit 0 = 1 (mandatory), bits 1-11 = manufacturer,
    // bits 12-27 = part number, bits 28-31 = version
    // BYPASS: single '0' bit

    int bit_pos = 0;
    int device_pos = 0;

    while (bit_pos < max_bits) {
        // Check bit 0 of potential IDCODE
        int byte_idx = bit_pos / 8;
        int bit_idx = bit_pos % 8;

        if (byte_idx >= static_cast<int>(tdo_data.size())) break;

        bool first_bit = (tdo_data[byte_idx] >> bit_idx) & 1;

        if (!first_bit) {
            // This is a BYPASS device (single 0 bit)
            // Check if we've reached the end (all remaining bits are 0)
            bool all_zero = true;
            for (int i = bit_pos; i < max_bits && i < bit_pos + 32; i++) {
                if ((tdo_data[i / 8] >> (i % 8)) & 1) {
                    all_zero = false;
                    break;
                }
            }
            if (all_zero) break;  // End of chain

            ChainDevice dev;
            dev.position = device_pos++;
            dev.idcode = 0;
            dev.ir_length = 1;  // Assume 1-bit IR for unknown devices
            devices_.push_back(std::move(dev));
            bit_pos += 1;
        } else {
            // Read 32-bit IDCODE
            uint32_t idcode = 0;
            for (int i = 0; i < 32 && (bit_pos + i) < max_bits; i++) {
                int bi = bit_pos + i;
                if ((tdo_data[bi / 8] >> (bi % 8)) & 1) {
                    idcode |= (1u << i);
                }
            }

            // Check for all-ones (end of chain marker)
            if (idcode == 0xFFFFFFFF) break;

            ChainDevice dev;
            dev.position = device_pos++;
            dev.idcode = idcode;
            // Determine IR length from manufacturer field (bits 1-11)
            // ARM Ltd (0x23B) = 4-bit IR for CoreSight/DAP; Xilinx default = 6
            uint32_t mfr = (idcode >> 1) & 0x7FF;
            dev.ir_length = (mfr == 0x23B) ? 4 : 6;
            devices_.push_back(std::move(dev));
            bit_pos += 32;
        }
    }

    if (!devices_.empty()) last_error_.clear();
    return static_cast<int>(devices_.size());
}

bool JtagChain::loadBsdl(int device_index, const std::string& bsdl_path) {
    if (device_index < 0 || device_index >= static_cast<int>(devices_.size())) {
        last_error_ = "Invalid device index";
        return false;
    }

    bsdl::BSDLParser parser;
    auto bsdl_dev = parser.parseFile(bsdl_path);
    if (!bsdl_dev) {
        last_error_ = "Failed to parse BSDL: " + parser.lastError();
        return false;
    }

    devices_[device_index].ir_length = bsdl_dev->instruction_length;
    devices_[device_index].bsdl_file = bsdl_path;
    devices_[device_index].bsdl = std::move(bsdl_dev);
    return true;
}

int JtagChain::totalIRLength() const {
    int total = 0;
    for (const auto& dev : devices_) {
        total += dev.ir_length;
    }
    return total;
}

bool JtagChain::selectInstruction(int device_index, uint32_t instruction) {
    if (device_index < 0 || device_index >= static_cast<int>(devices_.size())) {
        last_error_ = "Invalid device index";
        return false;
    }

    int total_ir = totalIRLength();
    std::vector<uint8_t> ir_data((total_ir + 7) / 8, 0);

    // Build IR chain: TDO-closest device first in the bit stream.
    //
    // JTAG IR chain:  TDI → [ARM DAP IR] → [PL TAP IR] → TDO
    // MPSSE shifts LSB first (bit 0 → TDI first).
    //
    // Bit 0 enters TDI, travels through ARM DAP, and ends up in PL TAP.
    // The LAST bits shifted stay in ARM DAP (TDI-closest).
    //
    // Therefore: device 0 (TDO-side, PL TAP) occupies LOW bit positions,
    //            device N-1 (TDI-side, ARM DAP) occupies HIGH bit positions.
    int bit_pos = 0;
    for (int i = 0; i < static_cast<int>(devices_.size()); i++) {
        int ir_len = devices_[i].ir_length;
        uint32_t opcode;

        if (i == device_index) {
            opcode = instruction;
        } else {
            // BYPASS = all 1s (use ULL to avoid UB when ir_len >= 32)
            opcode = static_cast<uint32_t>((1ULL << ir_len) - 1);
        }

        // Set bits in ir_data
        for (int b = 0; b < ir_len; b++) {
            if ((opcode >> b) & 1) {
                ir_data[(bit_pos + b) / 8] |= (1 << ((bit_pos + b) % 8));
            }
        }
        bit_pos += ir_len;
    }

    return tap_.shiftIR(ir_data.data(), total_ir);
}

bool JtagChain::readBSR(int device_index, std::vector<uint8_t>& bsr_data) {
    if (device_index < 0 || device_index >= static_cast<int>(devices_.size())) {
        last_error_ = "Invalid device index";
        return false;
    }

    const auto& dev = devices_[device_index];
    if (!dev.bsdl) {
        last_error_ = "BSDL not loaded for device";
        return false;
    }

    // Calculate total DR length: target device's BSR + 1-bit BYPASS for each other device
    int bsr_len = dev.bsdl->boundary_length;
    int total_dr = bsr_len;
    for (int i = 0; i < static_cast<int>(devices_.size()); i++) {
        if (i != device_index) {
            total_dr += 1;  // BYPASS register = 1 bit
        }
    }

    std::vector<uint8_t> full_dr;
    if (!tap_.readDR(full_dr, total_dr)) {
        last_error_ = tap_.lastError();
        return false;
    }

    // Extract target device's BSR from the chain
    // Devices closer to TDO (lower index) come out first
    int skip_before = 0;
    for (int i = 0; i < device_index; i++) {
        skip_before += 1;  // BYPASS bits for devices before target
    }

    bsr_data.resize((bsr_len + 7) / 8, 0);
    for (int b = 0; b < bsr_len; b++) {
        int src_bit = skip_before + b;
        if ((full_dr[src_bit / 8] >> (src_bit % 8)) & 1) {
            bsr_data[b / 8] |= (1 << (b % 8));
        }
    }

    return true;
}

bool JtagChain::writeBSR(int device_index, const uint8_t* bsr_data,
                           int bsr_bits) {
    if (device_index < 0 || device_index >= static_cast<int>(devices_.size())) {
        last_error_ = "Invalid device index";
        return false;
    }

    // Build full DR chain with BYPASS bits for other devices
    int total_dr = bsr_bits;
    for (int i = 0; i < static_cast<int>(devices_.size()); i++) {
        if (i != device_index) {
            total_dr += 1;
        }
    }

    std::vector<uint8_t> full_dr((total_dr + 7) / 8, 0);

    int bit_pos = 0;
    for (int i = 0; i < static_cast<int>(devices_.size()); i++) {
        if (i == device_index) {
            // Copy BSR data
            for (int b = 0; b < bsr_bits; b++) {
                if ((bsr_data[b / 8] >> (b % 8)) & 1) {
                    full_dr[(bit_pos + b) / 8] |= (1 << ((bit_pos + b) % 8));
                }
            }
            bit_pos += bsr_bits;
        } else {
            // BYPASS: 1 bit of 0
            bit_pos += 1;
        }
    }

    return tap_.shiftDR(full_dr.data(), total_dr);
}

} // namespace jtag
