# Smart Security System

A smart security and person-detection system built around an **Orange Pi Zero** and a laptop-based camera service. The system combines computer vision, a web dashboard, REST APIs, MQTT messaging, and email notifications to monitor activity and report security events.

## Features

- **Person Detection:** Detects people from a live webcam stream using Python and OpenCV.
- **Embedded Security Server:** A C-based application running on Orange Pi provides HTTPS and REST API endpoints.
- **Web Dashboard:** Provides a browser-based interface for monitoring the system.
- **MQTT Communication:** Publishes detection events, telemetry, and alarm messages through an authenticated MQTT broker.
- **Email Notifications:** Supports email alerts for security events.
- **System Monitoring:** Includes a software watchdog and service management for improved reliability.
- **API Documentation:** Provides interactive API documentation through FastAPI and Swagger UI.

## Project Structure

| Directory / File | Description |
|---|---|
| `orangepi-board/` | C-based embedded application, web interface, and systemd service files. |
| `laptop-camera/` | Python service that streams webcam video over HTTP as MJPEG. |
| `detector/` | Python-based person detection service. |
| `fastapi-gateway/` | FastAPI gateway providing interactive API documentation and forwarding requests to the embedded server. |
| `SETUP_GUIDE.md` | Detailed installation, configuration, deployment, and testing instructions. |

## System Architecture

The laptop captures webcam video and streams it to the Orange Pi, where the person detection service processes the feed. The embedded C application manages system operations and exposes secure APIs. MQTT carries event and telemetry messages to the monitoring client, while email notifications provide an additional alert mechanism.

## Getting Started

Refer to [`SETUP_GUIDE.md`](SETUP_GUIDE.md) for complete setup instructions.

The deployment requires an Orange Pi Zero, a laptop with a webcam, Python 3, a Mosquitto MQTT broker, and the required development libraries. Both machines should be reachable over the network.

## Technologies

**C · Python · OpenCV · Orange Pi · FastAPI · REST API · HTTPS · MQTT · Mosquitto · SQLite · Linux systemd**

