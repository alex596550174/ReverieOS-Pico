// SPDX-License-Identifier: Apache-2.0
#include "thingsos/graphics.h"
#include "thingsos/markdown.h"
#include "thingsos/pagination.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "clreq line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { WIDTH=684, HEIGHT=1216, LEFT=48, TOP=118, PX=40 };
static unsigned char full[WIDTH*HEIGHT], sliced[WIDTH*HEIGHT];
static thingsos_text_hits_t hits, resumed;
static thingsos_font_t font;
static thingsos_chapter_t chapter;
static const thingsos_canvas_t physical = {full, WIDTH, HEIGHT};

typedef struct { int width, height, hanging; unsigned spacing, indent; bool vertical; } layout_t;
static const layout_t standard = {588, 980, 20, 145, 0, false};

static void text(const char* source) {
    chapter = (thingsos_chapter_t){.text=(char*)source, .bytes=strlen(source)};
}

static size_t layout(layout_t cfg, size_t start, thingsos_canvas_t* canvas, thingsos_text_hits_t* result) {
    CHECK(thingsos_font_size(&font,PX));
    return thingsos_text_page_layout_flow(&font, canvas, &chapter, start, LEFT, TOP, cfg.width, cfg.height,
        cfg.spacing, cfg.indent, !chapter.markdown_index, NULL, NULL, result, cfg.vertical, cfg.hanging);
}

static size_t paint(layout_t cfg, size_t start) {
    thingsos_canvas_t canvas = physical;
    memset(full,255,sizeof(full));
    memset(&hits,0,sizeof(hits));
    size_t end = layout(cfg,start,&canvas,&hits);
    CHECK(end == layout(cfg,start,NULL,NULL));
    return end;
}

static const thingsos_glyph_hit_t* glyph(size_t offset) {
    for (size_t i=0;i<hits.glyph_count;++i)
        if (hits.glyphs[i].offset==offset) return &hits.glyphs[i];
    fprintf(stderr,"missing glyph at byte %zu in %s\n",offset,chapter.text);
    exit(1);
}

static int advance(uint32_t cp) { return thingsos_glyph(&font,NULL,cp,0,0); }

static int ink_edge(int top, int bottom, bool right) {
    for (int x=right?WIDTH-1:0; x>=0 && x<WIDTH; x+=right?-1:1)
        for (int y=top; y<bottom; ++y) if (full[y*WIDTH+x]<200) return x;
    return -1;
}

static void opening_marks(void) {
    static const struct { const char* utf8; uint32_t cp; } open[] = {
        {"（",0xff08}, {"“",0x201c}, {"《",0x300a}, {"「",0x300c}
    };
    layout_t cfg = standard;
    char source[96];
    int cell = advance(0x4e2d);
    for (size_t i=0;i<sizeof(open)/sizeof(open[0]);++i) {
        snprintf(source,sizeof(source),"中中中%s文",open[i].utf8);
        text(source);
        cfg.width = 3*cell + advance(open[i].cp);
        CHECK(paint(cfg,0)==chapter.bytes);
        CHECK(glyph(9)->y > glyph(6)->y && glyph(9)->x == LEFT && glyph(9)->y == glyph(12)->y);
    }
    text("中中中（“正文");
    cfg.width = 3*cell + advance(0xff08) + advance(0x201c) - 5;
    CHECK(paint(cfg,0)==chapter.bytes);
    CHECK(glyph(9)->y>glyph(6)->y && glyph(9)->y==glyph(12)->y && glyph(12)->y==glyph(15)->y);

    for (size_t i=0;i<2;++i) for (unsigned indent=0;indent<=2*PX;indent+=2*PX) {
        cfg.width=standard.width;
        cfg.indent=indent;
        snprintf(source,sizeof(source),"%s文",open[i].utf8);
        text(source);
        CHECK(paint(cfg,0)==chapter.bytes);
        int width = glyph(3)->x-glyph(0)->x;
        int trim = advance(open[i].cp)-width;
        CHECK(trim>0 && trim<=PX/2 && width==glyph(0)->w && glyph(0)->x==LEFT+(int)indent);
        CHECK(ink_edge(TOP-2,TOP+PX+10,false)>=LEFT+(int)indent);
        snprintf(source,sizeof(source),"中%s文",open[i].utf8);
        text(source);
        CHECK(paint(cfg,0)==chapter.bytes && glyph(6)->x-glyph(3)->x==advance(open[i].cp));
        if (!indent) printf("lead trim %s %d px\n",open[i].utf8,trim);
    }
}

