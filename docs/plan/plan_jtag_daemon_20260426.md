# Plan: JTAG Daemon Architecture — 2026-04-26

## Background

OwlTAP currently has two hardware ownership modes:

1. The GUI owns `FtdiDevice`, `TapController`, `JtagChain`, `Scanner`, and related capture objects directly.
2. The MCP server path owns a separate `HardwareExecutor`, which owns its own `HardwareContext` and opens the FTDI device independently.

Real hardware testing showed the expected failure mode: FTDI devices are effectively exclusive-open resources. If the GUI already holds the FTDI handle, starting the MCP executor from the GUI attempts a second open and fails with `FT_DEVICE_NOT_FOUND`.

A temporary fix now releases the GUI handle before starting MCP. That works, but it means the GUI and MCP cannot actively use hardware at the same time.

The long-term fix is to make one process own the FTDI/JTAG stack permanently, and make both the GUI and MCP clients of that process.

## Goal

Introduce a local `jtag_daemon` process that exclusively owns the FTDI device and serializes all JTAG operations through one `HardwareExecutor` worker queue.

The GUI and MCP server become peers:

```text
jtag_daemon.exe
  owns HardwareExecutor
    owns HardwareContext
      owns FtdiDevice / TapController / JtagChain

  MCP endpoint : 127.0.0.1:<mcp_port>
  GUI endpoint : 127.0.0.1:<gui_port>

jtag_viewer.exe
  starts/stops daemon
  connects as GUI client
  renders cached state and results

MCP client / proxy
  connects to daemon MCP endpoint
```

## Key Design Decisions

### Reuse Existing MCP And Hardware Infrastructure

Do not build the daemon from scratch.

The daemon should reuse the existing modules:

- `src/hardware/hardware_executor.{h,cpp}`
- `src/hardware/hardware_context.{h,cpp}`
- `src/hardware/hardware_job.{h,cpp}`
- `src/mcp/mcp_server.{h,cpp}`
- `src/mcp/mcp_transport.{h,cpp}`
- `src/mcp/executor_bridge.{h,cpp}`
- `src/mcp/tools/register_tools.{h,cpp}`
- `src/tools/owltap_mcp.cpp` as the closest existing entry-point pattern

`HardwareExecutor` already provides the correct core property: one hardware worker thread serializes all FTDI/JTAG operations through FIFO jobs.

### FTDI Access Only In The Daemon Worker

No GUI thread, MCP transport thread, device watcher thread, or status thread should directly touch FTDI.

All hardware operations must go through `HardwareExecutor::submit()`.

Cancellation remains out-of-band through `HardwareJob::requestCancel()` and periodic checks inside long-running operations. Cancellation must not be queued behind the job it is trying to cancel.

### Separate GUI And MCP Server Instances

The current `TcpTransport` supports one client at a time per transport, and `McpServer` owns one transport.

Use separate server instances rather than forcing a multi-client transport immediately:

```text
McpServer mcp_server    -> TcpTransport(mcp_port)
GuiRpcServer gui_server -> TcpTransport(gui_port) or dedicated GUI transport

Both share one ExecutorBridge / HardwareExecutor.
```

The MCP endpoint exposes standard MCP `initialize`, `tools/list`, and `tools/call`.

The GUI endpoint should use the same JSON-RPC framing style but expose GUI-oriented methods rather than pretending every GUI operation is an MCP tool.

### Loopback Only

Both endpoints must bind to `127.0.0.1` only. No remote bind address in the first daemon version.

### Prefer Dynamic Ports For GUI-Spawned Daemons

Fixed ports are useful for manual testing, but the GUI-managed daemon should support port `0` so the OS chooses free ports.

Startup handshake:

1. GUI spawns daemon.
2. Daemon opens/listens on loopback ports.
3. Daemon prints a single machine-readable ready line to stdout:

```json
{"event":"ready","gui_port":9998,"mcp_port":9999}
```

4. GUI parses the line and connects.

For manual and MCP testing, explicit `--gui-port` and `--mcp-port` options remain available.

## Proposed Process Model

```text
jtag_viewer.exe
  AppWindow
  DaemonProcessController
  GuiDaemonClient
  UI state cache

jtag_daemon.exe
  main thread
    parse args
    start HardwareExecutor
    start MCP server
    start GUI RPC server
    print ready event
    wait for shutdown

  hardware worker thread
    owned by HardwareExecutor
    only thread allowed to execute FTDI/JTAG operations

  MCP transport thread(s)
    parse MCP JSON-RPC
    submit jobs through ExecutorBridge

  GUI transport thread(s)
    parse GUI JSON-RPC
    submit jobs through ExecutorBridge or query cached daemon state
```

## Final Target State

The final architecture should have exactly one owner for live FTDI/JTAG state:

