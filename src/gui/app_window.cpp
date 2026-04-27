#include "app_window.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <GLFW/glfw3.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

#include "src/icon_rgba_64.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <thread>

#include "app_config.h"
#include "debug_log_panel.h"
#include "device_dialog.h"
#include "gui_theme.h"
#include "hex_panel.h"
#include "ila_panel.h"
#include "interconnect_panel.h"
#include "protocol_panel.h"
#include "signal_panel.h"
#include "src/boundary_scan/interconnect_test.h"
#include "src/config/pl_config.h"
#include "src/flash/flash_programmer.h"
#include "src/script/script_engine.h"
#include "src/script/test_suite.h"
#include "src/xdc/xdc_parser.h"
#include "trigger_dialog.h"
#include "vcd_export.h"
#include "waveform_view.h"

namespace jtag::gui {

static void glfwErrorCallback(int error, const char* description) {
    (void)error;
    (void)description;
}

static std::string firstLineOf(const std::string& message) {
    const size_t newline_pos = message.find('\n');
    if (newline_pos == std::string::npos) {
        return message;
    }
    return message.substr(0, newline_pos);
}

static std::string normalizeLineEndings(const std::string& input) {
    std::string result;
    result.reserve(input.size());

    for (size_t i = 0; i < input.size(); i++) {
        if (input[i] == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                i++;
            }
            result.push_back('\n');
            continue;
        }
        result.push_back(input[i]);
    }

    return result;
}

