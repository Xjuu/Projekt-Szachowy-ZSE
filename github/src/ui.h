#ifndef UI_H
#define UI_H

#include <stdbool.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "clock.h"

typedef enum {
    UI_MODE_SETUP,
    UI_MODE_GAME,
    UI_MODE_HELP,
    UI_MODE_ARBITER
} UIMode;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *font_xlarge;
    TTF_Font *font_large;
    TTF_Font *font_medium;
    TTF_Font *font_small;
    UIMode mode;
    uint32_t setup_minutes;
    uint32_t setup_increment;
} AppResources;

bool ui_init(AppResources *app);
void ui_cleanup(AppResources *app);
void ui_process_events(ClockState *clock_state, AppResources *app, bool *app_running);
void ui_render_frame(SDL_Renderer *renderer, AppResources *app, const ClockState *clock_state);

#endif
