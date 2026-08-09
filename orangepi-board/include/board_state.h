/*
 * board_state.h
 * -----------------------------------------------------------------------
 * Shared in-memory state for the Orange Pi board program.
 *
 * A single process (board_main) hosts:
 *   - the HTTPS web server (Part 1)
 *   - the REST API handlers backing the /api/v1/ endpoints (Part 2)
 *   - the MJPEG stream proxy pulling from the laptop (bridge)
 *   - person-count updates received from the Python detector (Part 3)
 *   - MQTT publish of persons/telemetry (Part 3)
 *   - guard mode, watchdog, thermal throttling (Part 4)
 */

#ifndef BOARD_STATE_H
#define BOARD_STATE_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

/* Forward declaration only -- board_state.h doesn't need the full
 * stream_proxy.h definition, just a pointer type, so we avoid an
 * unnecessary include here. */
typedef struct stream_proxy stream_proxy_t;

#define STUDENT_ID_MAXLEN     32
#define HISTORY_CAPACITY      5     /* "last 5 records" per the spec */
#define MAX_STREAM_URL_LEN    256
#define MAX_TOPIC_LEN         128

typedef struct {
    time_t   timestamp;
    int      person_count;
    double   cpu_temp_c;
    double   mem_free_mb;
    double   cpu_load_pct;
} diagnostic_record_t;

typedef struct {
    /* ---- identity ---- */
    char student_id[STUDENT_ID_MAXLEN];
    char student_name[128];

    /* ---- live sensor snapshot (Part 1c, Part 2a) ---- */
    double   cpu_temp_c;
    double   mem_free_mb;
    double   mem_total_mb;
    double   cpu_load_pct;

    /* ---- person detection (Part 3a), pushed via a small local
     *      HTTP callback from the Python detector process ---- */
    int      person_count;
    double   detector_fps;
    time_t   last_person_update;
    time_t   last_frame_time;       /* used by the software watchdog, Part 4.3 */

    /* ---- history / black box (Part 4.2) ---- */
    diagnostic_record_t history[HISTORY_CAPACITY];
    int      history_head;   /* next write index (circular buffer) */
    int      history_count;  /* number of valid entries, capped at HISTORY_CAPACITY */
    long     total_detections; /* lifetime counter, persisted in SQLite */

    /* ---- guard / anti-theft mode (Part 4.1) ---- */
    int      guard_mode_enabled;

    /* ---- adaptive thermal state (Part 4.4) ---- */
    int      throttled;
    int      target_fps;
    int      target_width;
    int      target_height;

    /* ---- laptop stream source ---- */
    char     laptop_stream_url[MAX_STREAM_URL_LEN];

    /* ---- stream proxy handles ---- */
    stream_proxy_t *camera_proxy;
    stream_proxy_t *detector_proxy;

    /* ---- MQTT topics, derived from student_id at startup ---- */
    char     topic_persons[MAX_TOPIC_LEN];
    char     topic_telemetry[MAX_TOPIC_LEN];
    char     topic_alarm[MAX_TOPIC_LEN];

    pthread_mutex_t mutex;
} board_state_t;

/* One process-wide instance, defined in board_main.c */
extern board_state_t g_state;

void board_state_init(board_state_t *st, const char *student_id, const char *student_name,
                       const char *laptop_stream_url);
void board_state_push_history(board_state_t *st, const diagnostic_record_t *rec);

#endif /* BOARD_STATE_H */