static const char* pinStateLabel(jtag::PinState state) {
    switch (state) {
        case jtag::PinState::LOW:
            return "LOW";
        case jtag::PinState::HIGH:
            return "HIGH";
        case jtag::PinState::UNKNOWN:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

static const char* stagedPinLabel(int value) {
    if (value < 0) {
        return "HIGH-Z";
    }
    return value == 0 ? "LOW" : "HIGH";
}

static std::vector<std::string> visibleDrivablePins(
        jtag::Scanner* scanner) {
    std::vector<std::string> result;
    if (scanner == nullptr) {
        return result;
    }

    const auto drivable = scanner->getDrivablePins();
    if (drivable.empty()) {
        return result;
    }

    const auto selected = SignalPanel::selectedSignals();
    std::set<std::string> drivable_set(drivable.begin(), drivable.end());
    for (const auto& pin : selected) {
        if (drivable_set.count(pin) > 0) {
            result.push_back(pin);
        }
    }

    if (!result.empty()) {
        return result;
    }
    return drivable;
}

class GuiScriptHost : public jtag::script::ScriptHost {
public:
    GuiScriptHost(jtag::Scanner& scanner, jtag::PinDriver& pin_driver,
                  bool& extest_outputs_active)
        : scanner_(scanner), pin_driver_(pin_driver),
          extest_outputs_active_(extest_outputs_active) {}

    bool sample(jtag::ScanResult& result, std::string& error) override {
        result = scanner_.sample();
        if (result.raw_bsr.empty()) {
            error = scanner_.lastError();
            return false;
        }
        pin_driver_.loadSnapshot(result.raw_bsr);
        extest_outputs_active_ = false;
        return true;
    }

    bool setPin(const std::string& pin_name, int value,
                std::string& error) override {
        if (!pin_driver_.setPin(pin_name, value)) {
            error = pin_driver_.lastError();
            return false;
        }
        return true;
    }

    bool setPinHighZ(const std::string& pin_name,
                     std::string& error) override {
        if (!pin_driver_.setPinHighZ(pin_name)) {
            error = pin_driver_.lastError();
            return false;
        }
        return true;
    }

    bool applyOutputs(std::string& error) override {
        if (!pin_driver_.applyOutputs()) {
            error = pin_driver_.lastError();
            return false;
        }
        extest_outputs_active_ = true;
        return true;
    }

    void resetToSafe() override {
        pin_driver_.resetToSafe();
    }

    bool sleepMs(int milliseconds, std::string& error) override {
        if (milliseconds < 0) {
            error = "sleep duration must be >= 0";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        return true;
    }

private:
    jtag::Scanner& scanner_;
    jtag::PinDriver& pin_driver_;
    bool& extest_outputs_active_;
};

// Convert a JSON samples array (from daemon captureGetSamples) to SampleFrame
// vector.  The JSON format matches samplesToJson() in register_tools.cpp:
//   [{ "timestamp_us": <int64>, "trigger_point": bool,
//      "pins": { "PIN_NAME": "high"|"low"|"unknown" } }]
static std::vector<jtag::SampleFrame> samplesFromJson(
    const nlohmann::json& arr) {
    std::vector<jtag::SampleFrame> frames;
    if (!arr.is_array()) return frames;
    frames.reserve(arr.size());
    for (const auto& s : arr) {
        jtag::SampleFrame f;
        if (s.contains("timestamp_us") && s["timestamp_us"].is_number_integer()) {
            f.timestamp = std::chrono::steady_clock::time_point{
                std::chrono::microseconds{
                    s["timestamp_us"].get<long long>()}};
        }
        f.trigger_point = s.value("trigger_point", false);
        if (s.contains("pins") && s["pins"].is_object()) {
            for (auto it = s["pins"].begin(); it != s["pins"].end(); ++it) {
                jtag::PinState state = jtag::PinState::UNKNOWN;
                if (it.value().is_string()) {
                    const std::string& v = it.value().get<std::string>();
                    if (v == "high")      state = jtag::PinState::HIGH;
                    else if (v == "low")  state = jtag::PinState::LOW;
                }
                f.data.pin_states[it.key()] = state;
            }
        }
        frames.push_back(std::move(f));
    }
    return frames;
}

static void syncSelectionViews(const std::vector<jtag::SampleFrame>& samples) {
    const auto selected = SignalPanel::selectedSignals();
    const auto& buses   = SignalPanel::buses();

    if (selected.empty() && buses.empty()) {
        WaveformView::clearData();
        HexPanel::updateValues(
            samples.empty() ? jtag::ScanResult{} : samples.back().data,
            selected);
        HexPanel::setBuses({});
        return;
    }

    WaveformView::setData(selected, buses, samples);
    HexPanel::setBuses(buses);

    if (samples.empty()) {
        WaveformView::setCursorPosition(-1);
        HexPanel::updateValues(jtag::ScanResult{}, selected);
        return;
    }

    int trigger_index = -1;
    for (size_t i = 0; i < samples.size(); i++) {
        if (samples[i].trigger_point) {
            trigger_index = static_cast<int>(i);
            break;
        }
    }
    WaveformView::setCursorPosition(trigger_index);

    HexPanel::updateValues(samples.back().data, selected);
}

// ── File dialog helpers ─────────────────────────────────────────────

std::string AppWindow::openFileDialog(const char* title, const char* filter) {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return path;
#endif
    return {};
}

std::string AppWindow::saveFileDialog(const char* title, const char* filter) {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) return path;
#endif
    return {};
}

// ── Constructor / Destructor ────────────────────────────────────────

AppWindow::AppWindow() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) return;

    daemon_ctrl_ = std::make_unique<DaemonProcessController>();
    gui_client_  = std::make_unique<GuiDaemonClient>();

    // Load saved config
    config_ = AppConfig::load("cfg.json");

    // Pre-fill device dialog from saved config
    {
        DeviceConfig dc;
        dc.vendor_id         = config_.vendor_id;
        dc.product_id        = config_.product_id;
        dc.serial            = config_.serial;
        dc.interface_channel = config_.interface_channel;
        dc.clock_freq_hz     = config_.clock_freq_hz;
        DeviceDialog::setConfig(dc);
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window_ = glfwCreateWindow(1280, 800,
                                "JTAG FPGA Waveform Viewer", nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        return;
    }

    // Set window / taskbar icon from embedded RGBA data.
    GLFWimage icon_image;
    icon_image.width  = kIconWidth;
    icon_image.height = kIconHeight;
    // GLFW requires a non-const pointer; the pixels are not modified.
    icon_image.pixels = const_cast<unsigned char*>(kIconRgba);
    glfwSetWindowIcon(window_, 1, &icon_image);

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    const float dpi_scale = theme::computeDpiScale(window_);
    theme::applyFonts(io, 16.0f, dpi_scale);
    theme::applyDarkTheme();
    theme::scaleStyleForDpi(dpi_scale);

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::snprintf(script_buffer_.data(), script_buffer_.size(),
                  "# set LED0 HIGH, apply, then confirm it\n"
                  "# set LED0 1\n"
                  "# apply\n"
                  "# expect LED0 1\n");

    setStatusMessage(status_text_);
}

AppWindow::~AppWindow() {
    // Save config before destroying
    config_.selected_pins = SignalPanel::selectedSignals();
    config_.buses         = SignalPanel::buses();
    config_.ila_signals   = ila_panel_.exportSignalConfigs();
    config_.ila_or_mode   = ila_panel_.orMode();
    config_.save("cfg.json");

    onDisconnect();

    // Disconnect GUI RPC client and join background threads.
    if (gui_client_ && gui_client_->isConnected()) gui_client_->disconnect();
    if (daemon_status_poll_thread_.joinable()) daemon_status_poll_thread_.join();
    if (daemon_connect_thread_.joinable())     daemon_connect_thread_.join();

    if (window_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

// ── Main loop ───────────────────────────────────────────────────────

void AppWindow::run() {
    if (!window_) return;

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        beginFrame();

        buildDockspace();

        // Panels
        SignalPanel::draw();
        if (SignalPanel::consumeSelectionChanged() ||
            SignalPanel::consumeBusChanged()) {
            syncSelectionViews(cached_samples_);
        }
        // Daemon mode: capture_engine_ is always null, so also enable when
        // the GUI RPC is connected and a BSDL has been loaded.
        const bool daemon_can_capture =
            gui_client_ && gui_client_->isConnected() && bsdl_device_index_ >= 0;
        const bool can_capture =
            (capture_engine_ != nullptr || daemon_can_capture) && !capturing_;
        const auto waveform_actions =
            WaveformView::draw(can_capture, capturing_);
        if (waveform_actions.run_requested) {
            onStartCapture();
        }
        if (waveform_actions.single_requested) {
            onSingleCapture();
        }
        if (waveform_actions.stop_requested) {
            onStopCapture();
        }
        if (waveform_actions.clear_requested) {
            onClearWaveforms();
        }
        if (waveform_actions.fit_requested) {
            onFitWaveforms();
        }
        HexPanel::draw();

        // Protocol panel
        {
            ProtocolPanel::draw(SignalPanel::selectedSignals(), cached_samples_);
            if (ProtocolPanel::consumeNewFrames()) {
                WaveformView::setAnnotations(ProtocolPanel::decodedFrames());
            }
        }

        drawPinControlPanel();
        drawScriptRunnerPanel();
        drawInterconnectPanel();
        ila_panel_.draw();
        drainDaemonLog();
        DebugLogPanel::draw(status_text_);

        // Device dialog
        if (show_device_dialog_) {
            DeviceDialog::draw(&show_device_dialog_);
            if (DeviceDialog::accepted()) {
                onConnect();
            }
        }
        // Trigger dialog
        if (show_trigger_dialog_) {
            TriggerDialog::draw(&show_trigger_dialog_);
            // Sync run_mode_ when dialog closes (OK pressed)
            if (!show_trigger_dialog_ && capture_engine_) {
                auto m = capture_engine_->trigger().mode();
                if (m != jtag::TriggerMode::SINGLE) run_mode_ = m;
            }
        }
        // About
        if (show_about_) {
            if (ImGui::Begin("About", &show_about_,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("JTAG FPGA Waveform Viewer");
                ImGui::Separator();
                ImGui::Text("Reads Xilinx FPGA pin states via JTAG boundary scan");
                ImGui::Text("and displays them as logic analyzer waveforms.");
                ImGui::Separator();
                ImGui::Text("Uses libftdi (MPSSE mode) for JTAG communication.");
                ImGui::Text("Supports BSDL files for pin mapping.");
            }
            ImGui::End();
        }
        // Help
        if (show_help_) {
            drawHelpWindow();
        }

        // PL programming progress modal
        drawProgramPlModal();

        // Flash programming progress modal
        drawProgramFlashModal();

        // Persistent status bar at the bottom of the viewport
        buildStatusBar();

        // Periodic refresh from capture engine (~20 Hz)
        if (capturing_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_refresh_).count();
            if (elapsed >= 50) {
                refreshFromCapture();
                last_refresh_ = now;
            }
        }

        endFrame();
    }
}

void AppWindow::buildStatusBar() {
    const float h = ImGui::GetFrameHeight();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!ImGui::BeginViewportSideBar("##StatusBar", vp, ImGuiDir_Down, h,
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_MenuBar |
            ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginMenuBar()) {
        // ── Connection ───────────────────────────────────
        if (connected_) {
            ImGui::TextColored(theme::kSuccess, "● Connected");
        } else if (daemon_hw_open_.load(std::memory_order_acquire)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "● Daemon (%d dev)",
                          daemon_device_count_.load(std::memory_order_acquire));
            ImGui::TextColored(theme::kAccent, "%s", buf);
        } else {
            ImGui::TextColored(theme::kMuted, "○ Disconnected");
        }

        ImGui::SameLine();
        ImGui::TextColored(theme::kMuted, "|");
        ImGui::SameLine();

        // ── Capture state ────────────────────────────────
        if (capturing_) {
            ImGui::TextColored(theme::kAccent, "▶ Capturing");
        } else if (capture_engine_) {
            ImGui::TextColored(theme::kMuted, "■ Idle");
        } else {
            ImGui::TextColored(theme::kMuted, "— No engine");
        }

        ImGui::SameLine();
        ImGui::TextColored(theme::kMuted, "|");
        ImGui::SameLine();
        ImGui::TextColored(theme::kMuted, "Samples:");
        ImGui::SameLine();
        ImGui::Text("%zu", cached_samples_.size());

        // ── Right-aligned: status message + FPS ──────────
        const ImGuiIO& io = ImGui::GetIO();
        char fps_buf[32];
        std::snprintf(fps_buf, sizeof(fps_buf), "%.0f FPS", io.Framerate);

        const float right_pad   = 12.0f;
        const float fps_w       = ImGui::CalcTextSize(fps_buf).x;
        const float status_w    = ImGui::CalcTextSize(status_text_.c_str()).x;
        const float sep_w       = ImGui::CalcTextSize(" | ").x;
        const float reserved    = fps_w + sep_w + status_w + right_pad;
        const float avail       = ImGui::GetContentRegionAvail().x;
        if (avail > reserved) {
            ImGui::SameLine(0.0f, avail - reserved);
        } else {
            ImGui::SameLine();
        }
        ImGui::TextColored(theme::kMuted, "%s", status_text_.c_str());
        ImGui::SameLine();
        ImGui::TextColored(theme::kMuted, "|");
        ImGui::SameLine();
        ImGui::TextColored(theme::kMuted, "%s", fps_buf);

        ImGui::EndMenuBar();
    }
    ImGui::End();
}

// ── Backend actions ─────────────────────────────────────────────────

void AppWindow::onConnect() {
    onDisconnect();

    const auto& cfg = DeviceDialog::config();

    // Persist device settings so jtag_daemon reads the same config.
    config_.vendor_id         = cfg.vendor_id;
    config_.product_id        = cfg.product_id;
    config_.serial            = cfg.serial;
    config_.interface_channel = cfg.interface_channel;
    config_.clock_freq_hz     = cfg.clock_freq_hz;
    config_.save("cfg.json");

    // Phase 5: always use the daemon path.
    // Start daemon if it is not already running.
    const DaemonStatus ds = daemon_ctrl_->status();
    if (ds.state == DaemonState::kOff || ds.state == DaemonState::kError) {
        onDaemonStart();
    }

    // If daemon is already up with a gui_port, connect immediately.
    const DaemonStatus ds2 = daemon_ctrl_->status();
    if (ds2.state == DaemonState::kRunning && ds2.gui_port != 0) {
        onDaemonConnect();
        // onDaemonConnect() is async; connected_ will be set once it completes
        // (see drainDaemonLog).
    } else {
        // Daemon is still starting; auto-connect once it reports gui_port.
        pending_daemon_connect_ = true;
        setStatusMessage("Daemon starting... will connect automatically.");
    }
}

void AppWindow::onDisconnect() {
    // Wait for any PL programming thread before destroying backend objects
    if (program_pl_thread_.joinable()) program_pl_thread_.join();
    if (program_flash_thread_.joinable()) program_flash_thread_.join();

    const bool had_backend_state = capturing_ || connected_ ||
        !daemon_capture_job_id_.empty() ||
        capture_engine_ != nullptr || pin_driver_ != nullptr ||
        scanner_ != nullptr || chain_ != nullptr || tap_ != nullptr ||
        (ftdi_ != nullptr && ftdi_->isOpen());

    // Cancel any in-flight daemon capture job.
    if (!daemon_capture_job_id_.empty() && gui_client_ &&
        gui_client_->isConnected()) {
        try { gui_client_->captureStop(daemon_capture_job_id_); } catch (...) {}
        daemon_capture_job_id_.clear();
    }

    if (capturing_) {
        onStopCapture();
    }

    // Disconnect GUI RPC client.
    if (gui_client_) gui_client_->disconnect();

    capture_engine_.reset();
    pin_driver_.reset();
    scanner_.reset();
    chain_scanners_.clear();
    chain_drivers_.clear();
    ila_panel_.setChain(nullptr, 0);
    ila_panel_.setDaemonClient(nullptr, 0, false);
    daemon_bscane_idx_.store(-1, std::memory_order_release);
    chain_.reset();
    tap_.reset();
    if (ftdi_ && ftdi_->isOpen()) {
        ftdi_->close();
    }
    ftdi_.reset();
    connected_ = false;
    bsdl_device_index_ = -1;

    // Save config on disconnect (preserves last BSDL path + pin selection)
    config_.selected_pins = SignalPanel::selectedSignals();
    config_.save("cfg.json");

    SignalPanel::clear();
    SignalPanel::setXdcAliases({});
    cached_samples_.clear();
    WaveformView::clearData();
    WaveformView::setSignalAliases({});
    HexPanel::setBuses({});
    HexPanel::setXdcAliases({});
    pin_readback_ = jtag::ScanResult{};
    pin_readback_error_.clear();
    extest_outputs_active_ = false;
    if (had_backend_state || status_text_ != "Disconnected") {
        setStatusMessage("Disconnected");
    }
}

void AppWindow::onOpenBsdl() {
    if (!connected_) return;

    // If we are in daemon mode but the RPC link has dropped, report it clearly
    // instead of silently falling through to the direct path (which fails too
    // because chain_ is always null in Phase-5 daemon mode).
    if (!chain_ && gui_client_ && !gui_client_->isConnected()) {
        DebugLogPanel::append("[daemon] BSDL load failed: daemon RPC disconnected");
        setStatusMessage("Error: daemon disconnected. Please reconnect.");
        return;
    }

    std::string path = openFileDialog("Open BSDL File",
        "BSDL Files (*.bsdl;*.bsd)\0*.bsdl;*.bsd\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;

    // ── Daemon path ──────────────────────────────────────────────────────────
    if (gui_client_ && gui_client_->isConnected() && !chain_) {
        int dev_idx = 0;
        try {
            gui_client_->loadBsdl(dev_idx, path);
            const auto pins_json = gui_client_->listPins(dev_idx);
            std::vector<std::string> obs, drv;
            if (pins_json.contains("observable") &&
                pins_json["observable"].is_array()) {
                for (const auto& p : pins_json["observable"])
                    obs.push_back(p.get<std::string>());
            }
            if (pins_json.contains("drivable") &&
                pins_json["drivable"].is_array()) {
                for (const auto& p : pins_json["drivable"])
                    drv.push_back(p.get<std::string>());
            }
            SignalPanel::populateFromPinLists(obs, drv);
            // Restore previously selected pins if BSDL path matches.
            if (path == config_.bsdl_path && !config_.selected_pins.empty())
                SignalPanel::setSelectedSignals(config_.selected_pins);

            bsdl_device_index_ = dev_idx;
            config_.bsdl_path         = path;
            config_.bsdl_device_index = dev_idx;
            config_.save("cfg.json");
            extest_outputs_active_ = false;
            TriggerDialog::bind(nullptr, nullptr);

            char buf[256];
            snprintf(buf, sizeof(buf),
                     "BSDL loaded via daemon. %zu observable / %zu drivable pins.",
                     obs.size(), drv.size());
            setStatusMessage(buf);
        } catch (const std::exception& e) {
            DebugLogPanel::append(std::string("[daemon] BSDL load error: ") + e.what());
            setStatusMessage(std::string("BSDL load error: ") + e.what());
        }
        return;
    }

    // ── Direct path ──────────────────────────────────────────────────────────
    if (!chain_) return;

    // Load for first device by default (device 0)
    int dev_idx = 0;
    if (!chain_->loadBsdl(dev_idx, path)) {
        setStatusMessage("BSDL load failed: " + chain_->lastError());
        return;
    }

    bsdl_device_index_ = dev_idx;

    // Create scanner and pin driver
    scanner_ = std::make_unique<jtag::Scanner>(*chain_, dev_idx);
    pin_driver_ = std::make_unique<jtag::PinDriver>(*chain_, dev_idx);

    if (!scanner_->isReady()) {
        setStatusMessage("Scanner not ready: " + scanner_->lastError());
        return;
    }

    // Populate signal panel from BSDL
    const auto* bsdl_dev = scanner_->bsdlDevice();
    if (bsdl_dev) {
        SignalPanel::populateFromBsdl(*bsdl_dev);
        // Restore previously selected pins if BSDL path matches
        if (path == config_.bsdl_path && !config_.selected_pins.empty()) {
            SignalPanel::setSelectedSignals(config_.selected_pins);
        }
    }

    // Save BSDL path to config
    config_.bsdl_path = path;
    config_.bsdl_device_index = dev_idx;
    config_.save("cfg.json");

    // Create capture engine
    capture_engine_ = std::make_unique<jtag::CaptureEngine>(*scanner_);
    extest_outputs_active_ = false;

    // Build per-device scanner/driver lists for interconnect testing
    // (one entry per device; entries for devices without BSDL are nullptr)
    chain_scanners_.clear();
    chain_drivers_.clear();
    const auto& devs = chain_->devices();
    for (size_t i = 0; i < devs.size(); i++) {
        if (devs[i].bsdl) {
            chain_scanners_.push_back(
                std::make_unique<jtag::Scanner>(*chain_, static_cast<int>(i)));
            chain_drivers_.push_back(
                std::make_unique<jtag::PinDriver>(*chain_, static_cast<int>(i)));
        } else {
            chain_scanners_.push_back(nullptr);
            chain_drivers_.push_back(nullptr);
        }
    }

    // Bind trigger dialog
    TriggerDialog::bind(scanner_.get(), &capture_engine_->trigger());

    // Quick verify: read IDCODE + do one test sample for diagnostics
    uint32_t idcode = 0;
    if (scanner_->readIdCode(idcode)) {
        char buf[512];
        const auto* bd = bsdl_dev;
        auto idcode_op = bd ? bd->idcodeOpcode() : std::nullopt;
        auto sample_op = bd ? bd->sampleOpcode() : std::nullopt;
        int total_ir = 0;
        for (const auto& dev : chain_->devices()) total_ir += dev.ir_length;
        int n = snprintf(buf, sizeof(buf),
            "BSDL loaded. IDCODE=0x%08X. %zu pins. BSR_len=%d\n"
            "SAMPLE_op=0x%02X  IDCODE_op=0x%02X  total_IR=%d  ir[0]=%d ir[1]=%d\n",
            idcode,
            scanner_->getObservablePins().size(),
            bd ? bd->boundary_length : -1,
            (int)sample_op.value_or(0xFF),
            (int)idcode_op.value_or(0xFF),
            total_ir,
            chain_->devices().size() > 0 ? chain_->devices()[0].ir_length : -1,
            chain_->devices().size() > 1 ? chain_->devices()[1].ir_length : -1);

        // Do a test sample and show raw BSR bytes + any named pin value
        auto result = scanner_->sample();
        if (!result.raw_bsr.empty()) {
            pin_readback_ = result;
            if (pin_driver_) {
                pin_driver_->loadSnapshot(result.raw_bsr);
            }
            pin_readback_error_.clear();
            extest_outputs_active_ = false;
            n += snprintf(buf + n, sizeof(buf) - n, "BSR[0..7]:");
            for (size_t i = 0; i < 8 && i < result.raw_bsr.size(); i++)
                n += snprintf(buf + n, sizeof(buf) - n, " %02X", result.raw_bsr[i]);
            int nz = 0;
            for (auto b : result.raw_bsr) if (b) nz++;
            n += snprintf(buf + n, sizeof(buf) - n,
                " (%d/%zu non-zero bytes, %zu decoded pins)",
                nz, result.raw_bsr.size(), result.pin_states.size());
        } else {
            pin_readback_ = jtag::ScanResult{};
            pin_readback_error_ = scanner_->lastError();
            extest_outputs_active_ = false;
            n += snprintf(buf + n, sizeof(buf) - n, "BSR empty! Error: %s",
                scanner_->lastError().c_str());
        }
        // Show DONE_T12 if present
        if (bsdl_dev) {
            for (const auto& cell : bsdl_dev->boundary_cells) {
                if (cell.pin_name == "DONE_T12") {
                    bool bit = result.getBit(cell.position);
                    n += snprintf(buf + n, sizeof(buf) - n,
                        "\nDONE_T12: BSR pos=%d  raw_bit=%d  decoded=%s",
                        cell.position, (int)bit,
                        result.pin_states.count("DONE_T12")
                            ? (result.pin_states.at("DONE_T12") == jtag::PinState::HIGH ? "HIGH" : "LOW")
                            : "UNKNOWN");
                    break;
                }
            }
        }
        setStatusMessage(buf);
    } else {
        pin_readback_ = jtag::ScanResult{};
        pin_readback_error_ = scanner_->lastError();
        extest_outputs_active_ = false;
        setStatusMessage("BSDL loaded. " +
            std::to_string(scanner_->getObservablePins().size()) +
            " pins. IDCODE read failed.");
    }
}

bool AppWindow::refreshPinReadback(bool report_status) {
    // ── Daemon path ──────────────────────────────────────────────────────────
    if (gui_client_ && gui_client_->isConnected() && !scanner_) {
        if (bsdl_device_index_ < 0) {
            if (report_status) setStatusMessage("No BSDL loaded (daemon mode).");
            return false;
        }
        try {
            const auto result = gui_client_->sampleBsr(bsdl_device_index_);
            pin_readback_.pin_states.clear();
            pin_readback_.raw_bsr.clear();
            if (result.contains("pins") && result["pins"].is_object()) {
                for (auto it = result["pins"].begin();
                     it != result["pins"].end(); ++it) {
                    jtag::PinState st = jtag::PinState::UNKNOWN;
                    const std::string& sv = it.value().get<std::string>();
                    if (sv == "high") st = jtag::PinState::HIGH;
                    else if (sv == "low") st = jtag::PinState::LOW;
                    pin_readback_.pin_states[it.key()] = st;
                }
            }
            pin_readback_error_.clear();
            extest_outputs_active_ = false;
            if (report_status) {
                setStatusMessage("Pin readback via daemon: " +
                                 std::to_string(pin_readback_.pin_states.size()) +
                                 " pins.");
            }
            return true;
        } catch (const std::exception& e) {
            pin_readback_error_ = e.what();
            if (report_status)
                setStatusMessage("Readback failed: " + pin_readback_error_);
            return false;
        }
    }

    // ── Direct path ──────────────────────────────────────────────────────────
    if (!scanner_) {
        if (report_status) {
            setStatusMessage("Scanner not ready.");
        }
        return false;
    }

    const auto result = scanner_->sample();
    if (result.raw_bsr.empty()) {
        pin_readback_error_ = scanner_->lastError();
        if (report_status) {
            setStatusMessage("Readback failed: " + scanner_->lastError());
        }
        return false;
    }

    pin_readback_ = result;
    if (pin_driver_) {
        pin_driver_->loadSnapshot(result.raw_bsr);
    }
    pin_readback_error_.clear();
    extest_outputs_active_ = false;

    if (report_status) {
        setStatusMessage("Pin readback updated via SAMPLE (" +
                         std::to_string(pin_readback_.pin_states.size()) +
                         " pins decoded).");
    }
    return true;
}

void AppWindow::drawPinControlPanel() {
    ImGui::Begin("Pin Control");

    // ── Daemon mode: limited read-only view ──────────────────────────────────
    if (gui_client_ && gui_client_->isConnected() && !scanner_) {
        if (bsdl_device_index_ < 0) {
            ImGui::TextDisabled("Load a BSDL file to enable pin readback.");
            ImGui::End();
            return;
        }
        if (capturing_) {
            ImGui::TextDisabled("Stop capture before driving pins.");
            ImGui::End();
            return;
        }
        if (ImGui::Button("Readback (SAMPLE)")) {
            refreshPinReadback(true);
        }
        ImGui::Separator();
        if (!pin_readback_.pin_states.empty()) {
            ImGui::TextDisabled("Read-only pin state from daemon:");
            for (const auto& [name, state] : pin_readback_.pin_states) {
                const char* sv = (state == jtag::PinState::HIGH) ? "HIGH"
                               : (state == jtag::PinState::LOW)  ? "LOW"
                               : "?";
                ImGui::Text("  %-32s  %s", name.c_str(), sv);
            }
        } else if (!pin_readback_error_.empty()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                               "Error: %s", pin_readback_error_.c_str());
        }
        ImGui::Separator();
        ImGui::TextDisabled("EXTEST pin drive: use Script Runner in daemon mode.");
        ImGui::End();
        return;
    }

    if (!scanner_ || !pin_driver_) {
        ImGui::TextDisabled("Load a BSDL file to enable EXTEST pin control.");
        ImGui::End();
        return;
    }

    if (capturing_) {
        ImGui::TextDisabled("Stop capture before driving pins.");
        ImGui::End();
        return;
    }

    const bool drive_allowed = pin_driver_->extestAllowed();
    const std::string drive_block_reason =
        drive_allowed ? std::string{} : pin_driver_->extestBlockedReason();

    const auto pins = visibleDrivablePins(scanner_.get());
    if (pins.empty()) {
        ImGui::TextDisabled("No drivable pins were found in the loaded BSDL.");
        ImGui::End();
        return;
    }

    if (ImGui::Button(extest_outputs_active_ ? "Readback (release EXTEST)"
                                             : "Readback")) {
        refreshPinReadback(true);
    }
    ImGui::SameLine();
    if (!drive_allowed) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Apply Staged")) {
        if (pin_driver_->applyOutputs()) {
            extest_outputs_active_ = true;
            pin_readback_error_.clear();
            setStatusMessage(
                "Applied staged EXTEST outputs. They remain active until SAMPLE/readback/capture changes the instruction.");
        } else {
            setStatusMessage("Apply failed: " + pin_driver_->lastError());
        }
    }
    if (!drive_allowed) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!drive_allowed) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Reset Safe")) {
        pin_driver_->resetToSafe();
        if (pin_driver_->applyOutputs()) {
            extest_outputs_active_ = true;
            pin_readback_error_.clear();
            setStatusMessage(
                "Applied BSDL safe output state in EXTEST. It remains active until SAMPLE/readback/capture changes the instruction.");
        } else {
            setStatusMessage("Reset Safe failed: " + pin_driver_->lastError());
        }
    }
    if (!drive_allowed) {
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    if (!drive_allowed) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                           "%s", drive_block_reason.c_str());
        ImGui::TextDisabled(
            "Readback is still available, but EXTEST drive is disabled to avoid resetting the running PS.");
    } else if (extest_outputs_active_) {
        ImGui::TextColored(ImVec4(0.85f, 0.9f, 0.45f, 1.0f),
                           "EXTEST active: staged outputs are being driven. Readback uses SAMPLE and will release them.");
    } else if (!pin_readback_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                           "Last readback failed: %s",
                           pin_readback_error_.c_str());
    } else if (!pin_readback_.raw_bsr.empty()) {
        ImGui::TextDisabled("Last readback decoded %zu pins.",
                            pin_readback_.pin_states.size());
    } else {
        ImGui::TextDisabled("No successful readback yet.");
    }
    ImGui::TextDisabled(
        "Showing selected drivable pins first. If none are selected, all drivable pins are shown.");

    if (ImGui::BeginTable("pin_control", 6,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Pin");
        ImGui::TableSetupColumn("Observed (SAMPLE)");
        ImGui::TableSetupColumn("Staged");
        ImGui::TableSetupColumn("Low");
        ImGui::TableSetupColumn("High");
        ImGui::TableSetupColumn("Z");
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < pins.size(); i++) {
            const auto& pin = pins[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pin.c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pinStateLabel(pin_readback_.getPin(pin)));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(stagedPinLabel(pin_driver_->getPinValue(pin)));

            ImGui::TableNextColumn();
            if (!drive_allowed) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("0")) {
                if (pin_driver_->setPin(pin, 0)) {
                    setStatusMessage("Staged " + pin + " = LOW");
                } else {
                    setStatusMessage("Stage failed: " + pin_driver_->lastError());
                }
            }
            if (!drive_allowed) {
                ImGui::EndDisabled();
            }

            ImGui::TableNextColumn();
            if (!drive_allowed) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("1")) {
                if (pin_driver_->setPin(pin, 1)) {
                    setStatusMessage("Staged " + pin + " = HIGH");
                } else {
                    setStatusMessage("Stage failed: " + pin_driver_->lastError());
                }
            }
            if (!drive_allowed) {
                ImGui::EndDisabled();
            }

            ImGui::TableNextColumn();
            if (!drive_allowed) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("Z")) {
                if (pin_driver_->setPinHighZ(pin)) {
                    setStatusMessage("Staged " + pin + " = HIGH-Z");
                } else {
                    setStatusMessage("Stage failed: " + pin_driver_->lastError());
                }
            }
            if (!drive_allowed) {
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void AppWindow::drawScriptRunnerPanel() {
    ImGui::Begin("Script Runner");

    ImGui::TextDisabled("%s",
                        script_path_.empty() ? "Unsaved script"
                                             : script_path_.c_str());
    if (ImGui::Button("Load Script...")) {
        onLoadScript();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Script")) {
        onSaveScript(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Script As...")) {
        onSaveScript(true);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Commands: sample, read, set, highz, apply, expect, sleep, reset_safe");
    ImGui::InputTextMultiline("##script_text", script_buffer_.data(),
                              script_buffer_.size(), ImVec2(-FLT_MIN, 220.0f));

    const bool daemon_mode = gui_client_ && gui_client_->isConnected() && !scanner_;
    const bool can_run_script =
        (!daemon_mode && scanner_ != nullptr && pin_driver_ != nullptr && !capturing_) ||
        (daemon_mode && bsdl_device_index_ >= 0 && !capturing_);
    if (!can_run_script) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Run Script")) {
        if (daemon_mode) {
            // Daemon path: send script text via runScript RPC.
            try {
                auto res = gui_client_->runScript(
                    bsdl_device_index_,
                    std::string(script_buffer_.data()));
                script_output_ = res.value("output", "");
                if (res.value("ok", false)) {
                    setStatusMessage("Script completed successfully (daemon).");
                } else {
                    setStatusMessage("Script failed (daemon).");
                }
            } catch (const std::exception& e) {
                script_output_ = e.what();
                setStatusMessage("Script error (daemon).");
            }
        } else {
            // Direct path.
            GuiScriptHost host(*scanner_, *pin_driver_, extest_outputs_active_);
            const auto result = jtag::script::ScriptEngine::run(
                std::string(script_buffer_.data()), host);
            script_output_ = result.output;
            if (result.success) {
                if (extest_outputs_active_) {
                    setStatusMessage(
                        "Script completed successfully. EXTEST outputs remain active until SAMPLE/readback/capture changes the instruction.");
                } else {
                    setStatusMessage("Script completed successfully.");
                }
            } else {
                setStatusMessage("Script failed at line " +
                                 std::to_string(result.failed_line) + ".");
            }
        }
    }
    if (!can_run_script) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Output")) {
        script_output_.clear();
    }

    if (!can_run_script) {
        ImGui::TextDisabled(
            "Load a BSDL and stop capture before running scripts.");
    } else if (!daemon_mode && !pin_driver_->extestAllowed()) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
            "Drive commands are blocked for this device. sample/read/expect/sleep remain available.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Output");
    ImGui::BeginChild("script_output", ImVec2(0.0f, 180.0f), true);
    ImGui::TextUnformatted(script_output_.c_str());
    ImGui::EndChild();

    // ── Suite runner ───────────────────────────────────────────────
    ImGui::Separator();
    ImGui::Text("Test Suite");
    if (ImGui::Button("Load Suite...")) {
        onLoadSuite();
    }
    ImGui::SameLine();
    const bool can_run_suite =
        !suite_paths_.empty() && can_run_script;
    if (!can_run_suite) ImGui::BeginDisabled();
    if (ImGui::Button("Run Suite")) {
        onRunSuite();
    }
    if (!can_run_suite) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!suite_has_result_) ImGui::BeginDisabled();
    if (ImGui::Button("Export Report...")) {
        onExportSuiteReport();
    }
    if (!suite_has_result_) ImGui::EndDisabled();

    if (!suite_path_.empty()) {
        ImGui::TextDisabled("%s  (%d tests)",
                            suite_path_.c_str(),
                            static_cast<int>(suite_paths_.size()));
    }

    if (suite_has_result_) {
        ImGui::Text("PASS %d / %d",
                    suite_result_.pass_count,
                    static_cast<int>(suite_result_.cases.size()));

        if (ImGui::BeginTable("suite_results", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 110.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Script");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Expects");
            ImGui::TableHeadersRow();

            for (const auto& tc : suite_result_.cases) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(tc.name.c_str());
                ImGui::TableSetColumnIndex(1);
                if (tc.success) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                         ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                    ImGui::TextUnformatted("PASS");
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                         ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    char buf[32];
                    snprintf(buf, sizeof(buf), "FAIL L%d", tc.failed_line);
                    ImGui::TextUnformatted(buf);
                }
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d/%d",
                            tc.total_expects - tc.failed_expects,
                            tc.total_expects);
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

