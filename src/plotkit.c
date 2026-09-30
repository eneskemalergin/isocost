#include "plot.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void style_init(Style *s, const Config *config, const FigureSize *size) {
    memset(s, 0, sizeof(*s));
    s->config = config;
    s->size = *size;
    s->u = (float)(size->base_px * config->font_scale);
    s->title = 1.55f * s->u;
    s->subtitle = 0.95f * s->u;
    s->legend = 0.95f * s->u;
    s->axis = 0.95f * s->u;
    s->tick = 0.88f * s->u;
    s->label = 0.95f * s->u;
    s->note = 0.8f * s->u;
    s->clabel = 0.82f * s->u;
    s->hairline = fmaxf(1.0f, 0.072f * s->u);
    s->line = fmaxf(1.5f, 0.14f * s->u);
    s->mark = size->print ? 0.7f * s->u : s->u;
    s->margin = size->print ? 1.0f * s->u : 2.0f * s->u;
    s->show_header = config->header == HEADER_SHOW || (config->header == HEADER_AUTO && !size->print);
    /* On print sizes an automatic note belongs in the caption; custom text is always drawn. */
    s->show_note = strcmp(config->note, "auto") != 0 || !size->print;
}

/* ---- axes ------------------------------------------------------------------- */

double axis_space(const Axis *a, double v) { return a->log ? log(v) : v; }

float axis_map(const Axis *a, double v) {
    double t = (axis_space(a, v) - a->lo) / (a->hi - a->lo);
    /* An extreme ratio maps far off the canvas; keep pixels inside int range. */
    t = fmax(-1e3, fmin(1e3, t));
    return a->p0 + (float)t * (a->p1 - a->p0);
}

void axis_fit(Axis *a, double vmin, double vmax, double forced_min, double forced_max) {
    double lo = axis_space(a, vmin), hi = axis_space(a, vmax), pad;
    if (hi - lo < 1e-9) {
        lo -= a->log ? log(1.2) : 0.1;
        hi += a->log ? log(1.2) : 0.1;
    }
    pad = (hi - lo) * 0.06;
    if (a->log && pad < log(1.04)) pad = log(1.04);
    lo -= pad;
    hi += pad;
    if (!a->log && lo < 0 && vmin >= 0) lo = 0;
    if (forced_min > 0) lo = axis_space(a, forced_min);
    if (forced_max > 0) hi = axis_space(a, forced_max);
    a->lo = lo;
    a->hi = hi;
}

static size_t log_ticks(const Axis *a, float pixels, float min_gap, double *ticks, size_t max) {
    static const double sets[5][10] = {{1, 1.25, 1.5, 2, 2.5, 3, 4, 5, 6, 8}, {1, 1.5, 2, 3, 5, 7}, {1, 2, 5}, {1, 3}, {1}};
    static const int sizes[5] = {10, 6, 3, 2, 1};
    double vlo = exp(a->lo), vhi = exp(a->hi);
    /* exp() of an extreme ratio is 0 or inf; keep the decade range inside int. */
    int klo = (int)fmax(floor(log10(vlo)), -330), khi = (int)fmin(ceil(log10(vhi)), 330);
    for (int s = 0; s < 5; s++) {
        size_t n = 0;
        int ok = 1;
        for (int k = klo - 1; k <= khi + 1 && n < max; k++)
            for (int m = 0; m < sizes[s] && n < max; m++) {
                double v = sets[s][m] * pow(10, k);
                if (v >= vlo && v <= vhi) ticks[n++] = v;
            }
        for (size_t i = 1; i < n; i++)
            if ((log(ticks[i]) - log(ticks[i - 1])) / (a->hi - a->lo) * pixels < min_gap) ok = 0;
        if (ok && n >= 2) return n;
        if (s == 4) {
            size_t kept = 0, stride = 1;
            while (stride < 8 && n / stride > (size_t)(pixels / min_gap)) stride++;
            for (size_t i = 0; i < n; i += stride) ticks[kept++] = ticks[i];
            return kept;
        }
    }
    return 0;
}

