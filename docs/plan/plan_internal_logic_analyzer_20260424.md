# Plan: Internal Logic Analyzer (ILA) IP — 2026-04-24

Vendor-neutral SystemVerilog soft-IP that samples a user-supplied 32-bit
signal bus into a 1024-word BRAM ring buffer, armed and read back over JTAG
via its own IEEE 1149.1 TAP controller daisy-chained on the existing JTAG
cable. Host-side driver reuses `TapController` / `JtagChain`; results feed
the existing `WaveformView` via a new `IlaSampleSource` adapter.

## Decisions (confirmed 2026-04-24)

- HDL language: **SystemVerilog**.
- Target: vendor-neutral (no BSCANE2). Dedicated TAP in daisy chain.
- Defaults: `DATA_W = 32`, `DEPTH = 1024`, IR length 5.
- Trigger: **single-match** `(data_in & mask) == value`.
- Clocking: **shared TCK** for JTAG side; user `sample_clk` independent.
  CDC inside `ila_bram`.
- IDCODE: placeholder `0xA17A0001` — documented, to be replaced before
  release.
- GUI: reuse `WaveformView` with a `SignalSource` switch.
- Verification: **Altair DSim 2025.1 + UVM 1.2**, modeled after
  `E:/Nautilus/workspace/fpgawork/AXIUART_RV32I` conventions (Option B —
  lightweight UVM env).

---

## Phase 1 — HDL IP core

**Goal**: Synthesizable SystemVerilog modules for the ILA, with a clean
separation between the capture pipeline and the JTAG access port.

**Files** (under `hdl/ila/rtl/`)

1. `ila_trigger.sv`
   - Inputs: `data_in[DATA_W-1:0]`, `mask[DATA_W-1:0]`, `value[DATA_W-1:0]`.
   - Output: `match` registered.
2. `ila_capture_fsm.sv`
   - States: `IDLE → ARMED → TRIGGERED → FULL`.
   - Ports: `arm`, `stop`, `force_trig`, `trig_match`, `sample_valid`.
   - Exposes `state`, `triggered`, `full`, `write_addr`, `trigger_addr`.
   - Pre/post split register `pre_samples[log2(DEPTH)-1:0]` (default
     `DEPTH/4 = 256`).
3. `ila_bram.sv`
   - True dual-port (inferred). Write port @ `sample_clk`, read port @ `tck`.
   - Gray-coded pointers across domains for a read-safe snapshot.
4. `ila_tap.sv`
   - Canonical IEEE 1149.1 TAP FSM (16 states).
   - IR length 5. Opcodes:
     | Name | Value | DR width |
     |------|-------|----------|
     | `IDCODE`     | `5'h01` | 32 |
     | `CTRL`       | `5'h08` | 4  |
     | `STATUS`     | `5'h09` | 8  |
     | `TRIG_MASK`  | `5'h0A` | 32 |
     | `TRIG_VAL`   | `5'h0B` | 32 |
     | `PRE_SAMPLES`| `5'h0E` | 16 |
     | `READ_ADDR`  | `5'h0C` | log2(DEPTH) |
     | `READ_DATA`  | `5'h0D` | DATA_W |
     | `BYPASS`     | `5'h1F` | 1  |
   - IDCODE default `32'hA17A_0001`.
   - CTRL DR bits `[0]=ARM, [1]=STOP, [2]=RESET, [3]=FORCE_TRIG` (pulse).
   - STATUS DR: `{5'b0, full, triggered, armed}`.
5. `ila_top.sv`
   - Parameters: `DATA_W`, `DEPTH`, `IDCODE`.
   - Ports: `tck, tms, tdi, tdo` + `sample_clk, sample_rst_n, data_in,
     data_valid`.
   - Daisy-chain capable: `tdi` from upstream, `tdo` to downstream.

**Out of scope for Phase 1**: multi-stage triggers, compression, storage
qualifiers.

---

## Phase 2 — Constraints & integration doc

