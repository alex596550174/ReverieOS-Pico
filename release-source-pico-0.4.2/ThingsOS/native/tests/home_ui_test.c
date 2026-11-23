// SPDX-License-Identifier: Apache-2.0
#include "thingsos/home_ui.h"
#include "thingsos/reader_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "home UI line %d: %s\n", __LINE__, #x); return false; } } while (0)
#define MINUTES(x) ((uint64_t)(x) * 60000)
enum { WIDTH = 684, HEIGHT = 1216 };
static thingsos_records_t records;
static unsigned char pixels[WIDTH * HEIGHT];

static time_t local_time(int year, int month, int day, int hour, int minute) {
    struct tm value = {0};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_isdst = -1;
    return mktime(&value);
}

static void add_day(uint32_t date, uint64_t ms) {
    records.days[records.day_count++] = (thingsos_day_record_t){date, ms};
}

static bool check_stats(void) {
    thingsos_records_init(&records);
    add_day(20261119, MINUTES(24));
    add_day(20261120, MINUTES(99));
    records.book_days[0] = (thingsos_book_day_t){7, MINUTES(10), 20260831};
    records.book_days[1] = (thingsos_book_day_t){8, MINUTES(12), 20260831};
    records.book_days[2] = (thingsos_book_day_t){7, MINUTES(99), 20261119};
    records.book_days[3] = (thingsos_book_day_t){7, UINT64_MAX, 20260901};
    records.book_days[4] = (thingsos_book_day_t){8, 1, 20260901};
    records.book_day_count = 5;
    thingsos_home_stats_t stats;
    CHECK(thingsos_home_stats_build(&records, local_time(2026,11,19,12,0), 0, &stats));
    CHECK(stats.dated && stats.today == 20261119);
    CHECK(stats.grid_dates[0] == 20260831 && stats.grid_dates[77] == 20261116 && stats.grid_dates[83] == 20261122);
    CHECK(stats.grid_ms[0] == MINUTES(22) && stats.grid_ms[1] == UINT64_MAX);
    CHECK(stats.today_ms == MINUTES(24) && stats.week_ms == MINUTES(24) && stats.month_ms == MINUTES(24));
    for (unsigned i = 0; i < THINGSOS_HOME_DAYS; ++i) CHECK(stats.grid_future[i] == (i >= 81));
    CHECK(!stats.grid_ms[81]);
    CHECK(thingsos_home_stats_build(&records, local_time(2026,11,16,0,5), MINUTES(15), &stats));
    CHECK(stats.today_ms == MINUTES(5) && stats.week_ms == MINUTES(5) && stats.month_ms == MINUTES(15));
    CHECK(stats.grid_dates[76] == 20261115 && stats.grid_ms[76] == MINUTES(10) && stats.grid_ms[77] == MINUTES(5));
    CHECK(thingsos_home_stats_build(&records, local_time(2026,6,1,0,5), MINUTES(15), &stats));
    CHECK(stats.today_ms == MINUTES(5) && stats.week_ms == MINUTES(5) && stats.month_ms == MINUTES(5));
    CHECK(stats.grid_dates[76] == 20260531 && stats.grid_ms[76] == MINUTES(10));
    CHECK(thingsos_home_stats_build(&records, local_time(2026,1,1,0,5), MINUTES(15), &stats));
    CHECK(stats.grid_dates[77] == 20251229 && stats.grid_dates[83] == 20260104);
    CHECK(stats.today_ms == MINUTES(5) && stats.week_ms == MINUTES(15) && stats.month_ms == MINUTES(5));
    CHECK(stats.grid_dates[79] == 20251231 && stats.grid_ms[79] == MINUTES(10));
    CHECK(thingsos_home_stats_build(&records, local_time(2024,2,29,12,0), UINT64_MAX, &stats));
    CHECK(stats.grid_dates[0] == 20231211 && stats.grid_dates[80] == 20240229 && stats.grid_dates[81] == 20240301);
    CHECK(stats.today_ms == MINUTES(720) && stats.month_ms == MINUTES(28 * 1440 + 720));
    CHECK(!thingsos_home_stats_build(&records, 1577836799, MINUTES(15), &stats));
    CHECK(!stats.dated && !stats.today && !stats.today_ms && !stats.week_ms && !stats.month_ms);
    for (unsigned i = 0; i < THINGSOS_HOME_DAYS; ++i)
        CHECK(!stats.grid_dates[i] && !stats.grid_ms[i] && !stats.grid_future[i]);
    const uint64_t thresholds[] = {0, 1, MINUTES(5), MINUTES(10), MINUTES(15), MINUTES(30), MINUTES(45), MINUTES(60)};
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(thingsos_stats_heat_level(thresholds[i]) == i);
        if (i) CHECK(thingsos_stats_heat_level(thresholds[i] - 1) == i - 1);
    }
    char n1[24], u1[16], n2[24], u2[16];
    thingsos_home_format_duration(MINUTES(187),n1,sizeof(n1),u1,sizeof(u1),n2,sizeof(n2),u2,sizeof(u2));
    CHECK(!strcmp(n1,"3") && !strcmp(u1,"小时") && !strcmp(n2,"7") && !strcmp(u2,"分"));
    thingsos_home_format_duration(0,n1,sizeof(n1),u1,sizeof(u1),n2,sizeof(n2),u2,sizeof(u2));
    CHECK(!strcmp(n1,"0") && !strcmp(u1,"分钟") && !n2[0] && !u2[0]);
    return true;
}

