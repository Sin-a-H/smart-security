#include "https_server.h"
#include "api_handlers.h"
#include "stream_proxy.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

#define REQ_BUF_SIZE   8192
#define RESP_BUF_SIZE  65536

static https_server_config_t g_cfg;
static board_state_t *g_st;
static SSL_CTX *g_ssl_ctx;

static const char *memmem_(const char *haystack, size_t hlen, const char *needle, size_t nlen);
static const char *strcasestr_(const char *haystack, const char *needle);

/* ---------------------------------------------------------------- *
 * Generic small helpers
 * ---------------------------------------------------------------- */

typedef struct {
    char method[8];
    char path[512];
    char body[4096];
} http_request_t;

/* HTTP/1.1 request-line + headers + body parser. */
static int parse_http_request(const char *raw, size_t raw_len, http_request_t *req)
{
    memset(req, 0, sizeof(*req));

    const char *line_end = memchr(raw, '\n', raw_len);
    if (!line_end) return -1;

    if (sscanf(raw, "%7s %511s", req->method, req->path) != 2) {
        return -1;
    }

    /* Find end of headers (blank line), then copy remaining as body. */
    const char *hdr_end = memmem_(raw, raw_len, "\r\n\r\n", 4);
    if (hdr_end) {
        const char *body_start = hdr_end + 4;
        size_t body_len = raw_len - (body_start - raw);
        if (body_len >= sizeof(req->body)) body_len = sizeof(req->body) - 1;
        memcpy(req->body, body_start, body_len);
        req->body[body_len] = '\0';
    }
    return 0;
}

/* Local memmem shim in case _GNU_SOURCE isn't picked up on some libc
 * configurations for this translation unit. */
static const char *memmem_(const char *haystack, size_t hlen, const char *needle, size_t nlen)
{
    if (nlen == 0 || hlen < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static int send_all(SSL *ssl, int plain_fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n;
        if (ssl) {
            n = SSL_write(ssl, data + sent, (int)(len - sent));
        } else {
            n = (int)write(plain_fd, data + sent, len - sent);
        }
        if (n <= 0) {
            return -1; /* caller decides whether/how to log this */
        }
        sent += (size_t)n;
    }
    return 0;
}

static void send_response(SSL *ssl, int plain_fd, int status, const char *content_type,
                           const char *body, size_t body_len)
{
    char header[512];
    const char *status_text = (status == 200) ? "OK" :
                               (status == 301) ? "Moved Permanently" :
                               (status == 400) ? "Bad Request" :
                               (status == 404) ? "Not Found" : "Error";
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        status, status_text, content_type, body_len);
    if (send_all(ssl, plain_fd, header, (size_t)hlen) != 0) {
        fprintf(stderr, "[https_server] client disconnected before response headers were sent\n");
        return;
    }
    if (body_len > 0) {
        send_all(ssl, plain_fd, body, body_len); /* client may still vanish mid-body; nothing more to do either way */
    }
}

static void send_json(SSL *ssl, int plain_fd, int status, const char *json, size_t json_len)
{
    send_response(ssl, plain_fd, status, "application/json", json, json_len);
}

/* ---------------------------------------------------------------- *
 * HTML dashboard page
 * ---------------------------------------------------------------- */

static void serve_index_html(SSL *ssl, int plain_fd)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/index.html", g_cfg.web_root);
    FILE *f = fopen(path, "rb");
    if (!f) {
        const char *msg = "index.html not found on board";
        send_response(ssl, plain_fd, 404, "text/plain", msg, strlen(msg));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)sz);
    size_t read_bytes = fread(buf, 1, (size_t)sz, f);
    if (read_bytes != (size_t)sz) {
        fprintf(stderr, "[https_server] WARNING: short read on %s (%zu of %ld bytes)\n",
                path, read_bytes, sz);
    }
    fclose(f);

    send_response(ssl, plain_fd, 200, "text/html", buf, (size_t)sz);
    free(buf);
}

static void serve_mjpeg_stream(SSL *ssl, int plain_fd, stream_proxy_t *proxy)
{
    if (!proxy) {
        const char *msg = "{\"error\":\"stream not configured\"}";
        send_json(ssl, plain_fd, 404, msg, strlen(msg));
        return;
    }

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=FRAME\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
    if (send_all(ssl, plain_fd, header, (size_t)hlen) != 0) {
        return; /* client vanished before the stream even started; nothing to log, this is routine */
    }

    static __thread unsigned char frame_buf[2 * 1024 * 1024];

    for (int i = 0; i < 100000; i++) { /* bounded loop: ~ long-lived but not infinite per thread */
        int n = stream_proxy_get_latest_frame(proxy, frame_buf, sizeof(frame_buf));
        if (n > 0) {
            char part_hdr[128];
            int phlen = snprintf(part_hdr, sizeof(part_hdr),
                "--FRAME\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", n);
            if (send_all(ssl, plain_fd, part_hdr, (size_t)phlen) != 0) break;
            if (send_all(ssl, plain_fd, (char *)frame_buf, (size_t)n) != 0) break;
            if (send_all(ssl, plain_fd, "\r\n", 2) != 0) break;
        }
        usleep(66000); /* ~15fps output cap regardless of source rate */
    }
}

