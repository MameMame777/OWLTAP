#include "register_tools.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "src/boundary_scan/pin_driver.h"
#include "src/boundary_scan/scanner.h"
#include "src/capture/capture_engine.h"
#include "src/capture/trigger.h"
#include "src/hardware/capture_session.h"
#include "src/hardware/hardware_context.h"
#include "src/ila/ila_driver.h"
#include "src/ila/bscane_ila_tap_backend.h"
#include "src/ila/ila_tap_backend.h"
#include "src/script/script_engine.h"

namespace jtag::mcp::tools {

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

static std::string idcodeStr(uint32_t id) {
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex
        << std::setfill('0') << std::setw(8) << id;
    return oss.str();
}

static nlohmann::json pinStateStr(PinState s) {
    switch (s) {
        case PinState::LOW:     return "low";
        case PinState::HIGH:    return "high";
        case PinState::UNKNOWN: return "unknown";
    }
    return "unknown";
}

static nlohmann::json scanResultToJson(const ScanResult& r) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [name, state] : r.pin_states) {
        j[name] = pinStateStr(state);
    }
    return j;
}

static nlohmann::json rawBsrToJson(const std::vector<uint8_t>& raw_bsr) {
    nlohmann::json arr = nlohmann::json::array();
    for (uint8_t byte : raw_bsr) {
        arr.push_back(byte);
    }
    return arr;
}

static nlohmann::json samplesToJson(const std::vector<SampleFrame>& frames) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& f : frames) {
        nlohmann::json s;
        s["timestamp_us"] = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                f.timestamp.time_since_epoch())
                .count());
        s["trigger_point"] = f.trigger_point;
        s["pins"] = scanResultToJson(f.data);
            s["raw_bsr"] = rawBsrToJson(f.data.raw_bsr);
        arr.push_back(std::move(s));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// ScriptHost implementation for MCP run_script tool
// ---------------------------------------------------------------------------

class McpScriptHost : public script::ScriptHost {
public:
    McpScriptHost(Scanner& scanner, PinDriver& driver)
        : scanner_(scanner), driver_(driver) {}

    bool sample(ScanResult& result, std::string& error) override {
        result = scanner_.sample();
        error  = scanner_.lastError();
        return error.empty();
    }

    bool setPin(const std::string& pin_name, int value,
                std::string& error) override {
        if (!driver_.setPin(pin_name, value)) {
            error = driver_.lastError();
            return false;
        }
        return true;
    }

    bool setPinHighZ(const std::string& pin_name,
                     std::string& error) override {
        if (!driver_.setPinHighZ(pin_name)) {
            error = driver_.lastError();
            return false;
        }
        return true;
    }

    bool applyOutputs(std::string& error) override {
        if (!driver_.applyOutputs()) {
            error = driver_.lastError();
            return false;
        }
        return true;
    }

    void resetToSafe() override {
        driver_.resetToSafe();
    }

    bool sleepMs(int milliseconds, std::string& /*error*/) override {
        if (milliseconds > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(milliseconds));
        }
        return true;
    }

