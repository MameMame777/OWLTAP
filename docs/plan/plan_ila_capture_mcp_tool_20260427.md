# Plan: ILA Capture MCP Tool + Hardware Smoke Test

**Date:** 2026-04-27  
**Status:** Committed

## Objective

Add a single MCP tool `ila_run_capture` that performs a complete ILA capture
sequence (reset → configure trigger → arm → optional force-trigger → poll
until full → read samples) in one synchronous call, and add a corresponding
hardware smoke test `test_ila_capture` in `test_mcp_full.py`.

## Motivation

`read_ila_status` exists but there is no way to actually acquire waveform data
via MCP.  The smoke test only needs to verify that data *can* be retrieved;
it does not need to test trigger conditioning against real signals.

## Approach

Use `forceTrigger()` to fire the trigger unconditionally after arming,
avoiding a dependency on real stimulus.  This is sufficient for a retrieval
smoke test.

## Steps

1. **Plan file** — this document (`docs/plan/plan_ila_capture_mcp_tool_20260427.md`)

2. **MCP tool** — append `ila_run_capture` (tool 16) to
   `src/mcp/tools/register_tools.cpp`.
   - Parameters: `device_index` (int), `use_bscane` (bool, default false),
     `force` (bool, default true), `pre_samples` (int, default 0),
     `timeout_ms` (int, default 5000)
   - Sequence inside `bridge.submitSync()`:
     1. `ila.probe(caps)` — read capabilities
     2. `ila.resetCapture()` — clear any previous run
     3. `ila.configureTrigger(0, 0, 0)` — match-all, no pre-samples
     4. `ila.arm()`
     5. If `force` → `ila.forceTrigger()`
     6. Poll `ila.readStatus()` until `full==true` or timeout
     7. `ila.setReadAddr(0)` + `ila.readSamples(samples)`
   - Returns: `{version, depth, data_width, sample_count, samples: [hex...]}`

3. **Test function** — add `test_ila_capture()` to `test_mcp_full.py` after
   `test_read_ila_status()`.
   - Calls `ila_run_capture` with `device_index=0, use_bscane=True, force=True`
   - Validates: `sample_count == depth`, `samples` is a non-empty list

4. **Wire test** — insert the test call in the HW test sequence in `main()`.

5. **Build check** — `bazel build //src:jtag_viewer` must succeed with no
   new warnings.

## Files Changed

| File | Change |
|------|--------|
| `src/mcp/tools/register_tools.cpp` | Add tool 16: `ila_run_capture` |
| `test_mcp_full.py` | Add `test_ila_capture()` + register in HW sequence |

## Non-Goals

- No new C++ unit test (IlaDriver is already tested in `ila_driver_test.cpp`)
- No new MCP tools for individual arm/stop/read operations
- No trigger condition testing against real signals
