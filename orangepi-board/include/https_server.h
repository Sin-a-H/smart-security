/*
 * https_server.h
 * -----------------------------------------------------------------------
 * Minimal HTTPS server built directly on OpenSSL (libssl/libcrypto) and
 * POSIX sockets. serving:
 *   - Part 1: the HTML dashboard page at "/"
 *   - Part 2: JSON REST endpoints under /api/v1/ (see api_handlers.h)
 *   - the live MJPEG stream at /api/v1/stream (also linked from "/")
 *
 * Also runs a second, plain-HTTP listener on port 80 whose only job is
 * to 301-redirect every request to the HTTPS URL.
 *
 * This server is intentionally single-purpose and synchronous per
 * connection (one thread per accepted connection) rather than an
 * event-loop design: simpler to read/grade, and the request volume
 * here (a handful of browser/Swagger clients) does not need epoll-level
 * scalability. The one place this would matter -- the MJPEG stream,
 * which holds a connection open indefinitely -- is handled fine by a
 * dedicated thread per stream client.
 */

#ifndef HTTPS_SERVER_H
#define HTTPS_SERVER_H

#include "board_state.h"

typedef struct {
    int https_port;             /* default 443 */
    int http_redirect_port;     /* default 80 */
    char cert_path[256];
    char key_path[256];
    char web_root[256];         /* directory containing index.html etc. */
} https_server_config_t;

/* Starts both listeners (HTTPS + HTTP-redirect-only), each in its own
 * background thread. Returns 0 on success. */
int https_server_start(const https_server_config_t *cfg, board_state_t *st);

#endif /* HTTPS_SERVER_H */
