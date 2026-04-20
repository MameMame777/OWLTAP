# Self-Review Report — FTDIJTAG Project

Date: 2025-07-25
Scope: All source files, tests, and build configuration

## Critical (Compile Errors / Hard Bugs)

### CR-1: BSDL instruction opcodes stored MSB-first, shifted out reversed
**File:** `src/bsdl/bsdl_parser.cpp` (parseInstructionOpcode)
**Impact:** Every instruction selection sends reversed opcode — JTAG device will not respond correctly.
**Root cause:** `opcode <<= 1` loop stores MSB in high bit, but shift-out assumes LSB in bit 0.
**Fix:** Reverse the bit order when storing, or reverse when loading into IR data in `jtag_chain.cpp`.

### CR-2: `readConcatenatedString()` + callers double-consume semicolons
**File:** `src/bsdl/bsdl_parser.cpp`
**Impact:** Skips an entire subsequent BSDL statement (instruction, BSR, or IDCODE could be missed).
**Fix:** Either don't consume the terminator in `readConcatenatedString`, or remove `skipToSemicolon` in callers.

### CR-3: `sampleOpcode()` searches for "SAMPLE" but real BSDL files use "SAMPLE/PRELOAD"
**File:** `src/bsdl/bsdl_model.h`
**Impact:** Primary boundary scan workflow (SAMPLE instruction) will fail for real BSDL files.
**Fix:** Search for both "SAMPLE" and "SAMPLE/PRELOAD".

### CR-4: `signal_tree.cpp` — `QString::fromStdString(pin.direction)` won't compile
**File:** `src/gui/signal_tree.cpp` ~L56
**Impact:** Compile error. `PinDirection` is an enum, not `std::string`.
**Fix:** Add a toString/conversion function for `PinDirection`.

### CR-5: `bsdl_parser_test.cpp` — type mismatches prevent compilation
**File:** `test/bsdl_parser_test.cpp`
- `getInstruction()` returns `int`, test calls `.has_value()` / `.value()` (expects `std::optional`)
- `idcode` is plain `IdCode` not `std::optional`; field is `raw` not `value`
**Fix:** Match test code to actual API: use `!= -1` check, `idcode.raw`.

### CR-6: `scanner.cpp readIdCode()` reads `boundary_length` bits instead of 32
**File:** `src/boundary_scan/scanner.cpp`
**Impact:** IDCODE read returns garbage — wrong DR length for IDCODE instruction.
**Fix:** Add a separate `readDR(32)` path or directly call `tap_controller.readDR(32)`.

### CR-7: Double `raw_idx++` in TapController::doShift() TDO reassembly
**File:** `src/jtag/tap_controller.cpp` ~L248-L262
**Impact:** Corrupts last TDO bit for any shift where `bulk_remaining > 0`.
**Fix:** Remove the extra `raw_idx++`.

## High Severity (Data Races / Logic Errors)

### HI-1: TriggerEngine members mutated from GUI thread, read from capture thread
**Files:** `src/capture/trigger.h`, `src/gui/trigger_dialog.cpp`
**Impact:** Data race on `conditions_`, `mode_`, `pre_trigger_ratio_`.
**Fix:** Either require capture to be stopped before configuring, or protect with mutex.

### HI-2: `sample_interval_us_` written from GUI, read from capture thread
**File:** `src/capture/capture_engine.cpp`
**Fix:** Make it `std::atomic<uint32_t>`.

### HI-3: `callback_` set from GUI, invoked from capture thread
**File:** `src/capture/capture_engine.cpp`
**Fix:** Set only when stopped, or use `std::atomic` + function pointer.

### HI-4: `sample()` decodes output cells alongside input cells
**File:** `src/boundary_scan/scanner.cpp`
**Impact:** Output cell stale value can overwrite current input pin reading.
**Fix:** In SAMPLE mode, only decode INPUT cells (and BIDIR input cells).

### HI-5: `ftdi_device.cpp transfer()` ignores partial writes
**File:** `src/ftdi/ftdi_device.cpp` ~L272
**Impact:** Partial USB write silently desynchronizes MPSSE protocol.
**Fix:** Loop until all bytes written, or error on short write.

### HI-6: Clock divisor calculation overflows uint16_t for low frequencies
**File:** `src/ftdi/ftdi_device.cpp` ~L247
**Impact:** Frequencies below ~458 Hz produce garbage divisor.
**Fix:** Clamp to `UINT16_MAX` or reject out-of-range.

