# Plan: ILA Embedded Waveform View — 2026-04-24

## Goal

Embed an ImPlot-based waveform display directly inside `IlaPanel` instead of
routing samples through `WaveformView` (which uses ms-scale timestamps and
is incompatible with 8 ns ILA samples).

## Changes

### Step 1 — `ila_panel.h`
- Remove `SampleCallback` / `sample_cb_` (no longer needed externally).
- Remove `preSamples()` getter (used only by callback).
- Add `drawWaveform()` private helper declaration.
- Add `wave_x_` / `wave_y_` member arrays for ImPlot data (pre-allocated at
  kDepth floats each).
- Increase default window height hint to accommodate the plot.

### Step 2 — `ila_panel.cpp`
- Add `#include <implot.h>`.
- After `doRead()` populates `samples_`, fill `wave_x_[i]` and `wave_y_[i]`:
  - `wave_x_[i]` = sample index in nanoseconds: `(int)i * 8.0f`  (8 ns/sample @ 125 MHz)
  - `wave_y_[i]` = `(float)samples_[i]`
- In `draw()`, add a `drawWaveform()` call below the sample-count line.
- `drawWaveform()` uses `ImPlot::PlotLine("ILA_DATA", wave_x_, wave_y_, n)`.
  - X axis label: "Time (ns)", Y axis: hex formatter via `ImPlot::SetupAxisFormat`.
  - Trigger cursor at `wave_x_[pre_samples_]` via `ImPlot::PlotVLines`.
  - Window expanded to `ImVec2(600, 500)` first-use hint.

### Step 3 — `app_window.cpp`
- Remove the `setSampleCallback` block (and its `BusDefinition` / `SampleFrame`
  conversion code) — IlaPanel now owns its own waveform display.
- `setBscaneChain()` call stays; only the callback wiring is removed.
- Remove `#include <chrono>` if it becomes unused (check first).

## Non-changes
- `WaveformView` untouched.
- `SampleCallback` typedef and `setSampleCallback()` can stay for future external
  use, but `sample_cb_` won't be set from `app_window.cpp` anymore.
- No BUILD.bazel changes needed (`implot` already in deps).

## Acceptance Criteria
- Build: 0 errors.
- IlaPanel window shows ImPlot graph after "Read Samples".
- X axis in nanoseconds; Y axis shows hex-formatted counter values.
- Trigger cursor (vertical line) at pre_samples_ position.
- JTAG WaveformView unaffected (boundary scan still works).
