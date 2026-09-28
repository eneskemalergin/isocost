/*
 * SVG backend. Glyph outlines are defined once in <defs> and placed with
 * <use>, so text looks the same in every viewer without a font lookup. The
 * page size is written in points so print layouts get the physical size.
 */
#include "canvas.h"
#include "platform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void num(Str *s, double v) {
    char b[32];
    if (fabs(v - round(v)) < 0.005) snprintf(b, sizeof(b), "%.0f", round(v));
    else {
        snprintf(b, sizeof(b), "%.2f", v);
        size_t n = strlen(b);
        while (b[n - 1] == '0') b[--n] = '\0';
    }
    if (strcmp(b, "-0") == 0) snprintf(b, sizeof(b), "0");
    str_append(s, b);
}

static void color(Str *s, Rgb c) { str_printf(s, "#%02x%02x%02x", c.r, c.g, c.b); }

static void xml(Str *s, const char *text) {
    for (; *text; text++) {
        switch (*text) {
        case '&': str_append(s, "&amp;"); break;
        case '<': str_append(s, "&lt;"); break;
        case '>': str_append(s, "&gt;"); break;
        case '"': str_append(s, "&quot;"); break;
        default: str_appendn(s, text, 1);
        }
    }
}

typedef struct {
    Str *s;
    int started;
} PathSink;

static void p_move(void *ctx, float x, float y) {
    PathSink *p = ctx;
    str_append(p->s, "M");
    num(p->s, x);
    str_append(p->s, " ");
    num(p->s, y);
}

static void p_line(void *ctx, float x, float y) {
    PathSink *p = ctx;
    str_append(p->s, "L");
    num(p->s, x);
    str_append(p->s, " ");
    num(p->s, y);
}

static void p_quad(void *ctx, float cx, float cy, float x, float y) {
    PathSink *p = ctx;
    str_append(p->s, "Q");
    num(p->s, cx);
    str_append(p->s, " ");
    num(p->s, cy);
    str_append(p->s, " ");
    num(p->s, x);
    str_append(p->s, " ");
    num(p->s, y);
}

static void p_close(void *ctx) { str_append(((PathSink *)ctx)->s, "Z"); }

static const OutlineSink svg_sink = {p_move, p_line, p_quad, p_close};

