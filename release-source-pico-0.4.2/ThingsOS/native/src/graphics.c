// SPDX-License-Identifier: Apache-2.0
#include "thingsos/graphics.h"
#include "thingsos/markdown.h"
#include "thingsos/reader.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SYSTEM_H
#include FT_MODULE_H
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#ifdef ESP_PLATFORM
/* FreeType's default malloc competes with SD/LCD/Wi-Fi for internal SRAM.
   Its outline, interpreter and raster buffers can all live in PSRAM. */
static void* font_allocate(FT_Memory memory,long bytes) {
    (void)memory;
    return bytes>0?heap_caps_malloc((size_t)bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
}
static void font_release(FT_Memory memory,void* block) {(void)memory;heap_caps_free(block);}
static void* font_reallocate(FT_Memory memory,long old,long bytes,void* block) {
    (void)memory;(void)old;
    if(bytes<=0){heap_caps_free(block);return NULL;}
    return heap_caps_realloc(block,(size_t)bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
}
static struct FT_MemoryRec_ font_memory={NULL,font_allocate,font_release,font_reallocate};
#endif
static FT_Error font_library_new(FT_Library* library) {
#ifdef ESP_PLATFORM
    FT_Error error=FT_New_Library(&font_memory,library);
    if(!error){FT_Add_Default_Modules(*library);FT_Set_Default_Properties(*library);}
    return error;
#else
    return FT_Init_FreeType(library);
#endif
}
static void font_library_done(FT_Library library) {
#ifdef ESP_PLATFORM
    /* The shared, stateless memory record is not owned by FreeType. */
    FT_Done_Library(library);
#else
    FT_Done_FreeType(library);
#endif
}
typedef struct { uint32_t codepoint; unsigned size; int advance, ink_left, ink_right; bool monochrome; } metric_t;
#define METRIC_SLOTS 8192
typedef struct {
    uint32_t codepoint;
    unsigned size, width, rows, weight;
    int left, top, advance;
    unsigned char* pixels;
    bool monochrome;
    unsigned used;
} bitmap_t;
#define BITMAP_SLOTS 512
static void* cache_allocate(size_t bytes) {
#ifdef ESP_PLATFORM
    /* Small glyphs must never consume the internal DMA heap needed by SDMMC.
       Optional caches use PSRAM exclusively; exhaustion disables caching. */
    return heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}
static void* cache_zero(size_t bytes) {
    void* p=cache_allocate(bytes);
    if(p) memset(p,0,bytes);
    return p;
}
static uint64_t signature(const unsigned char* data, size_t n, uint64_t h) {
    for (size_t i=0;i<n;++i) h=(h^data[i])*1099511628211ULL;
    return h;
}
static uint32_t next(const char* p, size_t left, unsigned* length) {
    unsigned char a = (unsigned char)p[0];
    unsigned n = a < 128                  ? 1
                 : a >= 0xc2 && a < 0xe0  ? 2
                 : a >= 0xe0 && a < 0xf0  ? 3
                 : a >= 0xf0 && a <= 0xf4 ? 4
                                          : 0;
    *length = 1;
    if (!n || left < n) return 0xfffd;
    uint32_t cp = a & (n == 1 ? 127 : n == 2 ? 31 : n == 3 ? 15 : 7);
    for (unsigned i = 1; i < n; i++) {
        unsigned char b = (unsigned char)p[i];
        if ((b & 0xc0) != 0x80) return 0xfffd;
        cp = cp << 6 | (b & 63);
    }
    if ((n == 2 && cp < 128) || (n == 3 && cp < 2048) || (n == 4 && cp < 65536) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff))
        return 0xfffd;
    *length = n;
    return cp;
}
#define FONT_IO_BLOCK 4096u
#define FONT_IO_SLOTS 8u
typedef struct {
    FT_StreamRec stream;
    FILE* file;
    unsigned char* cache;
    struct { unsigned long base; unsigned bytes, stamp; } slots[FONT_IO_SLOTS];
    unsigned clock;
    thingsos_font_io_t stats;
} font_stream_t;
#ifndef ESP_PLATFORM
static bool font_host_no_cache;
void thingsos_font_host_uncached(bool value) {font_host_no_cache=value;}
#endif
static unsigned long font_read(FT_Stream stream,unsigned long offset,unsigned char* out,unsigned long count) {
    font_stream_t* s=stream->descriptor.pointer;
    if(offset>stream->size)return count?0:1;
    if(!count)return 0;
    if(!out || !s->file)return 0;
    ++s->stats.requests;
    if(count>stream->size-offset)count=stream->size-offset;
    if(!s->cache) {
        if(fseek(s->file,(long)offset,SEEK_SET))return 0;
        ++s->stats.disk_reads;
        size_t got=fread(out,1,count,s->file);s->stats.disk_bytes+=got;
        return (unsigned long)got;
    }
    unsigned long done=0;
    while(done<count) {
        unsigned long position=offset+done,base=position/FONT_IO_BLOCK*FONT_IO_BLOCK;
        unsigned slot=FONT_IO_SLOTS,oldest=0;
        for(unsigned i=0;i<FONT_IO_SLOTS;++i) {
            if(s->slots[i].bytes && s->slots[i].base==base){slot=i;break;}
            if(s->slots[i].stamp<s->slots[oldest].stamp)oldest=i;
        }
        if(slot==FONT_IO_SLOTS) {
            slot=oldest;s->slots[slot].bytes=0;
            unsigned long wanted=stream->size-base;if(wanted>FONT_IO_BLOCK)wanted=FONT_IO_BLOCK;
            if(fseek(s->file,(long)base,SEEK_SET))break;
            ++s->stats.disk_reads;
            size_t got=fread(s->cache+slot*FONT_IO_BLOCK,1,wanted,s->file);
            s->stats.disk_bytes+=got;
            if(got!=wanted)break;
            s->slots[slot].base=base;s->slots[slot].bytes=(unsigned)got;
        }else ++s->stats.cache_hits;
        if(!++s->clock) {
            for(unsigned i=0;i<FONT_IO_SLOTS;++i)s->slots[i].stamp=0;
            s->clock=1;
        }
        s->slots[slot].stamp=s->clock;
        unsigned within=(unsigned)(position-base);
        if(within>=s->slots[slot].bytes)break;
        unsigned long n=s->slots[slot].bytes-within;if(n>count-done)n=count-done;
        memcpy(out+done,s->cache+slot*FONT_IO_BLOCK+within,n);done+=n;
    }
    return done;
}
static void font_stream_close(FT_Stream stream) {
    font_stream_t* s=stream->descriptor.pointer;
    if(s->file){fclose(s->file);s->file=NULL;}
    free(s->cache);s->cache=NULL;
}
bool thingsos_font_io(const thingsos_font_t* f,thingsos_font_io_t* out) {
    if(!f || !f->face || !out)return false;
    FT_Stream stream=((FT_Face)f->face)->stream;
    if(!stream || stream->read!=font_read)return false;
    *out=((font_stream_t*)stream->descriptor.pointer)->stats;return true;
}
size_t thingsos_font_drop_bitmaps(thingsos_font_t* f) {
    if(!f || !f->bitmaps)return 0;
    bitmap_t* entries=f->bitmaps;size_t bytes=0;
    for(unsigned i=0;i<BITMAP_SLOTS;++i) {
        if(entries[i].pixels)bytes+=(size_t)entries[i].width*entries[i].rows;
        free(entries[i].pixels);memset(entries+i,0,sizeof(*entries));
    }
    return bytes;
}
void thingsos_font_close(thingsos_font_t* f) {
    if (!f) return;
    font_stream_t* stream=NULL;
    if(f->face && ((FT_Face)f->face)->stream && ((FT_Face)f->face)->stream->read==font_read)
        stream=((FT_Face)f->face)->stream->descriptor.pointer;
    if (f->face) FT_Done_Face(f->face);
    free(stream);
    if (f->library) font_library_done(f->library);
    free(f->metrics);
    if (f->bitmaps) {
        bitmap_t* b = f->bitmaps;
        for (unsigned i=0;i<BITMAP_SLOTS;++i) free(b[i].pixels);
        free(b);
    }
    memset(f, 0, sizeof(*f));
}
static bool install(thingsos_font_t* f, FT_Library library, FT_Face face) {
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) || FT_Set_Pixel_Sizes(face, 0, 32)) {
        FT_Done_Face(face);
        font_library_done(library);
        return false;
    }
    thingsos_font_close(f);
    f->face = face;
    f->library = library;
    f->size = 32;
    f->metrics = cache_zero(METRIC_SLOTS*sizeof(metric_t));
    f->bitmaps = cache_zero(BITMAP_SLOTS*sizeof(bitmap_t));
    return true;
}
bool thingsos_font_memory(thingsos_font_t* f, const unsigned char* data, size_t bytes) {
    if (!f || !data || bytes < 64 || bytes > LONG_MAX) return false;
    FT_Library l;
    FT_Face face;
    if (font_library_new(&l)) return false;
    if (FT_New_Memory_Face(l, data, (FT_Long)bytes, 0, &face)) {
        font_library_done(l);
        return false;
    }
    if (!install(f, l, face)) return false;
    f->signature=signature(data,bytes,14695981039346656037ULL);
    return true;
}
bool thingsos_font_file(thingsos_font_t* f, const char* path) {
    if (!f || !path) return false;
    font_stream_t* source=cache_zero(sizeof(*source));
    if(!source)return false;
    source->file=fopen(path,"rb");
    if(!source->file){free(source);return false;}
    /* FreeType owns seeking; our bounded block cache replaces stdio's small
       implicit buffer and keeps all optional font I/O storage out of SRAM. */
    setvbuf(source->file,NULL,_IONBF,0);
    if(fseek(source->file,0,SEEK_END)){fclose(source->file);free(source);return false;}
    long bytes=ftell(source->file);
    if(bytes<64){fclose(source->file);free(source);return false;}
    bool use_cache=true;
#ifdef ESP_PLATFORM
    use_cache=heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>FONT_IO_BLOCK*FONT_IO_SLOTS+512u*1024u;
#else
    use_cache=!font_host_no_cache;
#endif
    if(use_cache)source->cache=cache_allocate(FONT_IO_BLOCK*FONT_IO_SLOTS);
    source->stats.cache_bytes=source->cache?FONT_IO_BLOCK*FONT_IO_SLOTS:0;
    source->stream.size=(unsigned long)bytes;
    source->stream.descriptor.pointer=source;
    source->stream.read=font_read;source->stream.close=font_stream_close;
    FT_Library l;
    FT_Face face;
    if (font_library_new(&l)) {font_stream_close(&source->stream);free(source);return false;}
    FT_Open_Args args={0};args.flags=FT_OPEN_STREAM;args.stream=&source->stream;
    if (FT_Open_Face(l, &args, 0, &face)) {
        font_stream_close(&source->stream);free(source);
        font_library_done(l);
        return false;
    }
    if (!install(f, l, face)) {free(source);return false;}
    FILE* file=fopen(path,"rb");
    unsigned char block[4096]; size_t n;
    uint64_t hash=14695981039346656037ULL;
    if (file) {
        while ((n=fread(block,1,sizeof(block),file))) hash=signature(block,n,hash);
        if (!ferror(file)) f->signature=hash;
        fclose(file);
    }
    return true;
}
bool thingsos_font_size(thingsos_font_t* f, unsigned px) {
    if (f && f->ui_size_percent) {
        px = (px * f->ui_size_percent + 50) / 100;
        if (px < 16) px = 16;
        if (px > 70) px = 70;
    }
    if (f && f->face && f->size == px) return true;
    if (!f || !f->face || px < 16 || px > 70) return false;
    if (FT_Set_Pixel_Sizes(f->face, 0, px)) {++f->errors;return false;}
    f->size = px;
    return true;
}
bool thingsos_font_weight(thingsos_font_t* f, unsigned weight) {
    if (f && f->ui_weight_override) weight = f->ui_weight_override - 1;
    if(!f || weight>2) return false;
    f->weight=weight;
    return true;
}
void thingsos_rect(thingsos_canvas_t* c, int x, int y, int w, int h, unsigned char shade) {
    if (!c || !c->pixels || w <= 0 || h <= 0) return;
    if(c->vertical_text) {
        thingsos_canvas_t physical=*c;physical.width=c->height;physical.height=c->width;physical.vertical_text=false;
        thingsos_rect(&physical,c->height-y-h,x,h,w,shade);return;
    }
    int right = x + w, bottom = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > c->width) right = c->width;
    if (bottom > c->height) bottom = c->height;
    for (int j = y; j < bottom; j++)
        for (int i = x; i < right; i++) c->pixels[(size_t)j * c->width + i] = shade;
}
static void glyph_placeholder(thingsos_canvas_t* c,unsigned size,int x,int baseline) {
    int side=(int)size-6;if(side<8)side=8;
    thingsos_rect(c,x+2,baseline-(int)size+3,side,1,0);
    thingsos_rect(c,x+2,baseline-4,side,1,0);
    thingsos_rect(c,x+2,baseline-(int)size+3,1,side,0);
    thingsos_rect(c,x+side+1,baseline-(int)size+3,1,side,0);
}
/* DU remains binary. Distribute coverage within each glyph rather than a
   screen-anchored Bayer grid: identical glyphs retain identical edges after
   pagination/movement. Two bounded error rows cost 1032 bytes of stack and
   no per-glyph allocation; larger custom glyphs use a safe threshold fallback.
   Strong stems and empty/fringe pixels are pinned, so error never deposits
   dots outside the outline or perforates fully covered strokes. */
