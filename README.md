<p align="center">
  <img src="docs/icon.png" alt="OwlTAP" width="160"/>
</p>

<h1 align="center">OwlTAP</h1>

<p align="center">
  A desktop JTAG boundary-scan diagnostic and waveform capture tool for FPGA/SoC devices,
  built with FTDI MPSSE, Dear ImGui, and ImPlot.
</p>

---

## Features

| Feature | Detail |
|---------|--------|
| **JTAG chain detection** | Auto-enumerates devices; reads IDCODE and IR length |
| **BSDL parsing** | Loads vendor BSDL files; maps logical pin names to BSR bit positions |
| **Boundary scan (EXTEST)** | Drive output pins HIGH/LOW/Hi-Z; read captured input values |
| **Safety guard** | Blocks EXTEST when Zynq PS\_DDR / PS\_MIO / PS\_POR\_B / PS\_SRST\_B pins detected |
| **Waveform capture** | Continuous or triggered multi-channel capture with configurable depth |
| **Trigger engine** | Rising edge, falling edge, either edge, level; pre/post-trigger ratio |
| **ImPlot waveform view** | Zoomable, pannable signal waveform display |
| **VCD export** | Standard Value Change Dump export for GTKWave / Vivado logic analyser |
| **Script engine** | Simple text script for automated set/expect sequences |
| **Config persistence** | Last device settings saved to `cfg.json` |

## Hardware Requirements

| Component | Requirement |
|-----------|-------------|
| FTDI adapter | FT2232H / FT4232H (MPSSE-capable); tested with FT4232H VID=0x0403 PID=0x6011 |
| Target device | Any IEEE 1149.1-compliant FPGA or SoC with a BSDL file |
| OS | Windows 10/11 (64-bit) |
| OpenGL | OpenGL 3.3 core profile |

Verified hardware: Xilinx Zynq XA7Z020-CLG484 PL TAP + ARM DAP via FTDI FT4232H.

## Architecture

```
┌─────────────────────────────────────┐
│          Qt6 GUI (ImGui/ImPlot)     │  ← app_window, signal_panel, waveform_view
├─────────────────────────────────────┤
│  Boundary Scan: Scanner / PinDriver │  ← BSR staging, EXTEST, capture decoding
│  Capture Engine + Trigger           │  ← ring buffer, edge/level trigger
├─────────────────────────────────────┤
│  JTAG Chain + BSDL Parser           │  ← device enumeration, pin mapping
├─────────────────────────────────────┤
│  TAP Controller  (IEEE 1149.1 FSM)  │  ← TMS path generation
├─────────────────────────────────────┤
│  MPSSE Command Buffer               │  ← FTDI protocol encoding
├─────────────────────────────────────┤
│  FtdiDevice  (libftdi1)             │  ← USB bulk transfer
└─────────────────────────────────────┘
```

## Build

### Prerequisites

