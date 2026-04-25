# Plan: Edge Trigger Extension

**Date:** 2026-04-25
**Status:** Reviewed (rev 2)

## Objective

Extend the ILA trigger system to support per-signal edge (Rise/Fall) conditions
in addition to the existing level (mask/value) comparison.  The user selects a
signal lane and specifies an edge type; the host computes the hardware register
values automatically.  RTL is extended with two new DR registers and a previous-
sample latch so edge detection runs entirely in the FPGA's sample clock domain.

---

## Current State (verified against repo)

| Register | IR   | Width | Notes |
|----------|------|-------|-------|
| TRIG_MASK | 0x0A | 32 | bits to compare |
| TRIG_VAL  | 0x0B | 32 | expected value |
| PRE_SAMPLES | 0x0E | 10 | |

Trigger fires when: `(data & TRIG_MASK) == (TRIG_VAL & TRIG_MASK)`

Real RTL files involved:
- `hdl/ila/rtl/ila_trigger.sv` — combinational mask/value comparator,
  no `prev_data` register today.
- `hdl/ila/rtl/ila_top.sv` — instantiates `ila_trigger` with mask/val from TAP.
- `hdl/ila/rtl/ila_bscane2_top.sv` — **also** instantiates its own `ila_trigger`
  (independent path; both must be updated).
- `hdl/ila/rtl/ila_capture_fsm.sv` — receives `trig_match` from `ila_trigger`.

CONFIG `[31:24] VERSION = 8'h01` today; bumping to `8'h02` signals edge support.

---

## New Architecture

### Hardware trigger equation (sample_clk domain)

```
prev_data updates:  on (data_valid && armed) only
rising  = ~prev_data &  data
falling =  prev_data & ~data

level_ok = (mask == 0)
        || ((data & mask) == (value & mask))

edge_ok  = (rise_mask == 0 && fall_mask == 0)
        || ((rising  & rise_mask) != 0)
        || ((falling & fall_mask) != 0)

match    = data_valid && level_ok && edge_ok
```

`prev_data` is **newly added** inside `ila_trigger` (it does not exist today).
Updated only on `data_valid` so edge detection respects valid-strobe gating.

### New IR opcodes

| IR   | Name           | Width | Description |
|------|----------------|-------|-------------|
| 0x0F | TRIG_RISE_MASK | 32    | Rising-edge bitmask  (default 0 = disabled) |
| 0x10 | TRIG_FALL_MASK | 32    | Falling-edge bitmask (default 0 = disabled) |

IR_W = 5 (range 0x00..0x1F). 0x0F/0x10 unallocated today — safe.

### CONFIG VERSION bump

`CONFIG_VAL[31:24]` = `8'h02` after this change.  Host treats:
- `version >= 0x02` -> edge trigger registers available
- `version == 0x01` -> legacy; rise/fall masks must be left at 0 (default)

### GUI per-lane trigger condition (semantics)

Each signal lane gets one `TriggerCondition` selector:

