#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *base;
    const char *p;
    const char *end;
    char *err;
    size_t errsz;
    bool failed;
} P;

static void fail(P *p, const char *msg) {
    if (!p->failed) {
        snprintf(p->err, p->errsz, "json: %s at byte %ld",
                 msg, (long)(p->p - p->base));
        p->failed = true;
    }
}

static void ws(P *p) {
    while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' ||
                             *p->p == '\n' || *p->p == '\r'))
        p->p++;
}

static JsonVal *parse_val(P *p);

static void *xcalloc(size_t n, size_t sz, P *p) {
    void *m = calloc(n, sz);
    if (!m) fail(p, "out of memory");
    return m;
}

static char *parse_str_raw(P *p) {
    if (p->p >= p->end || *p->p != '"') { fail(p, "expected string"); return NULL; }
    p->p++;
    size_t cap = 32, len = 0;
    char *out = xcalloc(cap, 1, p);
    if (!out) return NULL;

    #define GROW(need) do { \
        if (len + (need) + 1 > cap) { \
            while (len + (need) + 1 > cap) cap *= 2; \
            char *_n = realloc(out, cap); \
            if (!_n) { fail(p, "oom"); free(out); return NULL; } \
            out = _n; \
        } } while (0)

    while (p->p < p->end && *p->p != '"') {
        unsigned char c = (unsigned char)*p->p;
        if (c == '\\') {
            p->p++;
            if (p->p >= p->end) break;
            char e = *p->p++;
            char m;
            switch (e) {
            case '"': m = '"'; break;
            case '\\': m = '\\'; break;
            case '/': m = '/'; break;
            case 'b': m = '\b'; break;
            case 'f': m = '\f'; break;
            case 'n': m = '\n'; break;
            case 'r': m = '\r'; break;
            case 't': m = '\t'; break;
            case 'u': {
                /* \uXXXX，支持代理对。解析为 UTF-8。 */
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    if (p->p >= p->end) { fail(p, "bad unicode escape"); goto err; }
                    char h = *p->p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else { fail(p, "bad hex digit"); goto err; }
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p->end - p->p < 6 || p->p[0] != '\\' || p->p[1] != 'u') {
                        fail(p, "expected low surrogate"); goto err;
                    }
                    p->p += 2;
                    unsigned lo = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = *p->p++;
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                        else { fail(p, "bad hex digit"); goto err; }
                    }
                    if (lo < 0xDC00 || lo > 0xDFFF) { fail(p, "bad low surrogate"); goto err; }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    fail(p, "unexpected low surrogate"); goto err;
                }
                int n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
                GROW((size_t)n);
                if (n == 1) out[len++] = (char)cp;
                else if (n == 2) { out[len++]= (char)(0xC0|(cp>>6)); out[len++]=(char)(0x80|(cp&63)); }
                else if (n == 3) { out[len++]=(char)(0xE0|(cp>>12)); out[len++]=(char)(0x80|((cp>>6)&63)); out[len++]=(char)(0x80|(cp&63)); }
                else { out[len++]=(char)(0xF0|(cp>>18)); out[len++]=(char)(0x80|((cp>>12)&63)); out[len++]=(char)(0x80|((cp>>6)&63)); out[len++]=(char)(0x80|(cp&63)); }
                continue;
            }
            default: fail(p, "bad escape"); goto err;
            }
            GROW(1);
            out[len++] = m;
        } else if (c < 0x20) {
            fail(p, "unescaped control character"); goto err;
        } else {
            /* 直接复制 UTF-8 字节 */
            int n = c < 0x80 ? 1 : c < 0xC0 ? 1 : c < 0xE0 ? 2 :
                    c < 0xF0 ? 3 : 4;
            if ((size_t)(p->end - p->p) < (size_t)n) { fail(p, "truncated utf8"); goto err; }
            GROW((size_t)n);
            memcpy(out + len, p->p, (size_t)n);
            len += (size_t)n;
            p->p += n;
        }
    }
    if (p->p >= p->end) { fail(p, "unterminated string"); goto err; }
    p->p++; /* closing quote */
    out[len] = '\0';
    return out;
err:
    free(out);
    return NULL;
    #undef GROW
}

static JsonVal *parse_num(P *p) {
    const char *s = p->p;
    if (*p->p == '-') p->p++;
    if (p->p >= p->end || *p->p < '0' || *p->p > '9') { fail(p, "bad number"); return NULL; }
    if (*p->p == '0') p->p++;
    else while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    /* 本系统只接受整数；出现小数点/指数即拒绝（避免 NaN/1e400 等歧义） */
    if (p->p < p->end && (*p->p == '.' || *p->p == 'e' || *p->p == 'E')) {
        fail(p, "only integers allowed"); return NULL;
    }
    size_t n = (size_t)(p->p - s);
    char buf[32];
    if (n >= sizeof buf) { fail(p, "number too long"); return NULL; }
    memcpy(buf, s, n); buf[n] = 0;
    JsonVal *v = xcalloc(1, sizeof *v, p);
    if (v) { v->type = J_NUM; v->u.num = strtoll(buf, NULL, 10); }
    return v;
}

