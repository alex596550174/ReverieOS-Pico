// SPDX-License-Identifier: Apache-2.0
#include "thingsos/content.h"
#include "thingsos/graphics.h"
#include "thingsos/markdown.h"
#include "thingsos/pagination.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { WIDTH = 480, HEIGHT = 550, LEFT = 24, TOP = 30 };
static unsigned char pixels[WIDTH * HEIGHT], sliced[WIDTH * HEIGHT];
static thingsos_text_hits_t full_hits, sliced_hits;
static thingsos_font_t font;
static thingsos_chapter_t chapter;
static char member_path[1024];

/* A stored XHTML member with a real ZIP local header and CRC. Supplying the
   spine directly keeps the parser cases independent of OPF and TOC parsing. */
static void chapter_fixture(thingsos_epub_t* book, const char* xml, unsigned sections) {
    static const char name[] = "OPS/chapter.xhtml";
    const size_t length = strlen(xml);
    unsigned char header[30] = {0x50, 0x4b, 0x03, 0x04, 20};
    header[26] = sizeof(name) - 1;
    book->zip.file = fopen(member_path, "wb+");
    book->zip.entries = calloc(1, sizeof(*book->zip.entries));
    book->items = calloc(1, sizeof(*book->items));
    CHECK(book->zip.file && book->zip.entries && book->items && thingsos_epub_reserve_chapters(book, sections));
    book->zip.count = book->item_count = 1;
    book->zip.size = sizeof(header) + sizeof(name) - 1 + length;
    thingsos_zip_entry_t* entry = book->zip.entries;
    strcpy(entry->name, name);
    entry->bytes = entry->compressed = (uint32_t)length;
    entry->crc = (uint32_t)crc32(0, (const unsigned char*)xml, (uInt)length);
    CHECK(fwrite(header, sizeof(header), 1, book->zip.file) == 1);
    CHECK(fwrite(name, 1, sizeof(name) - 1, book->zip.file) == sizeof(name) - 1);
    CHECK(fwrite(xml, 1, length, book->zip.file) == length && !fflush(book->zip.file));
    book->items[0] = (thingsos_epub_item_t){entry->name, THINGSOS_EPUB_XHTML, 0};
    book->spine_count = sections;
    for (unsigned i = 0; i < sections; ++i) snprintf(book->chapter_names[i], 160, "第 %u 章", i + 1);
}

typedef struct {
    unsigned size, spacing, indent;
    int width, height;
    bool epub, vertical;
} layout_t;
static const layout_t standard = {30, 125, 60, 420, 430, true, false};

static size_t layout_page(const layout_t* style, size_t start, thingsos_canvas_t* canvas,
                          thingsos_text_hits_t* hits, thingsos_picture_fn picture, void* context) {
    CHECK(thingsos_font_size(&font, style->size));
    return thingsos_text_page_layout_flow(&font, canvas, &chapter, start, LEFT, TOP, style->width, style->height,
        style->spacing, style->indent, style->epub, picture, context, hits, style->vertical, 16);
}

static size_t paint_page(const layout_t* style, size_t start) {
    thingsos_canvas_t canvas = {pixels, WIDTH, HEIGHT};
    memset(pixels, 255, sizeof(pixels));
    memset(&full_hits, 0, sizeof(full_hits));
    return layout_page(style, start, &canvas, &full_hits, NULL, NULL);
}

static bool row_budget(void* context) {
    unsigned* rows = context;
    if (!*rows) return false;
    --*rows;
    return true;
}

static void check_resume(const layout_t* style, size_t start, size_t end) {
    CHECK(end == layout_page(style, start, NULL, NULL, NULL, NULL));
    memset(sliced, 255, sizeof(sliced));
    memset(&sliced_hits, 0, sizeof(sliced_hits));
    thingsos_canvas_t canvas = {sliced, WIDTH, HEIGHT};
    thingsos_text_cursor_t cursor = {0};
    bool done = false;
    size_t offset = start;
    unsigned slices = 0;
    while (!done) {
        unsigned budget = 1;
        offset = thingsos_text_page_resume_flow(&font, &canvas, &chapter, start, LEFT, TOP,
            style->width, style->height, style->spacing, style->indent, style->epub, NULL, NULL,
            &sliced_hits, row_budget, &budget, &done, &cursor, style->vertical, 16);
        CHECK(++slices < 100 && (done || cursor.active));
    }
    CHECK(slices > 1 && !cursor.active && offset == end);
    CHECK(!memcmp(pixels, sliced, sizeof(pixels)) && !memcmp(&full_hits, &sliced_hits, sizeof(full_hits)));
}