void AppWindow::onStartCapture() {
    // Daemon path: submit capture_start job and track job_id.
    if (gui_client_ && gui_client_->isConnected() && !capture_engine_) {
        if (bsdl_device_index_ < 0) return;
        if (capturing_) return;
        try {
            const std::string mode_str =
                (run_mode_ == jtag::TriggerMode::SINGLE) ? "single" :
                (run_mode_ == jtag::TriggerMode::NORMAL) ? "normal" : "free_run";
            auto result = gui_client_->captureStart(bsdl_device_index_, 10000,
                                                    1000, mode_str);
            if (result.contains("job_id")) {
                daemon_capture_job_id_ = result["job_id"].get<std::string>();
                capturing_ = true;
                extest_outputs_active_ = false;
                cached_samples_.clear();
                last_refresh_ = std::chrono::steady_clock::now();
                syncSelectionViews(std::vector<jtag::SampleFrame>{});
                WaveformView::requestResetView();
                setStatusMessage("Capturing via daemon...");
            } else {
                setStatusMessage("Capture error: no job_id returned");
            }
        } catch (const std::exception& e) {
            setStatusMessage(std::string("Capture error: ") + e.what());
        }
        return;
    }
    // Direct path.
    if (!capture_engine_) return;
    if (capture_engine_->state() != jtag::CaptureState::STOPPED)
        capture_engine_->stop();
    capture_engine_->trigger().setMode(run_mode_);
    if (capture_engine_->start()) {
        capturing_ = true;
        extest_outputs_active_ = false;
        cached_samples_.clear();
        last_refresh_ = std::chrono::steady_clock::now();
        syncSelectionViews(std::vector<jtag::SampleFrame>{});
        WaveformView::requestResetView();
        setStatusMessage("Capturing...");
    } else {
        setStatusMessage("Capture error: " + capture_engine_->lastError());
    }
}

