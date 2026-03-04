#!/usr/bin/env python3
"""
MTL-MXL POC — Control Server (ST 2022-7 stream toggle)

HTTP API that toggles TX stream paths by writing a control file
(/dev/shm/poc_ctrl.json) which the synthetic_st20_tx process polls at ~1 Hz.

Supported controls:
  - mute_p / mute_r:  Mute individual TX ports (MTL drops all packets
                       in the transmitter — simulates cable cut).
  - corrupt:          Enable odd/even packet corruption on both paths
                       (P drops even pkts, R drops odd pkts — needs both
                       streams for full reconstruction, like ST 2022-7).

Endpoints:
  GET  /api/status                → {"p": bool, "r": bool, "corrupt": bool}
  POST /api/toggle/p              → toggle P mute
  POST /api/toggle/r              → toggle R mute
  POST /api/toggle/corrupt        → toggle corruption
  POST /api/set/p?enabled=0|1     → explicit P mute set
  POST /api/set/r?enabled=0|1     → explicit R mute set
  POST /api/set/corrupt?enabled=0|1  → explicit corruption set

Usage:
  python3 control_server.py --port 8082
"""

import argparse
import json
import os
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

CTRL_FILE = "/dev/shm/poc_ctrl.json"

# ── State ──
# p/r: True = stream enabled (NOT muted), False = stream muted
# corrupt: True = odd/even corruption active
state = {"p": True, "r": True, "corrupt": False}


def read_ctrl_file():
    """Read current state from control file (written by us or synth_tx init)."""
    global state
    try:
        with open(CTRL_FILE, "r") as f:
            data = json.load(f)
        state["p"] = not data.get("mute_p", False)
        state["r"] = not data.get("mute_r", False)
        state["corrupt"] = data.get("corrupt", False)
    except (FileNotFoundError, json.JSONDecodeError):
        pass  # keep defaults


def write_ctrl_file():
    """Write state to control file for synth_tx to poll."""
    data = {
        "mute_p": not state["p"],
        "mute_r": not state["r"],
        "corrupt": state["corrupt"],
    }
    tmp = CTRL_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f)
        f.write("\n")
    os.replace(tmp, CTRL_FILE)  # atomic rename


class ControlHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # suppress default logging

    def _send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/status":
            self._send_json(state)
        else:
            self._send_json({"error": "not found"}, 404)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)

        if path == "/api/toggle/p":
            state["p"] = not state["p"]
            write_ctrl_file()
            log_state_change("P", state["p"])
            self._send_json(state)

        elif path == "/api/toggle/r":
            state["r"] = not state["r"]
            write_ctrl_file()
            log_state_change("R", state["r"])
            self._send_json(state)

        elif path == "/api/toggle/corrupt":
            state["corrupt"] = not state["corrupt"]
            write_ctrl_file()
            print(f"[CTRL] Corruption {'ENABLED' if state['corrupt'] else 'DISABLED'}", flush=True)
            self._send_json(state)

        elif path == "/api/set/p":
            enabled = get_enabled(qs, self)
            if enabled is None:
                return
            state["p"] = enabled
            write_ctrl_file()
            log_state_change("P", state["p"])
            self._send_json(state)

        elif path == "/api/set/r":
            enabled = get_enabled(qs, self)
            if enabled is None:
                return
            state["r"] = enabled
            write_ctrl_file()
            log_state_change("R", state["r"])
            self._send_json(state)

        elif path == "/api/set/corrupt":
            enabled = get_enabled(qs, self)
            if enabled is None:
                return
            state["corrupt"] = enabled
            write_ctrl_file()
            print(f"[CTRL] Corruption {'ENABLED' if state['corrupt'] else 'DISABLED'}", flush=True)
            self._send_json(state)

        else:
            self._send_json({"error": "not found"}, 404)


def get_enabled(qs, handler):
    """Extract 'enabled' param from query string or POST body."""
    val = qs.get("enabled", [None])[0]
    if val is None:
        # Try reading from POST body
        try:
            length = int(handler.headers.get("Content-Length", 0))
            if length:
                body = json.loads(handler.rfile.read(length))
                val = str(body.get("enabled", ""))
        except Exception:
            pass
    if val is None or val == "":
        handler._send_json({"error": "missing 'enabled' parameter"}, 400)
        return None
    return val.lower() in ("1", "true", "yes")


def log_state_change(path_name, enabled):
    label = "ENABLED" if enabled else "DISABLED"
    print(f"[CTRL] Path {path_name} {label}", flush=True)


def main():
    parser = argparse.ArgumentParser(description="ST 2022-7 control server")
    parser.add_argument("--port", type=int, default=8082)
    # Legacy args (ignored — kept for backward compat with run_tx.sh)
    parser.add_argument("--pf-iface", default="")
    parser.add_argument("--vf-p", type=int, default=0)
    parser.add_argument("--vf-r", type=int, default=2)
    args = parser.parse_args()

    # Read any existing control file (synth_tx writes initial state on start)
    read_ctrl_file()
    # Ensure streams are initially enabled
    state["p"] = True
    state["r"] = True
    state["corrupt"] = False
    write_ctrl_file()

    server = HTTPServer(("0.0.0.0", args.port), ControlHandler)
    print(f"[CTRL] ST 2022-7 control server on http://0.0.0.0:{args.port}", flush=True)
    print(f"[CTRL] Toggle mechanism: control file ({CTRL_FILE})", flush=True)
    print(f"[CTRL] Endpoints: /api/status, /api/toggle/{{p,r,corrupt}}, /api/set/{{p,r,corrupt}}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    main()
