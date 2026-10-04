#include "command.h"

#include <string.h>

static const char *NAMES[CMD__COUNT] = {
    [CMD_NONE] = "none",
    [CMD_CLOSE_PANEL] = "close_panel",
    [CMD_OPEN_SETTINGS] = "open_settings",
    [CMD_TEXT_INSERT] = "text_insert",
    [CMD_TEXT_BACKSPACE] = "text_backspace",
    [CMD_TEXT_ENTER] = "text_enter",
    [CMD_MOVE_LEFT] = "move_left",
    [CMD_MOVE_RIGHT] = "move_right",
    [CMD_MOVE_UP] = "move_up",
    [CMD_MOVE_DOWN] = "move_down",
    [CMD_TOGGLE_FULLSCREEN] = "toggle_fullscreen",
    [CMD_RESTORE_BACKGROUND] = "restore_background",
    [CMD_CONFIRM_BUSINESS] = "confirm_business",
    [CMD_QUIT] = "quit",
};

const char *cmd_name(CommandId id) {
    if ((unsigned)id >= CMD__COUNT) return "invalid";
    return NAMES[id];
}

CommandId cmd_from_name(const char *name) {
    if (!name) return CMD_NONE;
    for (int i = 0; i < CMD__COUNT; ++i)
        if (NAMES[i] && strcmp(NAMES[i], name) == 0) return (CommandId)i;
    return CMD_NONE;
}

bool cmd_is_business_confirm(CommandId id) { return id == CMD_CONFIRM_BUSINESS; }

bool cmd_is_movement(CommandId id) {
    return id == CMD_MOVE_LEFT || id == CMD_MOVE_RIGHT ||
           id == CMD_MOVE_UP || id == CMD_MOVE_DOWN;
}

bool cmd_is_edge_only(CommandId id) {
    /* 全屏切换来回触发是典型缺陷：F11 按住时的自动重复必须被吞掉。
       恢复背景、业务确认、面板开合同样只响应首次按下。 */
    return id == CMD_TOGGLE_FULLSCREEN || id == CMD_RESTORE_BACKGROUND ||
           id == CMD_CONFIRM_BUSINESS || id == CMD_OPEN_SETTINGS ||
           id == CMD_CLOSE_PANEL || id == CMD_QUIT;
}

bool cmd_allowed_modal(CommandId id) {
    switch (id) {
    case CMD_CLOSE_PANEL:
    case CMD_OPEN_SETTINGS:
    case CMD_TEXT_INSERT:
    case CMD_TEXT_BACKSPACE:
    case CMD_TEXT_ENTER:
    case CMD_QUIT:
        return true;
    default:
        return false; /* 展示层命令不得穿透模态面板 */
    }
}

bool cmd_allowed_presentation(CommandId id) {
    switch (id) {
    case CMD_OPEN_SETTINGS:
    case CMD_MOVE_LEFT:
    case CMD_MOVE_RIGHT:
    case CMD_MOVE_UP:
    case CMD_MOVE_DOWN:
    case CMD_TOGGLE_FULLSCREEN:
    case CMD_RESTORE_BACKGROUND:
    case CMD_CONFIRM_BUSINESS:
    case CMD_QUIT:
        return true;
    default:
        return false;
    }
}

void cq_init(CommandQueue *q) { memset(q, 0, sizeof *q); }

bool cq_push(CommandQueue *q, Command c) {
    if (q->count == COMMAND_QUEUE_CAP) {
        q->total_dropped++;
        return false;
    }
    q->buf[q->tail] = c;
    q->tail = (q->tail + 1) % COMMAND_QUEUE_CAP;
    q->count++;
    return true;
}

bool cq_pop(CommandQueue *q, Command *out) {
    if (q->count == 0) return false;
    *out = q->buf[q->head];
    q->head = (q->head + 1) % COMMAND_QUEUE_CAP;
    q->count--;
    return true;
}

uint32_t cq_count(const CommandQueue *q) { return q->count; }