static void mixed_spacing(void) {
    text("中文ABC测试123结束");
    CHECK(paint(standard,0)==chapter.bytes && hits.glyph_count==12);
    static const unsigned transitions[] = {1,4,6,9};
    for (unsigned i=0;i<4;++i) {
        const thingsos_glyph_hit_t* before = &hits.glyphs[transitions[i]], *after=before+1;
        CHECK(after->y==before->y && abs(after->x-before->x-before->w-PX/4)<=1);
    }
    printf("mixed CJK/Latin and digit gaps %d px\n",hits.glyphs[2].x-hits.glyphs[1].x-hits.glyphs[1].w);
    text("中文，ABC。123");
    CHECK(paint(standard,0)==chapter.bytes);
    CHECK(glyph(6)->x==glyph(3)->x+glyph(3)->w && glyph(9)->x==glyph(6)->x+glyph(6)->w);
    CHECK(glyph(12)->x==glyph(11)->x+glyph(11)->w && glyph(15)->x==glyph(12)->x+glyph(12)->w);
    text("ABC中文");
    CHECK(paint(standard,0)==chapter.bytes && glyph(0)->x==LEFT);
    text("中文ABC");
    CHECK(paint(standard,0)==chapter.bytes && glyph(8)->w==advance('C'));
    layout_t cfg=standard;
    cfg.width=2*advance(0x4e2d)+advance('A')+PX/4-1;
    CHECK(paint(cfg,0)==chapter.bytes && glyph(6)->x==LEFT && glyph(6)->y>glyph(3)->y);
}

static void trailing_marks(void) {
    static const struct { const char* utf8; uint32_t cp; } points[] = {
        {"。",0x3002}, {"，",0xff0c}, {"、",0x3001}, {"．",0xff0e}, {"：",0xff1a}, {"；",0xff1b}
    };
    static const char* const closed[] = {"”","’","）","》","〉","」","』","】","〕","〗","］","｝","！","？"};
    int cell=advance(0x4e2d), furthest=0;
    char source[64];
    layout_t cfg=standard;
    for (size_t i=0;i<sizeof(points)/sizeof(points[0]);++i) {
        int ink_right;
        CHECK(thingsos_text_ink_box(&font,points[i].utf8,200,NULL,&ink_right,NULL,NULL,NULL));
        cfg.width=4*cell+ink_right-7;
        snprintf(source,sizeof(source),"中中中中%s文",points[i].utf8);
        text(source);
        CHECK(paint(cfg,0)==chapter.bytes);
        CHECK(glyph(12)->y==glyph(0)->y && glyph(15)->y>glyph(12)->y);
        int overflow=ink_edge(TOP-2,TOP+PX+10,true)+1-(LEFT+cfg.width);
        CHECK(overflow>0 && overflow<=cfg.hanging);
        CHECK(glyph(12)->w<advance(points[i].cp));
        furthest=overflow>furthest?overflow:furthest;
        cfg.hanging=0;
        CHECK(paint(cfg,0)==chapter.bytes && glyph(12)->y>glyph(0)->y);
        cfg.hanging=standard.hanging;
    }
    printf("hanging point marks: maximum ink overrun %d px\n",furthest);
    int closest=-PX;
    for (size_t i=0;i<sizeof(closed)/sizeof(closed[0]);++i) {
        int right, mark_width;
        CHECK(thingsos_text_ink_box(&font,closed[i],200,NULL,&right,NULL,NULL,&mark_width));
        snprintf(source,sizeof(source),"中中中中%s文",closed[i]);
        text(source);
        cfg.width=4*cell+right-7;
        CHECK(paint(cfg,0)==chapter.bytes && glyph(12)->y>glyph(0)->y);
        CHECK(ink_edge(TOP-2,TOP+PX+10,true)<LEFT+cfg.width);
        cfg.width=4*cell+mark_width;
        int free_tail=mark_width-right-1;
        if (free_tail>PX/2) free_tail=PX/2;
        if (free_tail>mark_width/2) free_tail=mark_width/2;
        if (free_tail>4) cfg.width-=free_tail-4;
        CHECK(paint(cfg,0)==chapter.bytes && glyph(12)->y==glyph(0)->y);
        if (free_tail>2) CHECK(glyph(12)->w<mark_width);
        int overflow=ink_edge(TOP-2,TOP+PX+10,true)+1-(LEFT+cfg.width);
        CHECK(overflow<=0);
        if (i<12 && overflow>closest) closest=overflow;
    }
    printf("closing quote/bracket ink at most content right %+d px\n",closest);
}

