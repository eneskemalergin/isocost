/*
 * Workloads: one row per workload, one panel per metric, one marker per
 * tool at its ratio to the reference. It shows where an aggregate hides a
 * workload that a tool loses. The reference sits on the vertical line at 1x
 * and is not repeated as a marker.
 */
#include "plot.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const Figure *f;
    int sort;
} RowSort;

static double reference_value(const Figure *f, size_t kase, int memory) {
    for (size_t i = 0; i < f->point_count; i++)
        if (f->points[i].kase == kase)
            return memory ? f->points[i].reference->memory.estimate : f->points[i].reference->time.estimate;
    return 0;
}

static int cmp_rows(const void *a, const void *b, const void *ctx) {
    const RowSort *rs = ctx;
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    const Workload *wx = &rs->f->cases[x], *wy = &rs->f->cases[y];
    if (rs->sort == SORT_TIME || rs->sort == SORT_MEMORY) {
        double vx = reference_value(rs->f, x, rs->sort == SORT_MEMORY), vy = reference_value(rs->f, y, rs->sort == SORT_MEMORY);
        if (vx != vy) return vx < vy ? -1 : 1;
    }
    if (rs->sort == SORT_CONFIG && wx->order != wy->order) {
        if (!wx->order) return 1;
        if (!wy->order) return -1;
        return wx->order < wy->order ? -1 : 1;
    }
    return natural_compare(wx->label, wy->label);
}

static int case_has_points(const Figure *f, size_t kase) {
    for (size_t i = 0; i < f->point_count; i++)
        if (f->points[i].kase == kase) return 1;
    return 0;
}

