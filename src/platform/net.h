/*
 * net.h - 极简 HTTP 客户端（POSIX socket，无第三方依赖）
 * 只用于：轮询拉取当前策略信封、上报回执、上报真人确认。
 * 远程不接受任何"注入输入"——通道是单向控制面，不存在远程按键通路。
 */
#ifndef INPUT_STRATEGY_NET_H
#define INPUT_STRATEGY_NET_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    long status;
} HttpResponse;

void http_response_free(HttpResponse *r);

/* 阻塞 GET/POST（带秒级超时，适合每若干秒轮询一次）。
   body 为 JSON 文本；返回 false 表示网络层失败（非 2xx 也算 true，
   调用方检查 status）。 */
bool http_get(const char *url, HttpResponse *out);
bool http_post_json(const char *url, const char *json_body, HttpResponse *out);

#endif
