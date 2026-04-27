# Plan: Separate MCP from GUI-started Daemon

**Date:** 2026-04-27  
**Status:** Committed

## Objective

When the GUI starts `jtag_daemon`, MCP should not be exposed.  
MCP is intended for CLI / AI-agent usage with a fixed known port.  
The GUI uses the GUI-RPC path exclusively.

## Changes

### 1. `src/tools/daemon_runner.cpp`
- Add `--no-mcp-port` flag: skips MCP server creation entirely.
- Update usage text.

### 2. `src/gui/daemon_process_controller.cpp`
- When `mcp_port == 0` (GUI-initiated start), pass `--no-mcp-port`
  instead of `--mcp-port 0` in the child process command line.

### 3. `src/gui/app_window.cpp`
- Rename View > "MCP Server" submenu → "Daemon".
- Remove MCP port number display (irrelevant when MCP is disabled).
- Rename "Start Daemon (port 0)" → "Start Daemon".

## Non-Goals
- No new ports, no new tools, no behavior change for CLI users.
- `--mcp-port <N>` with N > 0 continues to work as before.
- No unit test changes needed.

## Files Changed
| File | Change |
|------|--------|
| `src/tools/daemon_runner.cpp` | Add `--no-mcp-port`, update usage |
| `src/gui/daemon_process_controller.cpp` | Pass `--no-mcp-port` when `mcp_port==0` |
| `src/gui/app_window.cpp` | Menu label/content cleanup |
