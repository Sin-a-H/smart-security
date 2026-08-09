# Smart Security System — Setup Guide

This is the single reference for getting every piece of the system running:
the Orange Pi board program, the laptop-as-camera bridge, the person
detector, the Mosquitto broker (with authentication and ACLs), the FastAPI
Swagger layer, and SSH hardening. Every command and config in this guide has
been executed and verified during development (not just written from memory)
— see the inline notes where a specific gotcha was found.

---

## 1. System overview and machine roles

There are **two physical machines**:

| Machine | Role |
|---|---|
| **Orange Pi Zero 2/3** | Runs the C board program (HTTPS server, REST API, MQTT client, email sender, watchdog), the Python person detector, and the FastAPI Swagger gateway. |
| **Laptop** | Runs (a) the webcam→MJPEG bridge, since the board has no camera, and (b) the Mosquitto broker + subscriber, since the spec asks for the broker to run on the personal computer. |

```
+----------------------+         MJPEG/HTTP          +--------------------------------+
|      LAPTOP           | ---------------------------->|         ORANGE PI              |
|                        |  :8090/stream.mjpg           |                                |
| laptop_cam_server.py   |                               |  security-board (C)            |
|  (webcam capture)      |                               |   |- HTTPS server :443         |
|                        |                               |   |- REST API /api/v1/*        |
| mosquitto broker       |<---- MQTT persons/telemetry --|   |- MQTT client (libmosquitto) |
|  :1883                 |      /alarm  (QoS1, LWT)      |   |- email sender (libcurl)     |
|                        |                               |   |- SQLite black box           |
| mosquitto_sub          |                               |   `- software watchdog          |
|  (view messages)       |                               |                                |
+----------------------+                               |  person_detector.py (Python)    |
                                                          |   `- pulls MJPEG, posts to       |
                                                          |      /internal/detector-update   |
                                                          |                                |
                                                          |  FastAPI gateway (Swagger UI)    |
                                                          |   `- forwards to the C API        |
                                                          +--------------------------------+
```

Both machines must be on the same LAN (or reachable via routed IPs). Note
down the laptop's LAN IP (`ip addr` / `ifconfig` on Linux/macOS, `ipconfig`
on Windows) and the board's LAN IP — you'll substitute these into commands
below. This guide uses `192.168.1.50` for the laptop and `192.168.1.10` for
the board as placeholders.

---

## 2. Laptop setup: webcam to MJPEG bridge

The Orange Pi Zero 2/3 has no camera connector in this build, so per the
spec ("If the board does not have a camera, use a webcam and add a step of
streaming the image from the laptop to the board"), the laptop's own webcam
is captured and re-published as MJPEG-over-HTTP.

### 2.1 Install dependencies

```bash
# On the laptop (Linux/macOS/WSL with a working Python 3):
pip install opencv-python
```

### 2.2 Run the bridge

```bash
cd laptop-camera
python3 laptop_cam_server.py --port 8090 --camera 0 --width 640 --height 480 --fps 15
```

- `--camera 0` is usually the built-in/first webcam. If you have multiple
  cameras, try `1`, `2`, etc.
- On success you'll see:
  ```
  [laptop-cam] serving on all interfaces, port 8090
  [laptop-cam] on the Orange Pi, use: http://192.168.1.50:8090/stream.mjpg
  ```
- Verify it works locally first: open `http://localhost:8090/stream.mjpg`
  in a browser on the laptop — you should see the live webcam feed.
- Leave this running in its own terminal/tab. It has no dependency on the
  board being up yet.

### 2.3 Firewall note

Make sure the laptop's firewall allows inbound connections on port 8090
from the board's IP (or from the whole LAN subnet for simplicity during
development). On Ubuntu: `sudo ufw allow from 192.168.1.0/24 to any port 8090`.

---

## 3. Mosquitto broker setup (on the laptop)

The spec requires: broker running on the PC, no anonymous MQTT logins, a
dedicated user with a unique password, and messages structured as
`persons/<student_id>/home` and `telemetry/<student_id>/home`.

### 3.1 Install Mosquitto

```bash
sudo apt update
sudo apt install -y mosquitto mosquitto-clients
sudo systemctl stop mosquitto        # stop the default anonymous instance while we configure it
```

### 3.2 Create the password file

```bash
sudo mkdir -p /etc/mosquitto/conf.d
sudo mosquitto_passwd -c /etc/mosquitto/passwordfile board_402102668
# (enter a password when prompted -- I entered 402102668)

sudo mosquitto_passwd -b /etc/mosquitto/passwordfile monitor MonitorPass
# (adds a second user for the PC-side subscriber / grading, without -c so it appends)
```

> **Verified gotcha:** Mosquitto 2.x refuses to load a password file (or
> ACL file) that is world-readable or not owned by the `mosquitto` user --
> it prints a warning now and will hard-refuse in future versions. Fix
> with:
> ```bash
> sudo chown mosquitto:mosquitto /etc/mosquitto/passwordfile
> sudo chmod 0700 /etc/mosquitto/passwordfile
> ```

