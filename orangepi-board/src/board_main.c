/*
 * board_main.c
 * -----------------------------------------------------------------------
 * Entry point for the Orange Pi board process. Started by systemd
 * after the network and the laptop-stream dependency are up.
 *
 * Usage:
 *   ./security-board <student_id> <student_name> <laptop_stream_url> [detector_stream_url]
 */

#include "board_state.h"
#include "https_server.h"
#include "sensor_thread.h"
#include "stream_proxy.h"
#include "mqtt_client.h"
#include "blackbox.h"
#include "watchdog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

int main(int argc, char *argv[])
{
    setvbuf(stderr, NULL, _IONBF, 0); /* ensure diagnostic logs are never lost to buffering */

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <student_id> <student_name> <laptop_stream_url> [detector_stream_url]\n", argv[0]);
        fprintf(stderr, "Example: %s 99123456 \"Jane Doe\" http://192.168.1.50:8090/stream.mjpg http://127.0.0.1:8091/annotated.mjpg\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN); /* a client closing mid-stream shouldn't kill us */

    const char *student_id = argv[1];
    const char *student_name = argv[2];
    const char *laptop_url = argv[3];
    const char *detector_stream_url = (argc >= 5) ? argv[4] : NULL;

    board_state_init(&g_state, student_id, student_name, laptop_url);

    const char *blackbox_path = getenv("SECURITY_BLACKBOX_DB")
        ? getenv("SECURITY_BLACKBOX_DB") : "/var/lib/smart-security/blackbox.db";
    if (blackbox_init(blackbox_path) != 0) {
        fprintf(stderr, "[board_main] WARNING: black box (SQLite) init failed at %s -- "
                        "/api/v1/detections/total will report 0 until this is fixed\n", blackbox_path);
    }

    g_state.camera_proxy = stream_proxy_start(laptop_url, "camera");
    if (!g_state.camera_proxy) {
        fprintf(stderr, "[board_main] WARNING: failed to start camera stream proxy for %s\n", laptop_url);
    }

    if (detector_stream_url) {
        g_state.detector_proxy = stream_proxy_start(detector_stream_url, "detector");
        if (!g_state.detector_proxy) {
            fprintf(stderr, "[board_main] WARNING: failed to start detector stream proxy for %s\n",
                    detector_stream_url);
        }
    } else {
        fprintf(stderr, "[board_main] no detector_stream_url given -- "
                        "/api/v1/detector-stream will return 404 until one is configured\n");
    }

    if (sensor_thread_start(&g_state) != 0) {
        fprintf(stderr, "[board_main] FATAL: failed to start sensor thread\n");
        return 1;
    }

    if (watchdog_start(&g_state) != 0) {
        fprintf(stderr, "[board_main] WARNING: failed to start software watchdog\n");
    }

    mqtt_config_t mqtt_cfg;
    if (mqtt_config_load(&mqtt_cfg, NULL) == 0) {
        if (mqtt_client_start(&mqtt_cfg, &g_state) != 0) {
            fprintf(stderr, "[board_main] WARNING: failed to start MQTT client\n");
        }
    } else {
        fprintf(stderr, "[board_main] WARNING: MQTT not configured -- "
                        "persons/telemetry/alarm topics will not be published\n");
    }

    https_server_config_t https_cfg = {
        .https_port = getenv("SECURITY_HTTPS_PORT") ? atoi(getenv("SECURITY_HTTPS_PORT")) : 443,
        .http_redirect_port = getenv("SECURITY_HTTP_PORT") ? atoi(getenv("SECURITY_HTTP_PORT")) : 80,
    };
    strncpy(https_cfg.cert_path,
            getenv("SECURITY_SSL_CERT") ? getenv("SECURITY_SSL_CERT") : "/etc/smart-security/ssl/server.crt",
            sizeof(https_cfg.cert_path) - 1);
    strncpy(https_cfg.key_path,
            getenv("SECURITY_SSL_KEY") ? getenv("SECURITY_SSL_KEY") : "/etc/smart-security/ssl/server.key",
            sizeof(https_cfg.key_path) - 1);
    strncpy(https_cfg.web_root,
            getenv("SECURITY_WEB_ROOT") ? getenv("SECURITY_WEB_ROOT") : "/opt/smart-security/web",
            sizeof(https_cfg.web_root) - 1);
    if (https_server_start(&https_cfg, &g_state) != 0) {
        fprintf(stderr, "[board_main] FATAL: failed to start HTTPS server\n");
        return 1;
    }

    fprintf(stderr, "[board_main] all subsystems started for student_id=%s\n", student_id);

    /* Main thread just idles; all real work happens in the started
     * threads. Sleeping in a loop (rather than pause()) also gives us
     * a natural place to add periodic housekeeping later if needed. */
    while (1) {
        sleep(60);
    }

    return 0;
}
