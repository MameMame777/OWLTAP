# Plan: SPI Config ROM Write Support (PYNQ-Z1/Z2 + MT25QL128) — 2026-04-23

Add JTAG-based write support for the SPI config ROM on Zynq-7000 boards, using the
BSCAN bridge bitstream approach (same mechanism used by OpenOCD / xc3sprog).

Target board: PYNQ-Z1/Z2 (XC7Z020-CLG400) + Micron MT25QL128 (16 MB, JEDEC 0x20BA18).
Input: raw `.bin` only. Operation: Program = Erase + Write + Verify (一括).
UI: GUI `Tools → Program Flash...` + CLI `flash_program.exe`.

---

## Phase 1 — Bridge transport layer

**Goal**: Drive a single SPI transaction (CS low → shift N bits TX+RX → CS high)
through the BSCAN bridge bitstream running on the PL TAP.

Reference implementation: quartiq/bscan_spi_bitstreams (MIT license).
Protocol: `USER1` IR selects the SPI shifter; entering SHIFT-DR asserts CS low;
exiting (Exit1-DR/Update-DR) releases CS. DR data is shifted LSB-first as usual,
but flash commands are MSB-first on SPI, so bit-reversal is required (reuse:
`PlConfig::bitReverseByte`).

**Steps**
1. Create `src/flash/flash_jtag_bridge.{h,cpp}` with class `IFlashBridge`
   (abstract) + `FlashJtagBridge` constructed from `JtagChain&, int device_index`.
2. Implement `transfer(const std::vector<uint8_t>& tx, std::vector<uint8_t>& rx)`:
   - Select `USER1` instruction (IR opcode `0x02`) via `chain.selectInstruction`
   - Bit-reverse `tx`, shift through `chain.tap().shiftDR(tx', rx', tx.size()*8)`
   - Bit-reverse `rx` back to MSB-first

**Scope**: `USER1` only. No quad/dual SPI. BYPASS handling is automatic via JtagChain.

---

## Phase 2 — MT25QL128 command layer

**Goal**: Flash primitives in terms of `IFlashBridge`.

**Steps**
1. Create `src/flash/spi_flash.{h,cpp}` with class `Mt25qFlash` holding
   `IFlashBridge& bridge_`.
2. Implement: `readId`, `readStatus`, `writeEnable`, `waitWipClear`,
   `bulkErase`, `pageProgram`, `read`.
3. Constants: `kJedecMt25ql128 = 0x20BA18`, `kPageSize = 256`,
   `kCapacityBytes = 16*1024*1024`.

**Scope excludes**: sector erase, 4-byte addressing, quad SPI, OTP/security regs.

---

## Phase 3 — Programmer orchestration

**Goal**: Single-call `program(binPath, progressCb)` that does the full sequence.

**Steps**
1. Create `src/flash/flash_programmer.{h,cpp}` with class `FlashProgrammer`.
2. `program()` sequence:
   - (a) `PlConfig(chain, pl_device_index).program(bridge_bit_path, nullptr)`
   - (b) `tap.reset(); tap.clkIdle(10);`
   - (c) Instantiate bridge + flash
   - (d) `flash.readId()` — abort if JEDEC ≠ `kJedecMt25ql128`
   - (e) Load `.bin`; abort if size > `kCapacityBytes`
   - (f) `flash.bulkErase()` with progress callback
   - (g) Page-loop: `flash.pageProgram()` with progress callback
   - (h) Verify: read back full range, `memcmp`

---

## Phase 4 — CLI tool

Create `src/tools/flash_program.cpp` mirroring `pl_program.cpp`:
Args `--bin <path>`, `--bridge <path>`, `--dev <N>`, `--freq <hz>`, `-v`, `-h`.

---

## Phase 5 — GUI integration

Duplicate the existing PL-programming UI exactly:
1. Add `program_flash_*` member block in `app_window.h`
2. Add menu entry `Tools → Program Flash...`
3. Add `onProgramFlash()` and `drawProgramFlashModal()`
4. Join flash thread in destructor

---

## Phase 6 — Tests

1. `test/spi_flash_test.cpp` — command encoding tests using a recording
   fake `IFlashBridge`
2. `test/flash_programmer_test.cpp` — JEDEC-mismatch aborts, oversize `.bin`
   aborts, verify-failure surfaces offset

---

## Phase 7 — Docs and assets

1. `assets/README.md` describing how to obtain `bscan_spi_xc7z020.bit`
   (not vendored, MIT from quartiq)
2. Extend `README.md` with "Flash programming" subsection
3. Update `THIRD_PARTY_NOTICES.md`

---

## Verification

1. `bazelisk build //src:jtag_viewer //src/tools:flash_program`
2. `bazelisk test //test/...`
3. Hardware on PYNQ-Z1: `flash_program --bin blink.bin --bridge assets/bscan_spi_xc7z020.bit`
4. Negative: JEDEC mismatch → clean error without TAP hang

---

## Decisions

- **Approach**: BSCAN bridge bitstream (industry standard)
- **Target**: PYNQ-Z1/Z2 + MT25QL128 only in v1; other ICs → explicit error
- **Input**: `.bin` only (no `.mcs`)
- **Erase**: Bulk erase only (sector erase deferred)
- **Bridge bit**: NOT vendored
- **SPI**: 1-bit standard only
- **Verification**: always performed; no opt-out in v1

## Scope exclusions

- Flash read-back to file
- Standalone erase command
- Flash ID/status UI
- Non-Zynq-7000 targets
- Hardware integration test in CI
