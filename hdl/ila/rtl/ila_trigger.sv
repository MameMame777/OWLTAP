// SPDX-License-Identifier: Apache-2.0
// OwlTAP Internal Logic Analyzer — Trigger comparator
`timescale 1ns/1ps
//
// Trigger equation (sample_clk domain):
//
// --- Group A (level + edge) ---
//   prev_data  -- registered copy of data_in; updated only on valid_in.
//   rising     = ~prev_data &  data_in
//   falling    =  prev_data & ~data_in
//   level_ok_a = (mask == 0) OR ((data_in & mask) == (value & mask))
//   edge_ok_a  = (rise_mask == 0 AND fall_mask == 0)
//                OR ((rising & rise_mask) != 0)
//                OR ((falling & fall_mask) != 0)
//   trig_active_a = (mask != 0) OR (rise_mask != 0) OR (fall_mask != 0)
//   match_a    = trig_active_a AND level_ok_a AND edge_ok_a
//
// --- Group B (level only, used in OR mode) ---
//   level_ok_b    = (mask2 == 0) OR ((data_in & mask2) == (val2 & mask2))
//   trig_active_b = (mask2 != 0)
//   match_b       = trig_active_b AND level_ok_b
//
// --- Combining ---
//   or_mode = 0 (AND): fire = (A inactive OR match_a) AND
//                             (B inactive OR match_b) AND
//                             (A active  OR B active)
//             → both active groups must match simultaneously;
//               if only one group is active, behaves like a single-group check.
//   or_mode = 1 (OR):  fire = match_a OR match_b
//                      (match_x is already 0 when group x is inactive)
//   match <= valid_in AND fire
//
// When or_mode=0 and Group B is all-zero, behaviour is identical to the
// original level+edge implementation (100% backward compatible).

`default_nettype none

module ila_trigger #(
    parameter int DATA_W = 32
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire [DATA_W-1:0]    data_in,
    input  wire                 valid_in,
    // Group A: level + edge
    input  wire [DATA_W-1:0]    mask,
    input  wire [DATA_W-1:0]    value,
    input  wire [DATA_W-1:0]    rise_mask,
    input  wire [DATA_W-1:0]    fall_mask,
    // Group B: level only (for OR mode)
    input  wire [DATA_W-1:0]    mask2,
    input  wire [DATA_W-1:0]    val2,
    // OR mode: 0 = AND (Group A only), 1 = A OR B
    input  wire                 or_mode,
    output logic                match
);

    logic [DATA_W-1:0] prev_data;

    // --- Group A combinational ---
    logic [DATA_W-1:0] rising_w, falling_w;
    logic              level_ok_w, edge_ok_w;

    assign rising_w   = ~prev_data &  data_in;
    assign falling_w  =  prev_data & ~data_in;
    assign level_ok_w = (mask == '0) | ((data_in & mask) == (value & mask));
    assign edge_ok_w  = ((rise_mask == '0) & (fall_mask == '0))
                      | ((rising_w  & rise_mask) != '0)
                      | ((falling_w & fall_mask) != '0);

    wire trig_active_a = (mask != '0) | (rise_mask != '0) | (fall_mask != '0);
    wire match_a_w     = trig_active_a & level_ok_w & edge_ok_w;

    // --- Group B combinational (level only) ---
    wire level_ok_b    = (mask2 == '0) | ((data_in & mask2) == (val2 & mask2));
    wire trig_active_b = (mask2 != '0);
    wire match_b_w     = trig_active_b & level_ok_b;

    // --- Combined fire ---
    // AND mode (or_mode=0): all active groups must match simultaneously.
    //   fire = (!trig_active_a | match_a_w) & (!trig_active_b | match_b_w)
    //          & (trig_active_a | trig_active_b)
    //   Special cases:
    //   • Group B inactive (mask2=0): collapses to match_a_w  — backward compat
    //   • Neither active:             fires never              — trig_active guard
    // OR mode (or_mode=1): either group matching fires the trigger.
    //   fire = match_a_w | match_b_w
    //   (match_x is always 0 when its group is inactive, so no extra guard needed)
    wire fire_w = or_mode
                ? (match_a_w | match_b_w)
                : ((!trig_active_a | match_a_w) & (!trig_active_b | match_b_w)
                   & (trig_active_a | trig_active_b));

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            prev_data <= '0;
            match     <= 1'b0;
        end else begin
            // Update prev_data only on valid samples so edge detection
            // respects the valid-strobe gating.
            if (valid_in) prev_data <= data_in;
            match <= valid_in & fire_w;
        end
    end

endmodule

`default_nettype wire
