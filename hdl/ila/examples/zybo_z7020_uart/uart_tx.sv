// SPDX-License-Identifier: Apache-2.0
// 8N1 UART transmitter.
//
// Assert tx_valid for exactly one cycle while tx_busy is low to start a
// transmission.  tx_busy is held high from the START bit through the STOP
// bit and returns low in the same cycle as the last STOP bit completes.
// The serial output idles high (MARK state).
`timescale 1ns/1ps
`default_nettype none

module uart_tx #(
    parameter int CLK_HZ = 125_000_000,
    parameter int BAUD   = 1_000_000
) (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       tx_valid,   // one-cycle strobe; ignored when tx_busy=1
    input  wire [7:0] tx_data,
    output logic      tx_busy,
    output logic      tx          // serial output; idle = 1
);
    localparam int DIV = CLK_HZ / BAUD;  // clock cycles per bit

    typedef enum logic [1:0] {
        IDLE  = 2'd0,
        START = 2'd1,
        DATA  = 2'd2,
        STOP  = 2'd3
    } state_t;

    state_t                  state;
    logic [$clog2(DIV)-1:0]  baud_cnt;
    logic [2:0]               bit_idx;
    logic [7:0]               shift_reg;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state     <= IDLE;
            baud_cnt  <= '0;
            bit_idx   <= '0;
            shift_reg <= '0;
            tx        <= 1'b1;
            tx_busy   <= 1'b0;
        end else begin
            case (state)
                IDLE: begin
                    tx      <= 1'b1;
                    tx_busy <= 1'b0;
                    if (tx_valid) begin
                        shift_reg <= tx_data;
                        baud_cnt  <= '0;
                        tx_busy   <= 1'b1;
                        state     <= START;
                    end
                end

                START: begin
                    tx <= 1'b0;  // start bit (SPACE)
                    if (baud_cnt == DIV - 1) begin
                        baud_cnt <= '0;
                        bit_idx  <= '0;
                        state    <= DATA;
                    end else begin
                        baud_cnt <= baud_cnt + 1;
                    end
                end

                DATA: begin
                    tx <= shift_reg[bit_idx];  // LSB first
                    if (baud_cnt == DIV - 1) begin
                        baud_cnt <= '0;
                        if (bit_idx == 3'd7) begin
                            state <= STOP;
                        end else begin
                            bit_idx <= bit_idx + 3'd1;
                        end
                    end else begin
                        baud_cnt <= baud_cnt + 1;
                    end
                end

                STOP: begin
                    tx <= 1'b1;  // stop bit (MARK)
                    if (baud_cnt == DIV - 1) begin
                        baud_cnt <= '0;
                        tx_busy  <= 1'b0;
                        state    <= IDLE;
                    end else begin
                        baud_cnt <= baud_cnt + 1;
                    end
                end
            endcase
        end
    end

endmodule

`default_nettype wire
