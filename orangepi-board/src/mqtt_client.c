#include "mqtt_client.h"
#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define MQTT_QOS 1
#define PUBLISH_INTERVAL_SECONDS 2
#define KEEPALIVE_SECONDS 30

static struct mosquitto *g_mosq = NULL;
static pthread_mutex_t g_mosq_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_connected = 0;
static char g_topic_alarm[MAX_TOPIC_LEN];
static char g_topic_status[MAX_TOPIC_LEN];

static void safe_copy(char *dst, size_t dst_sz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int read_kv_file(const char *path, mqtt_config_t *cfg)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char key[128], value[256];
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        size_t klen = eq - line;
        if (klen >= sizeof(key)) continue;
        strncpy(key, line, klen);
        key[klen] = '\0';
        safe_copy(value, sizeof(value), eq + 1);

        if (strcmp(key, "MQTT_HOST") == 0) safe_copy(cfg->broker_host, sizeof(cfg->broker_host), value);
        else if (strcmp(key, "MQTT_PORT") == 0) cfg->broker_port = atoi(value);
        else if (strcmp(key, "MQTT_USER") == 0) safe_copy(cfg->username, sizeof(cfg->username), value);
        else if (strcmp(key, "MQTT_PASS") == 0) safe_copy(cfg->password, sizeof(cfg->password), value);
    }
    fclose(f);
    return 0;
}

int mqtt_config_load(mqtt_config_t *cfg, const char *config_path)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->broker_port = 1883;
    const char *path = config_path ? config_path : "/etc/smart-security/mqtt.conf";

    read_kv_file(path, cfg); /* best-effort; env vars below can still fill gaps */

    const char *v;
    if (cfg->broker_host[0] == '\0' && (v = getenv("MQTT_HOST"))) safe_copy(cfg->broker_host, sizeof(cfg->broker_host), v);
    if ((v = getenv("MQTT_PORT"))) cfg->broker_port = atoi(v);
    if (cfg->username[0] == '\0' && (v = getenv("MQTT_USER"))) safe_copy(cfg->username, sizeof(cfg->username), v);
    if (cfg->password[0] == '\0' && (v = getenv("MQTT_PASS"))) safe_copy(cfg->password, sizeof(cfg->password), v);

    if (cfg->broker_host[0] == '\0') {
        fprintf(stderr, "[mqtt] WARNING: no MQTT_HOST configured (checked %s and env)\n", path);
        return -1;
    }
    return 0;
}

static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;
    if (rc == 0) {
        fprintf(stderr, "[mqtt] connected to broker\n");
        pthread_mutex_lock(&g_mosq_mutex);
        g_connected = 1;
        pthread_mutex_unlock(&g_mosq_mutex);

        /* Announce ourselves online now that we're connected (the LWT
         * "offline" message only fires on an unexpected drop. on a
         * clean connect we publish the positive counterpart ourselves,
         * retained, so a freshly-subscribing client immediately knows
         * our last known state either way). */
        mosquitto_publish(mosq, NULL, g_topic_status, strlen("online"), "online", MQTT_QOS, true);
    } else {
        fprintf(stderr, "[mqtt] connection failed, rc=%d (%s)\n", rc, mosquitto_connack_string(rc));
    }
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;
    fprintf(stderr, "[mqtt] disconnected, rc=%d\n", rc);
    pthread_mutex_lock(&g_mosq_mutex);
    g_connected = 0;
    pthread_mutex_unlock(&g_mosq_mutex);
}

static void format_iso_time(time_t t, char *out, size_t out_sz)
{
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    strftime(out, out_sz, "%Y-%m-%dT%H:%M:%S", &tm_buf);
}