static void paired_marks(void) {
    static const char* const pairs[] = {"……","——"};
    int cell=advance(0x4e2d);
    char source[80];
    unsigned cases=0;
    for (unsigned type=0;type<2;++type) for (int delta=-5;delta<=5;++delta) {
        snprintf(source,sizeof(source),"中中中%s%s文",pairs[type],pairs[type]);
        text(source);
        layout_t cfg=standard;
        cfg.width=4*cell+delta;
        CHECK(paint(cfg,0)==chapter.bytes);
        CHECK(glyph(9)->y==glyph(12)->y && glyph(15)->y==glyph(18)->y);
        cfg.height=PX;
        size_t first=paint(cfg,0);
        CHECK(first>0 && first!=12 && first!=18);
        cfg=standard;
        cfg.vertical=true;
        cfg.height=4*cell+delta;
        CHECK(paint(cfg,0)==chapter.bytes);
        CHECK(glyph(9)->x==glyph(12)->x && glyph(15)->x==glyph(18)->x);
        cases+=2;
    }
    /* The same units advance at narrow width and before an odd third mark. */
    for (unsigned type=0;type<2;++type) {
        snprintf(source,sizeof(source),"中%s%.3s文",pairs[type],pairs[type]);
        text(source);
        layout_t cfg=standard;
        cfg.width=cell+1;
        CHECK(paint(cfg,0)==chapter.bytes && glyph(3)->y==glyph(6)->y);
        ++cases;
    }
    printf("paired ellipses/dashes: %u cases, 0 splits\n",cases);
}

static void compression_priority(void) {
    text("中，“AB CD中。文");
    CHECK(paint(standard,0)==chapter.bytes);
    int normal_a=glyph(9)->x, normal_space=glyph(11)->w, natural=glyph(17)->x-LEFT+advance(0x3002);
    layout_t cfg=standard;
    cfg.hanging=0;
    /* The allowed pair is compressed in both short and wrapped rows. */
    CHECK(normal_a-LEFT<advance(0x4e2d)+advance(0xff0c)+advance(0x201c));
    bool spaces_used=false, natural_space=false;
    for (int width=natural; width>natural-2*PX; --width) {
        cfg.width=width;
        CHECK(paint(cfg,0)==chapter.bytes);
        if (glyph(17)->y!=glyph(0)->y) break;
        bool space=glyph(11)->w<normal_space;
        CHECK(glyph(9)->x==normal_a);
        CHECK(glyph(11)->w*3>=normal_space*2);
        natural_space|=!space;
        spaces_used|=space;
    }
    CHECK(natural_space && spaces_used);
    text("中中中（“文中");
    int cell=advance(0x4e2d);
    cfg.width=3*cell+advance(0xff08)+advance(0x201c)-5;
    CHECK(paint(cfg,0)==chapter.bytes && glyph(9)->y>glyph(6)->y);
    CHECK(glyph(3)->x> LEFT+cell && glyph(6)->x-glyph(3)->x-cell<=PX/8+1);
    CHECK(glyph(3)->x-LEFT-cell<=PX/8+1);
}

