/*
 * Overview: every tool's run time and peak memory relative to the reference.
 *
 * Layer order (back to front): faster-and-smaller tint, grid, reference lines,
 * equal-cost curves, faint workload points, Pareto line, aggregate markers,
 * spread bars and ellipses, selection ring, labels.
 */
#include "plot.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void dash_for(Op *op, int line, float width) {
    if (line == LINE_DASHED) op_dash(op, 3.2f * width, 2.2f * width);
    else if (line == LINE_DOTTED) op_dash(op, 0.01f, 2.2f * width);
}

static void ellipse_point(const Variant *v, double t, double *x, double *y) {
    double ex = v->e_cx + v->e_a * cos(t) * cos(v->e_theta) - v->e_b * sin(t) * sin(v->e_theta);
    double ey = v->e_cy + v->e_a * cos(t) * sin(v->e_theta) + v->e_b * sin(t) * cos(v->e_theta);
    *x = exp(ex);
    *y = exp(ey);
}

static size_t auto_cost_levels(const Config *c, const Axis *ax, const Axis *ay, double *levels, size_t max) {
    double cmin = INFINITY, cmax = 0;
    double xs[2] = {ax->log ? exp(ax->lo) : ax->lo, ax->log ? exp(ax->hi) : ax->hi};
    double ys[2] = {ay->log ? exp(ay->lo) : ay->lo, ay->log ? exp(ay->hi) : ay->hi};
    for (int i = 0; i < 4; i++) {
        double x = xs[i & 1] > 0 ? xs[i & 1] : 1e-9, y = ys[i >> 1] > 0 ? ys[i >> 1] : 1e-9;
        double k = cost_of(c, x, y);
        if (k < cmin) cmin = k;
        if (k > cmax) cmax = k;
    }
    static const double bases[6][3] = {{2, 1, 1.5}, {2, 1, 0}, {4, 1, 0}, {10, 1, 0}, {100, 1, 0}, {1e4, 1, 0}};
    for (int b = 0; b < 6; b++) {
        size_t n = 0;
        for (int k = -40; k <= 40 && n < max; k++)
            for (int m = 1; m <= 2 && n < max; m++) {
                if (bases[b][m] == 0) continue;
                double v = bases[b][m] * pow(bases[b][0], k);
                if (v > cmin * 1.02 && v < cmax / 1.02) levels[n++] = v;
            }
        sort_ctx(levels, n, sizeof(double), cmp_double, NULL);
        if (n > 0 && n <= 7) return n;
    }
    return 0;
}

static int contour_points(const Config *c, const Axis *ax, const Axis *ay, double level, float *pts, int max) {
    double total = c->time_weight + c->memory_weight;
    double wt = c->time_weight / total, wm = c->memory_weight / total;
    int n = 0;
    for (int i = 0; i < max; i++) {
        double s = ax->lo + (ax->hi - ax->lo) * i / (max - 1), x = ax->log ? exp(s) : s, y;
        if (x <= 0) continue;
        if (wm <= 0) break;
        y = c->cost_model == COST_LINEAR ? (level - wt * x) / wm : pow(level / pow(x, wt), 1 / wm);
        if (!isfinite(y) || (ay->log && !(y > 0))) continue;
        float sy = axis_map(ay, y);
        if (sy < ay->p1 - 4000 || sy > ay->p0 + 4000) continue;
        pts[2 * n] = axis_map(ax, x);
        pts[2 * n + 1] = sy;
        n++;
    }
    if (wm <= 0) { /* memory weight 0: vertical line x^wt = level */
        double x = pow(level, 1 / wt);
        pts[0] = pts[2] = axis_map(ax, x);
        pts[1] = ay->p1 - 10;
        pts[3] = ay->p0 + 10;
        n = 2;
    }
    return n;
}

