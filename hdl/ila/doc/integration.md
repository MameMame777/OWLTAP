# OwlTAP Internal Logic Analyzer — Integration Guide

Vendor-neutral SystemVerilog ILA IP accessed over a dedicated IEEE 1149.1
TAP controller. Designed to be daisy-chained on the same JTAG cable used by
the FPGA primary TAP.

## Status

**v1 — placeholder IDCODE.** The default parameter
`IDCODE_VAL = 32'hA17A_0001` is a development placeholder. Before releasing
hardware to others you MUST either:

1. Obtain a JEDEC manufacturer ID and encode a proper 32-bit IDCODE in the
   1149.1 format (`{version[3:0], part[15:0], mfg[10:0], 1'b1}`), or
2. Document clearly in your board bring-up notes that the IDCODE is
   non-unique and conflicts with any other device using the same value.

## Module summary

| Module | Role |
|--------|------|
| `ila_top` | Top-level integration. Instantiate this. |
| `ila_tap` | IEEE 1149.1 TAP FSM + IR (5 bits) + all DRs. |
| `ila_trigger` | `(data_in & mask) == value` single-match comparator. |
| `ila_capture_fsm` | Pre/post capture state machine + write pointer. |
| `ila_bram` | Dual-clock BRAM: write@sample_clk, read@tck. |
| `pulse_sync` | Toggle-based 1-bit pulse CDC (tck → sample_clk). |

## Parameters

| Name | Default | Notes |
|------|---------|-------|
| `DATA_W` | `32` | Capture width in bits. |
| `DEPTH` | `1024` | Number of samples. Must equal `1 << ADDR_W`. |
| `ADDR_W` | `10` | `$clog2(DEPTH)`. |
| `IDCODE_VAL` | `32'hA17A_0001` | JTAG IDCODE. Override for production. |

## Instantiation

```systemverilog
ila_top #(
    .DATA_W(32), .DEPTH(1024), .ADDR_W(10),
    .IDCODE_VAL(32'hA17A_0001)
) u_ila (
    // Dedicated JTAG pins (or chained behind main TAP, see below)
    .tck          (jtag_tck),
    .trst_n       (jtag_trst_n),   // tie high if unused
    .tms          (jtag_tms),
    .tdi          (jtag_tdi_to_ila),
    .tdo          (jtag_tdo_from_ila),

    // User capture interface
    .sample_clk   (user_clk),
    .sample_rst_n (user_rst_n),
    .data_in      (signals_to_capture),
    .data_valid   (1'b1)            // qualify with an enable if needed
);
```

## JTAG chain wiring

The ILA presents one additional TAP. To chain it after an existing FPGA TAP:

```
cable_tck  ──┬─► FPGA.TCK       ├─► ila_top.tck
             │
cable_tms  ──┼─► FPGA.TMS       ├─► ila_top.tms
             │
cable_tdi  ──► FPGA.TDI
FPGA.TDO   ──► ila_top.tdi
ila_top.tdo──► cable_tdo
```

IR length: **5 bits**. Report it to host tools accordingly. OwlTAP's
`JtagChain::detectDevices` will recognize the ILA via IDCODE; pre-populate
`ChainDevice.ir_length = 5` for the entry.

## Instruction register (IR_W = 5)

| Opcode | Mnemonic | DR width | Access | Description |
|--------|----------|----------|--------|-------------|
| `5'h01` | `IDCODE` | 32 | R | Device IDCODE (default on TLR). |
| `5'h08` | `CTRL` | 4 | W | Bit0=ARM, Bit1=STOP, Bit2=RESET, Bit3=FORCE_TRIG. Each bit is a single-cycle pulse at Update-DR. |
| `5'h09` | `STATUS` | 8 | R | `{5'b0, full, triggered, armed}`. |
| `5'h0A` | `TRIG_MASK` | 32 | R/W | Comparator mask. |
| `5'h0B` | `TRIG_VAL` | 32 | R/W | Comparator value. |
| `5'h0C` | `READ_ADDR` | 10 | R/W | BRAM read pointer. |
| `5'h0D` | `READ_DATA` | 32 | R | Read current BRAM word; auto-increments `READ_ADDR` at Update-DR. |
| `5'h0E` | `PRE_SAMPLES` | 16 | R/W | Pre-trigger sample count. Bits above `ADDR_W` ignored. |
| `5'h1F` | `BYPASS` | 1 | — | Mandatory 1149.1 BYPASS. |

## Operating sequence

1. **Reset**: drive TMS=1 for 5 TCK cycles (Test-Logic-Reset). IR resets to
   `IDCODE`.
2. **Read IDCODE**: Shift-DR 32 bits; verify match.
3. **Configure**:
   a. Load IR = `TRIG_MASK`, Shift-DR 32-bit mask.
   b. Load IR = `TRIG_VAL`, Shift-DR 32-bit value.
   c. Load IR = `PRE_SAMPLES`, Shift-DR desired count (e.g. `DEPTH/4`).
4. **Arm**: Load IR = `CTRL`, Shift-DR `4'b0001`.
5. **Poll**: Load IR = `STATUS`, repeatedly Shift-DR 8 bits; wait for
   `full == 1`.
   - Optionally `force_trig` via `CTRL = 4'b1000` to force a trigger.
   - `stop` (`CTRL = 4'b0010`) freezes capture wherever it is.
6. **Read**:
   a. Load IR = `READ_ADDR`, Shift-DR `trigger_addr - pre_samples` (mod
      DEPTH) to start reading the oldest pre-trigger sample.
   b. Load IR = `READ_DATA`.
   c. Shift-DR 32 bits × `DEPTH` times. `READ_ADDR` auto-increments after
      each DR update, so a single IR setting suffices for the whole buffer.

## Clock-domain policy

| Crossing | Mechanism |
|----------|-----------|
| `ctrl_*` pulses (tck → sample_clk) | `pulse_sync` (toggle + 2-FF sync + edge detect). |
| `mask`, `value`, `pre_samples` (tck → sample_clk) | 2-FF sync on each bit with `ASYNC_REG=TRUE`. Caller must leave values stable before asserting `arm`. |
| `armed`, `triggered`, `full` (sample_clk → tck) | 2-FF sync. |
| BRAM read data (sample_clk → tck) | Dual-port BRAM; read port naturally in tck domain. Caller must ensure capture has stopped (`full==1` or `stop` issued) before reading to avoid racing with write pointer. |

Apply `hdl/ila/constraints/ila_clocks.xdc` (or the equivalent for your
tool) to declare TCK vs. sample_clk as asynchronous groups and to
false-path the CDC flops.

## Limitations (v1)

- Single-stage trigger (no sequencers, no counters).
- Always-sample (no storage qualifier).
- IR length fixed at 5.
- No data compression.
- No cross-trigger between instances.

## References

- IEEE Std 1149.1-2013 — Standard Test Access Port and Boundary-Scan
  Architecture.
- OwlTAP plan: `docs/plan/plan_internal_logic_analyzer_20260424.md`.
