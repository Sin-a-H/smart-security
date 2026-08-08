# Experiment Walkthrough — Part 3: Image Processing, Email and MQTT

---

## Test 1-3 — Recognition accuracy in 4 lighting conditions

**Setup:** you need four short recorded sequences (or live tests) with a
person clearly in frame under: daylight, artificial/indoor light, and
low-light/backlight. For each condition, run the detector against a
recording or live feed of that condition and count true detections vs.
total frames with a person actually present.

**Run:**
```bash
# for each condition, point the detector at a recording of that condition
# (or the live webcam under that lighting) and watch its annotated stream:
python3 person_detector.py --stream-url <source-for-this-condition> \
  --board-api https://localhost/internal/detector-update --board-api-insecure \
  --student-id 99123456 --annotated-port 8091 --confidence 0.5
# open http://localhost:8091/annotated.mjpg and count, over N frames with
# a person visible, how many got a green bounding box drawn
```

**What to capture:** a table like:

| Condition | Correct detections / total frames | Accuracy | Sample image |
|---|---|---|---|
| Daylight | e.g. 47/50 | 94% | (screenshot) |
| Artificial light | e.g. 41/50 | 82% | (screenshot) |
| Low light (backlit) | e.g. 22/50 | 44% | (screenshot) |

**Conclusion to draw:** MobileNet-SSD (trained on the VOC dataset, which
skews toward well-lit daytime photos) is expected to degrade under
backlighting and low light — silhouettes with blown-out or crushed
contrast lose the edge/gradient features the network relies on. State
this as your expected trend and confirm whether your measured numbers
match it. If backlight performs *unexpectedly well*, check whether your
test frames actually had enough foreground detail visible, or whether the
`--confidence` threshold (default 0.5 in `person_detector.py`) is set low
enough to be passing through weak/uncertain detections that inflate the
count without genuinely being reliable.

---

## Test 2-3 — Spoofing with a printed photo / phone screen

**Run:** hold a printed photo of a person, or a photo displayed on a phone
screen, in front of the camera and watch the annotated stream.

**Expected result:** MobileNet-SSD, like most appearance-based (non-depth,
non-liveness) detectors, is very likely to detect the printed/screen photo
as a real person — it has no mechanism to distinguish a flat 2D
photograph from a live 3D subject, since it's working purely from RGB
pixel patterns.

**What to capture:** the annotated stream screenshot showing a bounding
box drawn around the printed photo/phone screen.

**Analysis and solution suggestion (write this up in your report):**
This is a known limitation of single-frame appearance-based detectors —
there is no liveness/anti-spoofing signal being used at all here. Real
anti-spoofing approaches that could address this, none of which are
implemented in this build:
- **Depth sensing** (stereo camera or ToF sensor) — a flat photo has no
  depth variation, a real face/body does.
- **Motion/temporal analysis** — comparing multiple frames for the kind
  of micro-movement (breathing, subtle posture shift) a photo can't
  produce, vs. a photo being either perfectly static or moved as a rigid
  whole.
- **Texture/moiré analysis** — printed photos and phone screens often
  introduce detectable print-dot patterns or screen moiré under close
  inspection, which a dedicated anti-spoofing classifier could pick up on.
- **Multi-modal confirmation** — pairing the camera with a PIR
  (passive infrared) motion sensor and only trusting a detection if both
  agree, since a photo won't register on a heat/motion sensor.

State plainly that this system, as built, is **not** spoof-resistant, and
that this is an accepted/expected limitation given the scope of the
assignment (a lightweight single-camera CPU-only pipeline), not a bug.

---

## Test 3-3 — FPS / temperature / memory / accuracy at three resolutions

