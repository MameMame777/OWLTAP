# Plan: ILA Generator Wizard

**Date:** 2026-04-28
**Status:** In Progress

## Goal
Add a first usable OWLTAP ILA auto-generation flow that launches from the GUI, collects BSCANE2 ILA parameters, and emits a Vivado helper package with relative-path references only.

## Scope
- BSCANE2 only (`ila_bscane2_top`)
- Manual signal-lane entry
- `DATA_W` range: 1..32
- Output files:
  - wrapper SystemVerilog
  - XDC
  - `create_project.tcl`
  - `build_bitstream.tcl`
  - generated README
- HDL-side generated IP specification under `hdl/ila/doc/`

## Steps
1. Add HDL-side generated IP spec document.
2. Add `src/ila/ila_generator.{h,cpp}`.
3. Add `test/ila_generator_test.cpp`.
4. Add GUI wizard dialog in `src/gui/ila_generator_dialog.{h,cpp}`.
5. Wire Tools menu entry and config persistence.
6. Update README.
7. Run `bazel build //src:owltap` and `bazel test //test/...`.

## Constraints
- Generated file references must be relative, never absolute.
- Keep generator logic testable outside GUI.
- Do not widen host ILA sample types beyond current `uint32_t` in this phase.
