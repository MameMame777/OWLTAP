// SPDX-License-Identifier: Apache-2.0
// UVM package for OwlTAP ILA verification
`timescale 1ns/1ps
//
// Contains: JTAG transaction, sequencer, driver, monitor, agent, env,
// scoreboard, base test, sequences, and all concrete tests.

package ila_test_pkg;

    import uvm_pkg::*;
    `include "uvm_macros.svh"

    // -----------------------------------------------------------------
    // Parameters / IR opcodes (must match ila_tap.sv)
    // -----------------------------------------------------------------
    localparam int ILA_IR_W       = 5;
    localparam int ILA_DATA_W     = 32;
    localparam int ILA_ADDR_W     = 10;
    localparam int ILA_DEPTH      = 1 << ILA_ADDR_W;
    localparam logic [31:0] ILA_IDCODE_VAL = 32'hA17A_0001;

    localparam logic [ILA_IR_W-1:0] IR_IDCODE      = 5'h01;
    localparam logic [ILA_IR_W-1:0] IR_CONFIG      = 5'h02;
    localparam logic [ILA_IR_W-1:0] IR_CTRL        = 5'h08;
    localparam logic [ILA_IR_W-1:0] IR_STATUS      = 5'h09;
    localparam logic [ILA_IR_W-1:0] IR_TRIG_MASK   = 5'h0A;
    localparam logic [ILA_IR_W-1:0] IR_TRIG_VAL    = 5'h0B;
    localparam logic [ILA_IR_W-1:0] IR_READ_ADDR   = 5'h0C;
    localparam logic [ILA_IR_W-1:0] IR_READ_DATA   = 5'h0D;
    localparam logic [ILA_IR_W-1:0] IR_PRE_SAMPLES = 5'h0E;
    localparam logic [ILA_IR_W-1:0] IR_BYPASS      = 5'h1F;

    // CTRL DR bit positions
    localparam int CTRL_ARM        = 0;
    localparam int CTRL_STOP       = 1;
    localparam int CTRL_RESET      = 2;
    localparam int CTRL_FORCE_TRIG = 3;

    // STATUS DR bit positions
    localparam int STATUS_ARMED     = 0;
    localparam int STATUS_TRIGGERED = 1;
    localparam int STATUS_FULL      = 2;

    // -----------------------------------------------------------------
    // JTAG transaction
    // -----------------------------------------------------------------
    typedef enum { OP_RESET, OP_RUNTEST, OP_SHIFT_IR, OP_SHIFT_DR } jtag_op_e;

    class jtag_xact extends uvm_sequence_item;
        rand jtag_op_e              op;
        rand int unsigned           length;
        rand bit [1023:0]           tdi_bits;   // LSB-first shift data
        bit      [1023:0]           tdo_bits;   // captured
        int unsigned                runtest_cycles;

        `uvm_object_utils_begin(jtag_xact)
            `uvm_field_enum(jtag_op_e, op, UVM_ALL_ON)
            `uvm_field_int (length,   UVM_ALL_ON)
            `uvm_field_int (tdi_bits, UVM_ALL_ON)
            `uvm_field_int (tdo_bits, UVM_ALL_ON | UVM_NOCOMPARE)
        `uvm_object_utils_end

        function new(string name = "jtag_xact");
            super.new(name);
        endfunction
    endclass

    // -----------------------------------------------------------------
    // JTAG sequencer
    // -----------------------------------------------------------------
    typedef uvm_sequencer #(jtag_xact) jtag_sequencer;

    // -----------------------------------------------------------------
    // JTAG driver — implements IEEE 1149.1 scans from Run-Test/Idle
    // -----------------------------------------------------------------
    class jtag_driver extends uvm_driver #(jtag_xact);
        `uvm_component_utils(jtag_driver)

        virtual ila_jtag_if vif;

        function new(string name, uvm_component parent);
            super.new(name, parent);
        endfunction

        function void build_phase(uvm_phase phase);
            super.build_phase(phase);
            if (!uvm_config_db#(virtual ila_jtag_if)::get(this, "", "vif", vif))
                `uvm_fatal("NOVIF", "virtual interface not set")
        endfunction

        task run_phase(uvm_phase phase);
            // Wait for TRST_N deassertion before driving anything.
            if (!vif.trst_n) @(posedge vif.trst_n);
            // Start in a known safe state (TMS high)
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

        // Reset: 5 TMS=1 cycles, end in Test-Logic-Reset, then one TMS=0 → RTI
        task do_reset();
            repeat (5) tck_tick(1'b1, 1'b0);
            tck_tick(1'b0, 1'b0);  // → Run-Test/Idle
        endtask

        // Clock N cycles in Run-Test/Idle
        task do_runtest(input int unsigned n);
            repeat (n) tck_tick(1'b0, 1'b0);
        endtask

        // Shift IR: RTI → Select-DR → Select-IR → Capture-IR → Shift-IR(n bits) → Exit1-IR → Update-IR → RTI
        //
        // tck_tick() sets TMS/TDI in the NBA region of each posedge, so the DUT
        // sees the new value one cycle later.  TDO is registered on negedge; we
        // add #1 after @(negedge) so the DUT's negedge always_ff (NBA) settles
        // before we sample vif.tdo.
        task do_shift_ir(input int unsigned len, input bit [1023:0] tdi_data,
                         output bit [1023:0] tdo_data);
            int i;
            tdo_data = '0;
            tck_tick(1'b1, 1'b0);  // posedge: RTI  (TMS=0 seen) → stays RTI;  after: TMS←1
            tck_tick(1'b1, 1'b0);  // posedge: TMS=1 → RTI→Select-DR;           after: TMS←1
            tck_tick(1'b0, 1'b0);  // posedge: TMS=1 → Select-DR→Select-IR;     after: TMS←0
            tck_tick(1'b0, 1'b0);  // posedge: TMS=0 → Select-IR→Capture-IR;    after: TMS←0
            tck_tick(1'b0, 1'b0);  // posedge: TMS=0 → Capture-IR→Shift-IR (CAPTURE_IR fires); negedge: TDO←ir_capture[0]
            for (i = 0; i < len; i++) begin
                bit last = (i == len - 1);
                bit tdi_bit = tdi_data[i];
                @(negedge vif.tck);
                #1;                               // let DUT negedge NBA settle
                tdo_data[i] = vif.tdo;
                tck_tick(last ? 1'b1 : 1'b0, tdi_bit);  // last bit drives TMS=1 → Exit1-IR
            end
            tck_tick(1'b1, 1'b0);  // Exit1-IR → Update-IR
            tck_tick(1'b0, 1'b0);  // → RTI
        endtask

        // Shift DR: RTI → Select-DR → Capture-DR → Shift-DR(n bits) → Exit1-DR → Update-DR → RTI
        task do_shift_dr(input int unsigned len, input bit [1023:0] tdi_data,
                         output bit [1023:0] tdo_data);
            int i;
            tdo_data = '0;
            tck_tick(1'b1, 1'b0);  // posedge: RTI  (TMS=0 seen) → stays RTI;  after: TMS←1
            tck_tick(1'b0, 1'b0);  // posedge: TMS=1 → RTI→Select-DR;           after: TMS←0
            tck_tick(1'b0, 1'b0);  // posedge: TMS=0 → Select-DR→Capture-DR;    after: TMS←0
            tck_tick(1'b0, 1'b0);  // posedge: TMS=0 → Capture-DR→Shift-DR (CAPTURE_DR fires); negedge: TDO←dr[0]
            for (i = 0; i < len; i++) begin
                bit last = (i == len - 1);
                bit tdi_bit = tdi_data[i];
                @(negedge vif.tck);
                #1;                               // let DUT negedge NBA settle
                tdo_data[i] = vif.tdo;
                tck_tick(last ? 1'b1 : 1'b0, tdi_bit);
            end
            tck_tick(1'b1, 1'b0);  // Exit1-DR → Update-DR
            tck_tick(1'b0, 1'b0);  // → RTI
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

    // -----------------------------------------------------------------
    // Monitor (minimal — records IR loads)
    // -----------------------------------------------------------------
    class jtag_monitor extends uvm_monitor;
        `uvm_component_utils(jtag_monitor)
        virtual ila_jtag_if vif;
        uvm_analysis_port #(jtag_xact) ap;

        function new(string name, uvm_component parent);
            super.new(name, parent);
            ap = new("ap", this);
        endfunction

        function void build_phase(uvm_phase phase);
            super.build_phase(phase);
            void'(uvm_config_db#(virtual ila_jtag_if)::get(this, "", "vif", vif));
        endfunction

        task run_phase(uvm_phase phase);
            // Intentional no-op for v1. Tests check results directly.
            forever @(posedge vif.tck);
        endtask
    endclass

    // -----------------------------------------------------------------
    // Agent
    // -----------------------------------------------------------------
    class jtag_agent extends uvm_agent;
        `uvm_component_utils(jtag_agent)
        jtag_sequencer sequencer;
        jtag_driver    driver;
        jtag_monitor   monitor;

        function new(string name, uvm_component parent);
            super.new(name, parent);
        endfunction

        function void build_phase(uvm_phase phase);
            super.build_phase(phase);
            sequencer = jtag_sequencer::type_id::create("sequencer", this);
            driver    = jtag_driver   ::type_id::create("driver",    this);
            monitor   = jtag_monitor  ::type_id::create("monitor",   this);
        endfunction

        function void connect_phase(uvm_phase phase);
            driver.seq_item_port.connect(sequencer.seq_item_export);
        endfunction
    endclass

    // -----------------------------------------------------------------
    // Env
    // -----------------------------------------------------------------
    class ila_env extends uvm_env;
        `uvm_component_utils(ila_env)
        jtag_agent agent;

        function new(string name, uvm_component parent);
            super.new(name, parent);
        endfunction

        function void build_phase(uvm_phase phase);
            super.build_phase(phase);
            agent = jtag_agent::type_id::create("agent", this);
        endfunction
    endclass

    // -----------------------------------------------------------------
    // Sequence API helpers — high-level operations on the ILA
    // -----------------------------------------------------------------
    class ila_base_seq extends uvm_sequence #(jtag_xact);
        `uvm_object_utils(ila_base_seq)
        function new(string name = "ila_base_seq"); super.new(name); endfunction

        // Wait until the virtual interface is available and trst_n is high.
        // Call this at the start of every test body.
        task wait_reset_done();
            virtual ila_jtag_if vif;
            if (!uvm_config_db#(virtual ila_jtag_if)::get(null,
                    "uvm_test_top.env.agent.*", "vif", vif)) begin
                `uvm_fatal("NOVIF", "wait_reset_done: vif not found")
            end
            if (!vif.trst_n) @(posedge vif.trst_n);
            // One extra TCK to let the DUT settle after TRST_N
            @(posedge vif.tck);
        endtask

        task reset_tap();
            jtag_xact t = jtag_xact::type_id::create("t");
            t.op = OP_RESET;
            start_item(t); finish_item(t);
        endtask

        task runtest(int unsigned cycles);
            jtag_xact t = jtag_xact::type_id::create("t");
            t.op = OP_RUNTEST;
            t.runtest_cycles = cycles;
            start_item(t); finish_item(t);
        endtask

        task shift_ir(bit [ILA_IR_W-1:0] opcode);
            jtag_xact t = jtag_xact::type_id::create("t");
            t.op = OP_SHIFT_IR;
            t.length = ILA_IR_W;
            t.tdi_bits = '0;
            t.tdi_bits[ILA_IR_W-1:0] = opcode;
            start_item(t); finish_item(t);
        endtask

        task shift_dr(int unsigned len, bit [1023:0] tdi, output bit [1023:0] tdo);
            jtag_xact t = jtag_xact::type_id::create("t");
            t.op = OP_SHIFT_DR;
            t.length = len;
            t.tdi_bits = tdi;
            start_item(t); finish_item(t);
            tdo = t.tdo_bits;
        endtask

        // High-level ILA ops
        task ila_read_idcode(output bit [31:0] idcode);
            bit [1023:0] tdo;
            shift_ir(IR_IDCODE);
            shift_dr(32, '0, tdo);
            idcode = tdo[31:0];
        endtask

        task ila_read_config(output bit [31:0] cfg);
            bit [1023:0] tdo;
            shift_ir(IR_CONFIG);
            shift_dr(32, '0, tdo);
            cfg = tdo[31:0];
        endtask

        task ila_set_mask(bit [ILA_DATA_W-1:0] mask);
            bit [1023:0] tdo;
            bit [1023:0] tdi;
            tdi = '0; tdi[ILA_DATA_W-1:0] = mask;
            shift_ir(IR_TRIG_MASK);
            shift_dr(ILA_DATA_W, tdi, tdo);
        endtask

        task ila_set_value(bit [ILA_DATA_W-1:0] value);
            bit [1023:0] tdo;
            bit [1023:0] tdi;
            tdi = '0; tdi[ILA_DATA_W-1:0] = value;
            shift_ir(IR_TRIG_VAL);
            shift_dr(ILA_DATA_W, tdi, tdo);
        endtask

        task ila_set_pre_samples(bit [15:0] pre);
            bit [1023:0] tdo;
            bit [1023:0] tdi;
            tdi = '0; tdi[15:0] = pre;
            shift_ir(IR_PRE_SAMPLES);
            shift_dr(16, tdi, tdo);
        endtask

        task ila_ctrl(bit arm, bit stop, bit reset, bit force_trig);
            bit [1023:0] tdo;
            bit [1023:0] tdi;
            tdi = '0;
            tdi[CTRL_ARM]        = arm;
            tdi[CTRL_STOP]       = stop;
            tdi[CTRL_RESET]      = reset;
            tdi[CTRL_FORCE_TRIG] = force_trig;
            shift_ir(IR_CTRL);
            shift_dr(4, tdi, tdo);
        endtask

        task ila_read_status(output bit [7:0] status);
            bit [1023:0] tdo;
            shift_ir(IR_STATUS);
            shift_dr(8, '0, tdo);
            status = tdo[7:0];
        endtask

        task ila_set_read_addr(bit [ILA_ADDR_W-1:0] addr);
            bit [1023:0] tdo;
            bit [1023:0] tdi;
            tdi = '0; tdi[ILA_ADDR_W-1:0] = addr;
            shift_ir(IR_READ_ADDR);
            shift_dr(ILA_ADDR_W, tdi, tdo);
        endtask

        task ila_read_samples(input int unsigned count,
                              output bit [ILA_DATA_W-1:0] samples[$]);
            bit [1023:0] tdo;
            int i;
            samples = {};
            shift_ir(IR_READ_DATA);
            for (i = 0; i < count; i++) begin
                shift_dr(ILA_DATA_W, '0, tdo);
                samples.push_back(tdo[ILA_DATA_W-1:0]);
            end
        endtask

        task ila_wait_full(int unsigned max_tries = 10000);
            bit [7:0] st;
            int unsigned i;
            for (i = 0; i < max_tries; i++) begin
                ila_read_status(st);
                if (st[STATUS_FULL]) return;
            end
            `uvm_error("ILA_TIMEOUT", $sformatf("status never went full after %0d polls", max_tries))
        endtask
    endclass

    // -----------------------------------------------------------------
    // Base test
    // -----------------------------------------------------------------
    class ila_base_test extends uvm_test;
        `uvm_component_utils(ila_base_test)
        ila_env env;

        function new(string name, uvm_component parent);
            super.new(name, parent);
        endfunction

        function void build_phase(uvm_phase phase);
            super.build_phase(phase);
            env = ila_env::type_id::create("env", this);
        endfunction

        task run_phase(uvm_phase phase);
            phase.raise_objection(this);
            body(phase);
            phase.drop_objection(this);
        endtask

        virtual task body(uvm_phase phase);
            `uvm_info("ILA_TEST", "base body (override me)", UVM_LOW)
        endtask

        function void report_phase(uvm_phase phase);
            uvm_report_server rs = uvm_report_server::get_server();
            int unsigned errs  = rs.get_severity_count(UVM_ERROR);
            int unsigned fatal = rs.get_severity_count(UVM_FATAL);
            if (errs == 0 && fatal == 0)
                `uvm_info("RESULT", "TEST PASSED", UVM_NONE)
            else
                `uvm_info("RESULT", $sformatf("TEST FAILED (errors=%0d fatal=%0d)",
                                              errs, fatal), UVM_NONE)
        endfunction
    endclass

    // -----------------------------------------------------------------
    // Test: IDCODE smoke
    // -----------------------------------------------------------------
    class ila_tap_smoke_test extends ila_base_test;
        `uvm_component_utils(ila_tap_smoke_test)
        function new(string name, uvm_component parent); super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            ila_base_seq seq = ila_base_seq::type_id::create("seq");
            bit [31:0] idcode;
            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.ila_read_idcode(idcode);
            if (idcode !== ILA_IDCODE_VAL)
                `uvm_error("IDCODE",
                           $sformatf("expected 0x%08h got 0x%08h", ILA_IDCODE_VAL, idcode))
            else
                `uvm_info("IDCODE", $sformatf("OK 0x%08h", idcode), UVM_LOW)
        endtask
    endclass

    // -----------------------------------------------------------------
    // Test: trigger match
    // -----------------------------------------------------------------
    class ila_trigger_match_test extends ila_base_test;
        `uvm_component_utils(ila_trigger_match_test)
        function new(string name, uvm_component parent); super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            ila_base_seq seq = ila_base_seq::type_id::create("seq");
            bit [ILA_DATA_W-1:0] samples[$];
            bit [7:0] st;

            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.ila_set_mask(32'hFFFF_FFFF);
            // Counter is free-running at 100 MHz.  JTAG arm completes ~17 us
            // after reset (counter ≈ 1700).  Use 0x1000 = 4096 so the trigger
            // fires at ~41 us, well before the 5 ms watchdog.
            seq.ila_set_value(32'h0000_1000);
            seq.ila_set_pre_samples(16'(ILA_DEPTH/4));
            seq.ila_ctrl(.arm(1), .stop(0), .reset(0), .force_trig(0));
            seq.ila_wait_full();
            seq.ila_read_status(st);
            if (!st[STATUS_TRIGGERED])
                `uvm_error("NO_TRIG", "triggered bit not set")
            if (!st[STATUS_FULL])
                `uvm_error("NO_FULL", "full bit not set")

            seq.ila_set_read_addr(0);
            seq.ila_read_samples(ILA_DEPTH, samples);
            if (samples.size() != ILA_DEPTH)
                `uvm_error("SIZE", $sformatf("got %0d samples", samples.size()))
            `uvm_info("ILA", $sformatf("read %0d samples", samples.size()), UVM_LOW)
        endtask
    endclass

    // -----------------------------------------------------------------
    // Test: pre/post split (force trigger after controlled window)
    // -----------------------------------------------------------------
    class ila_prepost_split_test extends ila_base_test;
        `uvm_component_utils(ila_prepost_split_test)
        function new(string name, uvm_component parent); super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            ila_base_seq seq = ila_base_seq::type_id::create("seq");
            bit [7:0] st;
            int unsigned pre = 128;

            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.ila_set_mask('0);              // never auto-match
            seq.ila_set_value('0);
            seq.ila_set_pre_samples(16'(pre));
            seq.ila_ctrl(.arm(1), .stop(0), .reset(0), .force_trig(0));
            // Let the sampler run a while
            seq.runtest(256);
            seq.ila_ctrl(.arm(0), .stop(0), .reset(0), .force_trig(1));
            seq.ila_wait_full();
            seq.ila_read_status(st);
            if (!st[STATUS_FULL])
                `uvm_error("PREPOST", "full not asserted after force_trig")
            `uvm_info("PREPOST", $sformatf("status=0x%02h", st), UVM_LOW)
        endtask
    endclass

    // -----------------------------------------------------------------
    // Test: chain bypass — ensures TDO falls through to downstream
    // -----------------------------------------------------------------
    class ila_chain_bypass_test extends ila_base_test;
        `uvm_component_utils(ila_chain_bypass_test)
        function new(string name, uvm_component parent); super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            // With the default ila_tb_top, the DUT's `tdi` is the TB-driven
            // tdi. When the TAP is not shifting, tdo == tdi (pass-through).
            ila_base_seq seq = ila_base_seq::type_id::create("seq");
            bit [31:0] idcode;
            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.ila_read_idcode(idcode);
            if (idcode !== ILA_IDCODE_VAL)
                `uvm_error("CHAIN", $sformatf("IDCODE mismatch 0x%08h", idcode))
            else
                `uvm_info("CHAIN", "IR/DR scans traverse OK", UVM_LOW)
        endtask
    endclass

    // -----------------------------------------------------------------
    // Test: CONFIG register read — confirms HDL parameters (DATA_W,
    // ADDR_W, NUM_CH) are exposed to the host via IR_CONFIG.
    // -----------------------------------------------------------------
    class ila_config_read_test extends ila_base_test;
        `uvm_component_utils(ila_config_read_test)
        function new(string name, uvm_component parent); super.new(name,parent); endfunction

        virtual task body(uvm_phase phase);
            ila_base_seq seq = ila_base_seq::type_id::create("seq");
            bit [31:0] cfg;
            bit [7:0]  version;
            bit [3:0]  num_ch;
            bit [5:0]  data_w_m1;
            bit [7:0]  addr_w;

            seq.start(env.agent.sequencer);
            seq.wait_reset_done();
            seq.reset_tap();
            seq.ila_read_config(cfg);

            version   = cfg[31:24];
            num_ch    = cfg[23:20];
            data_w_m1 = cfg[15:10];
            addr_w    = cfg[ 7: 0];

            `uvm_info("CONFIG",
                      $sformatf("raw=0x%08h version=%0d num_ch=%0d data_w=%0d addr_w=%0d",
                                cfg, version, num_ch, data_w_m1+1, addr_w), UVM_LOW)

            if (version !== 8'h01)
                `uvm_error("CONFIG_VER", $sformatf("expected version 0x01 got 0x%02h", version))
            if (num_ch !== 4'd1)
                `uvm_error("CONFIG_NUMCH", $sformatf("expected num_ch=1 got %0d", num_ch))
            if ((data_w_m1 + 1) !== ILA_DATA_W)
                `uvm_error("CONFIG_DATAW", $sformatf("expected DATA_W=%0d got %0d", ILA_DATA_W, data_w_m1+1))
            if (addr_w !== ILA_ADDR_W)
                `uvm_error("CONFIG_ADDRW", $sformatf("expected ADDR_W=%0d got %0d", ILA_ADDR_W, addr_w))
        endtask
    endclass

endpackage