static JsonVal *parse_arr(P *p) {
    p->p++; /* [ */
    JsonVal *v = xcalloc(1, sizeof *v, p);
    if (!v) return NULL;
    v->type = J_ARR;
    ws(p);
    if (p->p < p->end && *p->p == ']') { p->p++; return v; }
    uint32_t cap = 0;
    for (;;) {
        JsonVal *item = parse_val(p);
        if (!item) goto fail;
        if (v->u.arr.len == cap) {
            cap = cap ? cap * 2 : 4;
            JsonVal **nn = realloc(v->u.arr.items, cap * sizeof *nn);
            if (!nn) { json_free(item); fail(p, "oom"); goto fail; }
            v->u.arr.items = nn;
        }
        v->u.arr.items[v->u.arr.len++] = item;
        ws(p);
        if (p->p >= p->end) { fail(p, "unterminated array"); goto fail; }
        if (*p->p == ',') { p->p++; ws(p); continue; }
        if (*p->p == ']') { p->p++; break; }
        fail(p, "expected , or ]"); goto fail;
    }
    return v;
fail:
    json_free(v);
    return NULL;
}

static JsonVal *parse_obj(P *p) {
    p->p++; /* { */
    JsonVal *v = xcalloc(1, sizeof *v, p);
    if (!v) return NULL;
    v->type = J_OBJ;
    ws(p);
    if (p->p < p->end && *p->p == '}') { p->p++; return v; }
    uint32_t cap = 0;
    for (;;) {
        ws(p);
        char *k = parse_str_raw(p);
        if (!k) goto fail;
        ws(p);
        if (p->p >= p->end || *p->p != ':') { free(k); fail(p, "expected :"); goto fail; }
        p->p++;
        ws(p);
        JsonVal *mv = parse_val(p);
        if (!mv) { free(k); goto fail; }
        /* 重复键直接拒绝：防止映射包用后键覆盖前键隐藏冲突 */
        for (uint32_t i = 0; i < v->u.obj.len; ++i) {
            if (strcmp(v->u.obj.m[i].key, k) == 0) {
                free(k); json_free(mv); fail(p, "duplicate key"); goto fail;
            }
        }
        if (v->u.obj.len == cap) {
            cap = cap ? cap * 2 : 4;
            JsonMember *nm = realloc(v->u.obj.m, cap * sizeof *nm);
            if (!nm) { free(k); json_free(mv); fail(p, "oom"); goto fail; }
            v->u.obj.m = nm;
        }
        v->u.obj.m[v->u.obj.len].key = k;
        v->u.obj.m[v->u.obj.len].val = mv;
        v->u.obj.len++;
        ws(p);
        if (p->p >= p->end) { fail(p, "unterminated object"); goto fail; }
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == '}') { p->p++; break; }
        fail(p, "expected , or }"); goto fail;
    }
    return v;
fail:
    json_free(v);
    return NULL;
}

static JsonVal *parse_val(P *p) {
    ws(p);
    if (p->p >= p->end) { fail(p, "unexpected end"); return NULL; }
    char c = *p->p;
    if (c == '"') {
        char *s = parse_str_raw(p);
        if (!s) return NULL;
        JsonVal *v = xcalloc(1, sizeof *v, p);
        if (v) { v->type = J_STR; v->u.str = s; } else free(s);
        return v;
    }
    if (c == '{') return parse_obj(p);
    if (c == '[') return parse_arr(p);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_num(p);
    if (p->end - p->p >= 4 && strncmp(p->p, "true", 4) == 0) {
        p->p += 4;
        JsonVal *v = xcalloc(1, sizeof *v, p);
        if (v) { v->type = J_BOOL; v->u.boolean = true; }
        return v;
    }
    if (p->end - p->p >= 5 && strncmp(p->p, "false", 5) == 0) {
        p->p += 5;
        JsonVal *v = xcalloc(1, sizeof *v, p);
        if (v) { v->type = J_BOOL; v->u.boolean = false; }
        return v;
    }
    if (p->end - p->p >= 4 && strncmp(p->p, "null", 4) == 0) {
        p->p += 4;
        return xcalloc(1, sizeof(JsonVal), p); /* J_NULL == 0 */
    }
    fail(p, "unexpected token");
    return NULL;
}

JsonVal *json_parse(const char *text, char *err, size_t errsz) {
    if (err && errsz) err[0] = '\0';
    P st = { text, text, text + strlen(text), err, errsz, false };
    P *p = &st;
    JsonVal *v = parse_val(p);
    if (!v) return NULL;
    ws(p);
    if (p->p != p->end) { fail(p, "trailing garbage"); json_free(v); return NULL; }
    return v;
}

void json_free(JsonVal *v) {
    if (!v) return;
    if (v->type == J_STR) free(v->u.str);
    else if (v->type == J_ARR) {
        for (uint32_t i = 0; i < v->u.arr.len; ++i) json_free(v->u.arr.items[i]);
        free(v->u.arr.items);
    } else if (v->type == J_OBJ) {
        for (uint32_t i = 0; i < v->u.obj.len; ++i) {
            free(v->u.obj.m[i].key);
            json_free(v->u.obj.m[i].val);
        }
        free(v->u.obj.m);
    }
    free(v);
}