static const thingsos_glyph_hit_t* hit_at(size_t offset) {
    for (size_t i = 0; i < full_hits.glyph_count; ++i)
        if (full_hits.glyphs[i].offset == offset) return full_hits.glyphs + i;
    return NULL;
}

static int ink_edge(int first, int past, bool top) {
    for (int y = top ? first : past - 1; y >= first && y < past; y += top ? 1 : -1)
        for (int x = LEFT; x < WIDTH; ++x)
            if (pixels[y * WIDTH + x] < 200) return y;
    return -1;
}

static void check_opening_heading(void) {
    thingsos_epub_t book = {0};
    const char title[] = "第一章 晨雾";
    chapter_fixture(&book, "<html><body><h1>第一章 晨雾</h1><p>　正文从这里开始。</p></body></html>", 1);
    CHECK(thingsos_epub_chapter(&book, 0, &chapter));
    CHECK(chapter.heading && chapter.heading_start == 0 && chapter.heading_end == strlen(title));
    CHECK(!strcmp(book.chapter_names[0], title));
    size_t body = (size_t)(strstr(chapter.text, "正文") - chapter.text);
    size_t end = paint_page(&standard, 0);
    const thingsos_glyph_hit_t* title_hit = hit_at(0), *body_hit = hit_at(body);
    CHECK(end == chapter.bytes && title_hit && body_hit);
    CHECK(title_hit->x == LEFT && title_hit->h - body_hit->h == 9 && title_hit->h == 49);
    CHECK(title_hit->y == TOP + 30 && body_hit->x == LEFT + 60);
    int title_top = ink_edge(TOP, body_hit->y, true), title_bottom = ink_edge(TOP, body_hit->y, false);
    int body_top = ink_edge(body_hit->y, TOP + standard.height, true);
    CHECK(title_top >= TOP + 30 && title_top <= TOP + 40 && title_bottom >= title_top);
    CHECK(body_top - title_bottom - 1 >= 25 && body_top - title_bottom - 1 <= 48);
    check_resume(&standard, 0, end);
    layout_t large = standard;
    large.size = 66;
    CHECK(paint_page(&large, 0) > 0 && hit_at(0) && hit_at(0)->h == 80); /* 70px title cap. */
    thingsos_chapter_close(&chapter);
    thingsos_epub_close(&book);

    chapter_fixture(&book, "<html><body><h1>标题</h1>正文</body></html>", 1);
    CHECK(thingsos_epub_chapter(&book, 0, &chapter));
    CHECK(chapter.heading && !strcmp(chapter.text + chapter.heading_end, "正文"));
    layout_t shallow = standard;
    shallow.height = 72;
    CHECK(paint_page(&shallow, 0) == chapter.heading_end);
    CHECK(paint_page(&shallow, chapter.heading_end) == chapter.bytes);
    CHECK(hit_at(chapter.heading_end) && hit_at(chapter.heading_end)->x == LEFT + 60);
    thingsos_chapter_close(&chapter);
    thingsos_epub_close(&book);
}

