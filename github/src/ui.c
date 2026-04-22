#include "ui.h"

#include <stdio.h>
#include <math.h>

#include "app.h"

typedef struct {
    SDL_Rect rect;
    const char *label;
    bool hover;
} Button;

// Schemat kolorow
static const SDL_Color COLOR_BG = {20, 20, 25, 255};
static const SDL_Color COLOR_FG = {245, 245, 245, 255};
static const SDL_Color COLOR_ACCENT = {41, 175, 117, 255};
static const SDL_Color COLOR_ACTIVE = {100, 200, 150, 255};
static const SDL_Color COLOR_INACTIVE = {60, 60, 70, 255};
static const SDL_Color COLOR_BUTTON_HOVER = {80, 180, 130, 255};
static const SDL_Color COLOR_BUTTON_NORMAL = {41, 175, 117, 255};
static const SDL_Color COLOR_BUTTON_ERROR = {200, 70, 70, 255};
static const SDL_Color COLOR_BUTTON_ERROR_HOVER = {230, 100, 100, 255};
static const SDL_Color COLOR_BUTTON_BONUS = {200, 180, 70, 255};
static const SDL_Color COLOR_BUTTON_BONUS_HOVER = {230, 210, 100, 255};
static const SDL_Color COLOR_BUTTON_STOP = {150, 150, 150, 255};
static const SDL_Color COLOR_BUTTON_STOP_HOVER = {180, 180, 180, 255};

static SDL_Texture *render_text_texture(SDL_Renderer *renderer, TTF_Font *font, 
                                       const char *text, SDL_Color color, int *w, int *h) {
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (surface == NULL) {
        return NULL;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture != NULL) {
        *w = surface->w;
        *h = surface->h;
    }

    SDL_FreeSurface(surface);
    return texture;
}

static void draw_filled_rounded_rect(SDL_Renderer *renderer, SDL_Rect rect, 
                                    int radius, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    
    // Rysuj wypelniony prostokat
    SDL_Rect fill_rect = {rect.x + radius, rect.y, rect.w - 2 * radius, rect.h};
    SDL_RenderFillRect(renderer, &fill_rect);
    
    fill_rect = (SDL_Rect){rect.x, rect.y + radius, rect.w, rect.h - 2 * radius};
    SDL_RenderFillRect(renderer, &fill_rect);
    
    // Rysuj rogi (uproszczone - okregi w naroznikach)
    for (int i = -radius; i <= radius; i++) {
        for (int j = -radius; j <= radius; j++) {
            if (i * i + j * j <= radius * radius) {
                SDL_RenderDrawPoint(renderer, rect.x + radius + i, rect.y + radius + j);
                SDL_RenderDrawPoint(renderer, rect.x + (rect.w - radius) + i, rect.y + radius + j);
                SDL_RenderDrawPoint(renderer, rect.x + radius + i, rect.y + (rect.h - radius) + j);
                SDL_RenderDrawPoint(renderer, rect.x + (rect.w - radius) + i, rect.y + (rect.h - radius) + j);
            }
        }
    }
}

static void draw_button(SDL_Renderer *renderer, TTF_Font *font, Button *btn, bool is_hover) {
    SDL_Color bg_color = is_hover ? COLOR_BUTTON_HOVER : COLOR_BUTTON_NORMAL;
    draw_filled_rounded_rect(renderer, btn->rect, 8, bg_color);
    
    int text_w = 0, text_h = 0;
    SDL_Texture *txt = render_text_texture(renderer, font, btn->label, COLOR_FG, &text_w, &text_h);
    if (txt != NULL) {
        SDL_Rect dst = {
            .x = btn->rect.x + (btn->rect.w - text_w) / 2,
            .y = btn->rect.y + (btn->rect.h - text_h) / 2,
            .w = text_w,
            .h = text_h
        };
        SDL_RenderCopy(renderer, txt, NULL, &dst);
        SDL_DestroyTexture(txt);
    }
}

