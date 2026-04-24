# Plan: ILA Signal Lanes — Signal Selection & BUS Grouping
**Date:** 2026-04-25  
**Status:** In Progress

## Goal
Allow ILA waveform display to decompose the captured `data_w`-bit word into
named sub-fields, each rendered as its own lane in the waveform plot.
Default single-field lane is auto-populated from CONFIG register (`data_w`).

**Test target:** 32-bit → [31:16] 16-bit, [15:8] 8-bit, [7:4] 4-bit,
[3:2] 2-bit, [1:1] 1-bit, [0:0] 1-bit — all user-configurable via the editor.

## Design

### New Type: `IlaSignalDef` (in `ila_panel.h`)
```cpp
struct IlaSignalDef {
    char      name[32] = {};   // display label
    int       hi  = 31;        // MSB bit index (inclusive, 0-based)
    int       lo  = 0;         // LSB bit index (inclusive, 0-based)
    BusFormat fmt = BusFormat::HEX;

    int      width()                  const;  // hi - lo + 1
    uint32_t extract(uint32_t sample) const;  // shift+mask
};
```
- Width == 1 → digital (step-function) lane
- Width > 1  → bus (colored block) lane with formatted hex/dec/bin label

### New Members in `IlaPanel` (private)
```
std::vector<IlaSignalDef> signals_;
void drawSignalEditor();   // collapsible table for add/edit/remove
void resetSignalDefs();    // populate default from IlaCaps.data_w
```

### Default (after `probe()`)
- One signal: `data[dw-1:0]`, full width, HEX format
- "Reset from CAPS" button regenerates this default

### Multi-lane `drawWaveform()` layout
```
Y axis range: [0, n_lanes]
Lane 0 (top): y_hi = n_lanes - 0.1, y_lo = n_lanes - 0.9  (tick at n_lanes - 0.5)
Lane k:       y_hi = n_lanes - k - 0.1, y_lo = n_lanes - k - 0.9
```
- Y axis ticks labeled with `signals_[i].name` via `ImPlot::SetupAxisTicks`
- Plot height ≈ 28 px × n_lanes + 20 px (floor 80 px)
- Color palette: 6 cyclic cornflower-blue / green / orange / pink / teal / violet
- 1-bit lane: horizontal line at y_hi or y_lo + transition verticals
- Multi-bit lane: filled rect + hex/dec/bin label when block wide enough
- Trigger cursor: red vertical line at `wave_x_[pre_samples_]`

## Files to Modify
| File | Change |
|------|--------|
| `src/gui/ila_panel.h` | Add `#include "app_config.h"`, `IlaSignalDef` struct, `signals_` member, two private methods |
| `src/gui/ila_panel.cpp` | `resetSignalDefs()`, `drawSignalEditor()`, rewrite `drawWaveform()`, wire in `setChain`/`setBscaneChain`/`draw()` |

No new files, no BUILD.bazel changes needed (`app_config` is already a dep via `gui` target).

## Implementation Steps
1. [x] Write plan file
2. [ ] Extend `ila_panel.h`: IlaSignalDef, members, declarations
3. [ ] Implement `resetSignalDefs()` + `drawSignalEditor()` in ila_panel.cpp
4. [ ] Rewrite `drawWaveform()` for multi-lane
5. [ ] Wire: call `resetSignalDefs()` in `setChain`/`setBscaneChain`; add `drawSignalEditor()` call in `draw()`
6. [ ] Build, fix any compile errors

## Signal Editor UI (collapsible, always visible when hw_ok)
```
[v] Signal Definitions   [Reset from CAPS]  [+ Add]
┌────────────┬────┬────┬───┬─────┬──┐
│ Name       │ Hi │ Lo │ W │ Fmt │  │
├────────────┼────┼────┼───┼─────┼──┤
│ data[31:16]│ 31 │ 16 │16 │ HEX │X │
│ data[15:8] │ 15 │  8 │ 8 │ HEX │X │
│ data[7:4]  │  7 │  4 │ 4 │ HEX │X │
│ data[3:2]  │  3 │  2 │ 2 │ HEX │X │
│ data[1]    │  1 │  1 │ 1 │ BIN │X │
│ data[0]    │  0 │  0 │ 1 │ BIN │X │
└────────────┴────┴────┴───┴─────┴──┘
```
