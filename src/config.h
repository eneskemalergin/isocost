/*
 * isocost.toml: every setting a report can change, with built-in defaults.
 * Precedence: defaults, then the config file, then command-line options.
 */
#ifndef ISOCOST_CONFIG_H
#define ISOCOST_CONFIG_H

#include "util.h"

typedef struct { unsigned char r, g, b; } Rgb;

enum { STAT_MEDIAN, STAT_MEAN };
enum { COST_GEOMETRIC, COST_LINEAR };
enum { SCALE_AUTO, SCALE_LOG, SCALE_LINEAR };
enum { SPREAD_IQR, SPREAD_MINMAX, SPREAD_NONE };
enum { LEGEND_TOP, LEGEND_BOTTOM, LEGEND_RIGHT, LEGEND_NONE };
enum { HEADER_AUTO, HEADER_SHOW, HEADER_HIDE };
enum { SORT_NAME, SORT_TIME, SORT_MEMORY, SORT_CONFIG };
enum { SHAPE_AUTO = -1, SHAPE_CIRCLE, SHAPE_SQUARE, SHAPE_TRIANGLE, SHAPE_DIAMOND, SHAPE_TRIANGLE_DOWN,
       SHAPE_PLUS, SHAPE_STAR, SHAPE_CROSS, SHAPE_COUNT };
enum { FILL_AUTO = -1, FILL_SOLID, FILL_HOLLOW };
enum { LINE_SOLID, LINE_DASHED, LINE_DOTTED };
enum { LABEL_AUTO, LABEL_LEFT, LABEL_RIGHT, LABEL_ABOVE, LABEL_BELOW, LABEL_NONE };

#define VIEW_OVERVIEW 1u
#define VIEW_WORKLOADS 2u
#define FORMAT_PNG 1u
#define FORMAT_SVG 2u
#define FORMAT_PDF 4u

typedef struct {
    char *id;
    char *match;          /* NULL: styling only */
    char *label;          /* NULL: id */
    int has_color;
    Rgb color;
    int shape;            /* SHAPE_AUTO or a shape */
    int fill;             /* FILL_AUTO, FILL_SOLID, FILL_HOLLOW */
    int line;
    int order;            /* 0: unordered */
    int hide;
    int label_position;
    int line_number;      /* where it was defined, for messages */
} ToolSpec;

typedef struct {
    char *id;
    char *label;
    int order;
    int hide;
} WorkloadSpec;

typedef struct {
    char *match;
    char *title;          /* NULL: not set */
    char *subtitle;
    char *baseline;
    int hide;
} GroupSpec;

typedef struct {
    double *values;
    size_t count;
} Numbers;

typedef struct {
    char *path;                /* config file that was read, NULL for defaults */

    /* [report] */
    char *report_title;
    char *baseline;
    int statistic;
    int cost_model;
    double time_weight, memory_weight;
    int common_cases;
    int all_runs;
    Vec groups;                /* char* globs */
    Vec suites;
    unsigned views;
    unsigned formats;
    int data;

    /* [figure] */
    char *size;
    double dpi;
    double font_scale;
    char *title;               /* template */
    char *subtitle;            /* "auto", "", or template */
    int legend;
    int header;                /* HEADER_AUTO: shown on screen and slide sizes, hidden on print sizes */
    char *note;                /* "auto", "", or text */

    /* [overview] */
    char *x_label, *y_label;
    int x_scale, y_scale;
    double x_min, x_max, y_min, y_max;
    Numbers x_ticks, y_ticks;
    int contours;
    Numbers contour_levels;
    int case_points;
    int spread;
    int ellipse;
    int pareto;
    int highlight;
    int reference_lines;
    int labels;

    /* [workloads] */
    int workloads_time, workloads_memory;
    int workloads_sort;
    int workloads_scale;
    int workloads_labels;

    /* [theme] */
    Rgb background, ink, ink_secondary, muted, grid, reference, contour, region;
    Rgb palette[16];
    int palette_count;

    ToolSpec *tools;
    size_t tool_count;
    WorkloadSpec *workloads;
    size_t workload_count;
    GroupSpec *groups_spec;
    size_t group_count;
} Config;

/* A resolved canvas: pixels for rasters, points for PDF, base font in pixels. */
typedef struct {
    char name[64];
    int width, height;         /* pixels */
    double dpi;
    double base_px;            /* base font size in pixels */
    double width_pt, height_pt;
    int custom;                /* not a named preset */
    int print;                 /* physical size (paper, paper-wide, mm, in): compact print layout */
} FigureSize;

int figure_size(const char *spec, double dpi_override, FigureSize *out, char *error, size_t error_size);

void config_defaults(Config *c);
/* Reads and validates path. On error writes "path:line:column: message" into error and returns 0. */
int config_load(Config *c, const char *path, char *error, size_t error_size);
/* isocost.toml next to the first input or in a parent, stopping after a .git directory or 4 levels. */
char *config_discover(const char *first_input);
/* Print the configuration as TOML with a comment on every key. */
void config_print(const Config *c, FILE *out);
void config_free(Config *c);

const ToolSpec *config_tool(const Config *c, const char *id, const char *family);
const WorkloadSpec *config_workload(const Config *c, const char *id);
const GroupSpec *config_group(const Config *c, const char *group);

int parse_color(const char *text, Rgb *out);

#endif
