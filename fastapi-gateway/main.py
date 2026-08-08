"""
main.py -- Thin FastAPI/Swagger documentation & gateway layer.
-----------------------------------------------------------------------

Every route below does nothing but forward the HTTP call to the equivalent
endpoint on the C server and return its response verbatim.
There is no parsing of sensor values, no state, no business rules here.

Run (on the Orange Pi, alongside the C server):
    pip install fastapi uvicorn httpx
    uvicorn main:app --host 0.0.0.0 --port 8000

Then Swagger UI is available at:
    http://<board-ip>:8000/docs
"""

from __future__ import annotations

from typing import Any

import httpx
from fastapi import FastAPI, Request, Response
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import BaseModel

# The C server terminates TLS with a self-signed cert, so
# this internal loopback call must skip verification.
BOARD_BASE_URL = "https://127.0.0.1"

app = FastAPI(
    title="Smart Security System API",
    description=(
        "Documentation gateway only. All logic (temperature/memory "
        "reads, person counting, command execution) lives in the C "
        "program (security-board); this layer forwards requests to it "
        "and exists purely to provide Swagger UI."
    ),
    version="1.0.0",
)


class CommandRequest(BaseModel):
    cmd: str


class GuardModeRequest(BaseModel):
    enabled: int


async def _forward(method: str, path: str, json_body: dict[str, Any] | None = None) -> Response:
    """Forwards a request to the C server and passes its response straight through."""
    async with httpx.AsyncClient(verify=False, timeout=10.0) as client:
        resp = await client.request(method, f"{BOARD_BASE_URL}{path}", json=json_body)
        return JSONResponse(status_code=resp.status_code, content=resp.json())


@app.get("/api/v1/persons", summary="Current person count", tags=["monitoring"])
async def get_persons():
    """Timestamp + current number of people detected. Forwards to the C server."""
    return await _forward("GET", "/api/v1/persons")


@app.get("/api/v1/telemetry", summary="CPU temperature, free memory, CPU load", tags=["monitoring"])
async def get_telemetry():
    """Forwards to the C server, which reads /proc and /sys directly."""
    return await _forward("GET", "/api/v1/telemetry")


@app.get("/api/v1/history", summary="Last 5 diagnostic records", tags=["monitoring"])
async def get_history():
    """Forwards to the C server's circular history buffer."""
    return await _forward("GET", "/api/v1/history")


@app.post("/api/v1/command", summary="Execute a board command (e.g. reboot)", tags=["control"])
async def post_command(cmd: CommandRequest):
    """
    Forwards to the C server's command dispatcher. Currently supported
    by the C side: "reboot", "restart_camera_service" -- but per the
    spec this endpoint is designed to accept new commands added later
    without changing this gateway.
    """
    return await _forward("POST", "/api/v1/command", cmd.model_dump())


@app.post("/api/v1/guard-mode", summary="Enable/disable anti-theft guard mode", tags=["control"])
async def post_guard_mode(mode: GuardModeRequest):
    """Forwards to the C server; toggles instant-alert behavior on detections."""
    return await _forward("POST", "/api/v1/guard-mode", mode.model_dump())


@app.get("/api/v1/detections/total", summary="Lifetime total detections (black box)", tags=["monitoring"])
async def get_total_detections():
    """Forwards to the C server's SQLite-backed counter."""
    return await _forward("GET", "/api/v1/detections/total")


@app.get("/api/v1/stream", summary="Live MJPEG camera stream", tags=["monitoring"], response_class=StreamingResponse)
async def get_stream():
    """
    Proxies the live MJPEG stream from the C server. Swagger UI's
    "Try it out" will download the stream body rather than render it
    live. for a live view, open the board's own dashboard page at
    https://<board-ip>/ which uses a plain <img> tag instead.
    """
    client = httpx.AsyncClient(verify=False, timeout=None)
    req = client.build_request("GET", f"{BOARD_BASE_URL}/api/v1/stream")
    resp = await client.send(req, stream=True)

    async def stream_body():
        async for chunk in resp.aiter_bytes():
            yield chunk
        await resp.aclose()
        await client.aclose()

    return StreamingResponse(
        stream_body(),
        media_type=resp.headers.get("content-type", "multipart/x-mixed-replace; boundary=FRAME"),
    )


@app.get("/health", summary="Gateway liveness check", tags=["meta"])
async def health():
    """Simple check that this gateway process itself is up (does not check the C server)."""
    return {"status": "ok", "note": "gateway only; see /api/v1/telemetry to confirm the C server is reachable"}
