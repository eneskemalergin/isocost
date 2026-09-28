/*
 * PDF backend: one page, vector paths, Flate-compressed content.
 *
 * The page content applies one transform so the display list's pixel
 * coordinates (y down) map to points (y up). Each glyph used is a Form
 * XObject built from its outline (quadratic segments converted exactly to
 * cubics), placed with a cm matrix. Alpha uses ExtGState /ca and /CA.
 * The file has no creation date, so identical input gives identical bytes.
 */
#include "canvas.h"
#include "model.h"
#include "platform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void num(Str *s, double v) {
    char b[32];
    if (fabs(v - round(v)) < 0.0005) snprintf(b, sizeof(b), "%.0f", round(v));
    else {
        snprintf(b, sizeof(b), "%.3f", v);
        size_t n = strlen(b);
        while (b[n - 1] == '0') b[--n] = '\0';
    }
    if (strcmp(b, "-0") == 0) snprintf(b, sizeof(b), "0");
    str_append(s, b);
    str_append(s, " ");
}

static void rgb_op(Str *s, Rgb c, const char *op) {
    num(s, c.r / 255.0);
    num(s, c.g / 255.0);
    num(s, c.b / 255.0);
    str_append(s, op);
    str_append(s, "\n");
}

typedef struct {
    double alphas[64];
    int alpha_count;
    unsigned char *used;      /* [face][glyph]: 1 fill, 2 stroke */
    int glyphs_per_face;
} Resources;

static int alpha_index(Resources *r, double a) {
    a = round(a * 1000) / 1000;
    for (int i = 0; i < r->alpha_count; i++)
        if (r->alphas[i] == a) return i;
    if (r->alpha_count == 64) return 0;
    r->alphas[r->alpha_count] = a;
    return r->alpha_count++;
}

static void set_alpha(Str *s, Resources *r, double a) { str_printf(s, "/A%d gs\n", alpha_index(r, a)); }

typedef struct {
    Str *s;
    float px, py;
} CubicSink;

static void c_move(void *ctx, float x, float y) {
    CubicSink *c = ctx;
    num(c->s, x);
    num(c->s, y);
    str_append(c->s, "m\n");
    c->px = x;
    c->py = y;
}

static void c_line(void *ctx, float x, float y) {
    CubicSink *c = ctx;
    num(c->s, x);
    num(c->s, y);
    str_append(c->s, "l\n");
    c->px = x;
    c->py = y;
}

static void c_quad(void *ctx, float qx, float qy, float x, float y) {
    CubicSink *c = ctx;
    num(c->s, c->px + 2.0 / 3.0 * (qx - c->px));
    num(c->s, c->py + 2.0 / 3.0 * (qy - c->py));
    num(c->s, x + 2.0 / 3.0 * (qx - x));
    num(c->s, y + 2.0 / 3.0 * (qy - y));
    num(c->s, x);
    num(c->s, y);
    str_append(c->s, "c\n");
    c->px = x;
    c->py = y;
}

static void c_close(void *ctx) { str_append(((CubicSink *)ctx)->s, "h\n"); }

static const OutlineSink pdf_sink = {c_move, c_line, c_quad, c_close};

static void stroke_state(Str *s, Resources *r, const Op *op) {
    rgb_op(s, op->stroke_color, "RG");
    set_alpha(s, r, op->stroke_alpha);
    num(s, op->stroke_width);
    str_append(s, "w 1 J 1 j\n");
    if (op->dash_on > 0 || op->dash_off > 0) {
        str_append(s, "[");
        num(s, op->dash_on > 0 ? op->dash_on : 0.01);
        num(s, op->dash_off);
        str_append(s, "] 0 d\n");
    } else {
        str_append(s, "[] 0 d\n");
    }
}

static void circle_path(Str *s, float cx, float cy, float r) {
    const double k = 0.5522847498 * r;
    num(s, cx + r); num(s, cy); str_append(s, "m\n");
    num(s, cx + r); num(s, cy + k); num(s, cx + k); num(s, cy + r); num(s, cx); num(s, cy + r); str_append(s, "c\n");
    num(s, cx - k); num(s, cy + r); num(s, cx - r); num(s, cy + k); num(s, cx - r); num(s, cy); str_append(s, "c\n");
    num(s, cx - r); num(s, cy - k); num(s, cx - k); num(s, cy - r); num(s, cx); num(s, cy - r); str_append(s, "c\n");
    num(s, cx + k); num(s, cy - r); num(s, cx + r); num(s, cy - k); num(s, cx + r); num(s, cy); str_append(s, "c h\n");
}

