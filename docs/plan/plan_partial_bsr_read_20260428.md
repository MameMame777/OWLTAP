# Plan: Partial BSR Read Optimization

**Date**: 2026-04-28  
**Feature**: Reduce JTAG BSR scan time when only a subset of pins is selected

## Problem

The Zynq XA7Z020 BSR is **1077 bits** long.  
Every `Scanner::sample()` call shifts out all 1077 bits regardless of how many pins are in `decode_filter_`.  
For 2 pins (IO_M14 at cell 340, IO_M15 at cell 337 in CLG400), we only need **341 bits** — a **~3.2x** reduction in DR clock cycles.  
This is the primary FPS bottleneck.

## Solution: Partial BSR Read

During SHIFT-DR for SAMPLE instruction:
- Only clock out bits **0 … (maxCellPos)** inclusive.
- `CAPTURE-DR` latches all 1077 bits simultaneously (instantaneous).
- Exiting SHIFT-DR early (then UPDATE-DR) is safe because SAMPLE mode does not drive I/O.

## Steps

### Step 1 — `JtagChain::readBSR` partial-read parameter

**File**: `src/jtag/jtag_chain.h` and `.cpp`

Add optional parameter:
```cpp
bool readBSR(int device_index, std::vector<uint8_t>& bsr_data,
             int partial_bits = 0);
```
- `partial_bits == 0` → read full BSR (current behaviour, backward compatible).
- `partial_bits > 0 && < bsr_len` → read only `partial_bits` bits.
- `partial_bits >= bsr_len` → read full BSR.

Implementation: compute `read_len = (partial_bits > 0 && partial_bits < bsr_len) ? partial_bits : bsr_len` and use it instead of `bsr_len` when calling `tap_.readDR`.

Output `bsr_data` is sized to `(read_len + 7) / 8` bytes.  
Cells beyond `read_len` return 0 from `ScanResult::getBit()` (already handled by bounds check).

### Step 2 — `Scanner::maxNeededBsrBits()` helper

**File**: `src/boundary_scan/scanner.h` and `.cpp`

```cpp
/// Returns (max cell position across all cells of filtered pins) + 1.
/// Returns 0 if decode_filter_ is empty (meaning: read full BSR).
int maxNeededBsrBits() const;
```

Algorithm:
1. If `decode_filter_` is empty → return 0.
2. Iterate `bsdlDevice()->boundary_cells`.
3. For each cell where `cell.hasPin() && decode_filter_.count(cell.pin_name)`, track `max_pos`.
4. Return `max_pos + 1`.

### Step 3 — `Scanner::sample()` use partial read

**File**: `src/boundary_scan/scanner.cpp`

Replace:
```cpp
if (!chain_.readBSR(device_index_, result.raw_bsr)) {
```
with:
```cpp
const int partial = maxNeededBsrBits();
if (!chain_.readBSR(device_index_, result.raw_bsr, partial)) {
```

### Step 4 — Unit test

**File**: `test/scanner_test.cpp`

Add `ScannerTest.DecodePartialBsrFilterIsCorrect`:
- Build a BSDLDevice with 8 cells, pins at positions 2, 5, 7.
- Call `decodeBoundaryScan` with a 1-byte `raw_bsr` (only bits 0..4 valid, rest zero).
- Apply filter for the pin at position 2; verify correct decode.
- Verify pin at position 5 (beyond partial) decodes as LOW (0 from getBit).
- This tests that `decodeBoundaryScan` handles partial BSR correctly.

## Impact

| Scenario | Before | After |
|----------|--------|-------|
| 2 pins selected (M14/M15, CLG400) | 1077 bits | ~341 bits |
| 10 pins spread across full BSR | 1077 bits | up to 1077 bits |
| No filter (show all pins) | 1077 bits | 1077 bits (unchanged) |

## Risk

- `UPDATE-DR` after partial SHIFT-DR writes stale shift values back into low BSR cells.  
  **Acceptable**: SAMPLE mode does not forward BSR to I/O pads. Next CAPTURE-DR overwrites before any harm.
- `readBSR` callers in `pin_driver.cpp` use `partial_bits = 0` (default) → no change.

## Verification

`bazel test //test/...` — all existing + new scanner test must pass.
