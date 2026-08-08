/*
 * mqtt_client.h
 * -----------------------------------------------------------------------
 * MQTT publishing, written in C using libmosquitto.
 *
 * Publishes:
 *   - persons/<student_id>/home    : {"person_count": N, "timestamp": "..."}
 *   - telemetry/<student_id>/home  : {"cpu_temp_c": X, "timestamp": "..."}
 *   - alarm/<student_id>/home      : guard-mode instant alerts
 *
 * All publishes use QoS 1 (at-least-once) per the spec. A Last Will and
 * Testament (LWT) is registered at connect time on a dedicated status
 * topic so the broker announces this client's unexpected disconnection
 * to anyone subscribed.
 *
 * Authentication: credentials are loaded from a config file at runtime.
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "board_state.h"

typedef struct {
    char broker_host[256];
    int  broker_port;
    char username[128];
    char password[128];
} mqtt_config_t;

/*
 * Loads broker connection details + credentials from a config file
 * (default: /etc/smart-security/mqtt.conf, simple KEY=VALUE lines:
 * MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS) or environment variables
 * of the same names as a fallback. Returns 0 on success.
 */
int mqtt_config_load(mqtt_config_t *cfg, const char *config_path);

/*
 * Connects to the broker (with LWT pre-registered on
 * "status/<student_id>/home" = "offline"), and starts a background
 * thread that keeps the connection alive (handles reconnects) and
 * publishes persons/telemetry every time board_state changes are detected
 * (polled at a small interval; simpler and more robust than wiring
 * a callback through every state-mutation call site). Returns 0 on success.
 */
int mqtt_client_start(const mqtt_config_t *cfg, board_state_t *st);

/*
 * Publishes an immediate guard-mode alarm message on
 * "alarm/<student_id>/home" with QoS 1, bypassing the normal periodic
 * publish cycle Returns 0 on success, -1 if not currently connected to the broker.
 */
int mqtt_client_publish_alarm(board_state_t *st, int person_count);

#endif /* MQTT_CLIENT_H */
