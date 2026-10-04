/*
 * json.h - 极小、严格的 JSON 解析器（C11）
 * 安全策略：映射包来自网络，解析必须严格——
 *   - 拒绝尾随垃圾、未转义控制字符、重复键；
 *   - 数字只接受策略需要的整数（代次/步长），拒绝 NaN/Inf；
 *   - 规范化序列化结果与服务端 Python json.dumps(
 *     sort_keys=True, separators=(",",":"), ensure_ascii=False) 字节一致，
 *     以便重算 SHA-256 摘要。
 */
#ifndef INPUT_STRATEGY_JSON_H
#define INPUT_STRATEGY_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ
} JsonType;

typedef struct JsonVal JsonVal;
typedef struct JsonMember { char *key; JsonVal *val; } JsonMember;

struct JsonVal {
    JsonType type;
    union {
        bool boolean;
        int64_t num;
        char *str;
        struct { JsonVal **items; uint32_t len; } arr;
        struct { JsonMember *m; uint32_t len; } obj;
    } u;
};

JsonVal *json_parse(const char *text, char *err, size_t errsz);
void json_free(JsonVal *v);

const JsonVal *json_obj_get(const JsonVal *v, const char *key);
const char *json_str(const JsonVal *v);
bool json_int(const JsonVal *v, int64_t *out);

/* 规范化序列化：键排序、无空白、UTF-8 直出。写入动态缓冲。 */
typedef struct { char *data; size_t len, cap; } JsonBuf;
bool json_canonical(const JsonVal *v, JsonBuf *out);
void json_buf_free(JsonBuf *b);

/* 工具：直接规范化一段 JSON 文本（先解析再序列化），失败返回 false */
bool json_canonical_text(const char *text, JsonBuf *out, char *err, size_t errsz);

#endif
