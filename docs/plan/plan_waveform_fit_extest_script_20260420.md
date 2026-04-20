# Waveform Fit + EXTEST + Script Plan

## Scope

Implement the roadmap in stages. Start with a manual waveform Fit action in the GUI, then harden the EXTEST backend for generic BSDL-driven control, and finally expose deterministic scripting through a tested core engine that is surfaced in the GUI.

## Decisions

- Waveform fit is manual, user-invoked, not automatic.
- The first scripting surface is a GUI script runner backed by a tested reusable script engine.
- EXTEST support should be generic and BSDL-driven from the first implementation.
- Capture/view and EXTEST pin-driving are separate operating modes in the MVP.
- Show EXTEST pin control directly in the GUI through a dedicated control panel.
- Show script execution directly in the GUI through a dedicated script runner panel.

## Phases

### Phase 1: Manual Waveform Fit

Add a Fit action to the Waveforms panel and menu flow. The action should reset the visible X range to the buffered sample range without disturbing manual zoom until the user explicitly requests Fit again. Existing Clear, selection-change, and capture-refresh behavior should remain unchanged.

### Phase 2: Harden EXTEST Backend

Keep `JtagChain`, `Scanner`, and `PinDriver` as the core path, but add explicit validation and lifecycle rules around them before exposing them in the GUI:

- target device index must always be explicit
- BSDL must be loaded before drive operations
- BSR state must initialize from BSDL safe values after BSDL load
- non-drivable pins must fail fast
- tri-state requests must validate control-cell availability
- reset-to-safe must be a normal exit path

Then add a GUI `Pin Control` panel that lists drivable pins, shows staged values, and allows LOW/HIGH/HIGH-Z staging plus `Apply` and `Reset Safe` actions while capture is stopped.

### Phase 3: Add Script-Driven CLI

Create a reusable script engine with a simple line-oriented command language. Initial commands should cover sample/read-pin, set-pin, high-z, apply, sleep, expect, and reset-safe. Surface this engine in a GUI `Script Runner` panel with editable script text, run output, and error reporting.

### Phase 4: Tests and Verification

Add non-hardware unit tests around script parsing/interpreter behavior and pure EXTEST safety behavior. Verify the known-good Zybo Z7-20 path first, then explicitly verify that GUI pin control and GUI script execution are disabled while capture is running.

## Verification Targets

1. `bazel build //src:jtag_viewer`
2. `bazel test //test/...`
3. Manual GUI verification for capture, zoom, Fit, deselect, and Clear.
4. Manual GUI verification for `Pin Control` on Zybo Z7-20, including safe EXTEST drive and reset.
5. Manual GUI verification for `Script Runner`, including command success and line-numbered failure reporting.
