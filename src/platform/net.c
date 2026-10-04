#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

void http_response_free(HttpResponse *r) {
    free(r->data);
    r->data = NULL;
    r->len = 0;
}

typedef struct {
    char host[256];
    char port[8];
    char path[1024];
} Url;

static bool parse_url(const char *url, Url *u) {
    memset(u, 0, sizeof *u);
    if (strncmp(url, "http://", 7) != 0) return false;
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    size_t hl = (slash ? (size_t)(slash - p) : strlen(p));
    if (colon && (!slash || colon < slash)) {
        size_t n = (size_t)(colon - p);
        if (n >= sizeof u->host) return false;
        memcpy(u->host, p, n);
        snprintf(u->port, sizeof u->port, "%.*s",
                 (int)(hl - n - 1), colon + 1);
    } else {
        if (hl >= sizeof u->host) return false;
        memcpy(u->host, p, hl);
        snprintf(u->port, sizeof u->port, "80");
    }
    snprintf(u->path, sizeof u->path, "%s", slash ? slash : "/");
    return true;
}

static bool do_request(const char *method, const char *url,
                       const char *body, HttpResponse *out) {
    Url u;
    if (!parse_url(url, &u)) return false;

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u.host, u.port, &hints, &res) != 0) return false;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return false; }

    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    bool ok = false;
    if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
        char req[2048];
        size_t blen = body ? strlen(body) : 0;
        int n = snprintf(req, sizeof req,
            "%s %s HTTP/1.1\r\nHost: %s\r\n"
            "Content-Type: application/json\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            method, u.path, u.host, blen);
        if ((size_t)n < sizeof req &&
            (size_t)write(fd, req, (size_t)n) == (size_t)n &&
            (!body || (size_t)write(fd, body, blen) == blen)) {
            size_t cap = 8192, len = 0;
            char *buf = malloc(cap);
            ssize_t r;
            while ((r = read(fd, buf + len, cap - len - 1)) > 0) {
                len += (size_t)r;
                if (len + 1 == cap) {
                    cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) break;
                    buf = nb;
                }
            }
            if (buf) {
                buf[len] = 0;
                char *sep = strstr(buf, "\r\n\r\n");
                if (sep) {
                    long status = 0;
                    sscanf(buf, "HTTP/1.%*d %ld", &status);
                    char *bd = sep + 4;
                    size_t bl = len - (size_t)(bd - buf);
                    out->data = malloc(bl + 1);
                    memcpy(out->data, bd, bl);
                    out->data[bl] = 0;
                    out->len = bl;
                    out->status = status;
                    ok = true;
                }
                free(buf);
            }
        }
    }
    close(fd);
    freeaddrinfo(res);
    return ok;
}

bool http_get(const char *url, HttpResponse *out) {
    return do_request("GET", url, NULL, out);
}

bool http_post_json(const char *url, const char *json_body, HttpResponse *out) {
    return do_request("POST", url, json_body, out);
}
