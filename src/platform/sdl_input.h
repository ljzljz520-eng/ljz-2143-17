/*
 * sdl_input.h - SDL2 事件适配
 * 使用 SDL_Scancode（物理位置）而非 SDL_Keycode（布局相关），
 * 保证 QWERTY/AZERTY、Windows/Linux/macOS 下策略匹配一致。
 */
#ifndef INPUT_STRATEGY_SDL_INPUT_H
#define INPUT_STRATEGY_SDL_INPUT_H

#include <stdbool.h>
#include <SDL2/SDL.h>

#include "../core/events.h"

/* scancode -> 归一化物理键 */
NK_Key sdl_to_nk(SDL_Scancode sc);

/* 跟踪修饰键的即时快照（部分平台不写 keysym.mod） */
uint32_t sdl_mod_state(void);

/* 把 SDL_KeyboardEvent 翻译成 InputEvent。
   in_composition: 当前 IME 是否处于组合中（由 TEXTINPUT/TEXTEDITING 维护）。 */
InputEvent sdl_key_event(const SDL_KeyboardEvent *k, EventOrigin origin,
                         bool in_composition, uint32_t platform_id);

#endif
