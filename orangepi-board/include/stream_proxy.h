/*
 * stream_proxy.h
 * -----------------------------------------------------------------------
 * Bridges an upstream MJPEG-over-HTTP source into the board process's
 * own memory, so the board's own HTTP clients (dashboard, Swagger,
 * curl) can consume it without each opening a separate connection back
 * to the upstream source.
 *
 * This module is INSTANCE-BASED (not a global singleton): the board
 * runs two independent proxies side by side --
 *   - one pulling the laptop's raw webcam feed (see laptop_cam_server.py)
 *     for /api/v1/stream and the dashboard's "Raw Camera" view, and
 *   - one pulling the Python detector's own annotated-output stream
 *     (see person_detector.py's --annotated-port) for
 *     /api/v1/detector-stream and the dashboard's "Detector Output" view.
 * Both are handled by the exact same parsing/reconnect logic below,
 * just with separate instances holding separate connection state and
 * separate frame buffers -- there is no shared global state to collide
 * between the two.
 */

#ifndef STREAM_PROXY_H
#define STREAM_PROXY_H

#include <stddef.h>

typedef struct stream_proxy stream_proxy_t;

/*
 * Creates and starts a new proxy instance pulling from source_url
 * (e.g. "http://192.168.1.50:8090/stream.mjpg" or
 * "http://127.0.0.1:8091/annotated.mjpg"), and starts its background
 * puller thread. label is a short human-readable name used only in log
 * lines (e.g. "camera", "detector") to distinguish instances in the
 * journal. Returns a handle to pass to the other functions, or NULL on
 * failure (e.g. an unparseable URL).
 */
stream_proxy_t *stream_proxy_start(const char *source_url, const char *label);

/*
 * Copies the current latest JPEG frame into caller-provided buffer.
 * Returns the number of bytes copied, or 0 if no frame is available
 * yet, or -1 if buf_size is too small for the current frame.
 */
int stream_proxy_get_latest_frame(stream_proxy_t *proxy, unsigned char *buf, size_t buf_size);

/* Timestamp (seconds since epoch) of the last frame successfully
 * received from this instance's source. Used by the software watchdog
 * for the camera proxy instance. */
long stream_proxy_last_frame_time(stream_proxy_t *proxy);

#endif /* STREAM_PROXY_H */
