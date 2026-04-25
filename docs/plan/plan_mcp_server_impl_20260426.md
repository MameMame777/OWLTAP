# Plan: MCP Server Implementation Kickoff — 2026-04-26

Implementation plan that adopts and refines the design in
[plan_mcp_server_20260424.md](plan_mcp_server_20260424.md). This document
captures the actual file paths, reused public APIs, and verification steps
agreed with the user on 2026-04-26.

---

## Context

`jtag_viewer.exe` drives an FT2232H to perform three operations the user
wants AI agents (Claude Desktop, VS Code Copilot agent, Codex CLI) to
invoke directly:

1. **Bitstream programming** — Zynq PL via `PlConfig::program()`
2. **Waveform capture** — boundary-scan SAMPLE loop; existing GUI uses
  `CaptureEngine`, MCP uses the new worker-owned `CaptureSession`
3. **JTAG scan tests** — `Scanner` / `PinDriver` + `TestSuiteRunner` /
   `runInterconnectTest`

The 2026-04-24 design is complete but unimplemented. The user has
confirmed:

- **Both** standalone (`owltap_mcp.exe`) **and** GUI-integrated MCP
  server.
- **Full 12 domain-tool scope** from the original design.
- One gap to patch: the original 12-tool list does **not** include
  `program_bitstream`, even though it's one of the three named user
  operations. We add it as **domain tool 13**.
- MCP clients only expose tools returned by `tools/list`; custom JSON-RPC
  methods are not reliable agent-facing controls. Therefore `job_poll` and
  `job_cancel` are MCP tools as well, for **15 total exposed tools**.
  Flash programming stays out (deferred to a later plan, matching original
  scope).

The 2026-04-25 GUI work — `gui_theme.h`, `buildStatusBar()`, View menu,
Reset Layout — is a clean integration point for Phase 4 and reduces its
effort.

---

## Approach

Adopt [plan_mcp_server_20260424.md](plan_mcp_server_20260424.md) with two
MCP compliance fixes: job operations are exposed as tools, and
`program_bitstream` gets an implementable progress/cancel contract. Refine
Phase 4 to use the new status-bar / View-menu primitives, and Phase 5 to
share an init helper with `owltap_mcp.exe`. Add domain tool 13
(`program_bitstream`) plus two job-control tools to Phase 3.

### Phase 1 — JSON-RPC + MCP plumbing

Create `src/mcp/` library:

- `json_rpc.{h,cpp}` — request / response / notification types; framer
  for stdio (`Content-Length: N\r\n\r\n<body>`) + line-delimited fallback.
- `mcp_server.{h,cpp}` — dispatch loop, method routing for `initialize`,
  `tools/list`, `tools/call`, and `shutdown`. Job operations are regular
  MCP tools registered in Phase 3, not custom JSON-RPC methods.
- `mcp_transport.{h,cpp}` — `ITransport` interface, `StdioTransport`
  (blocking read on own thread), `TcpTransport` (127.0.0.1, single
  client, reader thread).
- `tool_registry.{h,cpp}` — `ToolSpec { name, description,
  input_schema_json, handler }`; validate inputs against schema.
- Error model: map internal `lastError()` strings into JSON-RPC error
  codes (-32000 range for server errors).

Add `bazel_dep(name = "nlohmann_json", version = "3.12.0.bcr.1")` to
[MODULE.bazel](../../MODULE.bazel).

### Phase 2 — Job table and executor bridge

`src/mcp/executor_bridge.{h,cpp}`:

- Converts MCP `tools/call` handlers into `HardwareExecutor` submissions.
- For short synchronous tools, waits for job completion with a bounded timeout.
- For async tools, returns `{job_id}` immediately.

`src/mcp/job_table.{h,cpp}`: UUID-keyed async jobs with status polling,
progress JSON, result JSON, and cancel state. Handlers receive a
`JobContext` containing `job_id`, `isCancelRequested()`, and
`setProgress(json)`.

`McpServer::setExecutorBridge(ExecutorBridge*)` lets the same server run in
GUI-integrated mode or standalone mode while all hardware work goes through
the same serialized executor.

### Phase 2A — HardwareExecutor for serialized FTDI access