| Option       | Effect on hw registers (bits within the lane's [hi:lo] range) |
|--------------|---------------------------------------------------------------|
| None         | mask=0, value=0, rise_mask=0, fall_mask=0 (lane unused)       |
| == value     | mask=lane_bits, value=user_value                              |
| != value     | mask=lane_bits, value=~user_value & lane_bits                 |
| Rising edge  | rise_mask=lane_bits                                            |
| Falling edge | fall_mask=lane_bits                                            |
| Either edge  | rise_mask=lane_bits, fall_mask=lane_bits                       |

**Multi-lane combination semantics:**
- All "level" lanes are combined by **AND** (their mask bits OR-merge into a
  single mask register; their value bits do likewise).
- All "edge" lanes are combined by **OR** (any matching edge bit fires).
- Final trigger = `level_ok AND edge_ok` (the equation above).

This mirrors the natural hardware behaviour and matches the existing single-
mask/value semantics for backward compatibility.

---

## Implementation Phases

### Phase 1 — RTL: TRIG_RISE_MASK + TRIG_FALL_MASK + prev_data

**Files:** `hdl/ila/rtl/ila_tap.sv`, `hdl/ila/rtl/ila_bscane2_top.sv`,
`hdl/ila/rtl/ila_top.sv`, `hdl/ila/rtl/ila_trigger.sv`.

1. `ila_tap.sv`:
   - Add `IR_TRIG_RISE = 5'h0F`, `IR_TRIG_FALL = 5'h10` localparams.
   - Add `rise_mask_reg`, `fall_mask_reg` DR registers (parallel to `mask_reg`).
   - Add CAPTURE/SHIFT/UPDATE cases for the new IRs.
   - Add module outputs `trig_rise_mask`, `trig_fall_mask`.
   - Bump `CONFIG_VAL[31:24]` from `8'h01` to `8'h02`.

2. `ila_top.sv`:
   - Receive `trig_rise_mask` / `trig_fall_mask` from `ila_tap`.
   - Add ASYNC_REG 2-FF synchronizers (`tck` -> `sample_clk`) for both signals,
     mirroring the existing `mask_sc`/`val_sc` pattern.
   - Connect synchronized values to extended `ila_trigger`.

3. `ila_bscane2_top.sv`:
   - Same modifications as `ila_top.sv` (independent trigger instance).
   - Inline TAP shift register also needs the new IR opcodes/registers.
   - Bump `CONFIG_VAL[31:24]` to `8'h02`.

4. `ila_trigger.sv`:
   - Add `rise_mask`, `fall_mask` input ports.
   - Add `prev_data` register; update only when `valid_in`.
   - Compute `rising`, `falling`, `level_ok`, `edge_ok` per the equation.
   - Output `match` registered as today.
   - Default behaviour preserved: when `rise_mask == 0 && fall_mask == 0`,
     `edge_ok == 1` so trigger reduces to legacy mask/value.

### Phase 2 — Host driver: extend configureTrigger()

**Files:** `src/ila/ila_driver.h`, `src/ila/ila_driver.cpp`.

1. Add `kIrTrigRise = 0x0F`, `kIrTrigFall = 0x10` to `Ir` enum.
2. New 5-arg overload:
   ```cpp
   bool configureTrigger(uint32_t mask, uint32_t value,
                         uint32_t rise_mask, uint32_t fall_mask,
                         uint16_t pre_samples);
   ```
   The legacy 3-arg version simply calls the new one with `rise=fall=0`.
3. Skip writing rise/fall registers when `caps_.version < 0x02` (silent no-op,
   set `last_error_` only if non-zero values were requested).
4. No changes to `ChainIlaTapBackend` / `BscaneIlaTapBackend` — they pass
   through any IR opcode transparently.

### Phase 3 — GUI: per-lane trigger editor

**Files:** `src/gui/ila_panel.h`, `src/gui/ila_panel.cpp`.

1. Add to `ila_panel.h`:
   ```cpp
   enum class TriggerCond { None, Eq, Neq, Rise, Fall, Either };
   struct LaneTrigger {
       TriggerCond cond  = TriggerCond::None;
       uint32_t    value = 0;   // only used by Eq / Neq; TODO widen if DATA_W>32
   };
   std::vector<LaneTrigger> lane_triggers_;  // size matches signals_
   ```
2. Resize `lane_triggers_` whenever `signals_` is rebuilt (in `setChain`,
   `setBscaneChain`, `resetSignalDefs`, `importSignalConfigs`).
3. Replace the raw mask/val InputText with a per-lane row inside
   `drawSignalEditor()`:
   - Combo box: `None / == / != / Rise / Fall / Either`.
   - InputText (hex) for value, shown only for `Eq` / `Neq`.
4. Add `computeTriggerRegisters(uint32_t& mask, uint32_t& value,
   uint32_t& rise_mask, uint32_t& fall_mask) const;`.
5. `doArm()` -> call new 5-arg `configureTrigger()`.
6. Show computed mask/value/rise/fall as **read-only** labels above the lane
   table (no separate "Advanced" override section in v1; raw editing returns
   only if a real user request comes up).
7. cfg.json round-trip: add `LaneTrigger` array to persisted config.

### Phase 4 — Tests

**Files:** `hdl/ila/sim/tb/ila_tb_top.sv` (or peer),
`test/ila_driver_test.cpp`, `test/support/mock_ila_tap.h`.

1. HDL sim:
   - Rising edge on `data[0]` triggers when `rise_mask=0x1`.
   - Falling edge on `data[15]` triggers when `fall_mask=0x8000`.
   - Combined level + edge: `data[31:16]==0x1234` AND rising on `data[0]`.
   - Legacy mode (`rise=fall=0`) behaves exactly as before.
2. C++ unit tests:
   - `ConfigureTriggerWritesAllFourRegisters`
   - `ConfigureTriggerLegacyVersionSkipsEdge` (caps.version=0x01)
   - `ComputeTriggerRegistersForLaneCombos` (host-side helper test)

### Phase 5 — Bitstream + hardware validation

1. Rebuild Vivado bitstream (`ila_bringup_top.sv` lanes unchanged).
2. Program Zybo Z7020.
3. Validate:
   - Rising edge on `data[15]` triggers reliably.
   - Level condition `data[31:16] == 0x1234` triggers.
   - Combined condition (rising on `data[0]` AND `data[31:16]==0x1234`).
   - Legacy hex-mask code path (manual register write) still functions.

---

## Notes / Risks

- **`prev_data` does not exist today.** It is a new register added inside
  `ila_trigger.sv`, gated by `valid_in`. This is the single most important
  delta from the v1 plan (which incorrectly assumed it existed).
- **Both `ila_top` and `ila_bscane2_top` instantiate `ila_trigger`** — both
  paths must be updated. CDC synchronizers (ASYNC_REG 2-FF) must be added in
  each top to bring `rise_mask` / `fall_mask` from `tck` to `sample_clk`,
  mirroring the existing `mask_sc` / `val_sc` pattern.
- **Backward compatibility**: legacy bitstreams report `version=0x01`; the host
  refuses to send non-zero rise/fall to such targets but still accepts level
  triggers. New host + old bitstream combo therefore continues to work.
- **DATA_W > 32**: edge masks are 32-bit DR registers — same constraint as the
  existing `mask`/`value`. Future widening is a separate work item.
- **Trigger semantics doc**: the multi-lane AND-of-level / OR-of-edge rule
  must appear in `OwlTAP_ILA_仕様書.md` once validated.

---

## Commit Strategy

- Plan v2 committed before implementation starts (this revision).
- Phase 1 (RTL only) committed when HDL sim passes.
- Phase 2 + 3 (host + GUI) committed together when unit tests pass.
- Phase 5 (validation) is a follow-up commit with any required tweaks.
