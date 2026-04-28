// SPDX-License-Identifier: Apache-2.0
#include "src/ila/bscane_ila_tap_backend.h"

#include <algorithm>

#include "src/jtag/jtag_chain.h"
#include "src/jtag/tap_controller.h"

namespace jtag::ila {

BscaneIlaTapBackend::BscaneIlaTapBackend(JtagChain& chain, int pl_tap_index,
                                          int user_chain)
    : chain_(chain)
    , pl_tap_index_(pl_tap_index)
    , user_chain_(user_chain < 1 || user_chain > 4 ? 1 : user_chain) {}

bool BscaneIlaTapBackend::selectIr(uint32_t opcode) {
    pending_ir_ = opcode & 0x1Fu;
    // Activate the selected BSCANE2 USERn scan chain on the PL TAP.
    const uint32_t user_ir = kUserOpcodes[user_chain_];
    if (!chain_.selectInstruction(pl_tap_index_, user_ir)) {
        last_error_ = "USER" + std::to_string(user_chain_) +
                      " selectInstruction failed: " + chain_.lastError();
        return false;
    }
    return true;
}

bool BscaneIlaTapBackend::doRawFrameShift(const uint8_t* data32,
                                           std::vector<uint8_t>& tdo) {
    // Build 37-bit frame: [4:0] = pending_ir_, [36:5] = data32 (32 bits)
    std::vector<uint8_t> frame_tdi((kFrameBits + 7) / 8, 0x00u);
    frame_tdi[0] = static_cast<uint8_t>(pending_ir_ & 0x1Fu);
    for (int b = 0; b < 32; ++b) {
        if ((data32[b / 8] >> (b % 8)) & 1u) {
            const int fbit = b + 5;
            frame_tdi[fbit / 8] |= static_cast<uint8_t>(1u << (fbit % 8));
        }
    }

    const auto& devs = chain_.devices();
    const int n = static_cast<int>(devs.size());
    const int total_dr = kFrameBits + (n - 1);

    std::vector<uint8_t> full_tdi((total_dr + 7) / 8, 0x00u);
    int bit_pos = 0;
    for (int i = 0; i < n; ++i) {
        if (i == pl_tap_index_) {
            for (int b = 0; b < kFrameBits; ++b) {
                if ((frame_tdi[b / 8] >> (b % 8)) & 1u) {
                    const int p = bit_pos + b;
                    full_tdi[p / 8] |= static_cast<uint8_t>(1u << (p % 8));
                }
            }
            bit_pos += kFrameBits;
        } else {
            bit_pos += 1;  // BYPASS
        }
    }

    std::vector<uint8_t> full_tdo;
    if (!chain_.tap().shiftDR(full_tdi.data(), full_tdo, total_dr)) {
        last_error_ = "shiftDR failed: " + chain_.tap().lastError();
        return false;
    }

    // Extract data field [36:5] from TDO, skipping BYPASS bits before pl_tap
    int skip = 0;
    for (int i = 0; i < pl_tap_index_; ++i) skip += 1;
    const int data_start = skip + 5;

    tdo.assign(5, 0x00u);  // 4 bytes data + 1 spare
    for (int b = 0; b < 32; ++b) {
        const int s = data_start + b;
        if ((full_tdo[s / 8] >> (s % 8)) & 1u)
            tdo[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
    }
    return true;
}

bool BscaneIlaTapBackend::shiftDr(int dr_bits, const uint8_t* tdi,
                                    std::vector<uint8_t>& tdo) {
    // BSCANE2 protocol: CAPTURE-DR preloads the register selected by
    // stored_ir (which was set during the *previous* UPDATE-DR).
    // If stored_ir_in_hw_ doesn't match pending_ir_ yet, we need a
    // "prime" scan first so the subsequent real scan captures correct data.
    if (stored_ir_in_hw_ != pending_ir_) {
        static const uint8_t zeros[4] = {};
        std::vector<uint8_t> dummy;
        if (!doRawFrameShift(zeros, dummy)) return false;
        stored_ir_in_hw_ = pending_ir_;
    }

    // Real scan: CAPTURE now preloads the register for pending_ir_
    const int data_bits = std::min(dr_bits, 32);
    std::vector<uint8_t> raw_tdo;
    // Build data payload padded to 4 bytes
    uint8_t data32[4] = {};
    for (int b = 0; b < data_bits; ++b) {
        if ((tdi[b / 8] >> (b % 8)) & 1u)
            data32[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
    }
    if (!doRawFrameShift(data32, raw_tdo)) return false;
    stored_ir_in_hw_ = pending_ir_;

    tdo.assign((dr_bits + 7) / 8, 0x00u);
    for (int b = 0; b < data_bits; ++b) {
        if ((raw_tdo[b / 8] >> (b % 8)) & 1u)
            tdo[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
    }
    return true;
}

} // namespace jtag::ila