static void check_heading_detection(void) {
    thingsos_epub_t book = {0};
    chapter_fixture(&book, "<html><body><p>导语</p><h3>小节标题</h3><p>正文</p></body></html>", 1);
    CHECK(thingsos_epub_chapter(&book, 0, &chapter) && !chapter.heading);
    CHECK(!strcmp(book.chapter_names[0], "第 1 章") && !strcmp(chapter.text, "导语\n小节标题\n正文"));
    CHECK(paint_page(&standard, 0) == chapter.bytes);
    const thingsos_glyph_hit_t* section = hit_at(strlen("导语\n"));
    CHECK(section && section->h == 40 && ink_edge(section->y, section->y + section->h, true) >= 0);
    thingsos_chapter_close(&chapter);
    thingsos_epub_close(&book);

    chapter_fixture(&book, "<html><head><title>Metadata</title></head><body>"
        "<p>导语</p><h3>小节标题</h3><p>正文</p></body></html>", 1);
    CHECK(thingsos_epub_chapter(&book, 0, &chapter) && !chapter.heading && !strcmp(book.chapter_names[0], "Metadata"));
    thingsos_chapter_close(&chapter);
    thingsos_epub_close(&book);

    chapter_fixture(&book, "<html><body>旧片段<div id='part'> \t\r\n&#160;　"
        "<img src='front.png'/><h1/><h1> &#160;　</h1><h2>片段标题</h2><p>正文</p></div>"
        "<h2 id='following'>下一节</h2></body></html>", 2);
    strcpy(book.chapter_fragments[0], "part");
    strcpy(book.chapter_fragments[1], "following");
    CHECK(thingsos_epub_chapter(&book, 0, &chapter) && chapter.heading);
    CHECK(chapter.source_start == strlen("旧片段") && chapter.image_count == 1 && !chapter.cover_image);
    CHECK(chapter.images[0].offset < chapter.heading_start && chapter.text[chapter.images[0].offset] == '\n');
    CHECK(chapter.heading_end - chapter.heading_start == strlen("片段标题"));
    CHECK(!strcmp(book.chapter_names[0], "片段标题") && !strstr(chapter.text, "下一节"));
    thingsos_chapter_close(&chapter);
    CHECK(thingsos_epub_chapter(&book, 1, &chapter) && chapter.heading && !strcmp(book.chapter_names[1], "下一节"));
    thingsos_chapter_close(&chapter);
    thingsos_epub_close(&book);
}

static void check_wrapping(void) {
    thingsos_epub_t book = {0};
    chapter_fixture(&book, "<html><body><h1>第一章晨雾晨雾晨雾晨雾晨雾晨雾晨雾晨雾"
        "晨雾晨雾晨雾晨雾晨雾晨雾晨雾晨雾晨雾晨雾晨雾晨雾晨雾晨雾</h1>"
        "<p>正文从这里开始。</p></body></html>", 1);
    CHECK(thingsos_epub_chapter(&book, 0, &chapter) && chapter.heading);
    layout_t style = standard;
    style.width = 280;
    size_t end = paint_page(&style, 0);
    CHECK(end == chapter.bytes);
    int last_y = -1;
    unsigned rows = 0;
    for (size_t i = 0; i < full_hits.glyph_count && full_hits.glyphs[i].offset < chapter.heading_end; ++i) {
        const thingsos_glyph_hit_t* glyph = full_hits.glyphs + i;
        CHECK(glyph->x >= LEFT && glyph->x + glyph->w <= LEFT + style.width);
        if (glyph->y != last_y) {
            CHECK(glyph->x == LEFT && (last_y < 0 || glyph->y - last_y >= 48));
            if (last_y >= 0) CHECK(ink_edge(last_y + 44, glyph->y, true) == -1);
            last_y = glyph->y;
            ++rows;
        }
    }
    CHECK(rows >= 3);
    check_resume(&style, 0, end);
    style.height = 114;
    size_t count = 0;
    for (size_t start = 0; start < chapter.bytes; start = end) {
        end = paint_page(&style, start);
        CHECK(++count < 20 && end > start && end == layout_page(&style, start, NULL, NULL, NULL, NULL));
        if (start && start < chapter.heading_end) CHECK(hit_at(start) && hit_at(start)->y == TOP);
    }
    CHECK(count >= 3);
    style = standard;
    style.vertical = true;
    end = paint_page(&style, 0);
    CHECK(end > 0 && hit_at(0) && hit_at(0)->w == 39);
    check_resume(&style, 0, end);
    for (size_t start = end; start < chapter.bytes; start = end) {
        end = paint_page(&style, start);
        CHECK(end > start && end == layout_page(&style, start, NULL, NULL, NULL, NULL));
    }
    thingsos_chapter_close(&chapter);
    thingsos_epub_close(&book);
}

typedef struct { int x, y, width, height; unsigned count; } picture_t;
static bool record_picture(void* context, const thingsos_inline_t* img, thingsos_canvas_t* canvas,
                           int x, int y, int width, int height) {
    (void)img;
    picture_t* result = context;
    *result = (picture_t){x, y, width, height, result->count + 1};
    thingsos_rect(canvas, x, y, width, height, 150);
    return true;
}