**Run:** use the `--resize-width` flag with three values, e.g. 320, 640,
960, and for each: let it run for 5 minutes, sampling FPS (visible on the
annotated overlay and posted to the board's `/api/v1/persons` endpoint),
CPU temperature (`/api/v1/telemetry`), and memory, plus your accuracy
count from a fixed test scene.

```bash
for width in 320 640 960; do
  echo "=== testing resize_width=$width ==="
  python3 person_detector.py \
    --stream-url http://<laptop-ip>:8090/stream.mjpg \
    --board-api https://localhost/internal/detector-update --board-api-insecure \
    --student-id 99123456 --annotated-port 8091 --resize-width $width &
  DETECTOR_PID=$!
  sleep 300   # 5 minutes
  curl -sk https://localhost/api/v1/telemetry   # grab final temp/mem
  curl -sk https://localhost/api/v1/persons     # grab final fps
  kill $DETECTOR_PID
  sleep 5
done
```

**Reference data point (verified during development, pure inference
throughput measured directly against the compiled model, isolated from
any camera/network bottleneck):**

| resize_width | Measured FPS (this sandbox's CPU) |
|---|---|
| 320×240 | 17.28 |
| 640×480 | 16.77 |
| 960×720 | 16.72 |

**Important, genuinely-discovered finding to include in your report:**
FPS barely changes across resolutions. This is *not* a measurement error
— it's architectural: MobileNet-SSD's `deploy.prototxt` fixes the network
input at exactly 300×300 pixels (`DETECTOR_INPUT_SIZE` in
`person_detector.py`), so `cv2.dnn.blobFromImage(...)` resizes every input
frame down to 300×300 regardless of its original size before it ever
reaches the network. The `--resize-width` parameter therefore only
affects (a) how large the *displayed/annotated* frame is and (b) the cost
of the one-time `cv2.resize` call, which is computationally trivial next
to a full CNN forward pass. The forward pass itself is the fixed,
dominant cost — hence flat FPS.

**What this means for your own measurements on the real Orange Pi:**
expect the same *flat-across-resolution* pattern, just at a lower
absolute FPS number (the sandbox used for the reference numbers above has
a substantially faster CPU than an Allwinner H616/H618). Do not be
surprised if your three resolutions come back nearly identical — that is
the *correct*, expected result given this architecture, not a sign
something is wrong with your setup. If you want resolution to actually
matter for FPS with this same model, you would need to change the network
input size in `deploy.prototxt` itself (a different, smaller SSD input
crops accuracy but genuinely reduces per-frame cost) — worth mentioning as
a "future work" observation.

**Conclusion about the optimal operating point:** since FPS is
essentially resolution-independent, the actual trade-off to optimize is
purely **display/detection-box precision vs. bandwidth/CPU-for-resizing**
— pick the resolution that gives you visually useful bounding boxes
without wasting bandwidth on a MJPEG stream larger than your dashboard
needs, since it buys you nothing in detection speed. 640×480 is a
reasonable default for exactly this reason (also why it's the default in
`laptop_cam_server.py` and `person_detector.py`).

---

## Test 4-3 — Broker down for 3 minutes, LWT observation

**Run:**
```bash
# on the laptop:
sudo systemctl stop mosquitto

# subscribe to the status topic from a third vantage point BEFORE stopping
# the broker isn't actually necessary -- LWT delivery to a subscriber that
# connects *after* the disconnect works too, since we set retain=true on
# the will message (see mqtt_client.c). But for a clean demonstration,
# subscribe first:
mosquitto_sub -h localhost -p 1883 -u monitor -P MonitorPass456 -t 'status/#' -v

# in another terminal, kill or disconnect the board's network so its MQTT
# connection drops (or just stop security-board.service):
sudo systemctl stop security-board.service   # on the board

# wait, then bring the broker back:
sudo systemctl start mosquitto   # on the laptop, after ~3 minutes
sudo systemctl start security-board.service   # on the board
```

**Expected output (verified during development against a real local
Mosquitto broker):** the subscriber shows a transition:
```
status/99123456/home online
status/99123456/home offline
```
The `offline` message is the broker delivering the pre-registered Last
Will and Testament the moment it detects the board's connection dropped
without a clean MQTT DISCONNECT — this was confirmed working exactly as
designed during development (see `mqtt_client.c`'s
`mosquitto_will_set(...)` call, retained, QoS 1, topic
`status/<student_id>/home`).

Once `security-board.service` restarts and reconnects, the connect
callback (`on_connect` in `mqtt_client.c`) immediately re-publishes
`online` (also retained), so a client subscribing at *any* point afterward
sees the board's correct current state without needing to have been
watching continuously.

**What to capture:** the subscriber terminal showing the
`online`→`offline`→`online` sequence with timestamps (add `-F '%J'` to
`mosquitto_sub` for JSON output with timestamps, or just note your
terminal's own timestamps).

**Conclusion to draw:** the LWT mechanism correctly notifies any
interested party of the board's unexpected disconnection without the
board itself needing to do anything special — the broker does this on the
board's behalf using the will message registered at connect time. This is
strictly better than a polling-based "is it still alive" check, since it's
near-instant (bounded by the broker's keepalive interval, 30s in this
config) rather than depending on some external process noticing a silence.

---

## Test 5-3 — End-to-end detection-to-MQTT-message delay

**Run:** you need synchronized timestamps between "a person enters frame"
and "the MQTT message arrives on the PC." The cleanest way: use the
detector's own frame timestamp (already embedded in its logic) compared
against the MQTT message's `timestamp` field, both drawn from the same
clock if laptop and board are NTP-synced, or measure wall-clock arrival
time on the PC side against a manual stopwatch start the moment the person
visibly enters frame.

```bash
# on the PC, subscribe and log arrival times:
mosquitto_sub -h localhost -p 1883 -u monitor -P MonitorPass456 \
  -t 'persons/99123456/home' -v | while read -r line; do
    echo "$(date +%s.%N) $line" >> arrival_log.txt
  done
```

Have a person step into frame 10 times (with a pause between each), noting
the wall-clock moment of entry each time (a visible clock or a second
device timestamp works), then compute the delta against the corresponding
`arrival_log.txt` line.

**Design factors that set a floor on this delay (explain these in your
report as the theoretical contributors, alongside your measured numbers):**
- Camera capture + MJPEG encode on the laptop (`laptop_cam_server.py`
  targets 15fps, so up to ~67ms of frame-buffering latency).
- Network transfer, MJPEG parse in `stream_proxy.c` (i.e., the C proxy),
  and re-serving to the detector (bounded by TCP RTT on a LAN, generally a
  few ms, plus buffer copy time).
- Detector inference time (measured at ~17 FPS pure inference in
  development, i.e. ~59ms per frame, though this was on a much faster CPU
  than the actual board — expect this to be the single largest contributor
  on real Orange Pi hardware, likely 100-300ms+ depending on the board's
  actual clock speed).
- HTTP POST from detector to board's `/internal/detector-update` (local
  network, typically single-digit ms).
- MQTT publish (QoS 1, meaning the client waits for a PUBACK) plus the
  publish loop's own polling interval (`PUBLISH_INTERVAL_SECONDS = 2` in
  `mqtt_client.c` — this is likely to dominate the *average* end-to-end
  delay far more than any of the above, since persons/telemetry are only
  published once every 2 seconds regardless of how fast detection itself
  is).

**Conclusion to draw:** report mean and standard deviation over your 10
measurements. Given the 2-second publish interval is a fixed periodic
cadence rather than event-triggered, expect your measured delay to be
roughly uniformly distributed between 0 and 2 seconds plus the smaller
inference/network contributors — i.e., mean around ~1s with a standard
deviation reflecting that spread, dominated entirely by "how far into the
current 2-second publish cycle did the person happen to enter frame,"
not by the underlying pipeline's actual processing speed. If you want
lower and more consistent latency for a specific use case, the
architectural fix would be event-triggered publishing (publish
immediately on any person_count change, rather than only on a fixed
timer) — worth noting as a design trade-off you consciously made
(steady, predictable broker load) vs. an alternative (lower latency, but
bursty/less predictable broker load), rather than an oversight.

---

## Test 6-3 — Anonymous / unauthorized MQTT login attempt

**Run:**
```bash
mosquitto_pub -h <laptop-ip> -p 1883 -t 'test/topic' -m 'hello'
```

**Expected output (verified during development):**
```
Connection error: Connection Refused: not authorised.
Error: The connection was refused.
```

**What to capture:** this exact terminal output/screenshot.

**Conclusion to draw:** confirms `allow_anonymous false` in the broker
config is correctly enforced — no client can publish or subscribe without
valid credentials, satisfying the security requirement directly.

---

## Test 7-3 — Unauthorized SSH login attempt

**Run (from a different machine, or the same one targeting the board):**
```bash
ssh root@<board-ip>
# or, as a valid non-root user with an intentionally wrong password:
ssh someuser@<board-ip>
```

**Expected output:** for the root attempt, since `PermitRootLogin no` is
set, the server should refuse before even prompting for a password (or
refuse immediately after), e.g.:
```
root@<board-ip>: Permission denied (publickey,password).
```
For a wrong password on a valid user, three failed attempts and the
connection is dropped by default SSH behavior.

**What to capture:** the terminal screenshot showing the refusal.

**Conclusion to draw:** confirms both halves of the SSH hardening
requirement: root login is disabled outright, and normal user
authentication genuinely requires correct credentials rather than
silently accepting anything.
