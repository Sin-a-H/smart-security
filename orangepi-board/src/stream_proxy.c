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

struct stream_proxy {
    char host[256];
    char path[256];
    int  port;
    char label[32];

    pthread_mutex_t frame_mutex;
    unsigned char *frame_buf;   /* MAX_FRAME_BYTES, heap-allocated per instance */
    size_t frame_len;
    long last_frame_time;
};

/* Parses a URL of the form http://host:port/path into components. */
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
 * control the sources but keep it robust anyway. */
static void *proxy_loop(void *arg)
{
    stream_proxy_t *proxy = (stream_proxy_t *)arg;
    unsigned char *recv_buf = malloc(MAX_FRAME_BYTES);
    if (!recv_buf) {
        fprintf(stderr, "[stream_proxy:%s] failed to allocate receive buffer\n", proxy->label);
        return NULL;
    }
    size_t buf_len = 0;

    while (1) {
        int sock = connect_to(proxy->host, proxy->port);
        if (sock < 0) {
            fprintf(stderr, "[stream_proxy:%s] cannot connect to %s:%d, retrying in 3s\n",
                    proxy->label, proxy->host, proxy->port);
            sleep(3);
            continue;
        }

        char request[768];
        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n",
                 proxy->path, proxy->host);
        if (send(sock, request, strlen(request), 0) < 0) {
            close(sock);
            sleep(2);
            continue;
        }

        buf_len = 0;
        int header_skipped = 0;

        while (1) {
            ssize_t n = recv(sock, recv_buf + buf_len, MAX_FRAME_BYTES - buf_len, 0);
            if (n <= 0) {
                fprintf(stderr, "[stream_proxy:%s] connection lost, reconnecting\n", proxy->label);
                break;
            }
            buf_len += (size_t)n;

            /* Skip the initial HTTP response header once. */
            if (!header_skipped) {
                unsigned char *hdr_end = (unsigned char *)memmem(recv_buf, buf_len, "\r\n\r\n", 4);
                if (!hdr_end) {
                    if (buf_len >= MAX_FRAME_BYTES) buf_len = 0; /* malformed, reset */
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
                pthread_mutex_lock(&proxy->frame_mutex);
                memcpy(proxy->frame_buf, jpeg_start, (size_t)content_len);
                proxy->frame_len = (size_t)content_len;
                proxy->last_frame_time = time(NULL);
                pthread_mutex_unlock(&proxy->frame_mutex);

                size_t consumed = (jpeg_start - recv_buf) + content_len;
                memmove(recv_buf, recv_buf + consumed, buf_len - consumed);
                buf_len -= consumed;
            }

            if (buf_len >= MAX_FRAME_BYTES - 1) {
                /* Shouldn't normally happen; guard against runaway growth. */
                buf_len = 0;
            }
        }

        close(sock);
        sleep(1);
    }
    free(recv_buf); /* unreachable (proxy_loop runs forever), kept for completeness/analyzers */
    return NULL;
}

stream_proxy_t *stream_proxy_start(const char *source_url, const char *label)
{
    stream_proxy_t *proxy = calloc(1, sizeof(stream_proxy_t));
    if (!proxy) {
        fprintf(stderr, "[stream_proxy] failed to allocate proxy instance for '%s'\n", label);
        return NULL;
    }

    if (parse_url(source_url, proxy->host, sizeof(proxy->host), &proxy->port,
                  proxy->path, sizeof(proxy->path)) != 0) {
        fprintf(stderr, "[stream_proxy] failed to parse URL: %s\n", source_url);
        free(proxy);
        return NULL;
    }

    strncpy(proxy->label, label, sizeof(proxy->label) - 1);

    proxy->frame_buf = malloc(MAX_FRAME_BYTES);
    if (!proxy->frame_buf) {
        fprintf(stderr, "[stream_proxy:%s] failed to allocate frame buffer\n", proxy->label);
        free(proxy);
        return NULL;
    }

    pthread_mutex_init(&proxy->frame_mutex, NULL);

    pthread_t tid;
    int rc = pthread_create(&tid, NULL, proxy_loop, proxy);
    if (rc != 0) {
        fprintf(stderr, "[stream_proxy:%s] failed to start puller thread\n", proxy->label);
        free(proxy->frame_buf);
        free(proxy);
        return NULL;
    }
    pthread_detach(tid);
    return proxy;
}

int stream_proxy_get_latest_frame(stream_proxy_t *proxy, unsigned char *buf, size_t buf_size)
{
    if (!proxy) return 0; /* no proxy configured for this stream (e.g. detector not set up) */

    pthread_mutex_lock(&proxy->frame_mutex);
    size_t len = proxy->frame_len;
    int result;
    if (len == 0) {
        result = 0;
    } else if (len > buf_size) {
        result = -1;
    } else {
        memcpy(buf, proxy->frame_buf, len);
        result = (int)len;
    }
    pthread_mutex_unlock(&proxy->frame_mutex);
    return result;
}

long stream_proxy_last_frame_time(stream_proxy_t *proxy)
{
    if (!proxy) return 0;

    pthread_mutex_lock(&proxy->frame_mutex);
    long t = proxy->last_frame_time;
    pthread_mutex_unlock(&proxy->frame_mutex);
    return t;
}
