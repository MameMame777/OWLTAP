#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
    void drawPinControlPanel();
    void drawScriptRunnerPanel();
    void setStatusMessage(const std::string& message);
    bool refreshPinReadback(bool report_status);

    // Backend actions
    void onConnect();
    void onDisconnect();
    void onOpenBsdl();
    void onStartCapture();
    void onStopCapture();
    void onSingleCapture();
    void onClearWaveforms();
    void onFitWaveforms();
    void onExportVcd();
    void onExportCsv();
    void onLoadScript();
    void onSaveScript(bool save_as);
    void onLoadConfig();
    void onSaveConfig();
    void onLoadXdc(const std::string& path);
    void refreshFromCapture();
    bool loadScriptFile(const std::string& path, std::string& error);
    bool saveScriptFile(const std::string& path, std::string& error) const;

    // PL programming (background thread)
    void onProgramPl();
    void drawProgramPlModal();

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
    std::array<char, 8192> script_buffer_{};
    std::string script_path_;
    std::string script_output_;
    jtag::ScanResult pin_readback_;
    std::string pin_readback_error_;
    bool extest_outputs_active_ = false;

    // PL programming state
    bool program_pl_popup_requested_ = false;
    std::atomic<bool> program_pl_running_{false};
    std::atomic<size_t> program_pl_bytes_{0};
    std::atomic<size_t> program_pl_total_{0};
    std::atomic<bool> program_pl_success_{false};
    std::string program_pl_path_;
    std::string program_pl_error_;   // written by thread; read only after running_ == false
    std::mutex program_pl_mutex_;
    std::thread program_pl_thread_;

    AppConfig config_;
};

} // namespace jtag::gui