static bool permit_row(void* context) {
    unsigned* rows=context;
    if (!*rows) return false;
    --*rows;
    return true;
}

static void equivalent(layout_t cfg, size_t start) {
    size_t expected=paint(cfg,start), end=start;
    CHECK(expected>start);
    memset(sliced,255,sizeof(sliced));
    memset(&resumed,0,sizeof(resumed));
    thingsos_canvas_t canvas={sliced,WIDTH,HEIGHT};
    thingsos_text_cursor_t cursor={0};
    bool completed=false;
    unsigned calls=0;
    while (!completed) {
        unsigned rows=2;
        CHECK(thingsos_font_size(&font,PX));
        end=thingsos_text_page_resume_flow(&font,&canvas,&chapter,start,LEFT,TOP,cfg.width,cfg.height,
            cfg.spacing,cfg.indent,!chapter.markdown_index,NULL,NULL,&resumed,permit_row,&rows,&completed,&cursor,cfg.vertical,cfg.hanging);
        CHECK(++calls<200 && (completed || cursor.active));
    }
    CHECK(calls>1 && end==expected && !cursor.active);
    CHECK(!memcmp(full,sliced,sizeof(full)) && !memcmp(&hits,&resumed,sizeof(hits)));
}

static void resume_and_annotations(const char* output) {
    static const char paragraph[]="　（“中文ABC测试123结束”，她说。）\n"
        "她沿着江边慢慢向前，今天要读《山路来信》……夜色静了——灯亮了。\n"
        "河岸吹来一点风，读完两页就回去吧。英文有 hello world，数字有2026。\n";
    char source[4096], md[4300], path[1024];
    snprintf(source,sizeof(source),"%s%s%s%s%s%s",paragraph,paragraph,paragraph,paragraph,paragraph,paragraph);
    text(source);
    char* note_text=strstr(source,"2026");
    CHECK(note_text);
    thingsos_footnote_t note={.begin=(size_t)(note_text-source),.end=(size_t)(note_text-source)+4,.note_bytes=5};
    thingsos_text_underline_t line={.begin=strlen("　（“中"),.end=strlen("　（“中文ABC测试"),.mine=true};
    chapter.footnotes=&note;
    chapter.footnote_count=1;
    chapter.underlines=&line;
    chapter.underline_count=1;
    for (unsigned vertical=0;vertical<2;++vertical) for (unsigned indent=0;indent<=2*PX;indent+=2*PX) {
        for (unsigned spacing=125;spacing<=175;spacing+=spacing==125?20:30) {
            layout_t cfg=standard;
            cfg.vertical=vertical!=0;
            cfg.indent=indent;
            cfg.spacing=spacing;
            equivalent(cfg,0);
            CHECK(hits.count==1 && hits.line_count>=1);
        }
    }
    static const char fenced[]="```\n中中中……文——中\n```\n\n";
    snprintf(md,sizeof(md),"%s# 《混排测试》\n\n**中文ABC** _italics_ 中文2026。\n\n%s%s",fenced,source,source);
    text(md);
    CHECK(snprintf(path,sizeof(path),"%s/clreq-layout-test.md",output)<(int)sizeof(path));
    CHECK(thingsos_markdown_prepare(path,&chapter));
    layout_t cfg=standard;
    cfg.width=4*advance(0x4e2d);
    cfg.indent=2*PX;
    equivalent(cfg,0);
    CHECK(glyph(13)->y==glyph(16)->y && glyph(22)->y==glyph(25)->y);
    CHECK(!remove(chapter.markdown_index));
    free(chapter.markdown_index);
    chapter.markdown_index=NULL;
}

