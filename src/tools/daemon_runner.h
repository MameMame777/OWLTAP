#pragma once

/// Run the JTAG daemon with the given command-line arguments.
///
/// argc/argv must NOT include the program name (argv[0]):
///   pass (original_argc - consumed, original_argv + consumed).
///
/// Supported flags: --mcp-port, --gui-port, --no-gui-port,
///                  --config, --exit-on-disconnect, --help/-h
///
/// Returns 0 on clean exit, 1 on fatal error.
int runDaemon(int argc, char* argv[]);
