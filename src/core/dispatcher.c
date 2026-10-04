#include "dispatcher.h"

#include <stdio.h>
#include <string.h>

/* ---------- 内部辅助 ---------- */

static void emit(Dispatcher *d, Command c) {
    /* 统一安全闸：非现场真人不得产生业务确认。
       这是"网页重放只能复现实验输入，不得伪造现场业务确认"的唯一强制点，
       无论命令来自按键映射、重放还是远程注入，都在此被拦下。 */
    if (cmd_is_business_confirm(c.id) && c.origin != ORIGIN_HUMAN) {
        d->stat_blocked_business++;
        /* 被拦的伪造确认也走钩子，便于审计/写 R_REPLAY_BLOCKED */
        if (d->ops.on_command) d->ops.on_command(d->ops.ud, &c);
        return;
    }
    if (c.id != CMD_NONE) {
        d->stat_commands++;
        d->stat_emitted++;
        if (d->ops.on_command) d->ops.on_command(d->ops.ud, &c);
        /* 无头模式（测试/参考模拟器/网页复算）命令从队列取出执行；
           有 UI 时命令已通过专用回调完成副作用，避免双重执行。 */
        if (d->headless)
            cq_push(&d->q, c);
    }
}

static Command make_cmd(CommandId id, EventOrigin origin) {
    Command c = {0};
    c.id = id;
    c.origin = origin;
    return c;
}

static uint32_t mod_bit(NK_Key key) {
    switch (key) {
    case NK_LSHIFT: case NK_RSHIFT: return KM_SHIFT;
    case NK_LCTRL:  case NK_RCTRL:  return KM_CTRL;
    case NK_LALT:   case NK_RALT:   return KM_ALT;
    case NK_LGUI:   case NK_RGUI:   return KM_GUI;
    default: return 0;
    }
}

static bool is_hard_quit_chord(KeyChord c) {
    KeyChord cq = {NK_Q, KM_CTRL};
    KeyChord mq = {NK_Q, KM_GUI};
    KeyChord a4 = {NK_F4, KM_ALT};
    return chord_equal(c, cq) || chord_equal(c, mq) || chord_equal(c, a4);
}

/* ---------- 展示层按键处理 ---------- */

static bool handle_presentation_key(Dispatcher *d, const InputEvent *e) {
    KeyChord chord = {e->key, e->mods};
    CommandId cmd = policy_lookup(act_current(&d->act),
                                  LAYER_PRESENTATION, chord);
    if (cmd == CMD_NONE) return false;
    if (!cmd_allowed_presentation(cmd)) return false;

    if (cmd_is_movement(cmd)) {
        if (e->down) {
            if (e->repeat) { d->stat_repeat_swallowed++; return true; }
            /* 立即移动一步；视口若已钳制则连发不启动。
               有 UI 回调时走回调；无头模式入队由消费者执行。 */
            bool moved = true;
            Command c0 = make_cmd(cmd, e->origin);
            c0.arg = 1;
            d->stat_emitted++;
            if (d->ops.on_command) d->ops.on_command(d->ops.ud, &c0);
            if (d->ops.do_move) {
                moved = d->ops.do_move(d->ops.ud, cmd, 1);
            } else if (d->headless) {
                cq_push(&d->q, c0);
            }
            if (moved)
                repeat_start(&d->repeat, e->key, cmd, d->now_ms, e->origin);
        } else {
            repeat_stop(&d->repeat, e->key);
        }
        return true;
    }

    if (cmd_is_edge_only(cmd)) {
        if (!e->down) {
            /* 边沿命令的抬起只做簿记（全屏 ARM 在 fs_request 内处理） */
            if (cmd == CMD_TOGGLE_FULLSCREEN) fs_keyup(&d->fs, e->key);
            return true;
        }
        if (e->repeat) {
            /* F11 按住的自动重复：绝不允许把全屏来回切换 */
            d->stat_repeat_swallowed++;
            return true;
        }
        /* 统一安全闸：confirm_business 的非真人来源在 emit() 内被拦截，
           其余边沿命令计数、审计钩子、无头入队也全部经 emit() 完成。 */
        emit(d, make_cmd(cmd, e->origin));
        /* 被安全闸拦截（重放伪造确认）后不执行任何副作用 */
        if (cmd == CMD_CONFIRM_BUSINESS && e->origin != ORIGIN_HUMAN)
            return true;
        switch (cmd) {
        case CMD_TOGGLE_FULLSCREEN: {
            if (fs_request(&d->fs, e->key, true, false, d->now_ms)) {
                bool done = d->ops.do_toggle_fullscreen
                    ? d->ops.do_toggle_fullscreen(d->ops.ud,
                                                  d->fs.pending_target)
                    : true;
                if (done) {
                    bool actual = d->ops.is_actually_fullscreen
                        ? d->ops.is_actually_fullscreen(d->ops.ud)
                        : d->fs.pending_target;
                    fs_switch_ok(&d->fs, actual);
                }
                /* done==false：等待平台异步结果；平台稍后必须
                   调用 disp_fullscreen_result() */
            } else {
                d->stat_edge_swallowed++;
            }
            return true;
        }
        case CMD_RESTORE_BACKGROUND:
            if (d->ops.do_restore_background)
                d->ops.do_restore_background(d->ops.ud);
            return true;
        case CMD_CONFIRM_BUSINESS:
            /* 安全闸已在预发射处（emit 路径）确认 origin==HUMAN；
               非真人不会执行到这里。 */
            if (e->origin == ORIGIN_HUMAN && d->ops.do_confirm_business)
                d->ops.do_confirm_business(d->ops.ud);
            return true;
        case CMD_OPEN_SETTINGS:
            if (d->ops.do_open_panel) d->ops.do_open_panel(d->ops.ud);
            /* 直接切换面板状态（不经过 disp_open_panel 的重复回调） */
            if (!d->panel.open)
                memset(d->panel.text, 0, DISPATCHER_TEXT_LEN);
            d->panel.open = true;
            d->panel.caret = 0;
            d->panel.composing = false;
            d->panel.focus = FOCUS_FIELD;
            repeat_stop_all(&d->repeat);
            fs_focus_lost(&d->fs);
            return true;
        default:
            return false;
        }
    }
    return false;
}

