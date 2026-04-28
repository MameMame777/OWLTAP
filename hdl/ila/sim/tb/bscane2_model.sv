// SPDX-License-Identifier: Apache-2.0
// Behavioral simulation model for Xilinx BSCANE2 primitive.
// Used in place of the unisim library during DSim simulation.
//
// In simulation (+define+SIMULATION), JTAG signals enter via explicit
// input ports (JTAG_TCK, JTAG_TMS, JTAG_TDI, JTAG_TRST_N) that are
// connected from the testbench through ila_bscane2_top's sim_* ports.
// This avoids the race condition where wires driven by a virtual interface
// package global don't track events correctly in some simulators.
//
// The muxed TDO (idle-high, USER1/Shift-DR: ILA TDO) is available as
// the wire 'tdo_muxed'; ila_bscane2_top exposes it as 'sim_tdo' for the
// testbench to connect to jtag_if.tdo.

`timescale 1ns/1ps

// bscane2_sim_pkg is kept for forward-compatibility but is no longer used.
package bscane2_sim_pkg;
endpackage

// ---------------------------------------------------------------------------
// BSCANE2 behavioral model
// ---------------------------------------------------------------------------
// Implements a 6-bit IEEE 1149.1 TAP FSM.
// When IR == USER1 (6'h02):
//   - CAPTURE, SHIFT, UPDATE are high in the corresponding TAP states
//   - TCK, TDI are forwarded to the user logic via output ports
//   - User's TDO is routed back via tdo_muxed wire
// ---------------------------------------------------------------------------
module BSCANE2 #(
    parameter integer JTAG_CHAIN = 1
) (
    output logic TCK,
    output logic TDI,
    output logic RESET,
    output logic RUNTEST,
    output logic SEL,
    output logic SHIFT,
    output logic CAPTURE,
    output logic UPDATE,
    output logic DRCK,
    input  wire  TDO
`ifdef SIMULATION
    // Simulation-only input ports for JTAG signals.
    // Connected from the testbench via ila_bscane2_top's sim_tck/tms/tdi/trst_n.
   ,input  wire  JTAG_TCK
   ,input  wire  JTAG_TMS
   ,input  wire  JTAG_TDI
   ,input  wire  JTAG_TRST_N
`endif
);

    // ------------------------------------------------------------------
    // JTAG signal sources
    // ------------------------------------------------------------------
`ifdef SIMULATION
    wire tck_in    = JTAG_TCK;
    wire tms_in    = JTAG_TMS;
    wire tdi_in    = JTAG_TDI;
    wire trst_n_in = JTAG_TRST_N;
