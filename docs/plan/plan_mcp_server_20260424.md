# Plan: MCP Server for OwlTAP — 2026-04-24

Expose OwlTAP's JTAG / boundary-scan / capture / script / ILA APIs through an
in-process **MCP (Model Context Protocol)** server. Enables external agents
(Claude Desktop, VS Code Copilot agent) to drive hardware diagnostics.

- Transport: stdio (default) + optional TCP loopback.
- Protocol: JSON-RPC 2.0 / MCP schema (2025-03).
- Concurrency: command queue drained on GUI frame tick — FTDI I/O never
  touched concurrently.
- JSON library: **nlohmann/json** (decided 2026-04-24).
- Long-running operations: **async job model** — `tools/call` returns
  `job_id`, polled via `job_poll` / cancelled via `job_cancel`.

---

## Phase 1 — JSON-RPC + MCP plumbing

**Goal**: A reusable MCP server library with pluggable transports and tool
registry — no OwlTAP domain logic yet.

**Steps**
1. Add `nlohmann_json` to `MODULE.bazel` (BCR: `bazel_dep(name = "nlohmann_json")`).
2. Create `src/mcp/BUILD.bazel` + new files:
   - `json_rpc.{h,cpp}` — request/response/notification types; framer for
     stdio (`Content-Length: N\r\n\r\n<body>`) + fallback line-delimited.
   - `mcp_server.{h,cpp}` — dispatch loop, method routing
     (`initialize`, `tools/list`, `tools/call`, `job_poll`, `job_cancel`,
     `shutdown`).
   - `mcp_transport.{h,cpp}` — `ITransport` interface + `StdioTransport`
     (blocking read on own thread) and `TcpTransport` (127.0.0.1, accept
     single client, spawn reader thread).
   - `tool_registry.{h,cpp}` — `ToolSpec { name, description, input_schema_json,
     handler }`; validate inputs against schema via nlohmann JSON-Schema.
3. Error model: map internal `lastError()` strings into JSON-RPC error codes
   (-32000 range for server errors).

---

## Phase 2 — Command queue bridge to GUI

**Goal**: MCP requests executed on the GUI thread to guarantee single-threaded
FTDI access.

**Steps**
1. `src/mcp/command_queue.{h,cpp}`:
   - `enqueue(std::function<nlohmann::json()>) → std::future<nlohmann::json>`.
   - Producer: MCP transport thread.
   - Consumer: GUI thread calls `drain()` each frame.
2. `McpServer::setExecutor(std::function<std::future<json>(std::function<json()>)>)`.
3. `AppWindow`:
   - Own `CommandQueue command_queue_` + `McpServer server_`.
   - `processMcpCommands()` called from main loop **before** Scanner/Capture
     tick.
   - Background threads: transport reader + dispatcher; shutdown cleanly on
     `AppWindow` destruction.
4. Async job model: jobs stored in `JobTable` keyed by UUID; handler can
   return `{ "job_id": "..." }` immediately and push actual work onto a
   worker (still serialized via CommandQueue for FTDI access).

---

## Phase 3 — Tool handlers

**Goal**: Thin wrappers over existing subsystems. Each handler lives in
`src/mcp/tools/` and is registered from `mcp_tools.cpp`.

**Tool set**

| Tool | Input | Output | Backed by |
|------|-------|--------|-----------|
| `list_devices` | `{}` | `{ devices: [{index, idcode, ir_length, bsdl_loaded}] }` | `JtagChain::devices()` |
| `read_idcode` | `{}` | `{ idcodes: [hex...] }` | `JtagChain::detectDevices()` |
| `load_bsdl` | `{device_index, path}` | `{ok}` | `JtagChain::loadBsdl` |
| `list_pins` | `{device_index}` | `{ pins: [{name, type, bit}] }` | BSDL port iteration |
| `set_pin` | `{device_index, pin, value: 0\|1\|"z"}` | `{ok}` | `PinDriver::setPin` + `applyOutputs` |
| `read_pin` | `{device_index, pin}` | `{value}` | `Scanner::sample` |
| `capture_start` | `{interval_us, depth, trigger?}` | `{job_id}` (async) | `CaptureEngine` |
| `capture_stop` | `{}` | `{ok}` | `CaptureEngine::stop` |
| `get_samples` | `{max?, since_index?}` | `{samples: [...]}` | `CaptureEngine::getSamples` |
| `run_script` | `{text}` | `{job_id}` (async); result via `job_poll` | `script::ScriptEngine::run` |
| `ila_arm` | `{device_index, mask, value, pre_samples}` | `{ok}` | ILA driver (Plan B) |
| `ila_read` | `{device_index}` | `{trigger_index, samples: [u32...]}` | ILA driver (Plan B) |

