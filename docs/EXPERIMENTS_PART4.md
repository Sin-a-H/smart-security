# Experiment Walkthrough — Part 4: Additional Features

---

## Test 1-4 — Watchdog mode operation (anti-theft / guard mode)

**Run:**
```bash
# 1. Enable guard mode via the dashboard toggle, or directly:
curl -sk -X POST https://localhost/api/v1/guard-mode \
  -H "Content-Type: application/json" -d '{"enabled": 1}'

# 2. Step into frame so the detector reports person_count > 0.

# 3. Watch for the instant email and the MQTT alarm:
mosquitto_sub -h <laptop-ip> -p 1883 -u monitor -P MonitorPass456 -t 'alarm/#' -v
```

**Expected output (verified during development):**
The alarm topic receives a message immediately on detection, e.g.:
```
alarm/99123456/home {"alarm":true,"person_count":1,"timestamp":"2026-07-19T12:00:31"}
```
and an email arrives (subject `[ALARM] Guard mode: N person(s) detected`)
with the detected count, timestamp, and — once the detector's snapshot
attachment is wired up and a real frame is available — a JPEG attachment
of the moment of detection.

**What to capture:** the MQTT subscriber terminal showing the alarm
message, and the received email (screenshot of the inbox).

**Conclusion to draw:** confirm the alarm fires the *instant* a detection
occurs while guard mode is active (not waiting for the normal 2-second
telemetry publish cycle — the alarm publish in `api_handlers.c` happens
synchronously inside the same request that updates `person_count`, unlike
the periodic `persons`/`telemetry` topics which are on a timer in
`mqtt_client.c`). Also confirm guard mode being *off* does **not** trigger
the alarm topic or the guard-specific email for the same detection event
(toggle it off and re-trigger a detection to show the contrast) — this
demonstrates the mode switch genuinely changes behavior rather than the
alarm always firing regardless of the toggle.

---

## Test 2-4 — Black box operation

**Run:**
```bash
# trigger a few detections first (walk in front of the camera a few times,
# or manually POST for a controlled test):
curl -sk -X POST https://localhost/internal/detector-update \
  -H "Content-Type: application/json" -d '{"person_count": 2, "fps": "10.0"}'

# then inspect the database directly:
sqlite3 -header -column /var/lib/smart-security/blackbox.db \
  "SELECT * FROM detections ORDER BY id DESC LIMIT 10;"

sqlite3 -header -column /var/lib/smart-security/blackbox.db \
  "SELECT * FROM lifetime_stats;"

# and cross-check against the API:
curl -sk https://localhost/api/v1/detections/total
```

**Expected output (verified during development with the exact schema
shipped in `blackbox.c`):**
```
id  timestamp   person_count  cpu_temp_c
--  ----------  ------------  ----------
1   1784461683  2             0.0
2   1784461683  4             0.0

id  total_detections
--  ----------------
1   6

{"total_detections":6}
```
(Your actual `cpu_temp_c` values will be real board temperatures, not
0.0 — that placeholder appeared during sandbox testing where no real
thermal sensor was present; on the Orange Pi this column will show
genuine readings from `sysinfo_read_cpu_temp`.)

**What to capture:** a screenshot of both `sqlite3` query outputs
side-by-side with the `/api/v1/detections/total` API response, showing
they agree exactly.

**Conclusion to draw:** the API and the on-disk database are consistent
because the API endpoint reads directly from SQLite (`blackbox_get_total_detections()`
in `api_handlers.c`) rather than maintaining a separate in-memory count
that could drift out of sync — confirm this by noting the numbers match
to the exact integer. Also demonstrate persistence across a restart:
```bash
sudo systemctl restart security-board.service
curl -sk https://localhost/api/v1/detections/total   # should show the SAME total as before the restart
```
This is the actual point of a "black box" — it must survive the process
(and ideally a reboot) that recorded the data, unlike the small in-memory
5-record ring buffer from Part 2 which intentionally resets on restart.

---

## Test 3-4 — Software watchdog operation

**Run:** physically unplug the laptop's webcam (or stop
`laptop_cam_server.py`), then watch the board's logs.

```bash
journalctl -u person-detector -u security-board -f
```

**Expected output (verified during development, using a shortened 5-second
threshold for practical testing rather than waiting the real 30 seconds
— see below for how to reproduce this exact test):**
```
[watchdog] no frame for 6s -- camera tampering alert + restarting detector
```
followed by an email (subject `[Security] Camera/watchdog alert`) with
body `Software watchdog triggered. Reason: No frame received for 6
seconds (threshold 5 s). Possible camera disconnection or tampering.`,
and `person-detector.service` being restarted via
`systemctl restart person-detector.service` (triggered by the watchdog
thread itself through the sudoers-scoped command, not manually).