static void check_cover_and_cached_heading(const char* fixtures) {
    thingsos_epub_t book = {0};
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/reading-cover-local.epub", fixtures) < (int)sizeof(path));
    CHECK(thingsos_epub_open(&book, path) && thingsos_epub_chapter(&book, 0, &chapter));
    CHECK(chapter.heading && chapter.cover_image == 1);
    picture_t picture = {0};
    thingsos_canvas_t canvas = {pixels, WIDTH, HEIGHT};
    memset(pixels, 255, sizeof(pixels));
    memset(&full_hits, 0, sizeof(full_hits));
    CHECK(layout_page(&standard, 0, &canvas, &full_hits, record_picture, &picture) == chapter.bytes);
    CHECK(layout_page(&standard, 0, NULL, NULL, record_picture, &picture) == chapter.bytes);
    CHECK(!full_hits.glyph_count && picture.count == 1 && picture.width == standard.width &&
          picture.y == TOP + 12 && picture.height == standard.height - 24);
    thingsos_chapter_close(&chapter);
    thingsos_epub_close(&book);

    chapter_fixture(&book, "<html><body><h1>标题</h1><p>　</p><img src='figure.png'/></body></html>", 1);
    CHECK(thingsos_epub_chapter(&book, 0, &chapter) && chapter.heading && !chapter.cover_image);
    CHECK(chapter.image_count == 1 && chapter.images[0].offset > chapter.heading_end);
    picture = (picture_t){0};
    memset(pixels, 255, sizeof(pixels));
    memset(&full_hits, 0, sizeof(full_hits));
    CHECK(layout_page(&standard, 0, &canvas, &full_hits, record_picture, &picture) == chapter.bytes);
    CHECK(hit_at(0) && picture.count == 1 && picture.y > hit_at(0)->y + hit_at(0)->h);
    thingsos_chapter_close(&chapter);
    thingsos_epub_close(&book);

    chapter_fixture(&book, "<html><body><h1>标题</h1><p>书名</p><h2>标题</h2><p>正文</p></body></html>", 1);
    strcpy(book.opf, "OPS/book.opf");
    strcpy(book.title, "书名");
    book.weidu_cache = true;
    CHECK(thingsos_epub_chapter(&book, 0, &chapter) && chapter.heading && chapter.hidden_end > chapter.hidden_start);
    CHECK(!strcmp(chapter.text, "标题\n书名\n标题\n正文") && chapter.heading_end == strlen("标题"));
    CHECK(paint_page(&standard, 0) == chapter.bytes && hit_at(0));
    for (size_t i = 0; i < full_hits.glyph_count; ++i)
        CHECK(full_hits.glyphs[i].offset < chapter.hidden_start || full_hits.glyphs[i].offset >= chapter.hidden_end);
    CHECK(hit_at(chapter.hidden_end) && hit_at(chapter.hidden_end)->x == LEFT + 60);
    check_resume(&standard, 0, chapter.bytes);
    memcpy(sliced, pixels, sizeof(sliced));
    thingsos_chapter_close(&chapter);
    thingsos_epub_close(&book);
    chapter_fixture(&book, "<html><body><h1>标题</h1><p>正文</p></body></html>", 1);
    CHECK(thingsos_epub_chapter(&book, 0, &chapter) && paint_page(&standard, 0) == chapter.bytes);
    CHECK(!memcmp(sliced, pixels, sizeof(pixels)));
    thingsos_chapter_close(&chapter);
    thingsos_epub_close(&book);
}

typedef struct { layout_t layout; unsigned advances, yields; } replay_t;
static size_t advance_page(void* context, size_t start) {
    replay_t* replay = context;
    ++replay->advances;
    return layout_page(&replay->layout, start, NULL, NULL, NULL, NULL);
}
static void yield_page(void* context) { ++((replay_t*)context)->yields; }
static size_t stall_page(void* context, size_t start) { (void)context; return start; }
static size_t byte_page(void* context, size_t start) {
    ++((replay_t*)context)->advances;
    return start + 1;
}

