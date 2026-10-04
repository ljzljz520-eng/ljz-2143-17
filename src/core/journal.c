#include "journal.h"

#include <stdlib.h>
#include <string.h>

#include "crc32.h"

/* ---------- R_INPUT 线格式 body：84 字节（紧凑、显式偏移，
   不依赖 C 结构体内存对齐） ---------- */
#define IB_LEN (4u + 1u + 2u + 4u + 1u + 4u + 4u + (unsigned)EVENT_TEXT_MAX)
/* 编译期断言：线格式 body 必须为 84 字节 */
typedef char ib_len_must_be_84[(IB_LEN == 84) ? 1 : -1];
enum {
    IB_REL = 0, IB_ORIGIN = 4, IB_KEY = 5, IB_MODS = 7, IB_FLAGS = 11,
    IB_PLATFORM = 12, IB_RAW = 16, IB_TEXT = 20
};

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static bool write_frame(FILE *fp, uint8_t type, const void *body, uint32_t blen) {
    uint32_t len = 1u + blen;
    uint8_t hdr[4];
    put_u32(hdr, len);
    if (fwrite(hdr, 1, 4, fp) != 4) return false;

    /* CRC 必须对连续内存计算，因此组装 TYPE+BODY */
    uint8_t *joined = malloc(len);
    if (!joined) return false;
    joined[0] = type;
    if (blen) memcpy(joined + 1, body, blen);
    uint32_t c = crc32_ieee(joined, len);
    bool ok = fwrite(joined, 1, len, fp) == len;
    uint8_t cb[4];
    put_u32(cb, c);
    ok = ok && fwrite(cb, 1, 4, fp) == 4;
    free(joined);
    return ok;
}

static void encode_input(uint8_t body[IB_LEN], uint32_t rel_ms,
                         const InputEvent *e) {
    memset(body, 0, IB_LEN);
    put_u32(body + IB_REL, rel_ms);
    body[IB_ORIGIN] = (uint8_t)e->origin;
    put_u16(body + IB_KEY, (uint16_t)e->key);
    put_u32(body + IB_MODS, e->mods);
    uint8_t flags = (uint8_t)(e->down ? 1 : 0);
    if (e->repeat) flags |= 2;
    if (e->composition) flags |= 4;
    body[IB_FLAGS] = flags;
    put_u32(body + IB_PLATFORM, e->platform);
    put_u32(body + IB_RAW, e->raw_code);
    memcpy(body + IB_TEXT, e->text, EVENT_TEXT_MAX);
}

bool journal_decode_input(const uint8_t *body, uint32_t len, InputEvent *out) {
    if (len < IB_LEN) return false;
    memset(out, 0, sizeof *out);
    out->type = IE_KEY;
    out->origin = (EventOrigin)body[IB_ORIGIN];
    out->key = (NK_Key)get_u16(body + IB_KEY);
    out->mods = get_u32(body + IB_MODS);
    uint8_t flags = body[IB_FLAGS];
    out->down = (flags & 1) != 0;
    out->repeat = (flags & 2) != 0;
    out->composition = (flags & 4) != 0;
    out->platform = get_u32(body + IB_PLATFORM);
    out->raw_code = get_u32(body + IB_RAW);
    memcpy(out->text, body + IB_TEXT, EVENT_TEXT_MAX);
    return true;
}

void encode_header(uint8_t out[JOURNAL_HEADER_SIZE],
                          const JournalHeader *h) {
    memset(out, 0, JOURNAL_HEADER_SIZE);
    memcpy(out, JOURNAL_MAGIC, 5);
    out[5] = JOURNAL_VERSION;
    memcpy(out + 6, h->device_id, JOURNAL_DEVICE_LEN);
    put_u64(out + 70, h->generation);
    memcpy(out + 78, h->started_at, JOURNAL_TIME_LEN);
    /* 86..115 为保留零字节 */
}