static void draw_colored_button(SDL_Renderer *renderer, TTF_Font *font, Button *btn, 
                                SDL_Color normal_color, SDL_Color hover_color, bool is_hover) {
    SDL_Color bg_color = is_hover ? hover_color : normal_color;
    draw_filled_rounded_rect(renderer, btn->rect, 8, bg_color);
    
    int text_w = 0, text_h = 0;
    SDL_Texture *txt = render_text_texture(renderer, font, btn->label, COLOR_FG, &text_w, &text_h);
    if (txt != NULL) {
        SDL_Rect dst = {
            .x = btn->rect.x + (btn->rect.w - text_w) / 2,
            .y = btn->rect.y + (btn->rect.h - text_h) / 2,
            .w = text_w,
            .h = text_h
        };
        SDL_RenderCopy(renderer, txt, NULL, &dst);
        SDL_DestroyTexture(txt);
    }
}

static void draw_close_button(SDL_Renderer *renderer, TTF_Font *font) {
    // Przycisk zamkniecia - prawy gorny rog (czerwony) - renderowany bez struktury Button
    SDL_Rect close_rect = {WINDOW_WIDTH - 55, 10, 45, 45};
    
    // Rysuj tlo przycisku
    draw_filled_rounded_rect(renderer, close_rect, 8, COLOR_BUTTON_ERROR);
    
    // Rysuj tekst X
    int text_w = 0, text_h = 0;
    SDL_Texture *txt = render_text_texture(renderer, font, "X", COLOR_FG, &text_w, &text_h);
    if (txt != NULL) {
        SDL_Rect dst = {
            .x = close_rect.x + (close_rect.w - text_w) / 2,
            .y = close_rect.y + (close_rect.h - text_h) / 2,
            .w = text_w,
            .h = text_h
        };
        SDL_RenderCopy(renderer, txt, NULL, &dst);
        SDL_DestroyTexture(txt);
    }
}

static void draw_setup_screen(SDL_Renderer *renderer, AppResources *app) {
    SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(renderer);
    
    int text_w = 0, text_h = 0;
    
    // Przycisk zamkniecia - prawy gorny rog
    draw_close_button(renderer, app->font_small);
    
    // Tytul - lewa strona
    SDL_Texture *title = render_text_texture(renderer, app->font_medium, "ZEGAR SZACHOWY", COLOR_ACCENT, &text_w, &text_h);
    SDL_Rect title_rect = {10, 10, text_w, text_h};
    SDL_RenderCopy(renderer, title, NULL, &title_rect);
    SDL_DestroyTexture(title);
    
    // Sekcja czasu - srodek
    char min_str[24];
    snprintf(min_str, sizeof(min_str), "Czas: %um", app->setup_minutes);
    SDL_Texture *min_txt = render_text_texture(renderer, app->font_small, min_str, COLOR_FG, &text_w, &text_h);
    SDL_Rect min_label = {340, 30, text_w, text_h};
    SDL_RenderCopy(renderer, min_txt, NULL, &min_label);
    SDL_DestroyTexture(min_txt);
    
    // Przyciski zmiany czasu
    Button minus_time = {.rect = {330, 60, 40, 35}, .label = "−"};
    Button plus_time = {.rect = {440, 60, 40, 35}, .label = "+"};
    
    draw_button(renderer, app->font_small, &minus_time, false);
    draw_button(renderer, app->font_small, &plus_time, false);
    
    // Sekcja inkrementu - prawa strona
    char inc_str[24];
    snprintf(inc_str, sizeof(inc_str), "Inc: %us", app->setup_increment);
    SDL_Texture *inc_txt = render_text_texture(renderer, app->font_small, inc_str, COLOR_FG, &text_w, &text_h);
    SDL_Rect inc_label = {810, 30, text_w, text_h};
    SDL_RenderCopy(renderer, inc_txt, NULL, &inc_label);
    SDL_DestroyTexture(inc_txt);
    
    // Przyciski zmiany inkrementu
    Button minus_inc = {.rect = {810, 60, 40, 35}, .label = "−"};
    Button plus_inc = {.rect = {920, 60, 40, 35}, .label = "+"};
    
    draw_button(renderer, app->font_small, &minus_inc, false);
    draw_button(renderer, app->font_small, &plus_inc, false);
    
    // Przycisk start - na srodku
    Button start_btn = {.rect = {340, 130, 300, 70}, .label = "ROZP. GRE"};
    draw_button(renderer, app->font_medium, &start_btn, false);
    
    // Tekst pomocy
    SDL_Texture *help = render_text_texture(renderer, app->font_small, 
                                           "Kliknij +/- aby ustawic, potem START", 
                                           (SDL_Color){150, 150, 150, 255}, &text_w, &text_h);
    if (help != NULL) {
        SDL_Rect dst = {(WINDOW_WIDTH - text_w) / 2, WINDOW_HEIGHT - 45, text_w, text_h};
        SDL_RenderCopy(renderer, help, NULL, &dst);
        SDL_DestroyTexture(help);
    }
    
    SDL_RenderPresent(renderer);
}

