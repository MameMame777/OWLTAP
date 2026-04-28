// SPDX-License-Identifier: Apache-2.0
// OwlTAP ILA -- BSCANE2-based access port (Xilinx 7-series / Zynq)
`timescale 1ns/1ps
//
// Replaces the dedicated TAP in ila_top with BSCANE2 USERn, allowing the
// ILA to be accessed via the FPGA's built-in USB JTAG port (FT2232H on
// Zybo Z7, Digilent JTAG-HS2, etc.) with no extra pins or cables.
// Up to four independent instances can coexist in one design by assigning
// each a unique JTAG_CHAIN value (1..4 → USER1..USER4).
//
// ---------------------------------------------------------------------------
// 37-bit DR frame protocol (BSCANE2 USER1 scan chain)
// ---------------------------------------------------------------------------
//  Host sends 37 bits LSB-first in every DR scan:
//    bits [4:0]  = 5-bit sub-opcode  (same values as ila_tap.sv IR opcodes)
//    bits [36:5] = 32-bit data payload
//
//  CAPTURE (before each shift) preloads:
//    [4:0]  = stored active opcode
//    [36:5] = register value selected by that opcode
//
//  UPDATE (after each shift) latches:
//    stored_ir  <- dr_shift[4:0]  (new opcode)
//    register   <- dr_shift[36:5] (new data, for writable regs)
//
//  To read a register (e.g. STATUS):
//    scan 1: {5'h09, 32'h0}  -- UPDATE sets stored_ir = STATUS
//    scan 2: {5'h09, 32'h0}  -- CAPTURE preloads status; read TDO[36:5]
//
//  To write a register (e.g. TRIG_MASK):
//    scan 1: {5'h0A, mask}   -- UPDATE writes trig_mask_tck
//
// ---------------------------------------------------------------------------
// Opcodes (identical to ila_tap.sv)
// ---------------------------------------------------------------------------
//  5'h01 IDCODE     32-bit R
//  5'h08 CTRL        4-bit W  [0]=ARM [1]=STOP [2]=RESET [3]=FORCE_TRIG
//  5'h09 STATUS      8-bit R  [0]=armed [1]=triggered [2]=full
//  5'h0A TRIG_MASK  32-bit RW
//  5'h0B TRIG_VAL   32-bit RW
//  5'h0C READ_ADDR  10-bit RW
//  5'h0D READ_DATA  32-bit R  (auto-increments READ_ADDR on UPDATE)
//  5'h0E PRE_SAMPLES 10-bit RW
//  5'h1F BYPASS      1-bit  (write all-1s)
//
// ---------------------------------------------------------------------------
// CDC policy: identical to ila_top.sv
//   - Control pulses:    toggle pulse-sync  tck -> sample_clk
//   - Status bits:       2-FF sync          sample_clk -> tck
//   - Config words:      ASYNC_REG 2-FF sync (quasi-static, set before ARM)
// ---------------------------------------------------------------------------

`default_nettype none

module ila_bscane2_top #(
    // BSCANE2 scan-chain selector (1..4 → USER1..USER4).
    // Each instance in the same design MUST use a unique value.
    parameter int JTAG_CHAIN = 1,
    parameter int DATA_W    = 32,
    parameter int DEPTH     = 1024,
    parameter int ADDR_W    = 10,
    parameter int NUM_CH    = 1,
    parameter logic [31:0] IDCODE_VAL = 32'hA17A_0001,
    // Signal definition ROM (Phase 2+).  Only indices [0:SIG_COUNT-1] are used.
    // SIG_HI[i]/SIG_LO[i] are inclusive 0-based bit indices; SIG_FMT[i]: 0=HEX 1=DEC 2=BIN.
    // SIG_COUNT must be in [1..15].
    parameter int         SIG_COUNT            = 1,
    parameter logic [7:0] SIG_HI  [0:14] = '{8'd31, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0},
    parameter logic [7:0] SIG_LO  [0:14] = '{default: 8'd0},
    parameter logic [3:0] SIG_FMT [0:14] = '{default: 4'd0}
) (
    // User capture interface
    input  wire              sample_clk,
    input  wire              sample_rst_n,
    input  wire [DATA_W-1:0] data_in,
    input  wire              data_valid
`ifdef SIMULATION
    // Simulation-only JTAG ports — connect from testbench.
    // Not present in synthesized design (ifdef'd out).
   ,input  wire  sim_tck
   ,input  wire  sim_tms
   ,input  wire  sim_tdi
   ,input  wire  sim_trst_n
   ,output wire  sim_tdo
`endif
);

    // ------------------------------------------------------------------
    // BSCANE2 -- USERn scan chain (JTAG_CHAIN selects USER1..USER4)
    // ------------------------------------------------------------------
    wire  bscan_tck, bscan_tdi;
    logic bscan_tdo;
    wire  bscan_shift, bscan_capture, bscan_update, bscan_reset;

    BSCANE2 #(.JTAG_CHAIN(JTAG_CHAIN)) u_bscan (
        .TCK     (bscan_tck),
        .TDI     (bscan_tdi),
        .TDO     (bscan_tdo),
        .SHIFT   (bscan_shift),
        .CAPTURE (bscan_capture),
        .UPDATE  (bscan_update),
        .RESET   (bscan_reset),
        .RUNTEST (),
        .SEL     (),
        .DRCK    ()
