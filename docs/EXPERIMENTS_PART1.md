# Experiment Walkthrough — Part 1: Self-service web server and SSL

For each test: exact commands to run, what a correct result looks like, and
how to read/conclude from it. Screenshots/videos are yours to capture on
the real board — this tells you what you should be capturing and why it
matters.

---

## Test 1-1 — Boot time measurement

**Run:**
```bash
sudo reboot
# after the board comes back up and you can SSH in again:
systemd-analyze
systemd-analyze blame | head -20
```

**Expected output:**
`systemd-analyze` prints a single summary line, e.g.:
```
Startup finished in 3.912s (kernel) + 8.221s (userspace) = 12.133s
```
`systemd-analyze blame` prints every unit sorted by how long it took to
initialize, slowest first. Expect to see `security-board.service` and
`person-detector.service` somewhere in the list — the Python detector
(loading the MobileNet-SSD model file, ~23MB, into OpenCV's DNN module)
is very likely to be one of the slower entries, typically in the
low-single-digit seconds on an Allwinner H616/H618, since model loading is
a one-time disk-read + graph-parse cost.

**What to capture:** a terminal screenshot showing both commands' output,
with your services' lines visible in the `blame` list.

**Conclusion to draw:** report the total boot time, and explicitly state
what fraction of it belongs to your services vs. the OS/kernel. If
`person-detector.service` dominates, that's expected and worth noting as a
trade-off: MobileNet-SSD's model-load time is the price of not re-training
a smaller custom model. If `security-board.service` is unexpectedly slow,
suspect the SQLite black-box schema creation or the initial MQTT connect
attempt blocking longer than expected — both are designed to be
non-blocking/retrying, so a slow boot here would be worth investigating
further rather than just reporting.

---

## Test 2-1 — Web server crash + auto-restart

**Run:**
```bash
# find the PID:
systemctl status security-board.service | grep "Main PID"
# or:
pgrep -f security-board

sudo kill -9 <PID>

sleep 5
journalctl -u security-board.service -n 30 --no-pager
```

**Expected output:** journalctl shows the process exiting (killed by
signal 9), followed within `RestartSec=3` (set in the `.service` file) by
systemd starting a fresh instance — you'll see log lines like:
```
security-board.service: Main process exited, code=killed, status=9/KILL
security-board.service: Failed with result 'signal'.
security-board.service: Scheduled restart job, restart counter is at 1.
Started Smart Security System - Board main process (HTTPS server + REST API).
[https_server] listening on 0.0.0.0:443 (TLS)
[board_main] all subsystems started for student_id=99123456
```

**What to capture:** the journalctl output showing the kill, the
"Scheduled restart job" line, and the fresh startup log lines.

**Conclusion to draw:** confirm the dashboard/API become reachable again
within a few seconds of the kill (`curl -sk https://localhost/api/v1/telemetry`
right after) — this demonstrates `Restart=on-failure` genuinely recovers
the service without manual intervention, which is the point of the test.
Also worth noting: the SQLite black box and any persisted state (MQTT
"online" retained message once reconnected) survive the restart since they
live in files/the broker rather than only in the killed process's memory.

---

## Test 3-1 — Full power cycle, no manual intervention

**Run:** physically power off the board, wait a few seconds, power it back
on. Do not touch the keyboard/SSH in immediately — just watch/record.

**What to capture:** a video from power-on to the dashboard becoming
reachable (the spec explicitly asks for this one as a video, which per
your instructions we're not producing here — you'll record your own).

**What "success" looks like when you check afterward:**
```bash
# once it's back up, from another machine:
curl -sk https://<board-ip>/api/v1/telemetry
curl -sk https://<board-ip>/api/v1/persons
systemctl is-enabled security-board person-detector swagger-gateway
# each should print "enabled"
```

**Conclusion to draw:** state clearly that no SSH login or manual command
was needed between power-on and the API responding — that's what
`WantedBy=multi-user.target` + `systemctl enable` guarantees. If you find
you *do* need to intervene, the most common causes are: a `.service` file
not actually enabled (re-run `systemctl enable`), or the laptop's MJPEG
bridge not running yet at the moment `person-detector.service` starts —
which is fine and expected, since that service's internal retry loop
handles it (verified during development: starting the detector before the
laptop bridge produces repeated "Could not open MJPEG stream" retries
until the bridge appears, then it recovers on its own).

---

## Test 4-1 — HTTP-to-HTTPS redirect

**Run:**
```bash
curl -sI http://<board-ip>/
```

**Expected output (verified during development):**
```
HTTP/1.1 301 Moved Permanently
Location: https://<board-ip>/
Content-Length: 0
Connection: close
```

Also open `http://<board-ip>/` directly in a browser with DevTools'
Network tab open, and confirm you see the `301` response followed by the
browser automatically re-requesting `https://<board-ip>/`.

**What to capture:** the DevTools Network tab showing the 301 status code
and the `Location` header, per the spec's instruction to use "the inspect
feature in the browser."

**Conclusion to draw:** the redirect preserves the original path (not just
redirecting everything to `/`) — you can verify this by hitting
`http://<board-ip>/api/v1/telemetry` and confirming the `Location` header
says `https://<board-ip>/api/v1/telemetry`, not just the bare domain. This
matters because a redirect that drops the path would break any client that
bookmarks or scripts a specific API URL over HTTP.

---

## Test 5-1 — Self-signed certificate inspection

**Run:** open `https://<board-ip>/` in a browser, click through the
"not secure" / certificate warning, and view the certificate details (in
Chrome: click the warning icon in the address bar → "Certificate is not
valid" → Details tab; in Firefox: click the padlock/warning → "More
information" → "View Certificate").

Alternatively, from a terminal:
```bash
openssl x509 -in /etc/smart-security/ssl/server.crt -noout -text | head -20
openssl s_client -connect <board-ip>:443 -servername <board-ip> </dev/null 2>/dev/null | openssl x509 -noout -subject
```

**Expected output (verified during development):**
```
subject=CN = 99123456, O = SmartSecuritySystem, C = IR
```

**What to capture:** both the full browser certificate-details panel
(showing the CN field with your student number) and, ideally, the terminal
`openssl x509` output for the report as an unambiguous text confirmation
alongside the screenshot.

**Conclusion to draw:** confirm the CN exactly matches your student
number with no typos, and note the validity period (825 days from
generation, per `generate_ssl_cert.sh`) — mention that a self-signed cert
will always show a browser trust warning since it isn't signed by a
publicly trusted CA, which is expected and acceptable for this assignment
(the requirement is a valid self-signed cert with the right CN, not a
CA-trusted one).

---

## Test 6-1 — Dashboard page content

**Run:** open `https://<board-ip>/` in a browser.

**Expected output:** the page title bar shows `Jane Doe - 99123456 - Smart
Security System` (or your actual name/ID once you edit `index.html`), the
live camera stream renders in the left panel, the person count updates
every 2 seconds beneath it, and the telemetry panel on the right shows CPU
temperature / free memory / CPU load, refreshing every 2 seconds.

**What to capture:** a full-page screenshot showing the title bar (name +
student number visible in the browser tab) together with the live stream
and telemetry panels populated with real (non-placeholder) numbers.

**Conclusion to draw:** confirm the "every 2 seconds" cadence visually —
watch the CPU load / temperature numbers tick over a few refresh cycles
and note that they're not static/frozen values, which would indicate the
polling JavaScript isn't actually running or the `/api/v1/telemetry`
endpoint isn't returning fresh data each call.