| Tool | Notes |
|------|-------|
| [Bazelisk](https://github.com/bazelbuild/bazelisk/releases) | Download `bazelisk-windows-amd64.exe`, rename to `bazelisk.exe`, place on `PATH` |
| Visual Studio 2022 | **Desktop development with C++** workload required (MSVC v143, Windows SDK) |
| Python 3.x | Required by Bazel host scripts; `python` must be on `PATH` |
| Pillow (optional) | `pip install pillow` — only needed for the icon-embedding post-build step |

libftdi1 and libusb-1.0 are vendored under `third_party/`; no separate installation is needed for building.

### Windows USB driver

libusb requires a WinUSB-compatible driver bound to the FTDI adapter.
Install [Zadig](https://zadig.akeo.ie/), select the FTDI interface used for JTAG (usually Interface 0 of the FT4232H), and install **WinUSB** or **libusbK**.
The FTDI COM-port driver (VCP) must **not** be bound to that interface at the same time.

### Build commands

```powershell
# Clone and enter the repository
git clone https://github.com/MameMame777/OwlTAP.git
cd OwlTAP

# Build the viewer
bazelisk build //src:jtag_viewer

# Embed the taskbar icon (requires Pillow; run once after each clean build)
Set-ItemProperty bazel-bin/src/jtag_viewer.exe -Name IsReadOnly -Value $false
python tools/embed_icon.py bazel-bin/src/jtag_viewer.exe docs/icon.png

# Run all unit tests (no hardware required)
bazelisk test //test/...

# Build with debug symbols
bazelisk build --config=debug //src:jtag_viewer

# Build the diagnostic CLI tool
bazelisk build //src/tools:jtag_diag
```

The binary is produced at `bazel-bin/src/jtag_viewer.exe`.

### BSDL files

BSDL/BSD files for your target device are **not included** in this repository.
Obtain them from your device vendor (e.g. Xilinx/AMD downloads, device datasheet package)
and load them at runtime via **Device → Load BSDL**.

## Usage

1. Connect the FTDI adapter to the target board's JTAG header.
2. Run `jtag_viewer.exe`.
3. **Device** → **Connect**: select VID/PID/serial and channel; click Connect.
4. **Device** → **Load BSDL**: choose the `.bsd` / `.bsdl` file for your target.
5. **Signal Panel**: select pins to monitor or drive.
6. **Capture** → **Start** to begin waveform acquisition.
7. **File** → **Export VCD** to save captured waveforms.

### Sampling rate and signal bandwidth

Boundary-scan capture works by repeatedly shifting the 1077-bit BSR of the
XC7Z020 through the JTAG TAP.  The attainable sample rate is limited by two
factors:

| Factor | Contribution |
|--------|-------------|
| JTAG clock (default 6 MHz) + IR/DR overhead | ~5.5 kHz theoretical max |
| USB bulk-transfer round-trip latency (~250–500 µs) | reduces to **~1–2 kHz** in practice |

> **Rule of thumb**: signals above ~500 Hz–1 kHz will alias and cannot be
> captured faithfully.  Boundary scan is suited for slow control signals,
> power-up sequencing, and bus idle/active states — not high-speed clocks or
> fast GPIO toggles.

For high-speed signal capture, use the **Xilinx Integrated Logic Analyser
(ILA)** core instantiated inside your PL design.

### Script Engine

Create a plain-text script file and load it via **File** → **Run Script**:

```
# Drive LED HIGH and verify
set LED0 1
apply
expect LED0 1

# Release to Hi-Z
highz LED0
apply
```

### Programming the PL bitstream (volatile)

**Tools → Program Bitstream...** writes a `.bit` / `.bin` file directly into
the Zynq PL via JTAG (UG470 configuration sequence: `JPROGRAM` →
`CFG_IN` → `JSTART` → `DONE`).  Lost on power cycle.

Command-line equivalent:

```
bazel-bin/src/tools/pl_program.exe --bit design.bit
```

### Programming the SPI configuration ROM (non-volatile)

**Tools → Program Flash (SPI ROM)...** writes a flash image into the Micron
MT25QL128 so the board boots the design after power-cycle.  The workflow:

1. Generate a raw SPI image from Vivado:

   ```tcl
   write_cfgmem -force -format BIN -interface SPIx1 -size 16 \
                -loadbit "up 0x00000000 design.bit" design.bin
   ```

2. Download the BSCAN SPI bridge bitstream from
   [quartiq/bscan_spi_bitstreams](https://github.com/quartiq/bscan_spi_bitstreams)
   — for XC7Z020 use `bscan_spi_xc7z020.bit`.  See [assets/README.md](assets/README.md).

3. In the GUI, select **Tools → Program Flash (SPI ROM)...**, pick the bridge
   `.bit` first, then your design `.bin`.  The tool loads the bridge into the
   PL, bulk-erases the flash, programs it page-by-page, and verifies the
   read-back.

Command-line equivalent:

```
bazel-bin/src/tools/flash_program.exe --bridge bscan_spi_xc7z020.bit --bin design.bin
```

Only MT25QL128 (JEDEC `0x20BA18`, 16 MB) is currently supported.

## Project Structure

```
src/
  boundary_scan/   -- BSR staging free functions + PinDriver
  bsdl/            -- BSDL lexer, parser, model
  capture/         -- CaptureEngine ring buffer + trigger logic
  config/          -- PL JTAG configuration (UG470 sequencer)
  flash/           -- SPI config ROM programming via BSCAN bridge
  ftdi/            -- libftdi1 wrapper (FtdiDevice) + MPSSE buffer
  gui/             -- ImGui application window, panels, dialogs
  jtag/            -- JtagChain + TAP controller FSM
  script/          -- ScriptEngine (set/expect/apply/highz)
  tools/           -- jtag_diag, pl_program, flash_program CLI tools
test/              -- Google Test unit tests (no hardware required)
third_party/       -- vendored libftdi1, libusb-1.0, GLFW, ImGui, ImPlot
docs/              -- architecture references, implementation plans
```

## Testing

Unit tests cover all hardware-independent logic:

```
//test:bsdl_parser_test    -- BSDL lexer + parser
//test:mpsse_test          -- MPSSE command encoding
//test:tap_controller_test -- TAP state machine
//test:trigger_test        -- Trigger engine
//test:scanner_test        -- Boundary-scan decoder
//test:pin_driver_test     -- BSR staging free functions
//test:script_engine_test  -- Script parser + executor
//test:spi_flash_test      -- MT25Q SPI flash command encoding
```

Hardware-dependent paths (`FtdiDevice`, `JtagChain::readBSR/writeBSR`) require a
physical FTDI adapter and are exercised manually or via `jtag_diag`.

## License

Original source code: **MIT License** — see [LICENSE](LICENSE).

This project vendors third-party libraries under separate licenses.
See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
License texts for LGPL-covered components are in [THIRD_PARTY_LICENSES/](THIRD_PARTY_LICENSES/).
