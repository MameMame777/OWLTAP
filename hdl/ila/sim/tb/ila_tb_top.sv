// SPDX-License-Identifier: Apache-2.0
// OwlTAP ILA UVM testbench top

`timescale 1ns/1ps

module ila_tb_top;

    import uvm_pkg::*;
    `include "uvm_macros.svh"

    import ila_test_pkg::*;

    // ---------- Clocks & resets ----------
    logic tck = 0;
    logic sample_clk = 0;
    logic sample_rst_n = 0;

    // TCK at 10 MHz (period 100 ns); sample_clk at 100 MHz
    always #50  tck        = ~tck;
    always #5   sample_clk = ~sample_clk;

    // ---------- Interface ----------
    ila_jtag_if jtag_if (.tck(tck));

    // ---------- Counter as stimulus ----------
    logic [31:0] counter = '0;
    logic        data_valid = 1'b0;

    always_ff @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) begin
            counter    <= '0;
            data_valid <= 1'b0;
        end else begin
            counter    <= counter + 1;
            data_valid <= 1'b1;
        end
    end

    // ---------- DUT ----------
    ila_top #(
        .DATA_W(32), .DEPTH(1024), .ADDR_W(10),
        .IDCODE_VAL(32'hA17A_0001)
    ) dut (
        .tck          (jtag_if.tck),
        .trst_n       (jtag_if.trst_n),
        .tms          (jtag_if.tms),
        .tdi          (jtag_if.tdi),
        .tdo          (jtag_if.tdo),
        .sample_clk   (sample_clk),
        .sample_rst_n (sample_rst_n),
        .data_in      (counter),
        .data_valid   (data_valid)
    );

    // ---------- Reset & UVM run ----------
    // uvm_config_db and run_test() MUST be called at time 0 (UVM 1.2 requirement).
    // Hardware reset is released after 200 ns from a separate initial block;
    // sequences must wait for trst_n/sample_rst_n before driving the DUT.
    initial begin
        uvm_config_db#(virtual ila_jtag_if)::set(null, "uvm_test_top.env.agent.*",
                                                 "vif", jtag_if);
        run_test();
    end

    initial begin
        jtag_if.tms    = 1'b1;
        jtag_if.tdi    = 1'b0;
        jtag_if.trst_n = 1'b0;
        sample_rst_n   = 1'b0;
        #200;
        jtag_if.trst_n = 1'b1;
        sample_rst_n   = 1'b1;
    end

    // Safety watchdog
    initial begin
        #5_000_000ns;
        `uvm_fatal("WATCHDOG", "simulation exceeded 5 ms")
    end

endmodule
