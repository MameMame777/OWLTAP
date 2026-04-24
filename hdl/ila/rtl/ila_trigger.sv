// SPDX-License-Identifier: Apache-2.0
// OwlTAP Internal Logic Analyzer — Trigger comparator
`timescale 1ns/1ps
//
// Single-match trigger: asserts `match` when (data_in & mask) == value.
// One-cycle latency; `match` is registered.

`default_nettype none

module ila_trigger #(
    parameter int DATA_W = 32
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire [DATA_W-1:0]    data_in,
    input  wire                 valid_in,
    input  wire [DATA_W-1:0]    mask,
    input  wire [DATA_W-1:0]    value,
    output logic                match
);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            match <= 1'b0;
        end else begin
            match <= valid_in && ((data_in & mask) == (value & mask));
        end
    end

endmodule

`default_nettype wire
