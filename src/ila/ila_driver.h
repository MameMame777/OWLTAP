// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/ila/ila_tap_backend.h"

namespace jtag::ila {

/// One signal lane decoded from the SIG_DEF DR (IR 5'h03).
///
/// SIG_DEF word layout (must match hdl/ila/rtl/ila_tap.sv):
///   [31:28] fmt[3:0]      (0=HEX 1=DEC 2=BIN)
///   [27:24] reserved
///   [23:16] hi[7:0]       (inclusive MSB, 0-based)
///   [15: 8] lo[7:0]       (inclusive LSB, 0-based)
///   [ 7: 0] name_idx[7:0] (0xFF = host auto-generates "data[hi:lo]")
struct IlaSignalEntry {
    uint8_t hi;
    uint8_t lo;
    uint8_t fmt;       ///< 0=HEX 1=DEC 2=BIN
    uint8_t name_idx;  ///< 0xFF means auto-generate
};

/// Status bits reported by the ILA STATUS DR.
struct IlaStatus {
    bool armed;
    bool triggered;
    bool full;
};

/// Capability information reported by the ILA CONFIG DR (IR 5'h02).
///
/// Populated by `IlaDriver::probe()`. Before probe runs, `probed` is false
/// and the fields hold the compile-time defaults (DATA_W=32, DEPTH=1024).
///
/// CONFIG register bit layout (must match hdl/ila/rtl/ila_tap.sv):
///   [31:24] VERSION    (IP version)
///   [23:20] NUM_CH     (number of channels; 1 for current IP)
///   [19:16] SIG_COUNT  (number of SIG_DEF entries; 0 = legacy, no SIG_DEF)
///   [15:10] DATA_W - 1 (6 bits; actual DATA_W = field + 1)
///   [ 9: 8] reserved
///   [ 7: 0] ADDR_W     (DEPTH = 1 << ADDR_W)
struct IlaCaps {
    uint8_t  version   = 0;
    uint8_t  num_ch    = 1;
    uint8_t  sig_count = 0;  ///< number of SIG_DEF entries (0 if legacy)
    uint8_t  data_w    = 32;
    uint8_t  addr_w    = 10;
    uint32_t depth     = 1024;
    uint32_t raw       = 0;
    bool     probed    = false;
};

/// Host-side driver for the OwlTAP Internal Logic Analyzer IP.
///
/// Communicates with the ILA's TAP via an injected `IlaTapBackend`. Use
/// `ChainIlaTapBackend` for real hardware; tests can substitute any mock
/// that implements `IlaTapBackend`.
class IlaDriver {
public:
    static constexpr uint32_t kDefaultIdcode = 0xA17A0001u;
    static constexpr int      kIrLength      = 5;

    // Compile-time defaults. These are retained for backward compatibility;
    // at runtime prefer `dataWidth()`, `addrWidth()`, `depth()` which reflect
    // the values reported by `probe()`.
    static constexpr int      kDataWidth     = 32;
    static constexpr int      kAddrWidth     = 10;
    static constexpr int      kDepth         = 1 << kAddrWidth;

    // IR opcodes (must match hdl/ila/rtl/ila_tap.sv)
    enum Ir : uint32_t {
        kIrIdcode      = 0x01,
        kIrConfig      = 0x02,
        kIrSigDef      = 0x03,
        kIrCtrl        = 0x08,
        kIrStatus      = 0x09,
        kIrTrigMask    = 0x0A,
        kIrTrigVal     = 0x0B,
        kIrReadAddr    = 0x0C,
        kIrReadData    = 0x0D,
        kIrPreSamples  = 0x0E,
        kIrTrigRise    = 0x0F,  ///< TRIG_RISE_MASK (version >= 2)
        kIrTrigFall    = 0x10,  ///< TRIG_FALL_MASK (version >= 2)
        kIrTrigMask2   = 0x11,  ///< TRIG_MASK2 — Group B level mask (version >= 3)
        kIrTrigVal2    = 0x12,  ///< TRIG_VAL2  — Group B level value (version >= 3)
        kIrTrigCtrl    = 0x13,  ///< TRIG_CTRL  — bit[0]=or_mode (version >= 3)
        kIrBypass      = 0x1F,
    };

    explicit IlaDriver(IlaTapBackend& backend);

    /// Read the CONFIG register and cache capabilities.
    /// On success `caps()` returns the decoded fields and `probed()` is true.
    bool probe(IlaCaps& out);
    bool probe() { IlaCaps tmp; return probe(tmp); }

    const IlaCaps& caps() const { return caps_; }
    bool probed() const { return caps_.probed; }

    // Runtime-configurable accessors. Return probed values when available,
    // compile-time defaults otherwise.
    int dataWidth() const { return caps_.data_w; }
    int addrWidth() const { return caps_.addr_w; }
    int depth()     const { return static_cast<int>(caps_.depth); }

    bool readIdcode(uint32_t& idcode);
    /// Read `caps().sig_count` signal-lane definitions from SIG_DEF (IR 5'h03).
    /// Returns false (and sets lastError()) if sig_count is 0 or a JTAG error occurs.
    bool readSignalDefs(std::vector<IlaSignalEntry>& out);

    /// Configure trigger: level match (mask/value), edge match (rise/fall bitmasks),
    /// OR-mode second group (mask2/val2), or_mode flag, and pre-trigger depth.
    /// Rise/fall/mask2/val2/or_mode registers are skipped for older bitstreams;
    /// using non-zero values on an incompatible version sets lastError() and returns false.
    bool configureTrigger(uint32_t mask, uint32_t value,
                          uint32_t rise_mask, uint32_t fall_mask,
                          uint32_t mask2, uint32_t val2, bool or_mode,
                          uint16_t pre_samples);

    /// 5-arg overload: calls 8-arg form with mask2=0, val2=0, or_mode=false.
    bool configureTrigger(uint32_t mask, uint32_t value,
                          uint32_t rise_mask, uint32_t fall_mask,
                          uint16_t pre_samples);

    /// Legacy 3-arg overload: calls 8-arg form with rise=fall=mask2=val2=0, or_mode=false.
    bool configureTrigger(uint32_t mask, uint32_t value, uint16_t pre_samples);

    bool arm();
    bool stop();
    bool resetCapture();
    bool forceTrigger();
    bool readStatus(IlaStatus& out);
    bool setReadAddr(uint16_t addr);
    bool readSamples(std::vector<uint32_t>& out);
    bool readSamples(std::vector<uint32_t>& out, uint32_t count);

    const std::string& lastError() const { return last_error_; }

private:
    bool shiftDrInt(int dr_bits, uint32_t tdi_val, uint32_t& tdo_val);

    IlaTapBackend& backend_;
    IlaCaps        caps_{};
    std::string    last_error_;
};

} // namespace jtag::ila