/* ---------- 输入框文本编辑 ---------- */

static void field_insert(PanelState *p, const char *s, EventOrigin origin) {
    size_t n = strlen(s);
    if (n == 0) return;
    if (p->caret + n >= DISPATCHER_TEXT_LEN)
        n = DISPATCHER_TEXT_LEN - 1 - p->caret;
    memmove(p->text + p->caret + n, p->text + p->caret,
            strlen(p->text + p->caret) + 1);
    memcpy(p->text + p->caret, s, n);
    p->caret += (uint32_t)n;
    (void)origin;
}

static void field_backspace(PanelState *p) {
    if (p->caret == 0) return;
    size_t n = strlen(p->text + p->caret);
    memmove(p->text + p->caret - 1, p->text + p->caret, n + 1);
    p->caret--;
}

static bool handle_field_key(Dispatcher *d, const InputEvent *e) {
    PanelState *p = &d->panel;
    if (!e->down) return true; /* 抬起一律吞掉 */

    /* IME 组合态：快捷键一律不得截走（中文输入中的 Esc/方向键/F11
       属于输入法：Esc 取消组词、方向键移动组词光标等）。
       唯一例外 Ctrl+Q/Cmd+Q/Alt+F4 可靠退出。 */
    if (e->composition) {
        KeyChord hard = {e->key, e->mods};
        if (is_hard_quit_chord(hard)) {
            d->quit_requested = true;
            emit(d, make_cmd(CMD_QUIT, e->origin));
        } else {
            d->stat_ime_swallowed++;
        }
        return true;
    }

    /* 未组合时，字段层只认少量内置编辑键；其他按键进入文本流程。 */
    switch (e->key) {
    case NK_ESC:
        /* 设置面板打开时按 Esc：可靠关闭面板（内置、不可改键） */
        disp_close_panel(d);
        emit(d, make_cmd(CMD_CLOSE_PANEL, e->origin));
        return true;
    case NK_BACKSPACE:
        if (!e->repeat) { field_backspace(p); }
        else { field_backspace(p); } /* 编辑键重复由字段自身处理 */
        emit(d, make_cmd(CMD_TEXT_BACKSPACE, e->origin));
        return true;
    case NK_RETURN:
        emit(d, make_cmd(CMD_TEXT_ENTER, e->origin));
        return true;
    case NK_LEFT:
        if (p->caret > 0) p->caret--;
        return true;
    case NK_RIGHT:
        if (p->caret < strlen(p->text)) p->caret++;
        return true;
    case NK_HOME:
        p->caret = 0;
        return true;
    case NK_END:
        p->caret = (uint32_t)strlen(p->text);
        return true;
    case NK_TAB:
        return true; /* 不允许 Tab 焦点跳出该字段（模态内闭环） */
    default:
        /* 组合修饰（Ctrl/Alt 等）的功能键不转成文本；只有真正的
           可打印输入由 IE_TEXT 事件携带（见 handle_text），
           这里忽略裸 keydown 的字符，避免跨平台 keycode 依赖。 */
        if (e->mods & (KM_CTRL | KM_ALT | KM_GUI)) {
            KeyChord hard = {e->key, e->mods};
            if (is_hard_quit_chord(hard)) {
                d->quit_requested = true;
                emit(d, make_cmd(CMD_QUIT, e->origin));
            }
            return true;
        }
        return true;
    }
}

