#include "emailer.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* Global debounce state: last successful send time, shared by all
 * categories. See header comment for rationale. */
static time_t g_last_sent = 0;
static pthread_mutex_t g_debounce_mutex = PTHREAD_MUTEX_INITIALIZER;
#define EMAIL_DEBOUNCE_SECONDS 30

static void safe_copy(char *dst, size_t dst_sz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int read_kv_file(const char *path, emailer_config_t *cfg)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char key[128], value[256];
        /* strip newline */
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        size_t klen = eq - line;
        if (klen >= sizeof(key)) continue;
        strncpy(key, line, klen);
        key[klen] = '\0';
        strncpy(value, eq + 1, sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';

        if (strcmp(key, "SMTP_URL") == 0) safe_copy(cfg->smtp_url, sizeof(cfg->smtp_url), value);
        else if (strcmp(key, "SMTP_USER") == 0) safe_copy(cfg->username, sizeof(cfg->username), value);
        else if (strcmp(key, "SMTP_PASS") == 0) safe_copy(cfg->password, sizeof(cfg->password), value);
        else if (strcmp(key, "SMTP_FROM") == 0) safe_copy(cfg->from_addr, sizeof(cfg->from_addr), value);
        else if (strcmp(key, "SMTP_TO") == 0) safe_copy(cfg->to_addr, sizeof(cfg->to_addr), value);
    }
    fclose(f);
    return 0;
}

int emailer_init(emailer_config_t *cfg, const char *config_path)
{
    memset(cfg, 0, sizeof(*cfg));
    const char *path = config_path ? config_path : "/etc/smart-security/email.conf";

    if (read_kv_file(path, cfg) != 0) {
        /* Fall back to environment variables. No secret is ever
         * compiled into the binary or committed to source, per the
         * security requirements section. */
        const char *v;
        if ((v = getenv("SMTP_URL")))  safe_copy(cfg->smtp_url, sizeof(cfg->smtp_url), v);
        if ((v = getenv("SMTP_USER"))) safe_copy(cfg->username, sizeof(cfg->username), v);
        if ((v = getenv("SMTP_PASS"))) safe_copy(cfg->password, sizeof(cfg->password), v);
        if ((v = getenv("SMTP_FROM"))) safe_copy(cfg->from_addr, sizeof(cfg->from_addr), v);
        if ((v = getenv("SMTP_TO")))   safe_copy(cfg->to_addr, sizeof(cfg->to_addr), v);
    }

    if (cfg->smtp_url[0] == '\0' || cfg->username[0] == '\0' || cfg->password[0] == '\0') {
        fprintf(stderr, "[emailer] WARNING: incomplete SMTP config (checked %s and env vars)\n", path);
        return -1;
    }
    return 0;
}

/* Builds a MIME multipart message with an optional JPEG attachment,
 * base64-encoded by libcurl's mime API (curl_mime_*), and performs the
 * SMTP submission. Returns CURLE_OK (0) on success. */
static CURLcode do_send(const emailer_config_t *cfg, const char *subject,
                         const char *body, const char *attachment_jpeg_path)
{
    CURL *curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;

    struct curl_slist *recipients = NULL;
    CURLcode res;

    curl_easy_setopt(curl, CURLOPT_URL, cfg->smtp_url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg->username);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg->password);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, cfg->from_addr);
    recipients = curl_slist_append(recipients, cfg->to_addr);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    curl_mime *mime = curl_mime_init(curl);

    curl_mimepart *part = curl_mime_addpart(mime);
    char headers_buf[1024];
    snprintf(headers_buf, sizeof(headers_buf),
             "From: %s\r\nTo: %s\r\nSubject: %s\r\n", cfg->from_addr, cfg->to_addr, subject);
    curl_mime_data(part, body, CURL_ZERO_TERMINATED);
    curl_mime_type(part, "text/plain");

    if (attachment_jpeg_path) {
        curl_mimepart *att = curl_mime_addpart(mime);
        curl_mime_filedata(att, attachment_jpeg_path);
        curl_mime_type(att, "image/jpeg");
    }

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    /* prepend headers by using CURLOPT_HTTPHEADER-equivalent for SMTP:
     * libcurl SMTP takes headers from the mime part's own header list
     * when CURLOPT_MIMEPOST is used, but Subject/From/To need to be on
     * the top-level message. */
    struct curl_slist *msg_headers = NULL;
    msg_headers = curl_slist_append(msg_headers, headers_buf);
    curl_mime_headers(curl_mime_addpart(mime), msg_headers, 1);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[emailer] send failed: %s\n", curl_easy_strerror(res));
    }

    curl_slist_free_all(recipients);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return res;
}

int emailer_send(const emailer_config_t *cfg, const char *subject,
                  const char *body, const char *attachment_jpeg_path)
{
    pthread_mutex_lock(&g_debounce_mutex);
    time_t now = time(NULL);
    if (g_last_sent != 0 && (now - g_last_sent) < EMAIL_DEBOUNCE_SECONDS) {
        pthread_mutex_unlock(&g_debounce_mutex);
        return 0; /* suppressed */
    }
    g_last_sent = now;
    pthread_mutex_unlock(&g_debounce_mutex);

    CURLcode res = do_send(cfg, subject, body, attachment_jpeg_path);
    return (res == CURLE_OK) ? 1 : -1;
}

int emailer_send_person_detected(const emailer_config_t *cfg, int person_count,
                                  double cpu_temp_c, const char *jpeg_path)
{
    char subject[128], body[512];
    time_t now = time(NULL);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    snprintf(subject, sizeof(subject), "[Security] %d person(s) detected", person_count);
    snprintf(body, sizeof(body),
             "Person detection event\nTimestamp: %s\nPersons detected: %d\nCPU temperature: %.1f C\n",
             timebuf, person_count, cpu_temp_c);
    return emailer_send(cfg, subject, body, jpeg_path);
}

int emailer_send_guard_alarm(const emailer_config_t *cfg, int person_count, const char *jpeg_path)
{
    char subject[128], body[512];
    time_t now = time(NULL);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    snprintf(subject, sizeof(subject), "[ALARM] Guard mode: %d person(s) detected", person_count);
    snprintf(body, sizeof(body),
             "GUARD MODE ALARM\nTimestamp: %s\nPersons detected: %d\n"
             "This event was published to the MQTT alarm topic and this email was sent immediately (no debounce delay was skipped, but note the global 30s minimum gap still applies).\n",
             timebuf, person_count);
    return emailer_send(cfg, subject, body, jpeg_path);
}

int emailer_send_thermal_event(const emailer_config_t *cfg, double cpu_temp_c,
                                int new_fps, int new_width, int new_height)
{
    char subject[128], body[512];
    snprintf(subject, sizeof(subject), "[Security] Thermal throttling engaged (%.1f C)", cpu_temp_c);
    snprintf(body, sizeof(body),
             "Adaptive thermal management triggered.\nCPU temperature: %.1f C\n"
             "New target FPS: %d\nNew resolution: %dx%d\n",
             cpu_temp_c, new_fps, new_width, new_height);
    return emailer_send(cfg, subject, body, NULL);
}

int emailer_send_watchdog_alert(const emailer_config_t *cfg, const char *reason)
{
    char subject[128], body[512];
    snprintf(subject, sizeof(subject), "[Security] Camera/watchdog alert");
    snprintf(body, sizeof(body), "Software watchdog triggered.\nReason: %s\n", reason);
    return emailer_send(cfg, subject, body, NULL);
}
