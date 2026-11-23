// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint32_t chapter, offset; } thingsos_page_position_t;
/* Finds the containing page, including restored offsets inside a page. */
size_t thingsos_page_find(const thingsos_page_position_t*, size_t, uint32_t, uint32_t);
typedef size_t (*thingsos_page_advance_fn)(void* context, size_t start);
typedef void (*thingsos_page_yield_fn)(void* context);
/* Find a strictly earlier page within this chapter. The index must use the
   current layout. On success, history contains earlier starts in ascending
   order with the nearest one last. On failure, history_count is zero. */
bool thingsos_chapter_prev_page(
    const thingsos_page_position_t* pages, size_t page_count,
    unsigned indexed_chapters, size_t indexed_offset, unsigned chapter, size_t target_start,
    thingsos_page_advance_fn advance, thingsos_page_yield_fn yield_fn, void* context,
    size_t* out_prev, size_t* history, size_t history_cap, size_t* history_count);

enum {
    THINGSOS_LAYOUT_TEXT = 34,
    THINGSOS_LAYOUT_SECTIONED_TEXT = 35,
    THINGSOS_LAYOUT_EPUB = 36
};
uint64_t thingsos_page_layout_key(unsigned version, unsigned px, unsigned line, unsigned margin,
                                 unsigned indent, unsigned width, unsigned height);
unsigned thingsos_book_percent(size_t, size_t);
size_t thingsos_page_percent(unsigned, size_t);
bool thingsos_page_cache_load(const char*, uint64_t, thingsos_page_position_t**, size_t*);
bool thingsos_page_cache_save(const char*, uint64_t, const thingsos_page_position_t*, size_t);
/* Only whole completed chapters are checkpointed. A partial index is never
   accepted by the full-index loader. The caller resumes at completed_chapters. */
bool thingsos_page_checkpoint_load(const char*, uint64_t, unsigned, thingsos_page_position_t**, size_t*, unsigned*);
bool thingsos_page_checkpoint_save(const char*, uint64_t, unsigned, const thingsos_page_position_t*, size_t, unsigned);
