#include "renderer.h"

#include <stdio.h>

#include <SDL2/SDL_image.h>

#define GEAR_SIZE 36
#define MODAL_WIDTH 560
#define MODAL_HEIGHT 360

static void set_color(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
}

static void draw_rect(SDL_Renderer *renderer, const SDL_Rect *rect,
                      Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    set_color(renderer, r, g, b, a);
    SDL_RenderFillRect(renderer, rect);
}

static void draw_outline(SDL_Renderer *renderer, const SDL_Rect *rect,
                         Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    set_color(renderer, r, g, b, a);
    SDL_RenderDrawRect(renderer, rect);
}

bool renderer_load_background(
    SceneRenderer *scene,
    SDL_Renderer *renderer,
    const char *image_path
) {
    if (scene == NULL || renderer == NULL || image_path == NULL) {
        fprintf(stderr, "renderer_load_background: invalid arguments\n");
        return false;
    }

    scene->background = IMG_LoadTexture(renderer, image_path);
    if (scene->background == NULL) {
        fprintf(
            stderr,
            "IMG_LoadTexture failed for %s: %s\n",
            image_path,
            IMG_GetError()
        );
        return false;
    }
    scene->marker_size = 72;
    scene->marker_x = 0;
    scene->marker_y = 0;

    return true;
}

void renderer_set_marker(SceneRenderer *scene, int x, int y, int size) {
    if (scene == NULL) return;
    scene->marker_x = x;
    scene->marker_y = y;
    scene->marker_size = size > 0 ? size : 72;
}

bool renderer_is_gear_click(int x, int y, int window_width) {
    SDL_Rect gear = {
        .x = window_width - GEAR_SIZE - 12,
        .y = 12,
        .w = GEAR_SIZE,
        .h = GEAR_SIZE
    };
    return x >= gear.x && x < gear.x + gear.w && y >= gear.y && y < gear.y + gear.h;
}

static void modal_rect(int window_width, int window_height, SDL_Rect *rect) {
    rect->w = MODAL_WIDTH;
    rect->h = MODAL_HEIGHT;
    rect->x = (window_width - MODAL_WIDTH) / 2;
    rect->y = (window_height - MODAL_HEIGHT) / 2;
}

bool renderer_is_modal_hit(int x, int y, int window_width, int window_height) {
    SDL_Rect modal;
    modal_rect(window_width, window_height, &modal);
    return x >= modal.x && x < modal.x + modal.w && y >= modal.y && y < modal.y + modal.h;
}

void renderer_get_textbox_rect(int window_width, int window_height, SDL_Rect *rect) {
    SDL_Rect modal;
    modal_rect(window_width, window_height, &modal);
    rect->x = modal.x + 32;
    rect->y = modal.y + 92;
    rect->w = modal.w - 64;
    rect->h = 48;
}

static void draw_marker(SDL_Renderer *renderer, const SceneRenderer *scene) {
    SDL_Rect marker = {
        .x = scene->marker_x,
        .y = scene->marker_y,
        .w = scene->marker_size,
        .h = scene->marker_size
    };
    draw_rect(renderer, &marker, 32, 204, 224, 190);
    draw_outline(renderer, &marker, 220, 255, 255, 230);
}

static void draw_gear(SDL_Renderer *renderer, int window_width) {
    SDL_Rect gear = {
        .x = window_width - GEAR_SIZE - 12,
        .y = 12,
        .w = GEAR_SIZE,
        .h = GEAR_SIZE
    };
    draw_rect(renderer, &gear, 20, 28, 42, 210);
    draw_outline(renderer, &gear, 180, 220, 255, 230);
    set_color(renderer, 230, 245, 255, 240);
    SDL_RenderDrawLine(renderer, gear.x + 10, gear.y + 18, gear.x + 26, gear.y + 18);
    SDL_RenderDrawLine(renderer, gear.x + 10, gear.y + 26, gear.x + 26, gear.y + 26);
}

static void draw_panel(SDL_Renderer *renderer, int window_width, int window_height,
                       const RendererUiState *ui) {
    SDL_Rect modal;
    modal_rect(window_width, window_height, &modal);
    SDL_Rect backdrop = {0, 0, window_width, window_height};
    draw_rect(renderer, &backdrop, 0, 0, 0, 150);
    draw_rect(renderer, &modal, 24, 31, 44, 245);
    draw_outline(renderer, &modal, 120, 190, 230, 255);

    SDL_Rect title = { modal.x + 24, modal.y + 22, 250, 10 };
    draw_rect(renderer, &title, 230, 245, 255, 255);

    SDL_Rect close = { modal.x + modal.w - 36, modal.y + 16, 22, 22 };
    draw_rect(renderer, &close, 90, 32, 42, 230);
    set_color(renderer, 255, 255, 255, 255);
    SDL_RenderDrawLine(renderer, close.x + 5, close.y + 5, close.x + 17, close.y + 17);
    SDL_RenderDrawLine(renderer, close.x + 17, close.y + 5, close.x + 5, close.y + 17);

    SDL_Rect label = { modal.x + 32, modal.y + 70, 250, 8 };
    draw_rect(renderer, &label, 170, 205, 230, 230);

    SDL_Rect textbox;
    renderer_get_textbox_rect(window_width, window_height, &textbox);
    draw_rect(renderer, &textbox, 8, 13, 22, 255);
    draw_outline(renderer, &textbox,
                 ui->textbox_focused ? 90 : 60,
                 ui->textbox_focused ? 210 : 150,
                 255, 255);

    if (ui->composing && ui->composition_text != NULL && ui->composition_text[0] != '\0') {
        (void)ui;
    }

    SDL_Rect hint1 = { modal.x + 32, modal.y + 166, 420, 8 };
    SDL_Rect hint2 = { modal.x + 32, modal.y + 190, 380, 8 };
    SDL_Rect hint3 = { modal.x + 32, modal.y + 214, 460, 8 };
    draw_rect(renderer, &hint1, 130, 165, 190, 220);
    draw_rect(renderer, &hint2, 130, 165, 190, 220);
    draw_rect(renderer, &hint3, 130, 165, 190, 220);
}

void renderer_draw(
    const SceneRenderer *scene,
    SDL_Renderer *renderer,
    int window_width,
    int window_height,
    const RendererUiState *ui
) {
    SDL_Rect dst_rect = {
        .x = 0,
        .y = 0,
        .w = window_width,
        .h = window_height,
    };

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    if (scene != NULL && scene->background != NULL) {
        SDL_RenderCopy(renderer, scene->background, NULL, &dst_rect);
    }

    draw_marker(renderer, scene);
    draw_gear(renderer, window_width);
    if (ui != NULL && ui->settings_open) {
        draw_panel(renderer, window_width, window_height, ui);
    }

    SDL_RenderPresent(renderer);
}

void renderer_destroy(SceneRenderer *scene) {
    if (scene == NULL) {
        return;
    }

    if (scene->background != NULL) {
        SDL_DestroyTexture(scene->background);
        scene->background = NULL;
    }
}
