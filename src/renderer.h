#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <SDL2/SDL.h>

typedef struct {
    SDL_Texture *background;
    int marker_x;
    int marker_y;
    int marker_size;
} SceneRenderer;

typedef struct {
    bool settings_open;
    bool textbox_focused;
    bool composing;
    const char *composition_text;
} RendererUiState;

bool renderer_load_background(
    SceneRenderer *scene,
    SDL_Renderer *renderer,
    const char *image_path
);

void renderer_set_marker(SceneRenderer *scene, int x, int y, int size);
bool renderer_is_gear_click(int x, int y, int window_width);
bool renderer_is_modal_hit(int x, int y, int window_width, int window_height);
void renderer_get_textbox_rect(int window_width, int window_height, SDL_Rect *rect);

void renderer_draw(
    const SceneRenderer *scene,
    SDL_Renderer *renderer,
    int window_width,
    int window_height,
    const RendererUiState *ui
);

void renderer_destroy(SceneRenderer *scene);

#endif