static void op_path(Str *s, const Op *op) {
    if (op->kind == OP_RECT) {
        num(s, op->x); num(s, op->y); num(s, op->w); num(s, op->h);
        str_append(s, "re\n");
    } else if (op->kind == OP_CIRCLE) {
        circle_path(s, op->x, op->y, op->r);
    } else {
        for (int k = 0; k < op->count; k++) {
            num(s, op->points[2 * k]);
            num(s, op->points[2 * k + 1]);
            str_append(s, k ? "l\n" : "m\n");
        }
        if (op->closed) str_append(s, "h\n");
    }
}

static void text_ops(Str *s, Resources *r, const Op *op, int stroke) {
    PlacedGlyph glyphs[512];
    size_t n = font_layout(op->text, op->size, op->bold, glyphs, 512);
    double a = op->size / (double)font_units_per_em(op->bold);
    float x = op->x;
    if (op->anchor != ANCHOR_START) {
        float w = font_width(op->text, op->size, op->bold);
        x -= op->anchor == ANCHOR_MIDDLE ? w / 2 : w;
    }
    for (size_t i = 0; i < n; i++) {
        r->used[(op->bold * r->glyphs_per_face + glyphs[i].glyph)] |= (unsigned char)(stroke ? 2 : 1);
        str_append(s, "q ");
        num(s, a);
        str_append(s, "0 0 ");
        num(s, -a);
        num(s, x + glyphs[i].x);
        num(s, op->y);
        str_printf(s, "cm /%s%c%d Do Q\n", stroke ? "S" : "F", op->bold ? 'b' : 'r', glyphs[i].glyph);
    }
}

static void content(const Scene *scene, Str *s, Resources *r) {
    double k = 72.0 / scene->dpi;
    /* Pixels, y down -> points, y up. */
    num(s, k);
    str_append(s, "0 0 ");
    num(s, -k);
    str_append(s, "0 ");
    num(s, scene->height_pt);
    str_append(s, "cm\n");
    rgb_op(s, scene->background, "rg");
    set_alpha(s, r, 1);
    str_printf(s, "0 0 %d %d re f\n", scene->width, scene->height);
    int depth = 0;
    for (size_t i = 0; i < scene->ops.count; i++) {
        const Op *op = scene->ops.items[i];
        switch (op->kind) {
        case OP_CLIP:
            if (depth) str_append(s, "Q\n");
            str_append(s, "q ");
            num(s, op->x); num(s, op->y); num(s, op->w); num(s, op->h);
            str_append(s, "re W n\n");
            depth = 1;
            break;
        case OP_UNCLIP:
            if (depth) str_append(s, "Q\n");
            depth = 0;
            break;
        case OP_PATH:
        case OP_RECT:
        case OP_CIRCLE:
            if (op->fill && (op->kind != OP_PATH || op->count >= 3)) {
                rgb_op(s, op->fill_color, "rg");
                set_alpha(s, r, op->fill_alpha);
                op_path(s, op);
                str_append(s, "f\n");
            }
            if (op->stroke && (op->kind != OP_PATH || op->count >= 2)) {
                stroke_state(s, r, op);
                op_path(s, op);
                str_append(s, "S\n");
            }
            break;
        case OP_TEXT: {
            double a = op->size / (double)font_units_per_em(op->bold);
            if (op->vertical) {
                /* Turn 90 degrees counterclockwise on the page about the text origin. */
                str_append(s, "q 0 -1 1 0 ");
                num(s, op->x - op->y);
                num(s, op->x + op->y);
                str_append(s, "cm\n");
            }
            if (op->halo > 0) {
                rgb_op(s, scene->background, "RG");
                rgb_op(s, scene->background, "rg");
                set_alpha(s, r, 0.9);
                num(s, 2 * op->halo / a);
                str_append(s, "w 1 J 1 j [] 0 d\n");
                text_ops(s, r, op, 1);
            }
            rgb_op(s, op->fill_color, "rg");
            set_alpha(s, r, op->fill_alpha);
            text_ops(s, r, op, 0);
            if (op->vertical) str_append(s, "Q\n");
            break;
        }
        }
    }
    if (depth) str_append(s, "Q\n");
}

typedef struct {
    Str file;
    size_t offsets[4096];
    int count;
    Workspace *ws;            /* one compressor workspace for every stream in the file */
} Writer;

static int begin_object(Writer *w) {
    int id = ++w->count;
    w->offsets[id] = w->file.length;
    str_printf(&w->file, "%d 0 obj\n", id);
    return id;
}

