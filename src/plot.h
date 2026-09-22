/*
 * Plot kit shared by the views, and the views themselves.
 *
 * Every size is a multiple of one unit u, the base font size in pixels for
 * the chosen figure size (screen 14 px, paper 7 pt at 300 dpi, and so on),
 * so a figure keeps its proportions at every size.
 */
#ifndef ISOCOST_PLOT_H
#define ISOCOST_PLOT_H

#include "canvas.h"
#include "model.h"

typedef struct {
    const Config *config;
    FigureSize size;
    float u;                  /* base font size in pixels */
    float title, subtitle, legend, axis, tick, label, note, clabel; /* font sizes */
    float hairline, line;     /* stroke widths */
    float mark;               /* marker unit: u on screens, 0.7 u on print sizes */
    float margin;
    int show_header;          /* draw title and subtitle */
    int show_note;            /* draw the note */
} Style;

void style_init(Style *s, const Config *config, const FigureSize *size);

typedef struct {
    int log;
    double lo, hi;            /* domain in axis space (natural log when log) */
    float p0, p1;             /* pixel range; p0 maps lo */
} Axis;

double axis_space(const Axis *a, double v);
float axis_map(const Axis *a, double v);
/* Fit the domain to [vmin, vmax] with padding; forced limits (> 0) win. */
void axis_fit(Axis *a, double vmin, double vmax, double forced_min, double forced_max);
size_t axis_ticks(const Axis *a, float min_gap, double *out, size_t max);
void axis_tick_label(const Axis *a, const double *ticks, size_t n, size_t i, char *out, size_t size);

/* Marker with an optional ring in the background color. Returns the visible op. */
Op *draw_marker(Scene *sc, int shape, int hollow, Rgb color, float cx, float cy, float r, float alpha, float ring, Rgb ring_color);

typedef struct {
    const char *label;
    const char *detail;       /* muted text after the label, may be NULL */
    int shape, hollow;
    Rgb color;
} LegendEntry;

/* Height (top/bottom) or width (right) the legend needs within `extent`. */
float legend_measure(const Style *s, const LegendEntry *entries, size_t count, int position, float extent);
void legend_draw(Scene *sc, const Style *s, const LegendEntry *entries, size_t count, int position, float x, float y, float extent);

int wrap_text(const char *text, float size, float width, char lines[][512], int max_lines);
/* Centered text kept inside the canvas (tick labels at the plot edges). */
Op *text_centered_inside(Scene *sc, float x, float y, const char *text, float size, Rgb color);

typedef struct {
    float x0, y0, x1, y1, weight;
} Box;

typedef struct {
    Box *boxes;
    size_t count, capacity;
    float fx0, fy0, fx1, fy1; /* labels must stay inside */
} Placer;

void placer_add(Placer *p, float x0, float y0, float x1, float y1, float weight);
float placer_score(const Placer *p, float x0, float y0, float x1, float y1);
/* Place a w x h label near (px, py) outside radius r; position is a LABEL_* value. */
int placer_place(Placer *p, float px, float py, float r, float w, float h, int position, float u, float *bx, float *by, float *distance);
void placer_free(Placer *p);

/* Header: title and subtitle at the top-left. Returns the y below them. */
float draw_header(Scene *sc, const Style *s, const char *title, const char *subtitle, float y);
/* Footer note from the bottom edge up. Returns its height. */
float measure_note(const Style *s, const char *note, float width, char lines[][512], int *count);

typedef struct {
    int x_log, y_log;
    double cost_levels[16];
    size_t cost_level_count;
    size_t labels_drawn, labels_dropped;
    int width, height;
} ViewInfo;

Scene *view_overview(const Figure *f, const Config *c, const FigureSize *size, ViewInfo *info);
Scene *view_workloads(const Figure *f, const Config *c, const FigureSize *size, ViewInfo *info);

/* Visible tools in legend order: config order first, then id. Returns the count. */
size_t visible_tools(const Figure *f, size_t *order);
char *auto_subtitle(const Figure *f);

#endif