static void glyph_blit(const thingsos_font_t* f,thingsos_canvas_t* c,
    const unsigned char* pixels,unsigned width,unsigned rows,int pitch,bool mono,
    int left,int top,int x,int baseline,bool rotated,bool corner) {
    if(!pixels || !width || !rows)return;
    enum { ERROR_WIDTH=256 };
    int16_t errors[2][ERROR_WIDTH+2]={{0}};
    bool diffuse=f->dither && !mono && width<=ERROR_WIDTH;
    for(unsigned j=0;j<rows;++j) {
        const unsigned char* row=pixels+(pitch>=0?(size_t)j*pitch:(size_t)(rows-j-1)*(-pitch));
        int direction=diffuse && (j&1u)?-1:1;
        int16_t* current=errors[j&1u];
        int16_t* next=errors[(j+1)&1u];
        if(diffuse)memset(next,0,sizeof(errors[0]));
        for(unsigned n=0;n<width;++n) {
            unsigned i=direction>0?n:width-1-n;
            unsigned char a=mono?((row[i/8]&(0x80u>>(i%8)))?255:0):row[i];
            if(diffuse) {
                int value=(int)a*16+current[i+1];
                unsigned char quantized=a<=16?0:a>=240?255:value>=128*16?255:0;
                /* Suppress diffusion into transparent padding and solid ink. */
                if(a>16 && a<240) {
                    int error=value-(int)quantized*16;
                    int at=(int)i+1;
                    current[at+direction]+=(int16_t)(error*7/16);
                    next[at-direction]+=(int16_t)(error*3/16);
                    next[at]+=(int16_t)(error*5/16);
                    next[at+direction]+=(int16_t)(error/16);
                }
                a=quantized;
            } else if(f->dither)a=a>=128?255:0;
            /* Process the whole bitmap even when clipped, so clipping and
               warm/cold bitmap caches cannot alter the visible edge pattern. */
            int xx=x+left+(int)i+(f->italic?(top-(int)j)/5:0), yy=baseline-top+(int)j;
            int stride=c->width,limit_y=c->height;
            if(c->vertical_text) {
                /* Transpose the line axes while leaving each glyph upright. */
                xx=c->height-baseline+left+(int)i+(f->italic?(top-(int)j)/5:0);
                int offset=(int)f->size-top;
                if(offset+(int)rows>(int)f->size)offset=(int)f->size-(int)rows;
                if(offset<0)offset=0;
                yy=x+offset+(int)j;
                if(rotated) {
                    xx=c->height-baseline+((int)f->size-(int)rows)/2+(int)rows-1-(int)j;
                    yy=x+((int)f->size-(int)width)/2+(int)i;
                } else if(corner) {
                    xx=c->height-baseline+(int)f->size-(int)width-2+(int)i;
                    yy=x+2+(int)j;
                }
                stride=c->height;limit_y=c->width;
                if(xx<c->height-baseline || xx>=c->height-baseline+(int)f->size ||
                   yy<x || yy>=x+(int)f->size)continue;
            }
            if(xx<0 || xx>=stride || yy<0 || yy>=limit_y)continue;
            unsigned char* pixel=&c->pixels[(size_t)yy*stride+xx];
            *pixel=(unsigned char)((unsigned)*pixel*(255-a)/255);
        }
    }
}
static uint32_t vertical_codepoint(FT_Face face,uint32_t cp) {
    uint32_t mapped=cp;
    switch(cp) {
        case ',':case 0xff0c:mapped=0xfe10;break;
        case 0x3001:mapped=0xfe11;break;case 0x3002:mapped=0xfe12;break;
        case ':':case 0xff1a:mapped=0xfe13;break;case ';':case 0xff1b:mapped=0xfe14;break;
        case '!':case 0xff01:mapped=0xfe15;break;case '?':case 0xff1f:mapped=0xfe16;break;
        case 0x2026:mapped=0xfe19;break;case 0x2014:mapped=0xfe31;break;case 0x2013:mapped=0xfe32;break;
        case '(':case 0xff08:mapped=0xfe35;break;case ')':case 0xff09:mapped=0xfe36;break;
        case '{':mapped=0xfe37;break;case '}':mapped=0xfe38;break;
        case '[':mapped=0xfe47;break;case ']':mapped=0xfe48;break;
        case 0x3014:mapped=0xfe39;break;case 0x3015:mapped=0xfe3a;break;
        case 0x3010:mapped=0xfe3b;break;case 0x3011:mapped=0xfe3c;break;
        case 0x300a:mapped=0xfe3d;break;case 0x300b:mapped=0xfe3e;break;
        case 0x3008:mapped=0xfe3f;break;case 0x3009:mapped=0xfe40;break;
        case 0x201c:case 0x300c:mapped=0xfe41;break;case 0x201d:case 0x300d:mapped=0xfe42;break;
        case 0x300e:mapped=0xfe43;break;case 0x300f:mapped=0xfe44;break;
    }
    return mapped!=cp && FT_Get_Char_Index(face,mapped)?mapped:cp;
}
static bool vertical_rotate_fallback(uint32_t cp) {
    switch(cp) {
        case '-':case '_':case '(':case ')':case '[':case ']':case '{':case '}':
        case 0x2013:case 0x2014:case 0x2026:case 0x201c:case 0x201d:
        case 0x3008:case 0x3009:case 0x300a:case 0x300b:case 0x300c:case 0x300d:
        case 0x300e:case 0x300f:case 0x3010:case 0x3011:case 0x3014:case 0x3015:
        case 0xff08:case 0xff09:return true;
        default:return false;
    }
}
int thingsos_glyph(thingsos_font_t* f, thingsos_canvas_t* c, uint32_t cp, int x, int baseline) {
    if (!f || !f->face) return 0;
    FT_Face face = f->face;
    uint32_t original_cp=cp;
    if(f->vertical_text)cp=vertical_codepoint(face,cp);
    bool rotated=f->vertical_text && cp==original_cp && vertical_rotate_fallback(cp);
    bool corner=f->vertical_text && (original_cp==',' || original_cp=='.' || original_cp==0xff0c || original_cp==0x3001 || original_cp==0x3002);
    metric_t* metric = f->metrics ? (metric_t*)f->metrics +
        ((cp * 2654435761u ^ f->size) % METRIC_SLOTS) : NULL;
    if ((!c || !c->pixels) && metric && metric->size == f->size && metric->codepoint == cp && metric->monochrome==f->monochrome)
        return f->vertical_text?(int)f->size:metric->advance;
    bitmap_t* cached=NULL;
    if(f->bitmaps && c && c->pixels) {
        /* Four candidates retain colliding common glyphs without increasing
           the number of cached bitmaps or their pixel allocation budget. */
        unsigned base=((cp*2654435761u ^ f->size ^ f->weight*40503u)%(BITMAP_SLOTS/4))*4;
        bitmap_t* set=(bitmap_t*)f->bitmaps+base;
        unsigned clock=f->bitmap_hits+f->glyph_loads;
        cached=set;
        for(unsigned i=0;i<4;++i) {
            bitmap_t* b=set+i;
            if(b->pixels && b->size==f->size && b->weight==f->weight &&
               b->codepoint==cp && b->monochrome==f->monochrome){cached=b;break;}
            if(!b->pixels || (cached->pixels && clock-b->used>clock-cached->used))cached=b;
        }
    }
    if (c && c->pixels && cached && cached->pixels &&
        cached->size==f->size && cached->weight==f->weight && cached->codepoint==cp && cached->monochrome==f->monochrome) {
        f->bitmap_hits++;
        cached->used=f->bitmap_hits+f->glyph_loads;
        glyph_blit(f,c,cached->pixels,cached->width,cached->rows,(int)cached->width,
            false,cached->left,cached->top,x,baseline,rotated,corner);
        return f->vertical_text?(int)f->size:cached->advance;
    }
#ifdef ESP_PLATFORM
    /* Cached advances are cheap. Yield for actual FreeType work only. */
    static unsigned work;
    if (++work % 64 == 0) vTaskDelay(1);
#endif
    /* Pagination needs the hinted advance, not a rasterized bitmap. */
    FT_Int32 flags = FT_LOAD_NO_SVG | FT_LOAD_NO_BITMAP | (f->monochrome?FT_LOAD_TARGET_MONO:0);
    f->glyph_loads++;
    FT_Error error=FT_Load_Char(face, cp, flags);
    /* Some custom fonts have unusable hint programs but valid outlines. */
    if(error)error=FT_Load_Char(face,cp,flags|FT_LOAD_NO_HINTING|FT_LOAD_NO_AUTOHINT);
    if (error) {
        ++f->errors;
#ifdef ESP_PLATFORM
        ESP_LOGE("ThingsOS_font","Glyph load failed U+%04lx size=%u error=0x%x internal=%u psram=%u",
            (unsigned long)cp,f->size,(unsigned)error,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
        glyph_placeholder(c,f->size,x,baseline);
        return (int)f->size;
    }
    FT_GlyphSlot g = face->glyph;
    if(c && c->pixels && f->weight && g->format==FT_GLYPH_FORMAT_OUTLINE) {
        /* Synthetic bold is optional. An unsupported contour orientation
           still has a drawable original outline; it is not a missing glyph. */
        FT_Error bold_error=FT_Outline_Embolden(&g->outline,(FT_Pos)f->weight*32);
#ifdef ESP_PLATFORM
        if(bold_error)ESP_LOGW("ThingsOS_font","Synthetic bold skipped U+%04lx error=0x%x",(unsigned long)cp,(unsigned)bold_error);
#else
        (void)bold_error;
#endif
    }
    int advance = (int)((g->advance.x + 63) / 64);
    if (metric) *metric = (metric_t){ cp, f->size, advance,
        (int)(g->metrics.horiBearingX >> 6),
        (int)((g->metrics.horiBearingX + g->metrics.width + 63) >> 6), f->monochrome };
    if (!c || !c->pixels) return f->vertical_text?(int)f->size:advance;
    /* Keep the gray raster for plain/dithered blits; quantization is derived
       from that bitmap, so it is not part of the cache key. */
    error=FT_Render_Glyph(g,f->monochrome?FT_RENDER_MODE_MONO:FT_RENDER_MODE_NORMAL);
    if(error) {
        ++f->errors;
#ifdef ESP_PLATFORM
        ESP_LOGE("ThingsOS_font","Glyph raster failed U+%04lx size=%u error=0x%x internal=%u psram=%u",
            (unsigned long)cp,f->size,(unsigned)error,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
        glyph_placeholder(c,f->size,x,baseline);
        return f->vertical_text?(int)f->size:advance;
    }
    FT_Bitmap* b = &g->bitmap;
    if (b->pixel_mode != FT_PIXEL_MODE_GRAY && b->pixel_mode != FT_PIXEL_MODE_MONO) return f->vertical_text?(int)f->size:advance;
    /* At most 2 MiB across 512 slots; allocation failure simply draws normally. */
    if (cached && b->width && b->rows && (size_t)b->width*b->rows<=4096) {
        unsigned char* pixels=cache_allocate((size_t)b->width*b->rows);
        if (pixels) {
            for(unsigned j=0;j<b->rows;++j) {
                const unsigned char* row=b->buffer+(b->pitch>=0?(size_t)j*b->pitch:(size_t)(b->rows-j-1)*(-b->pitch));
                for(unsigned i=0;i<b->width;++i)pixels[(size_t)j*b->width+i]=
                    b->pixel_mode==FT_PIXEL_MODE_MONO?((row[i/8]&(0x80u>>(i%8)))?255:0):row[i];
            }
            free(cached->pixels);
            *cached=(bitmap_t){cp,f->size,b->width,b->rows,f->weight,g->bitmap_left,g->bitmap_top,advance,pixels,f->monochrome,f->bitmap_hits+f->glyph_loads};
        }
    }
    glyph_blit(f,c,b->buffer,b->width,b->rows,b->pitch,b->pixel_mode==FT_PIXEL_MODE_MONO,
        g->bitmap_left,g->bitmap_top,x,baseline,rotated,corner);
    return f->vertical_text?(int)f->size:advance;
}
void thingsos_label(thingsos_font_t* f, thingsos_canvas_t* c, const char* s, int x, int y, int w) {
    if (!s) return;
    size_t n = strlen(s), i = 0;
    int used = 0;
    while (i < n) {
        unsigned bytes;
        uint32_t cp = next(s + i, n - i, &bytes);
        int a = thingsos_glyph(f, NULL, cp, 0, 0);
        if (used + a > w) break;
        thingsos_glyph(f, c, cp, x + used, y);
        used += a;
        i += bytes;
    }
}
static void label_aligned(thingsos_font_t* f, thingsos_canvas_t* c, const char* s,
                          int x, int y, int width, bool right) {
    if (!s || width <= 0) return;
    size_t n = strlen(s), i = 0;
    int measured = 0;
    while (i < n) {
        unsigned bytes;
        uint32_t cp = next(s + i, n - i, &bytes);
        int advance = thingsos_glyph(f, NULL, cp, 0, 0);
        if (measured + advance > width) break;
        measured += advance;
        i += bytes;
    }
    thingsos_label(f, c, s, x + (width - measured) / (right ? 1 : 2), y, measured);
}
void thingsos_label_centered(thingsos_font_t* f, thingsos_canvas_t* c, const char* s,
                             int x, int y, int width) {
    label_aligned(f, c, s, x, y, width, false);
}
void thingsos_label_right(thingsos_font_t* f, thingsos_canvas_t* c, const char* s,
                          int x, int y, int width) {
    label_aligned(f, c, s, x, y, width, true);
}
static void glyph_bitmap_ink(
    const unsigned char* buf, unsigned width, unsigned rows, int pitch, bool mono,
    int left, int top, int x_base, bool* any, int* min_x, int* max_x, int* min_y, int* max_y) {
    if (!buf || !width || !rows) return;
    int gx0 = INT_MAX, gx1 = INT_MIN, gy0 = INT_MAX, gy1 = INT_MIN;
    int px0 = INT_MAX, px1 = INT_MIN, py0 = INT_MAX, py1 = INT_MIN;
    for (unsigned j = 0; j < rows; ++j) {
        const unsigned char* row = buf + (pitch >= 0 ? (size_t)j * pitch : (size_t)(rows - j - 1) * (-pitch));
        for (unsigned i = 0; i < width; ++i) {
            unsigned alpha = mono ? ((row[i / 8] & (0x80u >> (i % 8))) ? 255u : 0u) : row[i];
            if (!alpha) continue;
            if ((int)i < px0) px0 = (int)i;
            if ((int)i > px1) px1 = (int)i;
            if ((int)j < py0) py0 = (int)j;
            if ((int)j > py1) py1 = (int)j;
            if (alpha < 32u) continue;
            if ((int)i < gx0) gx0 = (int)i;
            if ((int)i > gx1) gx1 = (int)i;
            if ((int)j < gy0) gy0 = (int)j;
            if ((int)j > gy1) gy1 = (int)j;
        }
    }
    if (gx0 > gx1) {
        if (px0 > px1) return;
        gx0 = px0; gx1 = px1; gy0 = py0; gy1 = py1;
    }
    int l = x_base + left + gx0, r = x_base + left + gx1;
    int t = -top + gy0, b = -top + gy1;
    if (!*any) {
        *any = true;
        *min_x = l; *max_x = r; *min_y = t; *max_y = b;
    } else {
        if (l < *min_x) *min_x = l;
        if (r > *max_x) *max_x = r;
        if (t < *min_y) *min_y = t;
        if (b > *max_y) *max_y = b;
    }
}
bool thingsos_text_ink_box(
    thingsos_font_t* f, const char* s, int max_width,
    int* ink_left, int* ink_right, int* ink_top, int* ink_bottom, int* advance_width) {
    if (!f || !f->face || !s) return false;
    size_t n = strlen(s), i = 0;
    int used = 0, min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    bool any = false;
    while (i < n) {
        unsigned bytes;
        uint32_t cp = next(s + i, n - i, &bytes);
        i += bytes;
        bitmap_t* b = NULL;
        if (f->bitmaps) {
            unsigned base = ((cp * 2654435761u ^ f->size ^ f->weight * 40503u) % (BITMAP_SLOTS / 4)) * 4;
            bitmap_t* set = (bitmap_t*)f->bitmaps + base;
            for (unsigned k = 0; k < 4; ++k) {
                if (set[k].pixels && set[k].size == f->size && set[k].weight == f->weight &&
                    set[k].codepoint == cp && set[k].monochrome == f->monochrome) {
                    b = set + k;
                    break;
                }
            }
        }
        if (b) {
            if (max_width > 0 && used + b->advance > max_width) break;
            glyph_bitmap_ink(
                b->pixels, b->width, b->rows, (int)b->width, false,
                b->left, b->top, used, &any, &min_x, &max_x, &min_y, &max_y);
            used += b->advance;
            continue;
        }
        FT_Face face = f->face;
        FT_Int32 flags = FT_LOAD_NO_SVG | FT_LOAD_NO_BITMAP | (f->monochrome ? FT_LOAD_TARGET_MONO : 0);
        FT_Error error = FT_Load_Char(face, cp, flags);
        if (error) error = FT_Load_Char(face, cp, flags | FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT);
        if (error) {
            int adv = (int)f->size;
            if (max_width > 0 && used + adv > max_width) break;
            used += adv;
            continue;
        }
        FT_GlyphSlot g = face->glyph;
        if (f->weight && g->format == FT_GLYPH_FORMAT_OUTLINE)
            FT_Outline_Embolden(&g->outline, (FT_Pos)f->weight * 32);
        int adv = (int)((g->advance.x + 63) / 64);
        if (max_width > 0 && used + adv > max_width) break;
        error = FT_Render_Glyph(g, f->monochrome ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL);
        if (!error && (g->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY || g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO))
            glyph_bitmap_ink(
                g->bitmap.buffer, g->bitmap.width, g->bitmap.rows, g->bitmap.pitch,
                g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO, g->bitmap_left, g->bitmap_top,
                used, &any, &min_x, &max_x, &min_y, &max_y);
        used += adv;
    }
    if (advance_width) *advance_width = used;
    if (!any) return false;
    if (ink_left) *ink_left = min_x;
    if (ink_right) *ink_right = max_x;
    if (ink_top) *ink_top = min_y;
    if (ink_bottom) *ink_bottom = max_y;
    return true;
}
static bool epub_paragraph_indent(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0x00a0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f || cp == 0x205f || cp == 0x3000;
}
/* Closing punctuation stays with the preceding character at a soft wrap.
   Explicit paragraph breaks and source bytes remain untouched. */
static bool line_start_prohibited(uint32_t cp) {
    switch(cp) {
        case ',': case '.': case ';': case ':': case '!': case '?':
        case '%': case ')': case ']': case '}':
        case 0x2019: case 0x201d: case 0x2026: case 0x2014:
        case 0x3001: case 0x3002: case 0x3009: case 0x300b:
        case 0x300d: case 0x300f: case 0x3011: case 0x3015:
        case 0x3017: case 0x3019: case 0x301b:
        case 0xff01: case 0xff05: case 0xff09: case 0xff0c:
        case 0xff0e: case 0xff1a: case 0xff1b: case 0xff1f:
        case 0xff3d: case 0xff5d: return true;
        default: return false;
    }
}
/* True when start is the first character of a paragraph: at the chapter start,
   or after the line break and indentation whitespace that ended the previous
   one. A page that resumes in the middle of a paragraph reports false, so its
   first line keeps the shared gutter. */
static bool paragraph_begin(const char* text, size_t start) {
    size_t i = start;
    while (i) {
        unsigned char c = (unsigned char)text[i - 1];
        if (c == '\n' || c == '\r') return true;
        if (c == ' ' || c == '\t') { --i; continue; }
        /* U+3000 ideographic space, the indentation most Chinese sources carry. */
        if (i >= 3 && (unsigned char)text[i - 3] == 0xe3 && (unsigned char)text[i - 2] == 0x80 &&
            c == 0x80) { i -= 3; continue; }
        break;
    }
    return i == 0;
}
/* Compact borrowed view also serves note text without allocating an EPUB image array. */
typedef struct {
    const char* text;size_t bytes;
    const thingsos_inline_t* images;size_t image_count;
    size_t heading_start,heading_end,hidden_start,hidden_end;bool heading;
    const thingsos_text_underline_t* underlines;size_t underline_count;
    const thingsos_footnote_t* footnotes;size_t footnote_count;
    const thingsos_chapter_t* markdown;
    unsigned cover_image;
} text_view_t;

#define PROSE_MAX_GLYPHS 128
typedef struct {
    size_t start, end;
    uint16_t count;
    bool soft_break;
    int16_t pre_shift[PROSE_MAX_GLYPHS];
    int16_t step_adv[PROSE_MAX_GLYPHS];
} prose_line_t;

static bool han_character(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
           (cp >= 0x20000 && cp <= 0x323af);
}

static metric_t prose_metric(thingsos_font_t* f, uint32_t cp) {
    int advance = thingsos_glyph(f, NULL, cp, 0, 0);
    if (!f->vertical_text && f->metrics) {
        metric_t* slot = (metric_t*)f->metrics + ((cp * 2654435761u ^ f->size) % METRIC_SLOTS);
        if (slot->codepoint == cp && slot->size == f->size && slot->monochrome == f->monochrome)
            return *slot;
    }
    return (metric_t){.advance=advance, .ink_right=advance};
}

static bool latin_or_digit(uint32_t cp) {
    return (cp>='0' && cp<='9') || (cp>='A' && cp<='Z') || (cp>='a' && cp<='z') ||
           (cp>=0x00c0 && cp<=0x024f && cp!=0x00d7 && cp!=0x00f7);
}
static bool cjk_latin_pair(uint32_t a, uint32_t b) {
    return (han_character(a) && latin_or_digit(b)) || (latin_or_digit(a) && han_character(b));
}
static bool open_punct(uint32_t cp) {
    switch(cp) {
        case 0x2018: case 0x201c: case 0x3008: case 0x300a: case 0x300c: case 0x300e:
        case 0x3010: case 0x3014: case 0x3016: case 0xff08: case 0xff3b: case 0xff5b: return true;
        default: return false;
    }
}
static bool close_punct(uint32_t cp) {
    switch(cp) {
        case 0x2019: case 0x201d: case 0x3009: case 0x300b: case 0x300d: case 0x300f:
        case 0x3011: case 0x3015: case 0x3017: case 0xff09: case 0xff3d: case 0xff5d: return true;
        default: return false;
    }
}
static bool hanging_punct(uint32_t cp) {
    return cp==0x3001 || cp==0x3002 || cp==0xff0c || cp==0xff0e || cp==0xff1a || cp==0xff1b;
}
static bool end_punct(uint32_t cp) {
    return hanging_punct(cp) || close_punct(cp) || cp==0xff01 || cp==0xff1f;
}
static bool line_end_prohibited(uint32_t cp) {
    return open_punct(cp) || cp=='(' || cp=='[' || cp=='{';
}
static bool pair_mark(uint32_t cp) { return cp==0x2026 || cp==0x2014; }
static size_t prose_limit(const text_view_t* ch, size_t start) {
    size_t limit = ch->bytes;
    if (start < ch->hidden_start && ch->hidden_start < limit) limit = ch->hidden_start;
    if (ch->heading && start < ch->heading_start && ch->heading_start < limit) limit = ch->heading_start;
    for (size_t i = 0; i < ch->image_count; ++i)
        if (ch->images[i].offset >= start && ch->images[i].offset < limit) limit = ch->images[i].offset;
    for (size_t i = 0; i < ch->footnote_count; ++i) {
        const thingsos_footnote_t* note = ch->footnotes + i;
        if (start >= note->begin && start < note->end) return start;
        if (note->begin >= start && note->begin < limit) limit = note->begin;
    }

    return limit;
}

static int min_int(int a, int b) { return a<b?a:b; }
static int positive(int n) { return n>0?n:0; }
static int side_trim(int space, metric_t glyph, int half) {
    return positive(min_int(space,min_int(half,glyph.advance/2)));
}
static int right_trim(metric_t glyph, int half) {
    return side_trim(glyph.ink_right>0?glyph.advance-glyph.ink_right-2:0,glyph,half);
}
static bool prose_break(const uint32_t* cp, const bool* pair_second, unsigned k, unsigned scanned, bool hard_stop) {
    if(k==scanned && hard_stop)return true;
    return !pair_second[k] && !line_end_prohibited(cp[k-1]) &&
        (!line_start_prohibited(cp[k]) || pair_second[k+1]);
}
static bool stretch_gap(uint32_t a, uint32_t b, bool pair_second) {
    if(pair_second || (end_punct(b) && han_character(a)))return false;
    if((open_punct(a) || end_punct(a)) && (open_punct(b) || end_punct(b)))return false;
    if((a==' ' || b==' ') && (line_start_prohibited(b) || line_end_prohibited(a)))return false;
    return (han_character(a) && (han_character(b) || open_punct(b))) ||
        (close_punct(a) && han_character(b)) || cjk_latin_pair(a,b) || a==' ';
}

static int hang_shift(metric_t glyph, int size) {
    return min_int(positive(glyph.ink_left-3),size/4);
}
static int hang_out(metric_t glyph, int size) {
    int ink=glyph.ink_right>0?glyph.ink_right:size/2;
    int out=ink-hang_shift(glyph,size);
    return out>0?out:1;
}

typedef struct {
    prose_line_t planned;
    unsigned overflow;
    bool hard_stop;
    size_t pos[PROSE_MAX_GLYPHS+1];
    uint32_t cp[PROSE_MAX_GLYPHS+2];
    bool pair_second[PROSE_MAX_GLYPHS+2];
    metric_t metric[PROSE_MAX_GLYPHS];
    int prefix[PROSE_MAX_GLYPHS+1], space_prefix[PROSE_MAX_GLYPHS+1];
    int16_t pair_cut[PROSE_MAX_GLYPHS];
    unsigned gap_prefix[PROSE_MAX_GLYPHS+1];
} prose_scratch_t;

typedef struct { unsigned count; int space_cut; bool hang; } prose_fit_t;
static inline __attribute__((always_inline)) prose_fit_t fit_prose_hang(unsigned k, int body, int width, int hanging, int spaces,
                                  int size, bool soft, const prose_scratch_t* scratch) {
    if(k<2 || hanging<=0 || hang_out(scratch->metric[k-1],size)>hanging)return (prose_fit_t){0};
    int cut=positive(body-width);
    if(cut>spaces)return (prose_fit_t){0};
    if(!soft)return (prose_fit_t){k,cut,true};
    /* Do not hang a point from a visibly short row when the allowed gaps
       cannot bring the preceding body ink close enough to the edge. */
    int slots=(int)scratch->gap_prefix[k-1], capacity=slots*(size/6);
    int missing=positive(width-(body-cut)-capacity);
    if(han_character(scratch->cp[k-2])) {
        int inset=positive(scratch->metric[k-2].advance-scratch->metric[k-2].ink_right+1);
        if(missing+inset>6)return (prose_fit_t){0};
    } else if(missing>=hang_out(scratch->metric[k-1],size))return (prose_fit_t){0};
    return (prose_fit_t){k,cut,true};
}

static __attribute__((noinline)) unsigned scan_prose_line(thingsos_font_t* f,const text_view_t* ch,
    size_t start,int start_x,int width,prose_scratch_t* scratch) {
    prose_line_t* out=&scratch->planned;
    size_t limit=prose_limit(ch,start), p=start;
    size_t* pos=scratch->pos;
    uint32_t* cp=scratch->cp;
    bool* pair_second=scratch->pair_second;
    metric_t* metric=scratch->metric;
    int *prefix=scratch->prefix, *space_prefix=scratch->space_prefix;
    int16_t* pair_cut=scratch->pair_cut;
    unsigned* gap_prefix=scratch->gap_prefix;
    space_prefix[0]=0;
    gap_prefix[0]=0;
    pair_second[0]=false;
    unsigned scanned=0, overflow=0;
    int half=(int)f->size/2, gap=(int)f->size/4;
    if(gap<1)gap=1;
    prefix[0]=start_x;
    for(;p<limit && scanned<PROSE_MAX_GLYPHS;++scanned) {
        unsigned bytes;
        unsigned i=scanned;
        cp[i]=next(ch->text+p,limit-p,&bytes);
        if(cp[i]=='\r' || cp[i]=='\n')break;
        pos[i]=p;
        p+=bytes;
        if(i) pair_second[i]=pair_mark(cp[i]) && cp[i]==cp[i-1] && !pair_second[i-1];
        gap_prefix[i+1]=gap_prefix[i]+(i && stretch_gap(cp[i-1],cp[i],pair_second[i]));
        metric[i]=prose_metric(f,cp[i]);
        int shift=0, cut=0;
        if(!i && open_punct(cp[i]))shift=-side_trim(metric[i].ink_left,metric[i],half);
        if(i) {
            uint32_t prev=cp[i-1];
            if(cjk_latin_pair(prev,cp[i]))shift=gap;
            /* Apply each permitted pair's full safe allowance at this stage.
               No extra punctuation compression remains when fitting a candidate. */
            if((end_punct(prev) && open_punct(cp[i])) || (close_punct(prev) && end_punct(cp[i])))
                cut=right_trim(metric[i-1],half);
            if(open_punct(cp[i]) && (end_punct(prev) || open_punct(prev)))
                shift-=min_int(half-cut,side_trim(metric[i].ink_left-2,metric[i],half));
            pair_cut[i-1]=(int16_t)cut;
            out->step_adv[i-1]-=(int16_t)cut;
        }
        out->pre_shift[i]=(int16_t)shift;
        out->step_adv[i]=(int16_t)(metric[i].advance+shift);
        prefix[i+1]=prefix[i]+out->step_adv[i]-cut;
        space_prefix[i+1]=space_prefix[i]+(cp[i]==' '?metric[i].advance/3:0);
        int trim=end_punct(cp[i])?right_trim(metric[i],half):0;
        if(!overflow && prefix[i+1]-trim>width)overflow=i+1;
    }
    if(!scanned)return 0;
    pair_cut[scanned-1]=0;
    cp[scanned+1]=0;
    pair_second[scanned]=pair_second[scanned+1]=false;
    pos[scanned]=p;
    bool hard_stop=p==limit || ch->text[p]=='\r' || ch->text[p]=='\n';
    if(!hard_stop) {
        /* Two lookahead glyphs make the scan cap a legal pair boundary. */
        size_t look=p;
        for(unsigned i=scanned;i<=scanned+1 && look<limit;++i) {
            unsigned bytes;
            cp[i]=next(ch->text+look,limit-look,&bytes);
            pair_second[i]=pair_mark(cp[i]) && cp[i]==cp[i-1] && !pair_second[i-1];
            look+=bytes;
        }
    }
    scratch->hard_stop=hard_stop;
    scratch->overflow=overflow;
    return scanned;
}

static __attribute__((noinline)) void plan_prose_line(thingsos_font_t* f,
                                    size_t start, int width, int hanging, unsigned scanned, prose_scratch_t* scratch) {
    if(!scratch)return;
    prose_line_t* out=&scratch->planned;
    out->start=start;
    out->end=0;
    out->count=0;
    size_t* pos=scratch->pos;
    uint32_t* cp=scratch->cp;
    bool* pair_second=scratch->pair_second;
    metric_t* metric=scratch->metric;
    int *prefix=scratch->prefix, *space_prefix=scratch->space_prefix;
    int16_t* pair_cut=scratch->pair_cut;
    unsigned* gap_prefix=scratch->gap_prefix;
    if(!scanned)return;
    unsigned overflow=scratch->overflow;
    bool hard_stop=scratch->hard_stop;
    int half=(int)f->size/2;
    prose_fit_t fit={.count=scanned};
    if(overflow) {
        fit.count=0;
        /* A terminal mark may fit as hanging even when it does not fit as
           ordinary text. A paragraph end that fits uses its natural spacing. */
        unsigned last=min_int((int)scanned,(int)overflow+4);
        for(unsigned pass=0;pass<2 && !fit.count;++pass) {
            for(unsigned k=overflow;k<=last;++k) {
                if(!prose_break(cp,pair_second,k,scanned,hard_stop))continue;
                int spaces=pass?space_prefix[k-1]:0;
                int trim=end_punct(cp[k-1])?right_trim(metric[k-1],half):0;
                bool soft=k<scanned || !hard_stop;
                if(hanging_punct(cp[k-1]) && (soft || prefix[k]-trim>width)) {
                    int body=prefix[k]-out->step_adv[k-1]-pair_cut[k-1];
                    fit=fit_prose_hang(k,body,width,hanging,spaces,(int)f->size,soft,scratch);
                    if(fit.count)break;
                }
                int cut=positive(prefix[k]-trim-width);
                if(cut<=spaces){fit=(prose_fit_t){k,cut,false};break;}
            }
        }
        if(!fit.count) {
            unsigned k;
            for(k=overflow-1;k && !prose_break(cp,pair_second,k,scanned,hard_stop);--k) {}
            if(k && hanging_punct(cp[k-1])) {
                int body=prefix[k]-out->step_adv[k-1]-pair_cut[k-1];
                fit=fit_prose_hang(k,body,width,hanging,space_prefix[k-1],(int)f->size,true,scratch);
            }
            if(!k)k=overflow>1?overflow-1:1;
            if(pair_second[k])k=k>1?k-1:2;
            if(!fit.count)fit.count=k;
        }
    } else if(!hard_stop) {
        unsigned k=scanned;
        while(k>1 && !prose_break(cp,pair_second,k,scanned,false))--k;
        if(pair_second[k])k=2;
        fit.count=k;
        if(hanging_punct(cp[k-1]) && prose_break(cp,pair_second,k,scanned,false)) {
            int body=prefix[k]-out->step_adv[k-1]-pair_cut[k-1];
            prose_fit_t trial=fit_prose_hang(k,body,width,hanging,space_prefix[k-1],(int)f->size,true,scratch);
            if(trial.count)fit=trial;
        }
    }
    unsigned k=fit.count;
    if(!k || k>scanned || pos[k]<=start)return;
    out->count=(uint16_t)k;
    out->end=pos[k];
    out->soft_break=k<scanned || !hard_stop;
    out->step_adv[k-1]+=pair_cut[k-1]; /* The next line cannot squeeze this glyph. */
    int cut=fit.space_cut;
    for(unsigned i=0;i+1<k && cut>0;++i)if(cp[i]==' ') {
        int n=min_int(cut,metric[i].advance/3);
        out->step_adv[i]-=(int16_t)n;
        cut-=n;
    }
    int tail=end_punct(cp[k-1])?right_trim(metric[k-1],half):0;
    if(out->soft_break || fit.hang) {
        int body=prefix[k]-out->step_adv[k-1]-fit.space_cut;
        unsigned end=fit.hang?k-1:k;
        int slack=out->soft_break?positive(width-(fit.hang?body:prefix[k]-tail-fit.space_cut)):0;
        unsigned gaps=gap_prefix[end];
        slack=min_int(slack,(int)gaps*((int)f->size/(fit.hang?6:8)));
        if(fit.hang) {
            int overhang=hang_out(metric[k-1],(int)f->size);
            if(body+slack+overhang<=width+hanging) {
                out->pre_shift[k-1]=(int16_t)-hang_shift(metric[k-1],(int)f->size);
                tail=positive(out->step_adv[k-1]-overhang);
            }
        }
        unsigned used=0;
        for(unsigned i=1;i<end;++i)if(stretch_gap(cp[i-1],cp[i],pair_second[i])) {
            int n=slack*(int)(used+1)/(int)gaps-slack*(int)used/(int)gaps;
            ++used;
            out->step_adv[i-1]+=(int16_t)n;
        }
    }
    out->step_adv[k-1]-=(int16_t)tail;
}
/* One footnote marker spans several glyphs; merge them into a single box and
   pad it slightly so a 17-pixel digit is still a comfortable target. The box is
   clamped to the text area so a padded first or last line cannot overlap the
   header, the footer or another page's touch target. */
static void text_hit_add(thingsos_text_hits_t* hits, const thingsos_footnote_t* note, int x, int y,
                         int w, int line, int top, int bottom) {
    if (!hits) return;
    int left = x < 0 ? 0 : x;
    int upper = y < top ? top : y;
    int lower = y + line > bottom ? bottom : y + line;
    if (lower <= upper) lower = upper + 1;
    for (size_t i = 0; i < hits->count; ++i) {
        thingsos_text_hit_t* hit = hits->items + i;
        if (hit->begin != note->begin) continue;
        int right = left + w > hit->x + hit->w ? left + w : hit->x + hit->w;
        int floor = lower > hit->y + hit->h ? lower : hit->y + hit->h;
        if (left < hit->x) hit->x = left;
        if (upper < hit->y) hit->y = upper;
        hit->w = right - hit->x;
        hit->h = floor - hit->y;
        return;
    }
    if (hits->count >= THINGSOS_TEXT_HIT_LIMIT) return;
    thingsos_text_hit_t* hit = hits->items + hits->count++;
    hit->begin = note->begin;
    hit->end = note->end;
    hit->x = left;
    hit->y = upper;
    hit->w = w;
    hit->h = lower - upper;
}
/* In vertical flow, physical body width is the available baseline extent.
   Use the column count whose fitted pitch is closest to the selected spacing,
   rather than losing a whole column to a few pixels of trailing grid slack.
   Horizontal baseline spacing remains byte-for-byte unchanged. */
static int text_line_advance(const thingsos_font_t* font,unsigned size,unsigned percent,int extent) {
    int nominal=(int)(size*percent/100);
    if(!font->vertical_text || extent<2*(int)size || nominal<=0)return nominal;
    int span=extent-(int)size,gaps=span/nominal;
    if(gaps<1)return nominal;
    int pitch=span/gaps,denser=span/(gaps+1);
    if(denser>=(int)size && abs(denser-nominal)<=abs(pitch-nominal))pitch=denser;
    return pitch;
}
static const thingsos_inline_t* text_image_at(const text_view_t* text, size_t offset) {
    for (size_t i = 0; i < text->image_count; ++i)
        if (text->images[i].offset == offset) return text->images + i;
    return NULL;
}

static size_t skip_heading_space(const text_view_t* text, size_t offset) {
    while (offset < text->bytes) {
        unsigned bytes;
        uint32_t cp = next(text->text + offset, text->bytes - offset, &bytes);
        if ((cp != '\n' && cp != '\r' && !epub_paragraph_indent(cp)) ||
            text_image_at(text, offset)) break;
        offset += bytes;
    }
    return offset;
}

static __attribute__((noinline)) size_t text_view_page_impl(
    thingsos_font_t* f, thingsos_canvas_t* c, const text_view_t* ch, size_t start, int left,
    int top, int width, int height, unsigned line_percent, unsigned indent_px,
    bool epub_paragraphs, thingsos_picture_fn picture, void* context, thingsos_text_hits_t* hits,
    thingsos_layout_continue_fn proceed, void* proceed_context, bool* completed,
    thingsos_text_cursor_t* cursor, int hanging, prose_scratch_t* scratch) {
    if (completed) *completed=true;
    bool resume=cursor && cursor->active;
    if(hits && !resume)hits->glyph_count=0;
    if (!f || !ch || !ch->text || start >= ch->bytes) return ch ? ch->bytes : 0;
    if (width <= 0 || !f->size || height < (int)f->size) return start;
    thingsos_md_cursor_t* md=thingsos_markdown_cursor(ch->markdown,resume?cursor->offset:start);
    if(ch->markdown && !md){if(completed)*completed=false;return start;}
    unsigned original_weight=f->weight;bool original_italic=f->italic;
    size_t p = start;
    unsigned body_size=f->size, title_size=body_size<61?body_size+9:70;
    bool valid_heading=ch->heading && ch->heading_end<=ch->bytes && ch->heading_end>ch->heading_start;
    bool inline_heading = valid_heading &&
                          !(epub_paragraphs && ch->cover_image);
    bool heading = false, paragraph_ink = false;
    if (line_percent < 125 || line_percent > 200) line_percent = 145;
    const int body_line = text_line_advance(f,body_size,line_percent,height);
    int title_line = text_line_advance(f,title_size,line_percent,height);
    const int min_title_line = (int)title_size + (int)title_size / 4;
    if (title_line < min_title_line) title_line = min_title_line;
    /* Body paragraphs share the same baseline grid. A page starting at the
       exact title boundary also begins a paragraph, even without a newline. */
    bool paragraph_head = indent_px && (paragraph_begin(ch->text, start) ||
                                        (inline_heading && start == ch->heading_end));
    int x = paragraph_head ? (int)indent_px : 0, line = body_line,
        y = top + (int)body_size, bottom = top + height;
    int checked_y=INT_MIN;
    if(resume) {
        p=cursor->offset;x=cursor->x;y=cursor->y;line=cursor->line;
        heading=cursor->heading;paragraph_ink=cursor->paragraph_ink;paragraph_head=cursor->paragraph_head;
        thingsos_font_size(f,cursor->size);
    }
    bool paused=false;
    prose_line_t* planned=scratch?&scratch->planned:NULL;
    unsigned planned_idx=0;
    size_t pair_end=0, last_p=SIZE_MAX;
    unsigned retries=0;
    while (p < ch->bytes) {
        /* Title setup and a wrap may retry an offset, but never indefinitely. */
        if(last_p!=SIZE_MAX && p<last_p)break;
        if(p==last_p) {
            if(++retries>8)break;
        } else {last_p=p;retries=0;}
        if (planned && planned->end && p == planned->end) {
            bool soft=planned->soft_break && planned_idx==planned->count;
            planned->end=0;
            planned->count=0;
            if (soft && ch->text[p] != '\n' && ch->text[p] != '\r') {
                x = 0;
                y += line;
                if (y > bottom) break;
            }
        }
        if(planned && planned->end &&
           (p<planned->start || p>=planned->end || planned_idx>=planned->count))
            planned->end=planned->count=0;
        if(y!=checked_y) {
            if(proceed && !proceed(proceed_context)) {
                if(completed)*completed=false;
                paused=true;
                break;
            }
            checked_y=y;
        }
        bool title = inline_heading && p >= ch->heading_start && p < ch->heading_end;
        if (heading && !title) {
            /* Leave the boundary unconsumed on a page break, so the next
               page still recognizes the first body paragraph. */
            y += body_line + (int)body_size;
            if (y > bottom) break;
            heading = false;
            thingsos_font_size(f, body_size);
            line = body_line;
            x = 0;
            paragraph_ink = false;
            paragraph_head = indent_px != 0;
            p = skip_heading_space(ch, p);
            continue;
        }
        if (p>=ch->hidden_start && p<ch->hidden_end) {p=ch->hidden_end<ch->bytes?ch->hidden_end:ch->bytes;continue;}
        if (epub_paragraphs && ch->cover_image && valid_heading && p>=ch->heading_start && p<ch->heading_end) {
            p = skip_heading_space(ch, ch->heading_end);
            paragraph_head = indent_px != 0;
            continue;
        }
        if (title && !heading) {
            int baseline = y + (int)title_size - (int)body_size;
            if (paragraph_ink) baseline += body_line;
            if (p == ch->heading_start) baseline += (int)body_size;
            /* A fresh page must consume title text even if it is too short
               for the usual leading space. Continuation pages omit that space. */
            if (baseline > bottom && (paragraph_ink || y > top + (int)body_size)) break;
            y = baseline > bottom ? bottom : baseline;
            heading = true;
            thingsos_font_size(f, title_size);
            line = title_line;
            x = 0;
            paragraph_ink = paragraph_head = false;
            if(planned)planned->end=planned->count=0;
            continue;
        }
        const thingsos_inline_t* img = text_image_at(ch, p);
        if (img) {
            if(planned)planned->end=planned->count=0;
            bool cover=ch->cover_image && img==ch->images+ch->cover_image-1;
            if(cover && !paragraph_ink && y==top+(int)body_size)y=top+12;
            int image_x=left,image_y=y,image_width=width,below=0;
            int picture_height=294,block=294+(int)body_size*2+38;
            bool show_caption=!cover && img->alt[0];
            if(!cover && epub_paragraphs) {
                /* Size against a fresh page. If the whole block cannot fit
                   after the preceding text, start it on the next page. */
                int caption=show_caption?line+16:0;
                if(caption>=height){caption=0;show_caption=false;}
                int limit=height-caption;
                thingsos_size_t fit={(unsigned)width,(unsigned)(limit<294?limit:294)};
                if(img->width && img->height)
                    thingsos_image_contain(f->vertical_text?img->height:img->width,
                        f->vertical_text?img->width:img->height,(unsigned)width,(unsigned)limit,&fit);
                image_y=!paragraph_ink && y==top+(int)body_size?top:y+(paragraph_ink?12:0);
                image_width=(int)fit.width;
                image_x=left+(width-image_width)/2;
                picture_height=(int)fit.height;
                block=picture_height+caption;
                below=(int)body_size+12;
            } else if(!cover && img->width && img->height) {
                int caption=img->alt[0]?line+16:0;
                int limit=height-2*(int)body_size-2*line-caption-12;
                if(limit<height/3)limit=height-2*(int)body_size-line-caption-12;
                if(limit<1)break;
                uint64_t natural=(uint64_t)(unsigned)width*(f->vertical_text?img->width:img->height)/
                    (f->vertical_text?img->height:img->width);
                picture_height=natural>(unsigned)limit?limit:natural?(int)natural:1;
                /* y is a text baseline. Reserve the next glyph's ascent too,
                   so a following title cannot paint into the image/hint. */
                block=picture_height+caption+(int)body_size+12;
            }
            if(cover) {
                /* A first-page cover owns the remaining reading area. Layout
                   and drawing share this calculation, at every font size. */
                int available=bottom-y;
                if(available<=24 || (available<height*3/4 && p>start))break;
                picture_height=available-12;block=available;
            }
            if (image_y + block > bottom) break;
            if (picture && c) picture(context, img, c, image_x, image_y, image_width, picture_height);
            if (show_caption && c) thingsos_label(f, c, img->alt, left, image_y + picture_height + (int)body_size + 8, width);
            y=image_y+block+below;
            x = 0;
            paragraph_ink=false;
            paragraph_head=false;
        }
        thingsos_md_run_t style={0};
        if(md) {
            if(!thingsos_markdown_style(md,p,&style)){if(completed)*completed=false;break;}
            if(style.flags&MD_HIDE){
                size_t end=style.end;
                for(size_t i=0;i<ch->image_count;++i)if(ch->images[i].offset>p && ch->images[i].offset<end)end=ch->images[i].offset;
                if(end<=p || end>ch->bytes)break;
                p=end;continue;
            }
            unsigned size=body_size+(style.flags&MD_H1?9:style.flags&MD_H2?6:style.flags&MD_H3?3:0);
            if(size>70)size=70;
            if(size!=f->size) {if(!paragraph_ink)y+=(int)size-(int)f->size;thingsos_font_size(f,size);}
            thingsos_font_weight(f,style.flags&MD_BOLD?2:original_weight);
            f->italic=(style.flags&MD_ITALIC)!=0;
            line=text_line_advance(f,size,line_percent,height);
            if(!paragraph_ink && (style.flags&(MD_H1|MD_H2|MD_H3|MD_CODE|MD_LIST|MD_QUOTE))) {
                x=style.flags&(MD_LIST|MD_QUOTE)?20:0;paragraph_head=false;
            }
            if((style.flags&(MD_QUOTE|MD_LIST)) && x<20)x=20;
        }
        unsigned bytes;
        uint32_t cp = next(ch->text + p, ch->bytes - p, &bytes);
        bool collapse_paragraphs = epub_paragraphs || (md && !(style.flags & MD_CODE));
        /* EPUBs often encode Chinese first-line indentation as literal U+3000
           spaces, which survive the XHTML markup parser. Drop only paragraph-
           leading whitespace so every paragraph starts at the shared gutter;
           with the indent setting on, local TXT keeps the same rule so the
           setting alone decides the first-line indent. Markdown outside fenced
           code blocks collapses blank lines the same way as EPUB so \n\n
           paragraph separators stay on the shared baseline grid. */
        if ((collapse_paragraphs || indent_px) && !(style.flags&MD_CODE) && !paragraph_ink && epub_paragraph_indent(cp)) {
            p += bytes;
            continue;
        }
        if (cp == '\r' || cp == '\n') {
            p += bytes;
            size_t line_end = heading ? ch->heading_end : ch->bytes;
            if (cp == '\r' && p < line_end && ch->text[p] == '\n' && !text_image_at(ch,p)) ++p;
            if (heading && p >= ch->heading_end) continue;
            paragraph_head = !heading && indent_px != 0;
            if (epub_paragraphs && !heading) {
                /* Do not skip image anchors, even when they point at whitespace. */
                while (p < ch->bytes && (ch->text[p] == '\n' || ch->text[p] == '\r' ||
                       ch->text[p] == ' ' || ch->text[p] == '\t')) {
                    bool anchor = false;
                    for (size_t i = 0; i < ch->image_count; ++i)
                        if (ch->images[i].offset == p) anchor = true;
                    if (anchor) break;
                    ++p;
                }
            }
            if (collapse_paragraphs && !paragraph_ink) continue;
            x = 0;
            paragraph_ink=false;
            y += line;
            if (y > bottom) break;
            continue;
        }
        if (indent_px && paragraph_head && !title && x == 0) x = (int)indent_px;
        bool note_marker = false;
        const thingsos_footnote_t* note = NULL;
        for (size_t i = 0; i < ch->footnote_count; ++i) {
            if (p < ch->footnotes[i].end && p + bytes > ch->footnotes[i].begin) {
                note_marker = true;
                note = ch->footnotes + i;
                break;
            }
        }
        if(planned && (note_marker || title || img || (planned->end && bytes>planned->end-p)))
            planned->end=planned->count=0;
        if(planned && hanging>=0 && !md && !heading && !title && !img && !note_marker &&
           !planned->end && p>=pair_end && (x==0 || (paragraph_head && x==(int)indent_px))) {
            /* Scan and close the line sequentially so their frames never overlap. */
            unsigned scanned=scan_prose_line(f,ch,p,x,width,scratch);
            plan_prose_line(f,p,width,hanging,scanned,scratch);
            planned_idx=0;
        }
        /* A footnote marker is a superscript: two thirds of the body size, raised
           by a third, so it reads as a reference and is obvious on e-ink. */
        unsigned marker_size = body_size, marker_rise = 0;
        if (note_marker && body_size > 24) {
            marker_size = body_size * 2 / 3;
            marker_rise = body_size / 3;
        }
        unsigned flow_size = f->size;
        int a;
        if (note_marker && marker_rise) {
            thingsos_font_size(f, marker_size);
            a = thingsos_glyph(f, NULL, cp, 0, 0);
            thingsos_font_size(f, flow_size);
        } else
            a = thingsos_glyph(f, NULL, cp, 0, 0);
        bool code_tab=cp=='\t' && (style.flags&MD_CODE);
        if(code_tab){int cell=thingsos_glyph(f,NULL,' ',0,0);if(cell<1)cell=(int)body_size/2;int stop=cell*4;a=stop-x%stop;}
        int shift=0;
        int hit_shift=0, hit_width=a;
        if(planned && planned->end && planned_idx<planned->count) {
            shift=planned->pre_shift[planned_idx];
            a=planned->step_adv[planned_idx++];
            hit_shift=positive(shift);
            hit_width=a-hit_shift;
        } else if(p>=pair_end) {
            /* Keep ellipses and dashes indivisible in title, Markdown and
               vertical flow as well as in horizontal prose. */
            size_t limit=prose_limit(ch,p);
            if(md && style.end<limit)limit=style.end;
            if(heading && ch->heading_end<limit)limit=ch->heading_end;
            int reserve=a;
            size_t unit_end=p+bytes;
            unsigned n;
            if(pair_mark(cp) && unit_end<limit && next(ch->text+unit_end,limit-unit_end,&n)==cp) {
                unit_end+=n;
                reserve+=a;
            } else if(!note_marker && !(style.flags&MD_CODE) && !epub_paragraph_indent(cp) && !line_start_prohibited(cp)) {
                for(size_t look=unit_end;look<limit && reserve<=width;) {
                    uint32_t following=next(ch->text+look,limit-look,&n);
                    if(!line_start_prohibited(following))break;
                    unsigned second;
                    if(pair_mark(following) && look+n<limit && next(ch->text+look+n,limit-look-n,&second)==following)break;
                    int adv=thingsos_glyph(f,NULL,following,0,0);
                    if(adv<=0 || adv>width || reserve>width-adv){reserve=a;break;}
                    reserve+=adv;look+=n;
                }
            }
            int line_left=md && (style.flags&(MD_QUOTE|MD_LIST))?20:0;
            if(x>line_left && (x+a>width || (reserve<=width-line_left && x>width-reserve))) {
                x=line_left;
                if(line<=0)break;
                y+=line;
                if(heading)paragraph_ink=false;
                if(y>bottom)break;
                continue;
            }
            pair_end=unit_end;
        }
        if (y > bottom) break;
        if (c) {
            if (marker_rise) thingsos_font_size(f, marker_size);
            if(style.flags&MD_CODE)thingsos_rect(c,left+x,y-(int)f->size,a,line,235);
            if(style.flags&MD_QUOTE)thingsos_rect(c,left+4,y-(int)f->size,3,line,100);
            if(!code_tab)thingsos_glyph(f, c, cp, left + x + shift, y - (int)marker_rise);
            if(style.flags&MD_LINK)thingsos_rect(c,left+x,y+3,a,1,100);
            if (marker_rise) thingsos_font_size(f, flow_size);
            unsigned underline=0;
            if (!note_marker)
                for(size_t i=0;i<ch->underline_count;++i)if(p<ch->underlines[i].end && p+bytes>ch->underlines[i].begin){
                    unsigned style=ch->underlines[i].mine?2:1;if(style>underline)underline=style;
                }
            int underline_offset=line-(int)f->size-4;
            if(underline_offset>10)underline_offset=10;
            if(underline_offset<1)underline_offset=1;
            if(underline)for(int xx=left+x;xx<left+x+a;++xx)if(underline==2 || xx%7<3)
                thingsos_rect(c,xx,y+underline_offset,1,3,0);
            /* Dotted rule right under the raised marker: the link affordance. */
            if(note_marker)for(int xx=left+x;xx<left+x+a;++xx)if(xx%4<2)
                thingsos_rect(c,xx,y+6-(int)marker_rise,1,2,0);
        }
        if(hits && !note_marker && hit_width>0 && hits->glyph_count<THINGSOS_GLYPH_HIT_LIMIT) {
            hits->glyphs[hits->glyph_count++]=(thingsos_glyph_hit_t){(uint32_t)p,(uint16_t)bytes,
                (int16_t)(left+x+hit_shift),(int16_t)(y-(int)f->size),(int16_t)hit_width,(int16_t)(f->size+(f->vertical_text?0:10))};
        }
        if(hits && !note_marker && a>0)for(size_t i=0;i<ch->underline_count;++i) {
            const thingsos_text_underline_t* u=ch->underlines+i;
            if(p>=u->end || p+bytes<=u->begin)continue;
            int upper=y-(int)f->size-3,lower=y+10;
            if(upper<top)upper=top;
            if(lower>bottom)lower=bottom;
            if(lower<=upper)continue;
            thingsos_text_hit_t* hit=NULL;
            for(size_t j=0;j<hits->line_count;++j)if(hits->lines[j].begin==u->begin && hits->lines[j].y==upper){hit=hits->lines+j;break;}
            if(hit){int right=left+x+a+3;if(right>hit->x+hit->w)hit->w=right-hit->x;}
            else if(hits->line_count<THINGSOS_LINE_HIT_LIMIT)hits->lines[hits->line_count++]=(thingsos_text_hit_t){u->begin,u->end,left+x-3,upper,a+6,lower-upper};
        }
        if (note)
            text_hit_add(hits, note, left + x - 6,
                         y - (int)marker_rise - (int)marker_size - 4, a + 12,
                         (int)marker_rise + (int)marker_size + 10, top, bottom);
        x += a;
        if(cp!=' ' && cp!='\t') {paragraph_ink=true;paragraph_head=false;}
        p += bytes;
    }
    if(cursor) *cursor=(thingsos_text_cursor_t){p,x,y,line,f->size,paused,heading,paragraph_ink,paragraph_head};
    thingsos_font_size(f,body_size);thingsos_font_weight(f,original_weight);f->italic=original_italic;
    thingsos_markdown_cursor_close(md);
    return p;
}
static size_t text_view_page(
    thingsos_font_t* f, thingsos_canvas_t* c, const text_view_t* ch, size_t start, int left,
    int top, int width, int height, unsigned line_percent, unsigned indent_px,
    bool epub_paragraphs, thingsos_picture_fn picture, void* context, thingsos_text_hits_t* hits,
    thingsos_layout_continue_fn proceed, void* proceed_context, bool* completed,
    thingsos_text_cursor_t* cursor, int hanging) {
    prose_scratch_t* scratch=hanging>=0 && ch && !ch->markdown && start<ch->bytes
        ?calloc(1,sizeof(*scratch)):NULL;
    size_t result=text_view_page_impl(f,c,ch,start,left,top,width,height,line_percent,indent_px,
        epub_paragraphs,picture,context,hits,proceed,proceed_context,completed,cursor,hanging,scratch);
    free(scratch);
    return result;
}
#include "vertical_text.inc"
size_t thingsos_text_page_resume_flow(thingsos_font_t* f,thingsos_canvas_t* c,const thingsos_chapter_t* ch,size_t start,int left,int top,int width,int height,unsigned line_percent,unsigned indent_px,bool epub_paragraphs,thingsos_picture_fn picture,void* context,thingsos_text_hits_t* hits,thingsos_layout_continue_fn proceed,void* proceed_context,bool* completed,thingsos_text_cursor_t* cursor,bool vertical,int hanging){
    if(!ch){if(completed)*completed=true;return 0;}
    text_view_t view={ch->text,ch->bytes,ch->images,ch->image_count,ch->heading_start,ch->heading_end,ch->hidden_start,ch->hidden_end,ch->heading,ch->underlines,ch->underline_count,ch->footnotes,ch->footnote_count,ch->markdown_index?ch:NULL,ch->cover_image};
    if(vertical && f)return vertical_text_page(f,c,&view,start,left,top,width,height,line_percent,indent_px,epub_paragraphs,picture,context,hits,proceed,proceed_context,completed,cursor);
    return text_view_page(f,c,&view,start,left,top,width,height,line_percent,indent_px,epub_paragraphs,picture,context,hits,proceed,proceed_context,completed,cursor,hanging);
}
size_t thingsos_text_page_resume(thingsos_font_t* f,thingsos_canvas_t* c,const thingsos_chapter_t* ch,size_t start,int left,int top,int width,int height,unsigned line_percent,unsigned indent_px,bool epub_paragraphs,thingsos_picture_fn picture,void* context,thingsos_text_hits_t* hits,thingsos_layout_continue_fn proceed,void* proceed_context,bool* completed,thingsos_text_cursor_t* cursor){
    return thingsos_text_page_resume_flow(f,c,ch,start,left,top,width,height,line_percent,indent_px,epub_paragraphs,picture,context,hits,proceed,proceed_context,completed,cursor,false,-1);
}
size_t thingsos_text_page_layout_flow(thingsos_font_t* f,thingsos_canvas_t* c,const thingsos_chapter_t* ch,size_t start,int left,int top,int width,int height,unsigned line_percent,unsigned indent_px,bool epub_paragraphs,thingsos_picture_fn picture,void* context,thingsos_text_hits_t* hits,bool vertical,int hanging){
    return thingsos_text_page_resume_flow(f,c,ch,start,left,top,width,height,line_percent,indent_px,epub_paragraphs,picture,context,hits,NULL,NULL,NULL,NULL,vertical,hanging);
}
size_t thingsos_text_page_interruptible(thingsos_font_t* f,thingsos_canvas_t* c,const thingsos_chapter_t* ch,size_t start,int left,int top,int width,int height,unsigned line_percent,unsigned indent_px,bool epub_paragraphs,thingsos_picture_fn picture,void* context,thingsos_text_hits_t* hits,thingsos_layout_continue_fn proceed,void* proceed_context,bool* completed){
    return thingsos_text_page_resume(f,c,ch,start,left,top,width,height,line_percent,indent_px,epub_paragraphs,picture,context,hits,proceed,proceed_context,completed,NULL);
}
size_t thingsos_text_string_page(thingsos_font_t* f,thingsos_canvas_t* c,const char* text,size_t bytes,size_t start,int left,int top,int width,int height,unsigned line_percent){
    text_view_t view={0};view.text=text;view.bytes=bytes;
    return text_view_page(f,c,&view,start,left,top,width,height,line_percent,0,false,NULL,NULL,NULL,NULL,NULL,NULL,NULL,-1);
}
size_t thingsos_text_page_layout(
    thingsos_font_t* f, thingsos_canvas_t* c, const thingsos_chapter_t* ch, size_t start,
    int left,int top,int width,int height,unsigned line_percent,unsigned indent_px,
    bool epub_paragraphs,thingsos_picture_fn picture,void* context,thingsos_text_hits_t* hits) {
    return thingsos_text_page_interruptible(f,c,ch,start,left,top,width,height,line_percent,
        indent_px,epub_paragraphs,picture,context,hits,NULL,NULL,NULL);
}
size_t thingsos_text_page(
    thingsos_font_t* f, thingsos_canvas_t* c, const thingsos_chapter_t* ch, size_t start,
    int left, int top, int width, int height, thingsos_picture_fn picture, void* context) {
    return thingsos_text_page_layout(f, c, ch, start, left, top, width, height, 175, 0, false,
                                   picture, context, NULL);
}