static void stream_object(Writer *w, const char *dict_extra, const Str *data) {
    Str z = {0};
    zlib_compress((const unsigned char *)str_cstr(data), data->length, w->ws, &z);
    begin_object(w);
    str_printf(&w->file, "<< %s /Length %zu /Filter /FlateDecode >>\nstream\n", dict_extra, z.length);
    str_appendn(&w->file, z.data, z.length);
    str_append(&w->file, "\nendstream\nendobj\n");
    str_free(&z);
}

static void pdf_string(Str *s, const char *text) {
    str_append(s, "(");
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == '(' || c == ')' || c == '\\') str_printf(s, "\\%c", c);
        else if (c < 32 || c > 126) str_printf(s, "\\%03o", c);
        else str_appendn(s, (const char *)&c, 1);
    }
    str_append(s, ")");
}

int render_pdf(const Scene *scene, const char *path) {
    Resources res = {0};
    Str page = {0};
    Writer w = {0};
    res.glyphs_per_face = font_glyph_count(0) > font_glyph_count(1) ? font_glyph_count(0) : font_glyph_count(1);
    res.used = xcalloc((size_t)res.glyphs_per_face * 2, 1);
    w.ws = workspace_new();
    content(scene, &page, &res);

    str_append(&w.file, "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n");
    /* 1 catalog, 2 pages, 3 page, 4 info, 5 content; then ExtGStates and glyph forms. */
    int catalog = begin_object(&w);
    str_append(&w.file, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    begin_object(&w);
    str_append(&w.file, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    int page_id = begin_object(&w);
    int first_extra = 6;
    int gs_first = first_extra;
    int form_first = gs_first + res.alpha_count;
    Str resources = {0};
    str_append(&resources, "<< /ExtGState <<");
    for (int i = 0; i < res.alpha_count; i++) str_printf(&resources, " /A%d %d 0 R", i, gs_first + i);
    str_append(&resources, " >> /XObject <<");
    int form_id = form_first;
    for (int bold = 0; bold < 2; bold++)
        for (int g = 0; g < res.glyphs_per_face; g++) {
            unsigned char u = res.used[bold * res.glyphs_per_face + g];
            if (u & 1) str_printf(&resources, " /F%c%d %d 0 R", bold ? 'b' : 'r', g, form_id++);
            if (u & 2) str_printf(&resources, " /S%c%d %d 0 R", bold ? 'b' : 'r', g, form_id++);
        }
    str_append(&resources, " >> >>");
    str_append(&w.file, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ");
    num(&w.file, scene->width_pt);
    num(&w.file, scene->height_pt);
    str_printf(&w.file, "] /Resources %s /Contents 5 0 R >>\nendobj\n", str_cstr(&resources));
    begin_object(&w);
    str_append(&w.file, "<< /Title ");
    pdf_string(&w.file, scene->title);
    str_append(&w.file, " /Producer (isocost " ISOCOST_VERSION ") >>\nendobj\n");
    stream_object(&w, "", &page);
    for (int i = 0; i < res.alpha_count; i++) {
        begin_object(&w);
        str_append(&w.file, "<< /Type /ExtGState /ca ");
        num(&w.file, res.alphas[i]);
        str_append(&w.file, "/CA ");
        num(&w.file, res.alphas[i]);
        str_append(&w.file, ">>\nendobj\n");
    }
    for (int bold = 0; bold < 2; bold++)
        for (int g = 0; g < res.glyphs_per_face; g++) {
            unsigned char u = res.used[bold * res.glyphs_per_face + g];
            for (int kind = 1; kind <= 2; kind++) {
                if (!(u & kind)) continue;
                Str glyph = {0};
                CubicSink sink = {&glyph, 0, 0};
                font_outline(bold, g, &pdf_sink, &sink);
                str_append(&glyph, kind == 1 ? "f\n" : "S\n");
                stream_object(&w, "/Type /XObject /Subtype /Form /BBox [-4096 -4096 8192 8192]", &glyph);
                str_free(&glyph);
            }
        }
    size_t xref = w.file.length;
    str_printf(&w.file, "xref\n0 %d\n0000000000 65535 f \n", w.count + 1);
    for (int i = 1; i <= w.count; i++) str_printf(&w.file, "%010zu 00000 n \n", w.offsets[i]);
    str_printf(&w.file, "trailer\n<< /Size %d /Root %d 0 R /Info 4 0 R >>\nstartxref\n%zu\n%%%%EOF\n", w.count + 1, catalog, xref);
    (void)page_id;
    int ok = pf_write_file_atomic(path, w.file.data, w.file.length);
    str_free(&w.file);
    str_free(&page);
    str_free(&resources);
    free(res.used);
    workspace_free(w.ws);
    return ok;
}
