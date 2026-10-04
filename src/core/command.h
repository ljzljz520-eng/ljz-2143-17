/*
 * command.h - 统一命令队列
 *
 * 架构抉择（详见 docs/design.md）：
 *   "本地归一化输入事件" 与 "统一命令队列" 不是二选一，而是分层共存——
 *   平台边界把原始事件归一化为 InputEvent；策略匹配后产出 Command，
 *   所有上层（UI/回放/模拟器）只消费命令队列。这保证：
 *     1) 重放走与真人完全相同的路径；
 *     2) 安全闸只有一个检查点；
 *     3) 业务确认无法被非现场来源伪造（见 replay.h）。
 */
#ifndef INPUT_STRATEGY_COMMAND_H
#define INPUT_STRATEGY_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

#include "events.h"

typedef enum {
    CMD_NONE = 0,

    /* 模态面板 */
    CMD_CLOSE_PANEL,
    CMD_OPEN_SETTINGS,

    /* 输入框（内置，策略不可映射到这些命令） */
    CMD_TEXT_INSERT,
    CMD_TEXT_BACKSPACE,
    CMD_TEXT_ENTER,

    /* 展示画面：移动（支持有界连发） */
    CMD_MOVE_LEFT,
    CMD_MOVE_RIGHT,
    CMD_MOVE_UP,
    CMD_MOVE_DOWN,

    /* 展示画面：单次动作（边沿触发，自动重复必须被吞掉） */
    CMD_TOGGLE_FULLSCREEN,
    CMD_RESTORE_BACKGROUND,

    /* 业务动作：由现场真人确认，重放/远程不得伪造 */
    CMD_CONFIRM_BUSINESS,

    /* 系统 */
    CMD_QUIT,

    CMD__COUNT
} CommandId;

const char *cmd_name(CommandId id);
CommandId cmd_from_name(const char *name);

/* 该命令是否属于"业务确认"类（安全闸拦截对象） */
bool cmd_is_business_confirm(CommandId id);
/* 该命令是否为可连发的移动类 */
bool cmd_is_movement(CommandId id);
/* 该命令是否必须边沿触发（按下只生效一次） */
bool cmd_is_edge_only(CommandId id);
/* 该命令是否允许在模态层使用 */
bool cmd_allowed_modal(CommandId id);
/* 该命令是否允许在展示层使用 */
bool cmd_allowed_presentation(CommandId id);

typedef struct {
    CommandId id;
    EventOrigin origin;          /* 产生来源（安全闸用） */
    int32_t arg;                 /* 移动步长/计数等 */
    char text[64];               /* 文本插入内容 */
} Command;

#define COMMAND_QUEUE_CAP 64

typedef struct {
    Command buf[COMMAND_QUEUE_CAP];
    uint32_t head, tail, count;
    uint64_t total_dropped;      /* 溢出计数（写日志/诊断） */
} CommandQueue;

void cq_init(CommandQueue *q);
bool cq_push(CommandQueue *q, Command c);
bool cq_pop(CommandQueue *q, Command *out);
uint32_t cq_count(const CommandQueue *q);

#endif
