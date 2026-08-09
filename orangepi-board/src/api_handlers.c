#include "api_handlers.h"
#include "json_min.h"
#include "emailer.h"
#include "mqtt_client.h"
#include "blackbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

static void format_iso_time(time_t t, char *out, size_t out_sz)
{
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    strftime(out, out_sz, "%Y-%m-%dT%H:%M:%S", &tm_buf);
}

static emailer_config_t g_detection_email_cfg;
static int g_detection_email_ready = 0;

static void ensure_email_ready(void)
{
    if (!g_detection_email_ready) {
        g_detection_email_ready = (emailer_init(&g_detection_email_cfg, NULL) == 0);
    }
}

int api_handle_persons(board_state_t *st, char *out_buf, size_t out_sz, int *http_status)
{
    pthread_mutex_lock(&st->mutex);
    int count = st->person_count;
    time_t ts = st->last_person_update;
    double fps = st->detector_fps;
    pthread_mutex_unlock(&st->mutex);

    char timebuf[64];
    format_iso_time(ts ? ts : time(NULL), timebuf, sizeof(timebuf));

    int n = snprintf(out_buf, out_sz,
        "{\"timestamp\":\"%s\",\"person_count\":%d,\"detector_fps\":%.2f}",
        timebuf, count, fps);

    *http_status = 200;
    return n;
}

int api_handle_telemetry(board_state_t *st, char *out_buf, size_t out_sz, int *http_status)
{
    pthread_mutex_lock(&st->mutex);
    double temp = st->cpu_temp_c;
    double free_mb = st->mem_free_mb;
    double total_mb = st->mem_total_mb;
    double load = st->cpu_load_pct;
    int throttled = st->throttled;
    int fps = st->target_fps;
    pthread_mutex_unlock(&st->mutex);

    char timebuf[64];
    format_iso_time(time(NULL), timebuf, sizeof(timebuf));

    int n = snprintf(out_buf, out_sz,
        "{\"timestamp\":\"%s\",\"cpu_temp_c\":%.1f,\"mem_free_mb\":%.1f,"
        "\"mem_total_mb\":%.1f,\"cpu_load_pct\":%.1f,\"throttled\":%s,\"target_fps\":%d}",
        timebuf, temp, free_mb, total_mb, load, throttled ? "true" : "false", fps);

    *http_status = 200;
    return n;
}

int api_handle_history(board_state_t *st, char *out_buf, size_t out_sz, int *http_status)
{
    pthread_mutex_lock(&st->mutex);
    int count = st->history_count;
    int head = st->history_head;
    diagnostic_record_t records[HISTORY_CAPACITY];
    for (int i = 0; i < count; i++) {
        /* oldest-first ordering */
        int idx = (head - count + i + HISTORY_CAPACITY) % HISTORY_CAPACITY;
        records[i] = st->history[idx];
    }
    pthread_mutex_unlock(&st->mutex);

    int written = snprintf(out_buf, out_sz, "{\"history\":[");
    for (int i = 0; i < count; i++) {
        char timebuf[64];
        format_iso_time(records[i].timestamp, timebuf, sizeof(timebuf));
        written += snprintf(out_buf + written, out_sz - written,
            "%s{\"timestamp\":\"%s\",\"person_count\":%d,\"cpu_temp_c\":%.1f,"
            "\"mem_free_mb\":%.1f,\"cpu_load_pct\":%.1f}",
            (i > 0) ? "," : "", timebuf, records[i].person_count,
            records[i].cpu_temp_c, records[i].mem_free_mb, records[i].cpu_load_pct);
    }
    written += snprintf(out_buf + written, out_sz - written, "]}");

    *http_status = 200;
    return written;
}