/* ---------- 模态层（面板打开、字段未聚焦） ---------- */

static bool handle_modal_key(Dispatcher *d, const InputEvent *e) {
    /* 模态 Esc 固定为关闭面板：内置、策略不可改、永远可靠 */
    KeyChord chord = {e->key, e->mods};
    if (is_hard_quit_chord(chord)) {
        if (e->down && !e->repeat) {
            d->quit_requested = true;
            emit(d, make_cmd(CMD_QUIT, e->origin));
        }
        return true;
    }
    if (e->key == NK_ESC && e->mods == 0) {
        if (e->down && !e->repeat) {
            disp_close_panel(d);
            emit(d, make_cmd(CMD_CLOSE_PANEL, e->origin));
        }
        return true; /* Esc 重复与抬起都被模态吃掉 */
    }
    /* 模态下展示层快捷键一律不穿透 */
    return true;
}

/* ---------- 生命周期 ---------- */

void disp_init(Dispatcher *d, const Policy *initial, DispatcherOps ops,
               uint32_t now_ms) {
    memset(d, 0, sizeof *d);
    act_init(&d->act);
    act_load_initial(&d->act, initial);
    const Policy *p = act_current(&d->act);
    repeat_init(&d->repeat, p->move_initial_delay_ms, p->move_repeat_ms,
                p->move_max_hold_ms);
    fs_init(&d->fs, 2000);
    cq_init(&d->q);
    d->ops = ops;
    d->now_ms = now_ms;
}

void disp_init_headless(Dispatcher *d, const Policy *initial, uint32_t now_ms) {
    disp_init(d, initial, (DispatcherOps){0}, now_ms);
    d->headless = true;
}

void disp_open_panel(Dispatcher *d) {
    if (!d->panel.open)
        memset(d->panel.text, 0, DISPATCHER_TEXT_LEN);
    d->panel.open = true;
    d->panel.caret = 0;
    d->panel.composing = false;
    d->panel.composition[0] = '\0';
    d->panel.focus = FOCUS_FIELD; /* 打开即聚焦输入框 */
    repeat_stop_all(&d->repeat);
    fs_focus_lost(&d->fs);
    if (d->ops.do_open_panel) d->ops.do_open_panel(d->ops.ud);
}

void disp_close_panel(Dispatcher *d) {
    bool was = d->panel.open;
    d->panel.open = false;
    d->panel.composing = false;
    d->panel.focus = FOCUS_NONE;
    repeat_stop_all(&d->repeat);
    if (was && d->ops.do_close_panel) d->ops.do_close_panel(d->ops.ud);
}

bool disp_panel_open(const Dispatcher *d) {
    return d->panel.open;
}

void disp_focus_field(Dispatcher *d, bool focus) {
    if (d->panel.open)
        d->panel.focus = focus ? FOCUS_FIELD : FOCUS_NONE;
}

const char *disp_panel_text(const Dispatcher *d) { return d->panel.text; }

void disp_fullscreen_result(Dispatcher *d, bool actual_fullscreen) {
    fs_switch_ok(&d->fs, actual_fullscreen);
}

void disp_report_display_mode(Dispatcher *d, bool actual_fullscreen) {
    fs_reconcile_to(&d->fs, actual_fullscreen);
}

/* ---------- 激活边界 ---------- */

uint32_t disp_stage_and_commit(Dispatcher *d, const Policy *p,
                               const PolicyEnvelope *env, uint32_t now_ms) {
    d->now_ms = now_ms;
    act_stage(&d->act, p, env);
    /* T1：停止所有连发；解除全屏 ARM；合成旧键抬起（按旧映射收尾）。
       合成抬起只做收尾，绝不下发命令——旧命令不会在新代次生效。 */
    repeat_stop_all(&d->repeat);
    uint32_t n = act_commit_prepare(&d->act);
    for (uint32_t i = 0; i < n; ++i) {
        NK_Key k = d->act.release_list[i];
        repeat_stop(&d->repeat, k);
        fs_keyup(&d->fs, k);
    }
    act_commit_finish(&d->act);
    d->mods = 0; /* 带修饰和弦必须重新按下 */
    repeat_set_clock_params(&d->repeat, p->move_initial_delay_ms,
                            p->move_repeat_ms, p->move_max_hold_ms);
    return n;
}

/* ---------- 主事件入口 ---------- */