const JsonVal *json_obj_get(const JsonVal *v, const char *key) {
    if (!v || v->type != J_OBJ) return NULL;
    for (uint32_t i = 0; i < v->u.obj.len; ++i)
        if (strcmp(v->u.obj.m[i].key, key) == 0) return v->u.obj.m[i].val;
    return NULL;
}

const char *json_str(const JsonVal *v) {
    return (v && v->type == J_STR) ? v->u.str : NULL;
}

bool json_int(const JsonVal *v, int64_t *out) {
    if (!v || v->type != J_NUM) return false;
    *out = v->u.num;
    return true;
}

static bool jb_put(JsonBuf *b, char c) {
    if (b->len + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        char *n = realloc(b->data, nc);
        if (!n) return false;
        b->data = n; b->cap = nc;
    }
    b->data[b->len++] = c;
    return true;
}

static bool jb_write(JsonBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (b->len + n + 1 > nc) nc *= 2;
        char *nn = realloc(b->data, nc);
        if (!nn) return false;
        b->data = nn; b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return true;
}

static bool emit_str(JsonBuf *b, const char *s) {
    if (!jb_put(b, '"')) return false;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        char esc[8];
        int el = 0;
        if (c == '"') { esc[0]='\\'; esc[1]='"'; el=2; }
        else if (c == '\\') { esc[0]='\\'; esc[1]='\\'; el=2; }
        else if (c == '\b') { esc[0]='\\'; esc[1]='b'; el=2; }
        else if (c == '\f') { esc[0]='\\'; esc[1]='f'; el=2; }
        else if (c == '\n') { esc[0]='\\'; esc[1]='n'; el=2; }
        else if (c == '\r') { esc[0]='\\'; esc[1]='r'; el=2; }
        else if (c == '\t') { esc[0]='\\'; esc[1]='t'; el=2; }
        else if (c < 0x20) { el = snprintf(esc, sizeof esc, "\\u%04x", c); }
        if (el) {
            if (!jb_write(b, esc, (size_t)el)) return false;
            p++;
        } else {
            int n = c < 0x80 ? 1 : c < 0xC0 ? 1 : c < 0xE0 ? 2 :
                    c < 0xF0 ? 3 : 4;
            if (!jb_write(b, (const char *)p, (size_t)n)) return false;
            p += n;
        }
    }
    return jb_put(b, '"');
}

static int member_cmp(const void *a, const void *b) {
    /* 按 Unicode 码位排序 == 按 UTF-8 字节序排序 */
    const JsonMember *ma = a, *mb = b;
    return strcmp(ma->key, mb->key);
}

static bool emit_val(JsonBuf *b, const JsonVal *v) {
    char numbuf[32];
    switch (v->type) {
    case J_NULL: return jb_write(b, "null", 4);
    case J_BOOL:
        if (v->u.boolean) return jb_write(b, "true", 4);
        return jb_write(b, "false", 5);
    case J_NUM:
        snprintf(numbuf, sizeof numbuf, "%lld", (long long)v->u.num);
        return jb_write(b, numbuf, strlen(numbuf));
    case J_STR:
        return emit_str(b, v->u.str ? v->u.str : "");
    case J_ARR:
        if (!jb_put(b, '[')) return false;
        for (uint32_t i = 0; i < v->u.arr.len; ++i) {
            if (i && !jb_put(b, ',')) return false;
            if (!emit_val(b, v->u.arr.items[i])) return false;
        }
        return jb_put(b, ']');
    case J_OBJ: {
        /* 复制成员数组后排序，不修改原树 */
        uint32_t n = v->u.obj.len;
        JsonMember *tmp = NULL;
        if (n) {
            tmp = malloc(n * sizeof *tmp);
            if (!tmp) return false;
            memcpy(tmp, v->u.obj.m, n * sizeof *tmp);
            qsort(tmp, n, sizeof *tmp, member_cmp);
        }
        bool ok = jb_put(b, '{');
        for (uint32_t i = 0; ok && i < n; ++i) {
            if (i && !jb_put(b, ',')) { ok = false; break; }
            ok = emit_str(b, tmp[i].key) && jb_put(b, ':') &&
                 emit_val(b, tmp[i].val);
        }
        free(tmp);
        return ok && jb_put(b, '}');
    }
    }
    return false;
}

bool json_canonical(const JsonVal *v, JsonBuf *out) {
    if (!emit_val(out, v)) return false;
    return jb_put(out, '\0') && (out->len--, true);
}

void json_buf_free(JsonBuf *b) {
    free(b->data);
    b->data = NULL; b->len = b->cap = 0;
}

bool json_canonical_text(const char *text, JsonBuf *out, char *err, size_t errsz) {
    JsonVal *v = json_parse(text, err, errsz);
    if (!v) return false;
    bool ok = json_canonical(v, out);
    json_free(v);
    return ok;
}