static size_t linear_ticks(const Axis *a, float pixels, float min_gap, double *ticks, size_t max) {
    double target = (a->hi - a->lo) * min_gap / pixels, magnitude = pow(10, floor(log10(target))), step = 10 * magnitude;
    static const double m[4] = {1, 2, 2.5, 5};
    for (int i = 0; i < 4; i++)
        if (m[i] * magnitude >= target) {
            step = m[i] * magnitude;
            break;
        }
    size_t n = 0;
    for (double v = ceil(a->lo / step) * step; v <= a->hi + step * 1e-9 && n < max; v += step) ticks[n++] = fabs(v) < step * 1e-9 ? 0 : v;
    return n;
}

size_t axis_ticks(const Axis *a, float min_gap, double *out, size_t max) {
    float pixels = fabsf(a->p1 - a->p0);
    return a->log ? log_ticks(a, pixels, min_gap, out, max) : linear_ticks(a, pixels, min_gap, out, max);
}

void axis_tick_label(const Axis *a, const double *ticks, size_t n, size_t i, char *out, size_t size) {
    char number[48];
    if (!a->log && n >= 2) {
        double step = fabs(ticks[1] - ticks[0]);
        int decimals = 0;
        while (decimals < 6 && fabs(step * pow(10, decimals) - round(step * pow(10, decimals))) > 1e-6) decimals++;
        snprintf(number, sizeof(number), "%.*f", decimals, ticks[i]);
    } else {
        format_number(number, sizeof(number), ticks[i]);
    }
    snprintf(out, size, "%s\xc3\x97", number);
}

/* ---- markers ---------------------------------------------------------------- */

static int shape_polygon(int shape, float cx, float cy, float r, float *pts) {
    switch (shape) {
    case SHAPE_SQUARE: {
        float h = 0.886f * r;
        float q[8] = {-h, -h, h, -h, h, h, -h, h};
        for (int i = 0; i < 4; i++) {
            pts[2 * i] = cx + q[2 * i];
            pts[2 * i + 1] = cy + q[2 * i + 1];
        }
        return 4;
    }
    case SHAPE_TRIANGLE:
    case SHAPE_TRIANGLE_DOWN: {
        float R = 1.347f * r, dir = shape == SHAPE_TRIANGLE ? -1.0f : 1.0f;
        for (int i = 0; i < 3; i++) {
            float a = (float)M_PI / 2 * dir + (float)i * 2 * (float)M_PI / 3;
            pts[2 * i] = cx + R * cosf(a);
            pts[2 * i + 1] = cy + R * sinf(a) - dir * 0.18f * R;
        }
        return 3;
    }
    case SHAPE_DIAMOND: {
        float d = 1.2533f * r;
        float q[8] = {0, -d, d, 0, 0, d, -d, 0};
        for (int i = 0; i < 4; i++) {
            pts[2 * i] = cx + q[2 * i];
            pts[2 * i + 1] = cy + q[2 * i + 1];
        }
        return 4;
    }
    case SHAPE_PLUS:
    case SHAPE_CROSS: {
        float R = 1.18f * r, w = 0.40f * R;
        float q[24] = {-w, -R, w, -R, w, -w, R, -w, R, w, w, w, w, R, -w, R, -w, w, -R, w, -R, -w, -w, -w};
        float c = shape == SHAPE_CROSS ? 0.70710678f : 1, sn = shape == SHAPE_CROSS ? 0.70710678f : 0;
        for (int i = 0; i < 12; i++) {
            pts[2 * i] = cx + q[2 * i] * c - q[2 * i + 1] * sn;
            pts[2 * i + 1] = cy + q[2 * i] * sn + q[2 * i + 1] * c;
        }
        return 12;
    }
    case SHAPE_STAR: {
        float R = 1.42f * r, inner = 0.5f * R;
        for (int i = 0; i < 10; i++) {
            float a = -(float)M_PI / 2 + (float)i * (float)M_PI / 5, rr = i % 2 ? inner : R;
            pts[2 * i] = cx + rr * cosf(a);
            pts[2 * i + 1] = cy + rr * sinf(a);
        }
        return 10;
    }
    default:
        return 0;
    }
}

