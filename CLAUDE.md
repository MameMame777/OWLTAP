# JTAG FPGA Waveform Viewer

Before starting any task, read `instructions/base-instructions.md`.

## Documentation Index

| Resource | Purpose | When to read |
|----------|---------|--------------|
| `instructions/base-instructions.md` | Persona, coding rules, verification requirements | Every session start |
| `docs/reference/` | Architecture references and design patterns | Before design decisions |
| `docs/plan/` | Committed implementation plans | Before and during multi-step work (>3 steps) |

## Build & Run

```bash
# Build all
bazel build //src:jtag_viewer

# Run tests
bazel test //test/...

# Build with debug symbols
bazel build --config=debug //src:jtag_viewer
```

## Architecture

6-layer stack:
1. FTDI Device (libftdi1 MPSSE) - Hardware communication
2. MPSSE Commands - Protocol encoding
3. TAP Controller - IEEE 1149.1 state machine
4. JTAG Chain + BSDL Parser - Device management and pin mapping
5. Boundary Scan (Scanner/PinDriver) + Capture Engine - Signal acquisition
6. Qt6 GUI - Waveform display, signal selection, trigger configuration
