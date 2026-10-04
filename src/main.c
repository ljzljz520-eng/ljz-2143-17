#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "input/input_logger.h"
#include "input/input_policy.h"
#include "policy_client.h"
#include "renderer.h"
#include "window.h"

#define WINDOW_TITLE "Visual Window App"
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define BACKGROUND_IMAGE_PATH "assets/background.png"
#define POLICY_POLL_MS 2000
#define HTTP_TIMEOUT_MS 800
#define MARKER_SIZE 72

typedef struct {
    InputCore input;
    InputLogWriter log;
    SceneRenderer scene;
    AppWindow app;
    bool settings_open;
    bool textbox_focused;
    bool text_editing;
    char composition[INPUT_TEXT_CAP];
    const char *policy_url;
    const char *device_id;
    uint32_t last_policy_poll;
} AppState;

static bool init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG;
    int initted = IMG_Init(img_flags);
    if ((initted & img_flags) == 0) {
        fprintf(stderr, "IMG_Init failed: %s\n", IMG_GetError());
        SDL_Quit();
        return false;
    }

    return true;
}

static void shutdown_sdl(void) {
    IMG_Quit();
    SDL_Quit();
}

static void record_and_process(AppState *state, InputEvent *event);

static uint32_t now_ms(void) {
    return SDL_GetTicks();
}

static CanonicalKey map_scancode(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_ESCAPE: return KEY_ESCAPE;
        case SDL_SCANCODE_F11: return KEY_F11;
        case SDL_SCANCODE_LEFT: return KEY_LEFT;
        case SDL_SCANCODE_RIGHT: return KEY_RIGHT;
        case SDL_SCANCODE_UP: return KEY_UP;
        case SDL_SCANCODE_DOWN: return KEY_DOWN;
        case SDL_SCANCODE_R: return KEY_R;
        case SDL_SCANCODE_B: return KEY_B;
        case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
        case SDL_SCANCODE_Q: return KEY_Q;
        case SDL_SCANCODE_W: return KEY_W;
        case SDL_SCANCODE_A: return KEY_A;
        case SDL_SCANCODE_S: return KEY_S;
        case SDL_SCANCODE_D: return KEY_D;
        default: return KEY_NONE;
    }
}

static uint16_t sdl_modifiers(Uint16 mods) {
    uint16_t out = MOD_NONE;
    if ((mods & KMOD_LSHIFT) != 0) out |= MOD_LSHIFT;
    if ((mods & KMOD_RSHIFT) != 0) out |= MOD_RSHIFT;
    if ((mods & KMOD_LCTRL) != 0) out |= MOD_LCTRL;
    if ((mods & KMOD_RCTRL) != 0) out |= MOD_RCTRL;
    if ((mods & KMOD_LALT) != 0) out |= MOD_LALT;
    if ((mods & KMOD_RALT) != 0) out |= MOD_RALT;
    if ((mods & KMOD_LGUI) != 0) out |= MOD_LGUI;
    if ((mods & KMOD_RGUI) != 0) out |= MOD_RGUI;
    return out;
}

