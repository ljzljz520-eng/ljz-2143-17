/*
 * fullscreen.h - 全屏切换边沿触发 FSM
 *
 * 缺陷背景：F11 按住产生的键重复会把窗口反复全屏/窗口化。
 * 方案：
 *   - 物理键首次 keydown（repeat==false）才 ARM；
 *   - ARM 后忽略同一物理键的所有重复/再次按下，直到真正 keyup；
 *   - 切换是异步操作（平台调用 + 显示模式确认），中间状态不再接受
 *     新的切换请求，避免"切换中拔屏"后状态错乱；
 *   - 切换有超时保护，超时/拔屏后由 DISPLAY 事件与实际模式 reconcile。
 */
#ifndef INPUT_STRATEGY_FULLSCREEN_H
#define INPUT_STRATEGY_FULLSCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include "key.h"

typedef enum {
    FS_WINDOWED = 1,   /* 窗口化，空闲 */
    FS_SWITCHING,      /* 已发起切换，等待平台确认 */
    FS_FULLSCREEN,     /* 全屏，空闲 */
    FS_RECONCILING     /* 显示异常，等待 DISPLAY_RESTORED/超时 reconcile */
} FsState;

typedef struct {
    FsState state;
    NK_Key armed_key;       /* 按住中的触键（0 表示无） */
    bool pending_target;    /* 切换目标：true=全屏 */
    uint32_t since_ms;
    uint32_t timeout_ms;    /* 切换/重对最长等待 */
    uint32_t enters, exits, losts, reconciles; /* 诊断计数 */
} FullscreenFsm;

void fs_init(FullscreenFsm *f, uint32_t timeout_ms);

/* 返回 true 表示这次事件应当真正执行一次平台全屏切换。
   调用方随后必须以 fs_switch_result 回报结果。 */
bool fs_request(FullscreenFsm *f, NK_Key key, bool down, bool repeat,
                uint32_t now_ms);

/* 平台切换成功（actual_fullscreen 为实际落地模式） */
void fs_switch_ok(FullscreenFsm *f, bool actual_fullscreen);

/* 物理键抬起：仅解除 ARM，不产生任何切换（防来回触发的关键） */
void fs_keyup(FullscreenFsm *f, NK_Key key);

/* 全屏切换过程中显示器被拔出 */
void fs_display_lost(FullscreenFsm *f);
/* 显示器恢复：进入重对，需要调用方报告当前实际模式 */
void fs_display_restored(FullscreenFsm *f, bool actual_fullscreen);

/* 周期 tick：切换/重对超时则进入 reconcile，返回 true 表示需要调用方
   读取实际模式并调用 fs_reconcile_to()。 */
bool fs_tick(FullscreenFsm *f, uint32_t now_ms);
/* 用平台报告的真实模式完成 reconcile */
void fs_reconcile_to(FullscreenFsm *f, bool actual_fullscreen);

/* 失焦：取消所有 ARM，防止漏收 keyup 造成永久触发态；
   进行中的切换不受影响（由结果/超时收尾）。 */
void fs_focus_lost(FullscreenFsm *f);

const char *fs_state_name(FsState s);

#endif
