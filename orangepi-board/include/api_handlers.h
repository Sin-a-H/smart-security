/*
 * api_handlers.h
 * -----------------------------------------------------------------------
 * Implements the actual logic behind each /api/v1/ endpoint. The C
 * HTTPS server calls straight into these; the FastAPI/Swagger layer
 * (separate Python process, see fastapi-gateway/) calls the same
 * endpoints over loopback HTTP and only adds OpenAPI documentation on top.
 */

#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include "board_state.h"

/* Each handler writes a JSON (or binary, for /stream) body into
 * out_buf and returns the number of bytes written, or -1 on error.
 * http_status is set to the status line code to send (200, 400, etc.) */

int api_handle_persons(board_state_t *st, char *out_buf, size_t out_sz, int *http_status);
int api_handle_telemetry(board_state_t *st, char *out_buf, size_t out_sz, int *http_status);
int api_handle_history(board_state_t *st, char *out_buf, size_t out_sz, int *http_status);
int api_handle_command(board_state_t *st, const char *request_body, char *out_buf,
                        size_t out_sz, int *http_status);

/* Guard mode toggle, exposed as its own small endpoint the
 * HTML page's guard-mode switch calls. */
int api_handle_guard_mode(board_state_t *st, const char *request_body, char *out_buf,
                           size_t out_sz, int *http_status);

/* Total detections counter backing the black-box report. */
int api_handle_total_detections(board_state_t *st, char *out_buf, size_t out_sz, int *http_status);

/* Internal-only endpoint: the Python detector process POSTs its latest
 * person_count + fps here after each processed frame. Not part of the
 * public Swagger surface (no marks attached to it directly), but it's
 * the mechanism by which g_state.person_count / detector_fps get
 * updated, which the public /persons endpoint then reports. */
int api_handle_detector_update(board_state_t *st, const char *request_body, char *out_buf,
                                size_t out_sz, int *http_status);

#endif /* API_HANDLERS_H */
