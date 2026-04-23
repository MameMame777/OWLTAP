# Third-Party Notices

The original code in this repository is licensed under the MIT License in
the root LICENSE file unless stated otherwise.

This repository also includes or links against third-party software under
separate licenses. Those components remain under their respective upstream
licenses.

## Included or linked components

- Dear ImGui
  - Path: `third_party/imgui`
  - License: MIT
  - License text: `third_party/imgui/LICENSE.txt`
  - Notes: Docking branch sources are vendored in this repository.

- ImPlot
  - Path: `third_party/implot`
  - License: MIT
  - License text: `third_party/implot/LICENSE`

- GLFW
  - Path: `third_party/glfw`
  - License: zlib/libpng license
  - License text: `third_party/glfw/LICENSE.md`

- libusb 1.0.27
  - Path: `third_party/libusb`
  - License: GNU LGPL v2.1 or later
  - Notes: Linux builds link the system library. Windows builds compile the
    vendored source in this repository.
  - Upstream COPYING: `third_party/libusb/libusb-src/libusb-1.0.27/COPYING`
  - Included text: `THIRD_PARTY_LICENSES/LGPL-2.1.txt`
  - Companion text: `THIRD_PARTY_LICENSES/GPL-2.0.txt` (referenced by LGPL 2.1)

- libftdi1
  - Path: `third_party/libftdi`
  - License: LGPL-2.1-only
  - Notes: Linux builds link the system library. Windows builds compile the
    vendored source in this repository.
  - Attribution notice: `third_party/libftdi/LICENSE`
  - Included text: `THIRD_PARTY_LICENSES/LGPL-2.1.txt`
  - Companion text: `THIRD_PARTY_LICENSES/GPL-2.0.txt` (referenced by LGPL 2.1)

- GoogleTest
  - Used by: unit tests only (`test/...`), not linked into the shipped
    `jtag_viewer` binary.
  - Source: fetched via Bazel module (`googletest` in `MODULE.bazel`)
  - License: BSD-3-Clause

- Digilent Zybo Z7 Master XDC
  - File: `z7020_maseter.xdc`
  - Source: <https://github.com/Digilent/digilent-xdc>
  - License: MIT (Digilent, Inc.)

- stb components bundled with Dear ImGui
  - Paths:
    - `third_party/imgui/imgui-src/imstb_rectpack.h`
    - `third_party/imgui/imgui-src/imstb_textedit.h`
    - `third_party/imgui/imgui-src/imstb_truetype.h`
  - License: MIT or public domain / Unlicense, as stated in each upstream file.

## User-generated Xilinx artifacts (not included)

This repository does not ship any Vivado-generated bitstream (`.bit`),
flash image (`.mcs`), or flash report (`.prm`) files. Those are
user-generated outputs of the Xilinx / AMD Vivado Design Suite and are
excluded from the source tree (see `.gitignore`). Users producing their
own equivalent outputs should refer to the end-user license terms of the
Xilinx / AMD tool they use.

## Optional runtime assets (not vendored)

The SPI flash programming feature loads a small helper bitstream into the PL
at runtime.  We do not ship this bitstream; users download it themselves:

- quartiq/bscan_spi_bitstreams
  - Source: <https://github.com/quartiq/bscan_spi_bitstreams>
  - License: MIT
  - See [assets/README.md](assets/README.md) for usage.

## Redistribution note

If you redistribute binaries, review the obligations for LGPL-covered
components, especially for Windows builds that compile `libusb` and `libftdi`
from vendored source.

At minimum, keep the relevant copyright notices and license texts for those
dependencies with the distribution and verify any source or relinking
requirements that apply to your release process.

For source-only publication, this repository includes the LGPL 2.1 license
text used by `libftdi` and as a valid license option for `libusb`, plus the
companion GNU GPL v2 text referenced by LGPL 2.1, at
`THIRD_PARTY_LICENSES/LGPL-2.1.txt` and `THIRD_PARTY_LICENSES/GPL-2.0.txt`.