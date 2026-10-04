#include "sdl_input.h"

#include <string.h>

#include "../core/key.h"

static const struct { SDL_Scancode sc; NK_Key nk; } MAP[] = {
    {SDL_SCANCODE_ESCAPE, NK_ESC},
    {SDL_SCANCODE_F1, NK_F1}, {SDL_SCANCODE_F2, NK_F2},
    {SDL_SCANCODE_F3, NK_F3}, {SDL_SCANCODE_F4, NK_F4},
    {SDL_SCANCODE_F5, NK_F5}, {SDL_SCANCODE_F6, NK_F6},
    {SDL_SCANCODE_F7, NK_F7}, {SDL_SCANCODE_F8, NK_F8},
    {SDL_SCANCODE_F9, NK_F9}, {SDL_SCANCODE_F10, NK_F10},
    {SDL_SCANCODE_F11, NK_F11}, {SDL_SCANCODE_F12, NK_F12},
    {SDL_SCANCODE_GRAVE, NK_GRAVE},
    {SDL_SCANCODE_1, NK_1}, {SDL_SCANCODE_2, NK_2}, {SDL_SCANCODE_3, NK_3},
    {SDL_SCANCODE_4, NK_4}, {SDL_SCANCODE_5, NK_5}, {SDL_SCANCODE_6, NK_6},
    {SDL_SCANCODE_7, NK_7}, {SDL_SCANCODE_8, NK_8}, {SDL_SCANCODE_9, NK_9},
    {SDL_SCANCODE_0, NK_0},
    {SDL_SCANCODE_MINUS, NK_MINUS}, {SDL_SCANCODE_EQUALS, NK_EQUAL},
    {SDL_SCANCODE_BACKSPACE, NK_BACKSPACE},
    {SDL_SCANCODE_TAB, NK_TAB},
    {SDL_SCANCODE_Q, NK_Q}, {SDL_SCANCODE_W, NK_W}, {SDL_SCANCODE_E, NK_E},
    {SDL_SCANCODE_R, NK_R}, {SDL_SCANCODE_T, NK_T}, {SDL_SCANCODE_Y, NK_Y},
    {SDL_SCANCODE_U, NK_U}, {SDL_SCANCODE_I, NK_I}, {SDL_SCANCODE_O, NK_O},
    {SDL_SCANCODE_P, NK_P},
    {SDL_SCANCODE_LEFTBRACKET, NK_LBRACKET},
    {SDL_SCANCODE_RIGHTBRACKET, NK_RBRACKET},
    {SDL_SCANCODE_BACKSLASH, NK_BACKSLASH},
    {SDL_SCANCODE_A, NK_A}, {SDL_SCANCODE_S, NK_S}, {SDL_SCANCODE_D, NK_D},
    {SDL_SCANCODE_F, NK_F}, {SDL_SCANCODE_G, NK_G}, {SDL_SCANCODE_H, NK_H},
    {SDL_SCANCODE_J, NK_J}, {SDL_SCANCODE_K, NK_K}, {SDL_SCANCODE_L, NK_L},
    {SDL_SCANCODE_SEMICOLON, NK_SEMICOLON},
    {SDL_SCANCODE_APOSTROPHE, NK_APOSTROPHE},
    {SDL_SCANCODE_RETURN, NK_RETURN}, {SDL_SCANCODE_KP_ENTER, NK_RETURN},
    {SDL_SCANCODE_Z, NK_Z}, {SDL_SCANCODE_X, NK_X}, {SDL_SCANCODE_C, NK_C},
    {SDL_SCANCODE_V, NK_V}, {SDL_SCANCODE_B, NK_B}, {SDL_SCANCODE_N, NK_N},
    {SDL_SCANCODE_M, NK_M}, {SDL_SCANCODE_COMMA, NK_COMMA},
    {SDL_SCANCODE_PERIOD, NK_PERIOD}, {SDL_SCANCODE_SLASH, NK_SLASH},
    {SDL_SCANCODE_SPACE, NK_SPACE},
    {SDL_SCANCODE_LEFT, NK_LEFT}, {SDL_SCANCODE_RIGHT, NK_RIGHT},
    {SDL_SCANCODE_UP, NK_UP}, {SDL_SCANCODE_DOWN, NK_DOWN},
    {SDL_SCANCODE_LSHIFT, NK_LSHIFT}, {SDL_SCANCODE_RSHIFT, NK_RSHIFT},
    {SDL_SCANCODE_LCTRL, NK_LCTRL}, {SDL_SCANCODE_RCTRL, NK_RCTRL},
    {SDL_SCANCODE_LALT, NK_LALT}, {SDL_SCANCODE_RALT, NK_RALT},
    {SDL_SCANCODE_LGUI, NK_LGUI}, {SDL_SCANCODE_RGUI, NK_RGUI},
    {SDL_SCANCODE_HOME, NK_HOME}, {SDL_SCANCODE_END, NK_END},
    {SDL_SCANCODE_PAGEUP, NK_PAGEUP}, {SDL_SCANCODE_PAGEDOWN, NK_PAGEDOWN},
    {SDL_SCANCODE_INSERT, NK_INSERT}, {SDL_SCANCODE_DELETE, NK_DELETE},
    {SDL_SCANCODE_CAPSLOCK, NK_CAPSLOCK},
    {SDL_SCANCODE_PRINTSCREEN, NK_PRINTSCREEN},
    {SDL_SCANCODE_SCROLLLOCK, NK_SCROLLLOCK},
    {SDL_SCANCODE_PAUSE, NK_PAUSE},
};

NK_Key sdl_to_nk(SDL_Scancode sc) {
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; ++i)
        if (MAP[i].sc == sc) return MAP[i].nk;
    return NK_UNKNOWN;
}

uint32_t sdl_mod_state(void) {
    SDL_Keymod m = SDL_GetModState();
    uint32_t mods = 0;
    if (m & (KMOD_LSHIFT | KMOD_RSHIFT)) mods |= KM_SHIFT;
    if (m & (KMOD_LCTRL | KMOD_RCTRL)) mods |= KM_CTRL;
    if (m & (KMOD_LALT | KMOD_RALT)) mods |= KM_ALT;
    if (m & (KMOD_LGUI | KMOD_RGUI)) mods |= KM_GUI;
    return mods;
}

InputEvent sdl_key_event(const SDL_KeyboardEvent *k, EventOrigin origin,
                         bool in_composition, uint32_t platform_id) {
    InputEvent e;
    memset(&e, 0, sizeof e);
    e.type = IE_KEY;
    e.origin = origin;
    e.key = sdl_to_nk(k->keysym.scancode);
    e.mods = sdl_mod_state();
    e.down = (k->type == SDL_KEYDOWN);
    e.repeat = (k->repeat != 0);
    e.composition = in_composition;
    e.platform = platform_id;
    e.raw_code = (uint32_t)k->keysym.sym;
    return e;
}
