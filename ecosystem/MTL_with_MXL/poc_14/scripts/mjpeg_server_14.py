#!/usr/bin/env python3
"""
MJPEG streaming server for 14-stream POC live preview.

Serves per-stream MJPEG feeds from JPEG thumbnails written by the
C binaries (thumbnail_14.c → /dev/shm/poc14_thumbs/thumb_{role}_{stream_id}.jpg).

Endpoints:
  /stream/{role}/{id}   — MJPEG stream for a single stream
                           e.g. /stream/tx/1, /stream/sender/5, /stream/rx/10
  /snapshot/{role}/{id}  — Single JPEG snapshot
  /streams              — JSON listing all available streams
  /                     — Simple HTML index

Usage:
  python3 mjpeg_server_14.py [--port 8082] [--dir /dev/shm/poc14_thumbs] [--fps 15]
"""

import argparse
import json
import os
import time
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

BOUNDARY = b"--poc14frame"


class MJPEGHandler16(BaseHTTPRequestHandler):

    # Disable Nagle's algorithm — without this, small HTTP responses are
    # delayed ~500 ms by the Nagle + delayed-ACK interaction.  With it,
    # round-trip drops from ~500 ms to ~1 ms per snapshot.
    disable_nagle_algorithm = True

    def log_message(self, fmt, *args):
        pass  # suppress per-request logs

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()

    def do_GET(self):
        # Strip query string for routing
        path = self.path.split('?', 1)[0]
        if path == "/streams":
            self._handle_stream_list()
        elif path.startswith("/stream/"):
            self._handle_mjpeg_stream()
        elif path.startswith("/snapshot/"):
            self._handle_snapshot()
        elif path == "/":
            self._handle_index()
        else:
            self.send_error(404)

    def _parse_role_id(self):
        """Extract (role, stream_id) from /stream/{role}/{id} or /snapshot/{role}/{id}."""
        # Strip query string if present (e.g. ?t=123456 cache-buster)
        path = self.path.split('?', 1)[0]
        parts = path.strip("/").split("/")
        if len(parts) >= 3:
            role = parts[1]
            try:
                sid = int(parts[2])
                return role, sid
            except ValueError:
                pass
        return None, None

    def _thumb_path(self, role, stream_id):
        return os.path.join(
            self.server.thumb_dir, f"thumb_{role}_{stream_id}.jpg")

    def _handle_stream_list(self):
        """Return JSON list of available streams based on thumb files on disk."""
        streams = []
        thumb_dir = self.server.thumb_dir
        try:
            for fn in sorted(os.listdir(thumb_dir)):
                if fn.startswith("thumb_") and fn.endswith(".jpg"):
                    # thumb_{role}_{id}.jpg
                    parts = fn[6:-4].rsplit("_", 1)
                    if len(parts) == 2:
                        role, sid_str = parts
                        try:
                            sid = int(sid_str)
                            streams.append({"role": role, "id": sid,
                                            "stream_url": f"/stream/{role}/{sid}",
                                            "snapshot_url": f"/snapshot/{role}/{sid}"})
                        except ValueError:
                            pass
        except FileNotFoundError:
            pass

        body = json.dumps(streams, indent=2).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _handle_snapshot(self):
        role, sid = self._parse_role_id()
        if role is None:
            self.send_error(400, "Expected /snapshot/{role}/{id}")
            return

        path = self._thumb_path(role, sid)
        try:
            with open(path, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-cache, no-store")
            self.end_headers()
            self.wfile.write(data)
        except FileNotFoundError:
            self.send_error(404, f"No thumbnail: {path}")

    def _handle_mjpeg_stream(self):
        role, sid = self._parse_role_id()
        if role is None:
            self.send_error(400, "Expected /stream/{role}/{id}")
            return

        self.send_response(200)
        self.send_header("Content-Type",
                         f"multipart/x-mixed-replace; boundary={BOUNDARY.decode()}")
        self.send_header("Cache-Control", "no-cache, no-store")
        self.send_header("Connection", "close")
        self.end_headers()

        target_interval = 1.0 / self.server.target_fps
        path = self._thumb_path(role, sid)
        prev_mtime = 0.0

        try:
            while True:
                t0 = time.monotonic()
                try:
                    st = os.stat(path)
                    if st.st_mtime != prev_mtime:
                        with open(path, "rb") as f:
                            data = f.read()
                        prev_mtime = st.st_mtime

                        self.wfile.write(BOUNDARY + b"\r\n")
                        self.wfile.write(b"Content-Type: image/jpeg\r\n")
                        self.wfile.write(f"Content-Length: {len(data)}\r\n".encode())
                        self.wfile.write(b"\r\n")
                        self.wfile.write(data)
                        self.wfile.write(b"\r\n")
                        self.wfile.flush()
                except FileNotFoundError:
                    pass
                except (BrokenPipeError, ConnectionResetError):
                    break

                elapsed = time.monotonic() - t0
                sleep_time = target_interval - elapsed
                if sleep_time > 0:
                    time.sleep(sleep_time)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def _handle_index(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        html = """<!DOCTYPE html>
<html><head><title>POC 16-Stream Preview</title></head>
<body style="background:#111;color:#eee;font-family:monospace;">
<h2>POC 16-Stream MJPEG Server</h2>
<p>Endpoints:</p>
<ul>
<li><a href="/streams">/streams</a> — JSON list of available streams</li>
<li>/stream/{role}/{id} — MJPEG feed</li>
<li>/snapshot/{role}/{id} — Single JPEG</li>
</ul>
<div id="grid"></div>
<script>
fetch('/streams').then(r=>r.json()).then(list=>{
  const g=document.getElementById('grid');
  list.forEach(s=>{
    const d=document.createElement('div');
    d.style.display='inline-block';d.style.margin='4px';
    d.innerHTML=`<div style="color:#0f0">${s.role}/${s.id}</div>
      <img src="${s.stream_url}" width="320" height="180">`;
    g.appendChild(d);
  });
});
</script>
</body></html>
"""
        self.wfile.write(html.encode())


class ThreadedHTTPServer16(HTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, *args, thumb_dir="/tmp", target_fps=15, **kwargs):
        super().__init__(*args, **kwargs)
        self.thumb_dir = thumb_dir
        self.target_fps = target_fps

    def process_request(self, request, client_address):
        t = threading.Thread(target=self.process_request_thread,
                             args=(request, client_address))
        t.daemon = True
        t.start()

    def process_request_thread(self, request, client_address):
        try:
            self.finish_request(request, client_address)
        except Exception:
            self.handle_error(request, client_address)
        finally:
            self.shutdown_request(request)


def main():
    parser = argparse.ArgumentParser(description="14-stream MJPEG server")
    parser.add_argument("--port", type=int, default=8082)
    parser.add_argument("--dir", type=str, default="/dev/shm/poc14_thumbs",
                        help="Directory with thumb_{role}_{id}.jpg files")
    parser.add_argument("--fps", type=int, default=15)
    parser.add_argument("--bind", type=str, default="0.0.0.0")
    args = parser.parse_args()

    server = ThreadedHTTPServer16(
        (args.bind, args.port), MJPEGHandler16,
        thumb_dir=args.dir, target_fps=args.fps)

    print(f"[MJPEG-16] Streaming on http://{args.bind}:{args.port}/ "
          f"(dir={args.dir}, fps={args.fps})", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    main()