static void handle_non_key(Dispatcher *d, const InputEvent *e) {
    switch (e->type) {
    case IE_TEXT:
        if (d->panel.open && d->panel.focus == FOCUS_FIELD &&
            !d->panel.composing) {
            field_insert(&d->panel, e->text, e->origin);
            Command c = make_cmd(CMD_TEXT_INSERT, e->origin);
            snprintf(c.text, sizeof c.text, "%s", e->text);
            emit(d, c);
        }
        break;
    case IE_COMPOSITION:
        if (d->panel.open && d->panel.focus == FOCUS_FIELD) {
            d->panel.composing = e->text[0] != '\0';
            snprintf(d->panel.composition, sizeof d->panel.composition,
                     "%s", e->text);
            d->stat_ime_swallowed++;
        }
        break;
    case IE_FOCUS_LOST:
        /* 失焦时可能漏收抬键：全部视为抬起，消除卡键。 */
        repeat_stop_all(&d->repeat);
        fs_focus_lost(&d->fs);
        d->mods = 0;
        for (int i = 0; i < ACT_HELD_KEYS_MAX; ++i)
            d->act.held_down[i] = false;
        break;
    case IE_FOCUS_GAINED:
        /* 重新聚焦后从干净状态开始；物理状态由后续真实事件重建 */
        d->mods = 0;
        break;
    case IE_DISPLAY_LOST:
        fs_display_lost(&d->fs);
        break;
    case IE_DISPLAY_RESTORED: {
        bool actual = d->ops.is_actually_fullscreen
            ? d->ops.is_actually_fullscreen(d->ops.ud)
            : false;
        fs_display_restored(&d->fs, actual);
        fs_reconcile_to(&d->fs, actual);
        break;
    }
    default:
        break;
    }
}

void disp_handle(Dispatcher *d, const InputEvent *e) {
    if (e->type != IE_KEY) { handle_non_key(d, e); return; }

    /* 修饰键状态维护（先于一切映射） */
    uint32_t mb = mod_bit(e->key);
    if (mb) {
        if (e->down) d->mods |= mb; else d->mods &= ~mb;
        if (e->down) act_mark_down(&d->act, e->key, d->mods);
        else act_mark_up(&d->act, e->key);
        /* 修饰键本身永不映射为命令 */
        return;
    }
    if (e->down) act_mark_down(&d->act, e->key, d->mods);
    else act_mark_up(&d->act, e->key);

    /* 硬保留退出键：任何层、任何 IME 状态、任何策略代次都生效。
       事件自带 mods（归一化快照）与跟踪态任一满足即判定，避免平台间
       修饰键抬起事件先后差异造成退出键偶发失效。 */
    KeyChord chord_track = {e->key, d->mods};
    KeyChord chord_event = {e->key, e->mods};
    if (is_hard_quit_chord(chord_track) || is_hard_quit_chord(chord_event)) {
        if (e->down && !e->repeat) {
            d->quit_requested = true;
            emit(d, make_cmd(CMD_QUIT, e->origin));
            if (d->ops.do_quit) d->ops.do_quit(d->ops.ud);
        }
        return;
    }

    /* 分层路由：模态优先；面板内再分 IME 组合态 / 输入框 / 模态空白区 */
    if (d->panel.open) {
        if (d->panel.composing) {
            /* 组合态统一门控（字段键处理内含同样规则，直接复用） */
            handle_field_key(d, e);
        } else if (d->panel.focus == FOCUS_FIELD) {
            handle_field_key(d, e);
        } else {
            handle_modal_key(d, e);
        }
        return;
    }

    /* 展示层 */
    InputEvent norm = *e;
    norm.mods = d->mods;
    handle_presentation_key(d, &norm);
}

/* ---------- 帧驱动 ---------- */

static bool repeat_emit_cb(void *userdata, Command c) {
    Dispatcher *d = userdata;
    /* origin 来自发起该连发的原始输入（真人=human，重放=replay）。
       移动不涉及业务确认，标记保持真实来源以便审计。 */
    bool allowed = true;
    d->stat_emitted++;
    if (d->ops.on_command) d->ops.on_command(d->ops.ud, &c);
    if (d->ops.do_move) {
        allowed = d->ops.do_move(d->ops.ud, c.id, c.arg);
    } else if (d->headless) {
        cq_push(&d->q, c);
        allowed = true; /* 无头模式下边界由消费者反馈 */
    }
    return allowed; /* false = 视口钳制，停连发 */
}

void disp_tick(Dispatcher *d, uint32_t now_ms, bool *needs_reconcile) {
    d->now_ms = now_ms;
    d->repeat.emit = repeat_emit_cb;
    d->repeat.userdata = d;
    repeat_tick(&d->repeat, now_ms);
    if (needs_reconcile) *needs_reconcile = fs_tick(&d->fs, now_ms);
}
