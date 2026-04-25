// SPDX-License-Identifier: Apache-2.0
// OwlTAP Internal Logic Analyzer — IEEE 1149.1 TAP controller + IR/DR shifters
`timescale 1ns/1ps
//
// Implements the canonical 16-state JTAG TAP FSM, a 5-bit instruction
// register, and all data registers visible through the TAP (IDCODE, CTRL,
// STATUS, TRIG_MASK, TRIG_VAL, PRE_SAMPLES, READ_ADDR, READ_DATA, BYPASS).
//
// Clock: all state is synchronous to `tck` (posedge for FSM/IR/DR capture
// and update; tdo is driven from a negedge register as required by the
// standard).
//
// READ_DATA DR supports auto-increment: every Update-DR increments the
// internal read pointer by 1, so the host can stream the buffer by issuing
// repeated DR scans after a single IR selection.

`default_nettype none

module ila_tap #(
    parameter int DATA_W      = 32,
    parameter int DEPTH       = 1024,
    parameter int ADDR_W      = 10,
    parameter int NUM_CH      = 1,   // number of channels (reserved, always 1 for now)
    parameter logic [31:0] IDCODE_VAL = 32'hA17A_0001,
    // Signal definition ROM (Phase 2+).  Only indices [0:SIG_COUNT-1] are used.
    // SIG_HI[i]/SIG_LO[i] are inclusive 0-based bit indices; SIG_FMT[i]: 0=HEX 1=DEC 2=BIN.
    // SIG_COUNT must be in [1..15].
    parameter int         SIG_COUNT            = 1,
    parameter logic [7:0] SIG_HI  [0:14] = '{8'd31, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0, 8'd0},
    parameter logic [7:0] SIG_LO  [0:14] = '{default: 8'd0},
    parameter logic [3:0] SIG_FMT [0:14] = '{default: 4'd0}
) (
    // JTAG port
    input  wire                 tck,
    input  wire                 trst_n,    // optional; tied high if unused
    input  wire                 tms,
    input  wire                 tdi,
    output logic                tdo,
    output logic                tdo_oe,

    // Control-plane outputs (in tck domain)
    output logic                ctrl_arm,
    output logic                ctrl_stop,
    output logic                ctrl_reset,
    output logic                ctrl_force_trig,
    output logic [DATA_W-1:0]   trig_mask,
    output logic [DATA_W-1:0]   trig_value,
    output logic [DATA_W-1:0]   trig_rise_mask,
    output logic [DATA_W-1:0]   trig_fall_mask,
    output logic [DATA_W-1:0]   trig_mask2,
    output logic [DATA_W-1:0]   trig_val2,
    output logic                trig_or_mode,
    output logic [ADDR_W-1:0]   pre_samples,

    // Status inputs (in tck domain; pre-synchronized by ila_top)
    input  wire                 sts_armed,
    input  wire                 sts_triggered,
    input  wire                 sts_full,

    // Readback interface to BRAM (tck domain)
    output logic [ADDR_W-1:0]   rd_addr,
    input  wire  [DATA_W-1:0]   rd_data
);

    // ------------------------------------------------------------------
    // TAP FSM
    // ------------------------------------------------------------------
    typedef enum logic [3:0] {
        TEST_LOGIC_RESET = 4'h0,
        RUN_TEST_IDLE    = 4'h1,
        SELECT_DR_SCAN   = 4'h2,
        CAPTURE_DR       = 4'h3,
        SHIFT_DR         = 4'h4,
        EXIT1_DR         = 4'h5,
        PAUSE_DR         = 4'h6,
        EXIT2_DR         = 4'h7,
        UPDATE_DR        = 4'h8,
        SELECT_IR_SCAN   = 4'h9,
        CAPTURE_IR       = 4'hA,
        SHIFT_IR         = 4'hB,
        EXIT1_IR         = 4'hC,
        PAUSE_IR         = 4'hD,
        EXIT2_IR         = 4'hE,
        UPDATE_IR        = 4'hF
    } tap_state_t;

    tap_state_t state, next_state;

    always_comb begin
        unique case (state)
            TEST_LOGIC_RESET: next_state = tms ? TEST_LOGIC_RESET : RUN_TEST_IDLE;
            RUN_TEST_IDLE:    next_state = tms ? SELECT_DR_SCAN   : RUN_TEST_IDLE;
            SELECT_DR_SCAN:   next_state = tms ? SELECT_IR_SCAN   : CAPTURE_DR;
            CAPTURE_DR:       next_state = tms ? EXIT1_DR         : SHIFT_DR;
            SHIFT_DR:         next_state = tms ? EXIT1_DR         : SHIFT_DR;
            EXIT1_DR:         next_state = tms ? UPDATE_DR        : PAUSE_DR;
            PAUSE_DR:         next_state = tms ? EXIT2_DR         : PAUSE_DR;
            EXIT2_DR:         next_state = tms ? UPDATE_DR        : SHIFT_DR;
            UPDATE_DR:        next_state = tms ? SELECT_DR_SCAN   : RUN_TEST_IDLE;
            SELECT_IR_SCAN:   next_state = tms ? TEST_LOGIC_RESET : CAPTURE_IR;
            CAPTURE_IR:       next_state = tms ? EXIT1_IR         : SHIFT_IR;
            SHIFT_IR:         next_state = tms ? EXIT1_IR         : SHIFT_IR;
            EXIT1_IR:         next_state = tms ? UPDATE_IR        : PAUSE_IR;
            PAUSE_IR:         next_state = tms ? EXIT2_IR         : PAUSE_IR;
            EXIT2_IR:         next_state = tms ? UPDATE_IR        : SHIFT_IR;
            UPDATE_IR:        next_state = tms ? SELECT_DR_SCAN   : RUN_TEST_IDLE;
            default:          next_state = TEST_LOGIC_RESET;
        endcase
    end

    always_ff @(posedge tck or negedge trst_n) begin
        if (!trst_n) state <= TEST_LOGIC_RESET;
        else         state <= next_state;
    end

    // ------------------------------------------------------------------
    // Instruction register (IR_W=5)
    // ------------------------------------------------------------------
    localparam int IR_W = 5;

    localparam logic [IR_W-1:0] IR_IDCODE      = 5'h01;
    localparam logic [IR_W-1:0] IR_CONFIG      = 5'h02;
    localparam logic [IR_W-1:0] IR_SIG_DEF     = 5'h03;
    localparam logic [IR_W-1:0] IR_CTRL        = 5'h08;
    localparam logic [IR_W-1:0] IR_STATUS      = 5'h09;
    localparam logic [IR_W-1:0] IR_TRIG_MASK   = 5'h0A;
    localparam logic [IR_W-1:0] IR_TRIG_VAL    = 5'h0B;
    localparam logic [IR_W-1:0] IR_READ_ADDR   = 5'h0C;
    localparam logic [IR_W-1:0] IR_READ_DATA   = 5'h0D;
    localparam logic [IR_W-1:0] IR_PRE_SAMPLES  = 5'h0E;
    localparam logic [IR_W-1:0] IR_TRIG_RISE   = 5'h0F;
    localparam logic [IR_W-1:0] IR_TRIG_FALL   = 5'h10;
    localparam logic [IR_W-1:0] IR_TRIG_MASK2  = 5'h11;
    localparam logic [IR_W-1:0] IR_TRIG_VAL2   = 5'h12;
    localparam logic [IR_W-1:0] IR_TRIG_CTRL   = 5'h13;  // bit[0]=or_mode
    localparam logic [IR_W-1:0] IR_BYPASS      = 5'h1F;

    logic [IR_W-1:0] ir_shift;
    logic [IR_W-1:0] ir_latched;

    always_ff @(posedge tck or negedge trst_n) begin
        if (!trst_n) begin
            ir_shift   <= '0;
            ir_latched <= IR_IDCODE;
        end else begin
            unique case (state)
                TEST_LOGIC_RESET: ir_latched <= IR_IDCODE;
                CAPTURE_IR:       ir_shift   <= {{(IR_W-2){1'b0}}, 2'b01}; // status bits per 1149.1
                SHIFT_IR:         ir_shift   <= {tdi, ir_shift[IR_W-1:1]};
                UPDATE_IR:        ir_latched <= ir_shift;
                default:          /* hold */ ;
            endcase
        end
    end

    // SIG_DEF ROM index: resets to 0 on trst_n or any IR change (UPDATE_IR)
    // so the first DR scan always reads entry[0].  Increments on each
    // SIG_DEF UPDATE_DR so consecutive reads step through entries in order.
    logic [3:0] sig_def_idx;
    always_ff @(posedge tck or negedge trst_n) begin
        if (!trst_n)                                          sig_def_idx <= '0;
        else if (state == UPDATE_IR)                          sig_def_idx <= '0;
        else if (state == UPDATE_DR && ir_latched == IR_SIG_DEF)
            sig_def_idx <= (sig_def_idx < 4'(SIG_COUNT - 1)) ? sig_def_idx + 4'd1 : '0;
    end

    // ------------------------------------------------------------------
    // CONFIG register (read-only)
    //   [31:24] VERSION    = 8'h01
    //   [23:20] NUM_CH
    //   [19:16] SIG_COUNT  = number of signal-lane slices reported by SIG_DEF
    //   [15:10] DATA_W - 1 (6 bits, value range 1..64)
    //   [ 9: 8] RESERVED   = 2'h0
    //   [ 7: 0] ADDR_W     (DEPTH = 1 << ADDR_W)
    // ------------------------------------------------------------------
    // SIG_DEF word format (read-only, 32 bits):
    //   [31:28] fmt[3:0]  (0=HEX 1=DEC 2=BIN; future: SIGNED/TIME)
    //   [27:24] reserved  = 4'h0
    //   [23:16] hi[7:0]   (inclusive MSB index, 0-based)
    //   [15: 8] lo[7:0]   (inclusive LSB index, 0-based)
    //   [ 7: 0] name_idx  (0xFF = host auto-generates "data[hi:lo]")
    localparam logic [31:0] CONFIG_VAL = {
        8'h03,           // VERSION 3: OR-mode trigger (TRIG_MASK2 / TRIG_VAL2 / TRIG_CTRL)
        4'(NUM_CH),
        4'(SIG_COUNT),
        6'(DATA_W - 1),
        2'h0,
        8'(ADDR_W)
    };

    // ------------------------------------------------------------------
    // Data registers
    // ------------------------------------------------------------------
    logic                bypass_shift;
    logic [31:0]         idcode_shift;
    logic [31:0]         config_shift;
    logic [31:0]         sigdef_shift;
    logic [3:0]          ctrl_shift;      // {FORCE_TRIG, RESET, STOP, ARM}
    logic [7:0]          status_shift;
    logic [DATA_W-1:0]   mask_shift,  mask_reg;
    logic [DATA_W-1:0]   val_shift,   val_reg;
    logic [DATA_W-1:0]   rise_shift,  rise_reg;
    logic [DATA_W-1:0]   fall_shift,  fall_reg;
    logic [15:0]         pre_shift;
    logic [15:0]         pre_reg;
    logic [ADDR_W-1:0]   addr_shift, addr_reg;
    logic [DATA_W-1:0]   data_shift;
    logic [DATA_W-1:0]   mask2_shift, mask2_reg;
    logic [DATA_W-1:0]   val2_shift,  val2_reg;
    logic [DATA_W-1:0]   tctrl_shift;  // bit[0] = or_mode
    logic                or_mode_reg;

    // Control-plane pulses (asserted for one tck cycle after UPDATE_DR).
    logic ctrl_arm_r, ctrl_stop_r, ctrl_reset_r, ctrl_force_trig_r;
    assign ctrl_arm        = ctrl_arm_r;
    assign ctrl_stop       = ctrl_stop_r;
    assign ctrl_reset      = ctrl_reset_r;
    assign ctrl_force_trig = ctrl_force_trig_r;
    assign trig_mask       = mask_reg;
    assign trig_value      = val_reg;
    assign trig_rise_mask  = rise_reg;
    assign trig_fall_mask  = fall_reg;
    assign trig_mask2      = mask2_reg;
    assign trig_val2       = val2_reg;
    assign trig_or_mode    = or_mode_reg;
    assign pre_samples     = pre_reg[ADDR_W-1:0];
    assign rd_addr         = addr_reg;

    // Shift-in (LSB first) and capture/update
    always_ff @(posedge tck or negedge trst_n) begin
        if (!trst_n) begin
            bypass_shift      <= 1'b0;
            idcode_shift      <= IDCODE_VAL;
            config_shift      <= CONFIG_VAL;
            sigdef_shift      <= '0;
            ctrl_shift        <= '0;
            status_shift      <= '0;
            mask_shift        <= '0;
            mask_reg          <= '0;
            val_shift         <= '0;
            val_reg           <= '0;
            rise_shift        <= '0;
            rise_reg          <= '0;
            fall_shift        <= '0;
            fall_reg          <= '0;
            pre_shift         <= '0;
            pre_reg           <= 16'(DEPTH/4);
            mask2_shift       <= '0;
            mask2_reg         <= '0;
            val2_shift        <= '0;
            val2_reg          <= '0;
            tctrl_shift       <= '0;
            or_mode_reg       <= 1'b0;
            addr_shift        <= '0;
            addr_reg          <= '0;
            data_shift        <= '0;
            ctrl_arm_r        <= 1'b0;
            ctrl_stop_r       <= 1'b0;
            ctrl_reset_r      <= 1'b0;
            ctrl_force_trig_r <= 1'b0;
        end else begin
            // Default: clear single-cycle pulses
            ctrl_arm_r        <= 1'b0;
            ctrl_stop_r       <= 1'b0;
            ctrl_reset_r      <= 1'b0;
            ctrl_force_trig_r <= 1'b0;

            if (state == CAPTURE_DR) begin
                unique case (ir_latched)
                    IR_IDCODE:      idcode_shift <= IDCODE_VAL;
                    IR_CONFIG:      config_shift <= CONFIG_VAL;
                    IR_SIG_DEF:     sigdef_shift <= {SIG_FMT[sig_def_idx], 4'h0,
                                                     SIG_HI[sig_def_idx], SIG_LO[sig_def_idx], 8'hFF};
                    IR_STATUS:      status_shift <= {5'b0, sts_full,
                                                     sts_triggered, sts_armed};
                    IR_TRIG_MASK:   mask_shift   <= mask_reg;
                    IR_TRIG_VAL:    val_shift    <= val_reg;
                    IR_TRIG_RISE:   rise_shift   <= rise_reg;
                    IR_TRIG_FALL:   fall_shift   <= fall_reg;
                    IR_TRIG_MASK2:  mask2_shift  <= mask2_reg;
                    IR_TRIG_VAL2:   val2_shift   <= val2_reg;
                    IR_TRIG_CTRL:   tctrl_shift  <= {{(DATA_W-1){1'b0}}, or_mode_reg};
                    IR_PRE_SAMPLES: pre_shift    <= pre_reg;
                    IR_READ_ADDR:   addr_shift   <= addr_reg;
                    IR_READ_DATA:   data_shift   <= rd_data;
                    IR_CTRL:        ctrl_shift   <= '0;
                    IR_BYPASS:      bypass_shift <= 1'b0;
                    default:        bypass_shift <= 1'b0;
                endcase
            end else if (state == SHIFT_DR) begin
                unique case (ir_latched)
                    IR_IDCODE:      idcode_shift <= {tdi, idcode_shift[31:1]};
                    IR_CONFIG:      config_shift <= {tdi, config_shift[31:1]};
                    IR_SIG_DEF:     sigdef_shift <= {tdi, sigdef_shift[31:1]};
                    IR_STATUS:      status_shift <= {tdi, status_shift[7:1]};
                    IR_TRIG_MASK:   mask_shift   <= {tdi, mask_shift[DATA_W-1:1]};
                    IR_TRIG_VAL:    val_shift    <= {tdi, val_shift[DATA_W-1:1]};
                    IR_TRIG_RISE:   rise_shift   <= {tdi, rise_shift[DATA_W-1:1]};
                    IR_TRIG_FALL:   fall_shift   <= {tdi, fall_shift[DATA_W-1:1]};
                    IR_TRIG_MASK2:  mask2_shift  <= {tdi, mask2_shift[DATA_W-1:1]};
                    IR_TRIG_VAL2:   val2_shift   <= {tdi, val2_shift[DATA_W-1:1]};
                    IR_TRIG_CTRL:   tctrl_shift  <= {tdi, tctrl_shift[DATA_W-1:1]};
                    IR_PRE_SAMPLES: pre_shift    <= {tdi, pre_shift[15:1]};
                    IR_READ_ADDR:   addr_shift   <= {tdi, addr_shift[ADDR_W-1:1]};
                    IR_READ_DATA:   data_shift   <= {tdi, data_shift[DATA_W-1:1]};
                    IR_CTRL:        ctrl_shift   <= {tdi, ctrl_shift[3:1]};
                    IR_BYPASS:      bypass_shift <= tdi;
                    default:        bypass_shift <= tdi;
                endcase
            end else if (state == UPDATE_DR) begin
                unique case (ir_latched)
                    IR_CTRL: begin
                        ctrl_arm_r        <= ctrl_shift[0];
                        ctrl_stop_r       <= ctrl_shift[1];
                        ctrl_reset_r      <= ctrl_shift[2];
                        ctrl_force_trig_r <= ctrl_shift[3];
                    end
                    IR_TRIG_MASK:   mask_reg <= mask_shift;
                    IR_TRIG_VAL:    val_reg  <= val_shift;
                    IR_TRIG_RISE:   rise_reg <= rise_shift;
                    IR_TRIG_FALL:   fall_reg <= fall_shift;
                    IR_TRIG_MASK2:  mask2_reg    <= mask2_shift;
                    IR_TRIG_VAL2:   val2_reg     <= val2_shift;
                    IR_TRIG_CTRL:   or_mode_reg  <= tctrl_shift[0];
                    IR_PRE_SAMPLES: pre_reg  <= pre_shift;
                    IR_READ_ADDR:   addr_reg <= addr_shift;
                    IR_READ_DATA:   addr_reg <= addr_reg + 1'b1; // auto-inc
                    default: /* read-only */ ;
                endcase
            end
        end
    end

    // ------------------------------------------------------------------
    // TDO mux (drive on negedge per IEEE 1149.1)
    // ------------------------------------------------------------------
    logic tdo_d;
    logic tdo_oe_d;

    always_comb begin
        tdo_oe_d = (state == SHIFT_DR) || (state == SHIFT_IR);
        tdo_d    = 1'b0;

        if (state == SHIFT_IR) begin
            tdo_d = ir_shift[0];
        end else if (state == SHIFT_DR) begin
            unique case (ir_latched)
                IR_IDCODE:      tdo_d = idcode_shift[0];
                IR_CONFIG:      tdo_d = config_shift[0];
                IR_SIG_DEF:     tdo_d = sigdef_shift[0];
                IR_STATUS:      tdo_d = status_shift[0];
                IR_TRIG_MASK:   tdo_d = mask_shift[0];
                IR_TRIG_VAL:    tdo_d = val_shift[0];
                IR_TRIG_RISE:   tdo_d = rise_shift[0];
                IR_TRIG_FALL:   tdo_d = fall_shift[0];
                IR_TRIG_MASK2:  tdo_d = mask2_shift[0];
                IR_TRIG_VAL2:   tdo_d = val2_shift[0];
                IR_TRIG_CTRL:   tdo_d = tctrl_shift[0];
                IR_PRE_SAMPLES: tdo_d = pre_shift[0];
                IR_READ_ADDR:   tdo_d = addr_shift[0];
                IR_READ_DATA:   tdo_d = data_shift[0];
                IR_CTRL:        tdo_d = ctrl_shift[0];
                IR_BYPASS:      tdo_d = bypass_shift;
                default:        tdo_d = bypass_shift;
            endcase
        end
    end

    always_ff @(negedge tck or negedge trst_n) begin
        if (!trst_n) begin
            tdo    <= 1'b0;
            tdo_oe <= 1'b0;
        end else begin
            tdo    <= tdo_d;
            tdo_oe <= tdo_oe_d;
        end
    end

endmodule

`default_nettype wire
