/* ui.h - 极简即时模式 UI（展示画面叠加层 + 设置模态面板） */
#ifndef UI_UI_H
#define UI_UI_H

#include <stdbool.h>
#include <SDL2/SDL.h>

#include "../core/dispatcher.h"

typedef struct {
    int margin_x, margin_y; /* 背景图偏移（方向键移动的演示状态） */
    bool restored;          /* 背景是否处于"恢复原位"状态 */
} PresentModel;

void ui_render(SDL_Renderer *r, const Dispatcher *d, const PresentModel *m,
               SDL_Texture *background, int win_w, int win_h,
               uint64_t generation);

/* 在 x,y 画一行 ASCII 文本 */
void ui_text(SDL_Renderer *r, int x, int y, const char *s,
             uint8_t rr, uint8_t gg, uint8_t bb);

#endif
