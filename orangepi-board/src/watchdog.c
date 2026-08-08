#include "watchdog.h"
#include "emailer.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

static void *watchdog_loop(void *arg)
{
    board_state_t *st = (board_state_t *)arg;

    int stall_threshold = getenv("SECURITY_WATCHDOG_THRESHOLD_S")
        ? atoi(getenv("SECURITY_WATCHDOG_THRESHOLD_S")) : WATCHDOG_STALL_THRESHOLD_SECONDS_DEFAULT;
    int check_interval = getenv("SECURITY_WATCHDOG_CHECK_INTERVAL_S")
        ? atoi(getenv("SECURITY_WATCHDOG_CHECK_INTERVAL_S")) : WATCHDOG_CHECK_INTERVAL_SECONDS_DEFAULT;

    emailer_config_t email_cfg;
    int email_ready = (emailer_init(&email_cfg, NULL) == 0);

    int already_alerted = 0; /* avoid re-sending every check while still stalled */

    while (1) {
        sleep(check_interval);

        pthread_mutex_lock(&st->mutex);
        time_t last_frame = st->last_frame_time;
        pthread_mutex_unlock(&st->mutex);

        if (last_frame == 0) {
            /* No frame has ever arrived yet (e.g. detector still
             * starting up) -- not a fault condition, just not ready. */
            continue;
        }

        time_t now = time(NULL);
        double stalled_for = difftime(now, last_frame);

        if (stalled_for > stall_threshold) {
            if (!already_alerted) {
                fprintf(stderr, "[watchdog] no frame for %.0fs -- camera tampering alert + restarting detector\n",
                        stalled_for);
                if (email_ready) {
                    char reason[128];
                    snprintf(reason, sizeof(reason),
                             "No frame received for %.0f seconds (threshold %d s). Possible camera disconnection or tampering.",
                             stalled_for, stall_threshold);
                    emailer_send_watchdog_alert(&email_cfg, reason);
                }

                /* Restart the detector service via systemctl, same
                 * privilege-separation approach as api_handle_command's
                 * "restart_camera_service": relies on a sudoers rule
                 * scoped to exactly this command (see docs/setup notes)
                 * rather than running the board process as root. */
                if (fork() == 0) {
                    execlp("sudo", "sudo", "/bin/systemctl", "restart",
                           "person-detector.service", (char *)NULL);
                    _exit(1);
                }
                already_alerted = 1;
            }
        } else {
            already_alerted = 0;
        }
    }
    return NULL;
}

int watchdog_start(board_state_t *st)
{
    pthread_t tid;
    if (pthread_create(&tid, NULL, watchdog_loop, st) != 0) {
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
