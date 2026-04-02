#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TIME_FORMAT_MM_SS,
    TIME_FORMAT_MM_SS_MS
} TimeFormat;

typedef struct {
    uint32_t remaining_ms;
    uint32_t increment_ms;
    uint32_t total_time_ms;
    uint8_t error_count;
} PlayerClock;

typedef enum {
    ACTIVE_LEFT = 0,
    ACTIVE_RIGHT = 1
} ActiveSide;

typedef enum {
    STATE_SETUP,
    STATE_PAUSED,
    STATE_RUNNING,
    STATE_STOPPED_BY_ARBITER,
    STATE_FINISHED_DRAW,
    STATE_FINISHED_LEFT_WIN,
    STATE_FINISHED_RIGHT_WIN
} GameState;

typedef struct {
    PlayerClock left;
    PlayerClock right;
    ActiveSide active;
    GameState state;
    uint32_t move_count;
} ClockState;

// Formatowanie czasu i inicjalizacja
void format_time(uint32_t ms, char *out, size_t out_len, TimeFormat format);
void init_clock(ClockState *clock_state, uint32_t start_ms, uint32_t increment_ms);
void reset_clock(ClockState *clock_state, uint32_t start_ms, uint32_t increment_ms);

// Sterowanie gra
void switch_side(ClockState *clock_state, ActiveSide pressed_side);
void pause_resume_clock(ClockState *clock_state);
void stop_by_arbiter(ClockState *clock_state);
void resume_by_arbiter(ClockState *clock_state);
void update_clock(ClockState *clock_state, uint32_t delta_ms);

// Modyfikacja czasu
void add_time(PlayerClock *clock, uint32_t ms);
void subtract_time(PlayerClock *clock, uint32_t ms);

// Funkcje arbitra - obsluga bledow i kar
void player_error(ClockState *clock_state, ActiveSide player);
void add_bonus_time(ClockState *clock_state, ActiveSide player, uint32_t bonus_ms);

#endif