int api_handle_command(board_state_t *st, const char *request_body, char *out_buf,
                        size_t out_sz, int *http_status)
{
    (void)st;
    char cmd[64] = {0};

    if (json_min_get_string(request_body, "cmd", cmd, sizeof(cmd)) != 0) {
        *http_status = 400;
        return snprintf(out_buf, out_sz, "{\"error\":\"missing 'cmd' field\"}");
    }

    if (strcmp(cmd, "reboot") == 0) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "[api] reboot command: fork() failed: %s\n", strerror(errno));
            *http_status = 500;
            return snprintf(out_buf, out_sz, "{\"error\":\"failed to schedule reboot (fork failed)\"}");
        }
        if (pid == 0) {
            sleep(2);
            execlp("sudo", "sudo", "/sbin/reboot", (char *)NULL);
            fprintf(stderr, "[api] reboot command: execlp(sudo /sbin/reboot) failed: %s -- "
                            "check that the service user has a passwordless sudoers rule for "
                            "/sbin/reboot (see SETUP_GUIDE.md section 4.7)\n", strerror(errno));
            _exit(1);
        }

        *http_status = 200;
        return snprintf(out_buf, out_sz, "{\"result\":\"reboot scheduled\"}");

    } else if (strcmp(cmd, "restart_camera_service") == 0) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "[api] restart_camera_service command: fork() failed: %s\n", strerror(errno));
            *http_status = 500;
            return snprintf(out_buf, out_sz, "{\"error\":\"failed to schedule restart (fork failed)\"}");
        }
        if (pid == 0) {
            execlp("sudo", "sudo", "/bin/systemctl", "restart", "person-detector.service", (char *)NULL);
            fprintf(stderr, "[api] restart_camera_service command: execlp(sudo systemctl restart) failed: %s -- "
                            "check that the service user has a passwordless sudoers rule for "
                            "'systemctl restart person-detector.service' (see SETUP_GUIDE.md section 4.7)\n",
                            strerror(errno));
            _exit(1);
        }
        *http_status = 200;
        return snprintf(out_buf, out_sz, "{\"result\":\"camera service restart scheduled\"}");
    }

    *http_status = 400;
    return snprintf(out_buf, out_sz, "{\"error\":\"unknown cmd '%s'\"}", cmd);
}

int api_handle_guard_mode(board_state_t *st, const char *request_body, char *out_buf,
                           size_t out_sz, int *http_status)
{
    int enabled = 0;
    if (json_min_get_int(request_body, "enabled", &enabled) != 0) {
        *http_status = 400;
        return snprintf(out_buf, out_sz, "{\"error\":\"missing 'enabled' field\"}");
    }

    pthread_mutex_lock(&st->mutex);
    st->guard_mode_enabled = enabled ? 1 : 0;
    int now_val = st->guard_mode_enabled;
    pthread_mutex_unlock(&st->mutex);

    *http_status = 200;
    return snprintf(out_buf, out_sz, "{\"guard_mode_enabled\":%s}", now_val ? "true" : "false");
}

int api_handle_total_detections(board_state_t *st, char *out_buf, size_t out_sz, int *http_status)
{
    (void)st;

    long total = blackbox_get_total_detections();

    *http_status = 200;
    return snprintf(out_buf, out_sz, "{\"total_detections\":%ld}", total);
}

int api_handle_detector_update(board_state_t *st, const char *request_body, char *out_buf,
                                size_t out_sz, int *http_status)
{
    int count = 0;
    double fps = 0.0;
    char fps_str[32] = {0};
    char jpeg_path[256] = {0};

    json_min_get_int(request_body, "person_count", &count);
    if (json_min_get_string(request_body, "fps", fps_str, sizeof(fps_str)) == 0) {
        fps = atof(fps_str);
    }

    json_min_get_string(request_body, "jpeg_path", jpeg_path, sizeof(jpeg_path));

    time_t now = time(NULL);

    pthread_mutex_lock(&st->mutex);
    st->person_count = count;
    st->detector_fps = fps;
    st->last_person_update = now;
    st->last_frame_time = now; /* feeds the software watchdog */

    diagnostic_record_t rec = {
        .timestamp = now,
        .person_count = count,
        .cpu_temp_c = st->cpu_temp_c,
        .mem_free_mb = st->mem_free_mb,
        .cpu_load_pct = st->cpu_load_pct,
    };
    board_state_push_history(st, &rec);
    if (count > 0) {
        st->total_detections += count; /* in-memory snapshot counter, resets on reboot */
    }
    double cpu_temp_snapshot = st->cpu_temp_c;
    int guard_mode = st->guard_mode_enabled;
    pthread_mutex_unlock(&st->mutex);

    if (count > 0) {
        blackbox_record_detection(now, count, cpu_temp_snapshot);

        if (guard_mode) {
            ensure_email_ready();
            if (g_detection_email_ready) {
                emailer_send_guard_alarm(&g_detection_email_cfg, count,
                                          jpeg_path[0] ? jpeg_path : NULL);
            }
            mqtt_client_publish_alarm(st, count);
        } else {
            ensure_email_ready();
            if (g_detection_email_ready) {
                emailer_send_person_detected(&g_detection_email_cfg, count, cpu_temp_snapshot,
                                              jpeg_path[0] ? jpeg_path : NULL);
            }
        }
    }

    *http_status = 200;
    return snprintf(out_buf, out_sz, "{\"ack\":true}");
}
