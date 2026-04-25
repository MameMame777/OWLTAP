// SPDX-License-Identifier: Apache-2.0
// OwlTAP Internal Logic Analyzer — Top level
`timescale 1ns/1ps
//
// Integrates the TAP controller, trigger comparator, capture FSM, and BRAM.
// Handles clock-domain crossings between `tck` (JTAG) and `sample_clk`
// (user clock).
//
// CDC policy:
//   - Control pulses (arm/stop/reset/force_trig): toggle-based pulse
//     synchronizer from tck to sample_clk.
//   - Status bits (armed/triggered/full): 2-flop synchronizer from
//     sample_clk to tck.
//   - Configuration words (mask/value/pre_samples): treated as quasi-static.
//     Caller must set them before asserting `arm`. The receiving flops use
//     `ASYNC_REG = TRUE` to tolerate the rare transient.
//   - Sample data: captured only inside sample_clk domain; readback via
//     dual-port BRAM (write: sample_clk, read: tck).
//
// Daisy-chain: `tdi` is from upstream device, `tdo` feeds downstream. When
// the TAP is not selected (SHIFT_*), tdo_oe=0 and the wire should be
// overridden externally if you need pure pass-through. For simplicity this
// implementation always drives tdo; concatenate multiple TAPs by routing
// `tdo` → next `tdi`.

`default_nettype none