void AppWindow::onStopCapture() {
    // Daemon path.
    if (!daemon_capture_job_id_.empty()) {
        try {
            gui_client_->captureStop(daemon_capture_job_id_);
        } catch (...) {}
        // refreshFromCapture() will notice the job_id and fetch final samples.
        capturing_ = false;
        refreshFromCapture();
        return;
    }
    // Direct path.
    if (!capture_engine_) return;
    capture_engine_->stop();
    capturing_ = false;
    refreshFromCapture();  // final update
    char buf[128];
    snprintf(buf, sizeof(buf), "Stopped. %zu samples captured.",
             capture_engine_->sampleCount());
    setStatusMessage(buf);
}

void AppWindow::onSingleCapture() {
    // Daemon path.
    if (gui_client_ && gui_client_->isConnected() && !capture_engine_) {
        if (bsdl_device_index_ < 0) return;
        if (capturing_) return;
        try {
            auto result = gui_client_->captureStart(bsdl_device_index_, 10000,
                                                    1000, "single");
            if (result.contains("job_id")) {
                daemon_capture_job_id_ = result["job_id"].get<std::string>();
                capturing_ = true;
                extest_outputs_active_ = false;
                cached_samples_.clear();
                last_refresh_ = std::chrono::steady_clock::now();
                syncSelectionViews(std::vector<jtag::SampleFrame>{});
                WaveformView::requestResetView();
                setStatusMessage("Single capture via daemon...");
            } else {
                setStatusMessage("Capture error: no job_id returned");
            }
        } catch (const std::exception& e) {
            setStatusMessage(std::string("Capture error: ") + e.what());
        }
        return;
    }
    // Direct path.
    if (!capture_engine_) return;
    if (capture_engine_->state() != jtag::CaptureState::STOPPED)
        capture_engine_->stop();
    capture_engine_->trigger().setMode(jtag::TriggerMode::SINGLE);
    if (capture_engine_->start()) {
        capturing_ = true;
        extest_outputs_active_ = false;
        cached_samples_.clear();
        last_refresh_ = std::chrono::steady_clock::now();
        syncSelectionViews(std::vector<jtag::SampleFrame>{});
        WaveformView::requestResetView();
        setStatusMessage("Single capture...");
    }
}

void AppWindow::onClearWaveforms() {
    if (capturing_) return;
    if (!daemon_capture_job_id_.empty() || capture_engine_) {
        if (capture_engine_) capture_engine_->clearSamples();
    }
    daemon_capture_job_id_.clear();
    cached_samples_.clear();
    syncSelectionViews(std::vector<jtag::SampleFrame>{});
    WaveformView::requestResetView();
    setStatusMessage("Waveform buffer cleared.");
}

void AppWindow::onFitWaveforms() {
    if (SignalPanel::selectedSignals().empty()) {
        setStatusMessage("No selected waveforms to fit.");
        return;
    }
    const bool has_data = !cached_samples_.empty() ||
        (capture_engine_ && capture_engine_->sampleCount() > 0);
    if (!has_data) {
        setStatusMessage("No waveform data to fit.");
        return;
    }
    WaveformView::requestFit();
    setStatusMessage("Waveform view fitted to buffered range.");
}

void AppWindow::refreshFromCapture() {
    // ── Daemon capture polling path ──────────────────────────────────────────
    if (!daemon_capture_job_id_.empty()) {
        try {
            auto snap = gui_client_->captureGetSamples(daemon_capture_job_id_);
            // snap keys: state, progress, result, error
            const std::string job_state = snap.value("state", "");
            const auto& progress = snap.value("progress", nlohmann::json{});
            const auto& result   = snap["result"];

            if (job_state == "complete" || job_state == "cancelled" ||
                job_state == "failed") {
                // Job is terminal: extract samples from result.
                if (result.is_object() && result.contains("samples")) {
                    cached_samples_ = samplesFromJson(result["samples"]);
                }
                syncSelectionViews(cached_samples_);
                if (capturing_) {
                    capturing_ = false;
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "Capture complete via daemon. %zu samples.",
                             cached_samples_.size());
                    setStatusMessage(buf);
                }
                // Keep job_id so user can export; clear on next start/clear.
            } else if (capturing_) {
                // Job still running: update waveform with partial samples
                // when the daemon includes them in the progress payload.
                if (progress.is_object()) {
                    if (progress.contains("samples") &&
                        progress["samples"].is_array()) {
                        auto partial = samplesFromJson(progress["samples"]);
                        if (!partial.empty()) {
                            cached_samples_ = std::move(partial);
                            syncSelectionViews(cached_samples_);
                        }
                    }
                    if (progress.contains("count")) {
                        static const char* kDots[] = {".", "..", "...", "...."};
                        static int dot_idx = 0;
                        dot_idx = (dot_idx + 1) % 4;
                        char buf[128];
                        snprintf(buf, sizeof(buf),
                                 "Capturing%s  %d samples (daemon)",
                                 kDots[dot_idx],
                                 progress["count"].get<int>());
                        setStatusMessage(buf);
                    }
                }
            }
        } catch (const std::exception& e) {
            // Network error or daemon gone; mark capture as stopped.
            if (capturing_) {
                capturing_ = false;
                daemon_capture_job_id_.clear();
                setStatusMessage(std::string("Daemon capture poll failed: ") +
                                 e.what());
            }
        }
        return;
    }

    // ── Direct capture path ──────────────────────────────────────────────────
    if (!capture_engine_ || !scanner_) return;

    auto samples = capture_engine_->getSamples();
    cached_samples_ = samples;  // update GUI cache (read every render frame)
    if (samples.empty()) return;

    syncSelectionViews(samples);
    if (pin_driver_ && !samples.back().data.raw_bsr.empty()) {
        pin_driver_->loadSnapshot(samples.back().data.raw_bsr);
    }

    // Check capture state
    auto state = capture_engine_->state();
    // SINGLE mode completes after one frame (stays COMPLETE until stopped).
    // NORMAL/FREE_RUN re-arms automatically; only stop when truly STOPPED.
    bool is_done = (state == jtag::CaptureState::STOPPED) ||
                   (state == jtag::CaptureState::COMPLETE &&
                    capture_engine_->trigger().mode() == jtag::TriggerMode::SINGLE);
    if (is_done && capturing_) {
        capture_engine_->stop();  // transition COMPLETE → STOPPED (no-op if already STOPPED)
        capturing_ = false;
        char buf[128];
        snprintf(buf, sizeof(buf), "Capture complete. %zu samples. %.1f Hz.",
                 samples.size(), capture_engine_->effectiveSampleRate());
        setStatusMessage(buf);
    } else if (capturing_ &&
               capture_engine_->trigger().mode() == jtag::TriggerMode::FREE_RUN &&
               !WaveformView::isAutoScroll()) {
        // FREE_RUN without auto-scroll: waveform is frozen in view, so show
        // a live status indicator so the user knows acquisition is still running.
        static const char* kDots[] = { ".", "..", "...", "...." };
        static int dot_idx = 0;
        dot_idx = (dot_idx + 1) % 4;
        char buf[128];
        snprintf(buf, sizeof(buf), "Capturing%s  %zu samples  %.1f Hz",
                 kDots[dot_idx], samples.size(),
                 capture_engine_->effectiveSampleRate());
        setStatusMessage(buf);
    }
}