static char *default_note(const Figure *f, const Config *c) {
    Str s = {0};
    double total = c->time_weight + c->memory_weight;
    double wt = c->time_weight / total, wm = c->memory_weight / total;
    char a[32], b[32];
    format_number(a, sizeof(a), wt);
    format_number(b, sizeof(b), wm);
    if (f->max_n > 1) {
        str_append(&s, "Large marks: geometric mean over workloads.");
        if (c->case_points) str_append(&s, " Faint marks: single workloads.");
        int outline = 0;
        for (size_t v = 0; v < f->variant_count; v++) outline |= c->ellipse && f->variants[v].has_ellipse && !f->variants[v].hidden;
        if (c->spread != SPREAD_NONE)
            str_printf(&s, " Bars%s: %s of workloads (descriptive, not a confidence interval).", outline ? " and outlines" : "",
                       c->spread == SPREAD_IQR ? "middle 50%" : "full range");
    } else {
        str_append(&s, "Each mark is one Zebrac result.");
    }
    if (c->contours) {
        if (c->cost_model == COST_LINEAR) str_printf(&s, " Dashed lines: equal cost C = %s time + %s memory.", a, b);
        else str_printf(&s, " Dashed curves: equal cost C = time^%s x memory^%s.", a, b);
    }
    if (c->pareto) str_append(&s, " Dark line: tools no other tool beats on both axes.");
    str_append(&s, f->max_n > 1 ? " Ring: lowest cost among tools measured on every workload." : " Ring: lowest cost.");
    char *out = xstrdup(str_cstr(&s));
    str_free(&s);
    return out;
}