`else
    // Synthesis: signals come from the PL TAP (not modeled here).
    // These tie-offs prevent synthesis lint warnings; they are never
    // synthesized into real logic because BSCANE2 is a black box.
    wire tck_in    = 1'b0;
    wire tms_in    = 1'b0;
    wire tdi_in    = 1'b0;
    wire trst_n_in = 1'b1;
`endif

    // ------------------------------------------------------------------
    // IEEE 1149.1 16-state TAP FSM
    // ------------------------------------------------------------------
    typedef enum logic [3:0] {
        S_TLR    = 4'd0,
        S_RTI    = 4'd1,
        S_SEL_DR = 4'd2,
        S_CAP_DR = 4'd3,
        S_SHF_DR = 4'd4,
        S_EX1_DR = 4'd5,
        S_PAU_DR = 4'd6,
        S_EX2_DR = 4'd7,
        S_UPD_DR = 4'd8,
        S_SEL_IR = 4'd9,
        S_CAP_IR = 4'd10,
        S_SHF_IR = 4'd11,
        S_EX1_IR = 4'd12,
        S_PAU_IR = 4'd13,
        S_EX2_IR = 4'd14,
        S_UPD_IR = 4'd15
    } tap_state_e;

    tap_state_e state;
    logic [5:0]  ir_reg;    // active IR (latched at Update-IR)
    logic [5:0]  ir_shift;  // shift register for IR scan

    // ------------------------------------------------------------------
    // USER opcode for the selected scan chain (Xilinx 7-series IR = 6 bits).
    //   JTAG_CHAIN 1 = USER1 (6'h02)
    //   JTAG_CHAIN 2 = USER2 (6'h03)
    //   JTAG_CHAIN 3 = USER3 (6'h22)
    //   JTAG_CHAIN 4 = USER4 (6'h23)
    // ------------------------------------------------------------------
    function automatic logic [5:0] user_opcode(input int chain);
        case (chain)
            1:       return 6'h02;
            2:       return 6'h03;
            3:       return 6'h22;
            4:       return 6'h23;
            default: return 6'h02;
        endcase
    endfunction

    localparam logic [5:0] USER_OPCODE = user_opcode(JTAG_CHAIN);

    // ------------------------------------------------------------------
    // TAP state machine
    // ------------------------------------------------------------------
    always_ff @(posedge tck_in or negedge trst_n_in) begin
        if (!trst_n_in) begin
            state  <= S_TLR;
            ir_reg <= 6'h3F;  // BYPASS default
        end else begin
            case (state)
                S_TLR:    state <= tms_in ? S_TLR    : S_RTI;
                S_RTI:    state <= tms_in ? S_SEL_DR : S_RTI;
                S_SEL_DR: state <= tms_in ? S_SEL_IR : S_CAP_DR;
                S_CAP_DR: state <= tms_in ? S_EX1_DR : S_SHF_DR;
                S_SHF_DR: state <= tms_in ? S_EX1_DR : S_SHF_DR;
                S_EX1_DR: state <= tms_in ? S_UPD_DR : S_PAU_DR;
                S_PAU_DR: state <= tms_in ? S_EX2_DR : S_PAU_DR;
                S_EX2_DR: state <= tms_in ? S_UPD_DR : S_SHF_DR;
                S_UPD_DR: state <= tms_in ? S_SEL_DR : S_RTI;
                S_SEL_IR: state <= tms_in ? S_TLR    : S_CAP_IR;
                S_CAP_IR: state <= tms_in ? S_EX1_IR : S_SHF_IR;
                S_SHF_IR: state <= tms_in ? S_EX1_IR : S_SHF_IR;
                S_EX1_IR: state <= tms_in ? S_UPD_IR : S_PAU_IR;
                S_PAU_IR: state <= tms_in ? S_EX2_IR : S_PAU_IR;
                S_EX2_IR: state <= tms_in ? S_UPD_IR : S_SHF_IR;
                S_UPD_IR: begin
                    state  <= tms_in ? S_SEL_DR : S_RTI;
                    ir_reg <= ir_shift;
                end
                default:  state <= S_TLR;
            endcase
        end
    end

    // ------------------------------------------------------------------
    // IR shift register (6 bits, LSB-first)
    // ------------------------------------------------------------------
    always_ff @(posedge tck_in or negedge trst_n_in) begin
        if (!trst_n_in)
            ir_shift <= 6'h3F;
        else if (state == S_CAP_IR)
            ir_shift <= 6'b000001;  // per 1149.1 capture value
        else if (state == S_SHF_IR)
            ir_shift <= {tdi_in, ir_shift[5:1]};
    end

    // ------------------------------------------------------------------
    // USERn selection gate
    // ------------------------------------------------------------------
    wire user1_active = (ir_reg == USER_OPCODE);

    // ------------------------------------------------------------------
    // BSCANE2 output ports
    // ------------------------------------------------------------------
    assign TCK     = tck_in;
    assign TDI     = tdi_in;
    assign RESET   = (state == S_TLR);
    assign RUNTEST = (state == S_RTI);
    assign SEL     = user1_active;
    assign SHIFT   = user1_active && (state == S_SHF_DR);
    assign CAPTURE = user1_active && (state == S_CAP_DR);
    assign UPDATE  = user1_active && (state == S_UPD_DR);
    assign DRCK    = user1_active ? tck_in : 1'b0;

    // ------------------------------------------------------------------
    // Muxed TDO: USER1/Shift-DR -> user TDO; otherwise idle-high.
    // The parent module (ila_bscane2_top) exposes this as sim_tdo so
    // the testbench can connect it to jtag_if.tdo.
    // ------------------------------------------------------------------
    wire tdo_muxed = (user1_active && (state == S_SHF_DR)) ? TDO : 1'b1;

endmodule
