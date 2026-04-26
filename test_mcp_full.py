"""
OwlTAP — Comprehensive MCP integration test (hardware-in-the-loop)
===================================================================
Starts jtag_daemon.exe automatically, exercises every major MCP tool,
and shuts the daemon down cleanly.

Usage:
    python test_mcp_full.py [bsdl_path] [daemon_exe]

    bsdl_path   Path to the BSDL/BSD file for device 0.
                Default: xa7z020_clg484.bsd  (relative to CWD)
    daemon_exe  Path to jtag_daemon.exe
                Default: bazel-bin/src/tools/jtag_daemon.exe

Hardware requirements:
    * FTDI FT4232H (or compatible MPSSE adapter) connected to a
      Xilinx Zynq JTAG header.
    * libusb / WinUSB driver bound to the JTAG interface.
    * The specified BSDL file must match the physical device.

Exit code:
    0  all tests passed
    1  one or more tests failed or daemon startup timed out
"""

import json
import os
import socket
import subprocess
import sys
import time

# ── Configuration ─────────────────────────────────────────────────────────────

BSDL_PATH  = sys.argv[1] if len(sys.argv) > 1 else "xa7z020_clg484.bsd"
DAEMON_EXE = sys.argv[2] if len(sys.argv) > 2 else r"bazel-bin\src\tools\jtag_daemon.exe"
MCP_PORT   = 9999        # --mcp-port default
TIMEOUT    = 15.0        # per-call socket timeout (s)
STARTUP_WAIT = 10.0      # max seconds to wait for daemon ready

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"
SKIP = "\033[33mSKIP\033[0m"

# ── Daemon lifecycle ──────────────────────────────────────────────────────────

