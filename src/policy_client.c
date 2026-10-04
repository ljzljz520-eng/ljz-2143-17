#define _POSIX_C_SOURCE 200809L

#include "policy_client.h"

#include <stdbool.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    char host[256];
    char port[16];
    char path[2048];
} ParsedUrl;

static bool parse_http_url(const char *url, ParsedUrl *parsed) {
    if (url == NULL || parsed == NULL || strncmp(url, "http://", 7) != 0) {
        return false;
    }
    memset(parsed, 0, sizeof(*parsed));
    const char *rest = url + 7;
    const char *slash = strchr(rest, '/');
    const char *host_end = slash == NULL ? rest + strlen(rest) : slash;
    const char *colon = memchr(rest, ':', (size_t)(host_end - rest));

    size_t host_len = colon == NULL
        ? (size_t)(host_end - rest)
        : (size_t)(colon - rest);
    if (host_len == 0 || host_len >= sizeof(parsed->host)) return false;
    memcpy(parsed->host, rest, host_len);

    if (colon != NULL) {
        size_t port_len = (size_t)(host_end - colon - 1);
        if (port_len == 0 || port_len >= sizeof(parsed->port)) return false;
        memcpy(parsed->port, colon + 1, port_len);
    } else {
        snprintf(parsed->port, sizeof(parsed->port), "80");
    }

    snprintf(parsed->path, sizeof(parsed->path), "%s", slash == NULL ? "/" : slash);
    return true;
}

static int connect_timeout(const ParsedUrl *url, int timeout_ms) {
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(url->host, url->port, &hints, &result) != 0) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = result; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000
        };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static int parse_status(const char *headers) {
    if (strncmp(headers, "HTTP/1.", 7) != 0) return -1;
    return atoi(headers + 9);
}

static int read_response(int fd, HttpResponse *response) {
    char buffer[2048];
    char raw[24576];
    size_t total = 0;

    memset(response, 0, sizeof(*response));
    do {
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (n < 0) return -1;
        if (n == 0) break;
        if (total + (size_t)n >= sizeof(raw)) return -1;
        memcpy(raw + total, buffer, (size_t)n);
        total += (size_t)n;
        raw[total] = '\0';
    } while (strstr(raw, "\r\n\r\n") == NULL);

    char *body = strstr(raw, "\r\n\r\n");
    if (body == NULL) return -1;
    body += 4;

    char *headers_end = body;
    size_t body_len = total - (size_t)(body - raw);
    char *content_length = strstr(raw, "Content-Length:");
    if (content_length == NULL) content_length = strstr(raw, "content-length:");
    size_t expected = body_len;
    if (content_length != NULL) {
        expected = (size_t)strtoul(content_length + strlen("Content-Length:"), NULL, 10);
    }

    char *status_end = strstr(raw, "\r\n");
    if (status_end == NULL) return -1;
    *status_end = '\0';
    response->status = parse_status(raw);
    (void)headers_end;
    while (body_len < expected) {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        if (body_len + (size_t)n >= sizeof(raw)) return -1;
        memcpy(body + body_len, buffer, (size_t)n);
        body_len += (size_t)n;
        body[body_len] = '\0';
    }

    if (body_len >= sizeof(response->body)) return -1;
    memcpy(response->body, body, body_len);
    response->length = body_len;
    response->body[body_len] = '\0';
    return response->status;
}

static int request(const char *method, const char *url, int timeout_ms,
                   const char *json, HttpResponse *response) {
    ParsedUrl parsed;
    if (!parse_http_url(url, &parsed)) return -1;
    int fd = connect_timeout(&parsed, timeout_ms);
    if (fd < 0) return -1;

    char request_text[20480];
    int length = json == NULL ? 0 : (int)strlen(json);
    int sent = snprintf(
        request_text, sizeof(request_text),
        "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept: application/json\r\n",
        method, parsed.path, parsed.host
    );
    if (json != NULL) {
        sent += snprintf(
            request_text + sent, sizeof(request_text) - (size_t)sent,
            "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
            length, json
        );
    } else {
        sent += snprintf(request_text + sent, sizeof(request_text) - (size_t)sent, "\r\n");
    }
    if (sent <= 0 || (size_t)sent >= sizeof(request_text)) {
        close(fd);
        return -1;
    }

    ssize_t written = send(fd, request_text, (size_t)sent, 0);
    int result = written == sent ? read_response(fd, response) : -1;
    close(fd);
    return result;
}

int http_get(const char *url, int timeout_ms, HttpResponse *response) {
    return request("GET", url, timeout_ms, NULL, response);
}

int http_post_json(const char *url, int timeout_ms, const char *json, HttpResponse *response) {
    return request("POST", url, timeout_ms, json, response);
}
