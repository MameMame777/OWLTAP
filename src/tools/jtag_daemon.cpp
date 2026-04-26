/// jtag_daemon - Local daemon that owns OwlTAP hardware access.
///
/// Phase 1A exposes the existing MCP tool set over TCP loopback while owning
/// the single HardwareExecutor instance. GUI RPC is intentionally deferred.
///
/// Usage:
///   jtag_daemon [--mcp-port <port>] [--config <path>] [--no-gui-port]

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "src/gui/app_config.h"
#include "src/hardware/hardware_executor.h"
#include "src/mcp/executor_bridge.h"
#include "src/mcp/mcp_server.h"
#include "src/mcp/mcp_transport.h"
#include "src/mcp/tools/register_tools.h"

namespace {

std::atomic<bool> g_stop_requested{false};

void signalHandler(int /*signal*/) {
    g_stop_requested.store(true, std::memory_order_release);
}

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
        "Usage: jtag_daemon [--mcp-port <port>] [--config <path>] [--no-gui-port]\n"
        "  --mcp-port <p>  MCP TCP port on 127.0.0.1 (default 9999, 0 = OS chosen).\n"
        "  --config <p>    cfg.json path (default: cfg.json).\n"
        "  --no-gui-port   Disable GUI RPC endpoint (required in Phase 1A).\n"
        "\n"
        "GUI RPC endpoint options are reserved for a later phase.\n");
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
    std::string config_path = "cfg.json";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mcp-port") == 0 && i + 1 < argc) {
            if (!parsePort(argv[++i], mcp_port)) {
                printErrorEvent(std::string("invalid MCP port: ") + argv[i]);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--no-gui-port") == 0) {
            // accepted and ignored in Phase 1A (GUI RPC not implemented)
        } else if (std::strcmp(argv[i], "--gui-port") == 0) {
            printErrorEvent("GUI RPC endpoint is not implemented in Phase 1A; use --no-gui-port");
            return 1;
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

    printEvent({{"event", "ready"},
                {"mcp_port", mcp_transport_ptr->port()},
                {"gui_port", nullptr},
                {"gui_rpc", false}});
    std::fprintf(stderr,
                 "[jtag_daemon] MCP listening on 127.0.0.1:%u\n",
                 mcp_transport_ptr->port());

    while (mcp_server.isRunning() &&
           !g_stop_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    mcp_server.stop();
    executor.shutdown();
    printEvent({{"event", "stopped"}});
    std::fprintf(stderr, "[jtag_daemon] Exiting.\n");
    return 0;
}
