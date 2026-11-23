// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <time.h>
#include "thingsos/graphics.h"
#include "thingsos/store.h"

enum {
    THINGSOS_HOME_DAYS = 84,
    THINGSOS_HOME_RECENT_LIMIT = 3,
    THINGSOS_HOME_BOOK_LIMIT = 1 + THINGSOS_HOME_RECENT_LIMIT
};
typedef struct {
    bool dated;
    uint32_t today;
    uint64_t today_ms, week_ms, month_ms;
    uint32_t grid_dates[THINGSOS_HOME_DAYS];
    uint64_t grid_ms[THINGSOS_HOME_DAYS];
    bool grid_future[THINGSOS_HOME_DAYS];
} thingsos_home_stats_t;

typedef struct {
    const char *title, *subtitle, *format;
    unsigned page, pages, percent;
    bool started;
} thingsos_home_book_t;

typedef enum {
    THINGSOS_HOME_NONE, THINGSOS_HOME_CONTINUE, THINGSOS_HOME_STATS,
    THINGSOS_HOME_RECENT_0, THINGSOS_HOME_RECENT_1, THINGSOS_HOME_RECENT_2,
    THINGSOS_HOME_LOCAL, THINGSOS_HOME_WEIDU
} thingsos_home_action_t;

typedef void (*thingsos_home_cover_fn)(void*, thingsos_canvas_t*, size_t, int, int, int, int);
uint64_t thingsos_records_day_duration(const thingsos_records_t*, uint32_t date);
unsigned thingsos_stats_heat_level(uint64_t ms);
bool thingsos_home_stats_build(const thingsos_records_t*, time_t now_sec, uint64_t pending_ms, thingsos_home_stats_t*);
void thingsos_home_format_duration(uint64_t ms, char*, size_t, char*, size_t, char*, size_t, char*, size_t);
thingsos_home_action_t thingsos_home_hit(int x, int y);
/* Book order is newest first. Non-book actions return THINGSOS_HOME_BOOK_LIMIT. */
size_t thingsos_home_book_index(thingsos_home_action_t action);
/* The first book is for Continue Reading, followed by the recent rows. */
void thingsos_home_draw(thingsos_font_t*, thingsos_canvas_t*, const thingsos_home_stats_t*,
    const thingsos_home_book_t*, size_t, bool sd_ready, thingsos_home_cover_fn, void*);