static void collect_pages(const layout_t* style, size_t* starts, size_t count) {
    starts[0] = 0;
    for (size_t i = 1; i < count; ++i) {
        starts[i] = layout_page(style, starts[i-1], NULL, NULL, NULL, NULL);
        CHECK(starts[i] > starts[i-1] && starts[i] < chapter.bytes);
    }
}

static void check_prev_page(const char* work) {
    const char paragraph[] = "山路沿着树林延伸。晨雾渐渐散开，溪水穿过山谷。\n";
    char* text = malloc(sizeof(paragraph) * 120);
    CHECK(text);
    char* out = text;
    for (unsigned i = 0; i < 120; ++i) {
        memcpy(out, paragraph, sizeof(paragraph) - 1);
        out += sizeof(paragraph) - 1;
    }
    *out = 0;
    chapter.text = text;
    chapter.bytes = (size_t)(out - text);
    replay_t replay = {.layout = {30, 125, 0, 280, 220, true, false}};
    size_t starts[8], previous = SIZE_MAX, history[8], count = 0;
    collect_pages(&replay.layout, starts, 8);
    const unsigned chapter_index = 3;
    CHECK(thingsos_chapter_prev_page(NULL, 0, 0, 0, chapter_index, starts[2] + 3,
        advance_page, yield_page, &replay, &previous, history, 8, &count));
    CHECK(previous == starts[2] && count == 2 && history[0] == 0 && history[1] == starts[1]);

    replay.advances = replay.yields = 0;
    CHECK(thingsos_chapter_prev_page(NULL, 0, 0, 0, chapter_index, starts[4],
        advance_page, yield_page, &replay, &previous, history, 8, &count));
    CHECK(previous == starts[3] && count == 3 && replay.advances == 4);
    for (size_t i = 3; i > 0; --i) {
        CHECK(count && history[count-1] < previous);
        previous = history[--count];
        CHECK(previous == starts[i-1]);
    }
    CHECK(!count && !previous && !thingsos_chapter_prev_page(NULL, 0, 0, 0, chapter_index, previous,
        advance_page, yield_page, &replay, &previous, history, 8, &count));
    CHECK(replay.advances == 4);

    thingsos_page_position_t partial[] = {{2,0}, {3,0}, {3,(uint32_t)starts[1]}, {3,(uint32_t)starts[2]}};
    replay.advances = replay.yields = 0;
    CHECK(thingsos_chapter_prev_page(partial, 4, 3, starts[3], chapter_index, starts[5],
        advance_page, yield_page, &replay, &previous, history, 2, &count));
    CHECK(previous == starts[4] && replay.advances == 3 && count == 2 && history[0] == starts[2] && history[1] == starts[3]);
    CHECK(thingsos_chapter_prev_page(partial, 4, 3, starts[3], chapter_index, starts[2],
        NULL, NULL, NULL, &previous, history, 8, &count));
    CHECK(previous == starts[1] && count == 1 && !history[0]);
    CHECK(thingsos_chapter_prev_page(partial, 4, 4, 0, chapter_index, starts[2] + 3,
        NULL, NULL, NULL, &previous, NULL, 0, NULL) && previous == starts[2]);
    const thingsos_page_position_t offset_only[] = {{2,0}, {3,(uint32_t)starts[1]}};
    CHECK(thingsos_chapter_prev_page(offset_only, 2, 4, 0, chapter_index, starts[1],
        advance_page, NULL, &replay, &previous, NULL, 0, NULL) && previous == 0);

    size_t old_position = starts[6];
    replay.layout.size = 46;
    replay.layout.spacing = 175;
    replay.layout.indent = 92;
    size_t new_start = 0, next;
    while ((next = layout_page(&replay.layout, new_start, NULL, NULL, NULL, NULL)) < old_position) {
        CHECK(next > new_start);
        new_start = next;
    }
    CHECK(thingsos_chapter_prev_page(NULL, 0, 0, 0, chapter_index, old_position,
        advance_page, yield_page, &replay, &previous, history, 8, &count) && previous == new_start);
    while (previous) {
        size_t from = previous;
        CHECK(thingsos_chapter_prev_page(NULL, 0, 0, 0, chapter_index, from,
            advance_page, yield_page, &replay, &previous, NULL, 0, NULL));
        CHECK(previous < from && layout_page(&replay.layout, previous, NULL, NULL, NULL, NULL) == from);
    }
    /* A section offset behaves like a restored source position within a page. */
    size_t section = (sizeof(paragraph) - 1) * 12;
    CHECK(thingsos_chapter_prev_page(NULL, 0, 0, 0, chapter_index, section,
        advance_page, yield_page, &replay, &previous, history, 8, &count));
    CHECK(previous < section && layout_page(&replay.layout, previous, NULL, NULL, NULL, NULL) >= section);

    replay.layout = (layout_t){30, 145, 0, 280, 220, false, false};
    for (unsigned markdown = 0; markdown < 2; ++markdown) {
        if (markdown) {
            char path[1024];
            CHECK(snprintf(path, sizeof(path), "%s/heading-prev-test.md", work) < (int)sizeof(path));
            memcpy(chapter.text, "#  ", 3); /* Replace one full UTF-8 character with the heading marker. */
            CHECK(thingsos_markdown_prepare(path, &chapter));
        }
        collect_pages(&replay.layout, starts, 8);
        CHECK(thingsos_chapter_prev_page(NULL, 0, 0, 0, 0, starts[5],
            advance_page, yield_page, &replay, &previous, history, 8, &count));
        CHECK(previous == starts[4]);
    }
    CHECK(!remove(chapter.markdown_index));
    thingsos_chapter_close(&chapter);

    previous = SIZE_MAX;
    CHECK(!thingsos_chapter_prev_page(NULL, 0, 0, 0, 3, 10, stall_page, NULL, NULL, &previous, history, 8, &count));
    CHECK(previous == SIZE_MAX && !count);
    replay.advances = replay.yields = 0;
    CHECK(!thingsos_chapter_prev_page(NULL, 0, 0, 0, 3, 65537, byte_page, yield_page, &replay,
        &previous, history, 8, &count));
    CHECK(replay.advances == 65536 && replay.yields == 16384 && previous == SIZE_MAX && !count);
}

