/// jtag_daemon - thin wrapper; all logic lives in daemon_runner.cpp.
///
/// Usage:
///   jtag_daemon [--mcp-port <port>] [--gui-port <port>] [--config <path>]
///               [--no-gui-port] [--exit-on-disconnect]

#include "daemon_runner.h"

int main(int argc, char* argv[]) {
    // Skip argv[0] (program name); pass the rest to runDaemon.
    return runDaemon(argc - 1, argv + 1);
}
