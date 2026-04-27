#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "app_config.h"
#include "daemon_process_controller.h"
#include "gui_daemon_client.h"
#include "ila_panel.h"
#include "src/boundary_scan/pin_driver.h"
#include "src/boundary_scan/scanner.h"
#include "src/capture/capture_engine.h"
#include "src/ftdi/ftdi_device.h"
#include "src/jtag/jtag_chain.h"
#include "src/jtag/tap_controller.h"
#include "src/script/test_suite.h"

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
    void buildStatusBar();
    void initializeDockLayout(unsigned int dockspace_id);
    void drawPinControlPanel();
    void drawScriptRunnerPanel();
    void drawInterconnectPanel();
    void drawHelpWindow();
    void setStatusMessage(const std::string& message);
    /// Update status bar only, without appending to the debug log.
    /// Use for high-frequency progress updates (e.g. "Capturing... N samples").
    void setStatusOnly(const std::string& message);
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
    void onLoadSuite();
    void onRunSuite();
    void onExportSuiteReport();

    // MCP server actions — removed; use jtag_viewer --mcp instead.

    // Daemon process controller actions
    void onDaemonStart();
    void onDaemonStop();
    void onDaemonConnect();        // Phase 3: connect GUI client to daemon GUI RPC port
    void onDaemonDetectDevices();  // Trigger hardware/detect_devices through daemon RPC
    void drainDaemonLog();

    // PL programming (background thread)
    void onProgramPl();
    void drawProgramPlModal();

    // SPI flash programming (background thread)
    void onProgramFlash();
    void drawProgramFlashModal();

    // Portable file dialog helpers (Win32 on Windows)
    static std::string openFileDialog(const char* title, const char* filter);
    static std::string saveFileDialog(const char* title, const char* filter);

    // GLFW / ImGui
    GLFWwindow* window_ = nullptr;
    bool show_device_dialog_ = false;
    bool show_trigger_dialog_ = false;
    bool show_about_ = false;
    bool show_help_ = false;
    int  help_topic_ = 0;
    std::string status_text_ = "Disconnected";
    bool dock_layout_initialized_ = false;
    bool reset_layout_requested_ = false;

    // Backend objects (constructed in dependency order)
    std::unique_ptr<jtag::FtdiDevice> ftdi_;
    std::unique_ptr<jtag::TapController> tap_;
    std::unique_ptr<jtag::JtagChain> chain_;
    std::unique_ptr<jtag::Scanner> scanner_;
    std::unique_ptr<jtag::PinDriver> pin_driver_;
    std::unique_ptr<jtag::CaptureEngine> capture_engine_;

    IlaPanel ila_panel_;

    bool connected_ = false;
    bool capturing_ = false;
    jtag::TriggerMode run_mode_ = jtag::TriggerMode::FREE_RUN;
    int bsdl_device_index_ = -1;
    std::chrono::steady_clock::time_point last_refresh_{};
    std::vector<jtag::SampleFrame> cached_samples_;  // updated at ~20 Hz; used every render frame
    size_t capture_last_total_ = 0;  // total_written_ value at last incremental fetch
    int capture_buffer_depth_ = 10000;  // ring buffer depth, configurable before capture
    std::string daemon_capture_job_id_;  // non-empty while a daemon capture job is running
    std::array<char, 8192> script_buffer_{};
    std::string script_path_;
    std::string script_output_;
    jtag::ScanResult pin_readback_;
    std::string pin_readback_error_;
    bool extest_outputs_active_ = false;

    // Test suite state
    std::vector<std::string> suite_paths_;
    std::string suite_path_;
    jtag::script::TestSuiteResult suite_result_;
    bool suite_has_result_ = false;
    std::string suite_output_;
    static constexpr size_t kSuiteOutputMax = 65536;

    // Interconnect device lists (populated on connect)
    std::vector<std::unique_ptr<jtag::Scanner>> chain_scanners_;
    std::vector<std::unique_ptr<jtag::PinDriver>> chain_drivers_;

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

    // Flash programming state
    bool program_flash_popup_requested_ = false;
    std::atomic<bool> program_flash_running_{false};
    std::atomic<int> program_flash_phase_{0};      // cast from FlashPhase
    std::atomic<size_t> program_flash_done_{0};
    std::atomic<size_t> program_flash_total_{0};
    std::atomic<bool> program_flash_success_{false};
    std::string program_flash_bin_path_;
    std::string program_flash_bridge_path_;
    std::string program_flash_error_;
    std::mutex program_flash_mutex_;
    std::thread program_flash_thread_;

    AppConfig config_;

    // Daemon process controller (optional; created on connect)
    std::unique_ptr<DaemonProcessController> daemon_ctrl_;
    std::unique_ptr<GuiDaemonClient>         gui_client_;
    uint16_t                                 daemon_last_reported_port_{0};
    bool                                     pending_daemon_connect_{false};

    // Daemon connect-via-RPC background operation (Phase 3)
    std::atomic<bool> daemon_connect_running_{false};
    std::mutex        daemon_connect_mutex_;
    std::string       daemon_connect_result_;  // set by bg thread, cleared on drain
    std::thread       daemon_connect_thread_;
    std::atomic<int>  daemon_bscane_idx_{-1};  // device index of Zynq PL TAP (-1 if not found)

    // Daemon hardware ownership cache — updated by periodic background poll.
    // Reflects hardware state as reported by the daemon's daemon/status RPC.
    std::atomic<bool> daemon_hw_open_{false};
    std::atomic<int>  daemon_device_count_{0};
    std::chrono::steady_clock::time_point last_daemon_status_poll_{};
    std::atomic<bool> daemon_status_poll_running_{false};
    std::thread       daemon_status_poll_thread_;
};

} // namespace jtag::gui
