# Plan: Public Release Preparation — 2026-04-21

## Scope

Three independent tasks required before public release:
1. README.md with icon
2. Security hardening (BSDL parser input validation)
3. Taskbar / window icon via GLFW + Windows resource file

---

## Task 1: README.md

File: `README.md` (workspace root)

Sections: badge/icon, description, features, architecture summary,
hardware requirements, build, usage, license placeholder.
Icon embedded as `docs/icon.png`.

---

## Task 2: Security — BSDL parser

Affected file: `src/bsdl/bsdl_parser.cpp`

### Issues

| Location | Issue | Fix |
|----------|-------|-----|
| `parseBoundaryLength()` line 282 | `stoi` result fed directly to `resize` — negative or >65536 not guarded | Cap to `[1, kMaxBsrBits]` |
| `parseBoundaryRegister()` line ~387 | `cell_pos + 1` passed to `resize` without upper-bound check — integer overflow / OOM | Cap to `kMaxBsrBits` before resize |

Constant: `static constexpr int kMaxBsrBits = 65536;`

---

## Task 3: Window / taskbar icon

### 3a: Generate assets

Run Python (Pillow) to produce:
- `src/icon_rgba_48.h` — 48×48 RGBA C array for `glfwSetWindowIcon()`
- `assets/jtag.ico`   — multi-size ICO (16, 32, 48, 256) for Windows exe resource

### 3b: Windows resource file

`src/app.rc`:
```
GLFW_ICON ICON "jtag.ico"
```
GLFW Win32 backend automatically loads a named "GLFW_ICON" icon resource
when `glfwSetWindowIcon(window, 0, NULL)` is called.

### 3c: BUILD.bazel

`src/BUILD.bazel`:
- Add `"app.rc"` and `"jtag.ico"` to `cc_binary` `srcs`
- Add `"icon_rgba_48.h"` to `gui` library `hdrs`

### 3d: app_window.cpp

After `glfwCreateWindow()` succeeds, insert:
```cpp
#include "src/icon_rgba_48.h"
// ...
GLFWimage icon_image;
icon_image.width  = kIconWidth;
icon_image.height = kIconHeight;
icon_image.pixels = const_cast<unsigned char*>(kIconRgba);
glfwSetWindowIcon(window_, 1, &icon_image);
```

---

## Verification

- `bazelisk build //src:jtag_viewer` — clean build
- `bazelisk test //test/...` — all tests pass
- Run executable: taskbar shows owl icon
