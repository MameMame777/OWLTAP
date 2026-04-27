"""
OwlTAP — Hardware-in-the-loop tests for .suite and .ict features
=================================================================
Starts jtag_daemon.exe automatically, exercises the TestSuiteRunner
(.suite) and interconnect-test (.ict) code paths against real hardware.

Usage:
    python test_hw_suite_ict.py [options]

Options:
    --bsdl PATH       BSDL/BSD file for device 0
                      Default: HWsample/xa7z020_clg484.bsd
    --daemon PATH     Path to jtag_daemon.exe
                      Default: bazel-bin/src/tools/jtag_daemon.exe
    --ict PATH        .ict file for interconnect test
                      If omitted a read-only smoke test is generated.

Exit code:
    0  all tests passed
    1  one or more tests failed or daemon startup timed out
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

# ── Configuration ─────────────────────────────────────────────────────────────

SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
BSDL_DEFAULT = os.path.join(SCRIPT_DIR, "HWsample", "xa7z020_clg484.bsd")
DAEMON_DEFAULT = os.path.join(SCRIPT_DIR,
                               "bazel-bin", "src", "tools", "jtag_daemon.exe")
MCP_PORT     = 9998        # use a different port to avoid conflict with test_mcp_full.py
TIMEOUT      = 15.0
STARTUP_WAIT = 10.0

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"
SKIP = "\033[33mSKIP\033[0m"

# ── Daemon lifecycle ───────────────────────────────────────────────────────────

def start_daemon(exe: str, mcp_port: int) -> subprocess.Popen:
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
            pass
    proc.kill()
    raise TimeoutError(f"daemon did not report 'ready' within {STARTUP_WAIT}s")


def stop_daemon(proc: subprocess.Popen):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

# ── MCP framing helpers ────────────────────────────────────────────────────────

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
            "clientInfo": {"name": "test_hw_suite_ict", "version": "1.0.0"},
        },
    })
    resp = recv_one(sock)
    if "error" in resp:
        raise RuntimeError(f"initialize error: {resp['error']}")
    send_frame(sock, {"jsonrpc": "2.0", "method": "notifications/initialized"})
    return resp.get("result", {})


_req_id = 1

def next_id() -> int:
    global _req_id
    val = _req_id
    _req_id += 1
    return val


def tool_call(sock: socket.socket, name: str, args: dict) -> dict:
    send_frame(sock, {
        "jsonrpc": "2.0", "id": next_id(),
        "method": "tools/call",
        "params": {"name": name, "arguments": args},
    })
    resp = recv_one(sock)
    if "error" in resp:
        raise RuntimeError(f"tools/call '{name}' error: {resp['error']}")
    return resp.get("result", {})


def extract_json(result) -> dict:
    if isinstance(result, list):
        for item in result:
            if isinstance(item, dict) and item.get("type") == "text":
                return json.loads(item["text"])
        raise ValueError(f"no text/json in content list: {result}")
    if isinstance(result, dict):
        if "content" in result:
            for item in result["content"]:
                if isinstance(item, dict) and item.get("type") == "text":
                    return json.loads(item["text"])
        return result
    raise ValueError(f"unexpected result type {type(result)}")

# ── .ict file helpers (pure Python re-implementation of parseIctFile) ──────────

def parse_ict_file(path: str) -> list[dict]:
    """
    Parse an .ict file.
    Returns list of dicts: {name, driver:{dev,pin}, receivers:[{dev,pin}]}
    Raises ValueError on parse error.
    """
    nets = []
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#")[0].strip()
            if not line:
                continue
            tokens = line.split()
            if len(tokens) < 3:
                raise ValueError(
                    f"line {lineno}: need at least name, driver, receiver")
            def parse_devpin(tok, ln):
                if ":" not in tok:
                    raise ValueError(f"line {ln}: expected dev:pin, got {tok!r}")
                d, p = tok.split(":", 1)
                return {"dev": int(d), "pin": p.upper()}
            nets.append({
                "name":      tokens[0],
                "driver":    parse_devpin(tokens[1], lineno),
                "receivers": [parse_devpin(t, lineno) for t in tokens[2:]],
            })
    return nets


def parse_suite_file(path: str) -> list[str]:
    """
    Parse a .suite file.
    Returns list of resolved absolute script paths (same logic as TestSuiteRunner::parseSuiteFile).
    """
    suite_dir = os.path.dirname(os.path.abspath(path))
    paths = []
    with open(path) as f:
        for raw in f:
            line = raw.split("#")[0].strip()
            if not line:
                continue
            if not os.path.isabs(line):
                line = os.path.join(suite_dir, line)
            paths.append(line)
    return paths

# ── Setup helpers ─────────────────────────────────────────────────────────────

def setup_detect_load(sock: socket.socket, bsdl_path: str) -> list[str]:
    """Detect devices, load BSDL, return observable pin list."""
    r = extract_json(tool_call(sock, "detect_devices", {}))
    if r.get("device_count", 0) == 0:
        raise RuntimeError("no JTAG devices detected")
    r2 = extract_json(tool_call(sock, "load_bsdl",
                                {"bsdl_path": bsdl_path, "device_index": 0}))
    entity = r2.get("entity", "")
    if not entity:
        raise RuntimeError("BSDL did not load (empty entity name)")
    r3 = extract_json(tool_call(sock, "list_pins", {"device_index": 0}))
    obs = r3.get("observable", [])
    print(f"    entity={entity!r}  observable={len(obs)}  drivable={len(r3.get('drivable',[]))}")
    return obs

# ── .suite test ───────────────────────────────────────────────────────────────

def test_suite_runner(sock: socket.socket, obs_pins: list[str]) -> bool:
    """
    .suite feature hardware test.

    Creates N temporary .script files (one SAMPLE + READ per file) and a
    .suite file listing them.  Parses the .suite file with the Python replica
    of TestSuiteRunner::parseSuiteFile, then runs each script via the MCP
    run_script tool and aggregates pass/fail — mirroring what TestSuiteRunner
    does on the daemon side.

    Expected outcome:
      - .suite file is parsed correctly (N entries returned)
      - Every script returns success=true from the real hardware
      - pass_count == N, fail_count == 0
    """
    SUITE_SCRIPTS = min(3, len(obs_pins))
    if SUITE_SCRIPTS == 0:
        print("    no observable pins available — SKIP")
        return True

    pins = obs_pins[:SUITE_SCRIPTS]

    with tempfile.TemporaryDirectory(prefix="owltap_suite_") as tmpdir:
        # Write one .script file per pin
        script_paths = []
        for i, pin in enumerate(pins):
            script_text = f"# auto-generated test case {i+1}\nsample\nread {pin}\n"
            p = os.path.join(tmpdir, f"test_{i+1}.script")
            with open(p, "w") as f:
                f.write(script_text)
            script_paths.append(p)

        # Write .suite file with relative paths
        suite_path = os.path.join(tmpdir, "hw_test.suite")
        with open(suite_path, "w") as f:
            f.write("# OwlTAP auto-generated hardware suite\n")
            for p in script_paths:
                # Write basename so relative-path resolution is exercised
                f.write(f"{os.path.basename(p)}\n")

        # Parse the .suite file (Python replica of TestSuiteRunner::parseSuiteFile)
        resolved = parse_suite_file(suite_path)
        if len(resolved) != SUITE_SCRIPTS:
            print(f"    parseSuiteFile returned {len(resolved)} "
                  f"entries, expected {SUITE_SCRIPTS}")
            return False
        print(f"    .suite parsed OK: {SUITE_SCRIPTS} scripts")

        # Run each script via the daemon's run_script MCP tool
        pass_count = 0
        fail_count = 0
        for i, (script_path, pin) in enumerate(zip(resolved, pins)):
            with open(script_path) as f:
                script_text = f.read()
            r = extract_json(tool_call(sock, "run_script",
                                       {"device_index": 0, "script": script_text}))
            ok     = r.get("success", False)
            output = r.get("output", "").strip()
            if ok:
                pass_count += 1
                print(f"    [{i+1}/{SUITE_SCRIPTS}] PASS  pin={pin}  output={output!r}")
            else:
                fail_count += 1
                print(f"    [{i+1}/{SUITE_SCRIPTS}] FAIL  pin={pin}  output={output!r}")

        print(f"    suite result: {pass_count}/{SUITE_SCRIPTS} passed")

        # Validation: all scripts must have succeeded
        if fail_count > 0:
            print(f"    UNEXPECTED: {fail_count} script(s) failed on real hardware")
            return False
        if pass_count != SUITE_SCRIPTS:
            print(f"    UNEXPECTED: only {pass_count}/{SUITE_SCRIPTS} scripts ran")
            return False

    return True


# ── .ict test ─────────────────────────────────────────────────────────────────

def test_ict_parse_and_observe(sock: socket.socket,
                               obs_pins: list[str],
                               ict_path: str | None) -> bool:
    """
    .ict feature hardware test (SAMPLE mode — no EXTEST pin driving).

    Without an .ict file:
      A minimal synthetic ICT net is generated from the first three observable
      pins (driver=pin[0], receivers=[pin[1], pin[2]]).  The test verifies
      that the .ict format is parsed correctly and that all receiver pins
      return a valid state (high/low/unknown) via read_pin.

    With an .ict file:
      The supplied file is parsed and the same observation flow runs.

    Expected outcome:
      - .ict file parsed without error
      - Every receiver pin returns high, low, or unknown (not a hard error)
      - All read_pin calls succeed (no RPC error)
    """
    with tempfile.TemporaryDirectory(prefix="owltap_ict_") as tmpdir:
        # ── Build or load the net list ────────────────────────────────────────
        if ict_path is None:
            # Synthesize a minimal ICT file from observable pins
            if len(obs_pins) < 2:
                print("    fewer than 2 observable pins — SKIP")
                return True
            ict_path = os.path.join(tmpdir, "smoke.ict")
            receivers = obs_pins[1:3]   # up to 2 receivers
            with open(ict_path, "w") as f:
                f.write("# Auto-generated smoke ICT file\n")
                rxs = "  ".join(f"0:{r}" for r in receivers)
                f.write(f"NET_A  0:{obs_pins[0]}  {rxs}\n")
            print(f"    generated synthetic .ict: driver=0:{obs_pins[0]} "
                  f"receivers={[f'0:{r}' for r in receivers]}")

        # ── Parse .ict ────────────────────────────────────────────────────────
        try:
            nets = parse_ict_file(ict_path)
        except (ValueError, OSError) as e:
            print(f"    parse_ict_file failed: {e}")
            return False

        if not nets:
            print("    .ict file has no nets — nothing to test")
            return True

        print(f"    .ict parsed OK: {len(nets)} net(s)")

        # ── Observation (SAMPLE mode, always safe) ────────────────────────────
        all_ok = True
        for net in nets:
            print(f"    net '{net['name']}'  driver=0:{net['driver']['pin']}")
            for recv in net["receivers"]:
                if recv["dev"] != 0:
                    # Multi-device ICT: skip devices not yet loaded in this test
                    print(f"      receiver dev{recv['dev']}:{recv['pin']} — SKIP (only dev 0 loaded)")
                    continue
                r = extract_json(tool_call(sock, "read_pin",
                                           {"device_index": recv["dev"],
                                            "pin_name": recv["pin"]}))
                state = r.get("state", "")
                valid = state in ("high", "low", "unknown")
                mark  = "OK" if valid else "ERR"
                print(f"      receiver 0:{recv['pin']} = {state!r}  [{mark}]")
                if not valid:
                    all_ok = False

        return all_ok

# ── Test runner ────────────────────────────────────────────────────────────────

def run_test(label: str, fn) -> bool:
    print(f"\n[TEST] {label}")
    try:
        result = fn()
    except Exception as e:
        print(f"    EXCEPTION: {e}")
        result = False
    mark = PASS if result else FAIL
    print(f"  → {mark}")
    return bool(result)

# ── Main ───────────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(description="OwlTAP .suite/.ict HW test")
    parser.add_argument("--bsdl",   default=BSDL_DEFAULT,  help="BSDL file path")
    parser.add_argument("--daemon", default=DAEMON_DEFAULT, help="jtag_daemon.exe path")
    parser.add_argument("--ict",    default=None,           help=".ict file (optional)")
    args = parser.parse_args()

    print("=" * 60)
    print("OwlTAP .suite / .ict Hardware-in-the-Loop Test")
    print(f"  daemon : {args.daemon}")
    print(f"  bsdl   : {args.bsdl}")
    if args.ict:
        print(f"  ict    : {args.ict}")
    print("=" * 60)

    # ── Start daemon ──────────────────────────────────────────────────
    print("\n[SETUP] Starting jtag_daemon...")
    try:
        proc = start_daemon(args.daemon, MCP_PORT)
    except Exception as e:
        print(f"[FATAL] {e}")
        return 1

    # ── Connect ───────────────────────────────────────────────────────
    print(f"\n[SETUP] Connecting to MCP port {MCP_PORT}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(TIMEOUT)
    try:
        sock.connect(("127.0.0.1", MCP_PORT))
        mcp_initialize(sock)
    except Exception as e:
        print(f"[FATAL] connect/initialize failed: {e}")
        stop_daemon(proc)
        return 1

    # ── Setup: detect + load BSDL ─────────────────────────────────────
    results = []
    obs_pins: list[str] = []

    print("\n[SETUP] Detecting devices and loading BSDL...")
    try:
        obs_pins = setup_detect_load(sock, args.bsdl)
        print(f"  Hardware ready.  {len(obs_pins)} observable pins available.")
    except Exception as e:
        print(f"[FATAL] Hardware setup failed: {e}")
        sock.close()
        stop_daemon(proc)
        return 1

    # ── .suite test ───────────────────────────────────────────────────
    results.append(run_test(
        ".suite: parse + run_script per script file",
        lambda: test_suite_runner(sock, obs_pins),
    ))

    # ── .ict test ─────────────────────────────────────────────────────
    results.append(run_test(
        ".ict: parse + read_pin observation (SAMPLE mode)",
        lambda: test_ict_parse_and_observe(sock, obs_pins, args.ict),
    ))

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
