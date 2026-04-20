# Plan: Qt6 to Dear ImGui Migration

**Date**: 2026-04-19
**Reason**: Qt6 LGPL license imposes dynamic-linking and distribution constraints.
Dear ImGui (MIT) eliminates all license restrictions.

## Scope

Replace all Qt6 GUI code (~1,400 lines, 7 files) with Dear ImGui + ImPlot.
No changes to non-GUI layers (FTDI, MPSSE, TAP, JTAG Chain, BSDL, Scanner, Trigger, CaptureEngine).

## Dependencies to Add

| Library | Version | License | Purpose |
|---------|---------|---------|---------|
| Dear ImGui | v1.91.8 (docking branch) | MIT | Core GUI framework |
| ImPlot | v0.16 | MIT | Waveform (digital signal) plotting |
| GLFW | 3.4 | Zlib | Window creation, input, OpenGL context |

All built from source via Bazel `cc_library`. No system install required.

## File Plan

### New third_party libraries

```
third_party/imgui/
  BUILD.bazel          -- cc_library for imgui core + GLFW/OpenGL3 backend
  imgui-src/           -- extracted source (imgui/*.cpp, *.h, backends/*)
third_party/implot/
  BUILD.bazel          -- cc_library for implot
  implot-src/          -- extracted source
third_party/glfw/
  BUILD.bazel          -- cc_library building GLFW from source (Windows)
  glfw-src/            -- extracted source
```

### GUI files to rewrite (src/gui/)

| Old file (Qt6) | New file (ImGui) | Notes |
|----------------|------------------|-------|
| main_window.h/cpp | app_window.h/cpp | GLFW window + ImGui frame loop, dockspace layout |
| waveform_widget.h/cpp | waveform_view.h/cpp | ImPlot digital signals + custom rendering |
| signal_tree.h/cpp | signal_panel.h/cpp | ImGui::TreeNodeEx + checkboxes |
| hex_display.h/cpp | hex_panel.h/cpp | ImGui::BeginTable |
| device_dialog.h/cpp | device_dialog.h/cpp | ImGui::BeginPopupModal |
| trigger_dialog.h/cpp | trigger_dialog.h/cpp | ImGui::BeginPopupModal |
| -- | vcd_export.h/cpp | Extract VCD/CSV export from main_window (pure logic, no GUI) |

### Files to modify

| File | Change |
|------|--------|
| src/main.cpp | Replace QApplication with GLFW+ImGui init loop |
| src/gui/BUILD.bazel | Replace qt6_widgets dep with imgui/implot/glfw |
| src/BUILD.bazel | No change (still depends on //src/gui:main_window) |
| third_party/qt6/ | DELETE entire directory |

## Implementation Steps

1. **Download & extract** ImGui (docking branch), ImPlot, GLFW sources into third_party/
2. **Create BUILD.bazel** for each third_party library
3. **Build-verify** third_party libs: `bazel build //third_party/imgui //third_party/implot //third_party/glfw`
4. **Write app_window** (GLFW window + ImGui context + dockspace frame loop)
5. **Write waveform_view** (ImPlot::PlotDigitalG or custom ImDrawList)
6. **Write signal_panel** (TreeNodeEx + checkbox per pin)
7. **Write hex_panel** (ImGui::BeginTable, format selector)
8. **Write device_dialog** (popup modal, combo for device list)
9. **Write trigger_dialog** (popup modal, condition editor)
10. **Extract vcd_export** (pure C++ VCD/CSV writer, no GUI dep)
11. **Rewrite main.cpp** (GLFW init, main loop, cleanup)
12. **Update BUILD.bazel** files, delete third_party/qt6
13. **Build & test** full //src:jtag_viewer
14. **Run all existing tests** (non-GUI tests must still pass)

## Design Decisions

- **Docking branch**: Use ImGui docking branch for QDockWidget-equivalent multi-panel layout.
- **File dialogs**: Use Windows native `GetOpenFileName`/`GetSaveFileName` via `<commdlg.h>` (no extra deps).
- **Thread safety**: CaptureEngine callback runs on capture thread; ImGui renders on main thread.
  Copy data under mutex, same pattern as Qt version.
- **Export logic**: Extract VCD/CSV export into non-GUI `vcd_export.h/cpp` to keep GUI layer thin.
- **No ImGuiFileDialog dep**: Avoid extra library; native OS dialogs are simpler.

## Risks

| Risk | Mitigation |
|------|------------|
| GLFW OpenGL context on all target machines | GLFW 3.4 supports OpenGL 3.3 which is baseline on Win10+ |
| ImPlot digital signal rendering perf | Downsample for display; only send visible range |
| ImGui docking branch stability | Docking is de-facto standard, used in production by many tools |

## Acceptance Criteria

- `bazel build --config=windows //src:jtag_viewer` succeeds
- `bazel test --config=windows //test/...` all pass (39 tests)
- Application launches, shows dockable panels
- No Qt6 dependency remains anywhere in the tree