Adopt a single `HardwareExecutor` as the owner of all live FTDI/JTAG access.
This replaces the earlier assumption that MCP can safely drain hardware work
from the GUI frame loop. The GUI thread remains responsible for ImGui state;
hardware commands run on one dedicated worker thread.

New library: `src/hardware/`.

- `hardware_executor.{h,cpp}` — owns one worker thread and a FIFO of
  `HardwareTask` closures. Public API:
  `submit(std::function<nlohmann::json(HardwareContext&)>) -> JobHandle`,
  `tryCancel(job_id)`, `poll(job_id)`, `shutdown()`.
- `hardware_context.{h,cpp}` — owns `FtdiDevice`, `TapController`,
  `JtagChain`, and lazily-created per-device `Scanner`, `PinDriver`,
  `CaptureSession`, and `PlConfig` objects. All methods are worker-thread
  only.
- `capture_session.{h,cpp}` — worker-owned boundary-scan capture state
  machine. It reuses `SampleFrame`, `CaptureState`, and `TriggerEngine`, but
  does not create its own thread. Each SAMPLE operation is performed by
  `HardwareExecutor` on the serialized FTDI lane.
- `hardware_job.{h,cpp}` — shared job state: `queued | running | complete |
  failed | cancelled`, progress JSON, result JSON, error string,
  cancellation flag.

Ownership and threading rules:

1. `FtdiDevice`, `TapController`, `JtagChain`, `Scanner`, `PinDriver`, and
   `PlConfig` are constructed and used only inside `HardwareContext` on the
   hardware worker thread.
2. MCP tool handlers never call scanner/chain/config APIs directly. They
   submit a task to `HardwareExecutor` and return either a result or `{job_id}`.
3. GUI actions that touch hardware also submit tasks. GUI widgets consume
   immutable snapshots or completed job results; no ImGui code runs on the
   worker thread.
4. Long-running hardware-exclusive tasks (`program_bitstream`, `run_script`,
   future flash programming) hold the hardware lane until complete or
   cancelled. Other JTAG tools return busy unless they are pure job/status
   tools.
5. MCP/GUI shared capture uses worker-owned `CaptureSession`, not the current
  threaded `CaptureEngine`. `CaptureEngine` may remain for legacy direct GUI
  use during migration, but MCP-integrated capture must not call
  `CaptureEngine::start()` because it spawns a private thread that calls
  `Scanner::sample()` outside the serialized lane.

### Phase 2B — CaptureSession refactor

Add `src/hardware/capture_session.{h,cpp}` as a non-threaded capture engine
for MCP and future GUI capture.

Responsibilities:

- Own capture configuration: `buffer_depth`, `sample_interval_us`, trigger
  configuration, selected device index, and capture mode.
- Own a ring buffer of `SampleFrame` and expose thread-safe snapshot methods
  through `HardwareExecutor` jobs: `samples(max, since_index)`, `latest()`,
  `state()`, `effectiveSampleRate()`.
- Provide `start()`, `stop()`, `configure()`, and `tick(HardwareContext&)`.
  `tick()` performs at most one `Scanner::sample()` when the interval has
  elapsed, evaluates triggers, appends one frame, updates progress/state, and
  returns quickly.
- Preserve existing `CaptureEngine` behavior for modes already used by the
  GUI: `SINGLE`, `FREE_RUN`, `WAITING_TRIGGER`, `TRIGGERED`, `COMPLETE`,
  pre-trigger ratio, post-trigger fill, and trigger-point marking.

Executor integration:

1. `capture_start` creates/configures a `CaptureSession` inside
   `HardwareContext`, marks the hardware lane as `capture_active`, and returns
   `{job_id}`. The job remains running until `capture_stop`, cancellation, or
   single-shot completion.
2. `HardwareExecutor` checks active sessions between normal queued jobs. If a
   capture interval has elapsed, it calls `CaptureSession::tick()` before
   taking the next non-capture hardware job.
3. While capture is active, tools that would change TAP state or BSR outputs
  return a busy error. Allowed tools: `capture_stop`, `get_samples`,
  `job_poll`, `job_cancel`, and pure status queries that do not touch FTDI.
4. `get_samples` copies buffered data from the session without performing FTDI
  I/O. It may run while capture is active.
5. `capture_stop` stops the session on the worker thread and completes the
  capture job with `{state, sample_count, effective_rate_hz}`.

Migration rule:

- Do not modify the existing `CaptureEngine` in this MCP phase except to share
  plain data types if necessary. Avoid a broad GUI capture rewrite until the
  MCP path has validated `CaptureSession` with tests and hardware smoke.

Executor behavior:

- Short operations (`read_idcode`, `read_pin`, `set_pin`) may block the caller
  until their submitted job completes, with a bounded timeout and JSON-RPC
  error on timeout.
- Async operations (`capture_start`, `run_script`, `program_bitstream`) return
  `{job_id}` immediately. `job_poll` and `job_cancel` read/update the same job
  table used by the executor.
- Shutdown order: stop transports, stop accepting new tasks, request cancel on
  running jobs, stop capture/programming if active, then close FTDI by RAII on
  the worker thread.

Rejected alternative: keep FTDI access on the GUI thread and make long
operations cooperative per frame. This is simpler initially but makes capture,
programming, and future flash operations compete with UI responsiveness and is
more fragile for external MCP clients.

### Phase 3 — Tool handlers (15 exposed MCP tools)

Thin wrappers over existing public APIs where possible. One handler per file
under `src/mcp/tools/`. Registered from `mcp_tools.cpp`.

All long-running tools return `{job_id}` from `tools/call`. Agents use the
MCP tools `job_poll` and `job_cancel` to observe or cancel those jobs; no
agent workflow depends on private JSON-RPC methods.

| # | Tool | Backed by |
|---|------|-----------|
| 1 | `list_devices` | `JtagChain::devices()` |
| 2 | `read_idcode` | `JtagChain::detectDevices()` |
| 3 | `load_bsdl` | `JtagChain::loadBsdl()` |
| 4 | `list_pins` | BSDL port iteration |
| 5 | `set_pin` | `PinDriver::setPin` + `applyOutputs` |
| 6 | `read_pin` | `Scanner::sample` |
| 7 | `capture_start` (async) | `CaptureSession::start` |
| 8 | `capture_stop` | `CaptureSession::stop` |
| 9 | `get_samples` | `CaptureSession::samples` |
| 10 | `run_script` (async) | `script::ScriptEngine::run` |
| 11 | `ila_arm` | `IlaDriver` (stub `not_available` until driver wired) |
| 12 | `ila_read` | same |
| **13** | **`program_bitstream`** (async, **new**) | `PlConfig::program()` |
| 14 | `job_poll` | `JobTable::get(job_id)` |
| 15 | `job_cancel` | `JobTable::requestCancel(job_id)` |

`program_bitstream` returns `{job_id}` immediately. Implement it as a
hardware-exclusive job:

1. Extend `PlConfig::program()` with an optional cancellation callback, e.g.
   `bool program(const std::string& path, PlProgressCallback progress_cb,
   PlCancelCallback cancel_cb)`, while preserving the existing two-argument
   overload for current callers.
2. Check cancellation between JTAG programming phases and inside the
   bitstream shift loop. If cancellation is requested before DONE
   verification, return a cancelled job state with the last completed phase.
3. Report progress through `JobContext::setProgress()` as
   `{phase, bytes_done, bytes_total}`; `job_poll` returns
   `{state, progress, result?, error?}`.
4. Submit the programming operation to `HardwareExecutor`; do not run it on
  the GUI thread or MCP transport thread.
5. While a bitstream job is running, other JTAG tools return a busy error
   except `job_poll` and `job_cancel`.

### Phase 4 — GUI integration

1. `AppConfig` keys (persist via existing JSON helpers in
   [src/gui/app_config.cpp](../../src/gui/app_config.cpp)):
   - `mcp.enabled` (bool)
   - `mcp.transport` (`"off" | "stdio" | "tcp"`)
   - `mcp.tcp_port` (int, default 4711)
2. **View menu** (added 2026-04-25): new submenu
   `MCP Server → {Off | Stdio | TCP}` radio.
3. **Status bar** (added 2026-04-25,
   [app_window.cpp `buildStatusBar()`](../../src/gui/app_window.cpp)):
   add a segment showing `MCP: off | stdio | tcp:4711`, colored with
   `theme::kSuccess` / `theme::kMuted` from
   [src/gui/gui_theme.h](../../src/gui/gui_theme.h).
4. Debug log panel: each MCP request logged with method + duration via
   `DebugLogPanel::append()`; errors at warning level.