static void realpage(const char* output) {
    static const char source[]=
        "我，这到底是个什么东西啊。她说，谢灵通啊，这都不知道。我说，是个人吗，小孩儿？还是动物？"
        "她说，海獭，科学家，背着个蓝色防水包，它很博学的，无所不知，还有个实验室，总钻在里面，但有点恐高，"
        "我儿子特别喜欢它，因为很像他爸。我说，他爸恐高？她说，不是，他爸也是科学家，天天在实验室里，不怎么爱回家。"
        "我说，实验啥？她说，我也搞不清楚，都是专业术语，生物的一类也许，我总想到黎明的那首歌，你还记得吧，"
        "快乐两千年，在实验室里做实验，看看有没有不变的诺言，所以，我觉得可能是诺言吧。我说，这么大岁数了，"
        "能不能正经说话。她说，见了你控制不住。";
    enum { SIZE=42, BODY_TOP=90, ROW=SIZE*145/100, HANGING=25 };
    for(int margin=36;margin<=54;margin+=18) {
        int column=WIDTH-2*margin;
        text(source);
        thingsos_canvas_t canvas=physical;
        memset(full,255,sizeof(full));
        memset(&hits,0,sizeof(hits));
        CHECK(thingsos_font_size(&font,SIZE));
        size_t dry=thingsos_text_page_layout_flow(&font,NULL,&chapter,0,margin,BODY_TOP,column,1040,145,0,
            true,NULL,NULL,NULL,false,HANGING);
        size_t end=thingsos_text_page_layout_flow(&font,&canvas,&chapter,0,margin,BODY_TOP,column,1040,145,0,
            true,NULL,NULL,&hits,false,HANGING);
        CHECK(end==dry && end>0 && hits.glyph_count>2);
        unsigned hanging_rows=0, comma_count=0;
        int min_over=WIDTH,max_over=0,comma_min=WIDTH,comma_max=0,max_body_error=0;
        for(size_t i=0;i<hits.glyph_count;++i) {
            const thingsos_glyph_hit_t* g=hits.glyphs+i;
            bool row_end=i+1==hits.glyph_count || g[1].y!=g->y;
            if(row_end) {
                int over=ink_edge(g->y,g->y+ROW,true)-(margin+column);
                if(over>0) {
                    CHECK((!memcmp(source+g->offset,"，",3) || !memcmp(source+g->offset,"。",3)) && over<=HANGING);
                    CHECK(i && g[-1].y==g->y && g[-1].bytes==3);
                    char before[4]={0};
                    memcpy(before,source+g[-1].offset,3);
                    CHECK((unsigned char)before[0]>=0xe4 && (unsigned char)before[0]<=0xe9);
                    int right;
                    CHECK(thingsos_text_ink_box(&font,before,0,NULL,&right,NULL,NULL,NULL));
                    int error=abs(margin+column-(g[-1].x+right));
                    CHECK(error<=6);
                    if(error>max_body_error)max_body_error=error;
                    ++hanging_rows;
                    if(over<min_over)min_over=over;
                    if(over>max_over)max_over=over;
                }
            } else if(!memcmp(source+g->offset,"，",3) && i && g[-1].y==g->y &&
                      memcmp(source+g[1].offset,"，",3) && memcmp(source+g[1].offset,"。",3)) {
                int step=g[1].x-g->x;
                CHECK(g->w==advance(0xff0c));
                if(step<comma_min)comma_min=step;
                if(step>comma_max)comma_max=step;
                ++comma_count;
            }
        }
        CHECK(comma_count && comma_max-comma_min<=4);
        CHECK(hanging_rows>=2);
        printf("photo text, margin %d px, column %d px: %u hanging rows (%d..%d px); previous Han ink at most %d px from right edge; %u medial commas, step %d..%d px; %zu/%zu bytes\n",
            margin,column,hanging_rows,min_over,max_over,max_body_error,comma_count,comma_min,comma_max,end,chapter.bytes);
        memset(sliced,255,sizeof(sliced));
        memset(&resumed,0,sizeof(resumed));
        thingsos_canvas_t part={sliced,WIDTH,HEIGHT};
        thingsos_text_cursor_t cursor={0};
        bool done=false;
        size_t sliced_end=0;
        unsigned calls=0;
        while(!done) {
            unsigned rows=2;
            sliced_end=thingsos_text_page_resume_flow(&font,&part,&chapter,0,margin,BODY_TOP,column,1040,145,0,
                true,NULL,NULL,&resumed,permit_row,&rows,&done,&cursor,false,HANGING);
            CHECK(++calls<100);
        }
        CHECK(calls>1 && sliced_end==end && !memcmp(full,sliced,sizeof(full)) && !memcmp(&hits,&resumed,sizeof(hits)));
        char path[1024];
        CHECK(snprintf(path,sizeof(path),"%s/clreq-v2-%s.pgm",output,margin==36?"std":"wide")<(int)sizeof(path));
        FILE* file=fopen(path,"wb");
        CHECK(file && fprintf(file,"P5\n%d %d\n255\n",WIDTH,HEIGHT)>0);
        CHECK(fwrite(full,1,sizeof(full),file)==sizeof(full) && !fclose(file));
    }
}