/* ---------------------------------------------------------------- *
 * Routing
 * ---------------------------------------------------------------- */

static void route_request(SSL *ssl, int plain_fd, const http_request_t *req)
{
    char json_buf[RESP_BUF_SIZE];
    int status = 200;
    int n;

    if (strcmp(req->path, "/") == 0 && strcmp(req->method, "GET") == 0) {
        serve_index_html(ssl, plain_fd);

    } else if (strcasecmp(req->path, "/api/v1/stream") == 0 && strcmp(req->method, "GET") == 0) {
        serve_mjpeg_stream(ssl, plain_fd, g_st->camera_proxy);

    } else if (strcasecmp(req->path, "/api/v1/detector-stream") == 0 && strcmp(req->method, "GET") == 0) {
        serve_mjpeg_stream(ssl, plain_fd, g_st->detector_proxy);

    } else if (strcasecmp(req->path, "/api/v1/persons") == 0 && strcmp(req->method, "GET") == 0) {
        n = api_handle_persons(g_st, json_buf, sizeof(json_buf), &status);
        send_json(ssl, plain_fd, status, json_buf, (size_t)n);

    } else if (strcasecmp(req->path, "/api/v1/telemetry") == 0 && strcmp(req->method, "GET") == 0) {
        n = api_handle_telemetry(g_st, json_buf, sizeof(json_buf), &status);
        send_json(ssl, plain_fd, status, json_buf, (size_t)n);

    } else if (strcasecmp(req->path, "/api/v1/history") == 0 && strcmp(req->method, "GET") == 0) {
        n = api_handle_history(g_st, json_buf, sizeof(json_buf), &status);
        send_json(ssl, plain_fd, status, json_buf, (size_t)n);

    } else if (strcasecmp(req->path, "/api/v1/command") == 0 && strcmp(req->method, "POST") == 0) {
        n = api_handle_command(g_st, req->body, json_buf, sizeof(json_buf), &status);
        send_json(ssl, plain_fd, status, json_buf, (size_t)n);

    } else if (strcasecmp(req->path, "/api/v1/guard-mode") == 0 && strcmp(req->method, "POST") == 0) {
        n = api_handle_guard_mode(g_st, req->body, json_buf, sizeof(json_buf), &status);
        send_json(ssl, plain_fd, status, json_buf, (size_t)n);

    } else if (strcasecmp(req->path, "/api/v1/detections/total") == 0 && strcmp(req->method, "GET") == 0) {
        n = api_handle_total_detections(g_st, json_buf, sizeof(json_buf), &status);
        send_json(ssl, plain_fd, status, json_buf, (size_t)n);

    } else if (strcasecmp(req->path, "/internal/detector-update") == 0 && strcmp(req->method, "POST") == 0) {
        n = api_handle_detector_update(g_st, req->body, json_buf, sizeof(json_buf), &status);
        send_json(ssl, plain_fd, status, json_buf, (size_t)n);

    } else {
        const char *msg = "{\"error\":\"not found\"}";
        send_json(ssl, plain_fd, 404, msg, strlen(msg));
    }
}

/* ---------------------------------------------------------------- *
 * Per-connection thread (HTTPS)
 * ---------------------------------------------------------------- */

typedef struct {
    int client_fd;
} conn_arg_t;

static void *handle_https_connection(void *arg)
{
    conn_arg_t *ca = (conn_arg_t *)arg;
    int client_fd = ca->client_fd;
    free(ca);

    SSL *ssl = SSL_new(g_ssl_ctx);
    if (!ssl) {
        fprintf(stderr, "[https_server] SSL_new failed\n");
        close(client_fd);
        return NULL;
    }
    SSL_set_fd(ssl, client_fd);

    int accept_rc = SSL_accept(ssl);
    if (accept_rc <= 0) {
        int ssl_err = SSL_get_error(ssl, accept_rc);
        // fprintf(stderr, "[https_server] SSL_accept failed: rc=%d ssl_err=%d\n", accept_rc, ssl_err);
        SSL_free(ssl);
        close(client_fd);
        return NULL;
    }

    static __thread char req_buf[REQ_BUF_SIZE];
    size_t total_read = 0;
    int have_full_request = 0;
    long content_length = -1;
    size_t header_len = 0;

    while (total_read < sizeof(req_buf) - 1) {
        int n = SSL_read(ssl, req_buf + total_read, (int)(sizeof(req_buf) - 1 - total_read));
        if (n <= 0) {
            break; /* client disconnected or a real error; handle what we have, if anything */
        }
        total_read += (size_t)n;
        req_buf[total_read] = '\0';

        char *hdr_end = strstr(req_buf, "\r\n\r\n");
        if (!hdr_end) {
            continue;
        }
        header_len = (size_t)(hdr_end - req_buf) + 4;

        if (content_length < 0) {
            /* Parse Content-Length once we can see the full header block. */
            const char *cl = strcasestr_(req_buf, "Content-Length:");
            if (cl) {
                content_length = atol(cl + strlen("Content-Length:"));
            } else {
                content_length = 0; /* no body expected (GET, etc.) */
            }
        }

        size_t body_so_far = total_read - header_len;
        if ((long)body_so_far >= content_length) {
            have_full_request = 1;
            break;
        }
    }

    if (total_read > 0) {
        http_request_t req;
        if (parse_http_request(req_buf, total_read, &req) == 0) {
            route_request(ssl, -1, &req);
        } else {
            fprintf(stderr, "[https_server] failed to parse HTTP request (n=%zu bytes)\n", total_read);
        }
        (void)have_full_request; /* header_len/content_length already guided the read loop above */
    }

    if (SSL_shutdown(ssl) == 0) {
        SSL_shutdown(ssl);
    }

    SSL_free(ssl);
    close(client_fd);
    return NULL;
}

