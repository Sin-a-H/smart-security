#include "sensor_thread.h"
#include "sysinfo.h"
#include "emailer.h"
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static emailer_config_t g_email_cfg;
static int g_email_ready = 0;

static void *sensor_loop(void *arg)
{
    board_state_t *st = (board_state_t *)arg;

    /* Testing convenience only: forces throttled=true immediately at
     * startup regardless of measured temperature, so the detector's
     * closed-loop response (actual resolution/FPS reduction, see
     * person_detector.py's telemetry-polling thread) can be exercised
     * without needing to physically raise the board's CPU temperature
     * past THERMAL_THROTTLE_ON_C. Not used in normal operation. */
    int force_throttled = getenv("SECURITY_FORCE_THROTTLED") != NULL;
    if (force_throttled) {
        pthread_mutex_lock(&st->mutex);
        st->throttled = 1;
        st->target_fps = 8;
        st->target_width = 320;
        st->target_height = 240;
        pthread_mutex_unlock(&st->mutex);
        fprintf(stderr, "[sensor] SECURITY_FORCE_THROTTLED set -- forcing throttled state for testing\n");
    }

    cpu_stat_sample_t prev_sample;
    int have_prev = (sysinfo_cpu_stat_sample(&prev_sample) == 0);

    while (1) {
        sleep(2); /* "live updated every 2 seconds" */

        double temp_c = 0.0, free_mb = 0.0, total_mb = 0.0;
        int temp_ok = (sysinfo_read_cpu_temp(&temp_c) == 0);
        int mem_ok  = (sysinfo_read_memory(&free_mb, &total_mb) == 0);

        double load_pct = 0.0;
        cpu_stat_sample_t cur_sample;
        if (sysinfo_cpu_stat_sample(&cur_sample) == 0) {
            if (have_prev) {
                load_pct = sysinfo_cpu_load_from_samples(&prev_sample, &cur_sample);
            }
            prev_sample = cur_sample;
            have_prev = 1;
        }

        pthread_mutex_lock(&st->mutex);
        if (temp_ok) st->cpu_temp_c = temp_c;
        if (mem_ok) { st->mem_free_mb = free_mb; st->mem_total_mb = total_mb; }
        st->cpu_load_pct = load_pct;

        /* Adaptive thermal management */
        int was_throttled = st->throttled;
        if (temp_ok && !st->throttled && temp_c >= THERMAL_THROTTLE_ON_C) {
            st->throttled = 1;
            st->target_fps = 8;
            st->target_width = 320;
            st->target_height = 240;
        } else if (temp_ok && st->throttled && temp_c <= THERMAL_THROTTLE_OFF_C) {
            st->throttled = 0;
            st->target_fps = 15;
            st->target_width = 640;
            st->target_height = 480;
        }
        int now_throttled = st->throttled;
        int notify_fps = st->target_fps, notify_w = st->target_width, notify_h = st->target_height;
        pthread_mutex_unlock(&st->mutex);

        if (!was_throttled && now_throttled) {
            fprintf(stderr, "[sensor] thermal threshold exceeded (%.1f C) -> throttling to %dfps %dx%d\n",
                    temp_c, notify_fps, notify_w, notify_h);
            if (!g_email_ready) {
                g_email_ready = (emailer_init(&g_email_cfg, NULL) == 0);
            }
            if (g_email_ready) {
                emailer_send_thermal_event(&g_email_cfg, temp_c, notify_fps, notify_w, notify_h);
            }
        }
    }
    return NULL;
}

int sensor_thread_start(board_state_t *st)
{
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, sensor_loop, st);
    if (rc != 0) {
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