static bool check_hits(void) {
    const struct { int first, past; thingsos_home_action_t action; } rows[] = {
        {120,388,THINGSOS_HOME_CONTINUE},{388,408,THINGSOS_HOME_NONE},
        {408,656,THINGSOS_HOME_STATS},{656,678,THINGSOS_HOME_NONE},
        {678,782,THINGSOS_HOME_RECENT_0},{782,886,THINGSOS_HOME_RECENT_1},
        {886,990,THINGSOS_HOME_RECENT_2},{990,1063,THINGSOS_HOME_LOCAL},
        {1063,1098,THINGSOS_HOME_NONE},
    };
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i) {
        CHECK(thingsos_home_hit(30,rows[i].first) == rows[i].action);
        CHECK(thingsos_home_hit(341,rows[i].past-1) == rows[i].action);
        CHECK(thingsos_home_hit(29,rows[i].first) == THINGSOS_HOME_NONE);
        CHECK(thingsos_home_hit(654,rows[i].past-1) == THINGSOS_HOME_NONE);
    }
    CHECK(thingsos_home_hit(653,387) == THINGSOS_HOME_CONTINUE);
    CHECK(thingsos_home_book_index(thingsos_home_hit(653,387)) == 0);
    CHECK(thingsos_home_book_index(thingsos_home_hit(50,700)) == 1);
    CHECK(thingsos_home_book_index(thingsos_home_hit(50,800)) == 2);
    CHECK(thingsos_home_book_index(thingsos_home_hit(50,900)) == 3);
    CHECK(thingsos_home_book_index(THINGSOS_HOME_LOCAL) == THINGSOS_HOME_BOOK_LIMIT);
    for (int y = 990; y < 1063; ++y) {
        CHECK(thingsos_home_hit(342,y) == THINGSOS_HOME_NONE);
        CHECK(thingsos_home_hit(343,y) == THINGSOS_HOME_WEIDU);
        CHECK(thingsos_home_hit(653,y) == THINGSOS_HOME_WEIDU);
    }
    CHECK(thingsos_home_hit(30,119) == THINGSOS_HOME_NONE);
    CHECK(thingsos_home_hit(-1,120) == THINGSOS_HOME_NONE && thingsos_home_hit(WIDTH,120) == THINGSOS_HOME_NONE);
    CHECK(thingsos_home_hit(50,-1) == THINGSOS_HOME_NONE && thingsos_home_hit(50,HEIGHT) == THINGSOS_HOME_NONE);
    return true;
}

static unsigned char pixel(int x, int y) { return pixels[(size_t)y * WIDTH + x]; }

static bool shade_box(int x, int y, int w, int h, unsigned shade) {
    for (int j = y; j < y+h; ++j) for (int i = x; i < x+w; ++i)
        if (pixel(i,j) != shade) return false;
    return true;
}

static int ink_edge(int x, int y, int w, int h, bool top) {
    for (int row = 0; row < h; ++row) {
        int py = top ? y+row : y+h-1-row;
        for (int i = x; i < x+w; ++i) if (pixel(i,py)<200) return py;
    }
    return -1;
}

/* Deterministic sample covers for the Host fixture previews. */
static void sample_cover(void* context, thingsos_canvas_t* canvas, size_t index, int x, int y, int w, int h) {
    thingsos_font_t* font = context;
    static const char* const titles[] = {"一页微光", "南方来信", "山间日记"};
    thingsos_rect(canvas,x,y,w,h,234);
    thingsos_rect(canvas,x,y+h*3/10,w,h*45/100,(unsigned char)(140 + index*11));
    for (int px = 0; px < w; ++px) {
        int left = w/3 - abs(px-w/3), right = w/4 - abs(px-w*3/4);
        int rise = left > right ? left : right;
        if (rise > 0) thingsos_rect(canvas,x+px,y+h*3/4-rise,1,rise,48);
    }
    int radius = w/12, sx = x+w/2, sy = y+h*43/100;
    for (int j = -radius; j <= radius; ++j) for (int i = -radius; i <= radius; ++i)
        if (i*i+j*j <= radius*radius) thingsos_rect(canvas,sx+i,sy+j,1,1,255);
    if (w>100) {
        thingsos_font_size(font,24);
        thingsos_label_centered(font,canvas,titles[index],x,y+49,w);
    }
}

