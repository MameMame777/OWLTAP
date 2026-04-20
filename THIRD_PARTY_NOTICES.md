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
  - Notes: Docking branch sources are vendored in this repository.

- ImPlot
  - Path: `third_party/implot`
  - License: MIT

- GLFW
  - Path: `third_party/glfw`
  - License: zlib/libpng license

- libusb 1.0.27
  - Path: `third_party/libusb`
  - License: GNU LGPL v2.1 or later
  - Notes: Linux builds link the system library. Windows builds compile the
    vendored source in this repository.
  - Included text: `THIRD_PARTY_LICENSES/LGPL-2.1.txt`
  - Companion text: `THIRD_PARTY_LICENSES/GPL-2.0.txt` (referenced by LGPL 2.1)

- libftdi1
  - Path: `third_party/libftdi`
  - License: LGPL-2.1-only
  - Notes: Linux builds link the system library. Windows builds compile the
    vendored source in this repository.
  - Included text: `THIRD_PARTY_LICENSES/LGPL-2.1.txt`
  - Companion text: `THIRD_PARTY_LICENSES/GPL-2.0.txt` (referenced by LGPL 2.1)

- stb components bundled with Dear ImGui
  - Paths:
    - `third_party/imgui/imgui-src/imstb_rectpack.h`
    - `third_party/imgui/imgui-src/imstb_textedit.h`
    - `third_party/imgui/imgui-src/imstb_truetype.h`
  - License: MIT or public domain / Unlicense, as stated in each upstream file.

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