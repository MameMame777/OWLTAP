#include "gui_theme.h"

#include <GLFW/glfw3.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

namespace jtag::gui::theme {

namespace {

// Build a candidate path from the Windows fonts directory.
std::string fontsDirPath(const char* file) {
    const char* root = std::getenv("SystemRoot");
    if (root == nullptr) root = "C:\\Windows";
    std::string p = root;
    p += "\\Fonts\\";
    p += file;
    return p;
}

}  // namespace

void applyDarkTheme() {
    ImGui::StyleColorsDark();

    ImGuiStyle& s = ImGui::GetStyle();

    // --- Shape ---
    s.WindowRounding    = 8.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 5.0f;
    s.PopupRounding     = 6.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 5.0f;

    // --- Spacing ---
    s.WindowPadding     = ImVec2(12.0f, 10.0f);
    s.FramePadding      = ImVec2(8.0f, 4.0f);
    s.ItemSpacing       = ImVec2(8.0f, 5.0f);
    s.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 8.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;

    ImVec4* c = s.Colors;

    // Backgrounds
    c[ImGuiCol_WindowBg]          = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
    c[ImGuiCol_ChildBg]           = ImVec4(0.09f, 0.09f, 0.12f, 1.00f);
    c[ImGuiCol_PopupBg]           = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    c[ImGuiCol_MenuBarBg]         = ImVec4(0.09f, 0.09f, 0.12f, 1.00f);
    c[ImGuiCol_ScrollbarBg]       = ImVec4(0.09f, 0.09f, 0.12f, 1.00f);

    // Borders & separators
    c[ImGuiCol_Border]            = ImVec4(0.28f, 0.28f, 0.36f, 0.60f);
    c[ImGuiCol_BorderShadow]      = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_Separator]         = ImVec4(0.28f, 0.28f, 0.36f, 0.80f);
    c[ImGuiCol_SeparatorHovered]  = ImVec4(0.40f, 0.70f, 1.00f, 0.60f);
    c[ImGuiCol_SeparatorActive]   = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);

    // Frames
    c[ImGuiCol_FrameBg]           = ImVec4(0.18f, 0.18f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBgHovered]    = ImVec4(0.24f, 0.24f, 0.32f, 1.00f);
    c[ImGuiCol_FrameBgActive]     = ImVec4(0.28f, 0.28f, 0.38f, 1.00f);

    // Title bars
    c[ImGuiCol_TitleBg]           = ImVec4(0.09f, 0.09f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive]     = ImVec4(0.16f, 0.29f, 0.48f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]  = ImVec4(0.09f, 0.09f, 0.12f, 0.80f);

    // Buttons
    c[ImGuiCol_Button]            = ImVec4(0.18f, 0.36f, 0.60f, 1.00f);
    c[ImGuiCol_ButtonHovered]     = ImVec4(0.26f, 0.48f, 0.75f, 1.00f);
    c[ImGuiCol_ButtonActive]      = ImVec4(0.14f, 0.28f, 0.50f, 1.00f);

    // Headers
    c[ImGuiCol_Header]            = ImVec4(0.18f, 0.36f, 0.60f, 0.50f);
    c[ImGuiCol_HeaderHovered]     = ImVec4(0.26f, 0.48f, 0.75f, 0.60f);
    c[ImGuiCol_HeaderActive]      = ImVec4(0.26f, 0.48f, 0.75f, 1.00f);

    // Accent elements
    c[ImGuiCol_CheckMark]         = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]        = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive]  = ImVec4(0.55f, 0.80f, 1.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]     = ImVec4(0.25f, 0.42f, 0.62f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.55f, 0.78f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_ResizeGrip]        = ImVec4(0.40f, 0.70f, 1.00f, 0.30f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.40f, 0.70f, 1.00f, 0.60f);
    c[ImGuiCol_ResizeGripActive]  = ImVec4(0.40f, 0.70f, 1.00f, 0.90f);

    // Tabs
    c[ImGuiCol_Tab]               = ImVec4(0.13f, 0.25f, 0.42f, 1.00f);
    c[ImGuiCol_TabHovered]        = ImVec4(0.40f, 0.70f, 1.00f, 0.80f);
    c[ImGuiCol_TabActive]         = ImVec4(0.20f, 0.42f, 0.68f, 1.00f);
    c[ImGuiCol_TabUnfocused]      = ImVec4(0.09f, 0.09f, 0.12f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]= ImVec4(0.14f, 0.26f, 0.42f, 1.00f);

    // Docking
    c[ImGuiCol_DockingPreview]    = ImVec4(0.40f, 0.70f, 1.00f, 0.70f);
    c[ImGuiCol_DockingEmptyBg]    = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);

    // Text selection / navigation
    c[ImGuiCol_TextSelectedBg]    = ImVec4(0.18f, 0.36f, 0.60f, 0.50f);
    c[ImGuiCol_NavHighlight]      = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);

    // Plot colors
    c[ImGuiCol_PlotLines]         = ImVec4(0.53f, 0.70f, 0.98f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]  = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_PlotHistogram]     = ImVec4(0.53f, 0.70f, 0.98f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
}

float computeDpiScale(GLFWwindow* window) {
    if (window == nullptr) return 1.0f;
    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window, &xs, &ys);
    const float s = (xs + ys) * 0.5f;
    // Clamp to a sensible range; some drivers report 0 if the monitor query fails.
    if (s < 0.5f) return 1.0f;
    if (s > 4.0f) return 4.0f;
    return s;
}

void applyFonts(ImGuiIO& io, float base_size_px, float dpi_scale) {
    if (dpi_scale < 0.5f) dpi_scale = 1.0f;
    const float px = base_size_px * dpi_scale;

    // Candidate font files in priority order.
    static const std::array<const char*, 4> kCandidates = {
        "CascadiaCode.ttf",   // Win11 default — modern monospace
        "CascadiaMono.ttf",   // Win11 fallback
        "segoeui.ttf",        // Windows UI sans
        "consola.ttf",        // Older Windows monospace
    };

    io.Fonts->Clear();
    ImFont* loaded = nullptr;
    for (const char* name : kCandidates) {
        std::string path = fontsDirPath(name);
        loaded = io.Fonts->AddFontFromFileTTF(path.c_str(), px);
        if (loaded != nullptr) break;
    }
    if (loaded == nullptr) {
        // Fall back to ImGui's built-in font; still scale it so DPI looks right.
        ImFontConfig cfg;
        cfg.SizePixels = px;
        io.Fonts->AddFontDefault(&cfg);
    }
}

void scaleStyleForDpi(float dpi_scale) {
    if (dpi_scale < 0.5f) dpi_scale = 1.0f;
    if (std::abs(dpi_scale - 1.0f) < 0.001f) return;
    ImGui::GetStyle().ScaleAllSizes(dpi_scale);
}

}  // namespace jtag::gui::theme
