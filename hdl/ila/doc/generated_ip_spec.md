# OwlTAP ILA Generated IP Specification

This document defines the first supported output format of the OwlTAP ILA generator.

## Status

Version 1.

## Scope

The generator currently emits a Xilinx BSCANE2-based wrapper around the existing `hdl/ila/rtl/ila_bscane2_top.sv` core.

Supported in this version:
- BSCANE2 only
- Manual signal-lane entry
- `DATA_W` from 1 to 32 bits
- `DEPTH` must be a power of two
- Up to 15 signal lanes
- Vivado helper output with relative-path references only

Not supported in this version:
- Dedicated TAP generation
- `DATA_W > 32`
- Auto-import from XDC/CSV
- Self-contained RTL export outside the repository structure

## Generated file set

The generator writes the following files into the selected output directory:
- `<top_module>.sv`
- `ila_generated.xdc`
- `create_project.tcl`
- `build_bitstream.tcl`
- `README.md`

All generated helper files must refer to repository RTL using relative paths only.
Absolute local machine paths are forbidden.

## Wrapper RTL contract

The generated wrapper module exposes these ports:
- `sample_clk` (or user-selected clock port name)
- `sample_rst_n` (or user-selected reset port name)
- `data_in[DATA_W-1:0]` (or user-selected data port name)
- `data_valid` (or user-selected valid port name)

The wrapper instantiates `ila_bscane2_top` with:
- `DATA_W`
- `DEPTH`
- `ADDR_W = log2(DEPTH)`
- `NUM_CH = 1`
- `IDCODE_VAL`
- `SIG_COUNT`
- `SIG_HI[0:14]`
- `SIG_LO[0:14]`
- `SIG_FMT[0:14]`

Unused signal-definition entries are padded with zero values.

## Lane rules

Each lane contains:
- name
- inclusive `hi`
- inclusive `lo`
- format (`HEX`, `DEC`, `BIN`)

Rules:
- `0 <= lo <= hi < DATA_W`
- lane count must be 1..15
- lane names must be valid identifiers for generated comments and documentation
- overlapping lanes are allowed

## Validation rules

The generator must reject:
- non-power-of-two depths
- `DATA_W < 1` or `DATA_W > 32`
- invalid or empty identifiers for project/top/port names
- absolute or parent-traversing output paths
- out-of-range lane bit indices
- zero lanes
- more than 15 lanes

## Vivado helper rules

`create_project.tcl` must:
- create a project for the requested device part
- add the shared core RTL from `hdl/ila/rtl`
- add the generated wrapper from the output directory
- add the generated XDC
- set the generated wrapper as top module

`build_bitstream.tcl` must:
- source or replicate the project creation flow
- run synthesis and implementation through bitstream generation
- report the expected bitstream path

`ila_generated.xdc` must:
- create a clock on the generated sample clock port
- avoid board-specific constraints unrelated to the ILA core wrapper

## GUI contract

The OWLTAP GUI wizard must:
- let the user edit all supported parameters
- validate before file generation
- show errors inline
- persist useful defaults in `cfg.json`

## Host-side limit

The first generator version is capped at `DATA_W <= 32` because the current host ILA driver and GUI still use `uint32_t` sample and trigger values.