static void record_and_process(AppState *state, InputEvent *event) {
    input_log_event(&state->log, event);
    OutputList outputs;
    input_core_process(&state->input, event, &outputs);
    for (int i = 0; i < outputs.count; ++i) {
        OutputEvent *out = &outputs.items[i];
        switch (out->type) {
            case OUT_MOVE:
                renderer_set_marker(&state->scene, out->x, out->y, MARKER_SIZE);
                break;
            case OUT_RESTORE_BACKGROUND:
                renderer_set_marker(&state->scene, out->x, out->y, MARKER_SIZE);
                break;
            case OUT_CANCEL_MODAL:
                state->settings_open = false;
                state->textbox_focused = false;
                {
                    InputEvent layer;
                    input_event_init(&layer, EV_LAYER, now_ms());
                    layer.layer = LAYER_PRESENTATION;
                    record_and_process(state, &layer);
                }
                break;
            case OUT_FULLSCREEN_TOGGLE: {
                Uint32 flags = SDL_GetWindowFlags(state->app.window);
                bool currently_fullscreen = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                if (out->target_fullscreen != currently_fullscreen) {
                    if (SDL_SetWindowFullscreen(
                            state->app.window,
                            out->target_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
                        fprintf(stderr, "fullscreen request failed: %s\n", SDL_GetError());
                    }
                }
                break;
            }
            case OUT_SAFE_QUIT:
                SDL_Event quit;
                memset(&quit, 0, sizeof(quit));
                quit.type = SDL_QUIT;
                SDL_PushEvent(&quit);
                break;
            case OUT_POLICY_STATUS:
                fprintf(stdout, "policy gen=%u status=%d reason=%s\n",
                        out->generation, (int)out->status, out->reason);
                if (state->policy_url != NULL && out->status != POLICY_STATUS_STAGED) {
                    char receipt[512];
                    const char *delivery = out->status == POLICY_STATUS_ACTIVATED ? "applied" : "rejected";
                    snprintf(receipt, sizeof(receipt),
                             "{\"device_id\":\"%s\",\"generation\":%u,\"status\":\"%s\",\"reason\":\"%s\"}",
                             state->device_id, out->generation, delivery, out->reason);
                    HttpResponse response;
                    char url[2304];
                    snprintf(url, sizeof(url), "%s/api/devices/%s/receipts",
                             state->policy_url, state->device_id);
                    http_post_json(url, HTTP_TIMEOUT_MS, receipt, &response);
                }
                break;
            case OUT_IGNORE:
                break;
        }
    }
}

static void set_layer(AppState *state, InputLayer layer) {
    InputEvent event;
    input_event_init(&event, EV_LAYER, now_ms());
    event.layer = layer;
    record_and_process(state, &event);
}

static void set_focus(AppState *state, bool focused) {
    InputEvent event;
    input_event_init(&event, EV_FOCUS, now_ms());
    event.focused = focused;
    record_and_process(state, &event);
    if (!focused && SDL_IsTextInputActive()) {
        SDL_StopTextInput();
    }
}

static void set_composition(AppState *state, bool composing, const char *text) {
    InputEvent event;
    input_event_init(&event, EV_COMPOSITION, now_ms());
    event.composing = composing;
    if (text != NULL) {
        snprintf(event.text, sizeof(event.text), "%s", text);
    }
    record_and_process(state, &event);
}

static void poll_remote_policy(AppState *state) {
    if (state->policy_url == NULL) return;
    if (now_ms() - state->last_policy_poll < POLICY_POLL_MS) return;
    state->last_policy_poll = now_ms();

    char url[2304];
    snprintf(url, sizeof(url), "%s/api/devices/%s/policy", state->policy_url, state->device_id);
    HttpResponse response;
    if (http_get(url, HTTP_TIMEOUT_MS, &response) != 200) {
        return;
    }

    PolicyPackage package;
    InputPolicy candidate;
    char reason[INPUT_REASON_CAP];
    PolicyParseResult parse = policy_package_parse(
        response.body, response.length, &package, &candidate, reason, sizeof(reason));

    InputEvent event;
    if (parse != POLICY_PARSE_OK) {
        input_event_init(&event, EV_POLICY_REJECTED, now_ms());
        event.generation = package.generation;
        snprintf(event.text, sizeof(event.text), "%s", policy_parse_result_name(parse));
        record_and_process(state, &event);
        policy_package_free(&package);
        return;
    }

    bool newer = package.generation > state->input.applied_generation &&
        (!state->input.has_staged || package.generation > state->input.staged.generation);
    if (newer) {
        OutputList outputs;
        input_core_stage_policy(&state->input, &candidate, package.generation,
                                package.checksum, &outputs);
        for (int i = 0; i < outputs.count; ++i) {
            OutputEvent out = outputs.items[i];
            char receipt[512];
            const char *status = out.status == POLICY_STATUS_ACTIVATED ? "applied" : "staged";
            snprintf(receipt, sizeof(receipt),
                     "{\"device_id\":\"%s\",\"generation\":%u,\"status\":\"%s\",\"reason\":\"%s\"}",
                     state->device_id, out.generation, status, out.reason);
            char receipt_url[2304];
            snprintf(receipt_url, sizeof(receipt_url), "%s/api/devices/%s/receipts",
                     state->policy_url, state->device_id);
            http_post_json(receipt_url, HTTP_TIMEOUT_MS, receipt, &response);
        }
    }
    policy_package_free(&package);
}

static void handle_key(AppState *state, const SDL_KeyboardEvent *source) {
    CanonicalKey key = map_scancode(source->keysym.scancode);
    if (key == KEY_NONE) return;

    InputEvent event;
    input_event_init(&event, EV_KEY, source->timestamp);
    event.key = key;
    event.phase = source->repeat ? PHASE_REPEAT : (source->state == SDL_PRESSED ? PHASE_DOWN : PHASE_UP);
    event.modifiers = sdl_modifiers(source->keysym.mod);
    event.scancode = (uint32_t)source->keysym.scancode;
    event.focused = true;
    event.composing = state->text_editing;
    event.layer = state->settings_open
        ? (state->textbox_focused ? LAYER_TEXTBOX : LAYER_MODAL)
        : LAYER_PRESENTATION;
    record_and_process(state, &event);
}

static void handle_mouse_button(AppState *state, const SDL_MouseButtonEvent *click) {
    if (click->button != SDL_BUTTON_LEFT || click->state != SDL_PRESSED) return;

    if (state->settings_open) {
        if (!renderer_is_modal_hit(click->x, click->y, state->app.width, state->app.height)) {
            state->settings_open = false;
            state->textbox_focused = false;
            state->text_editing = false;
            state->composition[0] = '\0';
            if (SDL_IsTextInputActive()) SDL_StopTextInput();
            set_composition(state, false, "");
            set_layer(state, LAYER_PRESENTATION);
        } else {
            SDL_Rect textbox;
            renderer_get_textbox_rect(state->app.width, state->app.height, &textbox);
            bool focused = click->x >= textbox.x && click->x < textbox.x + textbox.w &&
                           click->y >= textbox.y && click->y < textbox.y + textbox.h;
            if (state->textbox_focused != focused) {
                state->textbox_focused = focused;
                set_layer(state, focused ? LAYER_TEXTBOX : LAYER_MODAL);
                if (focused && !SDL_IsTextInputActive()) SDL_StartTextInput();
                if (!focused && SDL_IsTextInputActive()) SDL_StopTextInput();
            }
        }
        return;
    }

    if (renderer_is_gear_click(click->x, click->y, state->app.width)) {
        state->settings_open = true;
        state->textbox_focused = false;
        set_layer(state, LAYER_MODAL);
    }
}

static void handle_window(AppState *state, const SDL_WindowEvent *window, bool *running) {
    if (window->event == SDL_WINDOWEVENT_CLOSE) {
        *running = false;
        return;
    }

    if (window->event == SDL_WINDOWEVENT_FOCUS_LOST) {
        state->text_editing = false;
        set_focus(state, false);
    } else if (window->event == SDL_WINDOWEVENT_FOCUS_GAINED) {
        set_focus(state, true);
    } else if (window->event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        int width = state->app.width;
        int height = state->app.height;
        SDL_GetWindowSize(state->app.window, &width, &height);
        state->app.width = width;
        state->app.height = height;
        input_core_resize(&state->input, width, height);
    }
}

static void reconcile_fullscreen(AppState *state) {
    Uint32 flags = SDL_GetWindowFlags(state->app.window);
    bool actual = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (actual != state->input.fullscreen) {
        InputEvent event;
        input_event_init(&event, EV_FULLSCREEN_STATE, now_ms());
        event.fullscreen = actual;
        record_and_process(state, &event);
    }
}

static void check_display_present(AppState *state) {
    int displays = SDL_GetNumVideoDisplays();
    bool present = displays > 0;
    if (present != state->input.display_present) {
        InputEvent event;
        input_event_init(&event, EV_DISPLAY, now_ms());
        event.display_present = present;
        record_and_process(state, &event);
        if (!present) {
            SDL_SetWindowFullscreen(state->app.window, 0);
            SDL_SetWindowPosition(state->app.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    }
}

int main(int argc, char **argv) {
    AppState state;
    memset(&state, 0, sizeof(state));
    state.policy_url = getenv("POLICY_URL");
    state.device_id = getenv("DEVICE_ID");
    if (state.device_id == NULL || state.device_id[0] == '\0') {
        state.device_id = "local-sdl-device";
    }
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--policy-url") == 0 && i + 1 < argc) {
            state.policy_url = argv[++i];
        }
    }

    if (!init_sdl()) {
        return 1;
    }

    if (!window_init(&state.app, WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT)) {
        shutdown_sdl();
        return 1;
    }

    if (!renderer_load_background(&state.scene, state.app.renderer, BACKGROUND_IMAGE_PATH)) {
        window_destroy(&state.app);
        shutdown_sdl();
        return 1;
    }

    input_core_init(&state.input, WINDOW_WIDTH, WINDOW_HEIGHT, TRUST_OPERATIONAL);
    renderer_set_marker(
        &state.scene,
        state.input.marker_x,
        state.input.marker_y,
        MARKER_SIZE
    );

    char session[64];
    snprintf(session, sizeof(session), "run-%lu", (unsigned long)time(NULL));
    if (!input_log_open(&state.log, "logs/input.jsonl", session, TRUST_OPERATIONAL) ||
        !input_log_manifest(&state.log, state.device_id, "1.0")) {
        fprintf(stderr, "warning: input replay log is unavailable\n");
    }

    bool running = true;
    uint32_t last_frame = now_ms();
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event) == 1) {
            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;
                case SDL_WINDOWEVENT:
                    handle_window(&state, &event.window, &running);
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    handle_key(&state, &event.key);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    handle_mouse_button(&state, &event.button);
                    break;
                case SDL_TEXTEDITING:
                    state.text_editing = event.edit.text[0] != '\0';
                    snprintf(state.composition, sizeof(state.composition), "%s",
                             event.edit.text);
                    set_composition(&state, state.text_editing, state.composition);
                    break;
                case SDL_TEXTINPUT:
                    state.text_editing = false;
                    state.composition[0] = '\0';
                    set_composition(&state, false, "");
                    break;
                default:
                    break;
            }
        }

        uint32_t current = now_ms();
        if ((int32_t)(current - last_frame) >= 8) {
            InputEvent tick;
            input_event_init(&tick, EV_TICK, current);
            if (state.input.repeating_key != KEY_NONE) {
                input_log_event(&state.log, &tick);
            }
            OutputList outputs;
            input_core_process(&state.input, &tick, &outputs);
            for (int i = 0; i < outputs.count; ++i) {
                if (outputs.items[i].type == OUT_MOVE) {
                    renderer_set_marker(&state.scene, outputs.items[i].x,
                                        outputs.items[i].y, MARKER_SIZE);
                }
            }
            last_frame = current;
        }

        reconcile_fullscreen(&state);
        check_display_present(&state);
        poll_remote_policy(&state);

        RendererUiState ui = {
            .settings_open = state.settings_open,
            .textbox_focused = state.textbox_focused,
            .composing = state.text_editing,
            .composition_text = state.composition
        };
        renderer_draw(&state.scene, state.app.renderer, state.app.width, state.app.height, &ui);
        SDL_Delay(4);
    }

    input_log_close(&state.log);
    renderer_destroy(&state.scene);
    window_destroy(&state.app);
    shutdown_sdl();
    return 0;
}
