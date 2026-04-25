// SPDX-License-Identifier: Apache-2.0
// UVM test package for ila_bscane2_top verification.
//
// Protocol differences from ila_test_pkg.sv (ila_top native TAP):
//
//  1. The JTAG IR is 6 bits wide; USER1 = 6'h02 activates the BSCANE2
//     DR scan chain.  All ILA accesses must be preceded by (or assume)
//     USER1 is already loaded.
//
//  2. Every DR scan through USER1 is exactly 37 bits:
//       bits  [4:0]  = 5-bit ILA sub-opcode  (same values as ila_tap.sv)
//       bits [36:5]  = 32-bit data payload
//
//  3. Reads require TWO consecutive 37-bit scans:
//       scan 1: TDI = {opcode, 32'h0}  -> UPDATE stores new opcode
//       scan 2: TDI = {opcode, 32'h0}  -> CAPTURE loads register into
//               shift register; TDO[36:5] contains the register value.
//
//  4. Writes need ONE scan:
//       scan 1: TDI = {opcode, data}   -> UPDATE writes register.
//
//  5. CTRL pulses (ARM/STOP/RESET/FORCE_TRIG) are write-only, one scan.

`timescale 1ns/1ps

package ila_bscane2_test_pkg;

    import uvm_pkg::*;
    `include "uvm_macros.svh"

    // ------------------------------------------------------------------
    // ILA constants (must match ila_bscane2_top parameters)
    // ------------------------------------------------------------------
    localparam int ILA_DATA_W = 32;
    localparam int ILA_ADDR_W = 10;
    localparam int ILA_DEPTH  = 1 << ILA_ADDR_W;
    localparam logic [31:0] ILA_IDCODE_VAL = 32'hA17A_0001;

    // 5-bit sub-opcodes (ILA registers inside BSCANE2 frame)
    localparam logic [4:0] IR_IDCODE      = 5'h01;
    localparam logic [4:0] IR_CONFIG      = 5'h02;
    localparam logic [4:0] IR_SIG_DEF     = 5'h03;  // Signal definition ROM
    localparam logic [4:0] IR_CTRL        = 5'h08;
    localparam logic [4:0] IR_STATUS      = 5'h09;
    localparam logic [4:0] IR_TRIG_MASK   = 5'h0A;
    localparam logic [4:0] IR_TRIG_VAL    = 5'h0B;
    localparam logic [4:0] IR_READ_ADDR   = 5'h0C;
    localparam logic [4:0] IR_READ_DATA   = 5'h0D;
    localparam logic [4:0] IR_PRE_SAMPLES = 5'h0E;
    localparam logic [4:0] IR_TRIG_RISE   = 5'h0F;  // VERSION 2: edge trigger
    localparam logic [4:0] IR_TRIG_FALL   = 5'h10;
    localparam logic [4:0] IR_TRIG_MASK2  = 5'h11;  // VERSION 3: OR-mode Group B
    localparam logic [4:0] IR_TRIG_VAL2   = 5'h12;
    localparam logic [4:0] IR_TRIG_CTRL   = 5'h13;

    // STATUS DR bit positions
    localparam int STATUS_ARMED     = 0;
    localparam int STATUS_TRIGGERED = 1;
    localparam int STATUS_FULL      = 2;

    // 6-bit PL TAP IR constants
    localparam int          BSCANE2_IR_W    = 6;
    localparam logic [5:0]  USER1_OPCODE    = 6'h02;

    // BSCANE2 DR frame width = 5-bit opcode + 32-bit data
    localparam int          BSCANE2_FRAME_W = 37;

    // ------------------------------------------------------------------
    // Reuse JTAG transaction + sequencer + driver from ila_test_pkg.
    // We re-declare them here so this package is self-contained.
    // ------------------------------------------------------------------
    typedef enum { OP_RESET, OP_RUNTEST, OP_SHIFT_IR, OP_SHIFT_DR } jtag_op_e;

    class jtag_xact extends uvm_sequence_item;
        rand jtag_op_e    op;
        rand int unsigned length;
        rand bit [1023:0] tdi_bits;
        bit      [1023:0] tdo_bits;
        int unsigned      runtest_cycles;

        `uvm_object_utils_begin(jtag_xact)
            `uvm_field_enum(jtag_op_e, op,       UVM_ALL_ON)
            `uvm_field_int (length,               UVM_ALL_ON)
            `uvm_field_int (tdi_bits,             UVM_ALL_ON)
            `uvm_field_int (tdo_bits,             UVM_ALL_ON | UVM_NOCOMPARE)
        `uvm_object_utils_end

        function new(string name = "jtag_xact"); super.new(name); endfunction
    endclass

    typedef uvm_sequencer #(jtag_xact) jtag_sequencer;

    // ------------------------------------------------------------------
    // JTAG driver (identical waveform behaviour to ila_test_pkg)
    // ------------------------------------------------------------------
    class jtag_driver extends uvm_driver #(jtag_xact);
        `uvm_component_utils(jtag_driver)
        virtual ila_jtag_if vif;

        function new(string name, uvm_component parent); super.new(name,parent); endfunction

        function void build_phase(uvm_phase phase);
            super.build_phase(phase);
            if (!uvm_config_db#(virtual ila_jtag_if)::get(this,"","vif",vif))
                `uvm_fatal("NOVIF","virtual interface not set")
        endfunction

        task run_phase(uvm_phase phase);
            if (!vif.trst_n) @(posedge vif.trst_n);
            @(posedge vif.tck);
            vif.tms <= 1'b1;
            vif.tdi <= 1'b0;
            forever begin
                jtag_xact t;
                seq_item_port.get_next_item(t);
                drive_xact(t);
                seq_item_port.item_done();
            end
        endtask

        task tck_tick(input bit tms_v, input bit tdi_v);
            @(posedge vif.tck);
            vif.tms <= tms_v;
            vif.tdi <= tdi_v;
        endtask

        task do_reset();
            repeat (5) tck_tick(1'b1, 1'b0);
            tck_tick(1'b0, 1'b0);
        endtask

        task do_runtest(input int unsigned n);
            repeat (n) tck_tick(1'b0, 1'b0);
        endtask

        // Generic IR shift (works for any IR width)
        task do_shift_ir(input int unsigned len, input bit [1023:0] tdi_data,
                         output bit [1023:0] tdo_data);
            int i;
            tdo_data = '0;
            tck_tick(1'b1, 1'b0); // RTI -> stay RTI; TMS<-1
            tck_tick(1'b1, 1'b0); // -> Select-DR; TMS<-1
            tck_tick(1'b0, 1'b0); // -> Select-IR; TMS<-0
            tck_tick(1'b0, 1'b0); // -> Capture-IR; TMS<-0
            tck_tick(1'b0, 1'b0); // -> Shift-IR
            for (i = 0; i < len; i++) begin
                bit last    = (i == len - 1);
                bit tdi_bit = tdi_data[i];
                @(negedge vif.tck); #1;
                tdo_data[i] = vif.tdo;
                tck_tick(last ? 1'b1 : 1'b0, tdi_bit);
            end
            tck_tick(1'b1, 1'b0); // Exit1-IR -> Update-IR
            tck_tick(1'b0, 1'b0); // -> RTI
        endtask

        task do_shift_dr(input int unsigned len, input bit [1023:0] tdi_data,
                         output bit [1023:0] tdo_data);
            int i;
            tdo_data = '0;
            tck_tick(1'b1, 1'b0); // RTI -> stay RTI; TMS<-1
            tck_tick(1'b0, 1'b0); // -> Select-DR; TMS<-0
            tck_tick(1'b0, 1'b0); // -> Capture-DR; TMS<-0
            tck_tick(1'b0, 1'b0); // -> Shift-DR (CAPTURE fires)
            for (i = 0; i < len; i++) begin
                bit last    = (i == len - 1);
                bit tdi_bit = tdi_data[i];
                @(negedge vif.tck); #1;
                tdo_data[i] = vif.tdo;
                tck_tick(last ? 1'b1 : 1'b0, tdi_bit);
            end
            tck_tick(1'b1, 1'b0); // Exit1-DR -> Update-DR
            tck_tick(1'b0, 1'b0); // -> RTI
        endtask

        task drive_xact(jtag_xact t);
            case (t.op)
                OP_RESET:    do_reset();
                OP_RUNTEST:  do_runtest(t.runtest_cycles);
                OP_SHIFT_IR: do_shift_ir(t.length, t.tdi_bits, t.tdo_bits);
                OP_SHIFT_DR: do_shift_dr(t.length, t.tdi_bits, t.tdo_bits);
            endcase
        endtask
    endclass

    // ------------------------------------------------------------------
    // Monitor (minimal)
    // ------------------------------------------------------------------
    class jtag_monitor extends uvm_monitor;
        `uvm_component_utils(jtag_monitor)
        virtual ila_jtag_if vif;
        uvm_analysis_port #(jtag_xact) ap;
        function new(string name, uvm_component parent);
            super.new(name,parent); ap = new("ap",this); endfunction
        function void build_phase(uvm_phase phase);
            super.build_phase(phase);
            void'(uvm_config_db#(virtual ila_jtag_if)::get(this,"","vif",vif));
        endfunction
        task run_phase(uvm_phase phase);
            forever @(posedge vif.tck);
        endtask
    endclass

    // ------------------------------------------------------------------
    // Agent / Env
    // ------------------------------------------------------------------
    class jtag_agent extends uvm_agent;
        `uvm_component_utils(jtag_agent)
        jtag_sequencer sequencer;
        jtag_driver    driver;
        jtag_monitor   monitor;
        function new(string name, uvm_component parent); super.new(name,parent); endfunction
        function void build_phase(uvm_phase phase);
            super.build_phase(phase);
            sequencer = jtag_sequencer::type_id::create("sequencer",this);
            driver    = jtag_driver   ::type_id::create("driver",   this);
            monitor   = jtag_monitor  ::type_id::create("monitor",  this);
        endfunction
        function void connect_phase(uvm_phase phase);
            driver.seq_item_port.connect(sequencer.seq_item_export);
        endfunction
    endclass

    class ila_bscane2_env extends uvm_env;
        `uvm_component_utils(ila_bscane2_env)
        jtag_agent agent;
        function new(string name, uvm_component parent); super.new(name,parent); endfunction
        function void build_phase(uvm_phase phase);
            super.build_phase(phase);
            agent = jtag_agent::type_id::create("agent",this);
        endfunction
    endclass

    // ------------------------------------------------------------------
    // Sequence helper: high-level BSCANE2 ILA operations
    // ------------------------------------------------------------------
    class ila_bscane2_seq extends uvm_sequence #(jtag_xact);
        `uvm_object_utils(ila_bscane2_seq)
        function new(string name = "ila_bscane2_seq"); super.new(name); endfunction

        // ---- low-level helpers ---------------------------------------
        task wait_reset_done();
            virtual ila_jtag_if vif;
            if (!uvm_config_db#(virtual ila_jtag_if)::get(null,
                    "uvm_test_top.env.agent.*","vif",vif))
                `uvm_fatal("NOVIF","wait_reset_done: vif not found")
            if (!vif.trst_n) @(posedge vif.trst_n);
            @(posedge vif.tck);
        endtask

        task reset_tap();
            jtag_xact t = jtag_xact::type_id::create("t");
            t.op = OP_RESET; start_item(t); finish_item(t);
        endtask

        task runtest(int unsigned cycles);
            jtag_xact t = jtag_xact::type_id::create("t");
            t.op = OP_RUNTEST; t.runtest_cycles = cycles;
            start_item(t); finish_item(t);
        endtask

        // Load USER1 into PL TAP IR (6-bit)
        task load_user1();
            jtag_xact t = jtag_xact::type_id::create("t");
            t.op        = OP_SHIFT_IR;
            t.length    = BSCANE2_IR_W;
            t.tdi_bits  = '0;
            t.tdi_bits[BSCANE2_IR_W-1:0] = USER1_OPCODE;
            start_item(t); finish_item(t);
        endtask

        // Send one 37-bit frame. Returns captured frame bits.
        // frame TDI layout: [4:0]=opcode, [36:5]=data (LSB first).
        task frame(
            input  logic [4:0]  opcode,
            input  logic [31:0] data_in,
            output bit   [1023:0] tdo_raw
        );
            jtag_xact t = jtag_xact::type_id::create("t");
            t.op     = OP_SHIFT_DR;
            t.length = BSCANE2_FRAME_W;
            t.tdi_bits = '0;
            t.tdi_bits[4:0]  = opcode;
            t.tdi_bits[36:5] = data_in;
            start_item(t); finish_item(t);
            tdo_raw = t.tdo_bits;
        endtask

        // ---- ILA read: two frames, extract tdo[36:5] from 2nd scan
        task ila_read(
            input  logic [4:0]  opcode,
            output logic [31:0] result
        );
            bit [1023:0] tdo;
            frame(opcode, '0, tdo);  // scan 1: UPDATE sets stored_ir
            frame(opcode, '0, tdo);  // scan 2: CAPTURE loads register
            result = tdo[36:5];
        endtask

        // ---- ILA write: one frame
        task ila_write(input logic [4:0] opcode, input logic [31:0] data);
            bit [1023:0] tdo;
            frame(opcode, data, tdo);
        endtask

        // ---- High-level ILA ops -------------------------------------
        task ila_read_idcode(output bit [31:0] idcode);
            logic [31:0] r;
            load_user1();
            ila_read(IR_IDCODE, r);
            idcode = r;
        endtask

        task ila_read_config(output bit [31:0] cfg);
            logic [31:0] r;
            ila_read(IR_CONFIG, r);
            cfg = r;
        endtask

        task ila_read_sig_def(output bit [31:0] word);
            logic [31:0] r;
            ila_read(IR_SIG_DEF, r);
            word = r;
        endtask

        task ila_read_status(output bit [7:0] status);
            logic [31:0] r;
            ila_read(IR_STATUS, r);
            status = r[7:0];
        endtask

        task ila_set_mask(logic [31:0] mask);
            ila_write(IR_TRIG_MASK, mask);
        endtask

        task ila_set_value(logic [31:0] val);
            ila_write(IR_TRIG_VAL, val);
        endtask

        task ila_set_pre_samples(logic [9:0] pre);
            ila_write(IR_PRE_SAMPLES, {22'h0, pre});
        endtask

        task ila_set_read_addr(logic [9:0] addr);
            ila_write(IR_READ_ADDR, {22'h0, addr});
        endtask

        // CTRL: one write scan.  Bits [3:0] = {force_trig, reset, stop, arm}.
        task ila_ctrl(bit arm, bit stop, bit reset, bit force_trig);
            ila_write(IR_CTRL, {28'h0, force_trig, reset, stop, arm});
        endtask

        task ila_read_samples(
            input  int unsigned            count,
            output bit [ILA_DATA_W-1:0]    samples[$]
        );
            logic [31:0] r;
            int i;
            samples = {};
            // Set IR_READ_DATA sub-opcode in stored_ir (takes 2 scans)
            ila_read(IR_READ_DATA, r);  // r is BRAM[current addr], addr auto-inc
            samples.push_back(r);
            for (i = 1; i < count; i++) begin
                bit [1023:0] tdo;
                // For subsequent reads, stored_ir is already IR_READ_DATA.
                // One scan: CAPTURE loads next sample, UPDATE auto-increments addr.
                frame(IR_READ_DATA, '0, tdo);
                samples.push_back(tdo[36:5]);
            end
        endtask

        // ---- Edge-trigger helpers (VERSION 2 only) ------------------
        task ila_set_rise_mask(logic [31:0] rise);
            ila_write(IR_TRIG_RISE, rise);
        endtask

        task ila_set_fall_mask(logic [31:0] fall);
            ila_write(IR_TRIG_FALL, fall);
        endtask

        // ---- OR-mode helpers (VERSION 3 only) -----------------------
        task ila_set_mask2(logic [31:0] mask);
            ila_write(IR_TRIG_MASK2, mask);
        endtask

        task ila_set_val2(logic [31:0] val);
            ila_write(IR_TRIG_VAL2, val);
        endtask

        // or_mode: 1 = OR mode, 0 = AND mode (default)
        task ila_set_trig_ctrl(bit or_mode);
            ila_write(IR_TRIG_CTRL, {31'h0, or_mode});
        endtask

        task ila_wait_full(int unsigned max_tries = 20000);
            bit [7:0] st;
            for (int unsigned i = 0; i < max_tries; i++) begin
                ila_read_status(st);
                if (st[STATUS_FULL]) return;
            end
            `uvm_error("ILA_TIMEOUT",
                $sformatf("status never went full after %0d polls", max_tries))
        endtask
    endclass

    // ------------------------------------------------------------------
    // Base test
    // ------------------------------------------------------------------
    class ila_bscane2_base_test extends uvm_test;
        `uvm_component_utils(ila_bscane2_base_test)
        ila_bscane2_env env;

        function new(string name, uvm_component parent); super.new(name,parent); endfunction

        function void build_phase(uvm_phase phase);
            super.build_phase(phase);
            env = ila_bscane2_env::type_id::create("env",this);
        endfunction

        task run_phase(uvm_phase phase);
            phase.raise_objection(this);
            body(phase);
            phase.drop_objection(this);
        endtask

        virtual task body(uvm_phase phase);
            `uvm_info("TEST","base body (override me)", UVM_LOW)
        endtask

        function void report_phase(uvm_phase phase);
            uvm_report_server rs = uvm_report_server::get_server();
            int unsigned errs  = rs.get_severity_count(UVM_ERROR);
            int unsigned fatal = rs.get_severity_count(UVM_FATAL);
            if (errs == 0 && fatal == 0)
                `uvm_info("RESULT","TEST PASSED", UVM_NONE)
            else
                `uvm_info("RESULT",
                    $sformatf("TEST FAILED (errors=%0d fatal=%0d)",errs,fatal),
                    UVM_NONE)
        endfunction
    endclass

    // ------------------------------------------------------------------
    // Test 1: IDCODE smoke
    //   Load USER1 -> read IDCODE via 37-bit frame -> verify 0xA17A_0001
    // ------------------------------------------------------------------
    class ila_bscane2_smoke_test extends ila_bscane2_base_test;
        `uvm_component_utils(ila_bscane2_smoke_test)
        function new(string name, uvm_component parent);
            super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            ila_bscane2_seq seq = ila_bscane2_seq::type_id::create("seq");
            bit [31:0] idcode;

            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.ila_read_idcode(idcode);

            if (idcode !== ILA_IDCODE_VAL)
                `uvm_error("IDCODE",
                    $sformatf("expected 0x%08h got 0x%08h", ILA_IDCODE_VAL, idcode))
            else
                `uvm_info("IDCODE",
                    $sformatf("OK 0x%08h", idcode), UVM_LOW)
        endtask
    endclass

    // ------------------------------------------------------------------
    // Test 1b: CONFIG register read via BSCANE2
    //   Read IR_CONFIG (5'h02) — expected default value reflects HDL
    //   parameters DATA_W, ADDR_W, NUM_CH.
    // ------------------------------------------------------------------
    class ila_bscane2_config_read_test extends ila_bscane2_base_test;
        `uvm_component_utils(ila_bscane2_config_read_test)
        function new(string name, uvm_component parent);
            super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            ila_bscane2_seq seq = ila_bscane2_seq::type_id::create("seq");
            bit [31:0] cfg;
            bit [7:0]  version;
            bit [3:0]  num_ch;
            bit [3:0]  sig_count;
            bit [5:0]  data_w_m1;
            bit [7:0]  addr_w;

            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.load_user1();
            seq.ila_read_config(cfg);

            version   = cfg[31:24];
            num_ch    = cfg[23:20];
            sig_count = cfg[19:16];
            data_w_m1 = cfg[15:10];
            addr_w    = cfg[ 7: 0];

            `uvm_info("CONFIG",
                $sformatf("raw=0x%08h version=%0d num_ch=%0d sig_count=%0d data_w=%0d addr_w=%0d",
                          cfg, version, num_ch, sig_count, data_w_m1+1, addr_w), UVM_LOW)

            if (version !== 8'h03)
                `uvm_error("CONFIG_VER",
                    $sformatf("expected version 0x03 got 0x%02h", version))
            if (num_ch !== 4'd1)
                `uvm_error("CONFIG_NUMCH",
                    $sformatf("expected num_ch=1 got %0d", num_ch))
            if ((data_w_m1 + 1) !== ILA_DATA_W)
                `uvm_error("CONFIG_DATAW",
                    $sformatf("expected DATA_W=%0d got %0d", ILA_DATA_W, data_w_m1+1))
            if (addr_w !== ILA_ADDR_W)
                `uvm_error("CONFIG_ADDRW",
                    $sformatf("expected ADDR_W=%0d got %0d", ILA_ADDR_W, addr_w))
            if (sig_count !== 4'd1)
                `uvm_error("CONFIG_SIGCOUNT",
                    $sformatf("expected sig_count=1 got %0d", sig_count))
        endtask
    endclass

    // ------------------------------------------------------------------
    // Test 2: trigger match + full capture readback
    //   Configure mask=0xFFFFFFFF, value=0x00002000 (counter reaches
    //   0x2000 = 8192 at 125 MHz sample_clk ~65 us; watchdog = 10 ms).
    // ------------------------------------------------------------------
    class ila_bscane2_trigger_test extends ila_bscane2_base_test;
        `uvm_component_utils(ila_bscane2_trigger_test)
        function new(string name, uvm_component parent);
            super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            ila_bscane2_seq seq = ila_bscane2_seq::type_id::create("seq");
            bit [ILA_DATA_W-1:0] samples[$];
            bit [7:0] st;

            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.load_user1();

            seq.ila_set_mask(32'hFFFF_FFFF);
            // Counter runs at 125 MHz; JTAG arm completes ~30 us later
            // (counter ≈ 3750). Use 0x2000 = 8192 so trigger fires ~65 us
            // after reset, well inside the 10 ms watchdog.
            seq.ila_set_value(32'h0000_2000);
            seq.ila_set_pre_samples(10'(ILA_DEPTH/4));
            seq.ila_ctrl(.arm(1),.stop(0),.reset(0),.force_trig(0));
            seq.ila_wait_full();
            seq.ila_read_status(st);

            if (!st[STATUS_TRIGGERED])
                `uvm_error("NO_TRIG","triggered bit not set after full")
            if (!st[STATUS_FULL])
                `uvm_error("NO_FULL","full bit not set")

            seq.ila_set_read_addr(10'h0);
            seq.ila_read_samples(ILA_DEPTH, samples);
            if (samples.size() != ILA_DEPTH)
                `uvm_error("SIZE",
                    $sformatf("expected %0d samples, got %0d",
                              ILA_DEPTH, samples.size()))
            else
                `uvm_info("ILA",
                    $sformatf("read %0d samples; first=0x%08h last=0x%08h",
                              samples.size(), samples[0],
                              samples[samples.size()-1]), UVM_LOW)
        endtask
    endclass

    // ------------------------------------------------------------------
    // Test 3: edge trigger — rising edge on data[8] (counter bit 8)
    //   Counter bit 8 toggles every 256 cycles.  With 125 MHz sample_clk
    //   the first rising edge on bit 8 occurs at cycle 256 (~2 us), well
    //   inside the 10 ms simulation watchdog.
    // ------------------------------------------------------------------
    class ila_bscane2_edge_trigger_test extends ila_bscane2_base_test;
        `uvm_component_utils(ila_bscane2_edge_trigger_test)
        function new(string name, uvm_component parent);
            super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            ila_bscane2_seq seq = ila_bscane2_seq::type_id::create("seq");
            bit [ILA_DATA_W-1:0] samples[$];
            bit [7:0] st;
            bit [31:0] cfg;
            bit [7:0]  version;

            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.load_user1();

            // Verify firmware version supports edge trigger.
            seq.ila_read_config(cfg);
            version = cfg[31:24];
            if (version < 8'h02) begin
                `uvm_error("EDGE_VER",
                    $sformatf("edge trigger requires version>=2, got 0x%02h", version))
                return;
            end

            // --- Sub-test A: rising edge on bit 8 (no level mask) ---
            `uvm_info("EDGE","Sub-test A: rise on data[8]", UVM_LOW)
            seq.ila_set_mask(32'h0);          // level mask disabled
            seq.ila_set_value(32'h0);
            seq.ila_set_rise_mask(32'h0000_0100);  // bit 8
            seq.ila_set_fall_mask(32'h0);          // fall disabled
            seq.ila_set_pre_samples(10'(ILA_DEPTH/8));
            seq.ila_ctrl(.arm(1),.stop(0),.reset(0),.force_trig(0));
            seq.ila_wait_full();
            seq.ila_read_status(st);

            if (!st[STATUS_TRIGGERED])
                `uvm_error("EDGE_A_NOTRIG","triggered bit not set after rising-edge arm")
            else
                `uvm_info("EDGE","Sub-test A PASSED: rising edge triggered correctly", UVM_LOW)

            // --- Sub-test B: falling edge on bit 8 ---
            `uvm_info("EDGE","Sub-test B: fall on data[8]", UVM_LOW)
            seq.ila_ctrl(.arm(0),.stop(0),.reset(1),.force_trig(0));  // reset
            seq.ila_set_rise_mask(32'h0);
            seq.ila_set_fall_mask(32'h0000_0100);  // bit 8 falling
            seq.ila_ctrl(.arm(1),.stop(0),.reset(0),.force_trig(0));
            seq.ila_wait_full();
            seq.ila_read_status(st);

            if (!st[STATUS_TRIGGERED])
                `uvm_error("EDGE_B_NOTRIG","triggered bit not set after falling-edge arm")
            else
                `uvm_info("EDGE","Sub-test B PASSED: falling edge triggered correctly", UVM_LOW)

            // --- Sub-test C: Either edge on bit 1 (fires every 2 cycles) ---
            `uvm_info("EDGE","Sub-test C: either edge on data[1]", UVM_LOW)
            seq.ila_ctrl(.arm(0),.stop(0),.reset(1),.force_trig(0));
            seq.ila_set_rise_mask(32'h0000_0002);  // bit 1
            seq.ila_set_fall_mask(32'h0000_0002);  // bit 1 — either edge
            seq.ila_ctrl(.arm(1),.stop(0),.reset(0),.force_trig(0));
            seq.ila_wait_full();
            seq.ila_read_status(st);

            if (!st[STATUS_TRIGGERED])
                `uvm_error("EDGE_C_NOTRIG",
                    "triggered bit not set for either-edge trigger on data[1]")
            else
                `uvm_info("EDGE","Sub-test C PASSED: either-edge triggered correctly", UVM_LOW)
        endtask
    endclass

    // ------------------------------------------------------------------
    // Test 4: OR-mode trigger
    //   Group A: mask=0xFF,   val=0x42  (data[7:0] == 0x42)
    //   Group B: mask2=0xFF,  val2=0x55 (data[7:0] == 0x55)
    //   Sub-test A: or_mode=1, counter reaches 0x42 → triggered
    //   Sub-test B: or_mode=1, reset, counter reaches 0x55 → triggered
    //   Sub-test C: or_mode=0 (AND), only data[7:0] matching BOTH simultaneously
    //               is impossible with a monotone counter → use force_trig to
    //               confirm AND mode does NOT fire on 0x42 or 0x55 alone, then
    //               force to confirm the capture path works.
    // ------------------------------------------------------------------
    class ila_bscane2_or_trigger_test extends ila_bscane2_base_test;
        `uvm_component_utils(ila_bscane2_or_trigger_test)
        function new(string name, uvm_component parent);
            super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            ila_bscane2_seq seq = ila_bscane2_seq::type_id::create("seq");
            bit [7:0] st;
            bit [31:0] cfg;
            bit [7:0]  version;

            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.load_user1();

            // Verify firmware version supports OR-mode trigger.
            seq.ila_read_config(cfg);
            version = cfg[31:24];
            if (version < 8'h03) begin
                `uvm_error("OR_VER",
                    $sformatf("OR-mode trigger requires version>=3, got 0x%02h", version))
                return;
            end

            // ---- Common register setup ----
            seq.ila_set_mask  (32'h0000_00FF);   // Group A: bit[7:0]
            seq.ila_set_value (32'h0000_0042);   // Group A: value 0x42
            seq.ila_set_rise_mask(32'h0);
            seq.ila_set_fall_mask(32'h0);
            seq.ila_set_mask2 (32'h0000_00FF);   // Group B: bit[7:0]
            seq.ila_set_val2  (32'h0000_0055);   // Group B: value 0x55
            seq.ila_set_pre_samples(10'(ILA_DEPTH/8));

            // ---- Sub-test A: OR mode, expect trigger at counter == 0x42 ----
            `uvm_info("OR_TRIG","Sub-test A: OR mode, Group A fires at 0x42", UVM_LOW)
            seq.ila_set_trig_ctrl(1'b1);   // or_mode = 1
            seq.ila_ctrl(.arm(1),.stop(0),.reset(0),.force_trig(0));
            seq.ila_wait_full();
            seq.ila_read_status(st);

            if (!st[STATUS_TRIGGERED])
                `uvm_error("OR_A_NOTRIG",
                    "Sub-test A: triggered bit not set (expected Group A to fire at 0x42)")
            else
                `uvm_info("OR_TRIG","Sub-test A PASSED: OR mode fired on Group A", UVM_LOW)

            // ---- Sub-test B: OR mode, expect trigger at counter == 0x55 ----
            // Reset and re-arm.  Because counter is free-running from reset, it
            // will pass 0x55 before 0x42 only when the counter wraps (256 cycles
            // after 0x42 if it has not been reset).  We reset the ILA capture
            // and re-arm; the counter itself is NOT reset so it continues from
            // where it was.  Group A at 0x42 would have already passed if
            // counter > 0x42; Group B at 0x55 will fire next.
            // Swap A/B values to ensure Group B fires first after a reset.
            `uvm_info("OR_TRIG","Sub-test B: OR mode, Group B fires at 0x55", UVM_LOW)
            seq.ila_ctrl(.arm(0),.stop(0),.reset(1),.force_trig(0));
            // Set Group A to an unreachable value (0xAA), Group B stays 0x55
            seq.ila_set_value (32'h0000_00AA);   // Group A: unreachable in 8-bit window
            seq.ila_set_trig_ctrl(1'b1);         // or_mode = 1 (keep)
            seq.ila_ctrl(.arm(1),.stop(0),.reset(0),.force_trig(0));
            seq.ila_wait_full();
            seq.ila_read_status(st);

            if (!st[STATUS_TRIGGERED])
                `uvm_error("OR_B_NOTRIG",
                    "Sub-test B: triggered bit not set (expected Group B to fire at 0x55)")
            else
                `uvm_info("OR_TRIG","Sub-test B PASSED: OR mode fired on Group B", UVM_LOW)

            // ---- Sub-test C: AND mode (or_mode=0), impossible condition ---
            // Group A: data[7:0]==0x42, Group B: data[7:0]==0x55  (AND = both)
            // A monotone 8-bit counter can never be both 0x42 and 0x55 simultaneously.
            // After arming we wait a bounded time and verify NOT triggered.
            // Then force-trigger to confirm the ILA capture path still works.
            `uvm_info("OR_TRIG","Sub-test C: AND mode, impossible condition — should NOT trigger", UVM_LOW)
            seq.ila_ctrl(.arm(0),.stop(0),.reset(1),.force_trig(0));
            seq.ila_set_value (32'h0000_0042);   // Group A: 0x42
            seq.ila_set_trig_ctrl(1'b0);         // or_mode = 0 (AND)
            seq.ila_ctrl(.arm(1),.stop(0),.reset(0),.force_trig(0));

            // Wait ~100 JTAG-status polls; the counter will have wrapped multiple times
            // but AND-mode should never fire because both conditions can't be true
            // at the same sample.  100 × ~5 us ≈ 500 us budget (well under 10 ms watchdog).
            begin
                int unsigned polls;
                for (polls = 0; polls < 100; polls++) begin
                    seq.ila_read_status(st);
                    if (st[STATUS_TRIGGERED]) break;
                end
                if (st[STATUS_TRIGGERED])
                    `uvm_error("OR_C_SPURIOUS",
                        "Sub-test C: AND mode spuriously triggered — logic error in RTL")
                else
                    `uvm_info("OR_TRIG",
                        "Sub-test C PASSED: AND mode correctly did NOT trigger", UVM_LOW)
            end

            // Force-trigger to confirm capture still works in AND mode.
            seq.ila_ctrl(.arm(0),.stop(0),.reset(0),.force_trig(1));
            seq.ila_wait_full(.max_tries(20000));
            seq.ila_read_status(st);
            if (!st[STATUS_FULL])
                `uvm_error("OR_C_FORCE","Sub-test C: force trigger did not produce full capture")
            else
                `uvm_info("OR_TRIG","Sub-test C: force trigger captured OK", UVM_LOW)
        endtask
    endclass

    // ------------------------------------------------------------------
    // Test 5: SIG_DEF ROM readback
    //   Verify CONFIG[19:16] = sig_count = 1 (default DUT params).
    //   Read SIG_DEF entry[0] — expected 0x001F_00FF:
    //     [31:28] fmt=0 (HEX), [27:24] rsvd=0,
    //     [23:16] hi=31, [15:8] lo=0, [7:0] name_idx=0xFF
    // ------------------------------------------------------------------
    class ila_bscane2_sig_def_test extends ila_bscane2_base_test;
        `uvm_component_utils(ila_bscane2_sig_def_test)
        function new(string name, uvm_component parent);
            super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            ila_bscane2_seq seq = ila_bscane2_seq::type_id::create("seq");
            bit [31:0] cfg;
            bit [3:0]  sig_count;
            bit [31:0] sig_def_word;

            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.load_user1();

            // Verify CONFIG sig_count field (bits [19:16])
            seq.ila_read_config(cfg);
            sig_count = cfg[19:16];
            `uvm_info("SIG_DEF",
                $sformatf("CONFIG sig_count=%0d", sig_count), UVM_LOW)
            if (sig_count !== 4'd1)
                `uvm_error("SIG_COUNT",
                    $sformatf("expected sig_count=1 got %0d", sig_count))

            // Read entry[0]: fmt=0, hi=31, lo=0, name_idx=0xFF => 0x001F_00FF
            seq.ila_read_sig_def(sig_def_word);
            `uvm_info("SIG_DEF",
                $sformatf("entry[0]=0x%08h", sig_def_word), UVM_LOW)
            if (sig_def_word !== 32'h001F_00FF)
                `uvm_error("SIG_DEF_VAL",
                    $sformatf("expected 0x001F00FF got 0x%08h", sig_def_word))
        endtask
    endclass

endpackage
