# Base Instructions

JTAG FPGA Waveform Viewer project specific instructions.

## Persona

- Respond factually and concisely; do not spend effort on friendliness.
- Provide frank and direct feedback without hesitation.
- Flag blind spots and risks based on facts, not assumptions.
- Validate conclusions rigorously and avoid hallucination.
- Protect confidential data; review security and performance routinely.
- Allocate all available reasoning time.

## Operating Principles

- Produce only minimal, production-quality code with clear English comments when needed.
- Prefer ASCII in new edits unless the file already uses other characters for justified reasons.
- Never undo user changes or existing diffs unless explicitly instructed.
- NEVER start implementation of multi-step features (>3 steps) without a committed plan summary.
- Plan mode: save plan as `docs/plan/plan_<feature>_<YYYYMMDD>.md` before implementation.

## C++ Coding Standards

### Critical Rules (never violate)

- **Standard**: C++17 minimum
- **Build System**: Bazel with bzlmod (MODULE.bazel)
- **Namespaces**: All code in `jtag::` namespace; GUI in `jtag::gui::`
- **RAII**: All resource management via RAII. No raw new/delete in application code.
- **Headers**: Use `#pragma once`. Include what you use.
- **Const correctness**: Prefer `const` references for parameters, mark methods `const` where possible.

### Naming Conventions

| Element | Style | Example |
|---------|-------|---------|
| Class | PascalCase | `TapController`, `CaptureEngine` |
| Method | camelCase | `shiftDR()`, `readIdCode()` |
| Member variable | snake_case with trailing `_` | `buffer_depth_`, `ftdi_device_` |
| Local variable | snake_case | `sample_count`, `tms_bits` |
| Constant | kPascalCase or UPPER_SNAKE | `kTapTransitions`, `MPSSE_TMS_OUT` |
| Enum class | PascalCase values | `TapState::SHIFT_DR`, `PinState::HIGH` |
| Namespace | snake_case | `jtag`, `jtag::gui` |
| File | snake_case | `tap_controller.h`, `capture_engine.cpp` |

### Architecture Patterns

Reference: `docs/reference/InstrDriver-Architecture.md`

| Pattern | Application in this project |
|---------|---------------------------|
| **Layered Architecture** | FTDI -> MPSSE -> TAP -> Chain -> Scanner -> GUI |
| **Strategy** | Communication backend (FtdiDevice wraps libftdi) |
| **Template Method** | BSDLParser parse flow |
| **RAII** | FtdiDevice (ftdi_context lifetime) |
| **Observer** | CaptureEngine callback -> GUI update |
| **Ring Buffer** | CaptureEngine sample storage |

### Thread Safety

- `CaptureEngine` capture thread: uses `std::mutex` for buffer access, `std::atomic` for state/flags.
- GUI thread: all Qt widget operations on main thread only. Use signals/slots for cross-thread communication.
- Never hold a mutex while calling FTDI I/O (potential deadlock with USB stack).

## Bazel Conventions

- One `BUILD.bazel` per source directory
- `cc_library` for each module; `cc_binary` for the final executable
- `cc_test` with `@googletest//:gtest_main` for unit tests
- Third-party wrappers in `third_party/` with system library linkopts
- Visibility: use `//visibility:public` for libraries consumed across packages

## Qt6 Conventions

- Use Q_OBJECT macro in all QObject subclasses
- Prefer signals/slots over callbacks for GUI communication
- MOC files: require custom Bazel rules (see `bazel/qt_rules.bzl`)
- Keep business logic out of widget classes; widgets are for display and interaction only

## Verification Requirements

- Unit tests for all non-GUI, non-hardware modules (BSDL parser, TAP paths, MPSSE encoding, trigger logic)
- Integration testing with mock FTDI device where possible
- No placeholder code or simplified prototypes in production

## Prohibited Actions

- Do not suppress or ignore compilation errors; resolve root causes.
- Do not generate placeholder code or unverifiable logic.
- Do not expose sensitive information.
- Do not begin multi-step implementation without saving a plan file.