static void paint(Str *s, const Op *op) {
    if (op->fill) {
        str_append(s, " fill=\"");
        color(s, op->fill_color);
        str_append(s, "\"");
        if (op->fill_alpha < 1) str_printf(s, " fill-opacity=\"%.3g\"", op->fill_alpha);
    } else {
        str_append(s, " fill=\"none\"");
    }
    if (op->stroke) {
        str_append(s, " stroke=\"");
        color(s, op->stroke_color);
        str_append(s, "\" stroke-width=\"");
        num(s, op->stroke_width);
        str_append(s, "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"");
        if (op->stroke_alpha < 1) str_printf(s, " stroke-opacity=\"%.3g\"", op->stroke_alpha);
        if (op->dash_on > 0 || op->dash_off > 0) {
            str_append(s, " stroke-dasharray=\"");
            num(s, op->dash_on > 0 ? op->dash_on : 0.01);
            str_append(s, " ");
            num(s, op->dash_off);
            str_append(s, "\"");
        }
    }
}

static void finish(Str *s, const Op *op, const char *tag) {
    if (op->tooltip) {
        str_append(s, "><title>");
        xml(s, op->tooltip);
        str_printf(s, "</title></%s>\n", tag);
    } else {
        str_append(s, "/>\n");
    }
}

static float text_start(const Op *op) {
    if (op->anchor == ANCHOR_START) return op->x;
    float w = font_width(op->text, op->size, op->bold);
    return op->anchor == ANCHOR_MIDDLE ? op->x - w / 2 : op->x - w;
}

static void glyph_uses(Str *s, const Op *op, unsigned char *used) {
    PlacedGlyph glyphs[512];
    size_t n = font_layout(op->text, op->size, op->bold, glyphs, 512);
    float x = text_start(op), scale = op->size / (float)font_units_per_em(op->bold);
    int count = font_glyph_count(0);
    for (size_t i = 0; i < n; i++) {
        used[op->bold * count + glyphs[i].glyph] = 1;
        str_printf(s, "<use href=\"#%c%d\" transform=\"translate(", op->bold ? 'b' : 'r', glyphs[i].glyph);
        num(s, x + glyphs[i].x);
        str_append(s, " ");
        num(s, op->y);
        str_printf(s, ") scale(%.6g %.6g)\"/>", scale, -scale);
    }
}

int render_svg(const Scene *scene, const char *path) {
    Str body = {0}, out = {0};
    int count = font_glyph_count(0) > font_glyph_count(1) ? font_glyph_count(0) : font_glyph_count(1);
    unsigned char *used = xcalloc((size_t)count * 2, 1);
    int clip_id = 0, in_clip = 0;
    for (size_t i = 0; i < scene->ops.count; i++) {
        const Op *op = scene->ops.items[i];
        switch (op->kind) {
        case OP_CLIP:
            if (in_clip) str_append(&body, "</g>\n");
            clip_id++;
            str_printf(&body, "<clipPath id=\"c%d\"><rect x=\"", clip_id);
            num(&body, op->x);
            str_append(&body, "\" y=\"");
            num(&body, op->y);
            str_append(&body, "\" width=\"");
            num(&body, op->w);
            str_append(&body, "\" height=\"");
            num(&body, op->h);
            str_printf(&body, "\"/></clipPath>\n<g clip-path=\"url(#c%d)\">\n", clip_id);
            in_clip = 1;
            break;
        case OP_UNCLIP:
            if (in_clip) str_append(&body, "</g>\n");
            in_clip = 0;
            break;
        case OP_RECT:
            str_append(&body, "<rect x=\"");
            num(&body, op->x);
            str_append(&body, "\" y=\"");
            num(&body, op->y);
            str_append(&body, "\" width=\"");
            num(&body, op->w);
            str_append(&body, "\" height=\"");
            num(&body, op->h);
            str_append(&body, "\"");
            paint(&body, op);
            finish(&body, op, "rect");
            break;
        case OP_CIRCLE:
            str_append(&body, "<circle cx=\"");
            num(&body, op->x);
            str_append(&body, "\" cy=\"");
            num(&body, op->y);
            str_append(&body, "\" r=\"");
            num(&body, op->r);
            str_append(&body, "\"");
            paint(&body, op);
            finish(&body, op, "circle");
            break;
        case OP_PATH:
            str_append(&body, "<path d=\"");
            for (int k = 0; k < op->count; k++) {
                str_append(&body, k ? "L" : "M");
                num(&body, op->points[2 * k]);
                str_append(&body, " ");
                num(&body, op->points[2 * k + 1]);
            }
            if (op->closed) str_append(&body, "Z");
            str_append(&body, "\"");
            paint(&body, op);
            finish(&body, op, "path");
            break;
        case OP_TEXT: {
            float scale = op->size / (float)font_units_per_em(op->bold);
            if (op->vertical) {
                str_append(&body, "<g transform=\"rotate(-90 ");
                num(&body, op->x);
                str_append(&body, " ");
                num(&body, op->y);
                str_append(&body, ")\">");
            }
            str_append(&body, "<g aria-label=\"");
            xml(&body, op->text);
            str_append(&body, "\"");
            if (op->halo > 0) {
                str_append(&body, " stroke=\"");
                color(&body, scene->background);
                str_printf(&body, "\" stroke-width=\"%.4g\" stroke-linejoin=\"round\" stroke-opacity=\"0.9\" fill=\"", 2 * op->halo / scale);
                color(&body, scene->background);
                str_append(&body, "\" fill-opacity=\"0.9\">");
                glyph_uses(&body, op, used);
                str_append(&body, "</g>\n<g aria-hidden=\"true\"");
            }
            str_append(&body, " fill=\"");
            color(&body, op->fill_color);
            str_append(&body, "\"");
            if (op->fill_alpha < 1) str_printf(&body, " fill-opacity=\"%.3g\"", op->fill_alpha);
            str_append(&body, ">");
            if (op->tooltip) {
                str_append(&body, "<title>");
                xml(&body, op->tooltip);
                str_append(&body, "</title>");
            }
            glyph_uses(&body, op, used);
            str_append(&body, op->vertical ? "</g></g>\n" : "</g>\n");
            break;
        }
        }
    }
    if (in_clip) str_append(&body, "</g>\n");

    str_append(&out, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"");
    num(&out, scene->width_pt);
    str_append(&out, "pt\" height=\"");
    num(&out, scene->height_pt);
    str_printf(&out, "pt\" viewBox=\"0 0 %d %d\" role=\"img\">\n<title>", scene->width, scene->height);
    xml(&out, scene->title);
    str_append(&out, "</title>\n<defs>\n");
    for (int bold = 0; bold < 2; bold++)
        for (int g = 0; g < font_glyph_count(bold); g++) {
            if (!used[bold * font_glyph_count(0) + g]) continue;
            PathSink sink = {&out, 0};
            str_printf(&out, "<path id=\"%c%d\" d=\"", bold ? 'b' : 'r', g);
            font_outline(bold, g, &svg_sink, &sink);
            str_append(&out, "\"/>\n");
        }
    str_append(&out, "</defs>\n<rect width=\"100%\" height=\"100%\" fill=\"");
    color(&out, scene->background);
    str_append(&out, "\"/>\n");
    str_append(&out, str_cstr(&body));
    str_append(&out, "</svg>\n");
    int ok = pf_write_file_atomic(path, out.data, out.length);
    str_free(&body);
    str_free(&out);
    free(used);
    return ok;
}
