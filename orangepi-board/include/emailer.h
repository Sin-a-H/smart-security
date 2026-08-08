/*
 * emailer.h
 * -----------------------------------------------------------------------
 * Sends notification emails from C. Implemented with libcurl's SMTP support.
 *
 * Debounce: "maximum of one email every 30 seconds" is implemented
 * here as a simple last-sent timestamp guard shared across all callers,
 * so a burst of detections or events can't flood the inbox.
 *
 * All senders (person-detected, thermal, watchdog, guard-mode alarm)
 * share the same 30s gate.
 */

#ifndef EMAILER_H
#define EMAILER_H

#include <time.h>

typedef struct {
    char smtp_url[256];
    char username[256];
    char password[256];
    char from_addr[256];
    char to_addr[256];
} emailer_config_t;

/*
 * Loads SMTP credentials from a config file (default:
 * /etc/smart-security/email.conf, simple KEY=VALUE lines) or from
 * environment variables SMTP_URL / SMTP_USER / SMTP_PASS / SMTP_FROM /
 * SMTP_TO if the file is absent. Returns 0 on success.
 */
int emailer_init(emailer_config_t *cfg, const char *config_path);

/*
 * Sends an email if at least 30 seconds have passed since the last one
 * sent by *any* category (global debounce, see header comment above).
 * Returns 1 if actually sent, 0 if suppressed by debounce, -1 on error.
 *
 * attachment_jpeg_path may be NULL for text-only notifications
 * (e.g. watchdog / thermal events don't require a photo).
 */
int emailer_send(const emailer_config_t *cfg,
                  const char *subject,
                  const char *body,
                  const char *attachment_jpeg_path);

/* Convenience wrappers used by the different subsystems: */
int emailer_send_person_detected(const emailer_config_t *cfg, int person_count,
                                  double cpu_temp_c, const char *jpeg_path);
int emailer_send_guard_alarm(const emailer_config_t *cfg, int person_count,
                              const char *jpeg_path);
int emailer_send_thermal_event(const emailer_config_t *cfg, double cpu_temp_c,
                                int new_fps, int new_width, int new_height);
int emailer_send_watchdog_alert(const emailer_config_t *cfg, const char *reason);

#endif /* EMAILER_H */