static void draw_time_display(SDL_Renderer *renderer, TTF_Font *time_font, TTF_Font *label_font,
                             const char *label, uint32_t ms, SDL_Rect rect, 
                             bool is_active, bool is_winner) {
    SDL_Color bg_color;
    
    if (is_winner) {
        bg_color = COLOR_ACCENT;
    } else if (is_active) {
        bg_color = COLOR_ACTIVE;
    } else {
        bg_color = COLOR_INACTIVE;
    }
    
    draw_filled_rounded_rect(renderer, rect, 12, bg_color);
    
    char time_buf[16];
    format_time(ms, time_buf, sizeof(time_buf), TIME_FORMAT_MM_SS_MS);
    
    int text_w = 0, text_h = 0;
    SDL_Texture *txt = render_text_texture(renderer, time_font, time_buf, COLOR_FG, &text_w, &text_h);
    if (txt != NULL) {
        SDL_Rect dst = {
            .x = rect.x + (rect.w - text_w) / 2,
            .y = rect.y + 40,
            .w = text_w,
            .h = text_h
        };
        SDL_RenderCopy(renderer, txt, NULL, &dst);
        SDL_DestroyTexture(txt);
    }
    
    // Rysuj etykiete - wyzej
    SDL_Texture *lbl = render_text_texture(renderer, label_font, label, COLOR_FG, &text_w, &text_h);
    if (lbl != NULL) {
        SDL_Rect lbl_dst = {
            .x = rect.x + (rect.w - text_w) / 2,
            .y = rect.y + 10,
            .w = text_w,
            .h = text_h
        };
        SDL_RenderCopy(renderer, lbl, NULL, &lbl_dst);
        SDL_DestroyTexture(lbl);
    }
}

static void draw_status_bar(SDL_Renderer *renderer, TTF_Font *font, const ClockState *clock_state) {
    const char *status_text = "";
    SDL_Color status_color = COLOR_FG;
    
    if (clock_state->state == STATE_STOPPED_BY_ARBITER) {
        status_text = "ZATRZYMANO PRZEZ ARBITR";
        status_color = (SDL_Color){255, 165, 0, 255};
    } else if (clock_state->state == STATE_RUNNING) {
        status_text = "GRA TRWA";
        status_color = COLOR_ACCENT;
    } else if (clock_state->state == STATE_PAUSED) {
        status_text = "PAUZA";
        status_color = (SDL_Color){200, 150, 80, 255};
    } else if (clock_state->state == STATE_FINISHED_LEFT_WIN) {
        status_text = "LEWY GRACZ WYGRAL!";
        status_color = COLOR_ACCENT;
    } else if (clock_state->state == STATE_FINISHED_RIGHT_WIN) {
        status_text = "PRAWY GRACZ WYGRAL!";
        status_color = COLOR_ACCENT;
    }
    
    int text_w = 0, text_h = 0;
    SDL_Texture *txt = render_text_texture(renderer, font, status_text, status_color, &text_w, &text_h);
    if (txt != NULL) {
        SDL_Rect dst = {
            .x = (WINDOW_WIDTH - text_w) / 2,
            .y = 30,
            .w = text_w,
            .h = text_h
        };
        SDL_RenderCopy(renderer, txt, NULL, &dst);
        SDL_DestroyTexture(txt);
    }
    
    // Pokaz liczbe ruchow i liczbe bledow
    char info_str[64];
    snprintf(info_str, sizeof(info_str), "Ruch: %u | B:%u | C:%u", 
             clock_state->move_count, 
             clock_state->left.error_count,
             clock_state->right.error_count);
    SDL_Texture *info_txt = render_text_texture(renderer, font, info_str, COLOR_FG, &text_w, &text_h);
    if (info_txt != NULL) {
        SDL_Rect dst = {
            .x = WINDOW_WIDTH - 250,
            .y = 35,
            .w = text_w,
            .h = text_h
        };
        SDL_RenderCopy(renderer, info_txt, NULL, &dst);
        SDL_DestroyTexture(info_txt);
    }
}