`ifdef SIMULATION
       ,.JTAG_TCK  (sim_tck)
       ,.JTAG_TMS  (sim_tms)
       ,.JTAG_TDI  (sim_tdi)
       ,.JTAG_TRST_N(sim_trst_n)
`endif
    );

`ifdef SIMULATION
    // Expose the BSCANE2 muxed TDO as a top-level output for the testbench.
    // tdo_muxed is idle-high; it drives the ILA TDO only in USER1/Shift-DR.
    assign sim_tdo = u_bscan.tdo_muxed;
`endif

    // RESET = Test-Logic-Reset state; use as active-low reset for TCK domain.
    wire tck_rst_n = ~bscan_reset;

    // ------------------------------------------------------------------
    // 37-bit shift register
    //   [4:0]        = opcode field (driven to TDO first; TDI arrives last)
    //   [FRAME_W-1:5] = data field  (DATA_W bits)
    // JTAG LSB-first: TDO <- dr_shift[0]; new TDI -> dr_shift[FRAME_W-1]
    // After FRAME_W shifts: [4:0] = first 5 TDI bits sent = opcode
    //                        [FRAME_W-1:5] = next DATA_W TDI bits = data
    // ------------------------------------------------------------------
    localparam int FRAME_W = DATA_W + 5;  // 37

    logic [4:0]          stored_ir;
    logic [3:0]          sig_def_idx;  // index into SIG_DEF ROM; auto-increments per scan
    logic [FRAME_W-1:0]  dr_shift;

    // ILA control registers (TCK domain)
    logic                ctrl_arm_tck, ctrl_stop_tck;
    logic                ctrl_reset_tck, ctrl_force_trig_tck;
    logic [DATA_W-1:0]   trig_mask_tck, trig_value_tck;
    logic [DATA_W-1:0]   trig_rise_mask_tck, trig_fall_mask_tck;
    logic [DATA_W-1:0]   trig_mask2_tck, trig_val2_tck;
    logic                trig_or_mode_tck;
    logic [ADDR_W-1:0]   pre_samples_tck;
    logic [ADDR_W-1:0]   rd_addr_tck;
    logic [DATA_W-1:0]   rd_data_tck;

    // Status (sample_clk -> tck synced)
    logic sts_armed_tck, sts_triggered_tck, sts_full_tck;

    // ------------------------------------------------------------------
    // CONFIG register value (read-only; see ila_tap.sv for bit layout)
    // ------------------------------------------------------------------
    // SIG_DEF word format (read-only, 32 bits):
    //   [31:28] fmt[3:0]  (0=HEX 1=DEC 2=BIN; future: SIGNED/TIME)
    //   [27:24] reserved  = 4'h0
    //   [23:16] hi[7:0]   (inclusive MSB index, 0-based)
    //   [15: 8] lo[7:0]   (inclusive LSB index, 0-based)
    //   [ 7: 0] name_idx  (0xFF = host auto-generates "data[hi:lo]")
    localparam logic [31:0] CONFIG_VAL = {
        8'h03,           // VERSION 3: OR-mode trigger (TRIG_MASK2 / TRIG_VAL2 / TRIG_CTRL)
        4'(NUM_CH),
        4'(SIG_COUNT),
        6'(DATA_W - 1),
        2'h0,
        8'(ADDR_W)
    };

    // ------------------------------------------------------------------
    // CAPTURE preload mux: select readable register by stored_ir
    // ------------------------------------------------------------------
    logic [DATA_W-1:0] capture_data;
    always_comb begin
        case (stored_ir)
            5'h01:   capture_data = IDCODE_VAL;
            5'h02:   capture_data = CONFIG_VAL;
            5'h03:   capture_data = {SIG_FMT[sig_def_idx], 4'h0,
                                     SIG_HI[sig_def_idx], SIG_LO[sig_def_idx], 8'hFF};
            5'h09:   capture_data = {{(DATA_W-3){1'b0}},
                                      sts_full_tck, sts_triggered_tck,
                                      sts_armed_tck};
            5'h0A:   capture_data = trig_mask_tck;
            5'h0B:   capture_data = trig_value_tck;
            5'h0C:   capture_data = {{(DATA_W-ADDR_W){1'b0}}, rd_addr_tck};
            5'h0D:   capture_data = rd_data_tck;
            5'h0E:   capture_data = {{(DATA_W-ADDR_W){1'b0}}, pre_samples_tck};
            5'h0F:   capture_data = trig_rise_mask_tck;
            5'h10:   capture_data = trig_fall_mask_tck;
            5'h11:   capture_data = trig_mask2_tck;
            5'h12:   capture_data = trig_val2_tck;
            5'h13:   capture_data = {{(DATA_W-1){1'b0}}, trig_or_mode_tck};
            default: capture_data = '0;
        endcase
    end

    // ------------------------------------------------------------------
    // Shift register: CAPTURE loads; SHIFT clocks; TDO = LSB
    // ------------------------------------------------------------------
    always_ff @(posedge bscan_tck) begin
        if (bscan_capture) begin
            dr_shift <= {capture_data, stored_ir};
        end else if (bscan_shift) begin
            // LSB-first: new TDI bit enters at MSB
            dr_shift <= {bscan_tdi, dr_shift[FRAME_W-1:1]};
        end
    end

    assign bscan_tdo = dr_shift[0];

    // ------------------------------------------------------------------
    // UPDATE: latch new opcode + write registers / pulse CTRL
    // ------------------------------------------------------------------
    localparam logic [ADDR_W-1:0] kDefaultPre = ADDR_W'(DEPTH / 4);

    always_ff @(posedge bscan_tck or negedge tck_rst_n) begin
        if (!tck_rst_n) begin
            stored_ir           <= 5'h01;     // default: IDCODE
            sig_def_idx         <= '0;
            trig_mask_tck       <= '0;
            trig_value_tck      <= '0;
            trig_rise_mask_tck  <= '0;
            trig_fall_mask_tck  <= '0;
            trig_mask2_tck      <= '0;
            trig_val2_tck       <= '0;
            trig_or_mode_tck    <= 1'b0;
            pre_samples_tck     <= kDefaultPre;
            rd_addr_tck         <= '0;
            ctrl_arm_tck        <= 1'b0;
            ctrl_stop_tck       <= 1'b0;
            ctrl_reset_tck      <= 1'b0;
            ctrl_force_trig_tck <= 1'b0;
        end else begin
            // Single-cycle control pulses: clear every cycle
            ctrl_arm_tck        <= 1'b0;
            ctrl_stop_tck       <= 1'b0;
            ctrl_reset_tck      <= 1'b0;
            ctrl_force_trig_tck <= 1'b0;

            if (bscan_update) begin
                // IR change: reset SIG_DEF index so next read starts at entry[0].
                // Consecutive SIG_DEF scans (opcode unchanged) advance the index.
                if (dr_shift[4:0] != stored_ir)
                    sig_def_idx <= '0;
                else if (dr_shift[4:0] == 5'h03)
                    sig_def_idx <= (sig_def_idx < 4'(SIG_COUNT - 1))
                                   ? sig_def_idx + 4'd1 : '0;
                stored_ir <= dr_shift[4:0];

                case (dr_shift[4:0])
                    5'h08: begin  // CTRL: bits [3:0] of data field = pulse flags
                        ctrl_arm_tck        <= dr_shift[5];
                        ctrl_stop_tck       <= dr_shift[6];
                        ctrl_reset_tck      <= dr_shift[7];
                        ctrl_force_trig_tck <= dr_shift[8];
                    end
                    5'h0A: trig_mask_tck        <= dr_shift[FRAME_W-1:5];
                    5'h0B: trig_value_tck        <= dr_shift[FRAME_W-1:5];
                    5'h0C: rd_addr_tck           <= dr_shift[5 +: ADDR_W];
                    5'h0D: rd_addr_tck           <= rd_addr_tck + 1'b1;   // auto-inc
                    5'h0E: pre_samples_tck       <= dr_shift[5 +: ADDR_W];
                    5'h0F: trig_rise_mask_tck    <= dr_shift[FRAME_W-1:5];
                    5'h10: trig_fall_mask_tck    <= dr_shift[FRAME_W-1:5];
                    5'h11: trig_mask2_tck        <= dr_shift[FRAME_W-1:5];
                    5'h12: trig_val2_tck         <= dr_shift[FRAME_W-1:5];
                    5'h13: trig_or_mode_tck      <= dr_shift[5];  // bit[0] of data field
                    default: ;
                endcase
            end
        end
    end

    // ------------------------------------------------------------------
    // CDC: control pulses tck -> sample_clk
    // (bscane2_pulse_sync defined at bottom of this file)
    // ------------------------------------------------------------------
    logic arm_sc, stop_sc, reset_sc, force_sc;

    bscane2_pulse_sync u_arm_ps (
        .src_clk(bscan_tck),    .src_rst_n(tck_rst_n),
        .dst_clk(sample_clk),   .dst_rst_n(sample_rst_n),
        .src_pulse(ctrl_arm_tck),   .dst_pulse(arm_sc));
    bscane2_pulse_sync u_stop_ps (
        .src_clk(bscan_tck),    .src_rst_n(tck_rst_n),
        .dst_clk(sample_clk),   .dst_rst_n(sample_rst_n),
        .src_pulse(ctrl_stop_tck),  .dst_pulse(stop_sc));
    bscane2_pulse_sync u_reset_ps (
        .src_clk(bscan_tck),    .src_rst_n(tck_rst_n),
        .dst_clk(sample_clk),   .dst_rst_n(sample_rst_n),
        .src_pulse(ctrl_reset_tck), .dst_pulse(reset_sc));
    bscane2_pulse_sync u_force_ps (
        .src_clk(bscan_tck),    .src_rst_n(tck_rst_n),
        .dst_clk(sample_clk),   .dst_rst_n(sample_rst_n),
        .src_pulse(ctrl_force_trig_tck), .dst_pulse(force_sc));

    // ------------------------------------------------------------------
    // Quasi-static config: tck -> sample_clk (ASYNC_REG 2-FF)
    // ------------------------------------------------------------------
    (* ASYNC_REG = "TRUE" *) logic [DATA_W-1:0] mask_sc_m, mask_sc;
    (* ASYNC_REG = "TRUE" *) logic [DATA_W-1:0] val_sc_m,  val_sc;
    (* ASYNC_REG = "TRUE" *) logic [ADDR_W-1:0] pre_sc_m,  pre_sc;
    (* ASYNC_REG = "TRUE" *) logic [DATA_W-1:0] rise_sc_m, rise_sc;
    (* ASYNC_REG = "TRUE" *) logic [DATA_W-1:0] fall_sc_m, fall_sc;
    (* ASYNC_REG = "TRUE" *) logic [DATA_W-1:0] mask2_sc_m, mask2_sc;
    (* ASYNC_REG = "TRUE" *) logic [DATA_W-1:0] val2_sc_m,  val2_sc;
    (* ASYNC_REG = "TRUE" *) logic              or_mode_sc_m, or_mode_sc;

    always_ff @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) begin
            mask_sc_m <= '0; mask_sc <= '0;
            val_sc_m  <= '0; val_sc  <= '0;
            pre_sc_m  <= '0; pre_sc  <= '0;
            rise_sc_m <= '0; rise_sc <= '0;
            fall_sc_m <= '0; fall_sc <= '0;
            mask2_sc_m    <= '0; mask2_sc    <= '0;
            val2_sc_m     <= '0; val2_sc     <= '0;
            or_mode_sc_m  <= 1'b0; or_mode_sc <= 1'b0;
        end else begin
            mask_sc_m <= trig_mask_tck;         mask_sc <= mask_sc_m;
            val_sc_m  <= trig_value_tck;        val_sc  <= val_sc_m;
            pre_sc_m  <= pre_samples_tck;       pre_sc  <= pre_sc_m;
            rise_sc_m <= trig_rise_mask_tck;    rise_sc <= rise_sc_m;
            fall_sc_m <= trig_fall_mask_tck;    fall_sc <= fall_sc_m;
            mask2_sc_m   <= trig_mask2_tck;     mask2_sc   <= mask2_sc_m;
            val2_sc_m    <= trig_val2_tck;      val2_sc    <= val2_sc_m;
            or_mode_sc_m <= trig_or_mode_tck;   or_mode_sc <= or_mode_sc_m;
        end
    end

    // ------------------------------------------------------------------
    // Trigger comparator, capture FSM, BRAM
    // ------------------------------------------------------------------
    logic                trig_match_sc;
    logic                write_en_sc;
    logic [ADDR_W-1:0]   write_addr_sc, trigger_addr_sc;
    logic                armed_sc, triggered_sc, full_sc;

    ila_trigger #(.DATA_W(DATA_W)) u_trig (
        .clk      (sample_clk),  .rst_n   (sample_rst_n),
        .data_in  (data_in),     .valid_in(data_valid),
        .mask     (mask_sc),     .value   (val_sc),
        .rise_mask(rise_sc),     .fall_mask(fall_sc),
        .mask2    (mask2_sc),    .val2     (val2_sc),
        .or_mode  (or_mode_sc),
        .match    (trig_match_sc)
    );

    ila_capture_fsm #(.DEPTH(DEPTH), .ADDR_W(ADDR_W)) u_fsm (
        .clk           (sample_clk),    .rst_n         (sample_rst_n),
        .arm           (arm_sc),        .stop          (stop_sc),
        .reset_capture (reset_sc),      .force_trig    (force_sc),
        .pre_samples   (pre_sc),        .valid_in      (data_valid),
        .trig_match    (trig_match_sc),
        .write_addr    (write_addr_sc), .write_en      (write_en_sc),
        .trigger_addr  (trigger_addr_sc),
        .armed         (armed_sc),      .triggered     (triggered_sc),
        .full          (full_sc)
    );

    ila_bram #(.DATA_W(DATA_W), .DEPTH(DEPTH), .ADDR_W(ADDR_W)) u_bram (
        .wr_clk  (sample_clk),   .wr_en   (write_en_sc),
        .wr_addr (write_addr_sc),.wr_data (data_in),
        .rd_clk  (bscan_tck),    .rd_addr (rd_addr_tck),
        .rd_data (rd_data_tck)
    );

    // ------------------------------------------------------------------
    // Status sync: sample_clk -> tck (2-FF)
    // ------------------------------------------------------------------
    (* ASYNC_REG = "TRUE" *) logic armed_tck_m, triggered_tck_m, full_tck_m;

    always_ff @(posedge bscan_tck or negedge tck_rst_n) begin
        if (!tck_rst_n) begin
            armed_tck_m     <= 1'b0; sts_armed_tck     <= 1'b0;
            triggered_tck_m <= 1'b0; sts_triggered_tck <= 1'b0;
            full_tck_m      <= 1'b0; sts_full_tck      <= 1'b0;
        end else begin
            armed_tck_m     <= armed_sc;     sts_armed_tck     <= armed_tck_m;
            triggered_tck_m <= triggered_sc; sts_triggered_tck <= triggered_tck_m;
            full_tck_m      <= full_sc;      sts_full_tck      <= full_tck_m;
        end
    end

endmodule


// ---------------------------------------------------------------------------
// Toggle-based pulse synchronizer.
// Named bscane2_pulse_sync to avoid collision with pulse_sync in ila_top.sv
// when both files are present in the same Vivado project.
// ---------------------------------------------------------------------------
module bscane2_pulse_sync (
    input  wire  src_clk,
    input  wire  src_rst_n,
    input  wire  dst_clk,
    input  wire  dst_rst_n,
    input  wire  src_pulse,
    output logic dst_pulse
);
    logic toggle_src;
    always_ff @(posedge src_clk or negedge src_rst_n) begin
        if (!src_rst_n) toggle_src <= 1'b0;
        else if (src_pulse) toggle_src <= ~toggle_src;
    end

    (* ASYNC_REG = "TRUE" *) logic sync0, sync1, sync2;
    always_ff @(posedge dst_clk or negedge dst_rst_n) begin
        if (!dst_rst_n) begin
            sync0 <= 1'b0; sync1 <= 1'b0; sync2 <= 1'b0;
        end else begin
            sync0 <= toggle_src; sync1 <= sync0; sync2 <= sync1;
        end
    end

    assign dst_pulse = sync1 ^ sync2;
endmodule

`default_nettype wire
