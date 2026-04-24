// SPDX-License-Identifier: Apache-2.0
// OwlTAP Internal Logic Analyzer — Dual-clock BRAM
`timescale 1ns/1ps
//
// Inferred simple dual-port BRAM:
//   - Write port @ sample_clk (synchronous).
//   - Read port  @ tck         (synchronous).
//
// Addresses are used as-is on each domain. Read/write pointers are
// transferred across clock domains by ila_top using Gray-coded
// synchronizers; this module simply stores data.

`default_nettype none

module ila_bram #(
    parameter int DATA_W = 32,
    parameter int DEPTH  = 1024,
    parameter int ADDR_W = 10
) (
    // Write port (sample clock)
    input  wire                 wr_clk,
    input  wire                 wr_en,
    input  wire [ADDR_W-1:0]    wr_addr,
    input  wire [DATA_W-1:0]    wr_data,

    // Read port (JTAG TCK)
    input  wire                 rd_clk,
    input  wire [ADDR_W-1:0]    rd_addr,
    output logic [DATA_W-1:0]   rd_data
);

    // synthesis translate_off
    initial begin
        if ((1 << ADDR_W) != DEPTH) begin
            $fatal(1, "ila_bram: DEPTH (%0d) must equal 2**ADDR_W (%0d)",
                   DEPTH, (1 << ADDR_W));
        end
    end
    // synthesis translate_on

    (* ram_style = "block" *) logic [DATA_W-1:0] mem [0:DEPTH-1];

    always_ff @(posedge wr_clk) begin
        if (wr_en) mem[wr_addr] <= wr_data;
    end

    always_ff @(posedge rd_clk) begin
        rd_data <= mem[rd_addr];
    end

endmodule

`default_nettype wire
