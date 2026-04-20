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

#include <algorithm>
#include <cstdio>

#include "app_config.h"
#include "debug_log_panel.h"
#include "device_dialog.h"
#include "hex_panel.h"
#include "signal_panel.h"
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

static void syncSelectionViews(const std::vector<jtag::SampleFrame>& samples) {
    const auto selected = SignalPanel::selectedSignals();
    if (selected.empty()) {
        WaveformView::clearData();
        HexPanel::updateValues(
            samples.empty() ? jtag::ScanResult{} : samples.back().data,
            selected);
        return;
    }

    WaveformView::setData(selected, samples);

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

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    setStatusMessage(status_text_);
}

AppWindow::~AppWindow() {
    // Save config before destroying
    config_.selected_pins = SignalPanel::selectedSignals();
    config_.save("cfg.json");

    onDisconnect();

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
        if (SignalPanel::consumeSelectionChanged()) {
            syncSelectionViews(capture_engine_
                                   ? capture_engine_->getSamples()
                                   : std::vector<jtag::SampleFrame>{});
        }
        if (WaveformView::draw(capture_engine_ != nullptr && !capturing_)) {
            onClearWaveforms();
        }
        HexPanel::draw();
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

// ── Backend actions ─────────────────────────────────────────────────

void AppWindow::onConnect() {
    onDisconnect();

    const auto& cfg = DeviceDialog::config();

    // Persist device settings to config
    config_.vendor_id        = cfg.vendor_id;
    config_.product_id       = cfg.product_id;
    config_.serial           = cfg.serial;
    config_.interface_channel = cfg.interface_channel;
    config_.clock_freq_hz    = cfg.clock_freq_hz;

    ftdi_ = std::make_unique<jtag::FtdiDevice>();
    if (!ftdi_->open(cfg.vendor_id, cfg.product_id, cfg.serial,
                     static_cast<jtag::FtdiInterface>(cfg.interface_channel))) {
        setStatusMessage("Connect failed: " + ftdi_->lastError());
        ftdi_.reset();
        return;
    }

    if (!ftdi_->initMpsse(cfg.clock_freq_hz)) {
        setStatusMessage("MPSSE init failed: " + ftdi_->lastError());
        ftdi_.reset();
        return;
    }

    tap_ = std::make_unique<jtag::TapController>(*ftdi_);
    chain_ = std::make_unique<jtag::JtagChain>(*tap_);

    int count = chain_->detectDevices();
    if (count <= 0) {
        setStatusMessage("No JTAG devices found. " + chain_->lastError());
        chain_.reset();
        tap_.reset();
        ftdi_.reset();
        return;
    }

    connected_ = true;

    // Build device info string
    char buf[256];
    snprintf(buf, sizeof(buf), "Connected: %d device(s) in chain", count);
    std::string status_message = buf;

    // Log IDCODEs
    for (const auto& dev : chain_->devices()) {
        snprintf(buf, sizeof(buf), "  Device %d: IDCODE=0x%08X IR_len=%d",
                 dev.position, dev.idcode, dev.ir_length);
        status_message += std::string("\n") + buf;
    }
    setStatusMessage(status_message);
}

void AppWindow::onDisconnect() {
    const bool had_backend_state = capturing_ || connected_ ||
        capture_engine_ != nullptr || pin_driver_ != nullptr ||
        scanner_ != nullptr || chain_ != nullptr || tap_ != nullptr ||
        (ftdi_ != nullptr && ftdi_->isOpen());

    if (capturing_) {
        onStopCapture();
    }
    capture_engine_.reset();
    pin_driver_.reset();
    scanner_.reset();
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
    WaveformView::clearData();
    HexPanel::clearBuses();
    if (had_backend_state || status_text_ != "Disconnected") {
        setStatusMessage("Disconnected");
    }
}

void AppWindow::onOpenBsdl() {
    if (!connected_ || !chain_) return;

    std::string path = openFileDialog("Open BSDL File",
        "BSDL Files (*.bsdl;*.bsd)\0*.bsdl;*.bsd\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;

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
            n += snprintf(buf + n, sizeof(buf) - n, "BSR[0..7]:");
            for (size_t i = 0; i < 8 && i < result.raw_bsr.size(); i++)
                n += snprintf(buf + n, sizeof(buf) - n, " %02X", result.raw_bsr[i]);
            int nz = 0;
            for (auto b : result.raw_bsr) if (b) nz++;
            n += snprintf(buf + n, sizeof(buf) - n,
                " (%d/%zu non-zero bytes)", nz, result.raw_bsr.size());
        } else {
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
        setStatusMessage("BSDL loaded. " +
            std::to_string(scanner_->getObservablePins().size()) +
            " pins. IDCODE read failed.");
    }
}

void AppWindow::onStartCapture() {
    if (!capture_engine_) return;
    if (capture_engine_->start()) {
        capturing_ = true;
        last_refresh_ = std::chrono::steady_clock::now();
        setStatusMessage("Capturing...");
    } else {
        setStatusMessage("Capture error: " + capture_engine_->lastError());
    }
}

void AppWindow::onStopCapture() {
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
    if (!capture_engine_) return;
    capture_engine_->trigger().setMode(jtag::TriggerMode::SINGLE);
    if (capture_engine_->start()) {
        capturing_ = true;
        last_refresh_ = std::chrono::steady_clock::now();
        setStatusMessage("Single capture...");
    }
}

void AppWindow::onClearWaveforms() {
    if (!capture_engine_ || capturing_) return;

    capture_engine_->clearSamples();
    syncSelectionViews(std::vector<jtag::SampleFrame>{});
    setStatusMessage("Waveform buffer cleared.");
}

void AppWindow::refreshFromCapture() {
    if (!capture_engine_ || !scanner_) return;

    auto samples = capture_engine_->getSamples();
    if (samples.empty()) return;

    syncSelectionViews(samples);

    // Check capture state
    auto state = capture_engine_->state();
    if (state == jtag::CaptureState::COMPLETE ||
        state == jtag::CaptureState::STOPPED) {
        if (capturing_) {
            capturing_ = false;
            char buf[128];
            snprintf(buf, sizeof(buf), "Capture complete. %zu samples. %.1f Hz.",
                     samples.size(), capture_engine_->effectiveSampleRate());
            setStatusMessage(buf);
        }
    }
}

void AppWindow::onExportVcd() {
    if (!capture_engine_) return;
    auto samples = capture_engine_->getSamples();
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
    if (!capture_engine_) return;
    auto samples = capture_engine_->getSamples();
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

    setStatusMessage("Config loaded: " + path);
}

void AppWindow::onSaveConfig() {
    std::string path = saveFileDialog("Save Config",
        "JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0");
    if (path.empty()) return;

    // Ensure .json extension
    if (path.size() < 5 || path.substr(path.size() - 5) != ".json")
        path += ".json";

    config_.selected_pins = SignalPanel::selectedSignals();
    if (config_.save(path)) {
        setStatusMessage("Config saved: " + path);
    } else {
        setStatusMessage("Config save failed: " + path);
    }
}

void AppWindow::initializeDockLayout(unsigned int dockspace_id) {
    if (dock_layout_initialized_) return;
    dock_layout_initialized_ = true;

    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id);
    ImGuiWindowSettings* log_settings =
        ImGui::FindWindowSettingsByID(ImHashStr("Debug Log"));
    const bool log_has_saved_dock =
        log_settings != nullptr && log_settings->DockId != 0;

    if (root != nullptr && (root->ChildNodes[0] != nullptr ||
                            root->ChildNodes[1] != nullptr ||
                            root->Windows.Size > 0)) {
        if (!log_has_saved_dock) {
            ImGuiID dock_main = dockspace_id;
            ImGuiID dock_log = ImGui::DockBuilderSplitNode(
                dock_main, ImGuiDir_Down, 0.22f, nullptr, &dock_main);
            ImGui::DockBuilderDockWindow("Debug Log", dock_log);
            ImGui::DockBuilderFinish(dockspace_id);
        }
        return;
    }

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID dock_top = dockspace_id;
    ImGuiID dock_waveforms = ImGui::DockBuilderSplitNode(
        dock_top, ImGuiDir_Down, 0.38f, nullptr, &dock_top);
    ImGuiID dock_signals = ImGui::DockBuilderSplitNode(
        dock_top, ImGuiDir_Left, 0.20f, nullptr, &dock_top);
    ImGuiID dock_log = ImGui::DockBuilderSplitNode(
        dock_waveforms, ImGuiDir_Down, 0.35f, nullptr, &dock_waveforms);

    ImGui::DockBuilderDockWindow("Signals", dock_signals);
    ImGui::DockBuilderDockWindow("Bus Values", dock_top);
    ImGui::DockBuilderDockWindow("Waveforms", dock_waveforms);
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

void AppWindow::buildMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open BSDL File...", "Ctrl+O", false, connected_)) {
                onOpenBsdl();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Load Config...")) {
                onLoadConfig();
            }
            if (ImGui::MenuItem("Save Config", "Ctrl+S")) {
                onSaveConfig();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export VCD...", nullptr, false,
                                capture_engine_ != nullptr)) {
                onExportVcd();
            }
            if (ImGui::MenuItem("Export CSV...", nullptr, false,
                                capture_engine_ != nullptr)) {
                onExportCsv();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Device")) {
            if (ImGui::MenuItem("Connect...", nullptr, false, !connected_)) {
                show_device_dialog_ = true;
            }
            if (ImGui::MenuItem("Disconnect", nullptr, false, connected_)) {
                onDisconnect();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Capture")) {
            bool can_capture = (capture_engine_ != nullptr && !capturing_);
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
            if (can_capture) {
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Trigger Setup...", "F8", false,
                                capture_engine_ != nullptr)) {
                show_trigger_dialog_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                show_about_ = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

} // namespace jtag::gui