### HI-7: MPSSE shiftOut/In length wraps for >65536 bytes
**File:** `src/ftdi/mpsse.cpp`
**Impact:** Large FPGA chains (>524k bits) silently truncate transfer.
**Fix:** Add chunking loop or guard.

### HI-8: `onConnect()` error path leaves partial init state
**File:** `src/gui/main_window.cpp` ~L218
**Fix:** Clean up all created objects on failure.

## Medium Severity (Design / Build Issues)

### MD-1: `.bazelrc` — `-std=c++17` invalid on MSVC
**Impact:** Windows builds fail without `--config=windows`.
**Fix:** Use platform-specific config or auto-detection.

### MD-2: All `third_party/` Windows `linkopts` are empty
**Impact:** Linking will fail on Windows for libftdi, libusb, Qt6.
**Fix:** Add Windows library paths (vcpkg or explicit .lib paths).

### MD-3: No MOC processing for Qt6
**File:** `src/gui/BUILD.bazel`
**Impact:** Q_OBJECT classes won't link (missing MOC-generated vtable entries).
**Fix:** Create `bazel/qt_rules.bzl` with MOC rules.

### MD-4: `PinDriver` constructor calls `initBsrFromSafe()` before BSDL may be loaded
**File:** `src/boundary_scan/pin_driver.cpp`
**Fix:** Assert BSDL loaded, or defer initialization.

### MD-5: `getInstruction()` return `-1` collides with all-1s BYPASS opcode
**File:** `src/bsdl/bsdl_model.h`
**Fix:** Return `std::optional<uint32_t>`.

### MD-6: `(1u << ir_len) - 1` is UB when `ir_len >= 32`
**File:** `src/jtag/jtag_chain.cpp`
**Fix:** Use `(1ULL << ir_len) - 1` and truncate.

### MD-7: `trigger` depends transitively on full hardware stack
**File:** `src/capture/BUILD.bazel`
**Fix:** Extract `ScanResult`/`PinState` types into lightweight target.

### MD-8: Unterminated string literal returns as valid token in lexer
**File:** `src/bsdl/bsdl_lexer.cpp`
**Fix:** Return `TokenType::ERROR`.

### MD-9: VCD export symbol overflow for >94 signals
**File:** `src/gui/main_window.cpp`
**Fix:** Use multi-char VCD identifiers.

## Low Severity (Cosmetic / Unused Code)

- LS-1: Unused includes: `<cstring>` in mpsse.h, `<functional>` in ftdi_device.h
- LS-2: Dead `bit_offset` variable in `doShift()`
- LS-3: `TapState::NUM_STATES` sentinel inside enum class
- LS-4: `TokenType::BIT_STRING` declared but never produced
- LS-5: `waveform_widget.cpp Key_Right` has no upper bound on scroll_offset_
- LS-6: `hex_display.cpp` `.toUpper()` on whole string produces "0X" not "0x"
- LS-7: `device_dialog.cpp` silently accepts invalid hex VID/PID as 0

## Test Coverage Gaps

| Module | Coverage | Notes |
|--------|----------|-------|
| MPSSE encoder | Good | Missing non-byte-aligned, 0-bit, and TMS read tests |
| TAP free functions | Good | Paths verified |
| TapController class | **None** | Needs FtdiDevice mock/interface |
| JtagChain | **None** | Needs mock |
| BSDLParser | Good | Test has compile errors (CR-5) |
| BSDLLexer | **None** | No dedicated tests |
| BSDLModel | **None** | No dedicated tests |
| Scanner | **None** | Needs mock |
| PinDriver | **None** | Needs mock |
| CaptureEngine | **None** | Needs mock |
| TriggerEngine | Good | Missing NORMAL mode test |

## Recommended Fix Priority

1. **CR-1** (reversed opcodes) — makes all JTAG instructions wrong
2. **CR-2** (double-consume) — corrupts BSDL parse results
3. **CR-3** (SAMPLE/PRELOAD) — breaks primary workflow
4. **CR-6** (readIdCode DR length) — breaks device identification
5. **CR-7** (double raw_idx++) — corrupts TDO data
6. **HI-1/HI-2/HI-3** (data races) — undefined behavior
7. **HI-5** (partial write) — silent protocol corruption
8. **CR-4, CR-5** (compile errors) — fix for buildability
9. **HI-4, HI-6, HI-7, HI-8** — correctness
10. **MD-1 through MD-9** — build and design