Op *draw_marker(Scene *sc, int shape, int hollow, Rgb color, float cx, float cy, float r, float alpha, float ring, Rgb ring_color) {
    float pts[48];
    int n = shape_polygon(shape, cx, cy, r, pts);
    Op *op;
    if (ring > 0) {
        if (!n) op_fill(scene_circle(sc, cx, cy, r + ring), ring_color, 1);
        else op_stroke(op_fill(scene_path(sc, pts, n, 1), ring_color, 1), ring_color, 2 * ring, 1);
    }
    op = n ? scene_path(sc, pts, n, 1) : scene_circle(sc, cx, cy, r);
    if (hollow) {
        if (ring > 0) op_fill(op, ring_color, 1);
        op_stroke(op, color, fmaxf(1.2f, r * 0.34f), alpha);
    } else {
        op_fill(op, color, alpha);
    }
    return op;
}

/* ---- legend ------------------------------------------------------------------- */

static float entry_width(const Style *s, const LegendEntry *e) {
    /* marker (2.5 r, r = 0.36 legend) + gap + label, exactly as legend_draw places them */
    float w = 2.5f * 0.36f * s->legend + 0.45f * s->u + font_width(e->label, s->legend, 0);
    if (e->detail && *e->detail) w += 0.35f * s->u + font_width(e->detail, s->legend * 0.88f, 0);
    return w;
}

float legend_measure(const Style *s, const LegendEntry *entries, size_t count, int position, float extent) {
    float row = 1.55f * s->legend, gap = 1.4f * s->u, x = 0;
    int rows = count ? 1 : 0;
    if (position == LEGEND_NONE || !count) return 0;
    if (position == LEGEND_RIGHT) {
        float w = 0;
        for (size_t i = 0; i < count; i++) w = fmaxf(w, entry_width(s, &entries[i]));
        return w + gap;
    }
    for (size_t i = 0; i < count; i++) {
        float w = entry_width(s, &entries[i]);
        if (x > 0 && x + w > extent) {
            rows++;
            x = 0;
        }
        x += w + gap;
    }
    return (float)rows * row;
}

void legend_draw(Scene *sc, const Style *s, const LegendEntry *entries, size_t count, int position, float x0, float y0, float extent) {
    const Config *c = s->config;
    float row = 1.55f * s->legend, gap = 1.4f * s->u, r = 0.36f * s->legend;
    float x = x0, y = y0 + row / 2;
    if (position == LEGEND_NONE) return;
    for (size_t i = 0; i < count; i++) {
        const LegendEntry *e = &entries[i];
        float w = entry_width(s, e);
        if (position == LEGEND_RIGHT) {
            if (i) y += row;
        } else if (x > x0 && x + w > x0 + extent) {
            x = x0;
            y += row;
        }
        draw_marker(sc, e->shape, e->hollow, e->color, x + r * 1.25f, y, r, 1, 0.1f * s->u, c->background);
        float tx = x + 2.5f * r + 0.45f * s->u;
        scene_text(sc, tx, y + 0.36f * s->legend, e->label, s->legend, 0, c->ink, ANCHOR_START);
        if (e->detail && *e->detail) {
            float dx = tx + font_width(e->label, s->legend, 0) + 0.35f * s->u;
            scene_text(sc, dx, y + 0.36f * s->legend, e->detail, s->legend * 0.88f, 0, c->muted, ANCHOR_START);
        }
        if (position != LEGEND_RIGHT) x += w + gap;
    }
}

/* ---- text ---------------------------------------------------------------------- */

