// SPDX-License-Identifier: Apache-2.0
// OwlTAP Internal Logic Analyzer — Capture FSM
`timescale 1ns/1ps
//
// Controls the BRAM write pointer and captures the trigger point.
// States:
//   IDLE       — not capturing. `arm` transitions to ARMED.
//   ARMED      — capturing pre-trigger samples. Ring-buffer write, no stop.
//                `trig_match` or `force_trig` transitions to TRIGGERED.
//   TRIGGERED  — capturing post-trigger samples. Count until
//                (DEPTH - pre_samples) additional valid samples, then FULL.
//   FULL       — capture complete. Stays until `stop` or `reset` clears.
//
// All control inputs (`arm`, `stop`, `force_trig`, `reset`) are single-cycle
// pulses synchronous to `clk`.

`default_nettype none

module ila_capture_fsm #(
    parameter int DEPTH    = 1024,
    parameter int ADDR_W   = 10   // must satisfy (1 << ADDR_W) == DEPTH
) (
    input  wire                 clk,
    input  wire                 rst_n,

    // Control pulses
    input  wire                 arm,
    input  wire                 stop,
    input  wire                 reset_capture,
    input  wire                 force_trig,
    input  wire [ADDR_W-1:0]    pre_samples,

    // Sample path
    input  wire                 valid_in,
    input  wire                 trig_match,

    // Outputs
    output logic [ADDR_W-1:0]   write_addr,
    output logic                write_en,
    output logic [ADDR_W-1:0]   trigger_addr,
    output logic                armed,
    output logic                triggered,
    output logic                full
);

    typedef enum logic [1:0] {
        ST_IDLE      = 2'd0,
        ST_ARMED     = 2'd1,
        ST_TRIGGERED = 2'd2,
        ST_FULL      = 2'd3
    } state_t;

    state_t               state, next_state;
    logic [ADDR_W-1:0]    post_cnt;
    logic [ADDR_W:0]      post_target;   // DEPTH - pre_samples, up to DEPTH

    // Combinational outputs
    assign armed     = (state == ST_ARMED);
    assign triggered = (state == ST_TRIGGERED) || (state == ST_FULL);
    assign full      = (state == ST_FULL);
    assign write_en  = valid_in && (state == ST_ARMED || state == ST_TRIGGERED);

    // Post-trigger target = DEPTH - pre_samples
    always_comb begin
        post_target = (ADDR_W+1)'(DEPTH) - (ADDR_W+1)'(pre_samples);
    end

    // Next-state
    always_comb begin
        next_state = state;
        unique case (state)
            ST_IDLE: begin
                if (arm) next_state = ST_ARMED;
            end
            ST_ARMED: begin
                if (reset_capture || stop)          next_state = ST_IDLE;
                else if (valid_in && (trig_match || force_trig))
                                                    next_state = ST_TRIGGERED;
            end
            ST_TRIGGERED: begin
                if (reset_capture)                  next_state = ST_IDLE;
                else if (stop)                      next_state = ST_FULL;
                else if (valid_in && (post_cnt + 1'b1) >= post_target[ADDR_W-1:0])
                                                    next_state = ST_FULL;
            end
            ST_FULL: begin
                if (reset_capture || arm)           next_state = ST_IDLE;
            end
            default: next_state = ST_IDLE;
        endcase
    end

    // Registers
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state        <= ST_IDLE;
            write_addr   <= '0;
            trigger_addr <= '0;
            post_cnt     <= '0;
        end else begin
            state <= next_state;

            // Write pointer advances on every valid captured sample
            if (state == ST_IDLE && next_state == ST_ARMED) begin
                write_addr   <= '0;
                post_cnt     <= '0;
                trigger_addr <= '0;
            end else if (write_en) begin
                write_addr <= write_addr + 1'b1; // wraps naturally at 2^ADDR_W
            end

            // Capture trigger point on ARMED→TRIGGERED transition
            if (state == ST_ARMED && next_state == ST_TRIGGERED) begin
                trigger_addr <= write_addr;
            end

            // Post-trigger sample counter
            if (state == ST_TRIGGERED && write_en) begin
                post_cnt <= post_cnt + 1'b1;
            end
            if (next_state == ST_IDLE) begin
                post_cnt <= '0;
            end
        end
    end

endmodule

`default_nettype wire