module ila_top #(
    parameter int DATA_W      = 32,
    parameter int DEPTH       = 1024,
    parameter int ADDR_W      = 10,
    parameter int NUM_CH      = 1,
    parameter logic [31:0] IDCODE_VAL = 32'hA17A_0001,
    parameter int         SIG_COUNT            = 1,
    parameter logic [7:0] SIG_HI  [0:14] = '{8'd31, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0},
    parameter logic [7:0] SIG_LO  [0:14] = '{default: 8'd0},
    parameter logic [3:0] SIG_FMT [0:14] = '{default: 4'd0}
) (
    // JTAG
    input  wire                 tck,
    input  wire                 trst_n,
    input  wire                 tms,
    input  wire                 tdi,
    output logic                tdo,

    // User sampling interface
    input  wire                 sample_clk,
    input  wire                 sample_rst_n,
    input  wire [DATA_W-1:0]    data_in,
    input  wire                 data_valid
);

    // ------------------------------------------------------------------
    // TAP controller
    // ------------------------------------------------------------------
    logic                ctrl_arm_tck, ctrl_stop_tck;
    logic                ctrl_reset_tck, ctrl_force_trig_tck;
    logic [DATA_W-1:0]   trig_mask_tck, trig_value_tck;
    logic [ADDR_W-1:0]   pre_samples_tck;

    logic                sts_armed_tck;
    logic                sts_triggered_tck;
    logic                sts_full_tck;

    logic [ADDR_W-1:0]   rd_addr_tck;
    logic [DATA_W-1:0]   rd_data_tck;

    logic tdo_int, tdo_oe_int;

    ila_tap #(
        .DATA_W(DATA_W), .DEPTH(DEPTH), .ADDR_W(ADDR_W),
        .NUM_CH(NUM_CH),
        .IDCODE_VAL(IDCODE_VAL),
        .SIG_COUNT(SIG_COUNT),
        .SIG_HI(SIG_HI),
        .SIG_LO(SIG_LO),
        .SIG_FMT(SIG_FMT)
    ) u_tap (
        .tck             (tck),
        .trst_n          (trst_n),
        .tms             (tms),
        .tdi             (tdi),
        .tdo             (tdo_int),
        .tdo_oe          (tdo_oe_int),
        .ctrl_arm        (ctrl_arm_tck),
        .ctrl_stop       (ctrl_stop_tck),
        .ctrl_reset      (ctrl_reset_tck),
        .ctrl_force_trig (ctrl_force_trig_tck),
        .trig_mask       (trig_mask_tck),
        .trig_value      (trig_value_tck),
        .pre_samples     (pre_samples_tck),
        .sts_armed       (sts_armed_tck),
        .sts_triggered   (sts_triggered_tck),
        .sts_full        (sts_full_tck),
        .rd_addr         (rd_addr_tck),
        .rd_data         (rd_data_tck)
    );

    // When our TAP is not driving, we fall through TDI so a downstream
    // device can be chained behind us transparently.
    assign tdo = tdo_oe_int ? tdo_int : tdi;

    // ------------------------------------------------------------------
    // Pulse synchronizers: tck -> sample_clk
    // ------------------------------------------------------------------
    logic arm_sc, stop_sc, reset_sc, force_sc;

    pulse_sync u_arm_sync   (.src_clk(tck), .src_rst_n(trst_n),
                             .dst_clk(sample_clk), .dst_rst_n(sample_rst_n),
                             .src_pulse(ctrl_arm_tck), .dst_pulse(arm_sc));
    pulse_sync u_stop_sync  (.src_clk(tck), .src_rst_n(trst_n),
                             .dst_clk(sample_clk), .dst_rst_n(sample_rst_n),
                             .src_pulse(ctrl_stop_tck), .dst_pulse(stop_sc));
    pulse_sync u_reset_sync (.src_clk(tck), .src_rst_n(trst_n),
                             .dst_clk(sample_clk), .dst_rst_n(sample_rst_n),
                             .src_pulse(ctrl_reset_tck), .dst_pulse(reset_sc));
    pulse_sync u_force_sync (.src_clk(tck), .src_rst_n(trst_n),
                             .dst_clk(sample_clk), .dst_rst_n(sample_rst_n),
                             .src_pulse(ctrl_force_trig_tck),
                             .dst_pulse(force_sc));

    // ------------------------------------------------------------------
    // Quasi-static config (caller must set before arm)
    // ------------------------------------------------------------------
    (* ASYNC_REG = "TRUE" *) logic [DATA_W-1:0] mask_sc_m, mask_sc;
    (* ASYNC_REG = "TRUE" *) logic [DATA_W-1:0] val_sc_m, val_sc;
    (* ASYNC_REG = "TRUE" *) logic [ADDR_W-1:0] pre_sc_m, pre_sc;

    always_ff @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) begin
            mask_sc_m <= '0; mask_sc <= '0;
            val_sc_m  <= '0; val_sc  <= '0;
            pre_sc_m  <= '0; pre_sc  <= '0;
        end else begin
            mask_sc_m <= trig_mask_tck;   mask_sc <= mask_sc_m;
            val_sc_m  <= trig_value_tck;  val_sc  <= val_sc_m;
            pre_sc_m  <= pre_samples_tck; pre_sc  <= pre_sc_m;
        end
    end

    // ------------------------------------------------------------------
    // Sample-clock datapath
    // ------------------------------------------------------------------
    logic                trig_match_sc;
    logic                write_en_sc;
    logic [ADDR_W-1:0]   write_addr_sc, trigger_addr_sc;
    logic                armed_sc, triggered_sc, full_sc;

    ila_trigger #(.DATA_W(DATA_W)) u_trig (
        .clk      (sample_clk),
        .rst_n    (sample_rst_n),
        .data_in  (data_in),
        .valid_in (data_valid),
        .mask     (mask_sc),
        .value    (val_sc),
        .match    (trig_match_sc)
    );

    ila_capture_fsm #(.DEPTH(DEPTH), .ADDR_W(ADDR_W)) u_fsm (
        .clk           (sample_clk),
        .rst_n         (sample_rst_n),
        .arm           (arm_sc),
        .stop          (stop_sc),
        .reset_capture (reset_sc),
        .force_trig    (force_sc),
        .pre_samples   (pre_sc),
        .valid_in      (data_valid),
        .trig_match    (trig_match_sc),
        .write_addr    (write_addr_sc),
        .write_en      (write_en_sc),
        .trigger_addr  (trigger_addr_sc),
        .armed         (armed_sc),
        .triggered     (triggered_sc),
        .full          (full_sc)
    );

    // ------------------------------------------------------------------
    // BRAM
    // ------------------------------------------------------------------
    ila_bram #(.DATA_W(DATA_W), .DEPTH(DEPTH), .ADDR_W(ADDR_W)) u_bram (
        .wr_clk  (sample_clk),
        .wr_en   (write_en_sc),
        .wr_addr (write_addr_sc),
        .wr_data (data_in),
        .rd_clk  (tck),
        .rd_addr (rd_addr_tck),
        .rd_data (rd_data_tck)
    );

    // ------------------------------------------------------------------
    // Status sync: sample_clk -> tck
    // ------------------------------------------------------------------
    (* ASYNC_REG = "TRUE" *) logic armed_tck_m, triggered_tck_m, full_tck_m;

    always_ff @(posedge tck or negedge trst_n) begin
        if (!trst_n) begin
            armed_tck_m       <= 1'b0; sts_armed_tck     <= 1'b0;
            triggered_tck_m   <= 1'b0; sts_triggered_tck <= 1'b0;
            full_tck_m        <= 1'b0; sts_full_tck      <= 1'b0;
        end else begin
            armed_tck_m       <= armed_sc;     sts_armed_tck     <= armed_tck_m;
            triggered_tck_m   <= triggered_sc; sts_triggered_tck <= triggered_tck_m;
            full_tck_m        <= full_sc;      sts_full_tck      <= full_tck_m;
        end
    end

endmodule

// ---------------------------------------------------------------------
// Toggle-based pulse synchronizer (1-bit event).
// Source pulse is captured by toggling a flag on src_clk; the receiver
// detects edges on the synchronized flag and re-emits a single pulse.
// ---------------------------------------------------------------------
module pulse_sync (
    input  wire src_clk,
    input  wire src_rst_n,
    input  wire dst_clk,
    input  wire dst_rst_n,
    input  wire src_pulse,
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
            sync0 <= toggle_src;
            sync1 <= sync0;
            sync2 <= sync1;
        end
    end

    assign dst_pulse = sync1 ^ sync2;
endmodule

`default_nettype wire