static bool write_preview(const char* directory, const char* name) {
    char path[1024];
    int n = snprintf(path,sizeof(path),"%s/%s",directory,name);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    FILE* file = fopen(path,"wb");
    CHECK(file);
    bool ok = fprintf(file,"P5\n%d %d\n255\n",WIDTH,HEIGHT)>0 && fwrite(pixels,1,sizeof(pixels),file)==sizeof(pixels);
    CHECK(!fclose(file) && ok);
    return true;
}

static bool check_drawing(thingsos_font_t* font, const char* output) {
    thingsos_canvas_t canvas = {pixels,WIDTH,HEIGHT,false};
    thingsos_home_stats_t stats;
    thingsos_records_init(&records);
    add_day(20261119,MINUTES(24)); add_day(20261117,MINUTES(34));
    add_day(20261108,MINUTES(62)); add_day(20261102,MINUTES(47)); add_day(20261101,MINUTES(20));
    CHECK(thingsos_home_stats_build(&records,local_time(2026,11,19,12,0),0,&stats));
    const unsigned shade[] = {238,204,170,136,102,68,34,0};
    const unsigned minutes[] = {0,2,5,10,15,30,45,60};
    for (unsigned i = 0; i < 77; ++i) if (!stats.grid_ms[i]) stats.grid_ms[i] = MINUTES(minutes[(i*5+i/7)%8]);
    const thingsos_home_book_t books[] = {
        {"一页微光","第三章 · 河岸","EPUB",72,195,37,true},
        {"南方来信","","TXT",34,80,42,true},
        {"山间日记","","EPUB",51,81,63,true},
        {"海边旧事","","TXT",19,95,20,true},
    };
    memset(pixels,255,sizeof(pixels));
    thingsos_home_draw(font,&canvas,&stats,books,sizeof(books)/sizeof(books[0]),true,sample_cover,font);
    /* Cover stripes encode the input index so a repeated or shifted row is detectable. */
    CHECK(pixel(55,218) == 140);
    CHECK(pixel(55,716) == 151 && pixel(55,820) == 162 && pixel(55,924) == 173);
    CHECK(!font->errors);
    const int edges[][2] = {{120,388},{408,656},{678,1063}};
    for (size_t i = 0; i < 3; ++i) {
        CHECK(shade_box(30,edges[i][0],624,1,136) && shade_box(30,edges[i][1]-1,624,1,136));
        CHECK(shade_box(30,edges[i][0],1,edges[i][1]-edges[i][0],136));
        CHECK(shade_box(653,edges[i][0],1,edges[i][1]-edges[i][0],136));
    }
    CHECK(shade_box(236,354,145,10,0) && shade_box(381,354,249,10,220));
    CHECK(shade_box(342,1004,1,45,210));
    const int entry_regions[][2] = {{54,110},{366,422},{306,325},{612,631}};
    for (size_t i = 0; i < sizeof(entry_regions)/sizeof(entry_regions[0]); ++i) {
        unsigned char minimum = 255;
        for (int y = 1000; y < 1055; ++y)
            for (int x = entry_regions[i][0]; x < entry_regions[i][1]; ++x)
                if (pixel(x,y) < minimum) minimum = pixel(x,y);
        CHECK(minimum >= 85 && minimum < 118);
    }
    CHECK(shade_box(30,388,624,20,255) && shade_box(30,656,624,22,255) && shade_box(30,1063,624,35,255));
    bool used[8] = {0};
    for (unsigned i = 0; i < THINGSOS_HOME_DAYS; ++i) {
        int x = 311+(int)(i/7)*27, y = 454+(int)(i%7)*27;
        if (stats.grid_future[i]) {
            CHECK(shade_box(x,y,22,1,204) && shade_box(x,y+21,22,1,204));
            CHECK(shade_box(x,y,1,22,204) && shade_box(x+21,y,1,22,204));
            CHECK(shade_box(x+1,y+1,20,20,255));
        } else {
            unsigned level = thingsos_stats_heat_level(stats.grid_ms[i]);
            used[level] = true;
            CHECK(shade_box(x,y,22,22,shade[level]));
        }
    }
    for (unsigned i = 0; i < 8; ++i) CHECK(used[i]);
    struct { int top, bottom; } lines[6];
    unsigned line_count = 0;
    bool previous_ink = false;
    for (int y = 416; y < 646; ++y) {
        bool ink = ink_edge(54,y,216,1,true) >= 0;
        if (ink && !previous_ink) {
            CHECK(line_count < sizeof(lines)/sizeof(lines[0]));
            lines[line_count++].top = y;
        }
        if (ink) lines[line_count-1].bottom = y;
        previous_ink = ink;
    }
    CHECK(line_count == sizeof(lines)/sizeof(lines[0]));
    int inside_gaps[3], between_gaps[2];
    for (unsigned i = 0; i < 3; ++i)
        inside_gaps[i] = lines[2*i+1].top - lines[2*i].bottom - 1;
    for (unsigned i = 0; i < 2; ++i) {
        between_gaps[i] = lines[2*i+2].top - lines[2*i+1].bottom - 1;
        CHECK(between_gaps[i] > inside_gaps[i] + 8 && between_gaps[i] > inside_gaps[i+1] + 8);
    }
    printf("Stats spacing: %d/%d/%d px within groups, %d/%d px between groups\n",
        inside_gaps[0],inside_gaps[1],inside_gaps[2],between_gaps[0],between_gaps[1]);
    int today_top = lines[0].top, arrow_top = ink_edge(608,416,24,36,true);
    int month_bottom = lines[5].bottom;
    int grid_bottom = ink_edge(311,454,319,184,false);
    CHECK(today_top >= 0 && arrow_top >= 0 && abs(today_top-arrow_top) <= 2);
    CHECK(month_bottom >= 0 && abs(month_bottom-638) <= 2);
    CHECK(grid_bottom == 637 && abs(month_bottom-grid_bottom) <= 2);
    printf("今日阅读 y=%d, arrow y=%d, top delta %d px; month y=%d, grid last y=%d, bottom delta %d px (vs y=638: %d px)\n",
        today_top,arrow_top,abs(today_top-arrow_top),month_bottom,grid_bottom,abs(month_bottom-grid_bottom),abs(month_bottom-638));
    thingsos_draw_nav(font,&canvas,0);
    CHECK(write_preview(output,"home-preview-full.pgm"));

    thingsos_home_book_t long_book = {"一本书名很长的书一本书名很长的书一本书名很长的书一本书名很长的书", "", "TXT", 0, 0, 37, true};
    memset(pixels,255,sizeof(pixels));
    thingsos_home_draw(font,&canvas,&stats,&long_book,1,true,sample_cover,font);
    CHECK(ink_edge(236,147,356,49,true)>=0 && shade_box(596,147,18,49,255));
    CHECK(shade_box(596,690,18,48,255));
    CHECK(shade_box(450,311,180,35,255));
    CHECK(ink_edge(236,257,394,30,true)>=0 && shade_box(320,257,310,30,255));
    CHECK(shade_box(54,692,56,76,255) && shade_box(128,748,464,24,255));
    CHECK(ink_edge(54,805,576,65,true)>=0 && shade_box(128,886,464,100,255));
    CHECK(!font->errors);

    CHECK(!thingsos_home_stats_build(&records,0,0,&stats));
    memset(pixels,255,sizeof(pixels));
    thingsos_home_draw(font,&canvas,&stats,NULL,0,false,NULL,NULL);
    CHECK(ink_edge(54,200,540,110,true)>=0 && ink_edge(54,805,576,65,true)>=0);
    for (unsigned i = 0; i < 84; ++i) CHECK(shade_box(311+(int)(i/7)*27,454+(int)(i%7)*27,22,22,238));
    thingsos_draw_nav(font,&canvas,0);
    CHECK(!font->errors && write_preview(output,"home-preview-empty.pgm"));
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3) return 1;
#ifdef _WIN32
    if (_putenv_s("TZ","CST-8")) return 1;
    _tzset();
#else
    if (setenv("TZ","CST-8",1)) return 1;
    tzset();
#endif
    if (!check_stats() || !check_hits()) return 1;
    char font_path[1024];
    int n = snprintf(font_path,sizeof(font_path),"%s/default.ttf",argv[1]);
    thingsos_font_t font = {0};
    if (n<0 || (size_t)n>=sizeof(font_path) || !thingsos_font_file(&font,font_path)) return 1;
    bool valid = check_drawing(&font,argv[2]);
    thingsos_font_close(&font);
    if (!valid) return 1;
    puts("PASS: home aggregation, hit rectangles, real-font rendering and previews");
    return 0;
}