void AppWindow::onExportVcd() {
    const auto samples = capture_engine_ ? capture_engine_->getSamples()
                                         : cached_samples_;
    if (samples.empty()) {
        setStatusMessage("No data to export.");
        return;
    }

    std::string path = saveFileDialog("Export VCD",
        "VCD Files (*.vcd)\0*.vcd\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;

    auto selected = SignalPanel::selectedSignals();
    std::string err = VcdExport::exportVcd(path, samples, selected);
    if (err.empty()) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Exported %zu samples to VCD.", samples.size());
        setStatusMessage(buf);
    } else {
        setStatusMessage("Export error: " + err);
    }
}

void AppWindow::onExportCsv() {
    const auto samples = capture_engine_ ? capture_engine_->getSamples()
                                         : cached_samples_;
    if (samples.empty()) {
        setStatusMessage("No data to export.");
        return;
    }

    std::string path = saveFileDialog("Export CSV",
        "CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;

    auto selected = SignalPanel::selectedSignals();
    std::string err = VcdExport::exportCsv(path, samples, selected);
    if (err.empty()) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Exported %zu samples to CSV.", samples.size());
        setStatusMessage(buf);
    } else {
        setStatusMessage("Export error: " + err);
    }
}

bool AppWindow::loadScriptFile(const std::string& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Could not open file.";
        return false;
    }

    std::string contents((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    contents = normalizeLineEndings(contents);
    if (contents.size() >= script_buffer_.size()) {
        error = "File is too large for the editor buffer.";
        return false;
    }

    std::fill(script_buffer_.begin(), script_buffer_.end(), '\0');
    std::copy(contents.begin(), contents.end(), script_buffer_.begin());
    script_path_ = path;
    script_output_.clear();
    return true;
}

bool AppWindow::saveScriptFile(const std::string& path, std::string& error) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "Could not open file for writing.";
        return false;
    }

    file.write(script_buffer_.data(),
               static_cast<std::streamsize>(std::strlen(script_buffer_.data())));
    if (!file) {
        error = "Write failed.";
        return false;
    }
    return true;
}

