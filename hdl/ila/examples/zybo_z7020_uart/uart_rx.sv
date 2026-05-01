// SPDX-License-Identifier: Apache-2.0
// 8N1 UART receiver.
//
// Detects the falling edge of the start bit, waits half a bit period to
// reach the center of the start bit (and confirms it is still low), then
// samples each subsequent data bit at its midpoint.
//
// A two-flop synchronizer is applied to the rx input to prevent metastability.
// rx_valid is a single-cycle pulse; rx_data is stable while rx_valid is high.
`timescale 1ns/1ps
`default_nettype none

module uart_rx #(
    parameter int CLK_HZ = 125_000_000,
    parameter int BAUD   = 1_000_000
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        rx,
    output logic       rx_valid,
    output logic [7:0] rx_data
);
    localparam int DIV      = CLK_HZ / BAUD;   // cycles per bit
    localparam int HALF_DIV = DIV / 2;          // cycles to bit midpoint

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

    // Two-flop synchronizer to prevent metastability on rx input
    logic rx_meta, rx_s;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_meta <= 1'b1;
            rx_s    <= 1'b1;
        end else begin
            rx_meta <= rx;
            rx_s    <= rx_meta;
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state     <= IDLE;
            baud_cnt  <= '0;
            bit_idx   <= '0;
            shift_reg <= '0;
            rx_valid  <= 1'b0;
            rx_data   <= '0;
        end else begin
            rx_valid <= 1'b0;  // default: deasserted

            case (state)
                IDLE: begin
                    if (!rx_s) begin  // falling edge of start bit
                        baud_cnt <= '0;
                        state    <= START;
                    end
                end

                START: begin
                    // Advance to the midpoint of the start bit
                    if (baud_cnt == HALF_DIV - 1) begin
                        if (!rx_s) begin  // still low -> genuine start bit
                            baud_cnt <= '0;
                            bit_idx  <= '0;
                            state    <= DATA;
                        end else begin
                            state <= IDLE;  // false start, discard
                        end
                    end else begin
                        baud_cnt <= baud_cnt + 1;
                    end
                end

                DATA: begin
                    // Having entered at the midpoint of the start bit, each
                    // DIV-cycle count advances to the midpoint of the next bit.
                    if (baud_cnt == DIV - 1) begin
                        baud_cnt           <= '0;
                        shift_reg[bit_idx] <= rx_s;  // sample at midpoint
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
                    if (baud_cnt == DIV - 1) begin
                        baud_cnt <= '0;
                        state    <= IDLE;
                        if (rx_s) begin       // valid stop bit (MARK)
                            rx_valid <= 1'b1;
                            rx_data  <= shift_reg;
                        end
                        // framing error (rx_s=0) silently discarded
                    end else begin
                        baud_cnt <= baud_cnt + 1;
                    end
                end
            endcase
        end
    end

endmodule

`default_nettype wire
