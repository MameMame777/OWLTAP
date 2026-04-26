"""
Test BSDL loading via GUI RPC (plain JSON-RPC 2.0, no MCP handshake)
Simulates exactly what the GUI does when the user clicks "Load BSDL".
"""
import socket, json, sys

GUI_RPC_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 60294
BSDL_PATH    = sys.argv[2] if len(sys.argv) > 2 else "xa7z020_clg484.bsd"
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
        if len(buf) >= body_start + cl:
            return json.loads(buf[body_start:body_start + cl])

def call(sock, method, params, req_id):
    body = json.dumps({"jsonrpc": "2.0", "id": req_id, "method": method, "params": params})
    frame = f"Content-Length: {len(body)}\r\n\r\n{body}"
    sock.sendall(frame.encode())
    resp = recv_one(sock)
    if "error" in resp:
        raise RuntimeError(f"RPC error: {resp['error']}")
    return resp.get("result", {})

def run():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(TIMEOUT)
    sock.connect(("127.0.0.1", GUI_RPC_PORT))
    print(f"[OK] Connected to GUI RPC port {GUI_RPC_PORT}")

    # Step 1: detect_devices (same as onDaemonConnect does)
    print("\n=== hardware/detect_devices ===")
    r = call(sock, "hardware/detect_devices", {}, 1)
    print(f"[OK] {r}")

    # Step 2: load_bsdl (same as onOpenBsdl -> gui_client_->loadBsdl())
    print(f"\n=== hardware/load_bsdl ({BSDL_PATH}) ===")
    r = call(sock, "hardware/load_bsdl",
             {"device_index": 0, "bsdl_path": BSDL_PATH}, 2)
    print(f"[OK] {r}")

    # Step 3: list_pins (same as onOpenBsdl -> gui_client_->listPins())
    print("\n=== hardware/list_pins ===")
    r = call(sock, "hardware/list_pins", {"device_index": 0}, 3)
    obs = r.get("observable", [])
    drv = r.get("drivable", [])
    print(f"[OK] {len(obs)} observable, {len(drv)} drivable pins")
    print(f"  First 5 obs: {obs[:5]}")

    sock.close()
    print("\n[PASS] Full GUI BSDL load flow succeeded.")

if __name__ == "__main__":
    try:
        run()
    except Exception as e:
        print(f"[FAIL] {e}", file=sys.stderr)
        sys.exit(1)
