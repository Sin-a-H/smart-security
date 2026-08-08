#!/usr/bin/env python3
"""
person_detector.py - Human Detection
-----------------------------------------------------------------------

Pulls frames from the laptop's MJPEG stream (same URL the C board also
proxies), runs MobileNet-SSD (Caffe, via OpenCV's DNN module. CPU only)
counts "person" class detections, draws bounding boxes + a counter overlay,
burns in the student ID and live timestamp, measures actual achieved FPS, and:

  1. Serves the annotated stream itself (so you can point a browser or
     VLC at this process directly to sanity check detection quality),
     and
  2. POSTs a small JSON summary {person_count, fps} to the C board's
     /internal/detector-update endpoint after every processed frame,
     which is how g_state.person_count in the C program stays current
     for /api/v1/persons, the dashboard, and MQTT publishing.

It does NOT push annotated video into the C server. the C server's
/api/v1/stream continues to show the RAW laptop feed.

Run:
    python3 person_detector.py \
        --stream-url http://192.168.1.50:8090/stream.mjpg \
        --board-api https://192.168.1.10/internal/detector-update \
        --board-api-insecure \
        --student-id 402102668 \
        --annotated-port 8091

Then:
    - Annotated stream (for your own viewing):
        http://<board-ip>:8091/annotated.mjpg
    - Confirm the board is receiving updates:
        curl -sk https://<board-ip>/api/v1/persons
"""

import argparse
import json
import os
import ssl
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import cv2
import numpy as np

# VOC-20 classes this specific Caffe model was trained on. Index must
# match the training label map exactly -- "person" is class 15 in this
# particular chuanqi305/MobileNet-SSD release.
VOC_CLASSES = [
    "background", "aeroplane", "bicycle", "bird", "boat", "bottle",
    "bus", "car", "cat", "chair", "cow", "diningtable", "dog", "horse",
    "motorbike", "person", "pottedplant", "sheep", "sofa", "train", "tvmonitor",
]
PERSON_CLASS_ID = VOC_CLASSES.index("person")

DETECTOR_INPUT_SIZE = (300, 300)  # required by this network's architecture
DETECTOR_SCALE = 0.007843          # = 1/127.5, matches training preprocessing
DETECTOR_MEAN = (127.5, 127.5, 127.5)

annotated_frame_lock = threading.Lock()
latest_annotated_jpeg = None
watchdog_last_frame_time = time.time()
stop_event = threading.Event()


def open_mjpeg_stream(url: str) -> cv2.VideoCapture:
    """
    OpenCV's VideoCapture can open an http:// MJPEG URL directly via
    its FFmpeg backend, which handles the multipart parsing for us --
    this is the reason the laptop bridge speaks plain MJPEG-over-HTTP
    rather than a custom protocol.
    """
    cap = cv2.VideoCapture(url)
    if not cap.isOpened():
        raise RuntimeError(f"Could not open MJPEG stream at {url}")
    return cap

# def open_mjpeg_stream(url: str) -> cv2.VideoCapture:
#     """Blocks and retries until the stream is successfully opened."""
#     while not stop_event.is_set():
#         cap = cv2.VideoCapture(url)
#         if cap.isOpened():
#             return cap
#         print(f"[detector] Waiting for MJPEG stream at {url}...", file=sys.stderr)
#         time.sleep(2.0)
#     return None


def post_json(url: str, payload: dict, insecure: bool, timeout: float = 2.0):
    """POSTs a small JSON body to the C board's internal endpoint.
    insecure=True skips TLS verification since the board uses a
    self-signed cert (Part 1b) and this is a same-network internal call."""
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST",
                                  headers={"Content-Type": "application/json"})
    ctx = None
    if url.startswith("https://") and insecure:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            return resp.status
    except Exception as exc:
        print(f"[detector] WARNING: failed to POST update to board: {exc}", file=sys.stderr)
        return None


class AnnotatedStreamHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path.startswith("/annotated.mjpg"):
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=FRAME")
            self.end_headers()
            try:
                while not stop_event.is_set():
                    with annotated_frame_lock:
                        frame = latest_annotated_jpeg
                    if frame is None:
                        time.sleep(0.05)
                        continue
                    self.wfile.write(b"--FRAME\r\n")
                    self.wfile.write(b"Content-Type: image/jpeg\r\n")
                    self.wfile.write(f"Content-Length: {len(frame)}\r\n\r\n".encode())
                    self.wfile.write(frame)
                    self.wfile.write(b"\r\n")
                    time.sleep(0.03)
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.send_response(404)
            self.end_headers()


def run_annotated_server(port: int):
    server = ThreadingHTTPServer(("0.0.0.0", port), AnnotatedStreamHandler)
    print(f"[detector] annotated stream available at http://0.0.0.0:{port}/annotated.mjpg")
    server.serve_forever()


