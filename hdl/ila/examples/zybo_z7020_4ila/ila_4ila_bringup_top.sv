// SPDX-License-Identifier: Apache-2.0
// Zybo Z7-20 bring-up: four ila_bscane2_top instances on USER1..USER4.
//
// Each ILA captures the same 32-bit free-running counter but with a
// distinct JTAG_CHAIN value (1..4) and IDCODE so that OwlTAP can
// address them independently via BscaneIlaTapBackend(chain, idx, user_chain).
//
// Access path (all four share the same USB JTAG cable):
//   Zybo USB JTAG (FT2232H) -> FPGA PL TAP
//     -> BSCANE2 USER1 -> u_ila0
//     -> BSCANE2 USER2 -> u_ila1
//     -> BSCANE2 USER3 -> u_ila2
//     -> BSCANE2 USER4 -> u_ila3
//
// IDCODE values (embed chain index in byte[0] for easy identification):
//   u_ila0: 0xA17A_0001  (USER1)
//   u_ila1: 0xA17A_0002  (USER2)
//   u_ila2: 0xA17A_0003  (USER3)
//   u_ila3: 0xA17A_0004  (USER4)
//
// Suggested bring-up sequence in OwlTAP (repeat for each user_chain 1..4):
//   1. Connect to FTDI device (auto-detects Zynq PL TAP).
//   2. Tools > Internal Logic Analyzer, select BSCANE2 backend, user_chain=N.
//   3. Trigger mask = 0xFFFFFFFF, value = 0x00001000.
//   4. Arm -> counter reaches 0x1000 -> Full.
//   5. Read Samples -> inspect waveform.
`timescale 1ns/1ps
`default_nettype none

module ila_4ila_bringup_top (
    input  wire        sysclk,   // K17 -- 125 MHz
    output logic [3:0] led       // M14/M15/G14/D18
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

    assign led = counter[27:24];

    // ------------------------------------------------------------------
    // ILA 0 -- BSCANE2 USER1, DEPTH=256
    // ------------------------------------------------------------------
    ila_bscane2_top #(
        .JTAG_CHAIN(1),
        .DATA_W    (32),
        .DEPTH     (256),
        .ADDR_W    (8),
        .NUM_CH    (1),
        .IDCODE_VAL(32'hA17A_0001),
        .SIG_COUNT (1),
        .SIG_HI    ('{8'd31, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0}),
        .SIG_LO    ('{default: 8'd0}),
        .SIG_FMT   ('{default: 4'd0})
    ) u_ila0 (
        .sample_clk   (sysclk),
        .sample_rst_n (rst_n),
        .data_in      (counter),
        .data_valid   (1'b1)
    );

    // ------------------------------------------------------------------
    // ILA 1 -- BSCANE2 USER2, DEPTH=512
    // ------------------------------------------------------------------
    ila_bscane2_top #(
        .JTAG_CHAIN(2),
        .DATA_W    (32),
        .DEPTH     (512),
        .ADDR_W    (9),
        .NUM_CH    (1),
        .IDCODE_VAL(32'hA17A_0002),
        .SIG_COUNT (2),
        .SIG_HI    ('{8'd31, 8'd15, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0}),
        .SIG_LO    ('{default: 8'd0}),
        .SIG_FMT   ('{default: 4'd0})
    ) u_ila1 (
        .sample_clk   (sysclk),
        .sample_rst_n (rst_n),
        .data_in      (counter),
        .data_valid   (1'b1)
    );

    // ------------------------------------------------------------------
    // ILA 2 -- BSCANE2 USER3, DEPTH=1024
    // ------------------------------------------------------------------
    ila_bscane2_top #(
        .JTAG_CHAIN(3),
        .DATA_W    (32),
        .DEPTH     (1024),
        .ADDR_W    (10),
        .NUM_CH    (1),
        .IDCODE_VAL(32'hA17A_0003),
        .SIG_COUNT (3),
        .SIG_HI    ('{8'd31, 8'd15, 8'd7, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0}),
        .SIG_LO    ('{default: 8'd0}),
        .SIG_FMT   ('{default: 4'd0})
    ) u_ila2 (
        .sample_clk   (sysclk),
        .sample_rst_n (rst_n),
        .data_in      (counter),
        .data_valid   (1'b1)
    );

    // ------------------------------------------------------------------
    // ILA 3 -- BSCANE2 USER4, DEPTH=2048
    // ------------------------------------------------------------------
    ila_bscane2_top #(
        .JTAG_CHAIN(4),
        .DATA_W    (32),
        .DEPTH     (2048),
        .ADDR_W    (11),
        .NUM_CH    (1),
        .IDCODE_VAL(32'hA17A_0004),
        .SIG_COUNT (4),
        .SIG_HI    ('{8'd31, 8'd15, 8'd7, 8'd3, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0}),
        .SIG_LO    ('{default: 8'd0}),
        .SIG_FMT   ('{default: 4'd0})
    ) u_ila3 (
        .sample_clk   (sysclk),
        .sample_rst_n (rst_n),
        .data_in      (counter),
        .data_valid   (1'b1)
    );

endmodule

`default_nettype wire
