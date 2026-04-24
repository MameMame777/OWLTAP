// SPDX-License-Identifier: Apache-2.0
// Testbench top for ila_bscane2_top (BSCANE2 USER1 access port).
//
// The BSCANE2 behavioral model (bscane2_model.sv) replaces the Xilinx
// unisim primitive.  It reads JTAG signals from the bscane2_sim_pkg
// virtual interface, which is set from this file's initial block.
//
// JTAG chain seen by the driver:
//   [6-bit PL TAP IR]  USER1 (6'h02) activates the 37-bit DR frame path.
//   [37-bit DR]        {5-bit opcode, 32-bit data}

`timescale 1ns/1ps

module ila_bscane2_tb_top;

    import uvm_pkg::*;
    `include "uvm_macros.svh"

    import ila_bscane2_test_pkg::*;

    // ------------------------------------------------------------------
    // Clocks
    // ------------------------------------------------------------------
    logic tck        = 1'b0;
    logic sample_clk = 1'b0;
    logic sample_rst_n = 1'b0;

    // TCK 10 MHz (period 100 ns); sample_clk 125 MHz (period 8 ns, Zybo default)
    always #50  tck        = ~tck;
    always #4   sample_clk = ~sample_clk;

    // ------------------------------------------------------------------
    // JTAG interface
    // ------------------------------------------------------------------
    ila_jtag_if jtag_if (.tck(tck));

    // TDO is driven by dut.sim_tdo (BSCANE2 muxed output port).
    // jtag_if.tdo is a wire; direct port connection is the cleanest driver.

    // ------------------------------------------------------------------
    // 32-bit free-running counter (mimics ila_bringup_top.sv)
    // ------------------------------------------------------------------
    logic [31:0] counter   = '0;
    logic        data_valid = 1'b0;

    always_ff @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) begin
            counter    <= '0;
            data_valid <= 1'b0;
        end else begin
            counter    <= counter + 32'h1;
            data_valid <= 1'b1;
        end
    end

    // ------------------------------------------------------------------
    // DUT: ila_bscane2_top
    // (BSCANE2 inside it is replaced by bscane2_model.sv)
    // ------------------------------------------------------------------
    ila_bscane2_top #(
        .DATA_W    (32),
        .DEPTH     (1024),
        .ADDR_W    (10),
        .IDCODE_VAL(32'hA17A_0001)
    ) dut (
        .sample_clk   (sample_clk),
        .sample_rst_n (sample_rst_n),
        .data_in      (counter),
        .data_valid   (data_valid)
`ifdef SIMULATION
       ,.sim_tck   (jtag_if.tck)
       ,.sim_tms   (jtag_if.tms)
       ,.sim_tdi   (jtag_if.tdi)
       ,.sim_trst_n(jtag_if.trst_n)
       ,.sim_tdo   (jtag_if.tdo)
`endif
    );

    // ------------------------------------------------------------------
    // Inject virtual interface into UVM config_db.
    // ------------------------------------------------------------------
    initial begin
        uvm_config_db#(virtual ila_jtag_if)::set(
            null, "uvm_test_top.env.agent.*", "vif", jtag_if);
        run_test();
    end

    // ------------------------------------------------------------------
    // Hardware reset sequence (separate initial block per UVM 1.2 rules)
    // ------------------------------------------------------------------
    initial begin
        jtag_if.tms    = 1'b1;
        jtag_if.tdi    = 1'b0;
        jtag_if.trst_n = 1'b0;
        sample_rst_n   = 1'b0;
        #200;
        jtag_if.trst_n = 1'b1;
        sample_rst_n   = 1'b1;
    end

    // ------------------------------------------------------------------
    // Safety watchdog
    // ------------------------------------------------------------------
    initial begin
        #10_000_000ns;
        `uvm_fatal("WATCHDOG", "simulation exceeded 10 ms")
    end

endmodule
