#ifndef POLICY_CLIENT_H
#define POLICY_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#define HTTP_BODY_CAP 16384

typedef struct {
    int status;
    size_t length;
    char body[HTTP_BODY_CAP];
} HttpResponse;

int http_get(const char *url, int timeout_ms, HttpResponse *response);
int http_post_json(const char *url, int timeout_ms, const char *json, HttpResponse *response);

#endif