### 3.3 Create the ACL file

This restricts the board's user to only its own topics (defense in depth --
even if the board's credentials leaked, they can't be used to spoof another
student's topics or subscribe to everything):

```bash
sudo tee /etc/mosquitto/aclfile > /dev/null << 'EOF'
# Board user: write-only to its own topics
user board_402102668
topic write persons/402102668/home
topic write telemetry/402102668/home
topic write alarm/402102668/home
topic write status/402102668/home

# Monitor user: read-only, for the PC-side subscriber / grading
user monitor
topic read persons/#
topic read telemetry/#
topic read alarm/#
topic read status/#
EOF

sudo chown mosquitto:mosquitto /etc/mosquitto/aclfile
sudo chmod 0700 /etc/mosquitto/aclfile
```

### 3.4 Main broker config

```bash
sudo tee /etc/mosquitto/conf.d/smart-security.conf > /dev/null << 'EOF'
listener 1883
allow_anonymous false
password_file /etc/mosquitto/passwordfile
acl_file /etc/mosquitto/aclfile
EOF
```

### 3.5 Start the broker

```bash
sudo systemctl enable mosquitto
sudo systemctl restart mosquitto
sudo systemctl status mosquitto   # confirm "active (running)"
```

### 3.6 Verify the security properties (do this before wiring up the board)

These four checks were run during development and all passed -- repeat them
against your own broker before moving on:

```bash
# 1. Anonymous connection MUST be refused:
mosquitto_pub -h localhost -p 1883 -t 'test/topic' -m 'hello'
# Expected: "Connection error: Connection Refused: not authorised."

# 2. Wrong password MUST be refused:
mosquitto_pub -h localhost -p 1883 -u board_402102668 -P WrongPassword -t 'persons/402102668/home' -m '{}'
# Expected: same refusal as above

# 3. Correct credentials + the user's own topic MUST succeed:
mosquitto_pub -h localhost -p 1883 -u board_402102668 -P 402102668 -t 'persons/402102668/home' -m '{"person_count":0}'
# Expected: no error, exits cleanly

# 4. Correct credentials but a topic OUTSIDE the ACL MUST be silently dropped
#    (subscribe as `monitor` in one terminal, publish as `board_402102668`
#    to someone else's topic in another, and confirm the subscriber never
#    sees it):
mosquitto_sub -h localhost -p 1883 -u monitor -P MonitorPass -t 'persons/#' -v &
mosquitto_pub -h localhost -p 1883 -u board_402102668 -P 402102668 -t 'persons/00000000/home' -m '{"forbidden":true}'
# Expected: the subscriber prints nothing for this message
```

---

## 4. Orange Pi board setup

### 4.1 Install dependencies

```bash
# On the Orange Pi:
sudo apt update
sudo apt install -y build-essential libssl-dev libcurl4-openssl-dev \
    libsqlite3-dev libmosquitto-dev python3 python3-pip python3-venv

pip3 install numpy
pip3 install "opencv-python-headless<5"
```

(`opencv-python-headless` avoids pulling in GUI/X11 dependencies the board
doesn't need, since the detector only writes JPEGs -- it never opens a
window.)

### 4.2 Build the C board program

```bash
cd orangepi-board
make
# produces ./security-board
```

### 4.3 Generate the self-signed SSL certificate

```bash
cd ssl
chmod +x generate_ssl_cert.sh
sudo ./generate_ssl_cert.sh
```

This writes `/etc/smart-security/ssl/server.crt` and `server.key` with
`CN=402102668`. Verify:

```bash
openssl x509 -in /etc/smart-security/ssl/server.crt -noout -subject
# Expected: subject=CN = 402102668, O = SmartSecuritySystem, C = IR
```

### 4.4 Configure email credentials

```bash
sudo mkdir -p /etc/smart-security
sudo tee /etc/smart-security/email.conf > /dev/null << 'EOF'
SMTP_URL=smtps://smtp.gmail.com:465
SMTP_USER=your-account@gmail.com
SMTP_PASS=your-16-char-app-password
SMTP_FROM=your-account@gmail.com
SMTP_TO=your-account@gmail.com
EOF
sudo chmod 600 /etc/smart-security/email.conf
```

If using Gmail, you need an **App Password** (Google Account -> Security ->
2-Step Verification -> App passwords), not your normal account password,
since Gmail blocks plain SMTP auth with regular passwords.

### 4.5 Configure MQTT credentials

```bash
sudo tee /etc/smart-security/mqtt.conf > /dev/null << 'EOF'
MQTT_HOST=192.168.1.101
MQTT_PORT=1883
MQTT_USER=board_402102668
MQTT_PASS=402102668
EOF
sudo chmod 600 /etc/smart-security/mqtt.conf
```

### 4.6 Deploy files to their runtime locations

