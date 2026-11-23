// SPDX-License-Identifier: Apache-2.0
#include "thingsos/home_ui.h"
#include <stdio.h>
#include <string.h>

static uint32_t date_of(const struct tm* day) {
    return (uint32_t)(day->tm_year + 1900) * 10000 + (day->tm_mon + 1) * 100 + day->tm_mday;
}

uint64_t thingsos_records_day_duration(const thingsos_records_t* r, uint32_t date) {
    if (!r) return 0;
    for (size_t i=0;i<r->day_count;++i) if (r->days[i].date==date) return r->days[i].milliseconds;
    uint64_t total=0;
    for (size_t i=0;i<r->book_day_count;++i) if (r->book_days[i].date==date) {
        uint64_t v=r->book_days[i].milliseconds; total=UINT64_MAX-total<v?UINT64_MAX:total+v;
    }
    return total;
}
unsigned thingsos_stats_heat_level(uint64_t ms) { return !ms?0:ms<5ULL*60000?1:ms<10ULL*60000?2:ms<15ULL*60000?3:ms<30ULL*60000?4:ms<45ULL*60000?5:ms<60ULL*60000?6:7; }

static uint64_t sum(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }

static time_t local_midnight(struct tm day) {
    day.tm_hour = day.tm_min = day.tm_sec = 0;
    day.tm_isdst = -1;
    return mktime(&day);
}

bool thingsos_home_stats_build(const thingsos_records_t* records, time_t now_sec,
                             uint64_t pending_ms, thingsos_home_stats_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!records || now_sec < 1577836800) return false;
    struct tm today;
#ifdef _WIN32
    if (localtime_s(&today, &now_sec)) return false;
#else
    if (!localtime_r(&now_sec, &today)) return false;
#endif
    if (today.tm_year > 199) return false;
    uint32_t today_date = date_of(&today);
    uint32_t month_first = today_date - (unsigned)today.tm_mday + 1;
    struct tm monday = today;
    monday.tm_mday -= (today.tm_wday + 6) % 7;
    monday.tm_hour = 12;
    monday.tm_min = monday.tm_sec = 0;
    monday.tm_isdst = -1;
    if (mktime(&monday) == (time_t)-1) return false;
    uint32_t week_first = date_of(&monday);
    const uint64_t end = (uint64_t)now_sec * 1000;
    const uint64_t begin = pending_ms > end ? 0 : end - pending_ms;
    /* All three totals fit within this window. Intersect at most 84 local
       days with the session interval, using real midnights across DST. */
    for (unsigned i = 0; i < THINGSOS_HOME_DAYS; ++i) {
        struct tm day = monday;
        day.tm_mday += (int)i - 77;
        day.tm_isdst = -1;
        if (mktime(&day) == (time_t)-1) goto invalid;
        uint32_t date = date_of(&day);
        out->grid_dates[i] = date;
        if (date > today_date) { out->grid_future[i] = true; continue; }
        uint64_t duration = thingsos_records_day_duration(records, date);
        if (pending_ms) {
            time_t start = local_midnight(day);
            ++day.tm_mday;
            time_t next = local_midnight(day);
            if (start < 0 || next <= start) goto invalid;
            uint64_t a = (uint64_t)start * 1000, b = (uint64_t)next * 1000;
            if (a < begin) a = begin;
            if (b > end) b = end;
            if (a < b) duration = sum(duration, b - a);
        }
        out->grid_ms[i] = duration;
        if (date == today_date) out->today_ms = duration;
        if (date >= week_first) out->week_ms = sum(out->week_ms, duration);
        if (date >= month_first) out->month_ms = sum(out->month_ms, duration);
    }
    out->today = today_date;
    out->dated = true;
    return true;
invalid:
    memset(out, 0, sizeof(*out));
    return false;
}

void thingsos_home_format_duration(uint64_t ms, char* n1, size_t c1, char* u1, size_t cu1,
                                   char* n2, size_t c2, char* u2, size_t cu2) {
    uint64_t minutes = ms / 60000;
    bool hours = minutes >= 60;
    snprintf(n1, c1, "%llu", (unsigned long long)(hours ? minutes / 60 : minutes));
    snprintf(u1, cu1, "%s", hours ? "小时" : "分钟");
    if (hours && minutes % 60) {
        snprintf(n2, c2, "%llu", (unsigned long long)(minutes % 60));
        snprintf(u2, cu2, "%s", "分");
    } else {
        if (c2) n2[0] = 0;
        if (cu2) u2[0] = 0;
    }
}