static void *https_accept_loop(void *arg)
{
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)g_cfg.https_port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("[https_server] bind failed (are you running as root/CAP_NET_BIND_SERVICE for port <1024?)");
        return NULL;
    }
    listen(server_fd, 128);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            fprintf(stderr, "[https_server] accept() failed: errno=%d (%s)\n", errno, strerror(errno));
            continue;
        }

        conn_arg_t *ca = malloc(sizeof(conn_arg_t));
        ca->client_fd = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_https_connection, ca) == 0) {
            pthread_detach(tid);
        } else {
            fprintf(stderr, "[https_server] pthread_create failed for new connection\n");
            close(client_fd);
            free(ca);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- *
 * Plain-HTTP redirect-only listener
 * ---------------------------------------------------------------- */

static void *handle_redirect_connection(void *arg)
{
    conn_arg_t *ca = (conn_arg_t *)arg;
    int client_fd = ca->client_fd;
    free(ca);

    char req_buf[REQ_BUF_SIZE];
    int n = read(client_fd, req_buf, sizeof(req_buf) - 1);
    if (n > 0) {
        req_buf[n] = '\0';
        char method[8], path[512];
        char host_header[256] = {0};
        sscanf(req_buf, "%7s %511s", method, path);

        /* pull Host header so the redirect points at the same host the
         * client actually used, rather than a hard-coded hostname */
        const char *host_line = strcasestr_(req_buf, "Host:");
        if (host_line) {
            sscanf(host_line, "Host: %255s", host_header);
            /* strip trailing \r if present */
            char *cr = strchr(host_header, '\r');
            if (cr) *cr = '\0';
            /* strip any :80 suffix since we're redirecting to :443 */
            char *colon = strchr(host_header, ':');
            if (colon) *colon = '\0';
        }
        if (host_header[0] == '\0') {
            strcpy(host_header, "localhost");
        }

        char location[600];
        snprintf(location, sizeof(location), "https://%s%s", host_header, path);

        char response[800];
        int rlen = snprintf(response, sizeof(response),
            "HTTP/1.1 301 Moved Permanently\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            location);
        if (write(client_fd, response, (size_t)rlen) < 0) {
            /* Best-effort redirect; client already gone is not fatal. */
        }
    }
    close(client_fd);
    return NULL;
}

static const char *strcasestr_(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

static void *http_redirect_accept_loop(void *arg)
{
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)g_cfg.http_redirect_port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("[https_server] redirect listener bind failed");
        return NULL;
    }
    listen(server_fd, 32);
    fprintf(stderr, "[https_server] HTTP->HTTPS redirect listening on 0.0.0.0:%d\n", g_cfg.http_redirect_port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;

        conn_arg_t *ca = malloc(sizeof(conn_arg_t));
        ca->client_fd = client_fd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_redirect_connection, ca) == 0) {
            pthread_detach(tid);
        } else {
            close(client_fd);
            free(ca);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- *
 * Public entry point
 * ---------------------------------------------------------------- */

int https_server_start(const https_server_config_t *cfg, board_state_t *st)
{
    g_cfg = *cfg;
    g_st = st;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    g_ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!g_ssl_ctx) {
        ERR_print_errors_fp(stderr);
        return -1;
    }

    if (SSL_CTX_use_certificate_file(g_ssl_ctx, cfg->cert_path, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, cfg->key_path, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        return -1;
    }
    /* Harden the negotiated protocol/cipher set a little; self-signed
     * cert is still self-signed, but no reason to allow old TLS. */
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);

    pthread_t https_tid, redirect_tid;
    pthread_create(&https_tid, NULL, https_accept_loop, NULL);
    pthread_detach(https_tid);

    pthread_create(&redirect_tid, NULL, http_redirect_accept_loop, NULL);
    pthread_detach(redirect_tid);

    return 0;
}