int wrap_text(const char *text, float size, float width, char lines[][512], int max_lines) {
    int n = 0;
    Str line = {0};
    const char *p = text;
    while (*p && n < max_lines) {
        if (*p == '\n') {
            snprintf(lines[n++], 512, "%s", str_cstr(&line));
            str_free(&line);
            p++;
            continue;
        }
        const char *end = p;
        while (*end && *end != ' ' && *end != '\n') end++;
        size_t len = (size_t)(end - p);
        Str candidate = {0};
        if (line.length) {
            str_append(&candidate, line.data);
            str_append(&candidate, " ");
        }
        str_appendn(&candidate, p, len);
        if (line.length && font_width(candidate.data, size, 0) > width) {
            snprintf(lines[n++], 512, "%s", line.data);
            str_free(&line);
            str_appendn(&line, p, len);
            str_free(&candidate);
        } else {
            str_free(&line);
            line = candidate;
        }
        p = end;
        while (*p == ' ') p++;
    }
    if (line.length && n < max_lines) snprintf(lines[n++], 512, "%s", line.data);
    str_free(&line);
    return n;
}

static int wrap_bold(const char *text, float size, float width, char lines[][512], int max_lines) {
    /* wrap_text measures regular weight; bold is about 12% wider, so give it less room. */
    return wrap_text(text, size, width / 1.12f, lines, max_lines);
}

Op *text_centered_inside(Scene *sc, float x, float y, const char *text, float size, Rgb color) {
    float half = font_width(text, size, 0) / 2, edge = 0.25f * size;
    if (x - half < edge) x = edge + half;
    if (x + half > (float)sc->width - edge) x = (float)sc->width - edge - half;
    return scene_text(sc, x, y, text, size, 0, color, ANCHOR_MIDDLE);
}

float draw_header(Scene *sc, const Style *s, const char *title, const char *subtitle, float y) {
    const Config *c = s->config;
    char lines[3][512];
    float width = (float)s->size.width - 2 * s->margin;
    if (!s->show_header) return y;
    int n = wrap_bold(title, s->title, width, lines, 3);
    for (int i = 0; i < n; i++) {
        y += (i ? 1.25f * s->title : font_cap_height(s->title, 1));
        scene_text(sc, s->margin, y, lines[i], s->title, 1, c->ink, ANCHOR_START);
    }
    if (subtitle && *subtitle) {
        n = wrap_text(subtitle, s->subtitle, width, lines, 3);
        for (int i = 0; i < n; i++) {
            y += (i ? 1.3f * s->subtitle : 0.75f * s->u + font_cap_height(s->subtitle, 0));
            scene_text(sc, s->margin, y, lines[i], s->subtitle, 0, c->ink_secondary, ANCHOR_START);
        }
    }
    return y + font_descender(s->subtitle, 0);
}

float measure_note(const Style *s, const char *note, float width, char lines[][512], int *count) {
    *count = s->show_note && note && *note ? wrap_text(note, s->note, width, lines, 6) : 0;
    return (float)*count * s->note * 1.4f;
}

/* ---- label placement --------------------------------------------------------------- */

void placer_add(Placer *p, float x0, float y0, float x1, float y1, float weight) {
    if (p->count == p->capacity) {
        p->capacity = p->capacity ? p->capacity * 2 : 128;
        p->boxes = xrealloc(p->boxes, p->capacity * sizeof(Box));
    }
    Box b = {x0, y0, x1, y1, weight};
    p->boxes[p->count++] = b;
}

float placer_score(const Placer *p, float x0, float y0, float x1, float y1) {
    float score = 0;
    if (x0 < p->fx0 || x1 > p->fx1 || y0 < p->fy0 || y1 > p->fy1) return 1e12f;
    for (size_t i = 0; i < p->count; i++) {
        const Box *b = &p->boxes[i];
        float w = fminf(b->x1, x1) - fmaxf(b->x0, x0), h = fminf(b->y1, y1) - fmaxf(b->y0, y0);
        if (w > 0 && h > 0) score += w * h * b->weight;
    }
    return score;
}