enum { LEFT = 30, RIGHT = 654, CONTINUE_Y = 120, CONTINUE_END = 388,
       STATS_Y = 408, STATS_END = 656, RECENT_Y = 678, ROW = 104,
       ENTRIES_Y = 990, RECENT_END = 1063, SPLIT = 342,
       GRID_X = 311, GRID_Y = 454, GRID_END = 638, ARROW_HALF = 6,
       CARD_BORDER_SHADE = 136, ENTRY_INK_SHADE = 85 };

thingsos_home_action_t thingsos_home_hit(int x, int y) {
    if (x < LEFT || x >= RIGHT || y < CONTINUE_Y || y >= RECENT_END) return THINGSOS_HOME_NONE;
    if (y < CONTINUE_END) return THINGSOS_HOME_CONTINUE;
    if (y >= STATS_Y && y < STATS_END) return THINGSOS_HOME_STATS;
    if (y < RECENT_Y) return THINGSOS_HOME_NONE;
    if (y < ENTRIES_Y) return (thingsos_home_action_t)(THINGSOS_HOME_RECENT_0 + (y - RECENT_Y) / ROW);
    if (x == SPLIT) return THINGSOS_HOME_NONE;
    return x < SPLIT ? THINGSOS_HOME_LOCAL : THINGSOS_HOME_WEIDU;
}

size_t thingsos_home_book_index(thingsos_home_action_t action) {
    if (action == THINGSOS_HOME_CONTINUE) return 0;
    if (action >= THINGSOS_HOME_RECENT_0 && action <= THINGSOS_HOME_RECENT_2)
        return 1 + (size_t)(action - THINGSOS_HOME_RECENT_0);
    return THINGSOS_HOME_BOOK_LIMIT;
}

static void outline(thingsos_canvas_t* canvas, int x, int y, int w, int h, unsigned char shade) {
    thingsos_rect(canvas, x, y, w, 1, shade);
    thingsos_rect(canvas, x, y + h - 1, w, 1, shade);
    thingsos_rect(canvas, x, y, 1, h, shade);
    thingsos_rect(canvas, x + w - 1, y, 1, h, shade);
}

static void draw_chevron(thingsos_canvas_t* canvas, int tip, int center, int half) {
    for (int dy = -half; dy <= half; ++dy) {
        int distance = dy < 0 ? -dy : dy;
        thingsos_rect(canvas, tip - distance - 1, center + dy, 2, 1, 0);
    }
}

static void text(thingsos_font_t* font, thingsos_canvas_t* canvas, const char* label,
                 unsigned size, int x, int baseline, int width) {
    thingsos_font_size(font, size);
    thingsos_label(font, canvas, label, x, baseline, width);
}

static unsigned book_percent(const thingsos_home_book_t* book) {
    return book->percent > 100 ? 100 : book->percent;
}

static void draw_continue(thingsos_font_t* font, thingsos_canvas_t* canvas, const thingsos_home_book_t* book,
                          bool sd_ready, thingsos_home_cover_fn cover, void* context) {
    outline(canvas, LEFT, CONTINUE_Y, RIGHT - LEFT, CONTINUE_END - CONTINUE_Y, CARD_BORDER_SHADE);
    draw_chevron(canvas, 624, 174, ARROW_HALF);
    if (!book) {
        text(font, canvas, sd_ready ? "暂无阅读记录" : "未检测到 SD 卡", 28, 54, 248, 540);
        text(font, canvas, "点击进入本地书库", 22, 54, 296, 540);
        return;
    }
    if (cover) cover(context, canvas, 0, 54, 144, 154, 220);
    text(font, canvas, book->title, 40, 236, 188, 356);
    text(font, canvas, book->subtitle, 24, 236, 248, 394);
    char line[64];
    if (book->pages) snprintf(line, sizeof(line), "%s · %u 页", book->format, book->pages);
    else snprintf(line, sizeof(line), "%s", book->format);
    text(font, canvas, line, 20, 236, 280, 394);
    unsigned percent = book_percent(book);
    snprintf(line, sizeof(line), "%u", percent);
    thingsos_font_size(font, 34);
    int advance = 0;
    thingsos_text_ink_box(font, line, 100, NULL, NULL, NULL, NULL, &advance);
    text(font, canvas, line, 34, 236, 338, 100);
    text(font, canvas, "%", 22, 240 + advance, 338, 30);
    if (book->page) {
        snprintf(line, sizeof(line), "第 %u 页", book->page);
        thingsos_font_size(font, 22);
        thingsos_label_right(font, canvas, line, 450, 338, 180);
    }
    thingsos_rect(canvas, 236, 354, 394, 10, 220);
    thingsos_rect(canvas, 236, 354, (int)(394 * percent / 100), 10, 0);
}