static void check_cache_version(const char* work) {
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/heading-prev-test.pages", work) < (int)sizeof(path));
    uint64_t old_key = thingsos_page_layout_key(20, 30, 1, 1, 2, 684, 1040);
    uint64_t new_key = thingsos_page_layout_key(THINGSOS_LAYOUT_EPUB, 30, 1, 1, 2, 684, 1040);
    CHECK(old_key != new_key && (unsigned)THINGSOS_LAYOUT_TEXT > 19 && (unsigned)THINGSOS_LAYOUT_SECTIONED_TEXT > 21);
    const thingsos_page_position_t pages[] = {{0,0}, {0,100}};
    thingsos_page_position_t* loaded;
    size_t count;
    unsigned completed;
    CHECK(thingsos_page_cache_save(path, old_key, pages, 2));
    CHECK(thingsos_page_cache_load(path, old_key, &loaded, &count) && count == 2);
    free(loaded);
    CHECK(!thingsos_page_cache_load(path, new_key, &loaded, &count) && !loaded && !count);
    CHECK(thingsos_page_checkpoint_save(path, old_key, 2, pages, 2, 1));
    CHECK(thingsos_page_checkpoint_load(path, old_key, 2, &loaded, &count, &completed) && count == 2 && completed == 1);
    free(loaded);
    CHECK(!thingsos_page_checkpoint_load(path, new_key, 2, &loaded, &count, &completed) && !loaded && !count && !completed);
    CHECK(!remove(path));
}

static bool stress_picture(void* context,const thingsos_inline_t* image,
                           thingsos_canvas_t* canvas,int x,int y,int w,int h) {
    (void)image;
    ++*(unsigned*)context;
    thingsos_rect(canvas,x,y,w,h,180);
    return true;
}