Scene *view_workloads(const Figure *f, const Config *c, const FigureSize *base_size, ViewInfo *info) {
    Style st;
    FigureSize size = *base_size;
    style_init(&st, c, &size);
    const float u = st.u, M = st.margin;
    size_t *order = xcalloc(f->variant_count + 1, sizeof(size_t));
    size_t nvis = visible_tools(f, order);
    char buffer[512];

    /* Rows: workloads with a reference, sorted as configured. */
    size_t *rows = xcalloc(f->case_count + 1, sizeof(size_t)), nrows = 0;
    for (size_t k = 0; k < f->case_count; k++)
        if (case_has_points(f, k)) rows[nrows++] = k;
    RowSort rs = {f, c->workloads_sort};
    sort_ctx(rows, nrows, sizeof(size_t), cmp_rows, &rs);

    /* Legend and note sizes decide the height when the size preset allows it to grow. */
    LegendEntry *entries = xcalloc(nvis + 1, sizeof(LegendEntry));
    size_t nentries = 0;
    for (size_t k = 0; k < nvis; k++) {
        const Variant *v = &f->variants[order[k]];
        if ((long)order[k] == f->baseline || !v->n) continue;
        entries[nentries++] = (LegendEntry){v->label, NULL, v->shape, v->hollow, v->color};
    }
    float legend_extent = (float)size.width - 2 * M;
    int legend = c->legend == LEGEND_RIGHT ? LEGEND_TOP : c->legend;
    float legend_h = legend_measure(&st, entries, nentries, legend, legend_extent);
    char note_lines[6][512];
    int note_count = 0;
    const char *ref_label = f->variants[f->baseline].label;
    char *note_text;
    if (strcmp(c->note, "auto") == 0)
        note_text = xasprintf("Each row is one workload. Each mark is one tool's median divided by %s's median on that workload; the vertical line at 1x is %s. "
                              "Left of the line is faster or smaller.", ref_label, ref_label);
    else
        note_text = xstrdup(c->note);
    float note_h = measure_note(&st, note_text, legend_extent, note_lines, &note_count);
    free(note_text);

    /* Workload label column. */
    float label_w = 0, label_cap = 0.3f * ((float)size.width - 2 * M);
    char **row_labels = xcalloc(nrows + 1, sizeof(char *));
    for (size_t r = 0; r < nrows; r++) {
        /* Long names are shortened with "..." so the panels keep at least 70% of the width. */
        const char *full = f->cases[rows[r]].label;
        row_labels[r] = xstrdup(full);
        size_t n = strlen(full);
        while (n > 4 && font_width(row_labels[r], st.tick, 0) > label_cap) {
            n--;
            free(row_labels[r]);
            row_labels[r] = xasprintf("%.*s...", (int)n, full);
        }
        if (c->workloads_labels) label_w = fmaxf(label_w, font_width(row_labels[r], st.tick, 0));
    }
    float left = M + (label_w > 0 ? label_w + 0.8f * u : 0);
    int panels = c->workloads_time + c->workloads_memory;
    if (panels == 0) panels = 1;
    float gap = 1.6f * u;
    float panel_w = ((float)size.width - M - left - gap * (float)(panels - 1)) / (float)panels;
    /* Room for panel titles that wrap onto a second line in narrow panels. */
    int title_lines_needed = 1;
    {
        char probe[3][512];
        snprintf(buffer, sizeof(buffer), "Peak memory relative to %s", ref_label);
        title_lines_needed = wrap_text(buffer, st.axis, panel_w, probe, 2);
    }
    float row_h = 2.2f * u;
    float header = st.show_header ? M + font_cap_height(st.title, 1) + 0.75f * u + font_cap_height(st.subtitle, 0) + font_descender(st.subtitle, 0) + 0.9f * u : M;
    float chrome = header + legend_h + 0.3f * u + 1.3f * u + font_cap_height(st.axis, 0) + (float)(title_lines_needed - 1) * 1.25f * st.axis +
                   1.25f * st.tick + 0.8f * u + note_h + M;
    if (!size.custom) {
        float rows_h = row_h * (float)(nrows ? nrows : 1);
        if (rows_h < 7.0f * u) { /* a few rows still get a readable plot */
            row_h = 7.0f * u / (float)(nrows ? nrows : 1);
            rows_h = 7.0f * u;
        }
        int needed = (int)ceilf(chrome + rows_h + 0.5f * u);
        size.height = needed;
        size.height_pt = (double)needed * 72.0 / size.dpi;
    }
    const int W = size.width, H = size.height;
    info->width = W;
    info->height = H;
    Scene *sc = scene_new(W, H, size.dpi, size.width_pt, size.height_pt, c->background, f->title);

    char *subtitle = f->subtitle ? xstrdup(f->subtitle) : auto_subtitle(f);
    float y = draw_header(sc, &st, f->title, subtitle, M);
    free(subtitle);
    float top = st.show_header ? y + 0.9f * u : y;
    if (legend == LEGEND_TOP && legend_h > 0) {
        legend_draw(sc, &st, entries, nentries, LEGEND_TOP, M, top, legend_extent);
        top += legend_h + 0.3f * u;
    }

    float plot_top = top + 1.3f * u + font_cap_height(st.axis, 0) + (float)(title_lines_needed - 1) * 1.25f * st.axis;
    float plot_bottom = plot_top + row_h * (float)(nrows ? nrows : 1);
    if (size.custom) {
        float limit = (float)H - M - note_h - 0.8f * u - 1.25f * st.tick;
        if (limit < plot_bottom) {
            row_h = (limit - plot_top) / (float)(nrows ? nrows : 1);
            plot_bottom = limit;
        }
    }

    int metric_list[2], nmetrics = 0;
    if (c->workloads_time) metric_list[nmetrics++] = 0;
    if (c->workloads_memory) metric_list[nmetrics++] = 1;
    if (!nmetrics) metric_list[nmetrics++] = 0;

    for (int p = 0; p < nmetrics; p++) {
        int memory = metric_list[p];
        float x0 = left + (float)p * (panel_w + gap), x1 = x0 + panel_w;
        double vmin = 1, vmax = 1;
        for (size_t i = 0; i < f->point_count; i++) {
            const CasePoint *pt = &f->points[i];
            if (f->variants[pt->variant].hidden) continue;
            double v = memory ? pt->y : pt->x;
            vmin = fmin(vmin, v);
            vmax = fmax(vmax, v);
        }
        Axis ax = {0};
        ax.log = c->workloads_scale != SCALE_LINEAR;
        axis_fit(&ax, vmin, vmax, 0, 0);
        ax.p0 = x0;
        ax.p1 = x1;
        double ticks[64];
        size_t nt = axis_ticks(&ax, (ax.log ? 4.2f : 6.0f) * u, ticks, 64);

        char title_lines[3][512];
        snprintf(buffer, sizeof(buffer), "%s relative to %s", memory ? "Peak memory" : "Run time", ref_label);
        int nl = wrap_text(buffer, st.axis, panel_w, title_lines, 2);
        for (int k = 0; k < nl; k++)
            scene_text(sc, x0, plot_top - 0.7f * u - (float)(nl - 1 - k) * 1.25f * st.axis, title_lines[k], st.axis, 0, c->ink_secondary, ANCHOR_START);

        scene_clip(sc, x0, plot_top, panel_w, plot_bottom - plot_top);
        float rx = axis_map(&ax, 1);
        if (c->highlight && rx > x0) op_fill(scene_rect(sc, x0, plot_top, rx - x0, plot_bottom - plot_top), c->region, 1);
        for (size_t t = 0; t < nt; t++) op_stroke(scene_line(sc, axis_map(&ax, ticks[t]), plot_top, axis_map(&ax, ticks[t]), plot_bottom), c->grid, st.hairline, 1);
        for (size_t r = 0; r < nrows; r++) {
            float cy = plot_top + row_h * ((float)r + 0.5f);
            op_stroke(scene_line(sc, x0, cy, x1, cy), c->grid, st.hairline, 0.7f);
        }
        op_stroke(scene_line(sc, rx, plot_top, rx, plot_bottom), c->reference, 1.3f * st.hairline, 1);

        float mr = fminf(0.36f * st.mark, row_h * 0.3f);
        for (size_t r = 0; r < nrows; r++) {
            float cy = plot_top + row_h * ((float)r + 0.5f);
            for (size_t k = 0; k < nvis; k++) {
                size_t v = order[k];
                if ((long)v == f->baseline) continue;
                for (size_t i = 0; i < f->point_count; i++) {
                    const CasePoint *pt = &f->points[i];
                    if (pt->kase != rows[r] || pt->variant != v) continue;
                    double value = memory ? pt->y : pt->x;
                    Op *m = draw_marker(sc, f->variants[v].shape, f->variants[v].hollow, f->variants[v].color, axis_map(&ax, value), cy, mr, 0.95f,
                                        0.1f * u, c->background);
                    snprintf(buffer, sizeof(buffer), "%s, %s: %s %.3gx", f->variants[v].label, f->cases[rows[r]].label,
                             memory ? "peak memory" : "run time", value);
                    op_tooltip(m, buffer);
                }
            }
        }
        scene_unclip(sc);
        op_stroke(scene_line(sc, x0, plot_bottom, x1, plot_bottom), c->reference, st.hairline, 1);
        for (size_t t = 0; t < nt; t++) {
            axis_tick_label(&ax, ticks, nt, t, buffer, sizeof(buffer));
            text_centered_inside(sc, axis_map(&ax, ticks[t]), plot_bottom + 1.25f * st.tick, buffer, st.tick, c->ink_secondary);
        }
    }
    if (c->workloads_labels)
        for (size_t r = 0; r < nrows; r++) {
            float cy = plot_top + row_h * ((float)r + 0.5f);
            Op *t = scene_text(sc, left - 0.8f * u, cy + 0.36f * st.tick, row_labels[r], st.tick, 0, c->ink, ANCHOR_END);
            if (strcmp(row_labels[r], f->cases[rows[r]].label) != 0) op_tooltip(t, f->cases[rows[r]].label);
        }
    if (legend == LEGEND_BOTTOM && legend_h > 0)
        legend_draw(sc, &st, entries, nentries, LEGEND_BOTTOM, M, plot_bottom + 1.25f * st.tick + 0.8f * u, legend_extent);

    float fy = (float)H - M - note_h + st.note;
    for (int k = 0; k < note_count; k++, fy += st.note * 1.4f) scene_text(sc, M, fy, note_lines[k], st.note, 0, c->muted, ANCHOR_START);

    info->x_log = c->workloads_scale != SCALE_LINEAR;
    for (size_t r = 0; r < nrows; r++) free(row_labels[r]);
    free(row_labels);
    free(entries);
    free(rows);
    free(order);
    return sc;
}