bool decode_header(const uint8_t in[JOURNAL_HEADER_SIZE],
                          JournalHeader *h) {
    memset(h, 0, sizeof *h);
    if (memcmp(in, JOURNAL_MAGIC, 5) != 0 || in[5] != JOURNAL_VERSION)
        return false;
    memcpy(h->device_id, in + 6, JOURNAL_DEVICE_LEN);
    h->version = in[5];
    h->generation = get_u64(in + 70);
    memcpy(h->started_at, in + 78, JOURNAL_TIME_LEN);
    return true;
}

static void fill_header(JournalHeader *h, const char *device_id,
                        uint64_t generation, const char *started_at) {
    memset(h, 0, sizeof *h);
    memcpy(h->magic, JOURNAL_MAGIC, 5);
    h->version = JOURNAL_VERSION;
    if (device_id) snprintf(h->device_id, sizeof h->device_id, "%s", device_id);
    h->generation = generation;
    if (started_at) snprintf(h->started_at, sizeof h->started_at, "%s", started_at);
}

bool journal_open_write(Journal *j, const char *path, const char *device_id,
                        uint64_t generation, const char *started_at_iso) {
    memset(j, 0, sizeof *j);
    j->fp = fopen(path, "wb");
    if (!j->fp) return false;
    j->writable = true;
    fill_header(&j->header, device_id, generation, started_at_iso);
    uint8_t hb[JOURNAL_HEADER_SIZE];
    encode_header(hb, &j->header);
    if (fwrite(hb, 1, sizeof hb, j->fp) != sizeof hb) {
        fclose(j->fp); memset(j, 0, sizeof *j); return false;
    }
    return true;
}

bool journal_open_read(Journal *j, const char *path) {
    memset(j, 0, sizeof *j);
    j->fp = fopen(path, "rb");
    if (!j->fp) return false;
    uint8_t hb[JOURNAL_HEADER_SIZE];
    if (fread(hb, 1, sizeof hb, j->fp) != sizeof hb ||
        !decode_header(hb, &j->header)) {
        fclose(j->fp); memset(j, 0, sizeof *j); return false;
    }
    return true;
}

void journal_close(Journal *j) {
    if (j->fp) fclose(j->fp);
    memset(j, 0, sizeof *j);
}

bool journal_write_input(Journal *j, uint32_t rel_ms, const InputEvent *e) {
    if (!j->writable) return false;
    uint8_t body[IB_LEN];
    encode_input(body, rel_ms, e);
    return write_frame(j->fp, R_INPUT, body, sizeof body);
}

bool journal_write_command(Journal *j, uint32_t rel_ms, const char *cmd_name,
                           int32_t arg, EventOrigin origin) {
    if (!j->writable) return false;
    uint8_t body[4 + 1 + 4 + 48];
    memset(body, 0, sizeof body);
    put_u32(body, rel_ms);
    body[4] = (uint8_t)origin;
    put_u32(body + 5, (uint32_t)arg);
    snprintf((char *)body + 9, 48, "%s", cmd_name ? cmd_name : "");
    return write_frame(j->fp, R_COMMAND, body, (uint32_t)sizeof body);
}

bool journal_write_system(Journal *j, uint32_t rel_ms, uint8_t sys_code,
                          const char *detail) {
    if (!j->writable) return false;
    uint8_t body[4 + 1 + 80];
    memset(body, 0, sizeof body);
    put_u32(body, rel_ms);
    body[4] = sys_code;
    snprintf((char *)body + 5, 80, "%s", detail ? detail : "");
    return write_frame(j->fp, R_SYSTEM, body, (uint32_t)sizeof body);
}

bool journal_write_activation(Journal *j, uint32_t rel_ms, uint64_t old_gen,
                              uint64_t new_gen, uint32_t released_keys) {
    if (!j->writable) return false;
    uint8_t body[24];
    put_u32(body, rel_ms);
    put_u64(body + 4, old_gen);
    put_u64(body + 12, new_gen);
    put_u32(body + 20, released_keys);
    return write_frame(j->fp, R_ACTIVATION, body, sizeof body);
}