static void check_full_chapter_edges(void) {
    static const char mixed[]="测试章节标题\r\n隐藏文本\n正文和脚注1\r\n\n图后继续阅读。\n";
    static const char* const tokens[]={"，","……","——"};
    static unsigned char frame[684*1216], parts[684*1216];
    char repeated[8192];
    for(unsigned kind=0;kind<8;++kind) {
        const char* source=kind==0?"":kind==1?" \t　\r\n\r\n　\t ":kind==2?mixed:kind==7?"\r\n\r\n\r\n":repeated;
        if(kind>=3 && kind<=5) {
            size_t n=strlen(tokens[kind-3]);
            for(size_t i=0;i<100;++i)memcpy(repeated+n*i,tokens[kind-3],n);
            repeated[n*100]=0;
        }
        if(kind==6){memset(repeated,'W',sizeof(repeated)-1);repeated[sizeof(repeated)-1]=0;}
        chapter=(thingsos_chapter_t){.text=(char*)source,.bytes=strlen(source)};
        thingsos_footnote_t note={0};
        if(kind==2) {
            chapter.heading=true;chapter.heading_end=strlen("测试章节标题");
            chapter.hidden_start=(size_t)(strstr(source,"隐藏文本")-source);
            chapter.hidden_end=chapter.hidden_start+strlen("隐藏文本\n");
            note.begin=(size_t)(strchr(source,'1')-source);note.end=note.begin+1;note.note_bytes=4;
            chapter.footnotes=&note;chapter.footnote_count=1;
            chapter.image_count=1;chapter.images[0].offset=(size_t)(strstr(source,"\n\n图")-source)+1;
            chapter.images[0].width=160;chapter.images[0].height=100;
        }
        for(unsigned variant=0;variant<4;++variant) {
            const int widths[]={24,36,54,36};
            int margin=widths[variant],w=variant==3?1216:684,h=variant==3?684:1216;
            thingsos_canvas_t canvas={frame,w,h}, sliced_canvas={parts,w,h};
            size_t p=0;
            unsigned pages=0, pictures=0;
            do {
                CHECK(++pages<=chapter.bytes+1);
                memset(frame,255,sizeof(frame));memset(parts,255,sizeof(parts));
                memset(&full_hits,0,sizeof(full_hits));memset(&sliced_hits,0,sizeof(sliced_hits));
                CHECK(thingsos_font_size(&font,42));
                int width=w-2*margin,height=h-176;
                size_t dry=thingsos_text_page_layout_flow(&font,NULL,&chapter,p,margin,90,width,height,145,0,
                    true,NULL,NULL,NULL,false,25);
                unsigned drawn=0,sliced_drawn=0;
                size_t end=thingsos_text_page_layout_flow(&font,&canvas,&chapter,p,margin,90,width,height,145,0,
                    true,stress_picture,&drawn,&full_hits,false,25);
                CHECK(dry==end && end<=chapter.bytes && (end>p || !chapter.bytes));
                thingsos_text_cursor_t cursor={0};
                bool complete=false;unsigned calls=0;
                size_t sliced_end=p;
                do {
                    unsigned budget=2;
                    sliced_end=thingsos_text_page_resume_flow(&font,&sliced_canvas,&chapter,p,margin,90,width,height,
                        145,0,true,stress_picture,&sliced_drawn,&sliced_hits,row_budget,&budget,&complete,&cursor,false,25);
                    CHECK(++calls<100);
                } while(!complete);
                CHECK(sliced_end==end && drawn==sliced_drawn && !memcmp(frame,parts,sizeof(frame)));
                CHECK(!memcmp(&full_hits,&sliced_hits,sizeof(full_hits)));
                pictures+=drawn;p=end;
            } while(p<chapter.bytes);
            if(kind==2)CHECK(pictures==1);
        }
    }
    chapter=(thingsos_chapter_t){0};
    puts("PASS: 32 full-chapter edge cases; draw, measure and sliced resume agree");
}

int main(int argc, char** argv) {
    CHECK(argc == 3);
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/default.ttf", argv[1]) < (int)sizeof(path));
    CHECK(snprintf(member_path, sizeof(member_path), "%s/heading-prev-test.member", argv[2]) < (int)sizeof(member_path));
    CHECK(thingsos_font_file(&font, path));
    check_opening_heading();
    check_heading_detection();
    check_wrapping();
    check_cover_and_cached_heading(argv[1]);
    check_prev_page(argv[2]);
    check_cache_version(argv[2]);
    check_full_chapter_edges();
    CHECK(!font.errors && !remove(member_path));
    thingsos_font_close(&font);
    puts("PASS: EPUB opening headings, horizontal and vertical resume, same-chapter backtracking and cache version");
    return 0;
}