1. `hdl/ila/constraints/ila_clocks.xdc`
   - `create_clock -name tck -period 100 ns [get_ports tck]` (10 MHz).
   - `set_clock_groups -asynchronous -group {tck} -group {sample_clk}`.
   - False-path on Gray pointer CDC nets.
2. `hdl/ila/doc/integration.md`
   - Instantiation example.
   - JTAG cable wiring (TCK/TMS shared with FPGA TAP; chain ordering).
   - `JtagChain::detectDevices` expectations (IR length 5, IDCODE stated).
   - **Release warning**: IDCODE is placeholder; allocate a JEDEC
     manufacturer ID or override `IDCODE` parameter before production use.
3. `hdl/ila/examples/zynq_pynq/`
   - Minimal Vivado project stub pairing `ila_top` with a ring counter on
     PYNQ-Z1 for bring-up.

---

## Phase 3 — Host-side driver

**Files** (under `src/ila/`)

1. `src/ila/BUILD.bazel` + `src/ila/ila_driver.{h,cpp}`
   - Ctor: `IlaDriver(JtagChain& chain, int device_index)`.
   - Pre-condition: chain already detected; caller responsible for IDCODE
     match.
   - API:
     ```cpp
     struct IlaStatus { bool armed; bool triggered; bool full; };
     bool configureTrigger(uint32_t mask, uint32_t value,
                           uint16_t pre_samples);
     bool arm();
     bool stop();
     bool forceTrigger();
     IlaStatus status();
     bool readTriggerIndex(uint16_t& index);
     bool readSamples(std::vector<uint32_t>& out);
     const std::string& lastError() const;
     ```
   - Read loop: shift `READ_ADDR` IR once, then loop `READ_DATA` DR reads
     (TAP auto-increments `READ_ADDR` on each DR read). Falls back to
     explicit address writes if auto-increment is disabled.
2. `src/ila/ila_sample_source.{h,cpp}`
   - Adapter wrapping `IlaDriver`; exposes `std::vector<SampleFrame>
     fetch()` for `WaveformView`.
   - Timestamp: monotonic nanoseconds spaced by configured sample period
     (user-provided; unrelated to JTAG TCK).

---

## Phase 4 — GUI integration

1. `src/gui/ila_panel.{h,cpp}` — new ImGui panel:
   - Mask / value hex inputs (32-bit).
   - Pre-sample slider (0..DEPTH-1).
   - Buttons: **Arm**, **Stop**, **Force**, **Read**.
   - Status LEDs: Armed / Triggered / Full.
2. `src/gui/waveform_view.{h,cpp}` — add `SignalSource` enum:
   - `BoundaryScan` (existing), `IlaCapture` (new).
   - On switch: clear buffer and rebind labels.
3. `ila_signals.json` (loaded via existing `BusDefinition`): bit index →
   signal/bus name map.
4. `src/gui/app_window.{h,cpp}`: wire panel + menu
   `View → Internal Logic Analyzer`.

---

## Phase 5 — Verification (DSim + UVM)

Environment: Altair DSim 2025.1 + UVM 1.2. Structure mirrors
`E:/Nautilus/workspace/fpgawork/AXIUART_RV32I/sim/`.

**Directory layout** (`hdl/ila/sim/`)
```
sim/
  exec/          DSim filelist, compile options
  bfm/           jtag_if.sv, ila_jtag_api.svh
  uvm/
    jtag_seq_item.sv
    jtag_sequencer.sv
    jtag_driver.sv
    jtag_monitor.sv
    ila_agent.sv
    ila_scoreboard.sv
    ila_env.sv
    sequences/
      ila_idcode_seq.sv
      ila_trigger_seq.sv
      ila_prepost_seq.sv
      ila_chain_bypass_seq.sv
  tests/
    ila_base_test.sv
    ila_tap_smoke_test.sv
    ila_trigger_match_test.sv
    ila_prepost_split_test.sv
    ila_chain_bypass_test.sv
  scripts/
    run_test.ps1           (modeled after AXIUART_RV32I run_test.ps1)
  regression_tests.json
```

**UVM components**

