#include "canvas.h"

#include <stdlib.h>
#include <string.h>

Scene *scene_new(int width, int height, double dpi, double width_pt, double height_pt, Rgb background, const char *title) {
    Scene *s = xcalloc(1, sizeof(*s));
    s->width = width;
    s->height = height;
    s->dpi = dpi;
    s->width_pt = width_pt;
    s->height_pt = height_pt;
    s->background = background;
    s->title = xstrdup(title);
    return s;
}

void scene_free(Scene *s) {
    if (!s) return;
    for (size_t i = 0; i < s->ops.count; i++) {
        Op *op = s->ops.items[i];
        free(op->points);
        free(op->text);
        free(op->tooltip);
        free(op);
    }
    vec_free(&s->ops);
    free(s->title);
    free(s);
}

static Op *push(Scene *s, int kind) {
    Op *op = xcalloc(1, sizeof(*op));
    op->kind = kind;
    op->fill_alpha = op->stroke_alpha = 1;
    vec_push(&s->ops, op);
    return op;
}

Op *scene_path(Scene *s, const float *points, int count, int closed) {
    Op *op = push(s, OP_PATH);
    op->points = xmalloc(sizeof(float) * 2 * (size_t)(count > 0 ? count : 1));
    if (count > 0) memcpy(op->points, points, sizeof(float) * 2 * (size_t)count);
    op->count = count;
    op->closed = closed;
    return op;
}

Op *scene_line(Scene *s, float x0, float y0, float x1, float y1) {
    float p[4] = {x0, y0, x1, y1};
    return scene_path(s, p, 2, 0);
}

Op *scene_circle(Scene *s, float cx, float cy, float r) {
    Op *op = push(s, OP_CIRCLE);
    op->x = cx;
    op->y = cy;
    op->r = r;
    return op;
}

Op *scene_rect(Scene *s, float x, float y, float w, float h) {
    Op *op = push(s, OP_RECT);
    op->x = x;
    op->y = y;
    op->w = w;
    op->h = h;
    return op;
}

Op *scene_text(Scene *s, float x, float y, const char *text, float size, int bold, Rgb color, int anchor) {
    Op *op = push(s, OP_TEXT);
    op->x = x;
    op->y = y;
    op->text = xstrdup(text);
    op->size = size;
    op->bold = bold;
    op->anchor = anchor;
    op->fill = 1;
    op->fill_color = color;
    return op;
}

void scene_clip(Scene *s, float x, float y, float w, float h) {
    Op *op = push(s, OP_CLIP);
    op->x = x;
    op->y = y;
    op->w = w;
    op->h = h;
}

void scene_unclip(Scene *s) { push(s, OP_UNCLIP); }

Op *op_fill(Op *op, Rgb color, float alpha) {
    op->fill = 1;
    op->fill_color = color;
    op->fill_alpha = alpha;
    return op;
}

Op *op_stroke(Op *op, Rgb color, float width, float alpha) {
    op->stroke = 1;
    op->stroke_color = color;
    op->stroke_width = width;
    op->stroke_alpha = alpha;
    return op;
}

Op *op_dash(Op *op, float on, float off) {
    op->dash_on = on;
    op->dash_off = off;
    return op;
}

void op_tooltip(Op *op, const char *text) {
    free(op->tooltip);
    op->tooltip = xstrdup(text);
}
