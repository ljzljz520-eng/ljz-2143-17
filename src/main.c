/*
 * main.c - C 客户端入口
 *
 * 组装：SDL2 平台适配 -> 归一化输入事件 -> Dispatcher（三层分发、
 * IME 门控、有界连发、全屏边沿 FSM、激活边界）-> 统一命令出口。
 * 全部输入写入可回放日志；策略经服务轮询获取（保留键/冲突由服务校验）；
 * 真人业务确认上报；replay/remote 永远无法产生业务确认。
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "core/clock.h"
#include "core/dispatcher.h"
#include "core/events.h"
#include "core/journal.h"
#include "core/policy.h"
#include "platform/net.h"
#include "platform/sdl_input.h"
#include "platform/sync.h"
#include "renderer.h"
#include "ui/ui.h"
#include "window.h"

#define WINDOW_TITLE "Input Strategy Kiosk"
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define BACKGROUND_IMAGE_PATH "assets/background.png"
#define SYNC_INTERVAL_MS 5000

typedef struct {
    AppWindow *win;
    Dispatcher *disp;
    PresentModel *model;
} Ctx;

static bool sdl_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    int flags = IMG_INIT_PNG | IMG_INIT_JPG;
    if ((IMG_Init(flags) & flags) == 0) {
        fprintf(stderr, "IMG_Init failed: %s\n", IMG_GetError());
        SDL_Quit();
        return false;
    }
    return true;
}

static bool do_move(void *ud, CommandId cmd, int32_t steps) {
    Ctx *c = ud;
    const Policy *p = act_current(&c->disp->act);
    int dx = 0, dy = 0;
    if (cmd == CMD_MOVE_LEFT) dx = -steps * 8;
    if (cmd == CMD_MOVE_RIGHT) dx = steps * 8;
    if (cmd == CMD_MOVE_UP) dy = -steps * 8;
    if (cmd == CMD_MOVE_DOWN) dy = steps * 8;
    int nx = c->model->margin_x + dx;
    int ny = c->model->margin_y + dy;
    int maxx = 64; /* 简单演示边界（受策略 bounds 参数约束的语义见 docs） */
    int maxy = 48;
    int cx = nx < -maxx ? -maxx : nx > maxx ? maxx : nx;
    int cy = ny < -maxy ? -maxy : ny > maxy ? maxy : ny;
    c->model->margin_x = cx;
    c->model->margin_y = cy;
    (void)p;
    return cx == nx && cy == ny; /* false = 被视口钳制，连发应停止 */
}

