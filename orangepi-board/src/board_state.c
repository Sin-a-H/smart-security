#include "board_state.h"
#include <string.h>
#include <stdio.h>

board_state_t g_state;

void board_state_init(board_state_t *st, const char *student_id, const char *student_name,
                       const char *laptop_stream_url)
{
    memset(st, 0, sizeof(*st));

    strncpy(st->student_id, student_id, STUDENT_ID_MAXLEN - 1);
    strncpy(st->student_name, student_name, sizeof(st->student_name) - 1);
    strncpy(st->laptop_stream_url, laptop_stream_url, MAX_STREAM_URL_LEN - 1);

    snprintf(st->topic_persons, MAX_TOPIC_LEN, "persons/%s/home", student_id);
    snprintf(st->topic_telemetry, MAX_TOPIC_LEN, "telemetry/%s/home", student_id);
    snprintf(st->topic_alarm, MAX_TOPIC_LEN, "alarm/%s/home", student_id);

    st->target_fps = 15;
    st->target_width = 640;
    st->target_height = 480;
    st->guard_mode_enabled = 0;
    st->throttled = 0;

    pthread_mutex_init(&st->mutex, NULL);
}

/* Circular buffer push: overwrite oldest slot once full. */
void board_state_push_history(board_state_t *st, const diagnostic_record_t *rec)
{
    st->history[st->history_head] = *rec;
    st->history_head = (st->history_head + 1) % HISTORY_CAPACITY;
    if (st->history_count < HISTORY_CAPACITY) {
        st->history_count++;
    }
}
