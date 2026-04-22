#include "clock.h"

#include <stdio.h>
#include <string.h>

void format_time(uint32_t ms, char *out, size_t out_len, TimeFormat format) {
    uint32_t total_seconds = ms / 1000;
    uint32_t minutes = total_seconds / 60;
    uint32_t seconds = total_seconds % 60;
    uint32_t milliseconds = ms % 1000;

    if (format == TIME_FORMAT_MM_SS) {
        (void)snprintf(out, out_len, "%02u:%02u", minutes, seconds);
    } else {
        if (minutes > 0) {
            (void)snprintf(out, out_len, "%02u:%02u", minutes, seconds);
        } else {
            (void)snprintf(out, out_len, "%02u.%03u", seconds, milliseconds);
        }
    }
}

void init_clock(ClockState *clock_state, uint32_t start_ms, uint32_t increment_ms) {
    clock_state->left.remaining_ms = start_ms;
    clock_state->right.remaining_ms = start_ms;
    clock_state->left.increment_ms = increment_ms;
    clock_state->right.increment_ms = increment_ms;
    clock_state->left.total_time_ms = start_ms;
    clock_state->right.total_time_ms = start_ms;
    clock_state->left.error_count = 0;
    clock_state->right.error_count = 0;
    clock_state->active = ACTIVE_LEFT;
    clock_state->state = STATE_PAUSED;
    clock_state->move_count = 0;
}

void reset_clock(ClockState *clock_state, uint32_t start_ms, uint32_t increment_ms) {
    init_clock(clock_state, start_ms, increment_ms);
}

void pause_resume_clock(ClockState *clock_state) {
    if (clock_state->state == STATE_FINISHED_DRAW ||
        clock_state->state == STATE_FINISHED_LEFT_WIN ||
        clock_state->state == STATE_FINISHED_RIGHT_WIN) {
        return;
    }

    if (clock_state->state == STATE_PAUSED) {
        clock_state->state = STATE_RUNNING;
    } else if (clock_state->state == STATE_RUNNING) {
        clock_state->state = STATE_PAUSED;
    }
}

void switch_side(ClockState *clock_state, ActiveSide pressed_side) {
    if (clock_state->state != STATE_RUNNING) {
        return;
    }

    if (pressed_side != clock_state->active) {
        return;
    }

    //dodaj czas dla gracza ktory wlasnie zagral ruch, zanim przełączymy aktywnego gracza
    PlayerClock *active_clock = (clock_state->active == ACTIVE_LEFT) ? &clock_state->left : &clock_state->right;
    active_clock->remaining_ms += active_clock->increment_ms;

    // zmien aktywnego gracza
    clock_state->active = (clock_state->active == ACTIVE_LEFT) ? ACTIVE_RIGHT : ACTIVE_LEFT;
    clock_state->move_count++;
}

void update_clock(ClockState *clock_state, uint32_t delta_ms) {
    if (clock_state->state != STATE_RUNNING) {
        return;
    }

    PlayerClock *active_clock = (clock_state->active == ACTIVE_LEFT) ? &clock_state->left : &clock_state->right;

    if (delta_ms >= active_clock->remaining_ms) {
        active_clock->remaining_ms = 0;
        
        // wybierz zwyciezce
        if (clock_state->active == ACTIVE_LEFT) {
            clock_state->state = STATE_FINISHED_RIGHT_WIN;
        } else {
            clock_state->state = STATE_FINISHED_LEFT_WIN;
        }
        return;
    }

    active_clock->remaining_ms -= delta_ms;
}

void add_time(PlayerClock *clock, uint32_t ms) {
    clock->remaining_ms += ms;
}

void subtract_time(PlayerClock *clock, uint32_t ms) {
    if (ms >= clock->remaining_ms) {
        clock->remaining_ms = 0;
    } else {
        clock->remaining_ms -= ms;
    }
}

void stop_by_arbiter(ClockState *clock_state) {
    if (clock_state->state == STATE_RUNNING || clock_state->state == STATE_PAUSED) {
        clock_state->state = STATE_STOPPED_BY_ARBITER;
    }
}

void resume_by_arbiter(ClockState *clock_state) {
    if (clock_state->state == STATE_STOPPED_BY_ARBITER) {
        clock_state->state = STATE_PAUSED;
    }
}

void player_error(ClockState *clock_state, ActiveSide player) {
    PlayerClock *player_clock = (player == ACTIVE_LEFT) ? &clock_state->left : &clock_state->right;
    
    player_clock->error_count++;
    
    // przegrana na bledy
    if (player_clock->error_count >= 2) {
        if (player == ACTIVE_LEFT) {
            clock_state->state = STATE_FINISHED_RIGHT_WIN;
        } else {
            clock_state->state = STATE_FINISHED_LEFT_WIN;
        }
    }
}

void add_bonus_time(ClockState *clock_state, ActiveSide player, uint32_t bonus_ms) {
    PlayerClock *player_clock = (player == ACTIVE_LEFT) ? &clock_state->left : &clock_state->right;
    player_clock->remaining_ms += bonus_ms;
}