```text
                +-------------------------------+
                |          jtag_daemon          |
                |-------------------------------|
                | HardwareExecutor              |
                |   HardwareContext             |
                |     FtdiDevice                |
                |     TapController             |
                |     JtagChain                 |
                |     Scanner / PinDriver       |
                |     CaptureSession / ILA      |
                +---------------+---------------+
                                |
                    single FIFO hardware queue
                                |
        +-----------------------+-----------------------+
        |                                               |
  MCP JSON-RPC endpoint                         GUI JSON-RPC endpoint
  127.0.0.1:<mcp_port>                          127.0.0.1:<gui_port>
        |                                               |
  MCP clients / proxy                            jtag_viewer.exe
```

Final-state rules:

- `jtag_viewer.exe` must not open FTDI directly during normal daemon mode.
- MCP and GUI requests must serialize through the same `HardwareExecutor`.
- Long jobs must expose `job_id`, progress, terminal state, error text, and cancellation.
- GUI state is a cache of daemon state, not an independent hardware model.
- Daemon endpoints bind to loopback only.
- A stdio proxy may exist for MCP clients that cannot connect to TCP directly.

Non-goal for the final state: remote network JTAG access. That requires a separate security plan.

## Progress Management

Track implementation with this status table. Update it whenever a phase starts, completes, or changes scope.

| Phase | Status | Exit Criteria |
|-------|--------|---------------|
| 1A MCP-only daemon binary | Done | `//src/tools:jtag_daemon` builds and serves MCP TCP with existing tools |
| 1B daemon lifecycle hardening | Done | Windows console control, `--exit-on-disconnect`, `completedConnections()` counter, all 20 tests pass |
| 2 GUI process controller | Done | `DaemonProcessController` spawns/stops daemon, reads ready event, logs to DebugLog; GUI "MCP Server" menu has Start/Stop Daemon items |
| 3 GUI RPC minimal flow | Done | `GuiRpcServer` (daemon side) + `GuiDaemonClient` (GUI side); detect_devices/load_bsdl/list_pins/read_pin callable via GUI RPC; "Connect (via Daemon)" menu item; all 20 tests pass |
| 4 Capture and ILA migration | Not started | waveform capture and ILA status operate through daemon |
| 5 Remove GUI direct FTDI ownership | Not started | normal GUI mode has no direct `FtdiDevice` ownership |
| 6 stdio proxy and packaging | Not started | packaged binaries support GUI, daemon, and stdio-only MCP clients |

Per-phase progress should include:

- build target status;
- unit/integration/manual test status;
- known limitations;
- next recommended step.

Current implementation starts with Phase 1A only. GUI RPC is deliberately deferred until the daemon-owned hardware path is proven stable.

## GUI RPC API Draft

The GUI API should use JSON-RPC 2.0 with the same `Content-Length` framing already used by MCP transport.

Initial methods:

| Method | Purpose |
|--------|---------|
| `daemon/status` | Return daemon version, hardware state, active job summary |
| `hardware/detect_devices` | Detect JTAG chain and return device list |
| `hardware/load_bsdl` | Load BSDL for a device |
| `hardware/list_pins` | Return observable/drivable pin metadata |
| `hardware/read_pin` | Sample and return one pin |
| `hardware/set_pin` | Stage/apply EXTEST pin output |
| `capture/start` | Start capture job |
| `capture/stop` | Stop capture |
| `capture/get_samples` | Return sample frames, optionally since an index |
| `ila/status` | Read ILA status/config registers |
| `job/poll` | Poll any daemon job |
| `job/cancel` | Request cancellation |
| `daemon/shutdown` | Gracefully stop daemon when GUI owns it |

Events can be added later with either:

- polling (`daemon/status`, `job/poll`, `capture/get_samples`), or
- JSON-RPC notifications from daemon to GUI.

Start with polling to reduce transport complexity.

## MCP Endpoint

The existing MCP tool set should continue to work:

- `read_idcode`
- `detect_devices`
- `load_bsdl`
- `list_devices`
- `list_pins`
- `read_pin`
- `set_pin`
- `capture_start`
- `capture_stop`
- `get_samples`
- `run_script`
- `program_bitstream`
- `read_ila_status`
- `job_poll`
- `job_cancel`

The daemon MCP endpoint should be compatible with the current TCP smoke test behavior.

For clients that only support stdio MCP, add a later `owltap_mcp_proxy.exe`:

```text
stdio MCP client <-> owltap_mcp_proxy.exe <-> TCP <-> jtag_daemon.exe
```

## Device Hotplug Policy

Hotplug support is useful but should not be implemented by letting a watcher thread open or close FTDI directly.

Correct policy:

- watcher may enumerate devices and notice changes;
- open/close/reopen must be submitted to the hardware executor or performed during controlled executor lifecycle transitions;
- device identity must match configured VID/PID/serial/interface channel;
- daemon status should expose `device_absent`, `opening`, `connected`, `error`, and `disconnected` states.

Initial daemon version may skip auto-reopen and require explicit restart/reconnect. That is acceptable for reducing risk.

## Migration Roadmap

### Phase 1A — MCP-Only Daemon Binary

Add `src/tools/jtag_daemon.cpp` as a daemon entry point using the same core path as `owltap_mcp.cpp`.

Requirements:

