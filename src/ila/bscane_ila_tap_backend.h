// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "src/ila/ila_tap_backend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jtag { class JtagChain; }

namespace jtag::ila {

/// IlaTapBackend that routes through a BSCANE2 USERn scan chain on the
/// Xilinx PL TAP (7-series / Zynq).  Use with ila_bscane2_top.sv.
/// No additional JTAG cable is needed; the FPGA's built-in USB JTAG
/// (e.g. FT2232H on Zybo Z7) is sufficient.
///
/// Up to four independent instances can coexist in one design.  Pass
/// @p user_chain = 1..4 to select USER1..USER4 respectively.
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
///   BscaneIlaTapBackend be(chain, pl_tap_idx);          // USER1 (default)
///   BscaneIlaTapBackend be(chain, pl_tap_idx, 2);        // USER2
///   IlaDriver drv(be);
///   drv.arm();          // issues USERn + CTRL scan internally
///
/// @note  pl_tap_index is the ChainDevice index of the Zynq PL Config TAP,
///        which is typically index 1 in a Zynq chain (PS ARM DAP = 0).
class BscaneIlaTapBackend : public IlaTapBackend {
public:
    /// @param user_chain  BSCANE2 scan-chain index: 1=USER1, 2=USER2,
    ///                    3=USER3, 4=USER4.  Defaults to 1.
    explicit BscaneIlaTapBackend(JtagChain& chain, int pl_tap_index,
                                  int user_chain = 1);

    bool selectIr(uint32_t opcode) override;
    bool shiftDr(int dr_bits, const uint8_t* tdi,
                  std::vector<uint8_t>& tdo) override;
    /// Bulk override: performs a prime scan then N real scans via
    /// TapController::shiftDRRepeat — one USB round-trip per 512-scan chunk.
    bool shiftDrBatch(int count, int dr_bits, const uint8_t* tdi,
                       std::vector<uint8_t>& flat_tdo) override;
    const std::string& lastError() const override { return last_error_; }

    /// Xilinx 7-series / Zynq PL TAP USER instruction opcodes (IR = 6 bits).
    /// Index matches JTAG_CHAIN parameter (1..4).
    static constexpr uint32_t kUserOpcodes[5] = {
        0x00u,  // [0] unused
        0x02u,  // [1] USER1
        0x03u,  // [2] USER2
        0x22u,  // [3] USER3
        0x23u,  // [4] USER4
    };

private:
    JtagChain&  chain_;
    int         pl_tap_index_;
    int         user_chain_;             // 1..4 (BSCANE2 JTAG_CHAIN value)
    uint32_t    pending_ir_       = 0x01u;  // opcode to embed in next frame
    // Initialize to 0xFF (impossible opcode) so the very first selectIr/shiftDr
    // always triggers a prime scan regardless of the opcode.  This means the
    // BSCANE2 first-scan-always-0 quirk is absorbed by that prime scan, and the
    // subsequent real scan reliably captures register data.
    // (Previously 0x01 matched the post-reset stored_ir, skipping the prime scan
    // for IDCODE reads and leaving only 2 warm-up DR scans before CONFIG; on some
    // boards after PL programming that was insufficient.)
    uint32_t    stored_ir_in_hw_  = 0xFFu;  // HDL stored_ir after last UPDATE
    std::string last_error_;

    // Perform one 37-bit frame DR shift with the current pending_ir_ and the
    // given 4-byte data payload (LSB-first). Returns false on JTAG error.
    bool doRawFrameShift(const uint8_t* data32, std::vector<uint8_t>& tdo);

    // Total frame width: 5-bit opcode + 32-bit data
    static constexpr int kFrameBits = 37;
};

} // namespace jtag::ila