static bool do_toggle_fullscreen(void *ud, bool target) {
    Ctx *c = ud;
    SDL_SetWindowFullscreen(c->win->window,
        target ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    return true;
}

static bool is_actually_fullscreen(void *ud) {
    Ctx *c = ud;
    return (SDL_GetWindowFlags(c->win->window) &
            SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
}

static void do_restore_background(void *ud) {
    Ctx *c = ud;
    c->model->margin_x = 0;
    c->model->margin_y = 0;
    c->model->restored = true;
}

static void do_confirm_business(void *ud) {
    /* 仅真人触发（Dispatcher 已保证 origin==HUMAN）。 */
    Ctx *c = ud;
    const char *url_base = getenv("INPUT_STRATEGY_URL");
    const char *dev = getenv("INPUT_STRATEGY_DEVICE");
    if (!url_base || !dev) return;
    char url[512], body[256];
    snprintf(url, sizeof url, "%s/api/devices/%s/confirmations", url_base, dev);
    snprintf(body, sizeof body,
             "{\"generation\":%llu,\"origin\":\"human\",\"detail\":\"kiosk\"}",
             (unsigned long long)act_generation(&c->disp->act));
    HttpResponse r;
    if (http_post_json(url, body, &r)) http_response_free(&r);
}

static void do_open_panel(void *ud) { (void)ud; SDL_StartTextInput(); }
static void do_close_panel(void *ud) {
    (void)ud;
    /* 面板关闭后停止文本输入，展示层不再接收 IME 事件 */
    if (!SDL_IsTextInputActive()) return;
    SDL_StopTextInput();
}

static uint32_t g_start_ms;
static Journal g_journal;

static uint32_t rel_ms(void) {
    return clock_monotonic_ms() - g_start_ms;
}

static void on_command(void *ud, const Command *cmd) {
    (void)ud;
    if (cmd_is_business_confirm(cmd->id) && cmd->origin != ORIGIN_HUMAN) {
        journal_write_blocked(&g_journal, rel_ms(), cmd_name(cmd->id),
                              cmd->origin,
                              "non-human origin cannot confirm business");
    } else {
        journal_write_command(&g_journal, rel_ms(), cmd_name(cmd->id),
                              cmd->arg, cmd->origin);
    }
}

int main(void) {
    if (!sdl_init()) return 1;

    AppWindow app = {0};
    if (!window_init(&app, WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT)) {
        IMG_Quit(); SDL_Quit(); return 1;
    }
    SceneRenderer scene = {0};
    renderer_load_background(&scene, app.renderer, BACKGROUND_IMAGE_PATH);

    Policy initial;
    policy_default(&initial);
    PresentModel model = {0};
    Dispatcher disp;
    Ctx ctx = {&app, &disp, &model};
    DispatcherOps ops = {
        .do_move = do_move,
        .do_toggle_fullscreen = do_toggle_fullscreen,
        .is_actually_fullscreen = is_actually_fullscreen,
        .do_restore_background = do_restore_background,
        .do_confirm_business = do_confirm_business,
        .do_open_panel = do_open_panel,
        .do_close_panel = do_close_panel,
        .on_command = on_command,
        .ud = &ctx,
    };
    g_start_ms = clock_monotonic_ms();
    disp_init(&disp, &initial, ops, 0);

    const char *dev = getenv("INPUT_STRATEGY_DEVICE");
    const char *url = getenv("INPUT_STRATEGY_URL");
    if (dev && url) {
        sync_init((SyncConfig){url, dev}, NULL);
        sync_poll(&disp, 0, NULL);
    }

    /* 打开回放日志 */
    char jpath[160], started[32];
    snprintf(started, sizeof started, "%u", (unsigned)time(NULL));
    snprintf(jpath, sizeof jpath, "data/input-%s.islog",
             dev ? dev : "local");
    journal_open_write(&g_journal, jpath, dev ? dev : "local",
                       (uint64_t)act_generation(&disp.act), started);

    bool running = true;
    bool ime_composing = false;
    uint32_t last_sync = 0;

    while (running) {
        uint32_t now = clock_monotonic_ms() - g_start_ms;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_WINDOWEVENT &&
                 ev.window.event == SDL_WINDOWEVENT_CLOSE)) {
                running = false;
                break;
            }
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                InputEvent ie = sdl_key_event(
                    &ev.key, ORIGIN_HUMAN, ime_composing,
#if defined(__linux__)
                    PLATFORM_SDL_LINUX
#elif defined(_WIN32)
                    PLATFORM_SDL_WINDOWS
#elif defined(__APPLE__)
                    PLATFORM_SDL_MACOS
#else
                    PLATFORM_TEST
#endif
                );
                journal_write_input(&g_journal, now, &ie);
                disp_handle(&disp, &ie);
                if (disp.quit_requested) running = false;
            } else if (ev.type == SDL_TEXTINPUT) {
                InputEvent ie = {0};
                ie.type = IE_TEXT; ie.origin = ORIGIN_HUMAN;
                snprintf(ie.text, sizeof ie.text, "%s", ev.text.text);
                disp_handle(&disp, &ie);
            } else if (ev.type == SDL_TEXTEDITING) {
                InputEvent ie = {0};
                ie.type = IE_COMPOSITION; ie.origin = ORIGIN_HUMAN;
                snprintf(ie.text, sizeof ie.text, "%s", ev.edit.text);
                disp_handle(&disp, &ie);
                ime_composing = ev.edit.text[0] != 0;
            } else if (ev.type == SDL_WINDOWEVENT) {
                InputEvent ie = {0};
                ie.origin = ORIGIN_HUMAN;
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    ie.type = IE_FOCUS_LOST;
                    disp_handle(&disp, &ie);
                    journal_write_system(&g_journal, now,
                                         SYS_FOCUS_LOST, "window focus lost");
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    ie.type = IE_FOCUS_GAINED;
                    disp_handle(&disp, &ie);
                }
            } else if (ev.type == SDL_DISPLAYEVENT) {
                if (ev.display.event == SDL_DISPLAYEVENT_DISCONNECTED) {
                    InputEvent ie = {0};
                    ie.type = IE_DISPLAY_LOST; ie.origin = ORIGIN_HUMAN;
                    disp_handle(&disp, &ie);
                    journal_write_system(&g_journal, now,
                                         SYS_DISPLAY_LOST, "display unplugged");
                }
            }
        }

        bool need_reconcile = false;
        disp_tick(&disp, now, &need_reconcile);
        if (need_reconcile)
            disp_report_display_mode(
                &disp,
                (SDL_GetWindowFlags(app.window) &
                 SDL_WINDOW_FULLSCREEN_DESKTOP) != 0);

        if (url && dev && now - last_sync >= SYNC_INTERVAL_MS) {
            last_sync = now;
            uint32_t released = 0;
            uint64_t oldgen = act_generation(&disp.act);
            if (sync_poll(&disp, now, &released) &&
                act_generation(&disp.act) != oldgen) {
                journal_write_activation(&g_journal, now, oldgen,
                                         act_generation(&disp.act), released);
            }
        }

        renderer_draw_background(&scene, app.renderer, app.width, app.height);
        ui_render(app.renderer, &disp, &model, scene.background,
                  app.width, app.height, act_generation(&disp.act));
        SDL_RenderPresent(app.renderer);

        /* headless/测试环境无 VSYNC 时限速，实际桌面由 VSYNC 节流 */
        SDL_Delay(8);
    }

    journal_close(&g_journal);
    renderer_destroy(&scene);
    window_destroy(&app);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
