---
title: InstrDriver - Modular Instrument Driver Architecture
tags: [cpp, architecture, design-patterns, factory-pattern, scpi, instrument-driver]
date: 2026-03-07
repo: https://github.com/MameMame777/InstrDriver
category: Programming/CPP
status: Reference
---

# InstrDriver - Modular Instrument Driver Architecture

Reference architecture for layered C++ driver design.
Applicable patterns for the JTAG FPGA Viewer project are marked with [JTAG].

## Key Design Patterns

### Layered Architecture [JTAG]

```
FTDI Device (HW comm)  <->  ICommBackend (abstract comm)
MPSSE Commands          <->  SCPI Protocol
TAP Controller          <->  Mainframe (routing/state machine)
JTAG Chain / Scanner    <->  Cassette (domain knowledge)
Capture Engine          <->  Measurement pipeline
Qt6 GUI                 <->  Python/UNO UI layer
```

### Separation of Concerns [JTAG]

| Layer | Responsibility | Knowledge |
|-------|---------------|-----------|
| Scanner/PinDriver | JTAG instruction selection, BSR decode | Domain (FPGA boundary scan) |
| TapController | TAP state transitions, shift operations | Protocol (IEEE 1149.1) |
| FtdiDevice | USB communication, MPSSE init | Infrastructure (libftdi/USB) |

- New FPGA support = new BSDL file only; no code changes needed
- Communication changes (e.g., different FTDI chip) = FtdiDevice only; Scanner unchanged

### Strategy Pattern [JTAG]

FtdiDevice wraps libftdi as communication backend. Could be extended with:
- Mock backend for unit testing without hardware
- Network proxy backend for remote JTAG

### Factory Pattern (reference only)

InstrDriver uses CassetteFactory with self-registration for runtime type creation.
Not directly needed in JTAG project but useful if supporting multiple FPGA families
with different boundary scan implementations.

### Template Method [JTAG]

BSDLParser::parse() defines the overall parse flow:
1. Open source -> 2. Parse entity -> 3. Parse attributes -> 4. Build model

Subclass or extension points at each attribute parser.

### State Persistence (reference only)

InstrDriver persists measurement history as JSON. JTAG project could use similar
approach for saving/loading capture sessions (VCD/CSV export already implemented).

## MSVC-specific Notes

- `/WHOLEARCHIVE` required for static self-registration (if using factory pattern)
- `/std:c++17` flag for C++17 features (already in .bazelrc)

## Test Strategy Mapping

| InstrDriver Level | JTAG Project Equivalent |
|-------------------|------------------------|
| Unit (cassette logic) | BSDL parser, TAP paths, MPSSE encoding, trigger logic |
| Integration (mainframe + cassette) | TapController + FtdiDevice (mock), JtagChain + Scanner |
| System (simulator mode) | Full pipeline with simulated FTDI responses |
| E2E (real hardware) | Real FTDI + real FPGA board |

## Source

Full architecture document: [MameMame777/knowledge-share](https://github.com/MameMame777/knowledge-share)
Path: `Programming/CPP/InstrDriver - Modular Instrument Driver Architecture.md`