def detect_persons(net, frame, confidence_threshold: float):
    """
    Runs one forward pass and returns (person_count, boxes), where
    boxes is a list of (x1, y1, x2, y2, confidence) in pixel coords
    for every detection classified as 'person' above the threshold.
    """
    h, w = frame.shape[:2]
    blob = cv2.dnn.blobFromImage(frame, scalefactor=DETECTOR_SCALE,
                                  size=DETECTOR_INPUT_SIZE, mean=DETECTOR_MEAN, swapRB=False)
    net.setInput(blob)
    detections = net.forward()

    boxes = []
    for i in range(detections.shape[2]):
        confidence = detections[0, 0, i, 2]
        class_id = int(detections[0, 0, i, 1])
        if class_id == PERSON_CLASS_ID and confidence >= confidence_threshold:
            box = detections[0, 0, i, 3:7] * np.array([w, h, w, h])
            x1, y1, x2, y2 = box.astype(int)
            boxes.append((x1, y1, x2, y2, confidence))

    return len(boxes), boxes


def draw_overlay(frame, boxes, person_count, fps, student_id, capture_time):
    """
    Draws bounding boxes + counter (spec: "output should be displayed
    on the stream image with a frame and counter"), and burns in
    student ID + live timestamp + measured FPS.
    """
    now_str = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(capture_time))
    # now_str = time.strftime("%Y-%m-%d %H:%M:%S", capture_time)
    for (x1, y1, x2, y2, conf) in boxes:
        cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)
        label = f"person {conf:.2f}"
        cv2.putText(frame, label, (x1, max(y1 - 8, 12)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1, cv2.LINE_AA)

    # now_str = time.strftime("%Y-%m-%d %H:%M:%S")
    overlay_lines = [
        f"Student: {student_id}",
        f"Time: {now_str}",
        f"Persons: {person_count}",
        f"FPS: {fps:.1f}",
    ]
    y = 24
    for line in overlay_lines:
        cv2.putText(frame, line, (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                    (0, 0, 0), 3, cv2.LINE_AA)  # black outline for readability
        cv2.putText(frame, line, (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                    (255, 255, 255), 1, cv2.LINE_AA)
        y += 22

    return frame


_last_cleanup_time = 0.0


def _cleanup_old_snapshots(snapshot_dir: str, min_age_seconds: float = 90.0, min_interval_seconds: float = 30.0):
    """Deletes snapshot files older than `min_age_seconds`. Also only
    actually scans the directory once every `min_interval_seconds`,
    rather than on every single detection frame
    """
    global _last_cleanup_time
    now = time.time()
    if now - _last_cleanup_time < min_interval_seconds:
        return
    _last_cleanup_time = now

    try:
        for fname in os.listdir(snapshot_dir):
            if not (fname.startswith("detection_") and fname.endswith(".jpg")):
                continue
            fpath = os.path.join(snapshot_dir, fname)
            try:
                age = now - os.path.getmtime(fpath)
                if age > min_age_seconds:
                    os.remove(fpath)
            except OSError:
                pass
    except OSError:
        pass


_throttle_lock = threading.Lock()
_throttle_state = {"throttled": False, "target_fps": 15, "target_width": 640}


def _telemetry_poll_loop(board_api_base: str, insecure: bool, poll_interval: float = 5.0):
    ctx = None
    if insecure and board_api_base.startswith("https://"):
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

    telemetry_url = board_api_base.rsplit("/internal/", 1)[0] + "/api/v1/telemetry"

    while not stop_event.is_set():
        try:
            req = urllib.request.Request(telemetry_url, method="GET")
            with urllib.request.urlopen(req, timeout=3, context=ctx) as resp:
                data = json.loads(resp.read())
            with _throttle_lock:
                _throttle_state["throttled"] = bool(data.get("throttled", False))
                _throttle_state["target_fps"] = int(data.get("target_fps", 15))
        except Exception as exc:
            print(f"[detector] WARNING: telemetry poll failed: {exc}", file=sys.stderr)
        time.sleep(poll_interval)


def main():
    parser = argparse.ArgumentParser(description="MobileNet-SSD person detector for the Smart Security System")
    parser.add_argument("--stream-url", required=True, help="Laptop MJPEG stream URL, e.g. http://192.168.1.50:8090/stream.mjpg")
    parser.add_argument("--board-api", required=True, help="C board's internal update endpoint, e.g. https://192.168.1.10/internal/detector-update")
    parser.add_argument("--board-api-insecure", action="store_true", help="Skip TLS verification for the board's self-signed cert")
    parser.add_argument("--student-id", required=True)
    parser.add_argument("--model-dir", default="model", help="Directory containing MobileNetSSD_deploy.prototxt and .caffemodel")
    parser.add_argument("--confidence", type=float, default=0.5)
    parser.add_argument("--annotated-port", type=int, default=8091)
    parser.add_argument("--resize-width", type=int, default=640, help="Resize incoming frames before inference (Part 3.3.3 lets you vary this)")
    parser.add_argument("--snapshot-dir", default="/opt/smart-security/snapshots",
                         help="Directory (readable by the C board process) to save annotated frames for email attachments")
    args = parser.parse_args()

    prototxt = f"{args.model_dir}/MobileNetSSD_deploy.prototxt"
    caffemodel = f"{args.model_dir}/mobilenet_iter_73000.caffemodel"
    print(f"[detector] loading model: {prototxt}, {caffemodel}")
    net = cv2.dnn.readNetFromCaffe(prototxt, caffemodel)

    os.makedirs(args.snapshot_dir, exist_ok=True)

    cap = open_mjpeg_stream(args.stream_url)

    annotated_thread = threading.Thread(target=run_annotated_server, args=(args.annotated_port,), daemon=True)
    annotated_thread.start()

    telemetry_thread = threading.Thread(
        target=_telemetry_poll_loop, args=(args.board_api, args.board_api_insecure), daemon=True
    )
    telemetry_thread.start()

    global watchdog_last_frame_time, latest_annotated_jpeg

    frame_count = 0
    fps_window_start = time.time()
    measured_fps = 0.0

    print(f"[detector] starting detection loop (confidence >= {args.confidence})")

    try:
        while True:
            ok = cap.grab()
            if not ok:
                print("[detector] WARNING: frame read failed, retrying stream...", file=sys.stderr)
                time.sleep(1.0)
                cap.release()
                cap = open_mjpeg_stream(args.stream_url)
                continue

            drained = 0
            drain_deadline = time.time() + 0.05
            while time.time() < drain_deadline:
                ok2 = cap.grab()
                if not ok2:
                    break
                drained += 1

            ok, frame = cap.retrieve()
            frame_time = time.time()
            if not ok:
                print("[detector] WARNING: frame decode failed, retrying stream...", file=sys.stderr)
                time.sleep(1.0)
                cap.release()
                cap = open_mjpeg_stream(args.stream_url)
                continue

            watchdog_last_frame_time = time.time()

            with _throttle_lock:
                is_throttled = _throttle_state["throttled"]
                effective_target_fps = _throttle_state["target_fps"]

            # When the board's sensor_thread.c has decided the CPU is too hot,
            # actually shrink the frame and cap our own rate. THERMAL_THROTTLE_ON_C /
            # OFF_C in sensor_thread.h define the temperature thresholds.
            effective_resize_width = 320 if is_throttled else args.resize_width

            if args.resize_width and frame.shape[1] != effective_resize_width:
                scale = effective_resize_width / frame.shape[1]
                frame = cv2.resize(frame, (effective_resize_width, int(frame.shape[0] * scale)))

            person_count, boxes = detect_persons(net, frame, args.confidence)

            frame_count += 1
            elapsed = time.time() - fps_window_start
            if elapsed >= 1.0:
                measured_fps = frame_count / elapsed
                frame_count = 0
                fps_window_start = time.time()

            annotated = draw_overlay(frame.copy(), boxes, person_count, measured_fps, args.student_id, frame_time)
            ok, buf = cv2.imencode(".jpg", annotated, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
            if ok:
                with annotated_frame_lock:
                    latest_annotated_jpeg = buf.tobytes()

            update_payload = {"person_count": person_count, "fps": f"{measured_fps:.2f}"}

            if person_count > 0 and ok:
                # Only write a snapshot file when there's actually
                # something to attach. Becuase writing a JPEG to disk on
                # every single frame would wear an SD card on the board.
                snapshot_name = f"detection_{int(time.time() * 1000)}.jpg"
                snapshot_path = os.path.join(args.snapshot_dir, snapshot_name)
                try:
                    with open(snapshot_path, "wb") as f:
                        f.write(buf.tobytes())
                    update_payload["jpeg_path"] = snapshot_path
                    _cleanup_old_snapshots(args.snapshot_dir)
                except OSError as exc:
                    print(f"[detector] WARNING: could not write snapshot: {exc}", file=sys.stderr)

            post_json(args.board_api, update_payload, args.board_api_insecure)

            # Actually cap our own processing rate to the board-reported
            # target_fps when throttled.
            if is_throttled and effective_target_fps > 0:
                min_interval = 1.0 / effective_target_fps
                time.sleep(max(0.0, min_interval - 0.01))

    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        cap.release()


if __name__ == "__main__":
    main()
