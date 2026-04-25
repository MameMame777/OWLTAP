/// owltap_mcp - Standalone MCP server for OwlTAP JTAG toolchain.
///
/// Usage:
///   owltap_mcp [--tcp [port]] [--config <path>]
///
///   --tcp [port]   Use TCP transport on 127.0.0.1 (default port: 4711).
///                  Without --tcp, stdio transport (Content-Length framing) is used.
///   --config <p>   Path to cfg.json (default: cfg.json in CWD).
///
/// The process blocks until the transport is terminated (EOF on stdin for stdio,
/// or transport stop for TCP).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "src/gui/app_config.h"
#include "src/hardware/hardware_executor.h"
#include "src/mcp/executor_bridge.h"
#include "src/mcp/mcp_server.h"
#include "src/mcp/mcp_transport.h"
#include "src/mcp/tools/register_tools.h"

int main(int argc, char* argv[]) {
    bool        use_tcp    = false;
    uint16_t    tcp_port   = 4711;
    std::string config_path = "cfg.json";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tcp") == 0) {
            use_tcp = true;
            // Optional port follows.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const int p = std::atoi(argv[++i]);
                if (p > 0 && p < 65536) {
                    tcp_port = static_cast<uint16_t>(p);
                } else {
                    std::fprintf(stderr, "[ERROR] invalid port: %s\n", argv[i]);
                    return 1;
                }
            }
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 ||
                   std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "Usage: owltap_mcp [--tcp [port]] [--config <path>]\n"
                "  --tcp [port]   TCP transport on 127.0.0.1 (default 4711).\n"
                "  --config <p>   cfg.json path (default: cfg.json).\n"
                "Without --tcp, stdio transport (Content-Length framing) is used.\n");
            return 0;
        } else {
            std::fprintf(stderr, "[ERROR] Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    auto cfg = jtag::gui::AppConfig::load(config_path);
    std::fprintf(stderr,
                 "[owltap_mcp] VID=0x%04X PID=0x%04X serial='%s' ch=%d "
                 "freq=%u Hz\n",
                 cfg.vendor_id, cfg.product_id, cfg.serial.c_str(),
                 cfg.interface_channel, cfg.clock_freq_hz);

    // ── Hardware executor ────────────────────────────────────────────
    jtag::hardware::HardwareExecutor executor(cfg);
    std::string hw_err = executor.start();
    if (!hw_err.empty()) {
        std::fprintf(stderr, "[ERROR] Hardware executor start failed: %s\n",
                     hw_err.c_str());
        return 1;
    }
    std::fprintf(stderr, "[owltap_mcp] Hardware executor running.\n");

    // ── MCP server ───────────────────────────────────────────────────
    jtag::mcp::ExecutorBridge bridge(executor);
    jtag::mcp::McpServer      server({"owltap-mcp", "0.1.0"});

    server.setExecutorBridge(&bridge);
    jtag::mcp::tools::registerHardwareTools(server.registry(), bridge);

    if (use_tcp) {
        std::fprintf(stderr,
                     "[owltap_mcp] Starting TCP transport on 127.0.0.1:%u\n",
                     tcp_port);
        server.setTransport(
            std::make_unique<jtag::mcp::TcpTransport>(tcp_port));
    } else {
        std::fprintf(stderr,
                     "[owltap_mcp] Starting stdio transport.\n");
        server.setTransport(std::make_unique<jtag::mcp::StdioTransport>());
    }

    server.start();

    // Block the main thread until the server stops (transport EOF / shutdown).
    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    executor.shutdown();
    std::fprintf(stderr, "[owltap_mcp] Exiting.\n");
    return 0;
}