int placer_place(Placer *p, float px, float py, float r, float w, float h, int position, float u, float *bx, float *by, float *distance) {
    static const float all[8][2] = {{1, -1}, {1, 0}, {1, 1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}, {0, 1}};
    const float (*dirs)[2] = all;
    int ndirs = 8;
    static const float right[3][2] = {{1, 0}, {1, -1}, {1, 1}}, left[3][2] = {{-1, 0}, {-1, -1}, {-1, 1}};
    static const float above[3][2] = {{0, -1}, {1, -1}, {-1, -1}}, below[3][2] = {{0, 1}, {1, 1}, {-1, 1}};
    if (position == LABEL_NONE) return 0;
    if (position == LABEL_RIGHT) { dirs = right; ndirs = 3; }
    if (position == LABEL_LEFT) { dirs = left; ndirs = 3; }
    if (position == LABEL_ABOVE) { dirs = above; ndirs = 3; }
    if (position == LABEL_BELOW) { dirs = below; ndirs = 3; }
    float best = 1e30f, bx0 = 0, by0 = 0, bd = 0;
    float dists[4] = {r + 0.35f * u, r + 1.0f * u, r + 2.0f * u, r + 3.4f * u};
    for (int d = 0; d < 4; d++)
        for (int k = 0; k < ndirs; k++) {
            float dx = dirs[k][0], dy = dirs[k][1];
            float norm = (dx != 0 && dy != 0) ? 0.7071f : 1.0f;
            float ax = px + dx * dists[d] * norm, ay = py + dy * dists[d] * norm;
            float x0 = dx > 0 ? ax : dx < 0 ? ax - w : ax - w / 2;
            float y0 = dy > 0 ? ay : dy < 0 ? ay - h : ay - h / 2;
            float sc = placer_score(p, x0, y0, x0 + w, y0 + h) + (float)d * 2.8f * u + (float)k * 0.15f * u;
            if (sc < best) {
                best = sc;
                bx0 = x0;
                by0 = y0;
                bd = dists[d];
            }
        }
    /* Overlap with a label or marker (weight 1000) of more than a few square pixels drops the label. */
    if (best >= 30.0f * u * u) return 0;
    *bx = bx0;
    *by = by0;
    *distance = bd;
    placer_add(p, bx0 - 2, by0 - 2, bx0 + w + 2, by0 + h + 2, 1000);
    return 1;
}

void placer_free(Placer *p) {
    free(p->boxes);
    memset(p, 0, sizeof(*p));
}

/* ---- shared figure text ----------------------------------------------------------- */

static int cmp_tool_order(const void *a, const void *b, const void *ctx) {
    const Figure *f = ctx;
    const Variant *x = &f->variants[*(const size_t *)a], *y = &f->variants[*(const size_t *)b];
    if (x->order != y->order) {
        if (!x->order) return 1;
        if (!y->order) return -1;
        return x->order < y->order ? -1 : 1;
    }
    return strcmp(x->id, y->id);
}

size_t visible_tools(const Figure *f, size_t *order) {
    size_t n = 0;
    for (size_t v = 0; v < f->variant_count; v++)
        if (!f->variants[v].hidden) order[n++] = v;
    sort_ctx(order, n, sizeof(size_t), cmp_tool_order, f);
    return n;
}

char *auto_subtitle(const Figure *f) {
    Str s = {0};
    const Variant *ref = &f->variants[f->baseline];
    str_printf(&s, "Relative to %s%s on each workload, %zu workload%s", ref->label, f->baseline_auto ? " (chosen automatically)" : "",
               f->referenced_cases, f->referenced_cases == 1 ? "" : "s");
    if (*f->run) {
        const char *r = f->run;
        if (strlen(r) == 15 && r[8] == '_')
            str_printf(&s, ", run %.4s-%.2s-%.2s %.2s:%.2s", r, r + 4, r + 6, r + 9, r + 11);
        else
            str_printf(&s, ", run %s", r);
    }
    char *out = xstrdup(str_cstr(&s));
    str_free(&s);
    return out;
}
