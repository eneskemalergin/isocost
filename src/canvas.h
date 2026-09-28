/*
 * Display list and the three backends. A view builds one Scene in pixel
 * coordinates; render_png, render_svg, and render_pdf draw the same list.
 */
#ifndef ISOCOST_CANVAS_H
#define ISOCOST_CANVAS_H

#include "config.h"
#include "util.h"

enum { OP_PATH, OP_CIRCLE, OP_RECT, OP_TEXT, OP_CLIP, OP_UNCLIP };
enum { ANCHOR_START, ANCHOR_MIDDLE, ANCHOR_END };

typedef struct {
    int kind;
    float *points;            /* path: x0,y0,x1,y1,... */
    int count;
    int closed;
    float x, y, r, w, h;      /* circle center and radius; rect; text origin (baseline) */
    int fill;
    Rgb fill_color;
    float fill_alpha;
    int stroke;
    Rgb stroke_color;
    float stroke_alpha;
    float stroke_width;
    float dash_on, dash_off;  /* 0 = solid */
    char *text;
    float size;               /* text size in pixels (em) */
    int bold;
    int anchor;
    int vertical;             /* text turned 90 degrees counterclockwise about its origin (reads bottom to top) */
    float halo;               /* halo width in pixels, 0 = none */
    char *tooltip;            /* SVG <title> */
} Op;

typedef struct {
    int width, height;        /* pixels */
    double dpi;
    double width_pt, height_pt;
    Rgb background;
    char *title;
    Vec ops;                  /* Op* */
} Scene;

Scene *scene_new(int width, int height, double dpi, double width_pt, double height_pt, Rgb background, const char *title);
void scene_free(Scene *s);
Op *scene_path(Scene *s, const float *points, int count, int closed);
Op *scene_line(Scene *s, float x0, float y0, float x1, float y1);
Op *scene_circle(Scene *s, float cx, float cy, float r);
Op *scene_rect(Scene *s, float x, float y, float w, float h);
Op *scene_text(Scene *s, float x, float y, const char *text, float size, int bold, Rgb color, int anchor);
void scene_clip(Scene *s, float x, float y, float w, float h);
void scene_unclip(Scene *s);
Op *op_fill(Op *op, Rgb color, float alpha);
Op *op_stroke(Op *op, Rgb color, float width, float alpha);
Op *op_dash(Op *op, float on, float off);
void op_tooltip(Op *op, const char *text);
Op *op_vertical(Op *op);

/* ---- font (font.c) ---- */
typedef struct {
    int glyph;                /* index into the face */
    float x;                  /* pen position in pixels relative to the text origin */
} PlacedGlyph;

void font_init(void);  /* once, before any text is measured */
float font_width(const char *text, float size, int bold);
float font_cap_height(float size, int bold);   /* height of capital letters */
float font_descender(float size, int bold);    /* positive depth below the baseline */
/* Lay out UTF-8 text; returns the glyph count (at most max). */
size_t font_layout(const char *text, float size, int bold, PlacedGlyph *out, size_t max);
int font_units_per_em(int bold);
int font_glyph_count(int bold);

typedef struct {
    void (*move)(void *ctx, float x, float y);
    void (*line)(void *ctx, float x, float y);
    void (*quad)(void *ctx, float cx, float cy, float x, float y);
    void (*close)(void *ctx);
} OutlineSink;

/* Emit a glyph outline in font units, y up. */
void font_outline(int bold, int glyph, const OutlineSink *sink, void *ctx);

/* ---- backends ---- */

/*
 * Buffers one render thread reuses across figures, so each PNG does not
 * allocate (and page-fault) megabytes again. Pass NULL for a one-off render.
 */
typedef struct {
    unsigned char *image;     /* PNG scanlines: filter byte + RGB per row */
    size_t image_capacity;
    unsigned char *mask;      /* 8-bit coverage; all zero between drawing operations */
    size_t mask_capacity;
    unsigned char *out;       /* compressor output */
    size_t out_capacity;
    int *head, *prev;         /* compressor hash tables */
} Workspace;

Workspace *workspace_new(void);
void workspace_free(Workspace *ws);

int render_png(const Scene *scene, const char *path, Workspace *ws);
int render_svg(const Scene *scene, const char *path);
int render_pdf(const Scene *scene, const char *path);

/* zlib stream (deflate.c), shared by PNG and PDF. canvas_init() must run before any backend. */
void canvas_init(void);
void zlib_compress(const unsigned char *data, size_t length, Workspace *ws, Str *out);
unsigned long crc32_bytes(unsigned long crc, const unsigned char *data, size_t length);

#endif