static void draw_game_screen(SDL_Renderer *renderer, AppResources *app, const ClockState *clock_state) {
    SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(renderer);
    
    // Rysuj pasek stanu
    draw_status_bar(renderer, app->font_small, clock_state);
    
    // Rysuj zegary obok siebie (uklad kompaktowy)
    SDL_Rect left_rect = {20, 80, 600, 220};
    SDL_Rect right_rect = {660, 80, 600, 220};
    
    bool left_winner = clock_state->state == STATE_FINISHED_LEFT_WIN;
    bool right_winner = clock_state->state == STATE_FINISHED_RIGHT_WIN;
    bool left_active = clock_state->active == ACTIVE_LEFT && clock_state->state == STATE_RUNNING;
    bool right_active = clock_state->active == ACTIVE_RIGHT && clock_state->state == STATE_RUNNING;
    
    draw_time_display(renderer, app->font_xlarge, app->font_small, "BIALY", 
                     clock_state->left.remaining_ms, left_rect, left_active, left_winner);
    draw_time_display(renderer, app->font_xlarge, app->font_small, "CZARNY", 
                     clock_state->right.remaining_ms, right_rect, right_active, right_winner);
    
    // Rysuj przyciski sterowania gra na dole
    // Przycisk pauza/wznow - po lewej
    Button pause_btn = {.rect = {50, 325, 120, 60}, .label = 
        (clock_state->state == STATE_PAUSED) ? "START" : "PAUZA"};
    draw_button(renderer, app->font_medium, &pause_btn, false);
    
    // Przycisk reset
    Button reset_btn = {.rect = {200, 325, 100, 60}, .label = "RESET"};
    draw_button(renderer, app->font_medium, &reset_btn, false);
    
    // Przycisk menu arbitra
    Button arbiter_btn = {.rect = {350, 325, 150, 60}, .label = "ARBITR"};
    draw_button(renderer, app->font_medium, &arbiter_btn, false);
    
    // Przycisk zamkniecia - prawy gorny rog
    draw_close_button(renderer, app->font_small);
    
    // Przycisk pomocy w prawym dolnym rogu
    SDL_SetRenderDrawColor(renderer, COLOR_ACCENT.r, COLOR_ACCENT.g, COLOR_ACCENT.b, COLOR_ACCENT.a);
    SDL_Rect help_box = {1230, 365, 50, 35};
    SDL_RenderFillRect(renderer, &help_box);
    
    int text_w = 0, text_h = 0;
    SDL_Texture *help_txt = render_text_texture(renderer, app->font_small, "?", COLOR_BG, &text_w, &text_h);
    if (help_txt != NULL) {
        SDL_Rect dst = {
            .x = 1230 + (50 - text_w) / 2,
            .y = 365 + (35 - text_h) / 2,
            .w = text_w,
            .h = text_h
        };
        SDL_RenderCopy(renderer, help_txt, NULL, &dst);
        SDL_DestroyTexture(help_txt);
    }
    
    SDL_RenderPresent(renderer);
}