5. `AppWindow::run()`: poll completed `HardwareExecutor` jobs once per frame
  and apply GUI-visible snapshots before drawing panels. Never execute FTDI
  I/O from the ImGui frame loop.

### Phase 5 — Standalone `owltap_mcp.exe`

`src/tools/owltap_mcp.cpp`. Reuse the init sequence from
[src/tools/jtag_diag.cpp](../../src/tools/jtag_diag.cpp):

- Load `AppConfig::load("cfg.json")`
- `FtdiDevice::open()` → `initMpsse()` → `TapController` → `JtagChain` →
  `detectDevices()`
- Lazy-construct `Scanner` / `PinDriver` / `CaptureSession` per-device on
  first use.
- Run `McpServer` with `StdioTransport` on the main thread and use the same
  `HardwareExecutor` worker model as the GUI build. The CLI main thread only
  pumps stdio transport and job/status responses.

Consider extracting a `BackendBootstrap` helper into `src/tools/` to
dedupe the ~80-line init prologue currently copy-pasted across
`jtag_diag`, `pl_program`, `flash_program`.

Add `cc_binary` for `owltap_mcp` to
[src/tools/BUILD.bazel](../../src/tools/BUILD.bazel) with deps mirroring
`jtag_diag` + `pl_program` (since we expose bitstream programming).

### Phase 6 — Tests

- `test/mcp_server_test.cpp`: golden JSON-RPC sequence over an in-memory
  transport — `initialize` → `tools/list` → invalid tool (-32601) →
  schema violation (-32602); assert `job_poll` and `job_cancel` appear in
  `tools/list`.
- `test/hardware_executor_test.cpp`: FIFO ordering, busy rejection,
  cancellation flag propagation, shutdown while queued/running, and job
  progress/result state transitions.
- `test/capture_session_test.cpp`: non-threaded capture tick behavior,
  trigger transitions, pre/post-trigger fill, sample snapshot ordering, stop,
  cancellation, and busy-policy integration with the executor.
- `test/mcp_tools_test.cpp`: per-tool against mocks.
- `test/support/`: `mock_scanner.{h,cpp}`, `mock_jtag_chain.{h,cpp}`,
  `mock_pl_config.{h,cpp}`.
- Manual smoke: launch `owltap_mcp.exe` with a test JSON-RPC script that
  exercises `read_idcode → load_bsdl → list_pins → set_pin → read_pin →
  capture_start → job_poll → get_samples → program_bitstream → job_poll`.
  Also start a mock long-running bitstream job and verify `job_cancel`
  transitions it to `cancelled`.

---

## Critical files

### New

- [MODULE.bazel](../../MODULE.bazel) — add `nlohmann_json`
- `src/hardware/BUILD.bazel`
- `src/hardware/{hardware_executor,hardware_context,hardware_job,capture_session}.{h,cpp}`
- `src/mcp/BUILD.bazel`
- `src/mcp/{json_rpc,mcp_server,mcp_transport,tool_registry,executor_bridge,job_table,mcp_tools}.{h,cpp}`
- `src/mcp/tools/*.cpp` (15 files: 13 domain tools + job_poll + job_cancel)
- `src/tools/owltap_mcp.cpp` (+ optional `src/tools/backend_bootstrap.{h,cpp}`)
- `test/capture_session_test.cpp`, `test/hardware_executor_test.cpp`,
  `test/mcp_server_test.cpp`, `test/mcp_tools_test.cpp`
- `test/support/{mock_scanner,mock_jtag_chain,mock_pl_config}.{h,cpp}`

### Modified

- [src/gui/app_window.{h,cpp}](../../src/gui/app_window.h) — own
  `HardwareExecutor` + `McpServer`, poll jobs in run loop, status-bar segment,
  View > MCP Server submenu.
- [src/gui/app_config.{h,cpp}](../../src/gui/app_config.h) — `mcp.*` keys.
- [src/tools/BUILD.bazel](../../src/tools/BUILD.bazel) — add `owltap_mcp`
  target.
- [src/config/pl_config.{h,cpp}](../../src/config/pl_config.h) — add an
  optional cancellation callback while preserving existing call sites.

### Existing APIs / compatibility

