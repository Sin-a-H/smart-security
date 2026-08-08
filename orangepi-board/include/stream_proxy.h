/*
 * stream_proxy.h
 * -----------------------------------------------------------------------
 * Bridges the laptop's MJPEG stream (see laptop_cam_server.py) into the
 * board process's own memory, so both:
 *   - the board's HTTPS web server (Part 1a live camera view), and
 *   - the REST /api/v1/stream endpoint
 * can re-serve frames to their own clients without each opening a
 * separate connection back to the laptop.
 *
 * Design: one background thread keeps a persistent HTTP connection to
 * the laptop's /stream.mjpg, parses the multipart boundaries itself
 * (no external HTTP client library dependency), and stores only the
 * single latest decoded JPEG frame buffer, guarded by a mutex. This is
 * the same "keep only the newest frame" pattern used by the laptop
 * server itself, and it means a slow/disconnected downstream client
 * can never block frame ingestion.
 *
 * The Python person-detector also needs frames; rather than
 * making it a third independent puller of the laptop stream, the
 * detector pulls directly from the laptop URL too.
 */

#ifndef STREAM_PROXY_H
#define STREAM_PROXY_H

#include <stddef.h>

/* Starts the background puller thread. laptop_url e.g.
 * "http://192.168.1.50:8090/stream.mjpg". Returns 0 on success. */
int stream_proxy_start(const char *laptop_url);

/*
 * Copies the current latest JPEG frame into caller-provided buffer.
 * Returns the number of bytes copied, or 0 if no frame is available
 * yet, or -1 if buf_size is too small for the current frame.
 */
int stream_proxy_get_latest_frame(unsigned char *buf, size_t buf_size);

/* Timestamp (seconds since epoch) of the last frame successfully
 * received from the laptop. Used by the software watchdog. */
long stream_proxy_last_frame_time(void);

#endif /* STREAM_PROXY_H */