static void draw_help_screen(SDL_Renderer *renderer, AppResources *app) {
    SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(renderer);
    
    int text_w = 0, text_h = 0;
    
    // Przycisk zamkniecia - prawy gorny rog
    draw_close_button(renderer, app->font_small);
    
    // Tytul
    SDL_Texture *title = render_text_texture(renderer, app->font_medium, "POMOC", COLOR_ACCENT, &text_w, &text_h);
    SDL_Rect title_rect = {20, 10, text_w, text_h};
    SDL_RenderCopy(renderer, title, NULL, &title_rect);
    SDL_DestroyTexture(title);
    
    // Lewa kolumna - gracze
    SDL_Texture *h1 = render_text_texture(renderer, app->font_small, "Gracze:", COLOR_ACCENT, &text_w, &text_h);
    SDL_Rect r1 = {20, 60, text_w, text_h};
    SDL_RenderCopy(renderer, h1, NULL, &r1);
    SDL_DestroyTexture(h1);
    
    const char *player_lines[] = {
        "Lewa polowa: BIALY",
        "Prawa polowa: CZARNY",
        "SPACJA: Pauza",
        "R: Reset",
        "H/ESC: Menu"
    };
    
    int y = 85;
    for (int i = 0; i < 5; i++) {
        SDL_Texture *line = render_text_texture(renderer, app->font_small, player_lines[i], COLOR_FG, &text_w, &text_h);
        if (line != NULL) {
            SDL_Rect dst = {30, y, text_w, text_h};
            SDL_RenderCopy(renderer, line, NULL, &dst);
            SDL_DestroyTexture(line);
        }
        y += 25;
    }
    
    // Prawa kolumna - arbiter
    SDL_Texture *h2 = render_text_texture(renderer, app->font_small, "Arbitra:", COLOR_ACCENT, &text_w, &text_h);
    SDL_Rect r2 = {650, 60, text_w, text_h};
    SDL_RenderCopy(renderer, h2, NULL, &r2);
    SDL_DestroyTexture(h2);
    
    const char *arbiter_lines[] = {
        "A: Stop/Q: Wznow",
        "1/2: Blad B/C",
        "3/4: +2min B/C"
    };
    
    y = 85;
    for (int i = 0; i < 3; i++) {
        SDL_Texture *line = render_text_texture(renderer, app->font_small, arbiter_lines[i], COLOR_FG, &text_w, &text_h);
        if (line != NULL) {
            SDL_Rect dst = {660, y, text_w, text_h};
            SDL_RenderCopy(renderer, line, NULL, &dst);
            SDL_DestroyTexture(line);
        }
        y += 25;
    }
    
    // Stopka
    SDL_Texture *footer = render_text_texture(renderer, app->font_small, 
                                             "ESC: Powrot do gry", 
                                             (SDL_Color){150, 150, 150, 255}, &text_w, &text_h);
    if (footer != NULL) {
        SDL_Rect dst = {(WINDOW_WIDTH - text_w) / 2, WINDOW_HEIGHT - 40, text_w, text_h};
        SDL_RenderCopy(renderer, footer, NULL, &dst);
        SDL_DestroyTexture(footer);
    }
    
    SDL_RenderPresent(renderer);
}

static void draw_arbiter_menu(SDL_Renderer *renderer, AppResources *app, const ClockState *clock_state) {
    SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(renderer);
    
    int text_w = 0, text_h = 0;
    
    // Przycisk zamkniecia - prawy gorny rog
    draw_close_button(renderer, app->font_small);
    
    // Tytul
    SDL_Texture *title = render_text_texture(renderer, app->font_medium, "MENU ARBITRA", COLOR_ACCENT, &text_w, &text_h);
    SDL_Rect title_rect = {20, 20, text_w, text_h};
    SDL_RenderCopy(renderer, title, NULL, &title_rect);
    SDL_DestroyTexture(title);
    
    // Przyciski arbitra - siatka 3x2 (uklad kompaktowy)
    const char *stop_label = (clock_state->state == STATE_STOPPED_BY_ARBITER) ? "WZNOW" : "STOP";
    
    // Wiersz 1
    Button stop_btn = {.rect = {80, 90, 220, 65}, .label = stop_label};
    Button error_white = {.rect = {350, 90, 220, 65}, .label = "BLAD BIALY"};
    
    // Wiersz 2
    Button error_black = {.rect = {620, 90, 220, 65}, .label = "BLAD CZARNY"};
    Button bonus_white = {.rect = {80, 175, 220, 65}, .label = "+2MIN BIALY"};
    
    // Wiersz 3
    Button bonus_black = {.rect = {350, 175, 220, 65}, .label = "+2MIN CZARNY"};
    Button close_btn = {.rect = {620, 175, 220, 65}, .label = "ZAMKNIJ"};
    
    draw_colored_button(renderer, app->font_small, &stop_btn, COLOR_BUTTON_NORMAL, COLOR_BUTTON_HOVER, false);
    draw_colored_button(renderer, app->font_small, &error_white, COLOR_BUTTON_ERROR, COLOR_BUTTON_ERROR_HOVER, false);
    draw_colored_button(renderer, app->font_small, &error_black, COLOR_BUTTON_ERROR, COLOR_BUTTON_ERROR_HOVER, false);
    draw_colored_button(renderer, app->font_small, &bonus_white, COLOR_BUTTON_BONUS, COLOR_BUTTON_BONUS_HOVER, false);
    draw_colored_button(renderer, app->font_small, &bonus_black, COLOR_BUTTON_BONUS, COLOR_BUTTON_BONUS_HOVER, false);
    draw_colored_button(renderer, app->font_small, &close_btn, COLOR_BUTTON_STOP, COLOR_BUTTON_STOP_HOVER, false);
    
    // Pokaz informacje o biezacym stanie
    char info[64];
    snprintf(info, sizeof(info), "B: %u bledy | C: %u bledy", 
             clock_state->left.error_count, clock_state->right.error_count);
    SDL_Texture *info_txt = render_text_texture(renderer, app->font_small, info, COLOR_FG, &text_w, &text_h);
    if (info_txt != NULL) {
        SDL_Rect dst = {20, 270, text_w, text_h};
        SDL_RenderCopy(renderer, info_txt, NULL, &dst);
        SDL_DestroyTexture(info_txt);
    }
    
    SDL_RenderPresent(renderer);
}