bool journal_write_blocked(Journal *j, uint32_t rel_ms, const char *cmd_name,
                           EventOrigin origin, const char *reason) {
    if (!j->writable) return false;
    uint8_t body[4 + 1 + 48 + 80];
    memset(body, 0, sizeof body);
    put_u32(body, rel_ms);
    body[4] = (uint8_t)origin;
    snprintf((char *)body + 5, 48, "%s", cmd_name ? cmd_name : "");
    snprintf((char *)body + 53, 80, "%s", reason ? reason : "");
    return write_frame(j->fp, R_REPLAY_BLOCKED, body, (uint32_t)sizeof body);
}

bool journal_read_next(Journal *j, JournalRecord *rec, bool *eof, bool *corrupt) {
    *eof = false;
    if (corrupt) *corrupt = false;
    long frame_start = ftell(j->fp);
    uint8_t lb[4];
    if (fread(lb, 1, 4, j->fp) != 4) { *eof = true; return false; }
    uint32_t len = get_u32(lb);
    if (len < 1 || len > JOURNAL_MAX_FRAME - 4) {
        if (corrupt) *corrupt = true;
        return false;
    }
    if (fread(j->framebuf, 1, len + 4, j->fp) != len + 4) {
        if (corrupt) *corrupt = true;
        return false; /* 截断帧 */
    }
    uint32_t stored_crc = get_u32(j->framebuf + len);
    uint32_t calc_crc = crc32_ieee(j->framebuf, len);
    if (stored_crc != calc_crc) {
        if (corrupt) *corrupt = true;
        return false;
    }
    rec->type = j->framebuf[0];
    rec->body = j->framebuf + 1;
    rec->body_len = len - 1;
    rec->rel_ms = rec->body_len >= 4 ? get_u32(rec->body) : 0;
    j->frame_index++;
    (void)frame_start;
    return true;
}

/* ---------- 内存构建（测试用） ---------- */

static bool jm_put(JournalMem *m, const void *d, size_t n) {
    if (m->len + n > m->cap) {
        size_t c = m->cap ? m->cap : 128;
        while (m->len + n > c) c *= 2;
        uint8_t *nb = realloc(m->data, c);
        if (!nb) return false;
        m->data = nb; m->cap = c;
    }
    memcpy(m->data + m->len, d, n);
    m->len += n;
    return true;
}

static bool jm_frame(JournalMem *m, uint8_t type, const void *body, uint32_t blen) {
    uint32_t len = 1u + blen;
    uint8_t lb[4]; put_u32(lb, len);
    if (!jm_put(m, lb, 4)) return false;
    uint8_t *joined = malloc(len);
    if (!joined) return false;
    joined[0] = type;
    if (blen) memcpy(joined + 1, body, blen);
    uint32_t crc = crc32_ieee(joined, len);
    bool ok = jm_put(m, joined, len);
    uint8_t cb[4]; put_u32(cb, crc);
    ok = ok && jm_put(m, cb, 4);
    free(joined);
    return ok;
}

void jm_init(JournalMem *m) { memset(m, 0, sizeof *m); }
void jm_free(JournalMem *m) { free(m->data); memset(m, 0, sizeof *m); }

bool jm_write_header(JournalMem *m, const char *device_id,
                     uint64_t generation, const char *started_at_iso) {
    JournalHeader h;
    fill_header(&h, device_id, generation, started_at_iso);
    uint8_t hb[JOURNAL_HEADER_SIZE];
    encode_header(hb, &h);
    return jm_put(m, hb, sizeof hb);
}

bool jm_append_input(JournalMem *m, uint32_t rel_ms, const InputEvent *e) {
    uint8_t body[IB_LEN];
    encode_input(body, rel_ms, e);
    return jm_frame(m, R_INPUT, body, sizeof body);
}

bool jm_append_system(JournalMem *m, uint32_t rel_ms, uint8_t sys_code,
                      const char *detail) {
    uint8_t body[4 + 1 + 80];
    memset(body, 0, sizeof body);
    put_u32(body, rel_ms);
    body[4] = sys_code;
    snprintf((char *)body + 5, 80, "%s", detail ? detail : "");
    return jm_frame(m, R_SYSTEM, body, sizeof body);
}