Stub `ila_arm` / `ila_read` until Plan B Phase 3 lands; return
`"not_available"` error until the driver is wired.

---

## Phase 4 — GUI integration and config

1. `AppConfig` new keys (`app_config.{h,cpp}` + `cfg.json` schema):
   - `mcp.enabled` (bool)
   - `mcp.transport` (`"stdio" | "tcp" | "off"`)
   - `mcp.tcp_port` (int, default 4711)
2. Menu `Tools → MCP Server → {Off | Stdio | TCP}` with active radio.
3. Status indicator in status bar: `MCP: off / stdio / tcp:4711`.
4. Debug log panel: log request method + duration (`DebugLevel::Info`) and
   errors (`DebugLevel::Warning`).

---

## Phase 5 — CLI companion

1. New binary `src/tools/owltap_mcp.cpp`:
   - Opens FTDI device from CLI args (`--vid`, `--pid`, `--interface`).
   - Instantiates `FtdiDevice → TapController → JtagChain → Scanner →
     CaptureEngine`.
   - Runs `McpServer` on stdio transport in the main thread; command queue
     drained on a polling loop at 1 kHz.
   - Entry point for Claude Desktop / VS Code agent integration.
2. Update `src/tools/BUILD.bazel` with `cc_binary` target.

---

## Phase 6 — Tests

1. `test/mcp_server_test.cpp` — golden JSON-RPC sequence using an
   in-memory transport:
   - `initialize` → capability negotiation.
   - `tools/list` → schema shape.
   - `tools/call` with invalid tool → error -32601.
   - `tools/call` with schema violation → error -32602.
2. `test/mcp_tools_test.cpp` — each tool against lightweight mocks in
   `test/support/` (`MockScanner`, `MockCaptureEngine`, `MockJtagChain`).
3. Manual: launch `owltap_mcp.exe`, connect via Claude Desktop
   `~/AppData/Roaming/Claude/claude_desktop_config.json` entry; exercise
   each tool.

---

## Relevant files

**New**
- `src/mcp/BUILD.bazel`
- `src/mcp/{json_rpc,mcp_server,mcp_transport,tool_registry,command_queue,mcp_tools}.{h,cpp}`
- `src/mcp/tools/*.cpp`
- `src/tools/owltap_mcp.cpp`
- `test/mcp_server_test.cpp`, `test/mcp_tools_test.cpp`
- `test/support/{mock_scanner,mock_capture_engine,mock_jtag_chain}.{h,cpp}`

**Modified**
- `MODULE.bazel` (add `nlohmann_json`)
- `src/gui/app_window.{h,cpp}` (command queue integration, menu)
- `src/gui/app_config.{h,cpp}` (persistence)
- `src/tools/BUILD.bazel` (add `owltap_mcp` target)

**Unchanged (reused via public API)**
- `src/capture/capture_engine.h`
- `src/boundary_scan/{scanner,pin_driver}.h`
- `src/jtag/jtag_chain.h`
- `src/script/script_engine.h`

---

## Verification

1. `bazel test //test:mcp_server_test //test:mcp_tools_test` — all green.
2. `bazel run //src/tools:owltap_mcp` — hand-crafted JSON-RPC session
   drives `initialize → tools/list → tools/call list_devices →
   capture_start → job_poll → get_samples`.
3. GUI TCP mode: loopback smoke via `nc 127.0.0.1 4711` with
   newline-delimited JSON.
4. Regression: `bazel test //test:...` green (no changes to existing
   modules).

---

## Scope

**Included**
- stdio + TCP(loopback) transports.
- Boundary-scan read/write, capture, script, ILA hooks.
- Async job model for long operations.

**Excluded (initial release)**
- MCP prompts / resources / sampling features.
- Authentication beyond loopback binding.
- Remote TCP bind paths (127.0.0.1 only).
- Flash programming tool (gated behind a later plan).

---

## Security notes

- TCP transport binds to `127.0.0.1` only. No configurable bind address.
- All MCP-invoked ops execute on the GUI thread; no FTDI race.
- Long-running jobs hold no locks between GUI frames; cancellable via
  `job_cancel`.