static bool init_sdl_and_ttf(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        (void)fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return false;
    }

    if (TTF_Init() != 0) {
        (void)fprintf(stderr, "TTF_Init error: %s\n", TTF_GetError());
        SDL_Quit();
        return false;
    }

    return true;
}

static bool create_window_and_renderer(AppResources *app) {
    app->window = SDL_CreateWindow("Zegar Szachowy",
                                   SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED,
                                   WINDOW_WIDTH,
                                   WINDOW_HEIGHT,
                                   SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (app->window == NULL) {
        (void)fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        return false;
    }

    app->renderer = SDL_CreateRenderer(app->window, -1, 
                                      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (app->renderer == NULL) {
        (void)fprintf(stderr, "SDL_CreateRenderer error: %s\n", SDL_GetError());
        return false;
    }

    return true;
}

static bool load_fonts(AppResources *app) {
    const char *font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    
    app->font_xlarge = TTF_OpenFont(font_path, 72);
    if (app->font_xlarge == NULL) {
        (void)fprintf(stderr, "TTF_OpenFont error (xlarge): %s\n", TTF_GetError());
        return false;
    }
    
    app->font_large = TTF_OpenFont(font_path, 36);
    if (app->font_large == NULL) {
        (void)fprintf(stderr, "TTF_OpenFont error (large): %s\n", TTF_GetError());
        return false;
    }
    
    app->font_medium = TTF_OpenFont(font_path, 24);
    if (app->font_medium == NULL) {
        (void)fprintf(stderr, "TTF_OpenFont error (medium): %s\n", TTF_GetError());
        return false;
    }
    
    app->font_small = TTF_OpenFont(font_path, 12);
    if (app->font_small == NULL) {
        (void)fprintf(stderr, "TTF_OpenFont error (small): %s\n", TTF_GetError());
        return false;
    }

    return true;
}

bool ui_init(AppResources *app) {
    if (!init_sdl_and_ttf()) {
        return false;
    }

    if (!create_window_and_renderer(app) || !load_fonts(app)) {
        ui_cleanup(app);
        return false;
    }

    app->mode = UI_MODE_SETUP;
    app->setup_minutes = START_MINUTES;
    app->setup_increment = INCREMENT_SECONDS;

    return true;
}

void ui_cleanup(AppResources *app) {
    if (app->font_xlarge != NULL) {
        TTF_CloseFont(app->font_xlarge);
    }
    if (app->font_large != NULL) {
        TTF_CloseFont(app->font_large);
    }
    if (app->font_medium != NULL) {
        TTF_CloseFont(app->font_medium);
    }
    if (app->font_small != NULL) {
        TTF_CloseFont(app->font_small);
    }
    if (app->renderer != NULL) {
        SDL_DestroyRenderer(app->renderer);
    }
    if (app->window != NULL) {
        SDL_DestroyWindow(app->window);
    }
    TTF_Quit();
    SDL_Quit();
}

static void handle_setup_events(ClockState *clock_state, AppResources *app, 
                               const SDL_Event *event, bool *app_running) {
    if (event->type == SDL_QUIT) {
        *app_running = false;
        return;
    }

    if (event->type == SDL_KEYDOWN) {
        if (event->key.keysym.sym == SDLK_ESCAPE) {
            *app_running = false;
        }
        return;
    }

    if (event->type == SDL_MOUSEBUTTONDOWN) {
        int x = event->button.x;
        int y = event->button.y;
        
        // Sprawdz przycisk zamkniecia (prawy gorny rog)
        if (x >= WINDOW_WIDTH - 55 && x <= WINDOW_WIDTH && y >= 10 && y <= 55) {
            *app_running = false;
            return;
        }
        
        // Sprawdz przycisk zmniejszenia czasu (330-370, 60-95)
        if (x >= 330 && x < 370 && y >= 60 && y < 95) {
            if (app->setup_minutes > 1) {
                app->setup_minutes--;
            }
            return;
        }
        
        // Sprawdz przycisk zwiekszenia czasu (440-480, 60-95)
        if (x >= 440 && x < 480 && y >= 60 && y < 95) {
            if (app->setup_minutes < 60) {
                app->setup_minutes++;
            }
            return;
        }
        
        // Sprawdz przycisk zmniejszenia inkrementu (810-850, 60-95)
        if (x >= 810 && x < 850 && y >= 60 && y < 95) {
            if (app->setup_increment > 0) {
                app->setup_increment--;
            }
            return;
        }
        
        // Sprawdz, czy kliknieto przycisk zwiekszenia inkrementu (920-960, 60-95)
        if (x >= 920 && x < 960 && y >= 60 && y < 95) {
            if (app->setup_increment < 30) {
                app->setup_increment++;
            }
            return;
        }
        
        // Sprawdz, czy kliknieto przycisk start (340-640, 130-200)
        if (x >= 340 && x <= 640 && y >= 130 && y <= 200) {
            uint32_t start_ms = app->setup_minutes * 60 * 1000;
            uint32_t increment_ms = app->setup_increment * 1000;
            init_clock(clock_state, start_ms, increment_ms);
            app->mode = UI_MODE_GAME;
            clock_state->state = STATE_PAUSED;
            return;
        }
    }
}

static void handle_game_events(ClockState *clock_state, AppResources *app,
                              const SDL_Event *event, bool *app_running) {
    if (event->type == SDL_QUIT) {
        *app_running = false;
        return;
    }

    if (event->type == SDL_KEYDOWN) {
        if (event->key.keysym.sym == SDLK_ESCAPE) {
            app->mode = UI_MODE_SETUP;
            return;
        }

        if (event->key.keysym.sym == SDLK_h) {
            app->mode = UI_MODE_HELP;
            return;
        }

        if (event->key.keysym.sym == SDLK_SPACE) {
            pause_resume_clock(clock_state);
            return;
        }

        if (event->key.keysym.sym == SDLK_r) {
            uint32_t start_ms = app->setup_minutes * 60 * 1000;
            uint32_t increment_ms = app->setup_increment * 1000;
            reset_clock(clock_state, start_ms, increment_ms);
            return;
        }

        if (event->key.keysym.sym == SDLK_a) {
            stop_by_arbiter(clock_state);
            return;
        }

        if (event->key.keysym.sym == SDLK_q) {
            resume_by_arbiter(clock_state);
            return;
        }

        if (event->key.keysym.sym == SDLK_1) {
            player_error(clock_state, ACTIVE_LEFT);
            return;
        }

        if (event->key.keysym.sym == SDLK_2) {
            player_error(clock_state, ACTIVE_RIGHT);
            return;
        }

        if (event->key.keysym.sym == SDLK_3) {
            add_bonus_time(clock_state, ACTIVE_LEFT, 2000);
            return;
        }

        if (event->key.keysym.sym == SDLK_4) {
            add_bonus_time(clock_state, ACTIVE_RIGHT, 2000);
            return;
        }
        return;
    }

    if (event->type == SDL_MOUSEBUTTONDOWN) {
        int x = event->button.x;
        int y = event->button.y;
        
        // Sprawdz, czy kliknieto zamknij (prawy gorny rog)
        if (x >= WINDOW_WIDTH - 55 && x <= WINDOW_WIDTH && y >= 10 && y <= 55) {
            *app_running = false;
            return;
        }
        
        // Sprawdz, czy kliknieto przycisk pomocy (1230-1280, 365-400)
        if (x >= 1230 && x <= 1280 && y >= 365 && y <= 400) {
            app->mode = UI_MODE_HELP;
            return;
        }
        
        // Sprawdz, czy kliknieto przycisk pauzy (50-170, 325-385)
        if (x >= 50 && x <= 170 && y >= 325 && y <= 385) {
            pause_resume_clock(clock_state);
            return;
        }
        
        // Sprawdz, czy kliknieto przycisk reset (200-300, 325-385)
        if (x >= 200 && x <= 300 && y >= 325 && y <= 385) {
            uint32_t start_ms = app->setup_minutes * 60 * 1000;
            uint32_t increment_ms = app->setup_increment * 1000;
            reset_clock(clock_state, start_ms, increment_ms);
            return;
        }
        
        // Sprawdz, czy kliknieto przycisk arbitra (350-500, 325-385)
        if (x >= 350 && x <= 500 && y >= 325 && y <= 385) {
            app->mode = UI_MODE_ARBITER;
            return;
        }
        
        // Kliknieto srodek lewej lub prawej polowy - zmiana aktywnego gracza
        ActiveSide side = (x < WINDOW_WIDTH / 2) ? ACTIVE_LEFT : ACTIVE_RIGHT;
        switch_side(clock_state, side);
    }
}

static void handle_arbiter_events(ClockState *clock_state, AppResources *app,
                                   const SDL_Event *event, bool *app_running) {
    if (event->type == SDL_QUIT) {
        *app_running = false;
        return;
    }

    if (event->type == SDL_KEYDOWN) {
        if (event->key.keysym.sym == SDLK_ESCAPE) {
            app->mode = UI_MODE_GAME;
            return;
        }
        return;
    }

    if (event->type == SDL_MOUSEBUTTONDOWN) {
        int x = event->button.x;
        int y = event->button.y;
        
        // Sprawdz, czy kliknieto zamknij (prawy gorny rog) - powrot do gry
        if (x >= WINDOW_WIDTH - 55 && x <= WINDOW_WIDTH && y >= 10 && y <= 55) {
            app->mode = UI_MODE_GAME;
            return;
        }
        
        // Sprawdz, czy kliknieto przycisk stop/wznow (80-300, 90-155)
        if (x >= 80 && x <= 300 && y >= 90 && y <= 155) {
            if (clock_state->state == STATE_STOPPED_BY_ARBITER) {
                resume_by_arbiter(clock_state);
            } else {
                stop_by_arbiter(clock_state);
            }
            return;
        }
        
        // Sprawdz, czy kliknieto blad bialego (350-570, 90-155)
        if (x >= 350 && x <= 570 && y >= 90 && y <= 155) {
            player_error(clock_state, ACTIVE_LEFT);
            return;
        }
        
        // Sprawdz, czy kliknieto blad czarnego (620-840, 90-155)
        if (x >= 620 && x <= 840 && y >= 90 && y <= 155) {
            player_error(clock_state, ACTIVE_RIGHT);
            return;
        }
        
        // Sprawdz, czy kliknieto bonus bialego (80-300, 175-240)
        if (x >= 80 && x <= 300 && y >= 175 && y <= 240) {
            add_bonus_time(clock_state, ACTIVE_LEFT, 2000);
            return;
        }
        
        // Sprawdz, czy kliknieto bonus czarnego (350-570, 175-240)
        if (x >= 350 && x <= 570 && y >= 175 && y <= 240) {
            add_bonus_time(clock_state, ACTIVE_RIGHT, 2000);
            return;
        }
        
        // Sprawdz, czy kliknieto zamknij (620-840, 175-240)
        if (x >= 620 && x <= 840 && y >= 175 && y <= 240) {
            app->mode = UI_MODE_GAME;
            return;
        }
    }
}

void ui_process_events(ClockState *clock_state, AppResources *app, bool *app_running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (app->mode == UI_MODE_SETUP) {
            handle_setup_events(clock_state, app, &event, app_running);
        } else if (app->mode == UI_MODE_HELP) {
            if (event.type == SDL_QUIT) {
                *app_running = false;
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                app->mode = UI_MODE_GAME;
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                int x = event.button.x;
                int y = event.button.y;
                // Przycisk zamknij w prawym gornym rogu
                if (x >= WINDOW_WIDTH - 55 && x <= WINDOW_WIDTH && y >= 10 && y <= 55) {
                    app->mode = UI_MODE_GAME;
                }
            }
        } else if (app->mode == UI_MODE_ARBITER) {
            handle_arbiter_events(clock_state, app, &event, app_running);
        } else {
            handle_game_events(clock_state, app, &event, app_running);
        }
    }
}

void ui_render_frame(SDL_Renderer *renderer, AppResources *app, const ClockState *clock_state) {
    if (app->mode == UI_MODE_SETUP) {
        draw_setup_screen(renderer, app);
    } else if (app->mode == UI_MODE_HELP) {
        draw_help_screen(renderer, app);
    } else if (app->mode == UI_MODE_ARBITER) {
        draw_arbiter_menu(renderer, app, clock_state);
    } else {
        draw_game_screen(renderer, app, clock_state);
    }
}   
