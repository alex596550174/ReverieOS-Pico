// SPDX-License-Identifier: Apache-2.0
#include "thingsos/reader_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e) do { \
    if (!(e)) { fprintf(stderr, "reader UI line %d: %s\n", __LINE__, #e); return false; } \
} while (0)

enum { BUTTON_HEIGHT = 68, MAX_BUTTON_WIDTH = 574, PORTRAIT_WIDTH = 684, LANDSCAPE_WIDTH = 1216 };
typedef struct {
    int left, right;
    thingsos_menu_action_t action;
    const char* label;
} button_t;

static const button_t portrait_first[] = {
    {36, 230, THINGSOS_MENU_CHAPTERS, "目录"},
    {245, 439, THINGSOS_MENU_ADD_BOOKMARK, "添加书签"},
    {454, 648, THINGSOS_MENU_BOOKMARKS, "书签"},
};
static const button_t portrait_local[] = {
    {36, 328, THINGSOS_MENU_JUMP, "跳转"},
    {356, 648, THINGSOS_MENU_MORE, "更多"},
};
static const button_t portrait_weidu[] = {
    {36, 230, THINGSOS_MENU_JUMP, "跳转"},
    {245, 439, THINGSOS_MENU_NOTES, "划线"},
    {454, 648, THINGSOS_MENU_MORE, "更多"},
};
static const button_t landscape_first[] = {
    {24, 400, THINGSOS_MENU_CHAPTERS, "目录"},
    {420, 796, THINGSOS_MENU_ADD_BOOKMARK, "添加书签"},
    {816, 1192, THINGSOS_MENU_BOOKMARKS, "书签"},
};
static const button_t landscape_local[] = {
    {24, 598, THINGSOS_MENU_JUMP, "跳转"},
    {618, 1192, THINGSOS_MENU_MORE, "更多"},
};
static const button_t landscape_weidu[] = {
    {24, 400, THINGSOS_MENU_JUMP, "跳转"},
    {420, 796, THINGSOS_MENU_NOTES, "划线"},
    {816, 1192, THINGSOS_MENU_MORE, "更多"},
};

static thingsos_menu_action_t hit(bool landscape, bool weidu, int x, int y) {
    if (landscape) return thingsos_reader_menu_hit_landscape(x, y, NULL, weidu);
    if (weidu) return thingsos_reader_menu_hit_weidu(x, y, NULL);
    return thingsos_reader_menu_hit(x, y, NULL);
}

static bool check_label(thingsos_font_t* font, const thingsos_canvas_t* canvas,
                        const button_t* button, int top) {
    int width = button->right - button->left;
    unsigned char pixels[MAX_BUTTON_WIDTH * BUTTON_HEIGHT];
    memset(pixels, 255, (size_t)width * BUTTON_HEIGHT);
    thingsos_canvas_t label = { pixels, width, BUTTON_HEIGHT, false };
    CHECK(thingsos_font_size(font, 28));
    thingsos_label_centered(font, &label, button->label, 0, 45, width);
    unsigned ink = 0;
    for (int y = 2; y < BUTTON_HEIGHT - 2; ++y) {
        const unsigned char* actual = canvas->pixels + (size_t)(top + y) * canvas->width + button->left;
        const unsigned char* expected = pixels + (size_t)y * width;
        if (memcmp(actual + 2, expected + 2, (size_t)(width - 4))) {
            fprintf(stderr, "Unexpected menu label at (%d, %d): %s\n", button->left, top, button->label);
            return false;
        }
        for (int x = 2; x < width - 2; ++x) ink += actual[x] < 200;
    }
    CHECK(ink > 0);
    return true;
}

static bool check_row(const thingsos_canvas_t* canvas, bool landscape, bool weidu,
                      const button_t* buttons, size_t count, int top, int touch_end, int band_end) {
    for (int y = top; y < band_end; ++y) {
        const unsigned char* pixels = canvas->pixels + (size_t)y * canvas->width;
        size_t index = 0;
        for (int x = 0; x < canvas->width; ++x) {
            if (index < count && x == buttons[index].right) ++index;
            bool in_column = index < count && x >= buttons[index].left;
            thingsos_menu_action_t expected = in_column && y < touch_end ? buttons[index].action : THINGSOS_MENU_NONE;
            if (hit(landscape, weidu, x, y) != expected) {
                fprintf(stderr, "Wrong menu action at (%d, %d), landscape=%d weidu=%d\n", x, y, landscape, weidu);
                return false;
            }
            if (!in_column || y >= top + BUTTON_HEIGHT) {
                CHECK(pixels[x] == 255);
            } else if (x < buttons[index].left + 2 || x >= buttons[index].right - 2 ||
                       y < top + 2 || y >= top + BUTTON_HEIGHT - 2) {
                CHECK(pixels[x] == 0);
            }
        }
    }
    return true;
}

static bool check_menu(thingsos_font_t* font, unsigned char* pixels, bool landscape, bool weidu) {
    int width = landscape ? LANDSCAPE_WIDTH : PORTRAIT_WIDTH;
    int height = landscape ? PORTRAIT_WIDTH : LANDSCAPE_WIDTH;
    int first_top = landscape ? 488 : 1044;
    int second_top = landscape ? 586 : 1132;
    const button_t* first = landscape ? landscape_first : portrait_first;
    const button_t* second = landscape ? (weidu ? landscape_weidu : landscape_local)
                                       : (weidu ? portrait_weidu : portrait_local);
    size_t second_count = weidu ? 3 : 2;
    memset(pixels, 127, (size_t)width * height);
    thingsos_canvas_t canvas = { pixels, width, height, false };
    thingsos_reader_style_t style = { 46, 1, 1, 1, 0, true, weidu };
    thingsos_draw_reader_menu(font, &canvas, style);
    CHECK(font->errors == 0);
    CHECK(check_row(&canvas, landscape, weidu, first, 3, first_top, first_top + BUTTON_HEIGHT, second_top));
    /* The second row keeps its existing larger touch area all the way to the screen edge. */
    CHECK(check_row(&canvas, landscape, weidu, second, second_count, second_top, height, height));
    CHECK(hit(landscape, weidu, -1, second_top) == THINGSOS_MENU_NONE);
    CHECK(hit(landscape, weidu, width, second_top) == THINGSOS_MENU_NONE);
    CHECK(hit(landscape, weidu, 100, -1) == THINGSOS_MENU_NONE);
    CHECK(hit(landscape, weidu, 100, height) == THINGSOS_MENU_NONE);
    for (size_t i = 0; i < 3; ++i) CHECK(check_label(font, &canvas, &first[i], first_top));
    for (size_t i = 0; i < second_count; ++i) CHECK(check_label(font, &canvas, &second[i], second_top));
    CHECK(font->errors == 0);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fputs("Expected the native-fixtures directory as the only argument.\n", stderr);
        return 1;
    }
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/default.ttf", argv[1]);
    thingsos_font_t font = {0};
    if (length < 0 || (size_t)length >= sizeof(path) || !thingsos_font_file(&font, path)) {
        fprintf(stderr, "Could not load default.ttf from %s\n", argv[1]);
        return 1;
    }
    unsigned char* pixels = malloc((size_t)PORTRAIT_WIDTH * LANDSCAPE_WIDTH);
    bool ok = pixels && check_menu(&font, pixels, false, false) && check_menu(&font, pixels, false, true) &&
                        check_menu(&font, pixels, true, false) && check_menu(&font, pixels, true, true);
    free(pixels);
    thingsos_font_close(&font);
    if (ok) puts("PASS: reader menu touch areas, spacing, and exact labels in four modes using default.ttf");
    return ok ? 0 : 1;
}
