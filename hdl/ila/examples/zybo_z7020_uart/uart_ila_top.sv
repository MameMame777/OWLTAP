// SPDX-License-Identifier: Apache-2.0
// Zybo Z7-20: UART internal loopback captured with OwlTAP ILA.
//
// The TX core cycles through four bytes (0x55 / 0xAA / 0xF0 / 0x0F) at
// 115200 baud (CLK_HZ/1085 => ~1085 cycles/bit).  Its serial output is
// wired directly back to the RX core input -- no external UART pin is needed.
//
// ILA capture vector (DATA_W=32):
//   [0]     uart_line   - the TX->RX serial wire
//   [1]     tx_busy     - high throughout each TX frame
//   [2]     rx_valid    - single-cycle pulse when a byte is decoded
//   [10:3]  rx_data     - last received byte (stable while rx_valid=1)
//   [31:11] 0           - unused
//
// DATA_W must be 32: the BSCANE2 backend always sends 37-bit frames
// (5-bit opcode + 32-bit data), matching FRAME_W = DATA_W + 5 = 37.
//
// Suggested OwlTAP settings (ILA panel):
//   Backend : BSCANE2
//   Trigger : mask=0x0004, value=0x0004  -> arm on rx_valid rising edge
//   Protocol Decode: UART, rx_pin="data[0:0]", baud=115200, ns/sample=8
//
// At DEPTH=8192 (8 ns/sample => 65.5 us window) and 115200 baud
// (~86.8 us/frame), roughly 0.75 frames fit per capture.
// Reduce DEPTH=1024 to see multiple frames: 1024*8ns=8.19us < 1 frame;
// use DEPTH=65536 / ADDR_W=16 for a ~524 us window (~6 frames).
// BRAM usage: 8192 x 32 = 262144 bits ~ 8 BRAMs (well within XC7Z020's 140).
`timescale 1ns/1ps
`default_nettype none

module uart_ila_top (
    input  wire        sysclk,   // K17 -- 125 MHz system clock
    output logic [3:0] led       // M14/M15/G14/D18 -- shows last received nibble
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
    // UART signals
    // ------------------------------------------------------------------
    logic       uart_line;   // TX output wired directly to RX input
    logic       tx_busy;
    logic       tx_valid;
    logic [7:0] tx_byte;     // pre-loaded byte to transmit
    logic       rx_valid;
    logic [7:0] rx_data;

    // ------------------------------------------------------------------
    // UART TX / RX instances  (both at 1 Mbaud, 8N1)
    // ------------------------------------------------------------------
    uart_tx #(
        .CLK_HZ(125_000_000),
        .BAUD  (115_200)
    ) u_tx (
        .clk     (sysclk),
        .rst_n   (rst_n),
        .tx_valid(tx_valid),
        .tx_data (tx_byte),
        .tx_busy (tx_busy),
        .tx      (uart_line)
    );

    uart_rx #(
        .CLK_HZ(125_000_000),
        .BAUD  (115_200)
    ) u_rx (
        .clk     (sysclk),
        .rst_n   (rst_n),
        .rx      (uart_line),
        .rx_valid(rx_valid),
        .rx_data (rx_data)
    );

    // ------------------------------------------------------------------
    // TX sequencer: PREPARE -> FIRE -> GAP -> TXWAIT -> PREPARE ...
    //
    // PREPARE: wait for tx_busy=0, then latch next byte from ROM
    // FIRE   : assert tx_valid for exactly 1 cycle
    // GAP    : minimum inter-frame spacing (250 cycles ~= 2 us)
    // TXWAIT : wait for tx_busy to fall after the gap expires
    // ------------------------------------------------------------------
    localparam int GAP = 250;  // cycles between tx_valid pulses

    typedef enum logic [1:0] {
        SEQ_PREPARE = 2'd0,
        SEQ_FIRE    = 2'd1,
        SEQ_GAP     = 2'd2,
        SEQ_TXWAIT  = 2'd3
    } seq_state_t;

    seq_state_t seq_state;
    logic [1:0] seq_idx;       // which byte to send next (0..3)
    logic [7:0] gap_cnt;

    always_ff @(posedge sysclk or negedge rst_n) begin
        if (!rst_n) begin
            seq_state <= SEQ_PREPARE;
            seq_idx   <= 2'd0;
            gap_cnt   <= '0;
            tx_valid  <= 1'b0;
            tx_byte   <= 8'h55;
        end else begin
            tx_valid <= 1'b0;  // default: deasserted

            case (seq_state)
                SEQ_PREPARE: begin
                    // Wait for TX idle, then load the next byte
                    if (!tx_busy) begin
                        case (seq_idx)
                            2'd0: tx_byte <= 8'h55;
                            2'd1: tx_byte <= 8'hAA;
                            2'd2: tx_byte <= 8'hF0;
                            2'd3: tx_byte <= 8'h0F;
                        endcase
                        seq_state <= SEQ_FIRE;
                    end
                end

                SEQ_FIRE: begin
                    // tx_byte is now stable; assert tx_valid for one cycle
                    tx_valid  <= 1'b1;
                    seq_idx   <= seq_idx + 2'd1;  // advance index for next round
                    gap_cnt   <= '0;
                    seq_state <= SEQ_GAP;
                end

                SEQ_GAP: begin
                    // Minimum inter-frame gap (GAP cycles)
                    if (gap_cnt == GAP - 1) begin
                        gap_cnt   <= '0;
                        seq_state <= SEQ_TXWAIT;
                    end else begin
                        gap_cnt <= gap_cnt + 8'd1;
                    end
                end

                SEQ_TXWAIT: begin
                    // The gap is a minimum; wait for tx_busy to fall
                    // (at 1 Mbaud, a full frame takes 1250 cycles >> 250)
                    if (!tx_busy) begin
                        seq_state <= SEQ_PREPARE;
                    end
                end
            endcase
        end
    end

    // ------------------------------------------------------------------
    // LED: show lower nibble of last received byte; toggles on each rx_valid
    // ------------------------------------------------------------------
    always_ff @(posedge sysclk or negedge rst_n) begin
        if (!rst_n) led <= 4'b0001;
        else if (rx_valid) led <= rx_data[3:0];
    end

    // ------------------------------------------------------------------
    // ILA -- BSCANE2 USER1, DATA_W=32, DEPTH=8192
    //
    // Signal map (SIG_HI / SIG_LO are inclusive bit indices into data_in):
    //   sig[0] uart_line  -> [0:0]
    //   sig[1] tx_busy    -> [1:1]
    //   sig[2] rx_valid   -> [2:2]
    //   sig[3] rx_data    -> [10:3]
    // ------------------------------------------------------------------
    logic [31:0] ila_data;
    assign ila_data = {21'b0, rx_data, rx_valid, tx_busy, uart_line};

    ila_bscane2_top #(
        .JTAG_CHAIN(1),
        .DATA_W    (32),
        .DEPTH     (65536),   // 65536 * 8 ns = 524 us ~ 6 frames @ 115200 baud
        .ADDR_W    (16),
        .NUM_CH    (1),
        .IDCODE_VAL(32'hA17A_0003),
        .SIG_COUNT (4),
        .SIG_HI    ('{8'd0,  8'd1,  8'd2,  8'd10,
                      8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0}),
        .SIG_LO    ('{8'd0,  8'd1,  8'd2,  8'd3,
                      8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0}),
        .SIG_FMT   ('{default: 4'd0})
    ) u_ila (
        .sample_clk   (sysclk),
        .sample_rst_n (rst_n),
        .data_in      (ila_data),
        .data_valid   (1'b1)
    );

endmodule

`default_nettype wire