- Own one `HardwareExecutor`.
- Start MCP TCP endpoint on loopback.
- Register existing MCP hardware tools.
- Support `--config`, `--mcp-port`, and `--no-gui-port`.
- Reject GUI endpoint options until Phase 3 adds the GUI RPC server.
- Print machine-readable ready/error lines to stdout.
- Clean shutdown on MCP `shutdown` or process termination.

Verification:

```powershell
bazelisk build //src/tools:jtag_daemon
.\bazel-bin\src\tools\jtag_daemon.exe --mcp-port 9999 --no-gui-port
powershell -ExecutionPolicy Bypass -File test_mcp.ps1 -Port 9999
```

### Phase 1B — Daemon Lifecycle Hardening

After Phase 1A works on hardware, harden process behavior:

- support dynamic MCP port `0` and report the actual bound port;
- add structured `error` events before non-zero exits;
- add Windows console control handling;
- decide whether daemon should exit when the last client disconnects or remain persistent.

### Phase 2 — GUI Process Controller

Add a small GUI-side process manager.

Responsibilities:

- spawn daemon as child process;
- read stdout ready/error events;
- show daemon status in a Server panel or menu;
- stop child daemon on GUI exit when GUI owns the daemon;
- display daemon stderr/stdout in Debug Log.

At this phase, GUI may still use its existing direct hardware path for normal operation. The goal is process lifecycle only.

### Phase 3 — GUI RPC Client And Minimal Hardware Flow

Add `GuiDaemonClient` and route a minimal flow through daemon:

1. start daemon;
2. detect devices;
3. load BSDL;
4. list pins;
5. sample/read pin.

Keep existing direct GUI hardware path behind a fallback switch until parity is proven.

### Phase 4 — Capture And ILA Migration

Move capture-related GUI operations to daemon RPC:

- `capture/start`
- `capture/stop`
- `capture/get_samples`
- `ila/status`

Use polling first. Add daemon-to-GUI notifications only if polling becomes insufficient.

### Phase 5 — Remove GUI Direct FTDI Ownership

Once parity is reached, remove or disable direct GUI ownership of:

- `FtdiDevice`
- `TapController`
- `JtagChain`
- `Scanner`
- `PinDriver`
- `CaptureEngine`

The GUI should retain only cached display state and RPC client objects.

### Phase 6 — Stdio Proxy And Packaging

Add optional `owltap_mcp_proxy.exe` for stdio-only MCP clients.

Package:

```text
owltap/
  jtag_viewer.exe
  jtag_daemon.exe
  owltap_mcp_proxy.exe   optional
```

## Testing Strategy

Unit tests:

- JSON-RPC framing roundtrip for GUI protocol.
- GUI RPC method validation.
- daemon status serialization.
- process-ready line parser.

Integration tests without real FTDI:

- daemon starts with mock hardware context;
- GUI client can call `daemon/status`;
- MCP client can call `tools/list`;
- concurrent GUI and MCP requests serialize through one executor;
- cancellation reaches a running mock long job.

Manual real-hardware tests:

1. Start daemon manually.
2. Run MCP TCP smoke test: `initialize`, `tools/list`, `read_idcode`.
3. Start GUI and connect to daemon.
4. Load BSDL and sample pins from GUI.
5. While GUI is open, call `read_idcode` from MCP and confirm no FTDI conflict.
6. Start a long job and cancel it from the other client.

Regression:

```powershell
bazelisk test //test/...
bazelisk build //src:jtag_viewer //src/tools:jtag_daemon
```

## Security Notes

- Bind only to `127.0.0.1`.
- Do not expose remote TCP in the initial release.
- Treat file paths received from MCP/GUI as local privileged operations; validate existence and report exact errors.
- Avoid logging secrets or full user environment.
- If future remote access is added, require authentication and explicit opt-in.

## Risks And Mitigations

| Risk | Mitigation |
|------|------------|
| GUI migration is large | Migrate one vertical slice at a time with fallback direct mode |
| Fixed port collision | Support port 0 and stdout ready event |
| Multi-client transport complexity | Use separate server instances sharing one executor |
| Cancellation stuck behind FIFO | Use `HardwareJob::requestCancel()` out-of-band |
| Hotplug races FTDI operations | Keep FTDI open/close inside executor-controlled lifecycle |
| stdio-only MCP clients cannot use TCP daemon | Add `owltap_mcp_proxy.exe` |

## Open Questions

- Should the GUI endpoint reuse `McpServer` mechanics with a different registry, or use a separate lightweight `GuiRpcServer` class?
- Should daemon lifetime default to child-of-GUI, or allow persistent background mode?
- How much GUI state should live in the daemon versus in the GUI cache?
- Should BSDL and XDC paths be loaded by daemon directly, or should GUI send parsed/normalized metadata later?

## Current Recommendation

Proceed with Phase 1 and Phase 2 only after this plan is committed.

Do not immediately replace all GUI hardware calls. First prove that a daemon-owned `HardwareExecutor` can serve both MCP and a minimal GUI RPC client without FTDI conflicts.
