#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "font.h"

static void pixel(SDL_Renderer *r, int x, int y, int s,
                  uint8_t rr, uint8_t gg, uint8_t bb) {
    SDL_Rect px = {x, y, s, s};
    SDL_SetRenderDrawColor(r, rr, gg, bb, 255);
    SDL_RenderFillRect(r, &px);
}

void ui_text(SDL_Renderer *r, int x, int y, const char *s,
             uint8_t rr, uint8_t gg, uint8_t bb) {
    int cx = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '\n') { cx = x; y += FONT_H + 2; continue; }
        const unsigned char *g = font_glyph((char)*p);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (g[row] & (1 << (4 - col)))
                    pixel(r, cx + col, y + row, 1, rr, gg, bb);
            }
        }
        cx += 6;
        if (cx > 4000) break;
    }
}

static void rect_filled(SDL_Renderer *r, SDL_Rect rc,
                        uint8_t rr, uint8_t gg, uint8_t bb, uint8_t a) {
    SDL_SetRenderDrawColor(r, rr, gg, bb, a);
    SDL_RenderFillRect(r, &rc);
}

static void rect_outline(SDL_Renderer *r, SDL_Rect rc,
                         uint8_t rr, uint8_t gg, uint8_t bb) {
    SDL_SetRenderDrawColor(r, rr, gg, bb, 255);
    SDL_RenderDrawRect(r, &rc);
}

void ui_render(SDL_Renderer *r, const Dispatcher *d, const PresentModel *m,
               SDL_Texture *background, int win_w, int win_h,
               uint64_t generation) {
    /* 背景 */
    SDL_SetRenderDrawColor(r, 12, 18, 28, 255);
    SDL_RenderClear(r);
    if (background) {
        SDL_Rect dst = {m->margin_x, m->margin_y, win_w, win_h};
        SDL_RenderCopy(r, background, NULL, &dst);
    }

    /* 顶部状态栏 */
    rect_filled(r, (SDL_Rect){0, 0, win_w, 18}, 10, 18, 30, 220);
    char bar[256];
    snprintf(bar, sizeof bar,
        "GEN %llu  layer:%s  fs:%s  Esc=settings F11=fullscreen "
        "Ctrl+R=restore  (Ctrl+Q quit always works)",
        (unsigned long long)generation,
        d->panel.open ? "MODAL" : "presentation",
        d->fs.state == FS_FULLSCREEN ? "FULL" :
        d->fs.state == FS_SWITCHING ? "switching" :
        d->fs.state == FS_RECONCILING ? "reconcile" : "window");
    ui_text(r, 6, 5, bar, 200, 220, 240);

    if (m->restored)
        ui_text(r, 6, 24, "background restored to origin", 120, 220, 140);

    if (!d->panel.open) {
        ui_text(r, 6, win_h - 14,
                "Arrows: bounded repeat move   Enter: on-site confirm",
                150, 170, 190);
        return;
    }

    /* 模态遮罩 + 设置面板 */
    rect_filled(r, (SDL_Rect){0, 0, win_w, win_h}, 0, 0, 0, 150);
    int pw = win_w - 160, ph = win_h - 120;
    if (pw > 720) pw = 720;
    if (ph > 460) ph = 460;
    SDL_Rect panel = {(win_w - pw) / 2, (win_h - ph) / 2, pw, ph};
    rect_filled(r, panel, 28, 38, 54, 250);
    rect_outline(r, panel, 120, 160, 210);

    int x = panel.x + 18, y = panel.y + 14;
    ui_text(r, x, y, "SETTINGS PANEL (modal layer)", 230, 240, 255);
    y += 16;
    ui_text(r, x, y, "Esc closes this panel. Hotkeys do NOT pass through.",
            170, 190, 215);
    y += 14;
    ui_text(r, x, y, "Field focus: IME composition active = Chinese input.",
            170, 190, 215);
    y += 14;

    /* 映射只读列表（编辑在网页配置台完成） */
    const Policy *p = act_current(&d->act);
    if (p) {
        for (uint32_t i = 0; i < p->binding_count; ++i) {
            char line[96];
            snprintf(line, sizeof line, "%-20s -> %s",
                     p->bindings[i].chord_str, p->bindings[i].command_str);
            ui_text(r, x, y, line, 210, 222, 235);
            y += 11;
            if (y > panel.y + ph - 110) break;
        }
    }
    y += 6;
    ui_text(r, x, y, "repeat(ms):", 150, 170, 200);
    char rp[128];
    snprintf(rp, sizeof rp, "initial=%u interval=%u max_hold=%u bounds=%dx%d",
             p ? p->move_initial_delay_ms : 0,
             p ? p->move_repeat_ms : 0,
             p ? p->move_max_hold_ms : 0,
             p ? p->bounds_w : 0, p ? p->bounds_h : 0);
    ui_text(r, x + 80, y, rp, 200, 215, 230);
    y += 18;

    /* 文本输入框（IME 测试框） */
    ui_text(r, x, y, "IME test field:", 150, 170, 200);
    SDL_Rect field = {x, y + 12, pw - 36, 26};
    rect_filled(r, field, 12, 18, 28, 255);
    rect_outline(r, field, 120, 160, 210);
    if (d->panel.composing) {
        char pre[160];
        snprintf(pre, sizeof pre, "[%s]", d->panel.composition);
        ui_text(r, field.x + 6, field.y + 9, pre, 255, 210, 120);
    }
    ui_text(r, field.x + 6, field.y + 9, d->panel.text, 230, 240, 255);
    if (d->panel.composing) {
        int w = (int)strlen("[") * 6 +
                (int)strlen(d->panel.composition) * 6;
        rect_filled(r, (SDL_Rect){field.x + 6 + w, field.y + 6,
                                  1, FONT_H + 4},
                    255, 220, 120, 255);
    } else {
        int w = (int)strlen(d->panel.text) * 6;
        rect_filled(r, (SDL_Rect){field.x + 6 + w, field.y + 6,
                                  1, FONT_H + 4},
                    220, 230, 245, 255);
    }
}