typedef struct { char text[24]; unsigned size; int x, width, bottom; } metric_part_t;

static void draw_duration(thingsos_font_t* font, thingsos_canvas_t* canvas, uint64_t ms, int ink_end) {
    metric_part_t parts[] = {{{0},32,0,0,0},{{0},20,0,0,0},{{0},32,0,0,0},{{0},20,0,0,0}};
    thingsos_home_format_duration(ms, parts[0].text, sizeof(parts[0].text), parts[1].text, sizeof(parts[1].text),
                                  parts[2].text, sizeof(parts[2].text), parts[3].text, sizeof(parts[3].text));
    int left = 54, bottom = -32;
    for (unsigned i = 0; i < 4 && parts[i].text[0] && left < 270; ++i) {
        metric_part_t* p = &parts[i];
        p->x = left;
        thingsos_font_size(font, p->size);
        int advance = 0;
        if (thingsos_text_ink_box(font, p->text, 270 - left, NULL, NULL, NULL, &p->bottom, &advance)) {
            p->width = advance;
            if (p->bottom > bottom) bottom = p->bottom;
        }
        left += advance + (i % 2 ? 8 : 4);
    }
    for (unsigned i = 0; i < 4; ++i)
        if (parts[i].width) text(font, canvas, parts[i].text, parts[i].size, parts[i].x, ink_end - bottom, parts[i].width);
}

static void draw_stats(thingsos_font_t* font, thingsos_canvas_t* canvas, const thingsos_home_stats_t* stats) {
    outline(canvas, LEFT, STATS_Y, RIGHT - LEFT, STATS_END - STATS_Y, CARD_BORDER_SHADE);
    const int arrow_center = 435;
    draw_chevron(canvas, 624, arrow_center, ARROW_HALF);
    static const char* const labels[] = {"今日阅读", "本周已读", "本月已读"};
    const uint64_t durations[] = {stats->today_ms, stats->week_ms, stats->month_ms};
    const int group_pitch = 78;
    int top = -16;
    thingsos_font_size(font, 20);
    thingsos_text_ink_box(font, labels[0], 216, NULL, NULL, &top, NULL, NULL);
    const int first_baseline = arrow_center - ARROW_HALF - top;
    for (int i = 0; i < 3; ++i) {
        text(font, canvas, labels[i], 20, 54, first_baseline + i * group_pitch, 216);
        draw_duration(font, canvas, durations[i], GRID_END - 1 - (2 - i) * group_pitch);
    }
    static const unsigned char shades[] = {238,204,170,136,102,68,34,0};
    for (unsigned i = 0; i < THINGSOS_HOME_DAYS; ++i) {
        int x = GRID_X + (int)(i / 7) * 27, y = GRID_Y + (int)(i % 7) * 27;
        if (stats->dated && stats->grid_future[i]) {
            thingsos_rect(canvas, x, y, 22, 22, 255);
            outline(canvas, x, y, 22, 22, 204);
        } else thingsos_rect(canvas, x, y, 22, 22, shades[stats->dated ? thingsos_stats_heat_level(stats->grid_ms[i]) : 0]);
    }
    static const char* const days[] = {"一", "三", "五", "日"};
    for (unsigned i = 0; i < 4; ++i) {
        int ink_top = -14, ink_bottom = 1;
        thingsos_font_size(font, 18);
        thingsos_text_ink_box(font, days[i], 26, NULL, NULL, &ink_top, &ink_bottom, NULL);
        text(font, canvas, days[i], 18, 282, GRID_Y + 11 + (int)i * 54 - (ink_top + ink_bottom) / 2, 26);
    }
}

