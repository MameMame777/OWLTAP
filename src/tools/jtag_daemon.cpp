/// jtag_daemon - Local daemon that owns OwlTAP hardware access.
///
/// Phase 3: GUI RPC minimal flow — second TCP endpoint for the GUI process
/// to detect devices, load BSDL, and list pins through the daemon.
///
/// Usage:
///   jtag_daemon [--mcp-port <port>] [--gui-port <port>] [--config <path>]
///               [--no-gui-port] [--exit-on-disconnect]

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

#include "src/gui/app_config.h"
#include "src/hardware/hardware_executor.h"
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
// Windows console control events that SIGINT does not cover:
// CTRL_CLOSE_EVENT  — user closes the console window
// CTRL_LOGOFF_EVENT — interactive logoff
// CTRL_SHUTDOWN_EVENT — system shutdown
// The handler must return quickly; the OS kills the process after ~5 s.
BOOL WINAPI consoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop_requested.store(true, std::memory_order_release);
        return TRUE;  // suppress default handler
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
        "Usage: jtag_daemon [--mcp-port <port>] [--gui-port <port>]\n"
        "                   [--config <path>] [--no-gui-port]\n"
        "                   [--exit-on-disconnect]\n"
        "  --mcp-port <p>       MCP TCP port on 127.0.0.1 (default 9999, 0 = OS chosen).\n"
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

int main(int argc, char* argv[]) {
    uint16_t mcp_port = 9999;
    uint16_t gui_port = 0;      // 0 = OS-chosen ephemeral port
    bool     gui_rpc  = true;   // disabled by --no-gui-port
    std::string config_path = "cfg.json";
    bool exit_on_disconnect = false;

    for (int i = 1; i < argc; ++i) {
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
    const std::string hw_err = executor.start();
    if (!hw_err.empty()) {
        printErrorEvent("hardware executor start failed: " + hw_err);
        std::fprintf(stderr, "[jtag_daemon] ERROR: %s\n", hw_err.c_str());
        return 1;
    }
    std::fprintf(stderr, "[jtag_daemon] Hardware executor running.\n");

    jtag::mcp::ExecutorBridge bridge(executor);
    jtag::mcp::McpServer mcp_server({"owltap-jtag-daemon", "0.1.0"});
    mcp_server.setExecutorBridge(&bridge);
    jtag::mcp::tools::registerHardwareTools(mcp_server.registry(), bridge);

    auto mcp_transport = std::make_unique<jtag::mcp::TcpTransport>(mcp_port);
    auto* mcp_transport_ptr = mcp_transport.get();
    mcp_server.setTransport(std::move(mcp_transport));
    mcp_server.start();

    if (!mcp_transport_ptr->isListening()) {
        printErrorEvent("failed to listen on MCP TCP port");
        mcp_server.stop();
        executor.shutdown();
        return 1;
    }

    // -----------------------------------------------------------------------
    // GUI RPC server (Phase 3)
    // -----------------------------------------------------------------------
    std::unique_ptr<jtag::mcp::GuiRpcServer> gui_server;
    if (gui_rpc) {
        gui_server = std::make_unique<jtag::mcp::GuiRpcServer>(gui_port);

        // daemon/status — queries hardware state from the worker thread.
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

        // Delegate hardware methods to registered MCP tool handlers.
        // callToolRaw() skips the MCP content-array wrapper.
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
        {"mcp_port", mcp_transport_ptr->port()},
        {"gui_port", gui_server ? nlohmann::json(gui_server->port())
                                : nlohmann::json(nullptr)},
        {"gui_rpc",  gui_rpc}
    });
    std::fprintf(stderr,
                 "[jtag_daemon] MCP listening on 127.0.0.1:%u\n",
                 mcp_transport_ptr->port());

    while (mcp_server.isRunning() &&
           !g_stop_requested.load(std::memory_order_acquire)) {
        if (exit_on_disconnect &&
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
