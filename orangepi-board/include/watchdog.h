/*
 * watchdog.h
 * -----------------------------------------------------------------------
 *
 * "If the image processing process does not produce a new frame for
 * more than 30 seconds (for example, the camera is disconnected), the
 * system will detect it and send a camera tampering alert email and
 * restart the service."
 *
 * board_state_t.last_frame_time is updated every time the Python
 * detector POSTs to /internal/detector-update (see api_handlers.c) --
 * that POST only happens after a successful frame read, so a stalled
 * camera (laptop webcam unplugged, MJPEG bridge down, network drop)
 * shows up as last_frame_time simply not advancing. This module polls
 * that timestamp and, once it's stale by more than the threshold,
 * fires an email and restarts person-detector.service via the same
 * command mechanism /api/v1/command uses for "restart_camera_service".
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "board_state.h"

/* Defaults match the spec exactly (30s stall threshold). Both are
 * overridable via SECURITY_WATCHDOG_THRESHOLD_S / SECURITY_WATCHDOG_CHECK_INTERVAL_S
 * env vars -- mainly so test 3-4 (disconnect the camera, observe the
 * watchdog) can be exercised in a reasonable amount of time during
 * development without recompiling, while production always runs with
 * the spec's real 30s value by default. */
#define WATCHDOG_STALL_THRESHOLD_SECONDS_DEFAULT 30
#define WATCHDOG_CHECK_INTERVAL_SECONDS_DEFAULT   5

/* Starts the watchdog background thread. Returns 0 on success. */
int watchdog_start(board_state_t *st);

#endif /* WATCHDOG_H */