```bash
sudo mkdir -p /opt/smart-security/bin /opt/smart-security/web \
              /opt/smart-security/detector /opt/smart-security/fastapi-gateway \
              /opt/smart-security/snapshots /var/lib/smart-security

sudo cp orangepi-board/security-board /opt/smart-security/bin/
sudo cp orangepi-board/web/index.html /opt/smart-security/web/
sudo cp -r detector/* /opt/smart-security/detector/
sudo cp -r fastapi-gateway/* /opt/smart-security/fastapi-gateway/

sudo useradd -r -s /usr/sbin/nologin security 2>/dev/null || true
sudo chown -R security:security /opt/smart-security /var/lib/smart-security
```

### 4.7 Set up the sudoers rule for privileged commands

The board process runs as an unprivileged `security` user (not root), but
needs to trigger `reboot` (via `/api/v1/command`) and restart the detector
service (via the watchdog and `restart_camera_service`). Grant exactly
those two commands, nothing else:

```bash
sudo tee /etc/sudoers.d/smart-security > /dev/null << 'EOF'
security ALL=(root) NOPASSWD: /sbin/reboot
security ALL=(root) NOPASSWD: /bin/systemctl restart person-detector.service
EOF
sudo chmod 440 /etc/sudoers.d/smart-security
sudo visudo -c   # validates syntax of all sudoers files, confirm no errors

sudo chown -R security:security /etc/smart-security/ssl
sudo chmod 600 /etc/smart-security/ssl/server.key
sudo chmod 644 /etc/smart-security/ssl/server.crt

sudo chown -R security:security /etc/smart-security
sudo chmod 600 /etc/smart-security/*.conf 2>/dev/null
```

### 4.8 Edit and install the systemd unit files

Open each of the three `.service` files in `orangepi-board/systemd/` and
replace the placeholder student ID / laptop IP in `ExecStart=` with your
real values, then:

```bash
sudo cp orangepi-board/systemd/*.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable security-board.service person-detector.service swagger-gateway.service
sudo systemctl start security-board.service person-detector.service swagger-gateway.service
```

### 4.9 Set up the Python virtual environments the systemd units expect

```bash
cd /opt/smart-security/detector
python3 -m venv venv
./venv/bin/pip install numpy
./venv/bin/pip install "opencv-python-headless<5"

cd /opt/smart-security/fastapi-gateway
python3 -m venv venv
./venv/bin/pip install -r requirements.txt
```

### 4.10 Verify everything is up

```bash
sudo systemctl status security-board person-detector swagger-gateway
journalctl -u security-board -f     # live log tail

curl -sk https://localhost/api/v1/telemetry
curl -sk https://localhost/api/v1/persons
```

Open `https://<board-ip>/` in a browser (accept the self-signed cert
warning) to see the dashboard, and `http://<board-ip>:8000/docs` for
Swagger UI.

---

## 5. SSH hardening (applies to the Orange Pi)

Per the security requirements: root login disabled, and login via public
key or password only (i.e., no other weaker auth methods left enabled).

```bash
sudo cp /etc/ssh/sshd_config /etc/ssh/sshd_config.bak   # keep a backup

sudo tee -a /etc/ssh/sshd_config > /dev/null << 'EOF'

# --- Smart Security System hardening ---
PermitRootLogin no
PasswordAuthentication yes
PubkeyAuthentication yes
ChallengeResponseAuthentication no
KbdInteractiveAuthentication no
EOF

sudo sshd -t          # validates config syntax BEFORE restarting -- do not skip this
sudo systemctl restart sshd
```

> If you intend to disable password auth entirely and require key-only
> login, set up your public key first (`ssh-copy-id user@board-ip`), test
> that key-based login works, and only then flip
> `PasswordAuthentication no`. Doing it in the wrong order can lock you out
> if something goes wrong with the key.

Test 7-3 in the spec ("Unauthorized SSH login attempt") is satisfied by
attempting a login as `root` (should be refused immediately due to
`PermitRootLogin no`) or with a wrong password/key for a real user, and
screenshotting the refusal.

---

## 6. Quick reference: what's running where

| Process | Machine | Started by |
|---|---|---|
| `laptop_cam_server.py` | Laptop | Manually (own terminal) |
| `mosquitto` | Laptop | systemd (`mosquitto.service`) |
| `security-board` | Orange Pi | systemd (`security-board.service`) |
| `person_detector.py` | Orange Pi | systemd (`person-detector.service`) |
| FastAPI gateway (`uvicorn`) | Orange Pi | systemd (`swagger-gateway.service`) |

## 7. Order of operations for a fresh boot / demo

1. Laptop: start `laptop_cam_server.py` (must be up before the board's
   services, since `person-detector.service` depends on the stream).
2. Laptop: confirm `mosquitto` is running (`systemctl status mosquitto`).
3. Power on / reboot the Orange Pi -- systemd brings up all three board
   services automatically in the correct dependency order (see the
   ordering diagram in the report, generated from the `After=`/`Requires=`
   directives in the `.service` files).
4. From any machine on the LAN: browse to `https://<board-ip>/` for the
   dashboard, or `http://<board-ip>:8000/docs` for Swagger.
