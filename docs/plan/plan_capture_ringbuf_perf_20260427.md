# Plan: Capture Ring Buffer Performance + Buffer Depth UI

**Date**: 2026-04-27  
**Issue**: FPS drops and sample loss as capture count grows

---

## Root Cause

`AppWindow::refreshFromCapture()` calls `CaptureEngine::getSamples()` every 50 ms.
`getSamples()` copies the entire ring buffer under a mutex lock.
Each `SampleFrame` contains a `ScanResult` with `std::map<std::string, PinState>`,
which heap-allocates O(pin_count) nodes per copy.

With `buffer_depth_=10000` and ~100 pins:
- 10000 x ~100 map-node allocations = ~1M allocs per call
- 20 calls/sec = ~20M allocs/sec during capture
- Mutex held for the full copy, blocking the capture thread = sample gaps

---

## Fix Design

### 1. Incremental fetch API (`CaptureEngine`)

Add `std::atomic<size_t> total_written_{0}` (monotonically increasing write counter).
Add method:
```cpp
std::vector<SampleFrame> getNewSamples(size_t& inout_last_total) const;
```
Returns only frames written since `inout_last_total`.
Updates `inout_last_total` to current `total_written_`.
If ring has wrapped (missed > buffer_depth_), falls back to full copy.

### 2. AppWindow incremental update

- Add `size_t capture_last_total_ = 0;` member
- Reset to 0 on every capture start
- `refreshFromCapture()` direct path: call `getNewSamples(capture_last_total_)`
  and `append` to `cached_samples_`; pop front when `> buffer_depth_`
- Daemon path: unchanged (JSON incremental already bounded)

### 3. Buffer depth configuration

- Add `int capture_buffer_depth_ = 10000;` to AppWindow
- Add "Buffer Depth" input to TriggerDialog (shown next to Pre-trigger ratio)
- TriggerDialog: expose via `static int& bufferDepth();` pattern (same style as other fields)
- On `onStartCapture()` (direct): call `capture_engine_->setBufferDepth(capture_buffer_depth_)`
- On daemon `captureStart()`: pass `capture_buffer_depth_` instead of hardcoded 10000

---

## Files Changed

| File | Change |
|------|--------|
| `src/capture/capture_engine.h` | Add `total_written_`, `getNewSamples()` declaration |
| `src/capture/capture_engine.cpp` | Implement `getNewSamples()`, increment `total_written_` in captureLoop |
| `src/gui/app_window.h` | Add `capture_last_total_`, `capture_buffer_depth_` members |
| `src/gui/app_window.cpp` | Use `getNewSamples()`, reset on start, pass buffer depth to engine/daemon |
| `src/gui/trigger_dialog.h` | Add `buffer_depth_` static field and accessor |
| `src/gui/trigger_dialog.cpp` | Add buffer depth input widget |

---

## Acceptance Criteria

- FPS remains stable during continuous FREE_RUN capture
- `refreshFromCapture()` copies only newly-arrived samples (O(new) not O(total))
- User can set buffer depth in Trigger Setup dialog before capture
- Daemon path passes user-configured buffer depth
- Existing tests pass
