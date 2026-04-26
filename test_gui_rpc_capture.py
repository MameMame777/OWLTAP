"""
End-to-end capture test via GUI RPC.
Tests: detect_devices -> load_bsdl -> capture/start -> poll capture/get_samples
"""
import socket, json, sys, time

GUI_RPC_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 62714
BSDL_PATH    = sys.argv[2] if len(sys.argv) > 2 else r"E:\Nautilus\workspace\c++work\OWLTAP\xa7z020_clg484.bsd"
TIMEOUT      = 10.0

def recv_one(sock):
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
        return json.loads(buf[body_start:body_start + cl])

def call(sock, method, params, req_id):
    body = json.dumps({"jsonrpc": "2.0", "id": req_id, "method": method, "params": params})
    frame = f"Content-Length: {len(body)}\r\n\r\n{body}"
    sock.sendall(frame.encode())
    resp = recv_one(sock)
    if "error" in resp:
        raise RuntimeError(f"RPC error {method}: {resp['error']}")
    return resp.get("result", {})

def run():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(TIMEOUT)
    sock.connect(("127.0.0.1", GUI_RPC_PORT))
    print(f"[OK] Connected to GUI RPC port {GUI_RPC_PORT}")

    # 1. detect_devices
    print("\n=== hardware/detect_devices ===")
    r = call(sock, "hardware/detect_devices", {}, 1)
    print(f"[OK] {r['device_count']} device(s)")

    # 2. load_bsdl
    print(f"\n=== hardware/load_bsdl ===")
    r = call(sock, "hardware/load_bsdl", {"device_index": 0, "bsdl_path": BSDL_PATH}, 2)
    print(f"[OK] entity={r.get('entity')}")

    # 3. capture/start (single mode, small buffer)
    print("\n=== capture/start (single, depth=5) ===")
    t0 = time.time()
    r = call(sock, "capture/start",
             {"device_index": 0, "buffer_depth": 5, "interval_us": 100, "trigger_mode": "single"}, 3)
    if "job_id" not in r:
        raise RuntimeError(f"capture/start did not return job_id: {r}")
    job_id = r["job_id"]
    print(f"[OK] job_id={job_id} ({(time.time()-t0)*1000:.0f}ms)")

    # 4. poll until complete
    print("\n=== capture/get_samples polling ===")
    for i in range(60):  # up to 6s
        time.sleep(0.1)
        r = call(sock, "capture/get_samples", {"job_id": job_id}, 10 + i)
        state = r.get("state", "?")
        progress = r.get("progress", {})
        count = progress.get("count", "?") if isinstance(progress, dict) else "?"
        print(f"  poll {i+1}: state={state} count={count}")
        if state in ("complete", "failed", "cancelled"):
            break
    else:
        raise RuntimeError("capture did not complete in 6s")

    if state != "complete":
        raise RuntimeError(f"capture ended with state={state}")

    result = r.get("result", {})
    samples = result.get("samples", []) if isinstance(result, dict) else []
    print(f"\n[OK] {len(samples)} samples captured. First sample pins: {list(list(samples[0].get('pins', {}).keys())[:3]) if samples else 'none'}")

    sock.close()
    print("\n[PASS] Full capture flow succeeded.")

if __name__ == "__main__":
    try:
        run()
    except Exception as e:
        print(f"[FAIL] {e}", file=sys.stderr)
        import traceback; traceback.print_exc()
        sys.exit(1)
