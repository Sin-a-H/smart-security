"""
laptop_cam_server.py - Runs ON THE LAPTOP.
---------------------------------------------------------------

Because the Orange Pi has no camera connector, the laptop's own webcam
is captured here and re-published as a plain MJPEG-over-HTTP stream on
the local network. Both the Orange Pi C web server and the Python person-
detector pull frames from this endpoint with an ordinary HTTP GET.

Endpoints:
  GET /stream.mjpg   -> multipart/x-mixed-replace MJPEG stream
  GET /snapshot.jpg  -> single current JPEG frame
  GET /health        -> simple liveness check (200 OK)

Run:
  python3 laptop_cam_server.py --port 8090 --camera 0 --width 640 --height 480 --fps 15

Then on the Orange Pi side, point the board's stream-source URL at:
  http://<laptop-ip>:8090/stream.mjpg
"""

import argparse
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import cv2

frame_lock = threading.Lock()
latest_jpeg = None
capture_fps = 0.0
stop_event = threading.Event()


def capture_loop(camera_index: int, width: int, height: int, target_fps: int, jpeg_quality: int):
    """Continuously grabs frames from the webcam and keeps the latest JPEG ready."""
    global latest_jpeg, capture_fps

    cap = cv2.VideoCapture(camera_index)
    if not cap.isOpened():
        print(f"[laptop-cam] ERROR: cannot open camera index {camera_index}", file=sys.stderr)
        stop_event.set()
        return

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
    cap.set(cv2.CAP_PROP_FPS, target_fps)

    encode_params = [int(cv2.IMWRITE_JPEG_QUALITY), jpeg_quality]
    min_interval = 1.0 / target_fps if target_fps > 0 else 0
    last_report = time.time()
    frame_count = 0

    print(f"[laptop-cam] capture loop started (cam={camera_index}, "
          f"{width}x{height}@{target_fps}fps target)")

    while not stop_event.is_set():
        loop_start = time.time()
        ok, frame = cap.read()
        if not ok:
            print("[laptop-cam] WARNING: frame grab failed, retrying...", file=sys.stderr)
            time.sleep(0.5)
            continue

        ok, buf = cv2.imencode(".jpg", frame, encode_params)
        if ok:
            with frame_lock:
                latest_jpeg = buf.tobytes()

        frame_count += 1
        now = time.time()
        if now - last_report >= 1.0:
            with frame_lock:
                capture_fps = frame_count / (now - last_report)
            frame_count = 0
            last_report = now

        elapsed = time.time() - loop_start
        if min_interval > elapsed:
            time.sleep(min_interval - elapsed)

    cap.release()
    print("[laptop-cam] capture loop stopped")


class MJPEGHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Quiet default logging; keep only errors visible.
        pass

    def do_GET(self):
        if self.path.startswith("/stream.mjpg"):
            self._serve_stream()
        elif self.path.startswith("/snapshot.jpg"):
            self._serve_snapshot()
        elif self.path.startswith("/health"):
            self._serve_health()
        else:
            self.send_response(404)
            self.end_headers()

    def _serve_stream(self):
        self.send_response(200)
        self.send_header("Age", "0")
        self.send_header("Cache-Control", "no-cache, private")
        self.send_header("Pragma", "no-cache")
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=FRAME")
        self.end_headers()
        try:
            while not stop_event.is_set():
                with frame_lock:
                    frame = latest_jpeg
                if frame is None:
                    time.sleep(0.05)
                    continue
                try:
                    self.wfile.write(b"--FRAME\r\n")
                    self.wfile.write(b"Content-Type: image/jpeg\r\n")
                    self.wfile.write(f"Content-Length: {len(frame)}\r\n\r\n".encode())
                    self.wfile.write(frame)
                    self.wfile.write(b"\r\n")
                except (BrokenPipeError, ConnectionResetError):
                    break
                time.sleep(0.03)
        except Exception as exc:
            print(f"[laptop-cam] stream client disconnected: {exc}", file=sys.stderr)

    def _serve_snapshot(self):
        with frame_lock:
            frame = latest_jpeg
        if frame is None:
            self.send_response(503)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(frame)))
        self.end_headers()
        self.wfile.write(frame)

    def _serve_health(self):
        with frame_lock:
            has_frame = latest_jpeg is not None
            fps = capture_fps
        body = f'{{"ok": {"true" if has_frame else "false"}, "capture_fps": {fps:.2f}}}'.encode()
        self.send_response(200 if has_frame else 503)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def local_ip_hint():
    """Best-effort guess of the laptop's LAN IP, just for the printed hint."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "<laptop-ip>"


def main():
    parser = argparse.ArgumentParser(description="Laptop webcam -> MJPEG HTTP bridge for the Orange Pi board")
    parser.add_argument("--port", type=int, default=8090, help="HTTP port to listen on (default 8090)")
    parser.add_argument("--camera", type=int, default=0, help="OpenCV camera index (default 0)")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--fps", type=int, default=15)
    parser.add_argument("--quality", type=int, default=80, help="JPEG quality 1-100 (default 80)")
    args = parser.parse_args()

    cap_thread = threading.Thread(
        target=capture_loop,
        args=(args.camera, args.width, args.height, args.fps, args.quality),
        daemon=True,
    )
    cap_thread.start()

    time.sleep(1.0)
    if stop_event.is_set():
        print("[laptop-cam] failed to start capture, exiting", file=sys.stderr)
        sys.exit(1)

    server = ThreadingHTTPServer(("0.0.0.0", args.port), MJPEGHandler)
    ip_hint = local_ip_hint()
    print(f"[laptop-cam] serving on all interfaces, port {args.port}")
    print(f"[laptop-cam] on the Orange Pi, use: http://{ip_hint}:{args.port}/stream.mjpg")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        server.shutdown()
        cap_thread.join(timeout=2)


if __name__ == "__main__":
    main()
