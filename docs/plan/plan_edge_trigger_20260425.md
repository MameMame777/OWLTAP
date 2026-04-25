# Plan: Edge Trigger Extension

**Date:** 2026-04-25  
**Status:** Draft

## Objective

Extend the ILA trigger system to support per-signal edge (Rise/Fall) conditions
in addition to the existing level (mask/value) comparison.  The user selects a
signal lane and specifies an edge type; the host computes the hardware register
values automatically.  RTL is extended with two new DR registers so edge
detection is performed in the FPGA's sample clock domain (100% reliable even at
high capture rates).

---

## Current State

| Register | IR   | Width | Notes |
|----------|------|-------|-------|
| TRIG_MASK | 0x0A | 32 | bits to compare |
| TRIG_VAL  | 0x0B | 32 | expected value |
| PRE_SAMPLES | 0x0E | 10 | |

Trigger fires when: `(data & TRIG_MASK) == (TRIG_VAL & TRIG_MASK)`

No edge detection.  Host must configure raw hex mask/val manually.

---

## New Architecture

### Hardware trigger equation

```
level_ok  = (TRIG_MASK == 0)
          || ((data & TRIG_MASK) == (TRIG_VAL & TRIG_MASK))

rising    = ~prev_data & data       // bits that went 0->1
falling   =  prev_data & ~data      // bits that went 1->0

edge_ok   = (TRIG_RISE_MASK == 0 && TRIG_FALL_MASK == 0)
          || ((rising  & TRIG_RISE_MASK) != 0)
          || ((falling & TRIG_FALL_MASK) != 0)

TRIGGER   = level_ok && edge_ok
```

`prev_data` is the sample registered on the previous capture clock cycle in
`ila_core.sv` (sample_clk domain) — it already exists as the shift-register
pipeline stage.

### New IR opcodes

| IR   | Name           | Width | Description |
|------|----------------|-------|-------------|
| 0x0F | TRIG_RISE_MASK | 32    | Rising-edge bitmask |
| 0x10 | TRIG_FALL_MASK | 32    | Falling-edge bitmask |

IR space 0x04–0x07, 0x11–0x1E remain unallocated.

### GUI per-lane trigger condition

Each signal lane gets a `TriggerCondition` combo box:

| Option      | Meaning |
|-------------|---------|
| None        | Not used |
| == value    | Level: mask covers the lane's bits; value set from text field |
| != value    | Level: val inverted in the lane's bits |
| Rising edge | `TRIG_RISE_MASK` bits set for the lane |
| Falling edge| `TRIG_FALL_MASK` bits set for the lane |
| Either edge | Both RISE and FALL masks set for the lane |

Arm button auto-computes all four registers from the per-lane settings and calls
`configureTrigger()`.

---

## Implementation Phases

### Phase 1 — RTL: TRIG_RISE_MASK + TRIG_FALL_MASK + prev_data

Files: `hdl/ila/rtl/ila_tap.sv`, `hdl/ila/rtl/ila_bscane2_top.sv`,
       `hdl/ila/rtl/ila_core.sv` (or wherever trigger eval lives)

Changes:
1. Add `IR_TRIG_RISE = 5'h0F` and `IR_TRIG_FALL = 5'h10` localparams
2. Add `rise_mask_reg`, `fall_mask_reg` DR registers (same pattern as mask_reg)
3. Add new IR handling to CAPTURE/SHIFT/UPDATE cases
4. Add new outputs `trig_rise_mask`, `trig_fall_mask` from `ila_tap`
5. In `ila_top`/`ila_bscane2_top`: receive new outputs and pass to core trigger logic
6. In sample_clk capture logic: add `prev_data` register; modify trigger to
   include edge conditions

### Phase 2 — Host driver: extend configureTrigger()

Files: `src/ila/ila_driver.h`, `src/ila/ila_driver.cpp`

Changes:
1. Add `kIrTrigRise = 0x0F`, `kIrTrigFall = 0x10` to `Ir` enum
2. Add `configureTrigger(mask, val, rise_mask, fall_mask, pre_samples)` overload
   (old 3-arg version kept for backward compat, sets rise/fall = 0)
3. Extend `bscane_ila_tap_backend` / `chain_ila_tap_backend` if needed
   (transparent — same shiftDr mechanism)

### Phase 3 — GUI: per-lane trigger editor

Files: `src/gui/ila_panel.h`, `src/gui/ila_panel.cpp`

Changes:
1. Add `TriggerCond` enum: `{NONE, EQ, NEQ, RISE, FALL, EITHER}`
2. Add `struct LaneTrigger { TriggerCond cond; uint32_t value; }` per signal lane
3. Store `std::vector<LaneTrigger> lane_triggers_` sized to match `signals_`
4. Replace raw hex mask/val input with per-lane combo boxes in `drawSignalEditor()`
   - Combo: None / == / != / Rise / Fall / Either
   - InputHex for value (shown only for == and !=)
5. `computeTriggerRegisters()`: iterate `lane_triggers_` to compute
   `trig_mask`, `trig_val`, `trig_rise_mask`, `trig_fall_mask`
6. `doArm()`: call new 5-arg `configureTrigger()`
7. Keep raw hex inputs as an "Advanced" collapsible section for override

### Phase 4 — Build bitstream + validate

1. Rebuild Vivado bitstream for `ila_bringup_top.sv`
2. Program board, validate:
   - Rising edge on `data[15]` triggers reliably
   - Level condition `data[31:16] == 0x1234` triggers

---

## Notes / Risks

- **prev_data register**: must be registered in the sample_clk domain, gated by
  `data_valid`. Already available in the capture FIFO; confirm exact signal name
  before editing.
- **`ila_bscane2_top` lacks prev_data**: it passes `data_in` directly to the core.
  The prev_data register must live in `ila_core` or be added there.
- **Backward compatibility**: old bitstreams (VERSION=0x01) still work; host
  sends rise/fall = 0 which disables edge detection cleanly.
- **IR space**: 0x0F and 0x10 are currently unallocated — safe to use.

---

## Commit Strategy

- Phase 1 (RTL) + Phase 2 (driver) committed together after Phase 4 validation
- Plan file committed now as reference
