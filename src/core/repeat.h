/*
 * repeat.h - 方向键长按连发（有界连续移动）
 *
 * 操作系统自动重复事件被忽略，改由本控制器基于单调时钟产生移动命令：
 *   keydown 立即移动 1 步 -> initial_delay -> 每 repeat_ms 一步
 *   -> 达到 max_hold_ms 自动停止（时间上界）
 * 视口到达边界时 emit 回调返回 false，立即停止（空间上界）。
 * 失焦 / 策略激活边界 / 抬键 / 切换层，都会立刻取消对应槽位。
 */
#ifndef INPUT_STRATEGY_REPEAT_H
#define INPUT_STRATEGY_REPEAT_H

#include <stdbool.h>
#include <stdint.h>

#include "command.h"
#include "events.h"
#include "key.h"

#define REPEAT_SLOTS 4

typedef struct {
    bool active;
    NK_Key key;
    CommandId command;
    uint32_t start_ms;
    uint32_t last_ms;
    uint32_t deadline_ms;
    uint32_t count;
    EventOrigin origin; /* 发起该连发的输入来源（重放时保持 replay） */
} RepeatSlot;

typedef struct {
    RepeatSlot slots[REPEAT_SLOTS];
    uint32_t initial_delay_ms;
    uint32_t repeat_ms;
    uint32_t max_hold_ms;
    /* 返回 false 表示目标方向已被视口钳制，停止连发 */
    bool (*emit)(void *userdata, Command c);
    void *userdata;
} RepeatController;

void repeat_init(RepeatController *r, uint32_t initial_delay,
                 uint32_t interval, uint32_t max_hold);
void repeat_set_clock_params(RepeatController *r, uint32_t initial_delay,
                             uint32_t interval, uint32_t max_hold);
void repeat_start(RepeatController *r, NK_Key key, CommandId cmd,
                  uint32_t now_ms, EventOrigin origin);
void repeat_stop(RepeatController *r, NK_Key key);
void repeat_stop_all(RepeatController *r);
void repeat_tick(RepeatController *r, uint32_t now_ms);
bool repeat_any_active(const RepeatController *r);

#endif