- `jtag_seq_item`: fields `op (IR|DR|RESET|RUNTEST)`, `bits`, `tdi_data`,
  `tdo_data`, `length`, response `tdo_capture`.
- `jtag_driver`: asserts `tck/tms/tdi`, captures `tdo` per IEEE 1149.1.
- `jtag_monitor`: observes shift sequences, reconstructs transactions.
- `ila_agent`: sequencer + driver + monitor.
- `ila_scoreboard`: maintains golden model (shadow trigger state and BRAM
  contents) and checks against observed readback.
- `ila_env`: top-level env.

**Tests**

| Test | Purpose |
|------|---------|
| `ila_tap_smoke_test` | IDCODE match `0xA17A0001`; BYPASS path OK |
| `ila_trigger_match_test` | Configure mask/value; trigger on counter == value; readback trigger sample at `pre_samples` boundary |
| `ila_prepost_split_test` | Verify pre/post ratio; full-buffer termination |
| `ila_chain_bypass_test` | 2-device chain (ILA + BYPASS device); IR/DR traverse OK |

**Scripts & regression**

- `hdl/ila/sim/scripts/run_test.ps1` (PowerShell) — per-test wrapper:
  `./run_test.ps1 ila_trigger_match_test -Verbosity UVM_LOW`.
- Optional `-Plusargs "+define+ENABLE_ASSERTIONS"` for CDC / protocol
  assertions.
- `regression_tests.json` enumerates all four tests; consumed by a
  `run_regression.ps1` modeled after the reference project.

**C++ host parity**

- `test/ila_driver_test.cpp` with `test/support/mock_tap.{h,cpp}`.
- `MockTap` implements the same IR/DR semantics as `ila_tap.sv` against an
  in-memory BRAM + FSM model.
- Golden vectors exported from DSim runs to `test/data/ila_golden_*.hex`
  ensure the C++ model matches HDL behaviour.

---

## Relevant files

**New**
- `hdl/ila/rtl/{ila_trigger,ila_capture_fsm,ila_bram,ila_tap,ila_top}.sv`
- `hdl/ila/constraints/ila_clocks.xdc`
- `hdl/ila/doc/integration.md`
- `hdl/ila/examples/zynq_pynq/` (Vivado stub)
- `hdl/ila/sim/` (full UVM env above)
- `src/ila/BUILD.bazel`, `src/ila/{ila_driver,ila_sample_source}.{h,cpp}`
- `src/gui/{ila_panel}.{h,cpp}`
- `test/ila_driver_test.cpp`, `test/support/{mock_tap}.{h,cpp}`

**Modified**
- `src/gui/{waveform_view,app_window,app_config}.{h,cpp}`
- `src/gui/BUILD.bazel`, `test/BUILD.bazel`

**Confirmed reusable (no change required)**
- `src/jtag/jtag_chain.h` — `readDataDR` already supports BSDL-less devices.

---

## Verification

1. `bazel test //test:ila_driver_test` green (host-side driver against
   `MockTap`).
2. DSim regression:
   ```powershell
   pwsh hdl/ila/sim/scripts/run_test.ps1 -Regression
   ```
   All four tests report `0 UVM_ERROR` and `PASSED`.
3. Debug run: `-Plusargs "+define+ENABLE_ASSERTIONS"` enables CDC /
   protocol checks.
4. Bench: PYNQ-Z1 bitstream with `ila_top` + 32-bit ring counter; arm with
   mask `0xFFFFFFFF`, value `0x00000100`; confirm trigger and waveform
   alignment via GUI.
5. JTAG chain regression: `JtagChain::detectDevices` reports 3 devices
   (ARM-DAP, PL-TAP, ILA) with correct IR lengths; existing
   `scanner_test` / `trigger_test` remain green.

---

## Scope

**Included**: vendor-neutral SV IP, single-match trigger, BRAM depth 1024,
width 32, daisy-chain JTAG, host driver, GUI, DSim UVM tests.

**Excluded (initial)**: multi-stage triggers, compression, variable-rate
sampling, cross-trigger between ILA instances, Vivado `.xci` packaging,
AXI-4 ILA register interface.
