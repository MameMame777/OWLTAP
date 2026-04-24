# Plan: ILA Signal Definition ROM (RTL → JTAG auto-default)
**Date:** 2026-04-25
**Status:** Phase 1 In Progress

## Goal
Allow the ILA RTL to declare its signal-lane decomposition (hi/lo/format
per slice) via parameters, expose it through a new JTAG register, and
have the host populate the ILA panel's default lanes from it on connect.
User edits in `cfg.json` continue to take precedence.

## Decided
- Signal **names** are auto-generated host-side as `data[hi:lo]`
  (no SIG_NAME ROM in this PR — possible future Phase 6).
- `fmt` field width: **4 bits** (room for SIGNED/TIME etc.).
- Implementation order: **Phase 1 (RTL minimal) first.**
- Word format (32-bit, LSB→MSB on the JTAG wire as stored in CONFIG_VAL
  pattern):
  ```
  [31:24] fmt[3:0] | rsvd[3:0]   (fmt: 0=HEX 1=DEC 2=BIN; rsvd=0)
  [23:16] hi[7:0]  (inclusive MSB index, 0-based)
  [15: 8] lo[7:0]  (inclusive LSB index, 0-based)
  [ 7: 0] name_idx[7:0]  (0xFF = host auto-generates "data[hi:lo]")
  ```

## CONFIG register update (5'h02)
- `[19:16]` (was RESERVED) → `SIG_COUNT[3:0]` (0..15)
- `SIG_COUNT == 0` ⇒ host falls back to current default (`data[DW-1:0]`).
- Backward compatible: legacy bitstreams report `SIG_COUNT=0`.

## Phase 1 Scope (this PR)
**RTL minimal slice — 1 fixed entry, no array ROM yet.**

1. `hdl/ila/rtl/ila_tap.sv`:
   - Add `localparam IR_SIG_DEF = 5'h03`
   - Add `SIG_COUNT_VAL = 4'd1` localparam
   - Update `CONFIG_VAL` to embed `SIG_COUNT_VAL` in `[19:16]`
   - Add fixed `SIG_DEF_VAL = { 4'h0, 4'h0, 8'(DATA_W-1), 8'd0, 8'hFF }`
   - Capture/Shift/TDO mux: handle `IR_SIG_DEF` like `IR_CONFIG` (read-only 32b)
2. `hdl/ila/rtl/ila_bscane2_top.sv`:
   - Same `SIG_COUNT_VAL`, `SIG_DEF_VAL` localparams
   - `CONFIG_VAL` updated similarly
   - `capture_data` mux: `5'h03 → SIG_DEF_VAL`
3. `hdl/ila/rtl/ila_top.sv`: no functional change (parameters pass through)
4. Simulation testbench: `hdl/ila/sim/tb/ila_bscane2_test_pkg.sv` add a
   read-back test for SIG_DEF returning expected `0x00_1F_00_FF`
   (DATA_W=32) and CONFIG_VAL `[19:16] = 1`.

**Out of scope for Phase 1:**
- Variable SIG_COUNT / array ROM / index counter (→ Phase 2)
- Host-side `IlaDriver::readSignalDefs()` (→ Phase 3)
- ILA panel auto-population (→ Phase 4)
- Real-hardware bitstream rebuild (→ Phase 5)

## Verification (Phase 1)
- Host build (`bazel build //src:jtag_viewer`) must still succeed
  (no software changes needed for Phase 1).
- HDL compile cleanly (no parser errors). Sim run is recommended but
  optional in Phase 1 — Phase 2 reuses the same path with variable size.
- Manual: read CONFIG → expect `SIG_COUNT == 1`, read SIG_DEF → expect
  word with `hi=DATA_W-1, lo=0, fmt=0, name_idx=0xFF`.

## Files modified (Phase 1)
| File | Change |
|------|--------|
| `hdl/ila/rtl/ila_tap.sv` | IR_SIG_DEF, SIG_DEF_VAL, CONFIG_VAL update, mux entries |
| `hdl/ila/rtl/ila_bscane2_top.sv` | SIG_DEF_VAL, CONFIG_VAL update, capture_data mux |
| `docs/plan/plan_ila_sig_def_rom_20260425.md` | this file |

No host code, BUILD, or test changes in Phase 1.

## Future Phases (summary)
- **Phase 2 (RTL ROM):** parameter arrays `SIG_HI[]/SIG_LO[]/SIG_FMT[]`,
  index counter (incremented on UPDATE_DR like READ_DATA),
  `ila_signal_rom.sv` module if size warrants.
- **Phase 3 (Host driver):** `IlaCaps::sig_count`, `IlaDriver::readSignalDefs()`,
  unit tests with mock backend.
- **Phase 4 (UI):** `IlaPanel::resetSignalDefs()` priority logic
  (cfg.json → RTL ROM → fallback).
- **Phase 5 (Hardware):** Zybo Z7020 bringup top sets SIG_HI/LO/FMT for
  32→[16,8,4,2,1,1] split; real-hardware verification.
- **Phase 6 (optional):** SIG_NAME ROM for arbitrary names.
