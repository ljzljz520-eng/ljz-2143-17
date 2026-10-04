/*
 * dispatcher.h - 分层输入分发器
 *
 * 三层（自上而下优先级）：
 *   1. 模态面板  LAYER_MODAL：设置面板打开时，展示层快捷键全部失效；
 *      Esc/Ctrl+Q 等"退出语义"永远可用（内置、不可改键）。
 *   2. 输入框：面板中的文本字段获得焦点后，按键默认走文本编辑；
 *      IME 组合态（中文输入过程中）事件带 composition 标记，
 *      任何快捷键都不得截走这些事件（除 Ctrl+Q 硬退出）。
 *   3. 展示画面 LAYER_PRESENTATION：策略映射生效层。
 *
 * 所有输入最终只通过一条统一命令队列输出；origin 标记决定安全闸。
 */
#ifndef INPUT_STRATEGY_DISPATCHER_H
#define INPUT_STRATEGY_DISPATCHER_H

#include <stdbool.h>
#include <stdint.h>

#include "activation.h"
#include "command.h"
#include "events.h"
#include "fullscreen.h"
#include "policy.h"
#include "repeat.h"

#define DISPATCHER_TEXT_LEN 128

typedef enum { FOCUS_NONE = 0, FOCUS_FIELD = 1 } FieldFocus;

typedef struct {
    bool open;                  /* 模态面板是否打开 */
    char text[DISPATCHER_TEXT_LEN];
    uint32_t caret;
    bool composing;            /* IME 组合中 */
    char composition[DISPATCHER_TEXT_LEN];
    FieldFocus focus;         /* 打开时默认聚焦输入框 */
} PanelState;

typedef struct {
    /* 移动执行（含视口钳制：返回 false 表示已到边界） */
    bool (*do_move)(void *ud, CommandId cmd, int32_t steps);
    /* 返回 true 表示请求了平台切换；调用方完成后回报 fs_switch_ok */
    bool (*do_toggle_fullscreen)(void *ud, bool target_fullscreen);
    bool (*is_actually_fullscreen)(void *ud);
    void (*do_restore_background)(void *ud);
    /* 业务确认回调：仅 ORIGIN_HUMAN 会到达这里 */
    void (*do_confirm_business)(void *ud);
    void (*do_open_panel)(void *ud);
    void (*do_close_panel)(void *ud);
    void (*do_quit)(void *ud);
    /* 每个实际生效命令的观察钩子（写日志/审计），不产生副作用 */
    void (*on_command)(void *ud, const Command *c);
    void *ud;
} DispatcherOps;

typedef struct {
    Activation act;
    RepeatController repeat;
    FullscreenFsm fs;
    PanelState panel;
    CommandQueue q;
    DispatcherOps ops;

    /* 当前物理修饰键状态（失焦/激活边界时清零） */
    uint32_t mods;
    bool quit_requested;
    uint32_t now_ms;

    /* 无头模式（测试/参考模拟器）：无 UI 回调，命令全部进队列 */
    bool headless;

    /* 统计（供测试与诊断） */
    uint32_t stat_commands;
    uint32_t stat_emitted; /* 实际生效（未被安全闸拦截） */
    uint32_t stat_blocked_business;
    uint32_t stat_ime_swallowed;
    uint32_t stat_repeat_swallowed;
    uint32_t stat_edge_swallowed;
} Dispatcher;

void disp_init(Dispatcher *d, const Policy *initial, DispatcherOps ops,
               uint32_t now_ms);
/* 测试/参考模拟器：不传任何 UI 回调，全部命令走队列 */
void disp_init_headless(Dispatcher *d, const Policy *initial, uint32_t now_ms);

/* 暂存并立即提交新策略（设备在按键处理间隙轮询拉取的典型用法）。
   返回合成抬起的键数量；切换按 activation.h 的边界语义完成。 */
uint32_t disp_stage_and_commit(Dispatcher *d, const Policy *p,
                               const PolicyEnvelope *env, uint32_t now_ms);

/* 核心入口：归一化事件 -> 命令队列（副作用：面板状态/连发/FSM/日志钩子） */
void disp_handle(Dispatcher *d, const InputEvent *e);

/* 每帧调用：驱动连发与全屏超时。
   needs_reconcile 返回 true 时，平台层必须读取实际全屏模式并调用
   disp_report_display_mode()。 */
void disp_tick(Dispatcher *d, uint32_t now_ms, bool *needs_reconcile);
void disp_report_display_mode(Dispatcher *d, bool actual_fullscreen);

void disp_open_panel(Dispatcher *d);
void disp_close_panel(Dispatcher *d);
bool disp_panel_open(const Dispatcher *d);
void disp_focus_field(Dispatcher *d, bool focus);

/* 平台层回报全屏切换结果 */
void disp_fullscreen_result(Dispatcher *d, bool actual_fullscreen);

const char *disp_panel_text(const Dispatcher *d);

#endif
