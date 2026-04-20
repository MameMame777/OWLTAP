#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "app_config.h"
#include "src/boundary_scan/pin_driver.h"
#include "src/boundary_scan/scanner.h"
#include "src/capture/capture_engine.h"
#include "src/ftdi/ftdi_device.h"
#include "src/jtag/jtag_chain.h"
#include "src/jtag/tap_controller.h"

struct GLFWwindow;

namespace jtag::gui {

/// Main application window: GLFW/ImGui shell + full JTAG backend.
class AppWindow {
public:
    AppWindow();
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    bool isValid() const { return window_ != nullptr; }
    void run();

private:
    // Rendering
    void beginFrame();
    void endFrame();
    void buildDockspace();
    void buildMenuBar();
    void initializeDockLayout(unsigned int dockspace_id);
    void setStatusMessage(const std::string& message);

    // Backend actions
    void onConnect();
    void onDisconnect();
    void onOpenBsdl();
    void onStartCapture();
    void onStopCapture();
    void onSingleCapture();
    void onClearWaveforms();
    void onExportVcd();
    void onExportCsv();
    void onLoadConfig();
    void onSaveConfig();
    void refreshFromCapture();

    // Portable file dialog helpers (Win32 on Windows)
    static std::string openFileDialog(const char* title, const char* filter);
    static std::string saveFileDialog(const char* title, const char* filter);

    // GLFW / ImGui
    GLFWwindow* window_ = nullptr;
    bool show_device_dialog_ = false;
    bool show_trigger_dialog_ = false;
    bool show_about_ = false;
    std::string status_text_ = "Disconnected";
    bool dock_layout_initialized_ = false;

    // Backend objects (constructed in dependency order)
    std::unique_ptr<jtag::FtdiDevice> ftdi_;
    std::unique_ptr<jtag::TapController> tap_;
    std::unique_ptr<jtag::JtagChain> chain_;
    std::unique_ptr<jtag::Scanner> scanner_;
    std::unique_ptr<jtag::PinDriver> pin_driver_;
    std::unique_ptr<jtag::CaptureEngine> capture_engine_;

    bool connected_ = false;
    bool capturing_ = false;
    int bsdl_device_index_ = -1;
    std::chrono::steady_clock::time_point last_refresh_{};

    AppConfig config_;
};

} // namespace jtag::gui