static void *publish_loop(void *arg)
{
    board_state_t *st = (board_state_t *)arg;

    while (1) {
        sleep(PUBLISH_INTERVAL_SECONDS);

        pthread_mutex_lock(&g_mosq_mutex);
        int connected = g_connected;
        pthread_mutex_unlock(&g_mosq_mutex);
        if (!connected || !g_mosq) continue;

        pthread_mutex_lock(&st->mutex);
        int person_count = st->person_count;
        double temp = st->cpu_temp_c;
        char topic_persons[MAX_TOPIC_LEN];
        char topic_telemetry[MAX_TOPIC_LEN];
        safe_copy(topic_persons, sizeof(topic_persons), st->topic_persons);
        safe_copy(topic_telemetry, sizeof(topic_telemetry), st->topic_telemetry);
        pthread_mutex_unlock(&st->mutex);

        char timebuf[64];
        format_iso_time(time(NULL), timebuf, sizeof(timebuf));

        char payload[256];
        int n = snprintf(payload, sizeof(payload),
                          "{\"person_count\":%d,\"timestamp\":\"%s\"}", person_count, timebuf);
        mosquitto_publish(g_mosq, NULL, topic_persons, n, payload, MQTT_QOS, false);

        n = snprintf(payload, sizeof(payload),
                     "{\"cpu_temp_c\":%.1f,\"timestamp\":\"%s\"}", temp, timebuf);
        mosquitto_publish(g_mosq, NULL, topic_telemetry, n, payload, MQTT_QOS, false);
    }
    return NULL;
}

int mqtt_client_start(const mqtt_config_t *cfg, board_state_t *st)
{
    mosquitto_lib_init();

    /* Client ID derived from student_id keeps it unique and traceable
     * on the broker side without hard-coding anything secret. */
    char client_id[64];
    snprintf(client_id, sizeof(client_id), "board-%s", st->student_id);

    g_mosq = mosquitto_new(client_id, true, st);
    if (!g_mosq) {
        fprintf(stderr, "[mqtt] mosquitto_new failed\n");
        return -1;
    }

    if (cfg->username[0] != '\0') {
        mosquitto_username_pw_set(g_mosq, cfg->username, cfg->password);
    }

    snprintf(g_topic_status, sizeof(g_topic_status), "status/%s/home", st->student_id);
    safe_copy(g_topic_alarm, sizeof(g_topic_alarm), st->topic_alarm);

    /* Last Will and Testament: if the board drops off the network
     * without a clean disconnect, the broker publishes this on our
     * behalf so the PC-side subscriber immediately knows. */
    const char *lwt_payload = "offline";
    mosquitto_will_set(g_mosq, g_topic_status, (int)strlen(lwt_payload), lwt_payload, MQTT_QOS, true);

    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);

    int rc = mosquitto_connect(g_mosq, cfg->broker_host, cfg->broker_port, KEEPALIVE_SECONDS);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[mqtt] initial connect failed: %s (will keep retrying in background)\n",
                mosquitto_strerror(rc));
    }

    /* mosquitto_loop_start spins up libmosquitto's own background
     * thread that handles the network loop, keepalives, and automatic
     * reconnection -- we don't need to hand-roll a reconnect loop. */
    mosquitto_loop_start(g_mosq);

    pthread_t tid;
    if (pthread_create(&tid, NULL, publish_loop, st) != 0) {
        fprintf(stderr, "[mqtt] failed to start publish loop thread\n");
        return -1;
    }
    pthread_detach(tid);

    return 0;
}

int mqtt_client_publish_alarm(board_state_t *st, int person_count)
{
    pthread_mutex_lock(&g_mosq_mutex);
    int connected = g_connected;
    pthread_mutex_unlock(&g_mosq_mutex);
    if (!connected || !g_mosq) return -1;

    char timebuf[64];
    format_iso_time(time(NULL), timebuf, sizeof(timebuf));

    char payload[256];
    int n = snprintf(payload, sizeof(payload),
                      "{\"alarm\":true,\"person_count\":%d,\"timestamp\":\"%s\"}",
                      person_count, timebuf);

    int rc = mosquitto_publish(g_mosq, NULL, st->topic_alarm, n, payload, MQTT_QOS, false);
    return (rc == MOSQ_ERR_SUCCESS) ? 0 : -1;
}
