# Plan: BSCANE2 Multi-Instance ILA (USER1..USER4)

Date: 2026-04-28

## Goal

Support up to 4 simultaneous `ila_bscane2_top` instances in one design by
parameterizing the `JTAG_CHAIN` value (1..4) in both HDL and host software.
Deliver a Zybo Z7-20 example bitfile with all four USER chains populated.

## Background

Xilinx 7-series / Zynq PL TAP exposes four dedicated BSCANE2 scan chains:

| JTAG_CHAIN | PL TAP IR (6-bit) | OwlTAP constant |
|---|---|---|
| 1 (USER1) | `6'h02` | `kUserOpcodes[1]` |
| 2 (USER2) | `6'h03` | `kUserOpcodes[2]` |
| 3 (USER3) | `6'h22` | `kUserOpcodes[3]` |
| 4 (USER4) | `6'h23` | `kUserOpcodes[4]` |

Current code hard-codes `JTAG_CHAIN(1)` (HDL) and `kUser1Opcode = 0x02` (C++).

## Constraints

- `DEPTH == 2**ADDR_W` must hold.
- Vivado DRC: each `JTAG_CHAIN` value (1..4) may appear at most once per design.
- Backward compatibility: all existing callers default to USER1.

## Implementation Steps

### Step 1 — HDL: parameterize `ila_bscane2_top.sv`

Add `parameter int JTAG_CHAIN = 1` (range 1..4).  
Change `BSCANE2 #(.JTAG_CHAIN(1))` → `BSCANE2 #(.JTAG_CHAIN(JTAG_CHAIN))`.

File: `hdl/ila/rtl/ila_bscane2_top.sv`

### Step 2 — HDL: parameterize `bscane2_model.sv`

Replace `localparam USER1_OPCODE = 6'h02` with a lookup:

```
JTAG_CHAIN=1 → 6'h02
JTAG_CHAIN=2 → 6'h03
JTAG_CHAIN=3 → 6'h22
JTAG_CHAIN=4 → 6'h23
```

File: `hdl/ila/sim/tb/bscane2_model.sv`

### Step 3 — HDL: new 4-ILA example

Directory: `hdl/ila/examples/zybo_z7020_4ila/`

Files:
- `ila_4ila_bringup_top.sv`  — 4 instances (USER1..USER4), each captures
  the same 32-bit free-running counter with distinct IDCODE_VAL and DEPTH.
- `ila_4ila_bringup.xdc`  — clock, LED, CDC constraints.
- `create_project.tcl`  — interactive project creation.
- `build_bitstream.tcl`  — batch build to bitstream.

IDCODE assignment:
- u_ila0 (USER1): `0xA17A_0001`, DEPTH=256
- u_ila1 (USER2): `0xA17A_0002`, DEPTH=512
- u_ila2 (USER3): `0xA17A_0003`, DEPTH=1024
- u_ila3 (USER4): `0xA17A_0004`, DEPTH=2048

### Step 4 — C++: `BscaneIlaTapBackend` constructor

Add `int user_chain = 1` parameter (validated 1..4).  
Add static opcode lookup table.  
Replace `kUser1Opcode` with `userOpcode(user_chain)` in `selectIr`.

Files: `src/ila/bscane_ila_tap_backend.h`, `.cpp`

### Step 5 — C++: `daemon_runner.cpp`

Read `user_chain` from JSON params (default 1) and pass to constructor.

File: `src/tools/daemon_runner.cpp`

### Step 6 — C++: `register_tools.cpp`

Same: read `user_chain` from tool params, pass to constructor.  
Expose `user_chain` in tool schema where `use_bscane` is already exposed.

File: `src/mcp/tools/register_tools.cpp`

### Step 7 — C++: `IlaPanel::setBscaneChain`

Add `int user_chain = 1` parameter.

Files: `src/gui/ila_panel.h`, `src/gui/ila_panel.cpp`

## Verification

- `bazel build //src:jtag_viewer` must succeed.
- `bazel test //test/...` must pass (no test changes expected; existing
  `ila_driver_test` uses mock backend, not `BscaneIlaTapBackend` directly).
- Vivado synthesis of `zybo_z7020_4ila` must produce DRC-clean output.

## Non-goals

- No GUI control for selecting USER chain (that would require UX redesign).
- No changes to simulation testbench infrastructure beyond model parameterization.
