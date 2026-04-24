// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "src/ila/ila_driver.h"
#include "src/ila/ila_tap_backend.h"

namespace jtag::ila::test {

/// In-memory model of the ILA TAP for unit testing `IlaDriver`.
///
/// Implements the same IR/DR semantics as `hdl/ila/rtl/ila_tap.sv`:
///   - CAPTURE_DR for each instruction loads the current value.
///   - UPDATE_DR latches the TDI-shifted value back into the register
///     (or initiates the action for CTRL).
///   - READ_DATA auto-increments READ_ADDR on each UPDATE_DR.
class MockIlaTap : public IlaTapBackend {
public:
    static constexpr uint32_t kIdcode = IlaDriver::kDefaultIdcode;
    static constexpr int      kDepth  = IlaDriver::kDepth;

    MockIlaTap() { bram_.fill(0); }

    // IlaTapBackend
    bool selectIr(uint32_t opcode) override {
        current_ir_ = opcode & 0x1F;
        ir_select_count_++;
        return true;
    }

    bool shiftDr(int dr_bits, const uint8_t* tdi,
                  std::vector<uint8_t>& tdo) override {
        uint64_t tdi_val = 0;
        for (int i = 0; i < dr_bits && i < 64; i++) {
            if ((tdi[i / 8] >> (i % 8)) & 1u) tdi_val |= (1ULL << i);
        }
        uint64_t tdo_val = captureDr(dr_bits);
        updateDr(dr_bits, tdi_val);

        tdo.assign((dr_bits + 7) / 8, 0);
        for (int i = 0; i < dr_bits && i < 64; i++) {
            if ((tdo_val >> i) & 1ULL)
                tdo[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        }
        return true;
    }

    const std::string& lastError() const override { return last_error_; }

    // Test helpers -----------------------------------------------------------
    void setBramWord(int idx, uint32_t value)  { bram_.at(idx) = value; }
    uint32_t bramWord(int idx) const            { return bram_.at(idx); }
    uint32_t trigMask()  const { return trig_mask_; }
    uint32_t trigValue() const { return trig_value_; }
    uint16_t preSamples() const { return pre_samples_; }
    uint16_t readAddr()  const { return read_addr_; }
    bool     armRequested()   const { return arm_; }
    bool     stopRequested()  const { return stop_; }
    bool     resetRequested() const { return reset_; }
    bool     forceRequested() const { return force_; }
    int      irSelectCount()  const { return ir_select_count_; }

    /// Set STATUS bits the next CAPTURE_DR on IR_STATUS will report.
    void setStatus(bool armed, bool triggered, bool full) {
        status_ = (armed ? 0x1 : 0) | (triggered ? 0x2 : 0) | (full ? 0x4 : 0);
    }

    /// Override the CONFIG register value returned on IR_CONFIG reads.
    /// Default = 0x0110_7C0A (version=1, num_ch=1, data_w=32, addr_w=10).
    void setConfigValue(uint32_t v) { config_val_ = v; }
    uint32_t configValue() const { return config_val_; }

private:
    uint64_t captureDr(int dr_bits) {
        switch (current_ir_) {
            case IlaDriver::kIrIdcode:     return kIdcode;
            case IlaDriver::kIrConfig:     return config_val_;
            case IlaDriver::kIrStatus:     return status_;
            case IlaDriver::kIrTrigMask:   return trig_mask_;
            case IlaDriver::kIrTrigVal:    return trig_value_;
            case IlaDriver::kIrReadAddr:   return read_addr_;
            case IlaDriver::kIrReadData:
                return bram_.at(read_addr_ % kDepth);
            case IlaDriver::kIrPreSamples: return pre_samples_;
            case IlaDriver::kIrCtrl:       return 0;
            case IlaDriver::kIrBypass:     return 0;
            default:                        return 0;
        }
    }

    void updateDr(int dr_bits, uint64_t tdi_val) {
        uint64_t mask = (dr_bits >= 64) ? ~0ULL : ((1ULL << dr_bits) - 1);
        uint64_t v = tdi_val & mask;
        switch (current_ir_) {
            case IlaDriver::kIrTrigMask:   trig_mask_   = static_cast<uint32_t>(v); break;
            case IlaDriver::kIrTrigVal:    trig_value_  = static_cast<uint32_t>(v); break;
            case IlaDriver::kIrPreSamples: pre_samples_ = static_cast<uint16_t>(v); break;
            case IlaDriver::kIrReadAddr:   read_addr_   = static_cast<uint16_t>(v); break;
            case IlaDriver::kIrReadData:
                // Hardware auto-increments after UPDATE_DR.
                read_addr_ = static_cast<uint16_t>((read_addr_ + 1) % kDepth);
                break;
            case IlaDriver::kIrCtrl:
                arm_    = (v & 0x1) != 0;
                stop_   = (v & 0x2) != 0;
                reset_  = (v & 0x4) != 0;
                force_  = (v & 0x8) != 0;
                break;
            default: break;
        }
    }

    uint32_t current_ir_    = 0x1F;   // reset default = BYPASS
    uint32_t trig_mask_     = 0;
    uint32_t trig_value_    = 0;
    uint16_t pre_samples_   = 0;
    uint16_t read_addr_     = 0;
    uint32_t status_        = 0;
    uint32_t config_val_    = 0x0110'7C0Au;  // version=1, num_ch=1, data_w=32, addr_w=10
    bool     arm_   = false;
    bool     stop_  = false;
    bool     reset_ = false;
    bool     force_ = false;
    int      ir_select_count_ = 0;
    std::array<uint32_t, kDepth> bram_;
    std::string last_error_;
};

} // namespace jtag::ila::test
