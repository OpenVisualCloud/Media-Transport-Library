#!/usr/bin/env python3
"""
Lightweight MJPEG streaming server for POC live preview.

Supports single-file streams and **synchronized pair streams**.
A coordinator thread watches thumb_p.jpg and thumb_r.jpg; when both
update it latches the data and signals all waiting /stream/p and
/stream/r handlers simultaneously — guaranteeing frame-level sync
even over slow links.

Endpoints:
  /stream       — MJPEG stream from thumb.jpg (backwards compatible)
  /stream/p     — Synchronized MJPEG stream for P-path
  /stream/r     — Synchronized MJPEG stream for R-path
  /snapshot/pair— Single binary response with both P + R JPEGs
  /thumb*.jpg   — single latest frame snapshot

Usage:
  python3 mjpeg_server.py [--port 8081] [--dir /dev/shm/poc_thumbs] [--fps 30]
"""

import argparse
import os
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

BOUNDARY = b"--pocframe"


# ── Synchronized frame coordinator ──────────────────────────────────
class FrameCoordinator:
    """Watches P+R thumbnails, latches pairs, signals stream threads."""

    def __init__(self, thumb_dir, target_fps=30):
        self.thumb_dir = thumb_dir
        self.interval = 1.0 / target_fps
        self.lock = threading.Lock()
        self.frame_id = 0
        self.data_p = None
        self.data_r = None
        self.new_frame = threading.Condition(self.lock)
        self._stop = False

    def start(self):
        t = threading.Thread(target=self._run, daemon=True)
        t.start()

    def stop(self):
        self._stop = True

    def _run(self):
        path_p = os.path.join(self.thumb_dir, "thumb_p.jpg")
        path_r = os.path.join(self.thumb_dir, "thumb_r.jpg")
        prev_mtime_p = 0.0
        prev_mtime_r = 0.0

        while not self._stop:
            t0 = time.monotonic()
            try:
                st_p = os.stat(path_p)
                st_r = os.stat(path_r)
                mt_p = st_p.st_mtime
                mt_r = st_r.st_mtime

                if mt_p != prev_mtime_p or mt_r != prev_mtime_r:
                    with open(path_p, "rb") as f:
                        dp = f.read()
                    with open(path_r, "rb") as f:
                        dr = f.read()
                    prev_mtime_p = mt_p
                    prev_mtime_r = mt_r

                    with self.new_frame:
                        self.data_p = dp
                        self.data_r = dr
                        self.frame_id += 1
                        self.new_frame.notify_all()
            except FileNotFoundError:
                pass
            except Exception:
                pass

            elapsed = time.monotonic() - t0
            sleep = self.interval - elapsed
            if sleep > 0:
                time.sleep(sleep)

    def wait_frame(self, last_id, timeout=2.0):
        """Block until a new frame pair is available."""
        with self.new_frame:
            while self.frame_id == last_id:
                if not self.new_frame.wait(timeout=timeout):
                    return last_id, self.data_p, self.data_r
            return self.frame_id, self.data_p, self.data_r


# ── HTTP handler ────────────────────────────────────────────────────
class MJPEGHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        pass

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
        if self.path == "/snapshot/pair":
            self._handle_pair()
        elif self.path in ("/stream/p", "/stream/r"):
            self._handle_synced_stream()
        elif self.path.startswith("/stream"):
            self._handle_stream()
        elif self.path.endswith(".jpg"):
            self._handle_snapshot()
        elif self.path == "/":
            self._handle_index()
        else:
            self.send_error(404)

    def _handle_index(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        self.wfile.write(b'<html><body><img src="/stream"></body></html>\n')

    def _resolve_thumb_file(self):
        if self.path.startswith("/stream"):
            parts = self.path.strip("/").split("/")
            if len(parts) >= 2 and parts[1]:
                return f"thumb_{parts[1]}.jpg"
            return "thumb.jpg"
        return os.path.basename(self.path)

    def _handle_snapshot(self):
        filename = self._resolve_thumb_file()
        path = os.path.join(self.server.thumb_dir, filename)
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
            self.send_error(404, "No thumbnail yet")

    def _handle_pair(self):
        coord = self.server.coordinator
        _, dp, dr = coord.wait_frame(0)
        if dp is None or dr is None:
            self.send_error(404, "Thumbnails not ready")
            return
        body = struct.pack(">I", len(dp)) + dp + dr
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache, no-store")
        self.end_headers()
        self.wfile.write(body)

    def _handle_synced_stream(self):
        """MJPEG stream for /stream/p or /stream/r, synchronized via coordinator.
        Both P and R handlers wake on the SAME frame event → always in sync."""
        which = "p" if self.path.endswith("/p") else "r"
        coord = self.server.coordinator

        self.send_response(200)
        self.send_header(
            "Content-Type", f"multipart/x-mixed-replace; boundary={BOUNDARY.decode()}"
        )
        self.send_header("Cache-Control", "no-cache, no-store")
        self.send_header("Connection", "close")
        self.end_headers()

        last_id = 0
        try:
            while True:
                fid, dp, dr = coord.wait_frame(last_id, timeout=2.0)
                if fid == last_id:
                    continue
                last_id = fid

                data = dp if which == "p" else dr
                if data is None:
                    continue

                self.wfile.write(BOUNDARY + b"\r\n")
                self.wfile.write(b"Content-Type: image/jpeg\r\n")
                self.wfile.write(f"Content-Length: {len(data)}\r\n".encode())
                self.wfile.write(b"\r\n")
                self.wfile.write(data)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def _handle_stream(self):
        """Generic MJPEG stream (backwards compat for /stream)."""
        filename = self._resolve_thumb_file()

        self.send_response(200)
        self.send_header(
            "Content-Type", f"multipart/x-mixed-replace; boundary={BOUNDARY.decode()}"
        )
        self.send_header("Cache-Control", "no-cache, no-store")
        self.send_header("Connection", "close")
        self.end_headers()

        target_interval = 1.0 / self.server.target_fps
        path = os.path.join(self.server.thumb_dir, filename)
        prev_mtime = 0.0

        try:
            while True:
                t0 = time.monotonic()
                try:
                    st = os.stat(path)
                    mtime = st.st_mtime
                    if mtime != prev_mtime:
                        with open(path, "rb") as f:
                            data = f.read()
                        prev_mtime = mtime

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


# ── Server ──────────────────────────────────────────────────────────
class ThreadedHTTPServer(HTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, *args, thumb_dir="/dev/shm/poc_thumbs", target_fps=30, **kwargs):
        super().__init__(*args, **kwargs)
        self.thumb_dir = thumb_dir
        self.target_fps = target_fps
        self.coordinator = FrameCoordinator(thumb_dir, target_fps)
        self.coordinator.start()

    def process_request(self, request, client_address):
        t = threading.Thread(
            target=self.process_request_thread, args=(request, client_address)
        )
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
    parser = argparse.ArgumentParser(description="MJPEG streaming server")
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--dir", type=str, default="/dev/shm/poc_thumbs")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--bind", type=str, default="0.0.0.0")
    args = parser.parse_args()

    server = ThreadedHTTPServer(
        (args.bind, args.port), MJPEGHandler, thumb_dir=args.dir, target_fps=args.fps
    )

    print(
        f"[MJPEG] Streaming on http://{args.bind}:{args.port}/stream "
        f"(dir={args.dir}, fps={args.fps})",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    main()
