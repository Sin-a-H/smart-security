/* _GNU_SOURCE is supplied by the Makefile (-D_GNU_SOURCE) for memmem()
 * and strcasestr(); no need to redefine it here. */
#include "stream_proxy.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#define MAX_FRAME_BYTES   (2 * 1024 * 1024)  /* 2MB safety cap per JPEG frame */
#define RECV_CHUNK        8192

static unsigned char g_frame_buf[MAX_FRAME_BYTES];
static size_t g_frame_len = 0;
static pthread_mutex_t g_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static long g_last_frame_time = 0;

static char g_host[256];
static char g_path[256];
static int  g_port = 80;

/* Parses a URL of the form http://host:port/path into components.
 * Minimal by design: this only ever talks to our own laptop_cam_server. */
static int parse_url(const char *url, char *host, size_t host_sz, int *port, char *path, size_t path_sz)
{
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else return -1;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    size_t host_len;
    if (colon && (!slash || colon < slash)) {
        host_len = colon - p;
        *port = atoi(colon + 1);
    } else {
        host_len = slash ? (size_t)(slash - p) : strlen(p);
        *port = 80;
    }
    if (host_len >= host_sz) return -1;
    memcpy(host, p, host_len);
    host[host_len] = '\0';

    if (slash) {
        strncpy(path, slash, path_sz - 1);
        path[path_sz - 1] = '\0';
    } else {
        strncpy(path, "/", path_sz - 1);
    }
    return 0;
}

static int connect_to(const char *host, int port)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return sock;
}

/* Reads from sock into a growable ring-free linear buffer, scanning for
 * MJPEG multipart frames: "--FRAME\r\nContent-Type: ...\r\nContent-Length: N\r\n\r\n<N bytes>\r\n"
 * This is intentionally tolerant of exact header ordering/case since we
 * control the server (laptop_cam_server.py) but keep it robust anyway. */
static void *proxy_loop(void *arg)
{
    (void)arg;
    static unsigned char recv_buf[MAX_FRAME_BYTES];
    size_t buf_len = 0;

    while (1) {
        int sock = connect_to(g_host, g_port);
        if (sock < 0) {
            fprintf(stderr, "[stream_proxy] cannot connect to %s:%d, retrying in 3s\n", g_host, g_port);
            sleep(3);
            continue;
        }

        char request[768];
        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n",
                 g_path, g_host);
        if (send(sock, request, strlen(request), 0) < 0) {
            close(sock);
            sleep(2);
            continue;
        }

        buf_len = 0;
        int header_skipped = 0;

        while (1) {
            ssize_t n = recv(sock, recv_buf + buf_len, sizeof(recv_buf) - buf_len, 0);
            if (n <= 0) {
                fprintf(stderr, "[stream_proxy] connection lost, reconnecting\n");
                break;
            }
            buf_len += (size_t)n;

            /* Skip the initial HTTP response header once. */
            if (!header_skipped) {
                unsigned char *hdr_end = (unsigned char *)memmem(recv_buf, buf_len, "\r\n\r\n", 4);
                if (!hdr_end) {
                    if (buf_len >= sizeof(recv_buf)) buf_len = 0; /* malformed, reset */
                    continue;
                }
                size_t consumed = (hdr_end - recv_buf) + 4;
                memmove(recv_buf, hdr_end + 4, buf_len - consumed);
                buf_len -= consumed;
                header_skipped = 1;
            }

            /* Now scan for one or more complete multipart frames. */
            while (1) {
                unsigned char *boundary = (unsigned char *)memmem(recv_buf, buf_len, "--FRAME", 7);
                if (!boundary) break;

                unsigned char *part_hdr_end = (unsigned char *)memmem(
                    boundary, buf_len - (boundary - recv_buf), "\r\n\r\n", 4);
                if (!part_hdr_end) break; /* need more data */

                /* Extract Content-Length from the part header text. */
                char part_headers[256] = {0};
                size_t hdr_len = (size_t)(part_hdr_end - boundary);
                if (hdr_len >= sizeof(part_headers)) hdr_len = sizeof(part_headers) - 1;
                memcpy(part_headers, boundary, hdr_len);

                char *cl = strcasestr(part_headers, "Content-Length:");
                if (!cl) break;
                long content_len = atol(cl + strlen("Content-Length:"));
                if (content_len <= 0 || content_len > MAX_FRAME_BYTES) break;

                unsigned char *jpeg_start = part_hdr_end + 4;
                size_t available_after_header = buf_len - (jpeg_start - recv_buf);
                if (available_after_header < (size_t)content_len) break; /* need more data */

                /* We have a full frame. */
                pthread_mutex_lock(&g_frame_mutex);
                memcpy(g_frame_buf, jpeg_start, (size_t)content_len);
                g_frame_len = (size_t)content_len;
                g_last_frame_time = time(NULL);
                pthread_mutex_unlock(&g_frame_mutex);

                size_t consumed = (jpeg_start - recv_buf) + content_len;
                memmove(recv_buf, recv_buf + consumed, buf_len - consumed);
                buf_len -= consumed;
            }

            if (buf_len >= sizeof(recv_buf) - 1) {
                /* Shouldn't normally happen; guard against runaway growth. */
                buf_len = 0;
            }
        }

        close(sock);
        sleep(1);
    }
    return NULL;
}

int stream_proxy_start(const char *laptop_url)
{
    if (parse_url(laptop_url, g_host, sizeof(g_host), &g_port, g_path, sizeof(g_path)) != 0) {
        fprintf(stderr, "[stream_proxy] failed to parse URL: %s\n", laptop_url);
        return -1;
    }

    pthread_t tid;
    int rc = pthread_create(&tid, NULL, proxy_loop, NULL);
    if (rc != 0) return -1;
    pthread_detach(tid);
    return 0;
}

int stream_proxy_get_latest_frame(unsigned char *buf, size_t buf_size)
{
    pthread_mutex_lock(&g_frame_mutex);
    size_t len = g_frame_len;
    int result;
    if (len == 0) {
        result = 0;
    } else if (len > buf_size) {
        result = -1;
    } else {
        memcpy(buf, g_frame_buf, len);
        result = (int)len;
    }
    pthread_mutex_unlock(&g_frame_mutex);
    return result;
}

long stream_proxy_last_frame_time(void)
{
    pthread_mutex_lock(&g_frame_mutex);
    long t = g_last_frame_time;
    pthread_mutex_unlock(&g_frame_mutex);
    return t;
}
