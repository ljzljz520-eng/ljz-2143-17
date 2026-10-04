#include "fullscreen.h"

#include <string.h>

void fs_init(FullscreenFsm *f, uint32_t timeout_ms) {
    memset(f, 0, sizeof *f);
    f->state = FS_WINDOWED;
    f->timeout_ms = timeout_ms ? timeout_ms : 2000;
}

bool fs_request(FullscreenFsm *f, NK_Key key, bool down, bool repeat,
                uint32_t now_ms) {
    if (!down) { fs_keyup(f, key); return false; }
    if (repeat) return false;                 /* 吞掉操作系统键重复 */
    if (f->armed_key == key) return false;    /* 同键二次按下，忽略 */

    /* 切换中 / 重对中：不排队第二个切换请求（不翻转目标），
       拔屏恢复后模式由实际状态决定，绝不会来回跳。 */
    if (f->state == FS_SWITCHING || f->state == FS_RECONCILING)
        return false;

    /* 目标 = 当前空闲态的反面 */
    f->pending_target = (f->state == FS_WINDOWED);
    f->armed_key = key;
    f->state = FS_SWITCHING;
    f->since_ms = now_ms;
    return true;
}

void fs_switch_ok(FullscreenFsm *f, bool actual_fullscreen) {
    f->state = actual_fullscreen ? FS_FULLSCREEN : FS_WINDOWED;
    if (actual_fullscreen) f->enters++; else f->exits++;
}

void fs_keyup(FullscreenFsm *f, NK_Key key) {
    if (f->armed_key == key) f->armed_key = 0;
}

void fs_display_lost(FullscreenFsm *f) {
    f->losts++;
    if (f->state == FS_SWITCHING || f->state == FS_FULLSCREEN)
        f->state = FS_RECONCILING;
}

void fs_display_restored(FullscreenFsm *f, bool actual_fullscreen) {
    (void)actual_fullscreen;
    f->state = FS_RECONCILING;
}

bool fs_tick(FullscreenFsm *f, uint32_t now_ms) {
    if (f->state == FS_SWITCHING || f->state == FS_RECONCILING) {
        if ((int32_t)(now_ms - f->since_ms) >= (int32_t)f->timeout_ms) {
            f->state = FS_RECONCILING;
            f->since_ms = now_ms;
            return true; /* 请求调用方读取实际模式 */
        }
    }
    return false;
}

void fs_reconcile_to(FullscreenFsm *f, bool actual_fullscreen) {
    f->state = actual_fullscreen ? FS_FULLSCREEN : FS_WINDOWED;
    f->reconciles++;
}

void fs_focus_lost(FullscreenFsm *f) {
    f->armed_key = 0; /* 漏收的 keyup 一律视为已抬起 */
}

const char *fs_state_name(FsState s) {
    switch (s) {
    case FS_WINDOWED: return "windowed";
    case FS_SWITCHING: return "switching";
    case FS_FULLSCREEN: return "fullscreen";
    case FS_RECONCILING: return "reconciling";
    }
    return "?";
}