void AppWindow::onLoadScript() {
    const std::string path = openFileDialog(
        "Load Script",
        "Script Files (*.jts;*.txt)\0*.jts;*.txt\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;

    std::string error;
    if (loadScriptFile(path, error)) {
        setStatusMessage("Script loaded: " + path);
    } else {
        setStatusMessage("Script load failed: " + error);
    }
}

void AppWindow::onSaveScript(bool save_as) {
    std::string path = script_path_;
    if (save_as || path.empty()) {
        path = saveFileDialog(
            "Save Script",
            "Script Files (*.jts;*.txt)\0*.jts;*.txt\0All Files (*.*)\0*.*\0");
        if (path.empty()) return;

        const size_t dot_pos = path.find_last_of('.');
        const size_t slash_pos = path.find_last_of("\\/");
        if (dot_pos == std::string::npos ||
            (slash_pos != std::string::npos && dot_pos < slash_pos)) {
            path += ".jts";
        }
    }

    std::string error;
    if (saveScriptFile(path, error)) {
        script_path_ = path;
        setStatusMessage("Script saved: " + path);
    } else {
        setStatusMessage("Script save failed: " + error);
    }
}

void AppWindow::onLoadConfig() {
    std::string path = openFileDialog("Load Config",
        "JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;

    config_ = AppConfig::load(path);

    // Apply to device dialog
    DeviceConfig dc;
    dc.vendor_id         = config_.vendor_id;
    dc.product_id        = config_.product_id;
    dc.serial            = config_.serial;
    dc.interface_channel = config_.interface_channel;
    dc.clock_freq_hz     = config_.clock_freq_hz;
    DeviceDialog::setConfig(dc);

    // Restore pin selection if already connected with the same BSDL
    if (!config_.selected_pins.empty())
        SignalPanel::setSelectedSignals(config_.selected_pins);

    // Restore bus definitions
    SignalPanel::setBuses(config_.buses);

    // Restore ILA signal lane definitions (no-op if empty)
    ila_panel_.importSignalConfigs(config_.ila_signals);
    ila_panel_.setOrMode(config_.ila_or_mode);

    // Restore XDC aliases
    if (!config_.xdc_path.empty()) {
        onLoadXdc(config_.xdc_path);
    } else {
        SignalPanel::setXdcAliases({});
        WaveformView::setSignalAliases({});
        HexPanel::setXdcAliases({});
    }

    setStatusMessage("Config loaded: " + path);
}

void AppWindow::onLoadXdc(const std::string& path) {
    // Parse XDC: package_pin_designator -> user_port (e.g. "M14" -> "led_out[0]")
    const auto pkg_to_port = jtag::xdc::parseXdc(path);

    // Compose with BSDL package_pin_map to get signal_name -> user_label.
    // BSDL boundary cell pin names are IO buffer names, not package pin designators,
    // so we bridge via the PIN_MAP attribute.
    jtag::xdc::PinAliasMap signal_to_label;
    if (chain_ && bsdl_device_index_ >= 0 &&
        bsdl_device_index_ < static_cast<int>(chain_->devices().size()) &&
        chain_->devices()[bsdl_device_index_].bsdl) {
        const auto& pkg_pin_map =
            chain_->devices()[bsdl_device_index_].bsdl->package_pin_map;
        for (const auto& [signal, pkg_pin] : pkg_pin_map) {
            auto it = pkg_to_port.find(pkg_pin);
            if (it != pkg_to_port.end()) {
                signal_to_label[signal] = it->second;
            }
        }
    }

    // Fallback: Xilinx 7-series / Zynq BSDL names user IO signals as
    // "IO_<package_pin>" (e.g. signal "IO_M14" on package pin M14).
    // Add these heuristic entries for any pkg_pin not already resolved above.
    // emplace() is a no-op when the key already exists, so PIN_MAP-based
    // results take precedence.
    for (const auto& [pkg_pin, user_port] : pkg_to_port) {
        signal_to_label.emplace("IO_" + pkg_pin, user_port);
    }

    SignalPanel::setXdcAliases(signal_to_label);
    WaveformView::setSignalAliases(signal_to_label);
    HexPanel::setXdcAliases(signal_to_label);
    config_.xdc_path = path;
    config_.save("cfg.json");
    setStatusMessage("XDC loaded: " + path);
}

void AppWindow::onSaveConfig() {
    std::string path = saveFileDialog("Save Config",
        "JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;

    // Ensure .json extension
    if (path.size() < 5 || path.substr(path.size() - 5) != ".json")
        path += ".json";

    config_.selected_pins = SignalPanel::selectedSignals();
    config_.buses         = SignalPanel::buses();
    config_.ila_signals   = ila_panel_.exportSignalConfigs();
    config_.ila_or_mode   = ila_panel_.orMode();
    if (config_.save(path)) {
        setStatusMessage("Config saved: " + path);
    } else {
        setStatusMessage("Config save failed: " + path);
    }
}

// ── Test Suite ─────────────────────────────────────────────────────

void AppWindow::onLoadSuite() {
    const std::string path = openFileDialog(
        "Load Test Suite",
        "Suite Files (*.suite)\0*.suite\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;

    std::string error;
    const auto paths = jtag::script::TestSuiteRunner::parseSuiteFile(path, error);
    if (!error.empty()) {
        setStatusMessage("Suite load error: " + error);
        return;
    }
    suite_paths_ = paths;
    suite_path_  = path;
    suite_has_result_ = false;
    suite_output_.clear();
    setStatusMessage("Suite loaded: " + std::to_string(paths.size()) +
                     " test(s) from " + path);
}

void AppWindow::onRunSuite() {
    if (suite_paths_.empty() || !scanner_ || !pin_driver_ || capturing_) return;

    GuiScriptHost host(*scanner_, *pin_driver_, extest_outputs_active_);
    suite_result_ = jtag::script::TestSuiteRunner::run(suite_paths_, host);
    suite_has_result_ = true;

    const std::string report =
        jtag::script::TestSuiteRunner::formatReport(suite_result_);
    suite_output_ = report;
    if (suite_output_.size() > kSuiteOutputMax) {
        suite_output_.resize(kSuiteOutputMax);
    }

    setStatusMessage("Suite complete: " +
                     std::to_string(suite_result_.pass_count) + " passed, " +
                     std::to_string(suite_result_.fail_count) + " failed.");
}

void AppWindow::onExportSuiteReport() {
    if (!suite_has_result_) return;
    const std::string path = saveFileDialog(
        "Export Suite Report",
        "Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;
    std::ofstream f(path);
    if (f.is_open()) {
        f << jtag::script::TestSuiteRunner::formatReport(suite_result_);
        setStatusMessage("Report saved: " + path);
    } else {
        setStatusMessage("Failed to write report: " + path);
    }
}

void AppWindow::drawInterconnectPanel() {
    // Build raw pointer lists from chain_scanners_ / chain_drivers_
    std::vector<jtag::Scanner*> scanners;
    std::vector<jtag::PinDriver*> drivers;
    for (auto& s : chain_scanners_) scanners.push_back(s.get());
    for (auto& d : chain_drivers_)  drivers.push_back(d.get());
    InterconnectPanel::draw(scanners, drivers, connected_);
}

void AppWindow::initializeDockLayout(unsigned int dockspace_id) {
    const bool force_reset = reset_layout_requested_;
    if (force_reset) {
        reset_layout_requested_ = false;
        dock_layout_initialized_ = false;
    }
    if (dock_layout_initialized_) return;
    dock_layout_initialized_ = true;

    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id);
    ImGuiWindowSettings* log_settings =
        ImGui::FindWindowSettingsByID(ImHashStr("Debug Log"));
    const bool log_has_saved_dock =
        log_settings != nullptr && log_settings->DockId != 0;

    const bool has_existing_layout =
        root != nullptr && (root->ChildNodes[0] != nullptr ||
                            root->ChildNodes[1] != nullptr ||
                            root->Windows.Size > 0);

    if (has_existing_layout && !force_reset) {
        if (!log_has_saved_dock) {
            ImGuiID dock_main = dockspace_id;
            ImGuiID dock_log = ImGui::DockBuilderSplitNode(
                dock_main, ImGuiDir_Down, 0.22f, nullptr, &dock_main);
            ImGui::DockBuilderDockWindow("Debug Log", dock_log);
            ImGui::DockBuilderFinish(dockspace_id);
        }
        return;
    }

    // Default layout (also used by View > Reset Layout):
    //  ┌──────────┬────────────────────────────────┬──────────┐
    //  │ Signals  │  Waveforms / ILA / Bus Values  │   Pin    │
    //  │          │  (tabbed)                      │  Control │
    //  │          ├────────────────────────────────┤  Script  │
    //  │          │  Protocol Analyzer             │  Suite   │
    //  │          │                                │ (right)  │
    //  ├──────────┴────────────────────────────────┴──────────┤
    //  │                    Debug Log                          │
    //  └───────────────────────────────────────────────────────┘
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID dock_main = dockspace_id;
    ImGuiID dock_log = ImGui::DockBuilderSplitNode(
        dock_main, ImGuiDir_Down, 0.20f, nullptr, &dock_main);
    ImGuiID dock_signals = ImGui::DockBuilderSplitNode(
        dock_main, ImGuiDir_Left, 0.18f, nullptr, &dock_main);
    ImGuiID dock_tools = ImGui::DockBuilderSplitNode(
        dock_main, ImGuiDir_Right, 0.24f, nullptr, &dock_main);
    ImGuiID dock_inspectors = ImGui::DockBuilderSplitNode(
        dock_main, ImGuiDir_Down, 0.35f, nullptr, &dock_main);

    // Left: signal selector
    ImGui::DockBuilderDockWindow("Signals", dock_signals);
    // Center top (tabbed): primary signal views
    ImGui::DockBuilderDockWindow("Waveforms", dock_main);
    ImGui::DockBuilderDockWindow("Internal Logic Analyzer", dock_main);
    ImGui::DockBuilderDockWindow("Bus Values", dock_main);
    // Center bottom: protocol inspector
    ImGui::DockBuilderDockWindow("Protocol Analyzer", dock_inspectors);
    // Right (tabbed): control & scripting tools
    ImGui::DockBuilderDockWindow("Pin Control", dock_tools);
    ImGui::DockBuilderDockWindow("Script Runner", dock_tools);
    ImGui::DockBuilderDockWindow("Interconnect Test", dock_tools);
    // Bottom: log
    ImGui::DockBuilderDockWindow("Debug Log", dock_log);
    ImGui::DockBuilderFinish(dockspace_id);
}

void AppWindow::setStatusMessage(const std::string& message) {
    status_text_ = firstLineOf(message);
    DebugLogPanel::append(message);
}

// ── Rendering ───────────────────────────────────────────────────────

void AppWindow::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void AppWindow::endFrame() {
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window_, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);
}

void AppWindow::buildDockspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DockSpace", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);
    initializeDockLayout(dockspace_id);

    buildMenuBar();

    ImGui::End();
}

// ── Help window ─────────────────────────────────────────────────────

void AppWindow::drawHelpWindow() {
    static const char* kTopics[] = {
        "Quick Start",
        "Waveform Capture",
        "Script Runner",
        "Test Suite",
        "Interconnect Test",
        "PL Programming",
    };
    static const char* kContent[] = {
        // 0: Quick Start
        "QUICK START\n"
        "-----------\n"
        "1. Connect\n"
        "   Device > Connect... -> select FTDI device and interface, then OK.\n"
        "\n"
        "2. Load BSDL\n"
        "   File > Open BSDL File... -> select the .bsd / .bsdl file that\n"
        "   matches the FPGA on your board.  The Signals panel is populated\n"
        "   from the BSDL pin list.\n"
        "\n"
        "3. Select signals\n"
        "   In the Signals panel, check the pins you want to monitor.\n"
        "\n"
        "4. Capture\n"
        "   Capture > Run (F5) to start continuous sampling.\n"
        "   The waveform panel updates ~20 times per second.\n"
        "   Capture > Stop (F6) to halt.  Capture > Single (F7) for one shot.\n"
        "\n"
        "5. Drive outputs (EXTEST)\n"
        "   Use the Pin Control panel to set pin directions and values,\n"
        "   then click Apply to write them via EXTEST instruction.\n",

        // 1: Waveform Capture
        "WAVEFORM CAPTURE\n"
        "----------------\n"
        "Capture uses the JTAG SAMPLE/PRELOAD instruction to read all\n"
        "boundary-scan cells from the FPGA without disturbing I/O.\n"
        "\n"
        "Keyboard shortcuts:\n"
        "  F5  Run (continuous)\n"
        "  F6  Stop\n"
        "  F7  Single shot\n"
        "  F8  Open Trigger Setup dialog\n"
        "\n"
        "Trigger Setup (Capture > Trigger Setup... or F8):\n"
        "  Choose a pin, a polarity (rising / falling / high / low), and\n"
        "  an optional pre-trigger depth.  The engine captures until the\n"
        "  trigger condition fires, then stops.\n"
        "\n"
        "Waveform view:\n"
        "  Scroll wheel       Zoom time axis\n"
        "  Click + drag       Pan\n"
        "  Right-click plot   Context menu (fit, reset zoom)\n"
        "\n"
        "Export:\n"
        "  File > Export VCD...  Value Change Dump (for GTKWave etc.)\n"
        "  File > Export CSV...  Comma-separated values\n",

        // 2: Script Runner
        "SCRIPT RUNNER\n"
        "-------------\n"
        "The Script Runner panel lets you write and run small automation\n"
        "scripts against the live JTAG chain.\n"
        "\n"
        "Available commands:\n"
        "  sample           Issue SAMPLE/PRELOAD; read all boundary cells.\n"
        "  read <pin>       Print the current sampled state of <pin>.\n"
        "  set <pin> <0|1>  Stage a drive value (requires apply to take effect).\n"
        "  highz <pin>      Stage the pin as high-impedance.\n"
        "  apply            Push staged values via EXTEST instruction.\n"
        "  expect <pin> <0|1>  Assert the sampled value; fail if mismatch.\n"
        "  sleep <ms>       Wait the specified number of milliseconds.\n"
        "  reset_safe       Navigate the TAP to Test-Logic-Reset safely.\n"
        "\n"
        "Tip: use 'sample' before 'read' or 'expect' to refresh the scan.\n"
        "\n"
        "File operations:\n"
        "  File > Load Script...   Open a .jscript / .txt file.\n"
        "  File > Save Script      Save to current path.\n"
        "  File > Save Script As...  Save to a new path.\n",

        // 3: Test Suite
        "TEST SUITE\n"
        "----------\n"
        "A test suite is a plain-text .suite file that lists script files\n"
        "to run in sequence.  Use it for board-level regression testing.\n"
        "\n"
        ".suite file format:\n"
        "  # Lines beginning with # are comments and are ignored.\n"
        "  # Each non-empty line is a path to a script file.\n"
        "  # Relative paths are resolved from the .suite file's directory.\n"
        "  tests/power_on_check.jscript\n"
        "  tests/io_loopback.jscript\n"
        "\n"
        "Workflow:\n"
        "  1. Test > Load Suite (.suite)...  Open the .suite file.\n"
        "  2. Test > Run Suite               Run all listed scripts.\n"
        "  3. Review the results table in the Script Runner panel:\n"
        "       - PASS (green)  All expect commands succeeded.\n"
        "       - FAIL (red)    At least one expect failed; line number shown.\n"
        "       - Expects column shows passed/total count.\n"
        "  4. Test > Export Suite Report...  Save a text summary.\n",

        // 4: Interconnect Test
        "INTERCONNECT TEST\n"
        "-----------------\n"
        "Tests board-level net connectivity between FPGA pins using EXTEST.\n"
        "Each net is driven 0 then 1; all receivers are sampled each time.\n"
        "\n"
        ".ict file format (whitespace-separated columns, # comments):\n"
        "  net_name  driver_dev:DRIVER_PIN  recv_dev:PIN_A  recv_dev:PIN_B ...\n"
        "  Example:\n"
        "    CLK_NET  0:GCLK_P   1:CLK_IN_P   1:CLK_IN_N\n"
        "  dev index = position in JTAG chain (0 = TDO-closest).\n"
        "  Pin names must match the BSDL cell names for that device.\n"
        "  Minimum: driver + at least one receiver.\n"
        "\n"
        "Workflow:\n"
        "  1. Connect and load BSDL files for all devices in the chain.\n"
        "  2. Test > Interconnect Test...  to open the panel.\n"
        "  3. Click 'Load .ict...'  to open the netlist file.\n"
        "  4. Review the net preview table (Net / Driver / Receivers).\n"
        "  5. Click 'Run Test' to execute.\n"
        "     Results table shows per-net PASS (green) / FAIL (red);\n"
        "     expand a row to see the drive-0 and drive-1 observed values.\n"
        "  6. Click 'Export Report...' to save a text report.\n",

        // 5: PL Programming
        "PL PROGRAMMING\n"
        "--------------\n"
        "Programs a Xilinx FPGA PL (Programmable Logic) via JTAG.\n"
        "Supported file formats: .bit (Xilinx bitstream), .bin (raw binary).\n"
        "\n"
        "Requirements:\n"
        "  - Device must be connected.\n"
        "  - Capture must not be running.\n"
        "  - Device 0 in the JTAG chain is assumed to be the PL TAP\n"
        "    (TDO-closest, following Xilinx UG470 chain ordering).\n"
        "\n"
        "Workflow:\n"
        "  Tools > Program Bitstream...\n"
        "  -> Select the .bit or .bin file in the file dialog.\n"
        "  -> A progress dialog shows KB sent / total KB.\n"
        "  -> On success, 'DONE asserted' confirms the PL has started.\n"
        "  -> On failure, the error message is shown in the dialog and\n"
        "     in the status bar.\n"
        "\n"
        "Note: partial reconfiguration bitstreams are not yet supported.\n",
    };

    static_assert(sizeof(kTopics) / sizeof(kTopics[0]) ==
                  sizeof(kContent) / sizeof(kContent[0]),
                  "topic/content count mismatch");
    constexpr int kNumTopics = static_cast<int>(sizeof(kTopics) / sizeof(kTopics[0]));

    ImGui::SetNextWindowSize(ImVec2(780.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Help", &show_help_)) {
        ImGui::End();
        return;
    }

    // Left pane: topic list
    ImGui::BeginChild("help_topics", ImVec2(170.0f, 0.0f), true);
    for (int i = 0; i < kNumTopics; ++i) {
        if (ImGui::Selectable(kTopics[i], help_topic_ == i)) {
            help_topic_ = i;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right pane: content
    ImGui::BeginChild("help_content", ImVec2(0.0f, 0.0f), true);
    if (help_topic_ >= 0 && help_topic_ < kNumTopics) {
        ImGui::TextUnformatted(kContent[help_topic_]);
    }
    ImGui::EndChild();

    ImGui::End();
}

// ── Menu bar ─────────────────────────────────────────────────────────

void AppWindow::buildMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open BSDL File...", "Ctrl+O", false, connected_)) {
                onOpenBsdl();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Load Script...")) {
                onLoadScript();
            }
            if (ImGui::MenuItem("Save Script")) {
                onSaveScript(false);
            }
            if (ImGui::MenuItem("Save Script As...")) {
                onSaveScript(true);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Load Config...")) {
                onLoadConfig();
            }
            if (ImGui::MenuItem("Load XDC...", nullptr, false, bsdl_device_index_ >= 0)) {
                std::string xdc_path = openFileDialog("Load XDC", "XDC Files\0*.xdc\0All Files\0*.*\0");
                if (!xdc_path.empty()) onLoadXdc(xdc_path);
            }
            if (ImGui::MenuItem("Save Config", "Ctrl+S")) {
                onSaveConfig();
            }
            ImGui::Separator();
            {
                const bool has_data = capture_engine_ != nullptr ||
                                      !cached_samples_.empty();
                if (ImGui::MenuItem("Export VCD...", nullptr, false, has_data)) {
                    onExportVcd();
                }
                if (ImGui::MenuItem("Export CSV...", nullptr, false, has_data)) {
                    onExportCsv();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Device")) {
            if (ImGui::MenuItem("Connect...", nullptr, false,
                                !connected_ && !daemon_hw_open_.load(std::memory_order_acquire))) {
                show_device_dialog_ = true;
            }
        if (ImGui::MenuItem("Disconnect", nullptr, false,
                             connected_ && !program_pl_running_.load())) {
                onDisconnect();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Capture")) {
            const bool daemon_cap =
                gui_client_ && gui_client_->isConnected() && bsdl_device_index_ >= 0;
            bool can_capture = (capture_engine_ != nullptr || daemon_cap) && !capturing_;
            if (ImGui::MenuItem("Run", "F5", false, can_capture)) {
                onStartCapture();
            }
            if (ImGui::MenuItem("Stop", "F6", false, capturing_)) {
                onStopCapture();
            }
            if (ImGui::MenuItem("Single", "F7", false, can_capture)) {
                onSingleCapture();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Waveforms", nullptr, false, can_capture)) {
                onClearWaveforms();
            }
            if (ImGui::MenuItem("Fit Waveforms", nullptr, false, can_capture)) {
                onFitWaveforms();
            }
            if (can_capture) {
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Trigger Setup...", "F8", false,
                                capture_engine_ != nullptr)) {
                show_trigger_dialog_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Internal Logic Analyzer", nullptr,
                                ila_panel_.isVisible())) {
                ila_panel_.setVisible(!ila_panel_.isVisible());
            }
            if (ImGui::MenuItem("Protocol Analyzer", nullptr,
                                ProtocolPanel::isVisible())) {
                ProtocolPanel::setVisible(!ProtocolPanel::isVisible());
            }
            if (ImGui::MenuItem("Interconnect Test", nullptr,
                                InterconnectPanel::isVisible())) {
                InterconnectPanel::setVisible(!InterconnectPanel::isVisible());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) {
                reset_layout_requested_ = true;
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Daemon")) {
                // -- Daemon section --
                const DaemonStatus ds = daemon_ctrl_->status();
                const char* state_label =
                    ds.state == DaemonState::kOff      ? "Daemon: Off" :
                    ds.state == DaemonState::kStarting ? "Daemon: Starting..." :
                    ds.state == DaemonState::kRunning  ? "Daemon: Running" :
                    ds.state == DaemonState::kStopping ? "Daemon: Stopping..." :
                                                         "Daemon: Error";
                ImGui::TextDisabled("%s", state_label);
                if (!ds.error_message.empty()) {
                    ImGui::TextDisabled("  %s", ds.error_message.c_str());
                }
                ImGui::TextDisabled("MCP: use CLI --mcp-port <port>");
                const bool daemon_off =
                    (ds.state == DaemonState::kOff || ds.state == DaemonState::kError);
                const bool daemon_active =
                    (ds.state == DaemonState::kStarting ||
                     ds.state == DaemonState::kRunning);
                if (ImGui::MenuItem("Start Daemon", nullptr, false, daemon_off)) {
                    onDaemonStart();
                }
                if (ImGui::MenuItem("Stop Daemon", nullptr, false, daemon_active)) {
                    onDaemonStop();
                }
                // Phase 3: Connect GUI RPC client when daemon is running with gui_port.
                const bool can_connect_rpc =
                    (ds.state == DaemonState::kRunning && ds.gui_port != 0 &&
                     !daemon_connect_running_.load(std::memory_order_relaxed));
                if (ImGui::MenuItem("Connect (via Daemon)", nullptr, false, can_connect_rpc)) {
                    onDaemonConnect();
                }
                const bool can_detect =
                    gui_client_->isConnected() &&
                    !daemon_connect_running_.load(std::memory_order_relaxed);
                if (ImGui::MenuItem("Detect Devices (via Daemon)", nullptr, false, can_detect)) {
                    onDaemonDetectDevices();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Test")) {
            if (ImGui::MenuItem("Load Suite (.suite)...")) {
                onLoadSuite();
            }
            if (ImGui::MenuItem("Run Suite", nullptr, false,
                                !suite_paths_.empty() && scanner_ &&
                                    pin_driver_ && !capturing_)) {
                onRunSuite();
            }
            if (ImGui::MenuItem("Export Suite Report...", nullptr, false,
                                suite_has_result_)) {
                onExportSuiteReport();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Interconnect Test...", nullptr,
                                InterconnectPanel::isVisible())) {
                InterconnectPanel::setVisible(!InterconnectPanel::isVisible());
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            const bool can_program = connected_ && !capturing_ &&
                                     !program_pl_running_.load();
            if (ImGui::MenuItem("Program  Bitstream...", nullptr, false, can_program)) {
                onProgramPl();
            }
            const bool can_flash = connected_ && !capturing_ &&
                                   !program_pl_running_.load() &&
                                   !program_flash_running_.load();
            if (ImGui::MenuItem("Program  Flash (SPI ROM)...", nullptr, false, can_flash)) {
                onProgramFlash();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Protocol Analyzer...", nullptr,
                                ProtocolPanel::isVisible())) {
                ProtocolPanel::setVisible(!ProtocolPanel::isVisible());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Internal Logic Analyzer...", nullptr,
                                ila_panel_.isVisible())) {
                ila_panel_.setVisible(!ila_panel_.isVisible());
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Quick Start",        nullptr, false)) { show_help_ = true; help_topic_ = 0; }
            if (ImGui::MenuItem("Waveform Capture",   nullptr, false)) { show_help_ = true; help_topic_ = 1; }
            if (ImGui::MenuItem("Script Runner",      nullptr, false)) { show_help_ = true; help_topic_ = 2; }
            if (ImGui::MenuItem("Test Suite",         nullptr, false)) { show_help_ = true; help_topic_ = 3; }
            if (ImGui::MenuItem("Interconnect Test",  nullptr, false)) { show_help_ = true; help_topic_ = 4; }
            if (ImGui::MenuItem("PL Programming",     nullptr, false)) { show_help_ = true; help_topic_ = 5; }
            ImGui::Separator();
            if (ImGui::MenuItem("About")) {
                show_about_ = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

// ── PL Programming ──────────────────────────────────────────────────

void AppWindow::onProgramPl() {
    if (!connected_) return;
    if (gui_client_ && gui_client_->isConnected() && !chain_) {
        setStatusMessage("PL programming not yet available in daemon mode.");
        return;
    }
    if (!chain_) return;
    if (program_pl_running_.load()) return;

    std::string path = openFileDialog(
        "Program  Bitstream",
        "Bitstream Files (*.bit;*.bin)\0*.bit;*.bin\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;

    // Join any previously finished thread
    if (program_pl_thread_.joinable()) program_pl_thread_.join();

    program_pl_path_ = path;
    program_pl_bytes_.store(0);
    program_pl_total_.store(0);
    program_pl_success_.store(false);
    {
        std::lock_guard<std::mutex> lk(program_pl_mutex_);
        program_pl_error_.clear();
    }
    program_pl_running_.store(true);
    program_pl_popup_requested_ = true;

    setStatusMessage("Programming PL: " + path);

    program_pl_thread_ = std::thread([this, path]() {
        // Device 0 = PL TAP (TDO-closest in Zynq JTAG chain, UG470 ordering)
        jtag::PlConfig pl(*chain_, 0);
        bool ok = pl.program(path, [this](size_t sent, size_t total) {
            program_pl_bytes_.store(sent);
            program_pl_total_.store(total);
        });
        std::string err;
        if (!ok) err = pl.lastError();
        {
            std::lock_guard<std::mutex> lk(program_pl_mutex_);
            program_pl_error_ = err;
        }
        program_pl_success_.store(ok);
        program_pl_running_.store(false);
    });
}

void AppWindow::drawProgramPlModal() {
    if (program_pl_popup_requested_) {
        ImGui::OpenPopup("Programming PL");
        program_pl_popup_requested_ = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Programming PL", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        // Show short filename
        const std::string& path = program_pl_path_;
        const size_t slash = path.find_last_of("/\\");
        const std::string fname = (slash != std::string::npos)
                                      ? path.substr(slash + 1) : path;
        ImGui::TextUnformatted(fname.c_str());
        ImGui::Separator();

        const bool running = program_pl_running_.load();
        if (running) {
            const size_t sent  = program_pl_bytes_.load();
            const size_t total = program_pl_total_.load();
            const float frac   = (total > 0)
                ? static_cast<float>(sent) / static_cast<float>(total) : 0.0f;
            char overlay[64];
            if (total > 0) {
                snprintf(overlay, sizeof(overlay), "%zu / %zu KB",
                         sent / 1024, total / 1024);
            } else {
                snprintf(overlay, sizeof(overlay), "Loading...");
            }
            ImGui::ProgressBar(frac, ImVec2(-1.0f, 22.0f), overlay);
            ImGui::TextDisabled("Programming in progress — please wait...");
        } else {
            const bool success = program_pl_success_.load();
            if (success) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f),
                                   "SUCCESS: DONE asserted. PL is running.");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "FAILED");
                std::string err;
                {
                    std::lock_guard<std::mutex> lk(program_pl_mutex_);
                    err = program_pl_error_;
                }
                if (!err.empty()) {
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", err.c_str());
                }
            }
            ImGui::Separator();
            if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
                if (program_pl_thread_.joinable()) program_pl_thread_.join();
                if (success) {
                    setStatusMessage("PL programmed successfully.");
                } else {
                    std::lock_guard<std::mutex> lk(program_pl_mutex_);
                    setStatusMessage("PL programming failed: " + program_pl_error_);
                }
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

// ── Flash Programming ───────────────────────────────────────────────

void AppWindow::onProgramFlash() {
    if (!connected_ || !chain_) return;
    if (program_flash_running_.load()) return;

    std::string bridge = openFileDialog(
        "Select BSCAN SPI bridge bitstream",
        "Bitstream Files (*.bit)\0*.bit\0All Files (*.*)\0*.*\0");
    if (bridge.empty()) return;

    std::string bin = openFileDialog(
        "Select flash image (.bin or .mcs)",
        "Flash Image (*.bin;*.mcs;*.hex)\0*.bin;*.mcs;*.hex\0All Files (*.*)\0*.*\0");
    if (bin.empty()) return;

    if (program_flash_thread_.joinable()) program_flash_thread_.join();

    program_flash_bin_path_ = bin;
    program_flash_bridge_path_ = bridge;
    program_flash_phase_.store(0);
    program_flash_done_.store(0);
    program_flash_total_.store(0);
    program_flash_success_.store(false);
    {
        std::lock_guard<std::mutex> lk(program_flash_mutex_);
        program_flash_error_.clear();
    }
    program_flash_running_.store(true);
    program_flash_popup_requested_ = true;

    setStatusMessage("Programming Flash: " + bin);

    program_flash_thread_ = std::thread([this, bin, bridge]() {
        // Device 0 = PL TAP (same ordering as onProgramPl).
        jtag::flash::FlashProgrammer programmer(*chain_, 0, bridge);
        bool ok = programmer.program(
            bin,
            [this](jtag::flash::FlashPhase phase,
                   std::size_t done, std::size_t total) {
                program_flash_phase_.store(static_cast<int>(phase));
                program_flash_done_.store(done);
                program_flash_total_.store(total);
            });
        std::string err;
        if (!ok) err = programmer.lastError();
        {
            std::lock_guard<std::mutex> lk(program_flash_mutex_);
            program_flash_error_ = err;
        }
        program_flash_success_.store(ok);
        program_flash_running_.store(false);
    });
}

void AppWindow::drawProgramFlashModal() {
    if (program_flash_popup_requested_) {
        ImGui::OpenPopup("Programming Flash");
        program_flash_popup_requested_ = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Programming Flash", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string& path = program_flash_bin_path_;
        const size_t slash = path.find_last_of("/\\");
        const std::string fname = (slash != std::string::npos)
                                      ? path.substr(slash + 1) : path;
        ImGui::TextUnformatted(fname.c_str());
        ImGui::Separator();

        const bool running = program_flash_running_.load();
        const auto phase = static_cast<jtag::flash::FlashPhase>(
            program_flash_phase_.load());
        const char* phase_label = "...";
        switch (phase) {
            case jtag::flash::FlashPhase::BRIDGE_LOAD: phase_label = "Loading BSCAN bridge"; break;
            case jtag::flash::FlashPhase::ERASE:       phase_label = "Bulk erase (may take minutes)"; break;
            case jtag::flash::FlashPhase::PROGRAM:     phase_label = "Programming"; break;
            case jtag::flash::FlashPhase::VERIFY:      phase_label = "Verifying"; break;
        }

        if (running) {
            const size_t done  = program_flash_done_.load();
            const size_t total = program_flash_total_.load();
            float frac = 0.0f;
            char overlay[64];
            if (total > 0) {
                frac = static_cast<float>(done) / static_cast<float>(total);
                snprintf(overlay, sizeof(overlay),
                         "%zu / %zu KB", done / 1024, total / 1024);
            } else {
                // Indeterminate phases (BRIDGE_LOAD, ERASE)
                snprintf(overlay, sizeof(overlay), "working...");
            }
            ImGui::TextUnformatted(phase_label);
            ImGui::ProgressBar(frac, ImVec2(-1.0f, 22.0f), overlay);
            ImGui::TextDisabled("Do not disconnect the device.");
        } else {
            const bool success = program_flash_success_.load();
            if (success) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f),
                                   "SUCCESS: Flash image written and verified.");
                ImGui::TextDisabled("Power-cycle the board to boot from SPI.");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "FAILED");
                std::string err;
                {
                    std::lock_guard<std::mutex> lk(program_flash_mutex_);
                    err = program_flash_error_;
                }
                if (!err.empty()) {
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", err.c_str());
                }
            }
            ImGui::Separator();
            if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
                if (program_flash_thread_.joinable()) program_flash_thread_.join();
                if (success) {
                    setStatusMessage("Flash programmed successfully.");
                } else {
                    std::lock_guard<std::mutex> lk(program_flash_mutex_);
                    setStatusMessage("Flash programming failed: " +
                                     program_flash_error_);
                }
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

// ── Daemon process controller actions ────────────────────────────────────────

// Attempt to locate jtag_daemon.exe adjacent to the running executable.
static std::string findDaemonExe() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string dir(buf);
    const auto slash = dir.find_last_of("\\/");
    dir = (slash != std::string::npos) ? dir.substr(0, slash + 1) : "";
    const std::string candidates[] = {
        dir + "jtag_daemon.exe",
        dir + "tools\\jtag_daemon.exe",
        std::string("bazel-bin\\src\\tools\\jtag_daemon.exe"),
    };
    for (const auto& c : candidates) {
        if (GetFileAttributesA(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    }
#endif
    return {};
}

void AppWindow::onDaemonStart() {
    const std::string exe = findDaemonExe();
    if (exe.empty()) {
        DebugLogPanel::append("[daemon] Cannot find jtag_daemon.exe");
        setStatusMessage("Daemon not found.");
        return;
    }
    if (!daemon_ctrl_->start(exe, "cfg.json", /*mcp_port=*/0)) {
        DebugLogPanel::append("[daemon] Failed to start daemon process");
        setStatusMessage("Daemon start failed.");
    } else {
        setStatusMessage("Daemon starting...");
    }
}

void AppWindow::onDaemonStop() {
    if (daemon_ctrl_->status().state == DaemonState::kOff) return;
    if (gui_client_->isConnected()) gui_client_->disconnect();
    // Join poll thread before resetting — disconnect() causes it to exit promptly.
    if (daemon_status_poll_thread_.joinable()) daemon_status_poll_thread_.join();
    daemon_hw_open_.store(false, std::memory_order_release);
    daemon_device_count_.store(0, std::memory_order_release);
    daemon_ctrl_->stop();
    daemon_last_reported_port_ = 0;
    setStatusMessage("Daemon stopped.");
}

void AppWindow::onDaemonConnect() {
    const DaemonStatus ds = daemon_ctrl_->status();
    if (ds.state != DaemonState::kRunning || ds.gui_port == 0) {
        DebugLogPanel::append("[daemon] GUI RPC not available (daemon not running or gui_port=0)");
        return;
    }
    if (daemon_connect_running_.load(std::memory_order_acquire)) {
        DebugLogPanel::append("[daemon] Connect already in progress");
        return;
    }
    if (gui_client_->isConnected()) gui_client_->disconnect();

    const uint16_t gui_port = ds.gui_port;
    daemon_connect_running_.store(true, std::memory_order_release);
    setStatusMessage("Connecting to daemon GUI RPC...");

    if (daemon_connect_thread_.joinable()) daemon_connect_thread_.join();

    daemon_connect_thread_ = std::thread([this, gui_port]() {
        std::string report;

        // Connect.
        const std::string conn_err = gui_client_->connect(gui_port);
        if (!conn_err.empty()) {
            report = "[daemon] GUI RPC connect failed: " + conn_err;
            std::lock_guard<std::mutex> lk(daemon_connect_mutex_);
            daemon_connect_result_ = std::move(report);
            daemon_connect_running_.store(false, std::memory_order_release);
            return;
        }
        report += "[daemon] GUI RPC connected to 127.0.0.1:" +
                  std::to_string(gui_port) + "\n";

        // detect_devices
        try {
            auto devs = gui_client_->detectDevices();
            const int count = devs.value("device_count", 0);
            report += "[daemon] detect_devices: " + std::to_string(count) +
                      " device(s)\n";
            int bscane_idx = -1;
            if (devs.contains("devices") && devs["devices"].is_array()) {
                for (const auto& d : devs["devices"]) {
                    const int pos = d.value("position", -1);
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "[daemon]   [%d] IDCODE=%s ir_len=%d bsdl=%s",
                                  pos,
                                  d.value("idcode", "?").c_str(),
                                  d.value("ir_length", 0),
                                  d.value("bsdl_loaded", false) ? "loaded" : "none");
                    report += std::string(buf) + "\n";
                    // Detect Zynq PL Config TAP: lower 28 bits of IDCODE = 0x3727093.
                    const std::string id_str = d.value("idcode", "0x0");
                    uint32_t idcode = 0;
                    std::sscanf(id_str.c_str(), "0x%x", &idcode); // NOLINT(cert-err34-c)
                    if ((idcode & 0x0FFFFFFFu) == 0x03727093u && bscane_idx < 0)
                        bscane_idx = pos;
                }
            }
            daemon_bscane_idx_.store(bscane_idx, std::memory_order_release);
        } catch (const std::exception& e) {
            report += std::string("[daemon] detect_devices error: ") + e.what() + "\n";
            daemon_bscane_idx_.store(-1, std::memory_order_release);
        }

        std::lock_guard<std::mutex> lk(daemon_connect_mutex_);
        daemon_connect_result_ = std::move(report);
        daemon_connect_running_.store(false, std::memory_order_release);
    });
}

void AppWindow::onDaemonDetectDevices() {
    if (!gui_client_->isConnected()) {
        DebugLogPanel::append("[daemon] Not connected to GUI RPC");
        return;
    }
    if (daemon_connect_running_.load(std::memory_order_acquire)) {
        DebugLogPanel::append("[daemon] Operation already in progress");
        return;
    }
    daemon_connect_running_.store(true, std::memory_order_release);
    setStatusMessage("Detecting devices via daemon...");

    if (daemon_connect_thread_.joinable()) daemon_connect_thread_.join();

    daemon_connect_thread_ = std::thread([this]() {
        std::string report;
        try {
            auto devs = gui_client_->detectDevices();
            const int count = devs.value("device_count", 0);
            report += "[daemon] detect_devices: " + std::to_string(count) +
                      " device(s)\n";
            if (devs.contains("devices") && devs["devices"].is_array()) {
                for (const auto& d : devs["devices"]) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "[daemon]   [%d] IDCODE=%s ir_len=%d bsdl=%s",
                                  d.value("position", -1),
                                  d.value("idcode", "?").c_str(),
                                  d.value("ir_length", 0),
                                  d.value("bsdl_loaded", false) ? "loaded" : "none");
                    report += std::string(buf) + "\n";
                }
            }
            // Force immediate re-poll so status bar updates without waiting 2 s.
            last_daemon_status_poll_ = {};
        } catch (const std::exception& e) {
            report = std::string("[daemon] detect_devices error: ") + e.what();
        }
        {
            std::lock_guard<std::mutex> lk(daemon_connect_mutex_);
            daemon_connect_result_ = std::move(report);
        }
        daemon_connect_running_.store(false, std::memory_order_release);
    });
}

void AppWindow::drainDaemonLog() {
    std::vector<std::string> lines;
    daemon_ctrl_->drainLog(lines);
    for (const auto& ln : lines) {
        DebugLogPanel::append(ln);
    }
    // Update status bar when daemon becomes ready.
    const DaemonStatus ds = daemon_ctrl_->status();
    if (ds.state == DaemonState::kRunning && ds.mcp_port != 0) {
        if (daemon_last_reported_port_ != ds.mcp_port) {
            daemon_last_reported_port_ = ds.mcp_port;
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "Daemon running on 127.0.0.1:%u", ds.mcp_port);
            setStatusMessage(buf);
        }
    }

    // Phase 5: auto-connect GUI RPC when daemon is ready and connect is pending.
    if (pending_daemon_connect_ && ds.state == DaemonState::kRunning &&
        ds.gui_port != 0 &&
        !daemon_connect_running_.load(std::memory_order_acquire)) {
        pending_daemon_connect_ = false;
        onDaemonConnect();
    } else if (pending_daemon_connect_ && ds.state == DaemonState::kError) {
        pending_daemon_connect_ = false;
        setStatusMessage("Daemon failed to start. Cannot connect.");
    }

    // Drain result from background daemon-connect operation (Phase 3 / 5).
    if (!daemon_connect_running_.load(std::memory_order_acquire)) {
        std::string result;
        {
            std::lock_guard<std::mutex> lk(daemon_connect_mutex_);
            result.swap(daemon_connect_result_);
        }
        if (!result.empty()) {
            DebugLogPanel::append(result);
            if (daemon_connect_thread_.joinable()) {
                daemon_connect_thread_.join();
            }
            // Phase 5: set connected_ = true when daemon RPC connect succeeds.
            if (gui_client_ && gui_client_->isConnected() && !connected_) {
                connected_ = true;
                // Wire ILA panel to daemon. Use BSCANE2 if a Zynq PL TAP was
                // found; fall back to ChainIla at device 0 otherwise.
                const int bscane = daemon_bscane_idx_.load(std::memory_order_acquire);
                if (bscane >= 0) {
                    ila_panel_.setDaemonClient(gui_client_.get(), bscane, true);
                } else {
                    ila_panel_.setDaemonClient(gui_client_.get(), 0, false);
                }
            }
        }
    }

    // Periodic daemon hardware ownership poll — every 2 s when GUI RPC is live.
    if (gui_client_ && gui_client_->isConnected() &&
        !daemon_status_poll_running_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_daemon_status_poll_ > std::chrono::seconds(2)) {
            last_daemon_status_poll_ = now;
            daemon_status_poll_running_.store(true, std::memory_order_release);
            if (daemon_status_poll_thread_.joinable()) {
                daemon_status_poll_thread_.join();
            }
            daemon_status_poll_thread_ = std::thread([this]() {
                try {
                    auto j = gui_client_->daemonStatus();
                    daemon_hw_open_.store(
                        j.value("hardware_open", false),
                        std::memory_order_release);
                    daemon_device_count_.store(
                        j.value("device_count", 0),
                        std::memory_order_release);
                } catch (...) {
                    // Connection lost — reset cached state.
                    daemon_hw_open_.store(false, std::memory_order_release);
                    daemon_device_count_.store(0, std::memory_order_release);
                }
                daemon_status_poll_running_.store(false, std::memory_order_release);
            });
        }
    }
}

} // namespace jtag::gui