static void preview(const char* output) {
    static const char title[]="第一章　雾里的来信";
    static const char source[]="第一章　雾里的来信\n"
        "　（“今天是第3天吗？”）窗边的人轻轻问。她翻开《山野来信》，纸上有英文：Go slowly。\n"
        "　我走过石桥，望见晨雾缓缓漫过山脊，忽然想起旧日的约定……原来我们始终都没有离开——只是走得更远了。\n"
        "　到河边时，ABC计划已经写到第123页。她把‘秋水’圈起来，又在边上添了一句：“等风停，等雨过，等一封新的来信。”\n"
        "　故事还没有讲完。灯火照着小路、树梢和静静的山坡。";
    text(source);
    chapter.heading=true;
    chapter.heading_end=strlen(title);
    layout_t cfg=standard;
    cfg.indent=2*PX;
    CHECK(paint(cfg,0)==chapter.bytes);
    thingsos_canvas_t canvas=physical;
    CHECK(thingsos_font_size(&font,24));
    thingsos_label(&font,&canvas,"山野来信",LEFT,68,260);
    thingsos_rect(&canvas,LEFT,89,cfg.width,1,180);
    thingsos_label_right(&font,&canvas,"1 / 1",LEFT,1166,cfg.width);
    char path[1024];
    CHECK(snprintf(path,sizeof(path),"%s/clreq-preview.pgm",output)<(int)sizeof(path));
    FILE* file=fopen(path,"wb");
    CHECK(file && fprintf(file,"P5\n%d %d\n255\n",WIDTH,HEIGHT)>0);
    CHECK(fwrite(full,1,sizeof(full),file)==sizeof(full) && !fclose(file));
}

int main(int argc, char** argv) {
    CHECK(argc==3);
    char path[1024];
    CHECK(snprintf(path,sizeof(path),"%s/default.ttf",argv[1])<(int)sizeof(path));
    CHECK(thingsos_font_file(&font,path) && thingsos_font_size(&font,PX));
    CHECK(THINGSOS_LAYOUT_TEXT==34 && THINGSOS_LAYOUT_SECTIONED_TEXT==35 && THINGSOS_LAYOUT_EPUB==36);
    CHECK(thingsos_page_layout_key(33,PX,1,1,2,684,1040)!=thingsos_page_layout_key(THINGSOS_LAYOUT_EPUB,PX,1,1,2,684,1040));
    opening_marks();
    mixed_spacing();
    trailing_marks();
    paired_marks();
    compression_priority();
    resume_and_annotations(argv[2]);
    preview(argv[2]);
    realpage(argv[2]);
    CHECK(!font.errors);
    thingsos_font_close(&font);
    return 0;
}