private:
    Scanner&   scanner_;
    PinDriver& driver_;
};

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void registerHardwareTools(ToolRegistry& registry, ExecutorBridge& bridge) {

    // -----------------------------------------------------------------------
    // 1. read_idcode
    // -----------------------------------------------------------------------
    registry.registerTool({
        "read_idcode",
        "Read JTAG IDCODEs from all devices in the chain.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [&bridge](nlohmann::json /*params*/) -> nlohmann::json {
            return bridge.submitSync(
                [](hardware::HardwareContext& ctx,
                   hardware::HardwareJob& /*job*/) -> nlohmann::json {
                    const int n = ctx.chain().detectDevices();
                    ctx.resetDeviceObjects();
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& dev : ctx.chain().devices()) {
                        nlohmann::json d;
                        d["position"] = dev.position;
                        d["idcode"]   = idcodeStr(dev.idcode);
                        d["ir_length"] = dev.ir_length;
                        arr.push_back(std::move(d));
                    }
                    return nlohmann::json{{"device_count", n},
                                          {"devices", arr}};
                },
                "read_idcode",
                std::chrono::milliseconds{2000});
        }
    });

    // -----------------------------------------------------------------------
    // 2. detect_devices
    // -----------------------------------------------------------------------
    registry.registerTool({
        "detect_devices",
        "Re-detect and enumerate all JTAG devices in the chain.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [&bridge](nlohmann::json /*params*/) -> nlohmann::json {
            return bridge.submitSync(
                [](hardware::HardwareContext& ctx,
                   hardware::HardwareJob& /*job*/) -> nlohmann::json {
                    const int n = ctx.chain().detectDevices();
                    ctx.resetDeviceObjects();
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& dev : ctx.chain().devices()) {
                        nlohmann::json d;
                        d["position"]  = dev.position;
                        d["idcode"]    = idcodeStr(dev.idcode);
                        d["ir_length"] = dev.ir_length;
                        d["bsdl_loaded"] = (dev.bsdl != nullptr);
                        arr.push_back(std::move(d));
                    }
                    return nlohmann::json{{"device_count", n},
                                          {"devices", arr}};
                },
                "detect_devices",
                std::chrono::milliseconds{3000});
        }
    });

    // -----------------------------------------------------------------------
    // 3. load_bsdl
    // -----------------------------------------------------------------------
    registry.registerTool({
        "load_bsdl",
        "Load a BSDL file for a specific device in the JTAG chain.",
        {
            {"type", "object"},
            {"required", {"device_index", "bsdl_path"}},
            {"properties", {
                {"device_index", {{"type", "integer"}}},
                {"bsdl_path",    {{"type", "string"}}}
            }}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            int         dev_idx  = params["device_index"].get<int>();
            std::string bsdl_path = params["bsdl_path"].get<std::string>();
            return bridge.submitSync(
                [dev_idx, bsdl_path](hardware::HardwareContext& ctx,
                                     hardware::HardwareJob& /*job*/) -> nlohmann::json {
                    if (!ctx.chain().loadBsdl(dev_idx, bsdl_path)) {
                        throw std::runtime_error(ctx.chain().lastError());
                    }
                    const auto& dev = ctx.chain().devices().at(
                        static_cast<std::size_t>(dev_idx));
                    return nlohmann::json{
                        {"ok", true},
                        {"device_index", dev_idx},
                        {"bsdl_path", bsdl_path},
                        {"entity", dev.bsdl ? dev.bsdl->entity_name : ""}};
                },
                "load_bsdl",
                std::chrono::milliseconds{5000});
        }
    });

    // -----------------------------------------------------------------------
    // 4. list_devices
    // -----------------------------------------------------------------------
    registry.registerTool({
        "list_devices",
        "List currently detected JTAG devices (cached; call detect_devices to refresh).",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [&bridge](nlohmann::json /*params*/) -> nlohmann::json {
            return bridge.submitSync(
                [](hardware::HardwareContext& ctx,
                   hardware::HardwareJob& /*job*/) -> nlohmann::json {
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& dev : ctx.chain().devices()) {
                        nlohmann::json d;
                        d["position"]    = dev.position;
                        d["idcode"]      = idcodeStr(dev.idcode);
                        d["ir_length"]   = dev.ir_length;
                        d["bsdl_loaded"] = (dev.bsdl != nullptr);
                        if (dev.bsdl) {
                            d["entity"] = dev.bsdl->entity_name;
                        }
                        arr.push_back(std::move(d));
                    }
                    return nlohmann::json{
                        {"device_count", ctx.chain().deviceCount()},
                        {"devices", arr}};
                },
                "list_devices",
                std::chrono::milliseconds{500});
        }
    });

    // -----------------------------------------------------------------------
    // 5. list_pins
    // -----------------------------------------------------------------------
    registry.registerTool({
        "list_pins",
        "List observable and drivable pins for a device (BSDL must be loaded).",
        {
            {"type", "object"},
            {"required", {"device_index"}},
            {"properties", {{"device_index", {{"type", "integer"}}}}}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            int dev_idx = params["device_index"].get<int>();
            return bridge.submitSync(
                [dev_idx](hardware::HardwareContext& ctx,
                          hardware::HardwareJob& /*job*/) -> nlohmann::json {
                    Scanner& sc = ctx.scanner(dev_idx);
                    auto obs  = sc.getObservablePins();
                    auto drv  = sc.getDrivablePins();
                    nlohmann::json obs_arr = nlohmann::json::array();
                    for (auto& p : obs) obs_arr.push_back(std::move(p));
                    nlohmann::json drv_arr = nlohmann::json::array();
                    for (auto& p : drv) drv_arr.push_back(std::move(p));
                    return nlohmann::json{{"device_index", dev_idx},
                                          {"observable", obs_arr},
                                          {"drivable", drv_arr}};
                },
                "list_pins",
                std::chrono::milliseconds{1000});
        }
    });

    // -----------------------------------------------------------------------
    // 6. read_pin
    // -----------------------------------------------------------------------
    registry.registerTool({
        "read_pin",
        "Read the current state of a pin via JTAG boundary scan SAMPLE.",
        {
            {"type", "object"},
            {"required", {"device_index", "pin_name"}},
            {"properties", {
                {"device_index", {{"type", "integer"}}},
                {"pin_name",     {{"type", "string"}}}
            }}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            int         dev_idx  = params["device_index"].get<int>();
            std::string pin_name = params["pin_name"].get<std::string>();
            return bridge.submitSync(
                [dev_idx, pin_name](hardware::HardwareContext& ctx,
                                    hardware::HardwareJob& /*job*/) -> nlohmann::json {
                    Scanner& sc = ctx.scanner(dev_idx);
                    ScanResult result = sc.sample();
                    if (!sc.lastError().empty()) {
                        throw std::runtime_error(sc.lastError());
                    }
                    const PinState state = result.getPin(pin_name);
                    return nlohmann::json{
                        {"device_index", dev_idx},
                        {"pin_name", pin_name},
                        {"state", pinStateStr(state)}};
                },
                "read_pin",
                std::chrono::milliseconds{2000});
        }
    });

    // -----------------------------------------------------------------------
    // 7. set_pin
    // -----------------------------------------------------------------------
    registry.registerTool({
        "set_pin",
        "Drive a pin to HIGH (1), LOW (0), or HIGH-Z (-1) via EXTEST.",
        {
            {"type", "object"},
            {"required", {"device_index", "pin_name", "value"}},
            {"properties", {
                {"device_index", {{"type", "integer"}}},
                {"pin_name",     {{"type", "string"}}},
                {"value",        {{"type", "integer"},
                                   {"description", "0=LOW, 1=HIGH, -1=HIGH-Z"}}}
            }}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            int         dev_idx  = params["device_index"].get<int>();
            std::string pin_name = params["pin_name"].get<std::string>();
            int         value    = params["value"].get<int>();
            return bridge.submitSync(
                [dev_idx, pin_name, value](
                    hardware::HardwareContext& ctx,
                    hardware::HardwareJob& /*job*/) -> nlohmann::json {
                    PinDriver& drv = ctx.pinDriver(dev_idx);
                    bool ok;
                    if (value < 0) {
                        ok = drv.setPinHighZ(pin_name);
                    } else {
                        ok = drv.setPin(pin_name, value);
                    }
                    if (!ok) throw std::runtime_error(drv.lastError());
                    if (!drv.applyOutputs()) {
                        throw std::runtime_error(drv.lastError());
                    }
                    return nlohmann::json{{"ok", true},
                                          {"device_index", dev_idx},
                                          {"pin_name", pin_name},
                                          {"value", value}};
                },
                "set_pin",
                std::chrono::milliseconds{2000});
        }
    });

    // -----------------------------------------------------------------------
    // 8. capture_start
    //    Submits an async task that runs a CaptureSession until complete or
    //    cancelled.  Returns {job_id} for polling via get_samples / job_poll.
    // -----------------------------------------------------------------------
    registry.registerTool({
        "capture_start",
        "Start a boundary-scan capture session. Returns job_id for async polling.",
        {
            {"type", "object"},
            {"required", {"device_index"}},
            {"properties", {
                {"device_index",  {{"type", "integer"}}},
                {"buffer_depth",  {{"type", "integer"},
                                    {"description", "Number of samples (default 1000)"}}},
                {"interval_us",   {{"type", "integer"},
                                    {"description", "Sample interval in microseconds (default 1000)"}}},
                {"trigger_mode",  {{"type", "string"},
                                    {"description", "free_run | single | normal (default free_run)"}}},
                {"pin_filter",    {{"type", "array"}, {"items", {{"type", "string"}}},
                                    {"description", "Only decode these pin names (empty = all pins)"}}}
            }}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            int    dev_idx    = params["device_index"].get<int>();
            int    buf_depth  = params.value("buffer_depth", 1000);
            int    interval   = params.value("interval_us", 1000);
            std::string mode_str = params.value("trigger_mode",
                                                 std::string{"free_run"});

            // Optional pin filter: limits BSR decode and JSON serialisation to
            // the selected pins, which dramatically reduces per-sample overhead.
            std::vector<std::string> pin_filter;
            if (params.contains("pin_filter") && params["pin_filter"].is_array()) {
                for (const auto& p : params["pin_filter"])
                    pin_filter.push_back(p.get<std::string>());
            }

            TriggerMode tmode = TriggerMode::FREE_RUN;
            if (mode_str == "single")   tmode = TriggerMode::SINGLE;
            else if (mode_str == "normal") tmode = TriggerMode::NORMAL;

            return bridge.submitAsync(
                [dev_idx, buf_depth, interval, tmode,
                 pin_filter = std::move(pin_filter)](
                    hardware::HardwareContext& ctx,
                    hardware::HardwareJob& job) -> nlohmann::json {
                    Scanner& sc = ctx.scanner(dev_idx);
                    // Apply decode filter on daemon's scanner for partial BSR read
                    // and reduced JSON payload.
                    sc.setDecodeFilter(pin_filter);
                    hardware::CaptureSession cs(sc);
                    cs.setBufferDepth(static_cast<std::size_t>(buf_depth));
                    cs.setSampleInterval(static_cast<uint32_t>(interval));
                    cs.trigger().setMode(tmode);
                    cs.start();

                    // Report partial samples in progress so GUI can display
                    // live waveform updates without waiting for full completion.
                    std::size_t last_reported_count = 0;
                    static constexpr std::size_t kProgressBatchSize = 20;

                    while (cs.state() != CaptureState::COMPLETE &&
                           cs.state() != CaptureState::STOPPED) {
                        if (job.isCancelRequested()) {
                            cs.stop();
                            break;
                        }
                        cs.tick();
                        const std::size_t n = cs.sampleCount();
                        if (n >= last_reported_count + kProgressBatchSize ||
                            (n > 0 && last_reported_count == 0)) {
                            last_reported_count = n;
                            job.setProgress(nlohmann::json{
                                {"state",   "capturing"},
                                {"count",   static_cast<int>(n)},
                                {"samples", samplesToJson(cs.getSamples())}});
                        } else {
                            job.setProgress(nlohmann::json{
                                {"state", "capturing"},
                                {"count", static_cast<int>(n)}});
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(1));
                    }

                    if (!cs.lastError().empty()) {
                        throw std::runtime_error(cs.lastError());
                    }
                    return nlohmann::json{
                        {"device_index", dev_idx},
                        {"sample_count",
                         static_cast<int>(cs.sampleCount())},
                        {"samples", samplesToJson(cs.getSamples())}};
                },
                "capture_start");
        }
    });

    // -----------------------------------------------------------------------
    // 9. capture_stop
    // -----------------------------------------------------------------------
    registry.registerTool({
        "capture_stop",
        "Cancel a running capture_start job.",
        {
            {"type", "object"},
            {"required", {"job_id"}},
            {"properties", {{"job_id", {{"type", "string"}}}}}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            std::string job_id = params["job_id"].get<std::string>();
            return bridge.cancelJob(job_id);
        }
    });

    // -----------------------------------------------------------------------
    // 10. get_samples
    // -----------------------------------------------------------------------
    registry.registerTool({
        "get_samples",
        "Poll a capture_start job and return current state and samples.",
        {
            {"type", "object"},
            {"required", {"job_id"}},
            {"properties", {{"job_id", {{"type", "string"}}}}}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            std::string job_id = params["job_id"].get<std::string>();
            return bridge.pollJob(job_id);
        }
    });

    // -----------------------------------------------------------------------
    // 11. run_script
    // -----------------------------------------------------------------------
    registry.registerTool({
        "run_script",
        "Execute a waveform test script against a JTAG device.",
        {
            {"type", "object"},
            {"required", {"device_index", "script"}},
            {"properties", {
                {"device_index", {{"type", "integer"}}},
                {"script",       {{"type", "string"},
                                   {"description", "Script text to execute"}}}
            }}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            int         dev_idx = params["device_index"].get<int>();
            std::string script  = params["script"].get<std::string>();
            return bridge.submitSync(
                [dev_idx, script](hardware::HardwareContext& ctx,
                                  hardware::HardwareJob& /*job*/) -> nlohmann::json {
                    Scanner&   sc  = ctx.scanner(dev_idx);
                    PinDriver& drv = ctx.pinDriver(dev_idx);
                    McpScriptHost host(sc, drv);
                    const auto r = script::ScriptEngine::run(script, host);
                    return nlohmann::json{
                        {"success",        r.success},
                        {"failed_line",    r.failed_line},
                        {"output",         r.output},
                        {"total_expects",  r.total_expects},
                        {"failed_expects", r.failed_expects}};
                },
                "run_script",
                std::chrono::milliseconds{30000});
        }
    });

    // -----------------------------------------------------------------------
    // 12. program_bitstream
    // -----------------------------------------------------------------------
    registry.registerTool({
        "program_bitstream",
        "Program the FPGA PL from a .bit or .bin file (async). Returns job_id.",
        {
            {"type", "object"},
            {"required", {"device_index", "bitstream_path"}},
            {"properties", {
                {"device_index",    {{"type", "integer"}}},
                {"bitstream_path",  {{"type", "string"}}}
            }}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            int         dev_idx  = params["device_index"].get<int>();
            std::string bit_path = params["bitstream_path"].get<std::string>();
            return bridge.submitAsync(
                [dev_idx, bit_path](hardware::HardwareContext& ctx,
                                    hardware::HardwareJob& job) -> nlohmann::json {
                    PlConfig& pl = ctx.plConfig(dev_idx);
                    bool ok = pl.program(
                        bit_path,
                        [&job](size_t sent, size_t total) {
                            job.setProgress(nlohmann::json{
                                {"bytes_sent",  static_cast<long long>(sent)},
                                {"total_bytes", static_cast<long long>(total)},
                                {"percent",
                                 (total > 0)
                                     ? static_cast<int>(sent * 100 / total)
                                     : 0}});
                        });
                    if (!ok) throw std::runtime_error("program_bitstream failed");
                    return nlohmann::json{{"ok", true},
                                          {"bitstream_path", bit_path}};
                },
                "program_bitstream");
        }
    });

    // -----------------------------------------------------------------------
    // 13. read_ila_status
    // -----------------------------------------------------------------------
    registry.registerTool({
        "read_ila_status",
        "Read status and capabilities from the OwlTAP ILA IP TAP.",
        {
            {"type", "object"},
            {"required", {"device_index"}},
            {"properties", {
                {"device_index", {{"type", "integer"}}},
                {"use_bscane",   {{"type", "boolean"},
                                   {"description",
                                    "true = access ILA via BSCANE2 TAP (default false)"}}},
                {"user_chain",   {{"type", "integer"},
                                   {"description",
                                    "BSCANE2 chain index 1..4 (USER1..USER4, default 1)"}}}
            }}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            int  dev_idx    = params["device_index"].get<int>();
            bool use_bscane = params.value("use_bscane", false);
            int  user_chain = params.value("user_chain", 1);
            return bridge.submitSync(
                [dev_idx, use_bscane, user_chain](hardware::HardwareContext& ctx,
                          hardware::HardwareJob& /*job*/) -> nlohmann::json {
                    std::unique_ptr<ila::IlaTapBackend> backend;
                    if (use_bscane)
                        backend = std::make_unique<ila::BscaneIlaTapBackend>(
                                      ctx.chain(), dev_idx, user_chain);
                    else
                        backend = std::make_unique<ila::ChainIlaTapBackend>(
                                      ctx.chain(), dev_idx);
                    ila::IlaDriver ila(*backend);

                    ila::IlaCaps caps;
                    if (!ila.probe(caps)) {
                        throw std::runtime_error(backend->lastError());
                    }

                    ila::IlaStatus status;
                    if (!ila.readStatus(status)) {
                        throw std::runtime_error(backend->lastError());
                    }

                    return nlohmann::json{
                        {"version",    caps.version},
                        {"data_width", caps.data_w},
                        {"addr_width", caps.addr_w},
                        {"depth",      caps.depth},
                        {"sig_count",  caps.sig_count},
                        {"armed",      status.armed},
                        {"triggered",  status.triggered},
                        {"full",       status.full}};
                },
                "read_ila_status",
                std::chrono::milliseconds{3000});
        }
    });

    // -----------------------------------------------------------------------
    // 14. job_poll
    // -----------------------------------------------------------------------
    registry.registerTool({
        "job_poll",
        "Poll the status of an async hardware job.",
        {
            {"type", "object"},
            {"required", {"job_id"}},
            {"properties", {{"job_id", {{"type", "string"}}}}}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            return bridge.pollJob(params["job_id"].get<std::string>());
        }
    });

    // -----------------------------------------------------------------------
    // 15. job_cancel
    // -----------------------------------------------------------------------
    registry.registerTool({
        "job_cancel",
        "Request cancellation of a running or queued async hardware job.",
        {
            {"type", "object"},
            {"required", {"job_id"}},
            {"properties", {{"job_id", {{"type", "string"}}}}}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            return bridge.cancelJob(params["job_id"].get<std::string>());
        }
    });

    // -----------------------------------------------------------------------
    // 16. ila_run_capture
    //     Reset -> configure trigger (match-all) -> arm -> optional force
    //     -> poll until full -> read all samples.  One-shot synchronous call.
    // -----------------------------------------------------------------------
    registry.registerTool({
        "ila_run_capture",
        "Arm the OwlTAP ILA, optionally force-trigger, then return waveform samples.",
        {
            {"type", "object"},
            {"required", {"device_index"}},
            {"properties", {
                {"device_index", {{"type", "integer"}}},
                {"use_bscane",   {{"type", "boolean"},
                                   {"description",
                                    "true = access ILA via BSCANE2 TAP (default false)"}}},
                {"user_chain",   {{"type", "integer"},
                                   {"description",
                                    "BSCANE2 chain index 1..4 (USER1..USER4, default 1)"}}},
                {"force",        {{"type", "boolean"},
                                   {"description",
                                    "true = force-trigger immediately after arm (default true)"}}},
                {"pre_samples",  {{"type", "integer"},
                                   {"description",
                                    "Pre-trigger sample count (default 0)"}}},
                {"timeout_ms",   {{"type", "integer"},
                                   {"description",
                                    "Poll timeout in milliseconds (default 5000)"}}}
            }}
        },
        [&bridge](nlohmann::json params) -> nlohmann::json {
            int  dev_idx    = params["device_index"].get<int>();
            bool use_bscane = params.value("use_bscane",  false);
            int  user_chain = params.value("user_chain",  1);
            bool force      = params.value("force",       true);
            int  pre        = params.value("pre_samples", 0);
            int  timeout_ms = params.value("timeout_ms",  5000);

            return bridge.submitSync(
                [dev_idx, use_bscane, user_chain, force, pre, timeout_ms](
                    hardware::HardwareContext& ctx,
                    hardware::HardwareJob& /*job*/) -> nlohmann::json {

                    std::unique_ptr<ila::IlaTapBackend> backend;
                    if (use_bscane)
                        backend = std::make_unique<ila::BscaneIlaTapBackend>(
                                      ctx.chain(), dev_idx, user_chain);
                    else
                        backend = std::make_unique<ila::ChainIlaTapBackend>(
                                      ctx.chain(), dev_idx);

                    ila::IlaDriver ila(*backend);

                    // 1. Probe capabilities.
                    ila::IlaCaps caps;
                    if (!ila.probe(caps))
                        throw std::runtime_error(ila.lastError());

                    // 2. Reset any previous capture run.
                    if (!ila.resetCapture())
                        throw std::runtime_error(ila.lastError());

                    // 3. Configure trigger: match-all (mask=0, value=0).
                    if (!ila.configureTrigger(0, 0,
                                              static_cast<uint16_t>(pre)))
                        throw std::runtime_error(ila.lastError());

                    // 4. Arm.
                    if (!ila.arm())
                        throw std::runtime_error(ila.lastError());

                    // 5. Optionally force-trigger.
                    if (force) {
                        if (!ila.forceTrigger())
                            throw std::runtime_error(ila.lastError());
                    }

                    // 6. Poll readStatus() until full==true or timeout.
                    const auto deadline =
                        std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
                    ila::IlaStatus status{};
                    while (true) {
                        if (!ila.readStatus(status))
                            throw std::runtime_error(ila.lastError());
                        if (status.full)
                            break;
                        if (std::chrono::steady_clock::now() >= deadline)
                            throw std::runtime_error(
                                "ila_run_capture: timeout waiting for full");
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(10));
                    }

                    // 7. Read all samples from address 0.
                    if (!ila.setReadAddr(0))
                        throw std::runtime_error(ila.lastError());
                    std::vector<uint32_t> raw;
                    if (!ila.readSamples(raw))
                        throw std::runtime_error(ila.lastError());

                    // Encode samples as hex strings for JSON transport.
                    nlohmann::json sample_arr = nlohmann::json::array();
                    for (uint32_t v : raw) {
                        std::ostringstream oss;
                        oss << "0x" << std::uppercase << std::hex
                            << std::setfill('0') << std::setw(8) << v;
                        sample_arr.push_back(oss.str());
                    }

                    // Load signal names from sidecar JSON if available.
                    nlohmann::json signal_names = nlohmann::json::array();
                    const std::string& sidecar =
                        ctx.config().ila_config_json_path;
                    if (!sidecar.empty()) {
                        std::ifstream jf(sidecar);
                        if (jf.is_open()) {
                            try {
                                auto j = nlohmann::json::parse(jf);
                                if (j.contains("lanes") &&
                                    j["lanes"].is_array()) {
                                    for (const auto& lane : j["lanes"])
                                        signal_names.push_back(
                                            lane.value("name", ""));
                                }
                            } catch (...) {}
                        }
                    }

                    return nlohmann::json{
                        {"version",      caps.version},
                        {"data_width",   caps.data_w},
                        {"depth",        caps.depth},
                        {"sample_count", static_cast<int>(raw.size())},
                        {"signal_names", signal_names},
                        {"samples",      sample_arr}};
                },
                "ila_run_capture",
                std::chrono::milliseconds{static_cast<long long>(timeout_ms) + 3000});
        }
    });
}

}  // namespace jtag::mcp::tools
