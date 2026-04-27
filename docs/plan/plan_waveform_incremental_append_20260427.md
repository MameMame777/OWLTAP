# Plan: Incremental WaveformView Append

**Date**: 2026-04-27  
**Problem**: `WaveformView::setData()` rebuilds all lane data from scratch on every `refreshFromCapture()` call (~50ms interval), processing up to 10,000 samples × N signals even when only 1-5 new frames arrived. This causes FPS drops proportional to buffer size × signal count.

## Root Cause

```
refreshFromCapture() [50ms interval]
  getNewSamples() -> 1-5 new frames
  cached_samples_.push_back(new frames)       // O(1)
  syncSelectionViews(cached_samples_)          // O(N*M) full rebuild
    -> WaveformView::setData(all 10,000 frames)
       lanes_.clear()
       for each signal: iterate all 10,000 samples  // bottleneck
```

## Solution: `WaveformView::appendData()`

New method that appends only new frames to existing lanes, evicts oldest points if ring buffer is full.

Full rebuild (`setData`) is called only when:
- Signal/bus selection changes
- Capture starts (initial data)
- `requestFit()` triggers a repaint of the full buffer

## Implementation Steps

### 1. `waveform_view.h`
Add to private static members:
```cpp
static std::chrono::steady_clock::time_point t0_;  // timestamp of first sample
static std::vector<std::string> lane_signals_;      // signal names used to build lanes
static std::vector<std::string> lane_bus_names_;    // bus names used to build lanes
```
Add public static method:
```cpp
/// Incrementally append new frames to existing lanes.
/// Evicts `evict_front_count` oldest points from the front of each lane.
/// Returns false if signal/bus config changed (caller must call setData instead).
static bool appendData(const std::vector<std::string>& signals,
                       const std::vector<BusDefinition>& buses,
                       const std::vector<jtag::SampleFrame>& new_frames,
                       size_t evict_front_count);
```
Add `#include <chrono>` to header.

### 2. `waveform_view.cpp`
- `setData()`: store `t0_`, `lane_signals_`, `lane_bus_names_` after building lanes
- `clearData()`: reset `lane_signals_`, `lane_bus_names_`, `t0_`
- Implement `appendData()`:
  1. Compare signals/bus names against stored → return false if mismatch
  2. Evict `evict_front_count` points from front of each lane (erase first N)
  3. For each new frame: compute `us = (frame.timestamp - t0_)`, push to each lane
  4. Update `latest_time_`

### 3. `app_window.cpp` — `refreshFromCapture()` direct path
Replace:
```cpp
syncSelectionViews(cached_samples_);
```
With:
```cpp
const auto selected = SignalPanel::selectedSignals();
const auto& buses   = SignalPanel::buses();
if (!WaveformView::appendData(selected, buses, new_frames, evict_count)) {
    syncSelectionViews(cached_samples_);  // fallback: selection changed
}
HexPanel::setBuses(buses);
HexPanel::updateValues(cached_samples_.back().data, selected);
```

## Performance Impact

| Before | After |
|--------|-------|
| O(N_buffer × N_signals) every 50ms | O(N_new × N_signals) every 50ms |
| 10,000 × 5 = 50,000 ops/50ms | 5 × 5 = 25 ops/50ms |

Full rebuild (~50,000 ops) only occurs on signal selection change (rare) or capture start.

## Risk / Fallback
- If `appendData()` returns false, falls back to `syncSelectionViews()` (existing behavior)
- Existing `setData()` unchanged; all other callers unaffected
- No changes to CaptureEngine or Scanner

## Files Changed
- `src/gui/waveform_view.h`
- `src/gui/waveform_view.cpp`
- `src/gui/app_window.cpp`