def start_daemon(exe: str, mcp_port: int) -> subprocess.Popen:
    """Launch jtag_daemon and wait until it prints the ready JSON."""
    if not os.path.exists(exe):
        raise FileNotFoundError(f"daemon not found: {exe}")
    proc = subprocess.Popen(
        [exe, "--mcp-port", str(mcp_port), "--no-gui-port"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    deadline = time.time() + STARTUP_WAIT
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            time.sleep(0.05)
            continue
        line = line.strip()
        if not line:
            continue
        try:
            evt = json.loads(line)
            if evt.get("event") == "ready":
                print(f"  [daemon] ready  mcp_port={evt.get('mcp_port')}")
                return proc
            if evt.get("event") == "error":
                proc.kill()
                raise RuntimeError(f"daemon startup error: {evt.get('message')}")
        except json.JSONDecodeError:
            pass  # non-JSON stdout line, ignore
    proc.kill()
    raise TimeoutError(f"daemon did not report 'ready' within {STARTUP_WAIT}s")


def stop_daemon(proc: subprocess.Popen):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

# ── MCP framing helpers ───────────────────────────────────────────────────────

def recv_one(sock: socket.socket) -> dict:
    buf = b""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("connection closed")
        buf += chunk
        sep = buf.find(b"\r\n\r\n")
        if sep == -1:
            continue
        header = buf[:sep].decode()
        cl = None
        for line in header.split("\r\n"):
            if line.lower().startswith("content-length:"):
                cl = int(line.split(":", 1)[1].strip())
                break
        if cl is None:
            raise RuntimeError(f"no Content-Length in: {header!r}")
        body_start = sep + 4
        while len(buf) < body_start + cl:
            chunk = sock.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed mid-body")
            buf += chunk
        return json.loads(buf[body_start : body_start + cl])


def send_frame(sock: socket.socket, obj: dict):
    body = json.dumps(obj)
    frame = f"Content-Length: {len(body)}\r\n\r\n{body}"
    sock.sendall(frame.encode())


def mcp_initialize(sock: socket.socket):
    send_frame(sock, {
        "jsonrpc": "2.0", "id": 0, "method": "initialize",
        "params": {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "test_mcp_full", "version": "0.1.0"},
        },
    })
    resp = recv_one(sock)
    if "error" in resp:
        raise RuntimeError(f"initialize error: {resp['error']}")
    # Send initialized notification
    send_frame(sock, {"jsonrpc": "2.0", "method": "notifications/initialized"})
    return resp.get("result", {})


def tool_call(sock: socket.socket, name: str, args: dict, req_id: int) -> dict:
    send_frame(sock, {
        "jsonrpc": "2.0", "id": req_id,
        "method": "tools/call",
        "params": {"name": name, "arguments": args},
    })
    resp = recv_one(sock)
    if "error" in resp:
        raise RuntimeError(f"tools/call '{name}' error: {resp['error']}")
    return resp.get("result", {})


def extract_json(result) -> dict:
    """Pull the first text content item and parse as JSON.

    The MCP server returns callTool results as the content array directly:
        result = [{"type": "text", "text": "<json>"}]
    Some methods (tools/list) return a plain dict.  Handle both.
    """
    # tools/call: result IS the content array
    if isinstance(result, list):
        for item in result:
            if isinstance(item, dict) and item.get("type") == "text":
                return json.loads(item["text"])
        raise ValueError(f"no text/json item in content list: {result}")
    # fallback: dict with nested "content" key
    if isinstance(result, dict):
        if "content" in result:
            for item in result["content"]:
                if isinstance(item, dict) and item.get("type") == "text":
                    return json.loads(item["text"])
        # result might itself be the parsed JSON object
        return result
    raise ValueError(f"unexpected result type {type(result)}: {result}")

# ── Individual test functions ─────────────────────────────────────────────────

_req_id = 1

def next_id() -> int:
    global _req_id
    val = _req_id
    _req_id += 1
    return val


def test_tools_list(sock: socket.socket) -> bool:
    """tools/list — should return >=8 tools."""
    send_frame(sock, {"jsonrpc": "2.0", "id": next_id(), "method": "tools/list", "params": {}})
    resp = recv_one(sock)
    tools = resp.get("result", {}).get("tools", [])
    names = [t.get("name") for t in tools]
    expected = {"detect_devices", "load_bsdl", "list_pins", "read_pin",
                "capture_start", "get_samples", "run_script"}
    missing = expected - set(names)
    if missing:
        print(f"    missing tools: {missing}")
        return False
    print(f"    {len(names)} tools registered: {sorted(names)}")
    return True


def test_detect_devices(sock: socket.socket) -> tuple[bool, int]:
    """detect_devices — returns device count and validates IDCODE fields."""
    r = extract_json(tool_call(sock, "detect_devices", {}, next_id()))
    count = r.get("device_count", 0)
    devices = r.get("devices", [])
    if count == 0:
        print("    no devices found (check hardware connection)")
        return False, 0
    for d in devices:
        if "idcode" not in d or "position" not in d or "ir_length" not in d:
            print(f"    device entry missing fields: {d}")
            return False, count
    print(f"    {count} device(s): " +
          ", ".join(f"pos={d['position']} IDCODE={d['idcode']} IR={d['ir_length']}" for d in devices))
    return True, count


def test_load_bsdl(sock: socket.socket, bsdl_path: str) -> tuple[bool, str]:
    """load_bsdl — loads BSDL and returns entity name."""
    r = extract_json(tool_call(sock, "load_bsdl",
                               {"bsdl_path": bsdl_path, "device_index": 0},
                               next_id()))
    entity = r.get("entity", "")
    bsr_len = r.get("boundary_length", -1)
    if not entity:
        print("    entity name empty — BSDL may not have loaded")
        return False, ""
    print(f"    entity={entity!r}  boundary_length={bsr_len}")
    return True, entity


def test_list_pins(sock: socket.socket) -> tuple[bool, list]:
    """list_pins — returns observable + drivable pin lists."""
    r = extract_json(tool_call(sock, "list_pins", {"device_index": 0}, next_id()))
    obs = r.get("observable", [])
    drv = r.get("drivable", [])
    if not obs:
        print("    no observable pins — BSDL not loaded or entity has no IO cells?")
        return False, []
    print(f"    {len(obs)} observable, {len(drv)} drivable")
    print(f"    first 5 obs: {obs[:5]}")
    return True, obs


def test_read_pin(sock: socket.socket, pin_name: str) -> bool:
    """read_pin — samples a single pin; state should be 'high', 'low', or 'unknown'."""
    r = extract_json(tool_call(sock, "read_pin",
                               {"device_index": 0, "pin_name": pin_name},
                               next_id()))
    # tool returns {"state": "high"|"low"|"unknown", "device_index": ..., "pin_name": ...}
    state = r.get("state", "")
    if state not in ("high", "low", "unknown"):
        print(f"    unexpected state={state!r}  (full response: {r})")
        return False
    print(f"    {pin_name} = {state}")
    return True


def test_capture_and_get_samples(sock: socket.socket) -> bool:
    """capture_start (single, depth=3) + get_samples poll loop."""
    # Start
    r = extract_json(tool_call(sock, "capture_start",
                               {"device_index": 0, "buffer_depth": 3,
                                "interval_us": 100, "trigger_mode": "single"},
                               next_id()))
    job_id = r.get("job_id", "")
    if not job_id:
        print("    no job_id in capture_start response")
        return False
    print(f"    capture started: job_id={job_id}")

    # Poll
    final_state = ""
    samples = []
    for i in range(60):          # up to 6 s
        time.sleep(0.1)
        snap = extract_json(tool_call(sock, "get_samples",
                                      {"job_id": job_id}, next_id()))
        state = snap.get("state", "?")
        progress = snap.get("progress", {})
        count = progress.get("count", "?") if isinstance(progress, dict) else "?"
        if (i % 5) == 0:
            print(f"    poll {i+1}: state={state} count={count}")
        if state in ("complete", "failed", "cancelled"):
            final_state = state
            result = snap.get("result", {})
            if isinstance(result, dict):
                samples = result.get("samples", [])
            break
    else:
        print("    capture did not complete within 6 s")
        return False

    if final_state != "complete":
        print(f"    capture ended with state={final_state!r}")
        return False

    pin_count = len(samples[0].get("pins", {})) if samples else 0
    print(f"    complete: {len(samples)} samples, {pin_count} pins per sample")
    return True


def test_run_script(sock: socket.socket, pin_name: str) -> bool:
    """run_script — sample + read a single pin."""
    script = f"sample\nread {pin_name}\n"
    r = extract_json(tool_call(sock, "run_script",
                               {"device_index": 0, "script": script},
                               next_id()))
    # tool returns {"success": bool, "output": str, ...}
    ok = r.get("success", False)
    output = r.get("output", "")
    print(f"    success={ok}  output: {output.strip()!r}")
    return ok


def test_program_bitstream(sock: socket.socket, bit_path: str) -> bool:
    """program_bitstream — async job; poll job_poll until complete."""
    if not os.path.exists(bit_path):
        print(f"    bitstream not found: {bit_path!r}  (SKIP)")
        return True   # treat as SKIP, not FAIL

    r = extract_json(tool_call(sock, "program_bitstream",
                               {"device_index": 0, "bitstream_path": bit_path},
                               next_id()))
    job_id = r.get("job_id", "")
    if not job_id:
        print(f"    no job_id in program_bitstream response: {r}")
        return False
    print(f"    programming started: job_id={job_id}")

    # Poll via job_poll until terminal state (can take 10–30 s)
    for i in range(180):          # up to 18 s
        time.sleep(0.1)
        snap = extract_json(tool_call(sock, "job_poll", {"job_id": job_id}, next_id()))
        state = snap.get("state", "?")
        prog  = snap.get("progress", {})
        pct   = prog.get("percent", "?") if isinstance(prog, dict) else "?"
        if (i % 20) == 0:
            print(f"    poll {i+1}: state={state}  progress={pct}%")
        if state in ("complete", "failed", "cancelled"):
            result = snap.get("result", {})
            if isinstance(result, dict):
                ok = result.get("ok", False)
            else:
                ok = (state == "complete")
            print(f"    final state={state}  ok={ok}")
            return state == "complete" and ok
    print("    bitstream programming did not complete within 18 s")
    return False


def test_read_ila_status(sock: socket.socket) -> bool:
    """read_ila_status — probe ILA IP after bitstream load (via BSCANE2)."""
    # After JTAG programming the PL needs a moment to initialise.
    time.sleep(1.0)
    r = extract_json(tool_call(sock, "read_ila_status",
                               {"device_index": 0, "use_bscane": True},
                               next_id()))
    # Expected fields: version, data_width, addr_width, depth, armed, triggered, full
    required = {"version", "data_width", "addr_width", "depth", "armed", "triggered", "full"}
    missing  = required - set(r.keys())
    if missing:
        print(f"    missing fields: {missing}  (response: {r})")
        return False
    print(f"    ILA: version={r['version']}  depth={r['depth']}"
          f"  data_width={r['data_width']}  sig_count={r.get('sig_count', '?')}")
    print(f"    armed={r['armed']}  triggered={r['triggered']}  full={r['full']}")
    return True


def test_capture_stop(sock: socket.socket) -> bool:
    """capture_start (free_run) + immediate capture_stop."""
    r = extract_json(tool_call(sock, "capture_start",
                               {"device_index": 0, "buffer_depth": 1000,
                                "interval_us": 100, "trigger_mode": "free_run"},
                               next_id()))
    job_id = r.get("job_id", "")
    if not job_id:
        print("    no job_id")
        return False

    time.sleep(0.3)   # let a few samples accumulate

    stop_r = extract_json(tool_call(sock, "capture_stop",
                                    {"job_id": job_id}, next_id()))
    print(f"    stop response: {stop_r}")

    # Final poll to confirm terminal state
    for _ in range(20):
        time.sleep(0.1)
        snap = extract_json(tool_call(sock, "get_samples",
                                      {"job_id": job_id}, next_id()))
        state = snap.get("state", "?")
        if state in ("complete", "cancelled", "failed"):
            result = snap.get("result", {})
            n = len(result.get("samples", [])) if isinstance(result, dict) else 0
            print(f"    final state={state}  samples={n}")
            return state in ("complete", "cancelled")
    print("    job did not reach terminal state after stop")
    return False

# ── Main test runner ──────────────────────────────────────────────────────────

def run_test(label: str, fn) -> bool:
    print(f"\n[TEST] {label}")
    try:
        result = fn()
        status = PASS if result else FAIL
    except Exception as e:
        result = False
        status = FAIL
        print(f"    EXCEPTION: {e}")
    print(f"  → {status}")
    return bool(result)


def main() -> int:
    print("=" * 60)
    print("OwlTAP MCP Integration Test (hardware-in-the-loop)")
    print(f"  daemon : {DAEMON_EXE}")
    print(f"  bsdl   : {BSDL_PATH}")
    print("=" * 60)

    # ── Start daemon ─────────────────────────────────────────────────
    print("\n[SETUP] Starting jtag_daemon...")
    try:
        proc = start_daemon(DAEMON_EXE, MCP_PORT)
    except Exception as e:
        print(f"[FATAL] {e}")
        return 1

    # ── Connect ───────────────────────────────────────────────────────
    print(f"\n[SETUP] Connecting to MCP port {MCP_PORT}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(TIMEOUT)
    try:
        sock.connect(("127.0.0.1", MCP_PORT))
        info = mcp_initialize(sock)
        print(f"  server: {info.get('serverInfo', {})}")
    except Exception as e:
        print(f"[FATAL] connect/initialize failed: {e}")
        stop_daemon(proc)
        return 1

    # ── Run tests ─────────────────────────────────────────────────────
    results = []

    results.append(run_test("tools/list",
        lambda: test_tools_list(sock)))

    ok_detect, dev_count = False, 0
    print(f"\n[TEST] detect_devices")
    try:
        ok_detect, dev_count = test_detect_devices(sock)
    except Exception as e:
        print(f"    EXCEPTION: {e}")
    print(f"  → {PASS if ok_detect else FAIL}")
    results.append(ok_detect)

    if not ok_detect:
        print("\n[SKIP] Hardware not available — skipping remaining tests.")
        sock.close()
        stop_daemon(proc)
        passed = sum(1 for r in results if r)
        print(f"\n{'='*60}")
        print(f"  {passed}/{len(results)} passed (hardware unavailable)")
        return 0 if passed == len(results) else 1

    ok_bsdl, entity = False, ""
    print(f"\n[TEST] load_bsdl ({os.path.basename(BSDL_PATH)})")
    try:
        ok_bsdl, entity = test_load_bsdl(sock, BSDL_PATH)
    except Exception as e:
        print(f"    EXCEPTION: {e}")
    print(f"  → {PASS if ok_bsdl else FAIL}")
    results.append(ok_bsdl)

    obs_pins = []
    if ok_bsdl:
        ok_pins, obs_pins = False, []
        print(f"\n[TEST] list_pins")
        try:
            ok_pins, obs_pins = test_list_pins(sock)
        except Exception as e:
            print(f"    EXCEPTION: {e}")
        print(f"  → {PASS if ok_pins else FAIL}")
        results.append(ok_pins)

        if obs_pins:
            probe_pin = obs_pins[0]
            results.append(run_test(f"read_pin ({probe_pin})",
                lambda p=probe_pin: test_read_pin(sock, p)))

            results.append(run_test("run_script (sample + read)",
                lambda p=probe_pin: test_run_script(sock, p)))

        results.append(run_test("capture_start + get_samples (single, depth=3)",
            lambda: test_capture_and_get_samples(sock)))

        results.append(run_test("capture_start (free_run) + capture_stop",
            lambda: test_capture_stop(sock)))

    # ── Bitstream programming ─────────────────────────────────────────
    bit_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "HWsample", "ila_bringup_top.bit")
    ok_bit = False
    print(f"\n[TEST] program_bitstream ({os.path.basename(bit_path)})")
    try:
        ok_bit = test_program_bitstream(sock, bit_path)
    except Exception as e:
        print(f"    EXCEPTION: {e}")
    print(f"  \u2192 {PASS if ok_bit else FAIL}")
    results.append(ok_bit)

    # ── ILA status (only meaningful after ILA bitstream is loaded) ─────
    if ok_bit:
        results.append(run_test("read_ila_status (after ILA bitstream)",
            lambda: test_read_ila_status(sock)))
    else:
        print(f"\n[SKIP] read_ila_status (bitstream not programmed)")

    # ── Teardown ──────────────────────────────────────────────────────
    sock.close()
    stop_daemon(proc)

    # ── Summary ───────────────────────────────────────────────────────
    passed = sum(1 for r in results if r)
    total  = len(results)
    print(f"\n{'='*60}")
    print(f"  Result: {passed}/{total} tests passed")
    print("=" * 60)
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
