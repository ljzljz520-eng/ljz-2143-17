#define _POSIX_C_SOURCE 200809L

#include "replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool replay_stream(FILE *fp, const Policy *policy, ReplaySummary *out,
                          Dispatcher *disp_out) {
    Journal j;
    memset(&j, 0, sizeof j);
    j.fp = fp;
    /* 校验文件头（显式线格式，不依赖结构体内存对齐） */
    uint8_t hb[JOURNAL_HEADER_SIZE];
    if (fread(hb, 1, sizeof hb, fp) != sizeof hb) {
        if (out) { snprintf(out->corrupt_reason, sizeof out->corrupt_reason,
                            "bad journal header"); out->corrupt = true; }
        return false;
    }
    if (!decode_header(hb, &j.header)) {
        if (out) { snprintf(out->corrupt_reason, sizeof out->corrupt_reason,
                            "bad journal header"); out->corrupt = true; }
        return false;
    }

    Dispatcher local_d;
    Dispatcher *d = disp_out ? disp_out : &local_d;
    disp_init_headless(d, policy, 0);

    bool eof = false, corrupt = false;
    JournalRecord rec;
    uint32_t last_ms = 0;
    while (!eof && !corrupt) {
        bool r = journal_read_next(&j, &rec, &eof, &corrupt);
        if (eof) break;
        if (!r) {
            if (out)
                snprintf(out->corrupt_reason, sizeof out->corrupt_reason,
                         "CRC mismatch or truncated frame at index %u",
                         j.frame_index);
            corrupt = true;
            break;
        }
        last_ms = rec.rel_ms;
        bool need_rec = false;
        disp_tick(d, rec.rel_ms, &need_rec);
        if (need_rec) /* headless：没有真实模式，按窗口化完成重对 */
            disp_report_display_mode(d, false);

        if (rec.type == R_INPUT) {
            InputEvent e;
            if (!journal_decode_input(rec.body, rec.body_len, &e)) {
                corrupt = true;
                if (out) snprintf(out->corrupt_reason,
                                  sizeof out->corrupt_reason,
                                  "malformed input frame");
                break;
            }
            /* 重放安全核心：来源强制覆盖。设备真实日志里的 origin 不被信任。 */
            e.origin = ORIGIN_REPLAY;
            e.platform = PLATFORM_REPLAY;
            disp_handle(d, &e);
            if (out) out->frames_input++;
        } else if (rec.type == R_SYSTEM) {
            InputEvent e;
            memset(&e, 0, sizeof e);
            e.origin = ORIGIN_REPLAY;
            e.platform = PLATFORM_REPLAY;
            uint8_t code = rec.body_len > 4 ? rec.body[4] : 0;
            switch (code) {
            case SYS_FOCUS_LOST:      e.type = IE_FOCUS_LOST; break;
            case SYS_FOCUS_GAINED:    e.type = IE_FOCUS_GAINED; break;
            case SYS_DISPLAY_LOST:    e.type = IE_DISPLAY_LOST; break;
            case SYS_DISPLAY_RESTORED:e.type = IE_DISPLAY_RESTORED; break;
            default: continue;
            }
            disp_handle(d, &e);
            if (out) out->frames_system++;
        }
        /* R_COMMAND/R_ACTIVATION/R_REPLAY_BLOCKED 为审计帧，不回放 */
    }

    if (out) {
        out->corrupt = corrupt;
        out->blocked_business = d->stat_blocked_business;
        out->commands_emitted = d->stat_emitted;
        out->duration_ms = last_ms;
    }
    return !corrupt;
}

bool replay_memory(const uint8_t *data, size_t len, const Policy *policy,
                   ReplaySummary *out, Dispatcher *disp_out) {
    if (out) memset(out, 0, sizeof *out);
    FILE *fp = fmemopen((void *)data, len, "rb");
    if (!fp) return false;
    bool ok = replay_stream(fp, policy, out, disp_out);
    fclose(fp);
    return ok;
}

bool replay_file(const char *path, const Policy *policy, ReplaySummary *out,
                 Dispatcher *disp_out) {
    if (out) memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    bool ok = replay_stream(fp, policy, out, disp_out);
    fclose(fp);
    return ok;
}

uint32_t replay_drain(Dispatcher *d, ReplayCmd *out, uint32_t max) {
    uint32_t n = 0;
    Command c;
    while (n < max && cq_pop(&d->q, &c)) {
        out[n].name = cmd_name(c.id);
        out[n].arg = c.arg;
        out[n].origin = c.origin;
        n++;
    }
    return n;
}
