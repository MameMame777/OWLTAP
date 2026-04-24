// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "src/ila/ila_tap_backend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jtag { class JtagChain; }

namespace jtag::ila {

/// IlaTapBackend that routes through BSCANE2 USER1 on the Xilinx PL TAP.
///
/// Use with ila_bscane2_top.sv. No additional JTAG cable needed; the FPGA's
/// built-in USB JTAG (e.g. FT2232H on Zybo Z7) is sufficient.
///
/// Protocol: every JTAG transaction is a single 37-bit DR scan through the
/// PL TAP's USER1 scan chain:
///   bits [4:0]  = 5-bit sub-opcode (same as ila_tap.sv IR opcodes)
///   bits [36:5] = 32-bit data payload
///
/// selectIr(opcode):
///   Caches the opcode. Issues USER1 instruction to the PL TAP so subsequent
///   DR scans target the BSCANE2 USER1 chain.
///
/// shiftDr(dr_bits, tdi, tdo):
///   Builds a 37-bit frame {opcode[4:0], tdi[31:0]} and shifts it through
///   the PL TAP DR (inserting BYPASS bits for other chain devices). Returns
///   dr_bits bits extracted from the data field of TDO.
///   Two calls with the same opcode are required for reads (first scan sets
///   active opcode; second scan captures register value during CAPTURE-DR).
///
/// Typical usage:
///   BscaneIlaTapBackend be(chain, pl_tap_idx);
///   IlaDriver drv(be);
///   drv.arm();          // issues USER1 + CTRL scan internally
///
/// @note  pl_tap_index is the ChainDevice index of the Zynq PL Config TAP,
///        which is typically index 1 in a Zynq chain (PS ARM DAP = 0).
class BscaneIlaTapBackend : public IlaTapBackend {
public:
    explicit BscaneIlaTapBackend(JtagChain& chain, int pl_tap_index);

    bool selectIr(uint32_t opcode) override;
    bool shiftDr(int dr_bits, const uint8_t* tdi,
                  std::vector<uint8_t>& tdo) override;
    const std::string& lastError() const override { return last_error_; }

    /// USER1 instruction opcode for Xilinx 7-series / Zynq PL TAP (IR=6 bits).
    static constexpr uint32_t kUser1Opcode = 0x02u;

private:
    JtagChain&  chain_;
    int         pl_tap_index_;
    uint32_t    pending_ir_       = 0x01u;  // opcode to embed in next frame
    uint32_t    stored_ir_in_hw_  = 0x01u;  // HDL stored_ir after last UPDATE
    std::string last_error_;

    // Perform one 37-bit frame DR shift with the current pending_ir_ and the
    // given 4-byte data payload (LSB-first). Returns false on JTAG error.
    bool doRawFrameShift(const uint8_t* data32, std::vector<uint8_t>& tdo);

    // Total frame width: 5-bit opcode + 32-bit data
    static constexpr int kFrameBits = 37;
};

} // namespace jtag::ila