- [src/jtag/jtag_chain.h](../../src/jtag/jtag_chain.h) —
  `JtagChain::detectDevices`, `loadBsdl`, `devices()`
- [src/boundary_scan/scanner.h](../../src/boundary_scan/scanner.h) —
  `Scanner::sample`, `readIdCode`
- [src/boundary_scan/pin_driver.h](../../src/boundary_scan/pin_driver.h) —
  `PinDriver::setPin`, `applyOutputs`
- [src/capture/capture_engine.h](../../src/capture/capture_engine.h) —
  existing threaded GUI capture remains compatible during migration; MCP uses
  `src/hardware/capture_session.{h,cpp}` instead.
- [src/script/script_engine.h](../../src/script/script_engine.h) —
  `ScriptEngine::run`, `ScriptHost`
- [src/config/pl_config.h](../../src/config/pl_config.h) —
  `PlConfig::program(path, progress_cb)` remains supported; MCP uses the
  new cancellable overload.
- [src/gui/gui_theme.h](../../src/gui/gui_theme.h) — `theme::kSuccess`,
  `theme::kMuted` for status bar segment.

---

## Verification

1. **Unit:** `bazelisk test //test:capture_session_test
  //test:hardware_executor_test //test:mcp_server_test //test:mcp_tools_test`
  — green.
2. **Regression:** `bazelisk test //test:...` — no breakage in existing
   modules.
3. **Standalone smoke:** `bazel-bin/src/tools/owltap_mcp.exe`. Send via
  stdin a JSON-RPC sequence (`initialize` → `tools/list` →
  `tools/call read_idcode` → `tools/call program_bitstream {path: "..."}` →
  `tools/call job_poll {job_id}` until complete → assert DONE bit).
  Hardware-in-loop on Zybo Z7020.
4. **GUI:** launch `jtag_viewer.exe`; toggle View > MCP Server > TCP;
   confirm status bar shows `MCP: tcp:4711` in green; connect with
   `nc 127.0.0.1 4711`; send newline-delimited JSON; observe
   request/duration in the Debug Log panel.
5. **Claude Desktop:** add to
   `%AppData%\Claude\claude_desktop_config.json`:

   ```json
   {
     "mcpServers": {
       "owltap": { "command": "<path>/owltap_mcp.exe" }
     }
   }
   ```

   Restart; confirm tools appear in Claude Desktop's tool list; exercise
   each.

---

## Effort estimate

| Phase | Work | Effort |
|-------|------|--------|
| 1 | JSON-RPC + transports + registry | 1 day |
| 2 | ExecutorBridge + JobTable | 0.5 day |
| 2A | HardwareExecutor + HardwareContext | 1 day |
| 2B | CaptureSession non-threaded capture | 1 day |
| 3 | 15 tool handlers + schemas + cancellable PL programming | 2 days |
| 4 | GUI integration (small thanks to 2026-04-25 primitives) | 0.5 day |
| 5 | `owltap_mcp.cpp` + bootstrap helper | 0.5 day |
| 6 | Tests + mocks + manual smoke | 1 day |

**Total: ~7.5 days** of focused work. Each phase produces a runnable
artifact that can be reviewed before continuing.

---

## Scope reminder

**In:** stdio + TCP(loopback) transports; single-worker HardwareExecutor for
serialized FTDI/JTAG access; worker-owned non-threaded CaptureSession;
MCP-visible job control tools; boundary-scan read/write; capture; script;
cancellable PL bitstream programming; ILA hooks (stubbed); async job model.

**Out (deferred):** SPI flash programming tool; MCP prompts / resources /
sampling features; auth beyond loopback binding; remote TCP bind paths;
`IlaDriver` tool implementation (stub returns `not_available` until
[plan_ila_signal_lanes_20260425.md](plan_ila_signal_lanes_20260425.md)
land lands).

---

## Security notes

- TCP transport binds to `127.0.0.1` only. No configurable bind address.
- All FTDI/JTAG operations execute on the `HardwareExecutor` worker thread;
  GUI, MCP transports, and CLI main code never touch FTDI directly.
- Long-running jobs are visible and controllable through MCP tools
  (`job_poll`, `job_cancel`). `program_bitstream` checks cancellation between
  phases and during payload shifting; other JTAG tools are rejected with a
  busy error while hardware-exclusive jobs are active.
