#include <stdbool.h>
#include <stdint.h>

#include <SDL2/SDL.h>

#include "app.h"
#include "clock.h"
#include "ui.h"

int main(void) {
    AppResources app = {0};
    if (!ui_init(&app)) {
        return 1;
    }

    ClockState clock_state = {0};
    init_clock(&clock_state, START_MINUTES * 60 * 1000, INCREMENT_SECONDS * 1000);

    bool app_running = true;
    uint32_t prev_tick = SDL_GetTicks();

    while (app_running) {
        ui_process_events(&clock_state, &app, &app_running);

        uint32_t now = SDL_GetTicks();
        uint32_t delta = now - prev_tick;
        prev_tick = now;
        update_clock(&clock_state, delta);

        ui_render_frame(app.renderer, &app, &clock_state);
        SDL_Delay(1000 / FPS);
    }

    ui_cleanup(&app);
    return 0;
}
