/// daemon_runner.cpp — JTAG daemon entry point, callable from jtag_daemon.exe
/// and from jtag_viewer.exe --mcp.
///
/// argc/argv must NOT include the program name: pass (original_argc - consumed,
/// original_argv + consumed) from the caller.

#include "daemon_runner.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Windows SDK defines IN and OUT as empty SAL annotation macros.
// Undefine them to avoid corrupting project enum values like PinDirection::IN.
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif
#endif

#include <nlohmann/json.hpp>

#include "src/boundary_scan/scanner.h"
#include "src/gui/app_config.h"
#include "src/hardware/hardware_context.h"
#include "src/hardware/hardware_executor.h"
#include "src/ila/bscane_ila_tap_backend.h"
#include "src/ila/ila_driver.h"
#include "src/ila/ila_tap_backend.h"
#include "src/mcp/executor_bridge.h"
#include "src/mcp/gui_rpc_server.h"
#include "src/mcp/mcp_server.h"
#include "src/mcp/mcp_transport.h"
#include "src/mcp/tools/register_tools.h"

namespace {

std::atomic<bool> g_stop_requested{false};

void signalHandler(int /*signal*/) {
    g_stop_requested.store(true, std::memory_order_release);
}

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop_requested.store(true, std::memory_order_release);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

void printEvent(const nlohmann::json& event) {
    const std::string line = event.dump();
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

void printErrorEvent(const std::string& message) {
    printEvent({{"event", "error"}, {"message", message}});
}

void printUsage() {
    std::printf(
        "Usage: [--mcp-port <port>] [--gui-port <port>]\n"
        "       [--config <path>] [--no-gui-port] [--no-mcp-port]\n"
        "       [--exit-on-disconnect]\n"
        "  --mcp-port <p>       MCP TCP port on 127.0.0.1 (default 9999, 0 = OS chosen).\n"
        "  --no-mcp-port        Disable MCP endpoint entirely (GUI-only mode).\n"
        "  --gui-port <p>       GUI RPC TCP port (default 0 = OS chosen).\n"
        "  --no-gui-port        Disable GUI RPC endpoint entirely.\n"
        "  --config <p>         cfg.json path (default: cfg.json).\n"
        "  --exit-on-disconnect Exit when the last MCP client disconnects.\n");
}

bool parsePort(const char* text, uint16_t& out_port) {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > 65535) {
        return false;
    }
    out_port = static_cast<uint16_t>(value);
    return true;
}

}  // namespace

