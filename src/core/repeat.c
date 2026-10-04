#include "repeat.h"

#include <string.h>

static RepeatSlot *find(RepeatController *r, NK_Key key) {
    for (int i = 0; i < REPEAT_SLOTS; ++i)
        if (r->slots[i].active && r->slots[i].key == key)
            return &r->slots[i];
    return NULL;
}

void repeat_init(RepeatController *r, uint32_t initial_delay,
                 uint32_t interval, uint32_t max_hold) {
    memset(r, 0, sizeof *r);
    r->initial_delay_ms = initial_delay;
    r->repeat_ms = interval;
    r->max_hold_ms = max_hold;
}

void repeat_set_clock_params(RepeatController *r, uint32_t initial_delay,
                             uint32_t interval, uint32_t max_hold) {
    r->initial_delay_ms = initial_delay;
    r->repeat_ms = interval;
    r->max_hold_ms = max_hold;
}

void repeat_start(RepeatController *r, NK_Key key, CommandId cmd,
                  uint32_t now_ms, EventOrigin origin) {
    RepeatSlot *s = find(r, key);
    if (s) return; /* 物理按下重复事件：忽略 */
    for (int i = 0; i < REPEAT_SLOTS; ++i) {
        if (!r->slots[i].active) {
            s = &r->slots[i];
            break;
        }
    }
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->active = true;
    s->key = key;
    s->command = cmd;
    s->start_ms = s->last_ms = now_ms;
    s->deadline_ms = now_ms + r->max_hold_ms;
    s->count = 0;
    s->origin = origin;
}

void repeat_stop(RepeatController *r, NK_Key key) {
    RepeatSlot *s = find(r, key);
    if (s) memset(s, 0, sizeof *s);
}

void repeat_stop_all(RepeatController *r) {
    for (int i = 0; i < REPEAT_SLOTS; ++i)
        memset(&r->slots[i], 0, sizeof r->slots[i]);
}

bool repeat_any_active(const RepeatController *r) {
    for (int i = 0; i < REPEAT_SLOTS; ++i)
        if (r->slots[i].active) return true;
    return false;
}

void repeat_tick(RepeatController *r, uint32_t now_ms) {
    for (int i = 0; i < REPEAT_SLOTS; ++i) {
        RepeatSlot *s = &r->slots[i];
        if (!s->active) continue;

        /* 时间上界：总按时长用尽，自动松开（有界连发） */
        if ((int32_t)(now_ms - s->deadline_ms) >= 0) {
            memset(s, 0, sizeof *s);
            continue;
        }
        uint32_t next_at = s->count == 0
            ? s->start_ms + r->initial_delay_ms
            : s->last_ms + r->repeat_ms;
        if ((int32_t)(now_ms - next_at) < 0) continue;

        Command c = {0};
        c.id = s->command;
        c.arg = 1;
        c.origin = s->origin;
        if (r->emit && !r->emit(r->userdata, c)) {
            /* 空间上界：视口钳制，停止该方向连发 */
            memset(s, 0, sizeof *s);
            continue;
        }
        s->count++;
        s->last_ms = now_ms;
    }
}
