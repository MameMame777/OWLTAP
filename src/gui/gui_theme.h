#pragma once

#include "imgui.h"

struct GLFWwindow;

namespace jtag::gui::theme {

// Semantic accent colors used across panels for consistent status indication.
inline constexpr ImVec4 kAccent  = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);  // cyan-blue
inline constexpr ImVec4 kSuccess = ImVec4(0.40f, 0.85f, 0.55f, 1.00f);  // green
inline constexpr ImVec4 kWarning = ImVec4(1.00f, 0.78f, 0.30f, 1.00f);  // amber
inline constexpr ImVec4 kError   = ImVec4(1.00f, 0.45f, 0.45f, 1.00f);  // red
inline constexpr ImVec4 kMuted   = ImVec4(0.65f, 0.65f, 0.72f, 1.00f);  // gray

// Apply the OwlTAP dark theme (colors + spacing + rounding).
// Re-entrant: safe to call again after a DPI change.
void applyDarkTheme();

// Compute the desired DPI scale for the given window. Returns 1.0f if no
// window is provided. Multi-monitor: uses the monitor the window is on.
float computeDpiScale(GLFWwindow* window);

// Load the application font(s) into io.Fonts. Tries Cascadia Code, then
// Consolas, then falls back to ImGui's built-in font. base_size_px is the
// font size in DIP; the actual loaded size is base_size_px * dpi_scale.
// Must be called before ImGui_ImplOpenGL3_Init / ImGui_ImplOpenGL3_CreateFontsTexture.
void applyFonts(ImGuiIO& io, float base_size_px, float dpi_scale);

// Scale the current ImGuiStyle by `dpi_scale`. Apply once after applyDarkTheme()
// to avoid compounding scale on theme reapply.
void scaleStyleForDpi(float dpi_scale);

}  // namespace jtag::gui::theme