int runDaemon(int argc, char* argv[]) {
    // Reset stop flag in case runDaemon is called more than once in a process.
    g_stop_requested.store(false, std::memory_order_relaxed);

    uint16_t mcp_port = 9999;
    uint16_t gui_port = 0;      // 0 = OS-chosen ephemeral port
    bool     gui_rpc  = true;   // disabled by --no-gui-port
    bool     mcp_rpc  = true;   // disabled by --no-mcp-port
    std::string config_path = "cfg.json";
    bool exit_on_disconnect = false;

    // argc/argv here do NOT include program name; loop starts at 0.
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mcp-port") == 0 && i + 1 < argc) {
            if (!parsePort(argv[++i], mcp_port)) {
                printErrorEvent(std::string("invalid MCP port: ") + argv[i]);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--gui-port") == 0 && i + 1 < argc) {
            if (!parsePort(argv[++i], gui_port)) {
                printErrorEvent(std::string("invalid GUI port: ") + argv[i]);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--no-mcp-port") == 0) {
            mcp_rpc = false;
        } else if (std::strcmp(argv[i], "--no-gui-port") == 0) {
            gui_rpc = false;
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--exit-on-disconnect") == 0) {
            exit_on_disconnect = true;
        } else if (std::strcmp(argv[i], "--help") == 0 ||
                   std::strcmp(argv[i], "-h") == 0) {
            printUsage();
            return 0;
        } else {
            printErrorEvent(std::string("unknown argument: ") + argv[i]);
            return 1;
        }
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#endif

    const auto cfg = jtag::gui::AppConfig::load(config_path);
    std::fprintf(stderr,
                 "[jtag_daemon] VID=0x%04X PID=0x%04X serial='%s' ch=%d freq=%u Hz\n",
                 cfg.vendor_id, cfg.product_id, cfg.serial.c_str(),
                 cfg.interface_channel, cfg.clock_freq_hz);

    jtag::hardware::HardwareExecutor executor(cfg);
    // Release the D2XX handle after 30 s of inactivity so other processes
    // (or a reconnecting jtag_viewer) can acquire the device.
    executor.setIdleCloseTimeout(std::chrono::milliseconds{30000});
    executor.startWorker();
    std::fprintf(stderr, "[jtag_daemon] Hardware executor running (lazy open, 30s idle-close).\n");

    jtag::mcp::ExecutorBridge bridge(executor);

    // McpServer is always created because GUI-RPC delegates to its registry.
    // The TCP transport is optional; skipped when --no-mcp-port is passed.
    jtag::mcp::McpServer mcp_server({"owltap-jtag-daemon", "0.1.0"});
    mcp_server.setExecutorBridge(&bridge);
    jtag::mcp::tools::registerHardwareTools(mcp_server.registry(), bridge);

    jtag::mcp::TcpTransport* mcp_transport_ptr = nullptr;
    if (mcp_rpc) {
        auto mcp_transport = std::make_unique<jtag::mcp::TcpTransport>(mcp_port);
        mcp_transport_ptr  = mcp_transport.get();
        mcp_server.setTransport(std::move(mcp_transport));
        mcp_server.start();

        if (!mcp_transport_ptr->isListening()) {
            printErrorEvent("failed to listen on MCP TCP port");
            mcp_server.stop();
            executor.shutdown();
            return 1;
        }
    }

    // -----------------------------------------------------------------------
    // GUI RPC server
    // -----------------------------------------------------------------------
    std::unique_ptr<jtag::mcp::GuiRpcServer> gui_server;
    if (gui_rpc) {
        gui_server = std::make_unique<jtag::mcp::GuiRpcServer>(gui_port);

        gui_server->registerMethod("daemon/status",
            [&bridge](const nlohmann::json&) -> nlohmann::json {
                return bridge.submitSync(
                    [](jtag::hardware::HardwareContext& ctx,
                       jtag::hardware::HardwareJob& /*job*/) -> nlohmann::json {
                        nlohmann::json devs = nlohmann::json::array();
                        for (const auto& d : ctx.chain().devices()) {
                            char idcode_buf[12];
                            std::snprintf(idcode_buf, sizeof(idcode_buf),
                                          "0x%08X", d.idcode);
                            devs.push_back({
                                {"position",    d.position},
                                {"idcode",      idcode_buf},
                                {"ir_length",   d.ir_length},
                                {"bsdl_loaded", d.bsdl != nullptr},
                                {"entity",      d.bsdl ? d.bsdl->entity_name : ""},
                            });
                        }
                        return nlohmann::json{
                            {"version",       "0.1.0"},
                            {"status",        "running"},
                            {"hardware_open", ctx.isOpen()},
                            {"device_count",  ctx.chain().deviceCount()},
                            {"devices",       devs},
                        };
                    },
                    "daemon/status",
                    std::chrono::milliseconds{1000});
            });

        auto& mcp_reg = mcp_server.registry();
        gui_server->registerMethod("hardware/detect_devices",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("detect_devices", p);
            });
        gui_server->registerMethod("hardware/load_bsdl",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("load_bsdl", p);
            });
        gui_server->registerMethod("hardware/list_pins",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("list_pins", p);
            });
        gui_server->registerMethod("hardware/read_pin",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("read_pin", p);
            });
        gui_server->registerMethod("hardware/set_pin",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("set_pin", p);
            });

        gui_server->registerMethod("capture/start",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("capture_start", p);
            });
        gui_server->registerMethod("capture/stop",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("capture_stop", p);
            });
        gui_server->registerMethod("capture/get_samples",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("get_samples", p);
            });
        gui_server->registerMethod("ila/status",
            [&bridge](const nlohmann::json& params) -> nlohmann::json {
                const int dev_idx     = params.value("device_index", 0);
                const bool use_bscane = params.value("use_bscane", false);
                const int user_chain  = params.value("user_chain", 1);
                return bridge.submitSync(
                    [dev_idx, use_bscane, user_chain](jtag::hardware::HardwareContext& ctx,
                                          jtag::hardware::HardwareJob&) -> nlohmann::json {
                        std::unique_ptr<jtag::ila::IlaTapBackend> be;
                        if (use_bscane)
                            be = std::make_unique<jtag::ila::BscaneIlaTapBackend>(ctx.chain(), dev_idx, user_chain);
                        else
                            be = std::make_unique<jtag::ila::ChainIlaTapBackend>(ctx.chain(), dev_idx);
                        jtag::ila::IlaDriver drv(*be);
                        jtag::ila::IlaStatus s{};
                        if (!drv.readStatus(s))
                            throw std::runtime_error(drv.lastError());
                        return nlohmann::json{{"armed", s.armed},
                                              {"triggered", s.triggered},
                                              {"full", s.full}};
                    },
                    "ila/status",
                    std::chrono::milliseconds{5000});
            });

        gui_server->registerMethod("ila/probe",
            [&bridge](const nlohmann::json& params) -> nlohmann::json {
                const int dev_idx     = params.value("device_index", 0);
                const bool use_bscane = params.value("use_bscane", false);
                const int user_chain  = params.value("user_chain", 1);
                return bridge.submitSync(
                    [dev_idx, use_bscane, user_chain](jtag::hardware::HardwareContext& ctx,
                                          jtag::hardware::HardwareJob&) -> nlohmann::json {
                        std::unique_ptr<jtag::ila::IlaTapBackend> be;
                        if (use_bscane)
                            be = std::make_unique<jtag::ila::BscaneIlaTapBackend>(ctx.chain(), dev_idx, user_chain);
                        else
                            be = std::make_unique<jtag::ila::ChainIlaTapBackend>(ctx.chain(), dev_idx);
                        jtag::ila::IlaDriver drv(*be);
                        jtag::ila::IlaCaps caps{};
                        if (!drv.probe(caps)) {
                            std::fprintf(stderr,
                                "[ila/probe] FAIL dev=%d bscane=%d chain=%d: %s\n",
                                dev_idx, (int)use_bscane, user_chain,
                                drv.lastError().c_str());
                            throw std::runtime_error(drv.lastError());
                        }
                        std::fprintf(stderr,
                            "[ila/probe] OK dev=%d chain=%d "
                            "raw=0x%08X idcode=0x%08X depth=%u\n",
                            dev_idx, user_chain,
                            caps.raw, caps.idcode, caps.depth);
                        nlohmann::json r{
                            {"version",   caps.version},
                            {"num_ch",    caps.num_ch},
                            {"sig_count", caps.sig_count},
                            {"data_w",    caps.data_w},
                            {"addr_w",    caps.addr_w},
                            {"depth",     caps.depth},
                            {"raw",       caps.raw},
                            {"idcode",    caps.idcode}};
                        if (caps.sig_count > 0) {
                            std::vector<jtag::ila::IlaSignalEntry> entries;
                            if (drv.readSignalDefs(entries)) {
                                nlohmann::json sigs = nlohmann::json::array();
                                for (const auto& e : entries)
                                    sigs.push_back({{"hi",       e.hi},
                                                    {"lo",       e.lo},
                                                    {"fmt",      e.fmt},
                                                    {"name_idx", e.name_idx}});
                                r["signal_defs"] = std::move(sigs);
                            }
                        }
                        return r;
                    },
                    "ila/probe",
                    std::chrono::milliseconds{5000});
            });

        gui_server->registerMethod("ila/arm",
            [&bridge](const nlohmann::json& params) -> nlohmann::json {
                const int dev_idx     = params.value("device_index", 0);
                const bool use_bscane = params.value("use_bscane", false);
                const int user_chain  = params.value("user_chain", 1);
                const uint32_t mask   = params.value("mask", 0u);
                const uint32_t value  = params.value("value", 0u);
                const uint32_t rise   = params.value("rise_mask", 0u);
                const uint32_t fall   = params.value("fall_mask", 0u);
                const uint32_t mask2  = params.value("mask2", 0u);
                const uint32_t val2   = params.value("val2", 0u);
                const bool or_mode    = params.value("or_mode", false);
                const uint16_t pre    = static_cast<uint16_t>(params.value("pre_samples", 256));
                return bridge.submitSync(
                    [dev_idx, use_bscane, user_chain, mask, value, rise, fall,
                     mask2, val2, or_mode, pre](
                        jtag::hardware::HardwareContext& ctx,
                        jtag::hardware::HardwareJob&) -> nlohmann::json {
                        std::unique_ptr<jtag::ila::IlaTapBackend> be;
                        if (use_bscane)
                            be = std::make_unique<jtag::ila::BscaneIlaTapBackend>(ctx.chain(), dev_idx, user_chain);
                        else
                            be = std::make_unique<jtag::ila::ChainIlaTapBackend>(ctx.chain(), dev_idx);
                        jtag::ila::IlaDriver drv(*be);
                        (void)drv.probe();
                        if (!drv.configureTrigger(mask, value, rise, fall, mask2, val2, or_mode, pre))
                            throw std::runtime_error(drv.lastError());
                        if (!drv.arm())
                            throw std::runtime_error(drv.lastError());
                        return nlohmann::json{{"ok", true}};
                    },
                    "ila/arm",
                    std::chrono::milliseconds{5000});
            });

        gui_server->registerMethod("ila/stop",
            [&bridge](const nlohmann::json& params) -> nlohmann::json {
                const int dev_idx     = params.value("device_index", 0);
                const bool use_bscane = params.value("use_bscane", false);
                const int user_chain  = params.value("user_chain", 1);
                return bridge.submitSync(
                    [dev_idx, use_bscane, user_chain](jtag::hardware::HardwareContext& ctx,
                                          jtag::hardware::HardwareJob&) -> nlohmann::json {
                        std::unique_ptr<jtag::ila::IlaTapBackend> be;
                        if (use_bscane)
                            be = std::make_unique<jtag::ila::BscaneIlaTapBackend>(ctx.chain(), dev_idx, user_chain);
                        else
                            be = std::make_unique<jtag::ila::ChainIlaTapBackend>(ctx.chain(), dev_idx);
                        jtag::ila::IlaDriver drv(*be);
                        if (!drv.stop()) throw std::runtime_error(drv.lastError());
                        return nlohmann::json{{"ok", true}};
                    },
                    "ila/stop",
                    std::chrono::milliseconds{3000});
            });

        gui_server->registerMethod("ila/force_trigger",
            [&bridge](const nlohmann::json& params) -> nlohmann::json {
                const int dev_idx     = params.value("device_index", 0);
                const bool use_bscane = params.value("use_bscane", false);
                const int user_chain  = params.value("user_chain", 1);
                return bridge.submitSync(
                    [dev_idx, use_bscane, user_chain](jtag::hardware::HardwareContext& ctx,
                                          jtag::hardware::HardwareJob&) -> nlohmann::json {
                        std::unique_ptr<jtag::ila::IlaTapBackend> be;
                        if (use_bscane)
                            be = std::make_unique<jtag::ila::BscaneIlaTapBackend>(ctx.chain(), dev_idx, user_chain);
                        else
                            be = std::make_unique<jtag::ila::ChainIlaTapBackend>(ctx.chain(), dev_idx);
                        jtag::ila::IlaDriver drv(*be);
                        if (!drv.forceTrigger()) throw std::runtime_error(drv.lastError());
                        return nlohmann::json{{"ok", true}};
                    },
                    "ila/force_trigger",
                    std::chrono::milliseconds{3000});
            });

        gui_server->registerMethod("ila/reset",
            [&bridge](const nlohmann::json& params) -> nlohmann::json {
                const int dev_idx     = params.value("device_index", 0);
                const bool use_bscane = params.value("use_bscane", false);
                const int user_chain  = params.value("user_chain", 1);
                return bridge.submitSync(
                    [dev_idx, use_bscane, user_chain](jtag::hardware::HardwareContext& ctx,
                                          jtag::hardware::HardwareJob&) -> nlohmann::json {
                        std::unique_ptr<jtag::ila::IlaTapBackend> be;
                        if (use_bscane)
                            be = std::make_unique<jtag::ila::BscaneIlaTapBackend>(ctx.chain(), dev_idx, user_chain);
                        else
                            be = std::make_unique<jtag::ila::ChainIlaTapBackend>(ctx.chain(), dev_idx);
                        jtag::ila::IlaDriver drv(*be);
                        if (!drv.resetCapture()) throw std::runtime_error(drv.lastError());
                        return nlohmann::json{{"ok", true}};
                    },
                    "ila/reset",
                    std::chrono::milliseconds{3000});
            });

        gui_server->registerMethod("ila/read_samples",
            [&bridge](const nlohmann::json& params) -> nlohmann::json {
                const int dev_idx     = params.value("device_index", 0);
                const bool use_bscane = params.value("use_bscane", false);
                const int user_chain  = params.value("user_chain", 1);
                return bridge.submitSync(
                    [dev_idx, use_bscane, user_chain](jtag::hardware::HardwareContext& ctx,
                                          jtag::hardware::HardwareJob&) -> nlohmann::json {
                        std::unique_ptr<jtag::ila::IlaTapBackend> be;
                        if (use_bscane)
                            be = std::make_unique<jtag::ila::BscaneIlaTapBackend>(ctx.chain(), dev_idx, user_chain);
                        else
                            be = std::make_unique<jtag::ila::ChainIlaTapBackend>(ctx.chain(), dev_idx);
                        jtag::ila::IlaDriver drv(*be);
                        if (!drv.probe())
                            throw std::runtime_error(drv.lastError());
                        if (!drv.setReadAddr(static_cast<uint16_t>(drv.depth() - 1)))
                            throw std::runtime_error(drv.lastError());
                        std::vector<uint32_t> samples;
                        if (!drv.readSamples(samples))
                            throw std::runtime_error(drv.lastError());
                        nlohmann::json arr = nlohmann::json::array();
                        for (uint32_t v : samples) arr.push_back(v);
                        return nlohmann::json{{"ok", true},
                                              {"samples", std::move(arr)},
                                              {"data_w",  drv.dataWidth()}};
                    },
                    "ila/read_samples",
                    std::chrono::milliseconds{10000});
            });

        gui_server->registerMethod("job/poll",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("job_poll", p);
            });
        gui_server->registerMethod("job/cancel",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("job_cancel", p);
            });

        gui_server->registerMethod("hardware/sample_bsr",
            [&bridge](const nlohmann::json& params) -> nlohmann::json {
                int dev_idx = params.value("device_index", 0);
                return bridge.submitSync(
                    [dev_idx](jtag::hardware::HardwareContext& ctx,
                              jtag::hardware::HardwareJob& /*job*/) -> nlohmann::json {
                        jtag::Scanner& sc = ctx.scanner(dev_idx);
                        // Always clear any decode filter left by a prior GUI
                        // capture job so all pins are returned.
                        sc.setDecodeFilter({});
                        jtag::ScanResult result = sc.sample();
                        if (!sc.lastError().empty()) {
                            throw std::runtime_error(sc.lastError());
                        }
                        nlohmann::json pins = nlohmann::json::object();
                        for (const auto& [name, state] : result.pin_states) {
                            const char* s = "unknown";
                            if (state == jtag::PinState::HIGH)  s = "high";
                            else if (state == jtag::PinState::LOW) s = "low";
                            pins[name] = s;
                        }
                        nlohmann::json raw_bsr = nlohmann::json::array();
                        for (uint8_t byte : result.raw_bsr) {
                            raw_bsr.push_back(byte);
                        }
                        return nlohmann::json{{"device_index", dev_idx},
                                              {"pins", pins},
                                              {"raw_bsr", raw_bsr}};
                    },
                    "hardware/sample_bsr",
                    std::chrono::milliseconds{2000});
            });
        gui_server->registerMethod("hardware/program_pl",
            [&bridge](const nlohmann::json& params) -> nlohmann::json {
                const int dev_idx         = params.value("device_index", 0);
                const std::string path    = params.at("bitstream_path").get<std::string>();
                // Timeout: bitstream can be large; allow up to 120 s.
                return bridge.submitSync(
                    [dev_idx, path](jtag::hardware::HardwareContext& ctx,
                                   jtag::hardware::HardwareJob&) -> nlohmann::json {
                        jtag::PlConfig& pl = ctx.plConfig(dev_idx);
                        bool ok = pl.program(path);
                        if (!ok) throw std::runtime_error(pl.lastError());
                        return nlohmann::json{{"ok", true}};
                    },
                    "hardware/program_pl",
                    std::chrono::milliseconds{120000});
            });

        gui_server->registerMethod("script/run",
            [&mcp_reg](const nlohmann::json& p) {
                return mcp_reg.callToolRaw("run_script", p);
            });

        gui_server->start();

        if (!gui_server->isListening()) {
            printErrorEvent("failed to listen on GUI RPC TCP port");
            mcp_server.stop();
            executor.shutdown();
            return 1;
        }
        std::fprintf(stderr,
                     "[jtag_daemon] GUI RPC listening on 127.0.0.1:%u\n",
                     gui_server->port());
    }

    printEvent({{
        "event",    "ready"},
        {"mcp_port", mcp_transport_ptr ? mcp_transport_ptr->port() : uint16_t(0)},
        {"gui_port", gui_server ? nlohmann::json(gui_server->port())
                                : nlohmann::json(nullptr)},
        {"gui_rpc",  gui_rpc}
    });
    if (mcp_transport_ptr) {
        std::fprintf(stderr,
                     "[jtag_daemon] MCP listening on 127.0.0.1:%u\n",
                     mcp_transport_ptr->port());
    } else {
        std::fprintf(stderr, "[jtag_daemon] MCP disabled (--no-mcp-port).\n");
    }

    while (!g_stop_requested.load(std::memory_order_acquire) &&
           (!mcp_rpc || mcp_server.isRunning())) {
        if (exit_on_disconnect && mcp_transport_ptr &&
            mcp_transport_ptr->completedConnections() > 0) {
            std::fprintf(stderr, "[jtag_daemon] Client disconnected; exiting.\n");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (gui_server) gui_server->stop();
    mcp_server.stop();
    executor.shutdown();
    printEvent({{"event", "stopped"}});
    std::fprintf(stderr, "[jtag_daemon] Exiting.\n");
    return 0;
}