static void fade_region(thingsos_canvas_t* c, int x, int y, int w, int h, unsigned char shade) {
    if (!c || !c->pixels) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->width ? c->width : x + w;
    int y1 = y + h > c->height ? c->height : y + h;
    for (int yy = y0; yy < y1; ++yy) {
        unsigned char* row = c->pixels + (size_t)yy * c->width;
        for (int xx = x0; xx < x1; ++xx)
            if (row[xx] < 255) row[xx] = (unsigned char)(shade + (unsigned)row[xx] * (255u - shade) / 255u);
    }
}

static void draw_home_entry(thingsos_font_t* font, thingsos_canvas_t* canvas, const char* title,
                            int label_x, int arrow_x) {
    int top = -18, bottom = 2;
    int center = (ENTRIES_Y + RECENT_END) / 2;
    thingsos_font_size(font, 22);
    thingsos_text_ink_box(font, title, 240, NULL, NULL, &top, &bottom, NULL);
    text(font, canvas, title, 22, label_x, center - (top + bottom) / 2, 240);
    draw_chevron(canvas, arrow_x, center, 5);
}

static void draw_recent(thingsos_font_t* font, thingsos_canvas_t* canvas, const thingsos_home_book_t* books,
                        size_t count, thingsos_home_cover_fn cover, void* context) {
    outline(canvas, LEFT, RECENT_Y, RIGHT - LEFT, RECENT_END - RECENT_Y, CARD_BORDER_SHADE);
    for (size_t i = 0; i < THINGSOS_HOME_RECENT_LIMIT; ++i) {
        int row_y = RECENT_Y + (int)i * ROW;
        size_t index = thingsos_home_book_index((thingsos_home_action_t)(THINGSOS_HOME_RECENT_0 + i));
        if (index < count) {
            const thingsos_home_book_t* book = &books[index];
            if (cover) cover(context, canvas, index, 54, row_y + 14, 56, 76);
            text(font, canvas, book->title, 28, 128, row_y + 46, 464);
            char info[64];
            if (book->page) snprintf(info, sizeof(info), "%s · 第 %u 页 · %u%%", book->format, book->page, book_percent(book));
            else if (book->started) snprintf(info, sizeof(info), "%s · %u%%", book->format, book_percent(book));
            else snprintf(info, sizeof(info), "%s · 未读", book->format);
            text(font, canvas, info, 21, 128, row_y + 82, 464);
            draw_chevron(canvas, 624, row_y + 52, ARROW_HALF);
        }
        if (index < count || i == THINGSOS_HOME_RECENT_LIMIT - 1)
            thingsos_rect(canvas, 54, row_y + ROW - 1, 576, 1, 220);
    }
    if (count <= 1) {
        thingsos_font_size(font, 28);
        thingsos_label_centered(font, canvas, count ? "暂无其他最近阅读" : "暂无最近阅读", 54, 844, 576);
    }
    thingsos_rect(canvas, SPLIT, 1004, 1, 45, 210);
    draw_home_entry(font, canvas, "本地", 54, 318);
    draw_home_entry(font, canvas, "微读", 366, 624);
    fade_region(canvas, 32, 992, 309, 70, ENTRY_INK_SHADE);
    fade_region(canvas, 344, 992, 309, 70, ENTRY_INK_SHADE);
}

void thingsos_home_draw(thingsos_font_t* font, thingsos_canvas_t* canvas, const thingsos_home_stats_t* stats,
                       const thingsos_home_book_t* books, size_t count, bool sd_ready,
                       thingsos_home_cover_fn cover, void* context) {
    unsigned scale = font->ui_size_percent, weight = font->ui_weight_override;
    font->ui_size_percent = font->ui_weight_override = 0;
    unsigned previous_weight = font->weight;
    thingsos_font_weight(font, 0);
    draw_continue(font, canvas, count ? books : NULL, sd_ready, cover, context);
    draw_stats(font, canvas, stats);
    draw_recent(font, canvas, books, count, cover, context);
    thingsos_font_weight(font, previous_weight);
    font->ui_size_percent = scale;
    font->ui_weight_override = weight;
}