Scene *view_overview(const Figure *f, const Config *c, const FigureSize *size, ViewInfo *info) {
    Style st;
    style_init(&st, c, size);
    const float u = st.u, M = st.margin;
    const int W = size->width, H = size->height;
    Scene *sc = scene_new(W, H, size->dpi, size->width_pt, size->height_pt, c->background, f->title);
    const Variant *ref = &f->variants[f->baseline];
    size_t *order = xcalloc(f->variant_count + 1, sizeof(size_t));
    size_t nvis = visible_tools(f, order);
    char buffer[1024];
    Placer placer = {0};
    info->width = W;
    info->height = H;

    /* Header. */
    char *subtitle = f->subtitle ? xstrdup(f->subtitle) : auto_subtitle(f);
    float y = draw_header(sc, &st, f->title, subtitle, M);
    free(subtitle);

    /* Legend entries. */
    LegendEntry *entries = xcalloc(nvis + 1, sizeof(LegendEntry));
    char (*details)[32] = xcalloc(nvis + 1, sizeof(*details));
    int counts_differ = 0;
    for (size_t k = 1; k < nvis; k++)
        if (f->variants[order[k]].n != f->variants[order[0]].n) counts_differ = 1;
    for (size_t k = 0; k < nvis; k++) {
        const Variant *v = &f->variants[order[k]];
        if (!v->n) snprintf(details[k], sizeof(details[k]), "no data");
        else if (counts_differ) snprintf(details[k], sizeof(details[k]), "n=%zu", v->n);
        entries[k] = (LegendEntry){v->label, details[k], v->shape, v->hollow, v->color};
    }
    float legend_extent = (float)W - 2 * M;
    float legend_h = legend_measure(&st, entries, nvis, c->legend, legend_extent);

    /* Note. */
    char note_lines[6][512];
    int note_count = 0;
    char *note = strcmp(c->note, "auto") == 0 ? default_note(f, c) : xstrdup(c->note);
    float note_h = measure_note(&st, note, (float)W - 2 * M, note_lines, &note_count);
    free(note);

    /* Domain from what is drawn. */
    double xmin = 1, xmax = 1, ymin = 1, ymax = 1;
    for (size_t i = 0; i < f->point_count; i++) {
        const CasePoint *p = &f->points[i];
        if (f->variants[p->variant].hidden || (!c->case_points && f->variants[p->variant].n > 1)) continue;
        xmin = fmin(xmin, p->x); xmax = fmax(xmax, p->x);
        ymin = fmin(ymin, p->y); ymax = fmax(ymax, p->y);
    }
    for (size_t k = 0; k < nvis; k++) {
        const Variant *v = &f->variants[order[k]];
        if (!v->n) continue;
        xmin = fmin(xmin, v->gx); xmax = fmax(xmax, v->gx);
        ymin = fmin(ymin, v->gy); ymax = fmax(ymax, v->gy);
        if (c->spread != SPREAD_NONE && v->n > 1) {
            xmin = fmin(xmin, v->xlo); xmax = fmax(xmax, v->xhi);
            ymin = fmin(ymin, v->ylo); ymax = fmax(ymax, v->yhi);
        }
        if (c->ellipse && v->has_ellipse)
            for (int t = 0; t < 36; t++) {
                double ex, ey;
                ellipse_point(v, t * 2 * M_PI / 36, &ex, &ey);
                xmin = fmin(xmin, ex); xmax = fmax(xmax, ex);
                ymin = fmin(ymin, ey); ymax = fmax(ymax, ey);
            }
    }
    Axis ax = {0}, ay = {0};
    ax.log = c->x_scale == SCALE_LOG || (c->x_scale == SCALE_AUTO && xmax / xmin > 6);
    ay.log = c->y_scale == SCALE_LOG || (c->y_scale == SCALE_AUTO && ymax / ymin > 6);
    axis_fit(&ax, xmin, xmax, c->x_min, c->x_max);
    axis_fit(&ay, ymin, ymax, c->y_min, c->y_max);
    info->x_log = ax.log;
    info->y_log = ay.log;

    /* Plot rectangle. */
    float top = st.show_header ? y + 0.9f * u : y;
    if (c->legend == LEGEND_TOP && legend_h > 0) {
        legend_draw(sc, &st, entries, nvis, LEGEND_TOP, M, top, legend_extent);
        top += legend_h + 0.3f * u;
    }
    float plot_top = top + 0.9f * u + font_cap_height(st.axis, 0);
    float bottom = (float)H - M - note_h - (note_count ? 0.6f * u : 0);
    if (c->legend == LEGEND_BOTTOM && legend_h > 0) bottom -= legend_h + 0.5f * u;
    float plot_bottom = bottom - 1.25f * st.tick - 1.0f * u - font_cap_height(st.axis, 0) - 0.4f * u;
    float right = (float)W - M;
    if (c->legend == LEGEND_RIGHT && legend_h > 0) right -= legend_h;

    ay.p0 = plot_bottom;
    ay.p1 = plot_top;
    double yticks[64], xticks[64];
    size_t ny;
    if (c->y_ticks.count) {
        ny = 0;
        for (size_t i = 0; i < c->y_ticks.count && ny < 64; i++) {
            double v = axis_space(&ay, c->y_ticks.values[i]);
            if (v >= ay.lo && v <= ay.hi) yticks[ny++] = c->y_ticks.values[i];
        }
    } else {
        ny = axis_ticks(&ay, (ay.log ? 3.3f : 5.6f) * u, yticks, 64);
    }
    float ytick_w = 0;
    for (size_t i = 0; i < ny; i++) {
        axis_tick_label(&ay, yticks, ny, i, buffer, sizeof(buffer));
        ytick_w = fmaxf(ytick_w, font_width(buffer, st.tick, 0));
    }
    ax.p0 = M + ytick_w + 0.7f * u;
    ax.p1 = right;
    size_t nx;
    if (c->x_ticks.count) {
        nx = 0;
        for (size_t i = 0; i < c->x_ticks.count && nx < 64; i++) {
            double v = axis_space(&ax, c->x_ticks.values[i]);
            if (v >= ax.lo && v <= ax.hi) xticks[nx++] = c->x_ticks.values[i];
        }
    } else {
        nx = axis_ticks(&ax, (ax.log ? 5.4f : 7.8f) * u, xticks, 64);
    }
    const float px0 = ax.p0, px1 = ax.p1, py0 = plot_top, py1 = plot_bottom;
    placer.fx0 = px0 + 0.2f * u;
    placer.fy0 = py0 + 0.2f * u;
    placer.fx1 = px1 - 0.2f * u;
    placer.fy1 = py1 - 0.2f * u;

    /* Axis titles: y above the axis, x centered below the ticks. */
    char *xl = expand_template(c->x_label, f), *yl = expand_template(c->y_label, f);
    snprintf(buffer, sizeof(buffer), "%s (%s%s)", yl, "lower is better", ay.log ? ", log scale" : "");
    scene_text(sc, px0, py0 - 0.7f * u, buffer, st.axis, 0, c->ink_secondary, ANCHOR_START);
    snprintf(buffer, sizeof(buffer), "%s (%s%s)", xl, "lower is faster", ax.log ? ", log scale" : "");
    scene_text(sc, (px0 + px1) / 2, py1 + 1.25f * st.tick + 1.0f * u + font_cap_height(st.axis, 0), buffer, st.axis, 0, c->ink_secondary, ANCHOR_MIDDLE);
    free(xl);
    free(yl);

    scene_clip(sc, px0, py0, px1 - px0, py1 - py0);

    /* Faster-and-smaller region. */
    float rx = axis_map(&ax, 1), ry = axis_map(&ay, 1);
    if (c->highlight && rx > px0 && ry < py1) {
        op_fill(scene_rect(sc, px0, ry, rx - px0, py1 - ry), c->region, 1);
    }

    /* Grid (hairline, solid) and reference lines. */
    for (size_t i = 0; i < nx; i++) op_stroke(scene_line(sc, axis_map(&ax, xticks[i]), py0, axis_map(&ax, xticks[i]), py1), c->grid, st.hairline, 1);
    for (size_t i = 0; i < ny; i++) op_stroke(scene_line(sc, px0, axis_map(&ay, yticks[i]), px1, axis_map(&ay, yticks[i])), c->grid, st.hairline, 1);
    if (c->reference_lines) {
        op_stroke(scene_line(sc, rx, py0, rx, py1), c->reference, 1.3f * st.hairline, 1);
        op_stroke(scene_line(sc, px0, ry, px1, ry), c->reference, 1.3f * st.hairline, 1);
    }

    /* Equal-cost curves. */
    double levels[16];
    size_t level_count = 0;
    enum { SAMPLES = 240 };
    float *curves = NULL;
    int curve_n[16] = {0};
    if (c->contours) {
        if (c->contour_levels.count) {
            for (size_t i = 0; i < c->contour_levels.count && level_count < 16; i++) levels[level_count++] = c->contour_levels.values[i];
        } else {
            level_count = auto_cost_levels(c, &ax, &ay, levels, 16);
        }
        curves = xcalloc(level_count * SAMPLES * 2 + 2, sizeof(float));
        for (size_t i = 0; i < level_count; i++) {
            curve_n[i] = contour_points(c, &ax, &ay, levels[i], curves + i * SAMPLES * 2, SAMPLES);
            if (curve_n[i] >= 2) op_dash(op_stroke(scene_path(sc, curves + i * SAMPLES * 2, curve_n[i], 0), c->contour, 1.1f * st.hairline, 1), 0.42f * u, 0.3f * u);
        }
    }
    info->cost_level_count = level_count;
    memcpy(info->cost_levels, levels, sizeof(double) * level_count);

    /* Faint workload points. */
    float agg_r = 0.5f * st.mark, case_r = 0.29f * st.mark;
    if (c->case_points)
        for (size_t i = 0; i < f->point_count; i++) {
            const CasePoint *p = &f->points[i];
            const Variant *v = &f->variants[p->variant];
            if (v->hidden || v->n < 2) continue;
            float cx = axis_map(&ax, p->x), cy = axis_map(&ay, p->y);
            Op *m = draw_marker(sc, v->shape, v->hollow, v->color, cx, cy, case_r, 0.55f, 0, c->background);
            snprintf(buffer, sizeof(buffer), "%s, %s: run time %.3gx, memory %.3gx", v->label, f->cases[p->kase].label, p->x, p->y);
            op_tooltip(m, buffer);
            placer_add(&placer, cx - case_r, cy - case_r, cx + case_r, cy + case_r, 20);
        }

    /* Pareto line through non-dominated aggregates, in time order. */
    if (c->pareto) {
        size_t *pareto = xcalloc(nvis + 1, sizeof(size_t)), count = 0;
        for (size_t k = 0; k < nvis; k++)
            if (f->variants[order[k]].n && f->variants[order[k]].pareto) pareto[count++] = order[k];
        for (size_t a = 0; a < count; a++)
            for (size_t b = a + 1; b < count; b++)
                if (f->variants[pareto[b]].gx < f->variants[pareto[a]].gx) {
                    size_t t = pareto[a];
                    pareto[a] = pareto[b];
                    pareto[b] = t;
                }
        if (count >= 2) {
            float *pts = xcalloc(count * 2, sizeof(float));
            for (size_t k = 0; k < count; k++) {
                pts[2 * k] = axis_map(&ax, f->variants[pareto[k]].gx);
                pts[2 * k + 1] = axis_map(&ay, f->variants[pareto[k]].gy);
            }
            op_stroke(scene_path(sc, pts, (int)count, 0), c->ink, st.line, 0.85f);
            for (size_t k = 0; k + 1 < count; k++)
                for (int t = 0; t <= 12; t++) {
                    float gx = pts[2 * k] + (pts[2 * k + 2] - pts[2 * k]) * (float)t / 12;
                    float gy = pts[2 * k + 1] + (pts[2 * k + 3] - pts[2 * k + 1]) * (float)t / 12;
                    placer_add(&placer, gx - 0.2f * u, gy - 0.2f * u, gx + 0.2f * u, gy + 0.2f * u, 30);
                }
            free(pts);
        }
        free(pareto);
    }

    /* Aggregate markers. */
    for (size_t k = 0; k < nvis; k++) {
        const Variant *v = &f->variants[order[k]];
        if (!v->n) continue;
        float cx = axis_map(&ax, v->gx), cy = axis_map(&ay, v->gy);
        Op *m = draw_marker(sc, v->shape, v->hollow, v->color, cx, cy, agg_r, 1, 0.14f * st.mark, c->background);
        snprintf(buffer, sizeof(buffer), "%s (%zu workload%s): run time %.3gx (speedup %.3gx), memory %.3gx, cost %.3g%s", v->label, v->n,
                 v->n == 1 ? "" : "s", v->gx, 1 / v->gx, v->gy, v->cost, v->pareto ? ", not beaten on both axes" : "");
        op_tooltip(m, buffer);
        placer_add(&placer, cx - agg_r - 0.2f * u, cy - agg_r - 0.2f * u, cx + agg_r + 0.2f * u, cy + agg_r + 0.2f * u, 1000);
    }

    /* Spread above the markers. */
    for (size_t k = 0; k < nvis && c->spread != SPREAD_NONE; k++) {
        const Variant *v = &f->variants[order[k]];
        if (v->n < 2) continue;
        float cx = axis_map(&ax, v->gx), cy = axis_map(&ay, v->gy), cap = 0.28f * u;
        float x1 = axis_map(&ax, v->xlo), x3 = axis_map(&ax, v->xhi), y1 = axis_map(&ay, v->ylo), y3 = axis_map(&ay, v->yhi);
        if (x3 - x1 > 1) {
            dash_for(op_stroke(scene_line(sc, x1, cy, x3, cy), v->color, st.line, 1), v->line, st.line);
            op_stroke(scene_line(sc, x1, cy - cap, x1, cy + cap), v->color, st.line, 1);
            op_stroke(scene_line(sc, x3, cy - cap, x3, cy + cap), v->color, st.line, 1);
            placer_add(&placer, x1 - 2, cy - cap, x3 + 2, cy + cap, 8);
        }
        if (y1 - y3 > 1) {
            dash_for(op_stroke(scene_line(sc, cx, y1, cx, y3), v->color, st.line, 1), v->line, st.line);
            op_stroke(scene_line(sc, cx - cap, y1, cx + cap, y1), v->color, st.line, 1);
            op_stroke(scene_line(sc, cx - cap, y3, cx + cap, y3), v->color, st.line, 1);
            placer_add(&placer, cx - cap, y3 - 2, cx + cap, y1 + 2, 8);
        }
        if (c->ellipse && v->has_ellipse) {
            float pts[144];
            for (int t = 0; t < 72; t++) {
                double ex, ey;
                ellipse_point(v, t * 2 * M_PI / 72, &ex, &ey);
                pts[2 * t] = axis_map(&ax, ex);
                pts[2 * t + 1] = axis_map(&ay, ey);
                if (t % 6 == 0) placer_add(&placer, pts[2 * t] - 2, pts[2 * t + 1] - 2, pts[2 * t] + 2, pts[2 * t + 1] + 2, 6);
            }
            dash_for(op_stroke(scene_path(sc, pts, 72, 1), v->color, 0.8f * st.line, 0.9f), v->line, st.line);
        }
    }

    /* Selection ring. */
    for (size_t k = 0; k < nvis; k++) {
        const Variant *v = &f->variants[order[k]];
        if (!v->selected) continue;
        float cx = axis_map(&ax, v->gx), cy = axis_map(&ay, v->gy);
        op_stroke(scene_circle(sc, cx, cy, agg_r + 0.5f * st.mark), c->ink, st.line, 1);
        placer_add(&placer, cx - agg_r - 0.65f * u, cy - agg_r - 0.65f * u, cx + agg_r + 0.65f * u, cy + agg_r + 0.65f * u, 1000);
    }

    /* The region hint goes in the corner only when no mark or label is there. */
    if (c->highlight && rx > px0 && ry < py1) {
        const char *hint = "faster and smaller";
        float hw = font_width(hint, st.note, 0), hx = px0 + 0.55f * u, hy = py1 - 0.55f * u;
        if (rx - px0 > hw + 1.2f * u && py1 - ry > 2 * st.note && placer_score(&placer, hx - 2, hy - st.note, hx + hw + 2, hy + 2) < 1) {
            scene_text(sc, hx, hy, hint, st.note, 0, c->muted, ANCHOR_START);
            placer_add(&placer, hx - 2, hy - st.note, hx + hw + 2, hy + 2, 1000);
        }
    }

    /* Direct labels: selected, Pareto, reference, then by coverage. */
    if (c->labels != LABEL_NONE) {
        size_t *by_priority = xcalloc(nvis + 1, sizeof(size_t)), count = 0;
        for (size_t k = 0; k < nvis; k++)
            if (f->variants[order[k]].n) by_priority[count++] = order[k];
        for (size_t a = 0; a < count; a++)
            for (size_t b = a + 1; b < count; b++) {
                const Variant *va = &f->variants[by_priority[a]], *vb = &f->variants[by_priority[b]];
                int pa = va->selected * 4 + va->pareto * 2 + ((long)by_priority[a] == f->baseline);
                int pb = vb->selected * 4 + vb->pareto * 2 + ((long)by_priority[b] == f->baseline);
                if (pb > pa || (pb == pa && vb->n > va->n)) {
                    size_t t = by_priority[a];
                    by_priority[a] = by_priority[b];
                    by_priority[b] = t;
                }
            }
        for (size_t k = 0; k < count; k++) {
            const Variant *v = &f->variants[by_priority[k]];
            float cx = axis_map(&ax, v->gx), cy = axis_map(&ay, v->gy), bx, by, dist;
            float w = font_width(v->label, st.label, v->selected), h = st.label * 1.05f;
            float reach = agg_r + (v->selected ? 0.5f * st.mark + st.line : 0.15f * u);
            if (!placer_place(&placer, cx, cy, reach, w, h, v->label_position, u, &bx, &by, &dist)) {
                if (v->label_position != LABEL_NONE) info->labels_dropped++;
                continue;
            }
            info->labels_drawn++;
            /* A leader line whenever the label does not touch its marker (or its selection ring). */
            float tx = fminf(fmaxf(cx, bx), bx + w), ty = fminf(fmaxf(cy, by), by + h);
            float len = hypotf(tx - cx, ty - cy);
            (void)dist;
            if (len - reach > 0.6f * u) {
                {
                    float ex = cx + (tx - cx) / len * reach, ey = cy + (ty - cy) / len * reach;
                    op_stroke(scene_line(sc, ex, ey, tx, ty), c->ink_secondary, st.hairline, 0.8f);
                }
            }
            Op *t = scene_text(sc, bx, by + font_cap_height(st.label, 0) + (h - font_cap_height(st.label, 0)) / 2, v->label, st.label, v->selected, c->ink,
                               ANCHOR_START);
            t->halo = 0.18f * u;
        }
        free(by_priority);
    }

    /* Equal-cost labels on a free stretch of their own curve. */
    for (size_t i = 0; i < level_count; i++) {
        int n = curve_n[i];
        float *pts = curves + i * SAMPLES * 2;
        char number[32], label[48];
        format_number(number, sizeof(number), levels[i]);
        snprintf(label, sizeof(label), "C=%s", number);
        float w = font_width(label, st.clabel, 0), h = st.clabel;
        float best = 1e30f, bx = 0, by = 0;
        for (int k = n / 10; k < n - n / 10; k += 4) {
            float x0 = pts[2 * k] - w / 2, y0 = pts[2 * k + 1] - h / 2;
            float score = placer_score(&placer, x0 - 3, y0 - 3, x0 + w + 3, y0 + h + 3) - (float)k * 0.02f;
            if (score < best) {
                best = score;
                bx = x0;
                by = y0;
            }
        }
        if (best < 2 * u * u) {
            Op *t = scene_text(sc, bx, by + h * 0.8f, label, st.clabel, 0, c->muted, ANCHOR_START);
            t->halo = 0.18f * u;
            placer_add(&placer, bx - 3, by - 3, bx + w + 3, by + h + 3, 1000);
        }
    }
    free(curves);
    scene_unclip(sc);

    /* Ticks and the baseline under the plot. */
    for (size_t i = 0; i < nx; i++) {
        axis_tick_label(&ax, xticks, nx, i, buffer, sizeof(buffer));
        text_centered_inside(sc, axis_map(&ax, xticks[i]), py1 + 1.25f * st.tick, buffer, st.tick, c->ink_secondary);
    }
    for (size_t i = 0; i < ny; i++) {
        axis_tick_label(&ay, yticks, ny, i, buffer, sizeof(buffer));
        scene_text(sc, px0 - 0.55f * u, axis_map(&ay, yticks[i]) + 0.36f * st.tick, buffer, st.tick, 0, c->ink_secondary, ANCHOR_END);
    }
    op_stroke(scene_line(sc, px0, py1, px1, py1), c->reference, st.hairline, 1);

    if (c->legend == LEGEND_BOTTOM && legend_h > 0) legend_draw(sc, &st, entries, nvis, LEGEND_BOTTOM, M, bottom + 0.5f * u, legend_extent);
    if (c->legend == LEGEND_RIGHT && legend_h > 0) legend_draw(sc, &st, entries, nvis, LEGEND_RIGHT, right + 1.2f * u, py0, legend_h);

    float fy = (float)H - M - note_h + st.note;
    for (int k = 0; k < note_count; k++, fy += st.note * 1.4f) scene_text(sc, M, fy, note_lines[k], st.note, 0, c->muted, ANCHOR_START);

    (void)ref;
    free(entries);
    free(details);
    free(order);
    placer_free(&placer);
    return sc;
}