**To reproduce the shortened-threshold version for a faster development
test** (verified working during development), the watchdog's threshold
and check interval are overridable via environment variables without
recompiling:
```bash
# add to security-board.service's [Service] section temporarily:
Environment=SECURITY_WATCHDOG_THRESHOLD_S=5
Environment=SECURITY_WATCHDOG_CHECK_INTERVAL_S=2
sudo systemctl daemon-reload
sudo systemctl restart security-board.service
```
Remove these overrides before your final grading run so the real 30-second
threshold from the spec is in effect — the shortened version is only for
convenient iteration during development, which is exactly how it was used
during this project's own testing.

**What to capture:** the journalctl log showing the watchdog firing, the
email received, and — after re-plugging the webcam — confirmation that
`person-detector.service` is running again and producing fresh detections
(`systemctl status person-detector` showing recent start time, plus
`/api/v1/persons` timestamp advancing again).

**Conclusion to draw:** the watchdog correctly distinguishes "no new
frames arriving" (a stall) from "zero people detected" (a valid, common
steady state) — it keys off `last_frame_time`, which is updated by
*every* detector POST regardless of `person_count`'s value (see
`api_handle_detector_update` in `api_handlers.c`), so an empty room for
30+ seconds does **not** falsely trigger the watchdog; only a genuine
absence of *any* update does. Confirm this distinction explicitly in your
report by showing a period of `person_count: 0` that does *not* trigger
the watchdog, contrasted with the stall test that does.

---

## Test 4-4 — Adaptive thermal management operation

**Run:** artificially load the CPU to push the temperature past the
70°C threshold (`THERMAL_THROTTLE_ON_C` in `sensor_thread.h`):

```bash
sudo apt install -y stress-ng
stress-ng --cpu $(nproc) --timeout 120s --metrics-brief
```

(`--cpu $(nproc)` spins up one CPU-bound worker per core — on the Orange
Pi Zero 2/3's 4-core Cortex-A53, this is `--cpu 4`; verified during
development that `stress-ng --cpu <N> --timeout <T>s --metrics-brief` runs
cleanly and reports bogo-ops/sec throughput stats at the end, useful as
supporting evidence that real sustained load was applied.)

While that's running, poll telemetry every couple seconds:
```bash
while true; do curl -sk https://localhost/api/v1/telemetry; echo; sleep 2; done
```

**Expected output:** once `cpu_temp_c` crosses 70.0, the next telemetry
poll should show:
```json
{"...", "cpu_temp_c": 71.2, "throttled": true, "target_fps": 8, ...}
```
and the journalctl log should show:
```
[sensor] thermal threshold exceeded (71.2 C) -> throttling to 8fps 320x240
```
followed by an email (subject `[Security] Thermal throttling engaged
(71.2 C)`). Once `stress-ng` finishes and the board cools back down below
60°C (`THERMAL_THROTTLE_OFF_C`, the hysteresis floor), `throttled` should
flip back to `false` and `target_fps`/resolution back to the normal
15fps/640×480 defaults.

**What to capture:** a temperature-vs-time graph spanning the stress test
(rising past 70°C, plateauing, then falling back below 60°C after
`stress-ng` ends), annotated with the exact moments `throttled` flipped
`true` then `false`, plus the email received.

**Conclusion to draw:** explain the 10°C hysteresis gap (70°C on, 60°C
off) as a deliberate design choice to prevent rapid oscillation
("flapping") right at a single threshold boundary — without it, a
temperature hovering at exactly 70°C could flip `throttled` on and off
every 2-second sample, sending a flood of throttle-event emails and
repeatedly changing the detector's target FPS/resolution, which would
itself add instability.

The loop is fully closed end-to-end (verified during development): the C
server's `sensor_thread.c` owns the real temperature reading and decides
*whether* to throttle; `person_detector.py` runs a background thread
(`_telemetry_poll_loop`) that polls `/api/v1/telemetry` every 5 seconds and
updates its own shared throttle state; the main detection loop then
actually shrinks incoming frames to 320px width and caps its own
processing rate to the reported `target_fps` whenever `throttled` is
`true`, rather than only reporting the decision without acting on it. This
was confirmed working by forcing the board into a throttled state (there's
a `SECURITY_FORCE_THROTTLED=1` env var for exactly this kind of testing,
see `sensor_thread.c`, since physically heating a board to a specific
temperature on demand isn't practical for iterative development) and
observing the detector's polling thread correctly pick up
`throttled: true, target_fps: 8` within one polling interval.

If you want to verify this on your own real hardware without waiting for
genuine thermal throttling, temporarily set
`Environment=SECURITY_FORCE_THROTTLED=1` in `security-board.service`,
restart it, and confirm the detector's annotated stream visibly drops to a
smaller frame size and lower measured FPS — then remove the override
before your final graded run.
