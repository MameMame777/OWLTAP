#include "flash_jtag_bridge.h"

#include "src/config/pl_config.h"

namespace jtag::flash {

namespace {

// Per-byte LSB<->MSB reversal.  Copied locally to avoid pulling PlConfig into
// the public header of this module.  Identical logic to PlConfig::bitReverseByte.
uint8_t reverseByte(uint8_t b) {
    b = static_cast<uint8_t>(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = static_cast<uint8_t>(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = static_cast<uint8_t>(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

}  // namespace

FlashJtagBridge::FlashJtagBridge(JtagChain& chain, int device_index)
    : chain_(chain), device_index_(device_index) {}

bool FlashJtagBridge::transfer(const std::vector<uint8_t>& tx,
                                std::vector<uint8_t>& rx) {
    if (tx.empty()) {
        last_error_ = "transfer: tx is empty";
        return false;
    }
    if (device_index_ < 0 || device_index_ >= chain_.deviceCount()) {
        last_error_ = "transfer: invalid device index";
        return false;
    }

    // Load USER1 once; subsequent transfers can skip IR loading to reduce
    // TCK overhead (the TAP stays in RUN-TEST-IDLE between shifts).
    if (!user1_selected_) {
        if (!chain_.selectInstruction(device_index_, kUser1Ir)) {
            last_error_ = "USER1 load: " + chain_.lastError();
            return false;
        }
        user1_selected_ = true;
    }

    // Build a chain-aware DR payload:
    //   devices TDO-side of target  → 1 BYPASS bit each (target sees these
    //                                  leading bits first shifted out)
    //   target device               → the actual SPI bytes (bit-reversed)
    //   devices TDI-side of target  → 1 BYPASS bit each
    //
    // JtagChain orders device index 0 as TDO-closest.  Bit 0 of the shifted
    // vector enters TDI first and ends up at the TDO-closest device.  So
    // devices with index < target occupy the LOW bits.
    const int data_bits = static_cast<int>(tx.size()) * 8;
    int bypass_before = 0;  // devices closer to TDO than target
    int bypass_after = 0;   // devices closer to TDI than target
    for (int i = 0; i < chain_.deviceCount(); i++) {
        if (i < device_index_) bypass_before++;
        else if (i > device_index_) bypass_after++;
    }
    const int total_bits = bypass_before + data_bits + bypass_after;
    std::vector<uint8_t> tdi((total_bits + 7) / 8, 0);

    // Bit-reverse tx and pack it starting at bit offset `bypass_before`.
    // Within each reversed byte, bit 0 (LSB) is shifted first; that bit must
    // correspond to SPI wire bit 7 (MSB of the original byte).
    for (size_t i = 0; i < tx.size(); i++) {
        const uint8_t rb = reverseByte(tx[i]);
        const int base = bypass_before + static_cast<int>(i) * 8;
        for (int b = 0; b < 8; b++) {
            if ((rb >> b) & 1) {
                tdi[(base + b) / 8] |= static_cast<uint8_t>(1 << ((base + b) % 8));
            }
        }
    }

    std::vector<uint8_t> tdo;
    if (!chain_.tap().shiftDR(tdi.data(), tdo, total_bits)) {
        last_error_ = "shiftDR: " + chain_.tap().lastError();
        return false;
    }

    // Extract target-device bits from tdo starting at bit offset bypass_before,
    // bit-reverse every 8-bit group back to MSB-first on return.
    rx.assign(tx.size(), 0);
    for (size_t i = 0; i < tx.size(); i++) {
        uint8_t packed = 0;
        const int base = bypass_before + static_cast<int>(i) * 8;
        for (int b = 0; b < 8; b++) {
            if ((tdo[(base + b) / 8] >> ((base + b) % 8)) & 1) {
                packed |= static_cast<uint8_t>(1 << b);
            }
        }
        rx[i] = reverseByte(packed);
    }
    return true;
}

}  // namespace jtag::flash
