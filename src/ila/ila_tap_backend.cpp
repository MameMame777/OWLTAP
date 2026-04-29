// SPDX-License-Identifier: Apache-2.0
#include "src/ila/ila_tap_backend.h"

#include <cstring>

#include "src/jtag/jtag_chain.h"
#include "src/jtag/tap_controller.h"

namespace jtag::ila {

ChainIlaTapBackend::ChainIlaTapBackend(JtagChain& chain, int device_index)
    : chain_(chain), device_index_(device_index) {}

bool ChainIlaTapBackend::selectIr(uint32_t opcode) {
    if (!chain_.selectInstruction(device_index_, opcode)) {
        last_error_ = "selectInstruction failed: " + chain_.lastError();
        return false;
    }
    return true;
}

bool ChainIlaTapBackend::shiftDr(int dr_bits, const uint8_t* tdi,
                                   std::vector<uint8_t>& tdo) {
    const auto& devs = chain_.devices();
    const int n = static_cast<int>(devs.size());
    if (n == 0) { last_error_ = "No devices in chain"; return false; }

    int total_dr = dr_bits + (n - 1);

    // Assemble full-chain TDI. Lower indices occupy low bit positions.
    std::vector<uint8_t> full_tdi((total_dr + 7) / 8, 0);
    int pos = 0;
    for (int i = 0; i < n; i++) {
        if (i == device_index_) {
            for (int b = 0; b < dr_bits; b++) {
                if ((tdi[b / 8] >> (b % 8)) & 1u) {
                    full_tdi[(pos + b) / 8] |=
                        static_cast<uint8_t>(1u << ((pos + b) % 8));
                }
            }
            pos += dr_bits;
        } else {
            pos += 1; // BYPASS
        }
    }

    std::vector<uint8_t> full_tdo;
    if (!chain_.tap().shiftDR(full_tdi.data(), full_tdo, total_dr)) {
        last_error_ = "shiftDR failed: " + chain_.tap().lastError();
        return false;
    }

    int skip_before = 0;
    for (int i = 0; i < device_index_; i++) skip_before += 1;

    tdo.assign((dr_bits + 7) / 8, 0);
    for (int b = 0; b < dr_bits; b++) {
        int s = skip_before + b;
        if ((full_tdo[s / 8] >> (s % 8)) & 1u) {
            tdo[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
        }
    }
    return true;
}

// Default shiftDrBatch: loop over shiftDr.  Backends may override for batching.
bool IlaTapBackend::shiftDrBatch(int count, int dr_bits, const uint8_t* tdi,
                                  std::vector<uint8_t>& flat_tdo) {
    const int bytes_per = (dr_bits + 7) / 8;
    flat_tdo.assign(static_cast<size_t>(count) * bytes_per, 0u);
    for (int i = 0; i < count; ++i) {
        std::vector<uint8_t> tdo_i;
        if (!shiftDr(dr_bits, tdi, tdo_i)) return false;
        std::memcpy(flat_tdo.data() + i * bytes_per, tdo_i.data(), bytes_per);
    }
    return true;
}

} // namespace jtag::ila
