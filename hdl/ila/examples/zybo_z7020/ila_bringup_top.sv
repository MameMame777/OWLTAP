// SPDX-License-Identifier: Apache-2.0
// Zybo Z7-20 ILA bring-up: 32-bit free-running counter captured via
// ila_bscane2_top (BSCANE2 USER1). No extra JTAG cable needed.
//
// Access path:
//   Zybo USB JTAG (FT2232H) -> FPGA PL TAP -> BSCANE2 USER1 -> ILA
//
// Suggested bring-up sequence in OwlTAP GUI:
//   1. Connect to FTDI device (auto-detects Zynq PL TAP).
//   2. Tools > Internal Logic Analyzer, select BSCANE2 backend.
//   3. Trigger mask = 0xFFFFFFFF, value = 0x00001000.
//   4. Arm -> counter reaches 0x1000 (~8 us at 125 MHz) -> Full.
//   5. Read Samples -> inspect waveform.
`timescale 1ns/1ps
`default_nettype none

module ila_bringup_top (
    input  wire        sysclk,   // K17 -- 125 MHz system clock
    output logic [3:0] led       // M14/M15/G14/D18 -- blink for activity
);

    // ------------------------------------------------------------------
    // Synchronous power-on reset (256-cycle hold)
    // ------------------------------------------------------------------
    logic [7:0] rst_cnt = 8'h00;
    logic       rst_n;

    always_ff @(posedge sysclk) begin
        if (!(&rst_cnt)) rst_cnt <= rst_cnt + 8'h01;
    end
    assign rst_n = &rst_cnt;

    // ------------------------------------------------------------------
    // 32-bit free-running counter -- the signal under test
    // ------------------------------------------------------------------
    logic [31:0] counter;

    always_ff @(posedge sysclk or negedge rst_n) begin
        if (!rst_n) counter <= 32'h0;
        else        counter <= counter + 32'h1;
    end

    // Upper bits blink LEDs so you can confirm the design is running
    assign led = counter[27:24];

    // ------------------------------------------------------------------
    // ILA instantiation
    // Trigger example: mask=0xFFFFFFFF, value=0x00001000
    //   => fires when counter == 0x1000 (approx 8.192 us after reset)
    // ------------------------------------------------------------------
    ila_bscane2_top #(
        .DATA_W    (32),
        .DEPTH     (1024),
        .ADDR_W    (10),
        .NUM_CH    (1),
        .IDCODE_VAL(32'hA17A_0001)
    ) u_ila (
        .sample_clk   (sysclk),
        .sample_rst_n (rst_n),
        .data_in      (counter),
        .data_valid   (1'b1)
    );

endmodule

`default_nettype wire
