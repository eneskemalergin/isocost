#include "config.h"
#include "platform.h"
#include "toml.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Validated for all-pairs (scatter) use on #fcfcfb; see plan/SPEC.md. */
static const unsigned DEFAULT_PALETTE[8] = {0x2a78d6, 0xeb6834, 0x1baf7a, 0x763a64, 0xb191ea, 0xce35a8, 0x876114, 0x5922bf};

static const char *const SHAPES[] = {"circle", "square", "triangle", "diamond", "triangle-down", "plus", "star", "cross", NULL};
static const char *const FILLS[] = {"solid", "hollow", NULL};
static const char *const LINES[] = {"solid", "dashed", "dotted", NULL};
static const char *const LABEL_POSITIONS[] = {"auto", "left", "right", "above", "below", "none", NULL};
static const char *const STATISTICS[] = {"median", "mean", NULL};
static const char *const COSTS[] = {"geometric", "linear", NULL};
static const char *const CASES[] = {"all", "common", NULL};
static const char *const RUNS[] = {"latest", "all", NULL};
static const char *const SCALES[] = {"auto", "log", "linear", NULL};
static const char *const SPREADS[] = {"iqr", "minmax", "none", NULL};
static const char *const LEGENDS[] = {"top", "bottom", "right", "none", NULL};
static const char *const HEADERS[] = {"auto", "show", "hide", NULL};
static const char *const SORTS[] = {"name", "time", "memory", "config", NULL};
static const char *const LABEL_MODES[] = {"auto", "none", NULL};

static Rgb rgb(unsigned hex) {
    Rgb c = {(unsigned char)(hex >> 16), (unsigned char)(hex >> 8), (unsigned char)hex};
    return c;
}

int parse_color(const char *text, Rgb *out) {
    unsigned value = 0;
    if (!text || text[0] != '#' || strlen(text) != 7) return 0;
    for (int i = 1; i < 7; i++) {
        int c = tolower((unsigned char)text[i]);
        if (!isxdigit(c)) return 0;
        value = value * 16 + (unsigned)(isdigit(c) ? c - '0' : c - 'a' + 10);
    }
    *out = rgb(value);
    return 1;
}

void config_defaults(Config *c) {
    memset(c, 0, sizeof(*c));
    c->report_title = xstrdup("Speed and memory report");
    c->baseline = xstrdup("");
    c->statistic = STAT_MEDIAN;
    c->cost_model = COST_GEOMETRIC;
    c->time_weight = 1;
    c->memory_weight = 1;
    vec_push(&c->groups, xstrdup("*"));
    vec_push(&c->suites, xstrdup("*"));
    c->views = VIEW_OVERVIEW | VIEW_WORKLOADS;
    c->formats = FORMAT_PNG;

    c->size = xstrdup("screen");
    c->font_scale = 1;
    c->title = xstrdup("{project} {suite} {group}");
    c->subtitle = xstrdup("auto");
    c->legend = LEGEND_TOP;
    c->note = xstrdup("auto");

    c->x_label = xstrdup("Run time relative to {reference}");
    c->y_label = xstrdup("Peak memory relative to {reference}");
    c->contours = c->case_points = c->ellipse = c->pareto = c->highlight = c->reference_lines = 1;
    c->spread = SPREAD_IQR;
    c->labels = LABEL_AUTO;

    c->workloads_time = c->workloads_memory = 1;
    c->workloads_sort = SORT_NAME;
    c->workloads_scale = SCALE_LOG;
    c->workloads_labels = 1;

    c->background = rgb(0xfcfcfb);
    c->ink = rgb(0x0b0b0b);
    c->ink_secondary = rgb(0x52514e);
    c->muted = rgb(0x898781);
    c->grid = rgb(0xe1e0d9);
    c->reference = rgb(0xa9a79f);
    c->contour = rgb(0xb9b7ae);
    c->region = rgb(0xf1f0ea);
    c->palette_count = 8;
    for (int i = 0; i < 8; i++) c->palette[i] = rgb(DEFAULT_PALETTE[i]);
}

static void free_strings(Vec *v) {
    for (size_t i = 0; i < v->count; i++) free(v->items[i]);
    vec_free(v);
}

void config_free(Config *c) {
    free(c->path);
    free(c->report_title);
    free(c->baseline);
    free_strings(&c->groups);
    free_strings(&c->suites);
    free(c->size);
    free(c->title);
    free(c->subtitle);
    free(c->note);
    free(c->x_label);
    free(c->y_label);
    free(c->x_ticks.values);
    free(c->y_ticks.values);
    free(c->contour_levels.values);
    for (size_t i = 0; i < c->tool_count; i++) {
        free(c->tools[i].id);
        free(c->tools[i].match);
        free(c->tools[i].label);
    }
    free(c->tools);
    for (size_t i = 0; i < c->workload_count; i++) {
        free(c->workloads[i].id);
        free(c->workloads[i].label);
    }
    free(c->workloads);
    for (size_t i = 0; i < c->group_count; i++) {
        free(c->groups_spec[i].match);
        free(c->groups_spec[i].title);
        free(c->groups_spec[i].subtitle);
        free(c->groups_spec[i].baseline);
    }
    free(c->groups_spec);
    memset(c, 0, sizeof(*c));
}

/* ---- loading ---------------------------------------------------------------- */

typedef struct {
    const char *path;
    char *error;
    size_t error_size;
    int failed;
} Loader;

static int loader_fail(Loader *ld, const TomlValue *at, const char *format, ...) ISO_PRINTF(3, 4);
static int loader_fail(Loader *ld, const TomlValue *at, const char *format, ...) {
    va_list args;
    char message[400];
    if (ld->failed) return 0;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    snprintf(ld->error, ld->error_size, "%s:%d:%d: %s", ld->path, at ? at->line : 1, at ? at->column : 1, message);
    ld->failed = 1;
    return 0;
}

static const char *closest(const char *key, const char *const *allowed) {
    const char *best = NULL;
    size_t best_distance = (size_t)-1;
    for (size_t i = 0; allowed[i]; i++) {
        size_t d = edit_distance(key, allowed[i]);
        if (d < best_distance) {
            best_distance = d;
            best = allowed[i];
        }
    }
    size_t limit = strlen(key) / 3 + 1;
    return best_distance <= (limit > 3 ? 3 : limit) ? best : NULL;
}

static int check_keys(Loader *ld, const TomlValue *table, const char *where, const char *const *allowed) {
    for (size_t i = 0; i < table->count; i++) {
        int known = 0;
        for (size_t k = 0; allowed[k] && !known; k++) known = strcmp(table->keys[i], allowed[k]) == 0;
        if (!known) {
            const char *hint = closest(table->keys[i], allowed);
            Str list = {0};
            for (size_t k = 0; allowed[k]; k++) str_printf(&list, "%s%s", k ? ", " : "", allowed[k]);
            if (hint) loader_fail(ld, table->items[i], "unknown key '%s' in %s; did you mean '%s'?", table->keys[i], where, hint);
            else loader_fail(ld, table->items[i], "unknown key '%s' in %s; valid keys: %s", table->keys[i], where, list.data);
            str_free(&list);
            return 0;
        }
    }
    return 1;
}

static const TomlValue *find(const TomlValue *table, const char *key) {
    if (!table) return NULL;
    for (size_t i = 0; i < table->count; i++)
        if (strcmp(table->keys[i], key) == 0) return table->items[i];
    return NULL;
}

static int want_type(Loader *ld, const TomlValue *v, TomlType type, const char *where, const char *key) {
    int ok = v->type == type || (type == TOML_FLOAT && v->type == TOML_INTEGER);
    if (!ok) loader_fail(ld, v, "%s.%s must be a %s, not a %s", where, key, toml_type_name(type), toml_type_name(v->type));
    return ok;
}

static void get_string(Loader *ld, const TomlValue *t, const char *where, const char *key, char **out) {
    const TomlValue *v = find(t, key);
    if (!v || !want_type(ld, v, TOML_STRING, where, key)) return;
    free(*out);
    *out = xstrdup(v->string);
}

static void get_bool(Loader *ld, const TomlValue *t, const char *where, const char *key, int *out) {
    const TomlValue *v = find(t, key);
    if (v && want_type(ld, v, TOML_BOOL, where, key)) *out = v->boolean;
}

static void get_number(Loader *ld, const TomlValue *t, const char *where, const char *key, double *out, double min, double max) {
    const TomlValue *v = find(t, key);
    if (!v || !want_type(ld, v, TOML_FLOAT, where, key)) return;
    if (v->number < min || v->number > max) {
        loader_fail(ld, v, "%s.%s must be between %g and %g", where, key, min, max);
        return;
    }
    *out = v->number;
}

static void get_int(Loader *ld, const TomlValue *t, const char *where, const char *key, int *out, int min, int max) {
    const TomlValue *v = find(t, key);
    if (!v || !want_type(ld, v, TOML_INTEGER, where, key)) return;
    if (v->number < min || v->number > max) {
        loader_fail(ld, v, "%s.%s must be between %d and %d", where, key, min, max);
        return;
    }
    *out = (int)v->number;
}

static int enum_index(const char *text, const char *const *names) {
    for (int i = 0; names[i]; i++)
        if (strcmp(text, names[i]) == 0) return i;
    return -1;
}

static int enum_value(Loader *ld, const TomlValue *v, const char *where, const char *key, const char *const *names) {
    int index = enum_index(v->string, names);
    if (index < 0) {
        Str list = {0};
        for (int i = 0; names[i]; i++) str_printf(&list, "%s%s", i ? ", " : "", names[i]);
        const char *hint = closest(v->string, names);
        if (hint) loader_fail(ld, v, "%s.%s = \"%s\" is not valid; did you mean \"%s\"? (choices: %s)", where, key, v->string, hint, list.data);
        else loader_fail(ld, v, "%s.%s = \"%s\" is not valid (choices: %s)", where, key, v->string, list.data);
        str_free(&list);
    }
    return index;
}

static void get_enum(Loader *ld, const TomlValue *t, const char *where, const char *key, const char *const *names, int *out) {
    const TomlValue *v = find(t, key);
    if (!v || !want_type(ld, v, TOML_STRING, where, key)) return;
    int index = enum_value(ld, v, where, key, names);
    if (index >= 0) *out = index;
}

static void get_color(Loader *ld, const TomlValue *t, const char *where, const char *key, Rgb *out, int *has) {
    const TomlValue *v = find(t, key);
    if (!v || !want_type(ld, v, TOML_STRING, where, key)) return;
    if (!parse_color(v->string, out)) {
        loader_fail(ld, v, "%s.%s = \"%s\" is not a color; use \"#rrggbb\"", where, key, v->string);
        return;
    }
    if (has) *has = 1;
}

static void get_strings(Loader *ld, const TomlValue *t, const char *where, const char *key, Vec *out) {
    const TomlValue *v = find(t, key);
    if (!v || !want_type(ld, v, TOML_ARRAY, where, key)) return;
    for (size_t i = 0; i < v->count; i++)
        if (!want_type(ld, v->items[i], TOML_STRING, where, key)) return;
    free_strings(out);
    for (size_t i = 0; i < v->count; i++) vec_push(out, xstrdup(v->items[i]->string));
}

static void get_numbers(Loader *ld, const TomlValue *t, const char *where, const char *key, Numbers *out) {
    const TomlValue *v = find(t, key);
    if (!v || !want_type(ld, v, TOML_ARRAY, where, key)) return;
    for (size_t i = 0; i < v->count; i++) {
        if (!want_type(ld, v->items[i], TOML_FLOAT, where, key)) return;
        if (!(v->items[i]->number > 0)) {
            loader_fail(ld, v->items[i], "%s.%s values must be positive ratios", where, key);
            return;
        }
    }
    free(out->values);
    out->values = xcalloc(v->count + 1, sizeof(double));
    out->count = v->count;
    for (size_t i = 0; i < v->count; i++) out->values[i] = v->items[i]->number;
    sort_ctx(out->values, out->count, sizeof(double), cmp_double, NULL);
}

static unsigned get_flags(Loader *ld, const TomlValue *t, const char *where, const char *key, const char *const *names, unsigned current) {
    const TomlValue *v = find(t, key);
    unsigned flags = 0;
    if (!v || !want_type(ld, v, TOML_ARRAY, where, key)) return current;
    for (size_t i = 0; i < v->count; i++) {
        if (!want_type(ld, v->items[i], TOML_STRING, where, key)) return current;
        int index = enum_value(ld, v->items[i], where, key, names);
        if (index < 0) return current;
        flags |= 1u << index;
    }
    if (!flags) {
        loader_fail(ld, v, "%s.%s must list at least one value", where, key);
        return current;
    }
    return flags;
}

static const TomlValue *section(Loader *ld, const TomlValue *root, const char *name) {
    const TomlValue *t = find(root, name);
    if (t && t->type != TOML_TABLE) {
        loader_fail(ld, t, "[%s] must be a table", name);
        return NULL;
    }
    return t;
}

static const TomlValue *array_of_tables(Loader *ld, const TomlValue *root, const char *name) {
    const TomlValue *t = find(root, name);
    if (t && (t->type != TOML_ARRAY || !t->array_of_tables)) {
        loader_fail(ld, t, "'%s' must be written as [[%s]] entries", name, name);
        return NULL;
    }
    return t;
}

static void load_report(Loader *ld, Config *c, const TomlValue *t) {
    static const char *const keys[] = {"title", "baseline", "statistic", "cost", "time_weight", "memory_weight", "cases",
                                       "runs", "groups", "suites", "views", "formats", "data", NULL};
    static const char *const views[] = {"overview", "workloads", NULL};
    static const char *const formats[] = {"png", "svg", "pdf", NULL};
    int cases = c->common_cases, runs = c->all_runs;
    if (!t || !check_keys(ld, t, "[report]", keys)) return;
    get_string(ld, t, "report", "title", &c->report_title);
    get_string(ld, t, "report", "baseline", &c->baseline);
    get_enum(ld, t, "report", "statistic", STATISTICS, &c->statistic);
    get_enum(ld, t, "report", "cost", COSTS, &c->cost_model);
    get_number(ld, t, "report", "time_weight", &c->time_weight, 0, 1e6);
    get_number(ld, t, "report", "memory_weight", &c->memory_weight, 0, 1e6);
    if (c->time_weight + c->memory_weight <= 0) loader_fail(ld, find(t, "time_weight"), "report.time_weight and report.memory_weight cannot both be 0");
    get_enum(ld, t, "report", "cases", CASES, &cases);
    get_enum(ld, t, "report", "runs", RUNS, &runs);
    c->common_cases = cases;
    c->all_runs = runs;
    get_strings(ld, t, "report", "groups", &c->groups);
    get_strings(ld, t, "report", "suites", &c->suites);
    c->views = get_flags(ld, t, "report", "views", views, c->views);
    c->formats = get_flags(ld, t, "report", "formats", formats, c->formats);
    get_bool(ld, t, "report", "data", &c->data);
}

static void load_figure(Loader *ld, Config *c, const TomlValue *t) {
    static const char *const keys[] = {"size", "dpi", "font_scale", "header", "title", "subtitle", "legend", "note", NULL};
    if (!t || !check_keys(ld, t, "[figure]", keys)) return;
    get_string(ld, t, "figure", "size", &c->size);
    get_number(ld, t, "figure", "dpi", &c->dpi, 0, 2400);
    get_number(ld, t, "figure", "font_scale", &c->font_scale, 0.25, 4);
    get_enum(ld, t, "figure", "header", HEADERS, &c->header);
    get_string(ld, t, "figure", "title", &c->title);
    get_string(ld, t, "figure", "subtitle", &c->subtitle);
    get_enum(ld, t, "figure", "legend", LEGENDS, &c->legend);
    get_string(ld, t, "figure", "note", &c->note);
    if (!ld->failed) {
        FigureSize size;
        char message[200];
        if (!figure_size(c->size, c->dpi, &size, message, sizeof(message))) loader_fail(ld, find(t, "size"), "figure.size: %s", message);
    }
}

static void load_overview(Loader *ld, Config *c, const TomlValue *t) {
    static const char *const keys[] = {"x_label", "y_label", "x_scale", "y_scale", "x_min", "x_max", "y_min", "y_max",
                                       "x_ticks", "y_ticks", "contours", "contour_levels", "case_points", "spread",
                                       "ellipse", "pareto", "highlight", "reference_lines", "labels", NULL};
    if (!t || !check_keys(ld, t, "[overview]", keys)) return;
    get_string(ld, t, "overview", "x_label", &c->x_label);
    get_string(ld, t, "overview", "y_label", &c->y_label);
    get_enum(ld, t, "overview", "x_scale", SCALES, &c->x_scale);
    get_enum(ld, t, "overview", "y_scale", SCALES, &c->y_scale);
    get_number(ld, t, "overview", "x_min", &c->x_min, 0, 1e12);
    get_number(ld, t, "overview", "x_max", &c->x_max, 0, 1e12);
    get_number(ld, t, "overview", "y_min", &c->y_min, 0, 1e12);
    get_number(ld, t, "overview", "y_max", &c->y_max, 0, 1e12);
    if (c->x_min > 0 && c->x_max > 0 && c->x_min >= c->x_max) loader_fail(ld, find(t, "x_min"), "overview.x_min must be below overview.x_max");
    if (c->y_min > 0 && c->y_max > 0 && c->y_min >= c->y_max) loader_fail(ld, find(t, "y_min"), "overview.y_min must be below overview.y_max");
    get_numbers(ld, t, "overview", "x_ticks", &c->x_ticks);
    get_numbers(ld, t, "overview", "y_ticks", &c->y_ticks);
    get_bool(ld, t, "overview", "contours", &c->contours);
    get_numbers(ld, t, "overview", "contour_levels", &c->contour_levels);
    get_bool(ld, t, "overview", "case_points", &c->case_points);
    get_enum(ld, t, "overview", "spread", SPREADS, &c->spread);
    get_bool(ld, t, "overview", "ellipse", &c->ellipse);
    get_bool(ld, t, "overview", "pareto", &c->pareto);
    get_bool(ld, t, "overview", "highlight", &c->highlight);
    get_bool(ld, t, "overview", "reference_lines", &c->reference_lines);
    get_enum(ld, t, "overview", "labels", LABEL_MODES, &c->labels);
}

static void load_workloads(Loader *ld, Config *c, const TomlValue *t) {
    static const char *const keys[] = {"metrics", "sort", "scale", "labels", NULL};
    static const char *const metrics[] = {"time", "memory", NULL};
    static const char *const scales[] = {"log", "linear", NULL};
    int scale = c->workloads_scale == SCALE_LINEAR;
    if (!t || !check_keys(ld, t, "[workloads]", keys)) return;
    unsigned m = get_flags(ld, t, "workloads", "metrics", metrics, (unsigned)(c->workloads_time | (c->workloads_memory << 1)));
    c->workloads_time = (m & 1u) != 0;
    c->workloads_memory = (m & 2u) != 0;
    get_enum(ld, t, "workloads", "sort", SORTS, &c->workloads_sort);
    get_enum(ld, t, "workloads", "scale", scales, &scale);
    c->workloads_scale = scale ? SCALE_LINEAR : SCALE_LOG;
    get_bool(ld, t, "workloads", "labels", &c->workloads_labels);
}

static void load_theme(Loader *ld, Config *c, const TomlValue *t) {
    static const char *const keys[] = {"background", "ink", "ink_secondary", "muted", "grid", "reference", "contour",
                                       "region", "palette", NULL};
    if (!t || !check_keys(ld, t, "[theme]", keys)) return;
    get_color(ld, t, "theme", "background", &c->background, NULL);
    get_color(ld, t, "theme", "ink", &c->ink, NULL);
    get_color(ld, t, "theme", "ink_secondary", &c->ink_secondary, NULL);
    get_color(ld, t, "theme", "muted", &c->muted, NULL);
    get_color(ld, t, "theme", "grid", &c->grid, NULL);
    get_color(ld, t, "theme", "reference", &c->reference, NULL);
    get_color(ld, t, "theme", "contour", &c->contour, NULL);
    get_color(ld, t, "theme", "region", &c->region, NULL);
    const TomlValue *p = find(t, "palette");
    if (p && want_type(ld, p, TOML_ARRAY, "theme", "palette")) {
        if (p->count < 1 || p->count > 16) {
            loader_fail(ld, p, "theme.palette needs 1 to 16 colors");
            return;
        }
        for (size_t i = 0; i < p->count; i++) {
            if (!want_type(ld, p->items[i], TOML_STRING, "theme", "palette")) return;
            if (!parse_color(p->items[i]->string, &c->palette[i])) {
                loader_fail(ld, p->items[i], "theme.palette entry \"%s\" is not a color; use \"#rrggbb\"", p->items[i]->string);
                return;
            }
        }
        c->palette_count = (int)p->count;
    }
}

static void load_tools(Loader *ld, Config *c, const TomlValue *list) {
    static const char *const keys[] = {"id", "match", "label", "color", "shape", "fill", "line", "order", "hide",
                                       "label_position", NULL};
    if (!list) return;
    c->tools = xcalloc(list->count + 1, sizeof(ToolSpec));
    for (size_t i = 0; i < list->count && !ld->failed; i++) {
        const TomlValue *t = list->items[i];
        ToolSpec *s = &c->tools[c->tool_count++];
        s->shape = SHAPE_AUTO;
        s->fill = FILL_AUTO;
        s->line_number = t->line;
        if (!check_keys(ld, t, "[[tool]]", keys)) return;
        get_string(ld, t, "tool", "id", &s->id);
        if (!s->id || !*s->id) {
            loader_fail(ld, t, "every [[tool]] needs a non-empty id");
            return;
        }
        for (size_t k = 0; k + 1 < c->tool_count; k++)
            if (strcmp(c->tools[k].id, s->id) == 0) {
                loader_fail(ld, find(t, "id"), "[[tool]] id \"%s\" is also defined on line %d", s->id, c->tools[k].line_number);
                return;
            }
        get_string(ld, t, "tool", "match", &s->match);
        get_string(ld, t, "tool", "label", &s->label);
        get_color(ld, t, "tool", "color", &s->color, &s->has_color);
        get_enum(ld, t, "tool", "shape", SHAPES, &s->shape);
        get_enum(ld, t, "tool", "fill", FILLS, &s->fill);
        get_enum(ld, t, "tool", "line", LINES, &s->line);
        get_int(ld, t, "tool", "order", &s->order, 0, 100000);
        get_bool(ld, t, "tool", "hide", &s->hide);
        get_enum(ld, t, "tool", "label_position", LABEL_POSITIONS, &s->label_position);
    }
}

static void load_workload_specs(Loader *ld, Config *c, const TomlValue *list) {
    static const char *const keys[] = {"id", "label", "order", "hide", NULL};
    if (!list) return;
    c->workloads = xcalloc(list->count + 1, sizeof(WorkloadSpec));
    for (size_t i = 0; i < list->count && !ld->failed; i++) {
        const TomlValue *t = list->items[i];
        WorkloadSpec *s = &c->workloads[c->workload_count++];
        if (!check_keys(ld, t, "[[workload]]", keys)) return;
        get_string(ld, t, "workload", "id", &s->id);
        if (!s->id || !*s->id) {
            loader_fail(ld, t, "every [[workload]] needs a non-empty id");
            return;
        }
        get_string(ld, t, "workload", "label", &s->label);
        get_int(ld, t, "workload", "order", &s->order, 0, 100000);
        get_bool(ld, t, "workload", "hide", &s->hide);
    }
}

static void load_groups(Loader *ld, Config *c, const TomlValue *list) {
    static const char *const keys[] = {"match", "title", "subtitle", "baseline", "hide", NULL};
    if (!list) return;
    c->groups_spec = xcalloc(list->count + 1, sizeof(GroupSpec));
    for (size_t i = 0; i < list->count && !ld->failed; i++) {
        const TomlValue *t = list->items[i];
        GroupSpec *s = &c->groups_spec[c->group_count++];
        if (!check_keys(ld, t, "[[group]]", keys)) return;
        get_string(ld, t, "group", "match", &s->match);
        if (!s->match) {
            loader_fail(ld, t, "every [[group]] needs a match glob, for example match = \"perf_*\"");
            return;
        }
        get_string(ld, t, "group", "title", &s->title);
        get_string(ld, t, "group", "subtitle", &s->subtitle);
        get_string(ld, t, "group", "baseline", &s->baseline);
        get_bool(ld, t, "group", "hide", &s->hide);
    }
}

int config_load(Config *c, const char *path, char *error, size_t error_size) {
    unsigned char *data;
    size_t length;
    const char *why = NULL;
    char toml_error[300];
    Loader ld = {path, error, error_size, 0};
    if (!pf_read_file(path, &data, &length, 4u << 20, &why)) {
        snprintf(error, error_size, "%s: cannot read: %s", path, why);
        return 0;
    }
    TomlValue *root = toml_parse((const char *)data, length, toml_error, sizeof(toml_error));
    free(data);
    if (!root) {
        snprintf(error, error_size, "%s:%s", path, toml_error);
        return 0;
    }
    static const char *const top[] = {"schema", "report", "figure", "overview", "workloads", "theme", "tool", "workload", "group", NULL};
    if (check_keys(&ld, root, "the top level", top)) {
        const TomlValue *schema = find(root, "schema");
        if (schema && (schema->type != TOML_INTEGER || schema->number != 1)) loader_fail(&ld, schema, "schema must be 1");
        load_report(&ld, c, section(&ld, root, "report"));
        load_figure(&ld, c, section(&ld, root, "figure"));
        load_overview(&ld, c, section(&ld, root, "overview"));
        load_workloads(&ld, c, section(&ld, root, "workloads"));
        load_theme(&ld, c, section(&ld, root, "theme"));
        load_tools(&ld, c, array_of_tables(&ld, root, "tool"));
        load_workload_specs(&ld, c, array_of_tables(&ld, root, "workload"));
        load_groups(&ld, c, array_of_tables(&ld, root, "group"));
    }
    toml_free(root);
    if (ld.failed) return 0;
    free(c->path);
    c->path = xstrdup(path);
    return 1;
}

char *config_discover(const char *first_input) {
    char *dir = pf_kind(first_input) == PF_DIR ? xstrdup(first_input) : path_dir(first_input);
    for (int level = 0; level <= 4; level++) {
        char *candidate = path_join(dir, "isocost.toml");
        if (pf_kind(candidate) == PF_FILE) {
            free(dir);
            return candidate;
        }
        free(candidate);
        char *git = path_join(dir, ".git");
        int top = pf_kind(git) != PF_NONE || strcmp(dir, "/") == 0 || strcmp(dir, ".") == 0;
        free(git);
        if (top) break;
        char *parent = path_dir(dir);
        free(dir);
        dir = parent;
    }
    free(dir);
    return NULL;
}

/* ---- sizes ---------------------------------------------------------------- */

int figure_size(const char *spec, double dpi_override, FigureSize *out, char *error, size_t error_size) {
    static const struct { const char *name; double w, h; int unit; double dpi, base_pt; } presets[] = {
        {"screen", 1600, 1000, 0, 96, 10.5},
        {"paper", 89, 72, 1, 300, 7},
        {"paper-wide", 183, 100, 1, 300, 7},
        {"slide", 13.333, 7.5, 2, 144, 14},
    };
    double w = 0, h = 0, dpi, base_pt;
    int unit = -1;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++)
        if (strcmp(spec, presets[i].name) == 0) {
            w = presets[i].w;
            h = presets[i].h;
            unit = presets[i].unit;
            dpi = presets[i].dpi;
            base_pt = presets[i].base_pt;
            snprintf(out->name, sizeof(out->name), "%s", spec);
        }
    if (unit < 0) {
        char suffix[8] = "";
        if (sscanf(spec, "%lfx%lf%7s", &w, &h, suffix) != 3 || !(w > 0) || !(h > 0)) {
            snprintf(error, error_size, "\"%s\" is not a size; use screen, paper, paper-wide, slide, or WxH with px, mm, or in (\"120x80mm\")", spec);
            return 0;
        }
        if (strcmp(suffix, "px") == 0) unit = 0;
        else if (strcmp(suffix, "mm") == 0) unit = 1;
        else if (strcmp(suffix, "in") == 0) unit = 2;
        else {
            snprintf(error, error_size, "size unit \"%s\" is not px, mm, or in", suffix);
            return 0;
        }
        dpi = unit == 0 ? 96 : 300;
        base_pt = unit == 0 ? 10.5 : 7;
        out->custom = 1;
        snprintf(out->name, sizeof(out->name), "%s", spec);
    }
    out->print = unit != 0 && base_pt < 10;
    if (dpi_override > 0) dpi = dpi_override;
    double to_px = unit == 0 ? 1 : unit == 1 ? dpi / 25.4 : dpi;
    out->width = (int)lround(w * to_px);
    out->height = (int)lround(h * to_px);
    out->dpi = unit == 0 && dpi_override <= 0 ? 96 : dpi;
    /* Pixel sizes keep their pixel count; dpi then only scales type and marks. */
    out->base_px = base_pt * (unit == 0 ? (dpi_override > 0 ? dpi_override : 96) : dpi) / 72.0;
    out->width_pt = unit == 0 ? w * 72.0 / out->dpi : w * (unit == 1 ? 72.0 / 25.4 : 72.0);
    out->height_pt = unit == 0 ? h * 72.0 / out->dpi : h * (unit == 1 ? 72.0 / 25.4 : 72.0);
    if (out->width < 320 || out->height < 200 || out->width > 12000 || out->height > 12000) {
        snprintf(error, error_size, "\"%s\" at %g dpi is %dx%d pixels; the canvas must be between 320x200 and 12000x12000", spec, dpi, out->width, out->height);
        return 0;
    }
    return 1;
}

/* ---- lookups -------------------------------------------------------------- */

const ToolSpec *config_tool(const Config *c, const char *id, const char *family) {
    for (size_t i = 0; i < c->tool_count; i++)
        if (strcmp(c->tools[i].id, id) == 0) return &c->tools[i];
    if (family)
        for (size_t i = 0; i < c->tool_count; i++)
            if (strcmp(c->tools[i].id, family) == 0) return &c->tools[i];
    return NULL;
}

const WorkloadSpec *config_workload(const Config *c, const char *id) {
    for (size_t i = 0; i < c->workload_count; i++)
        if (strcmp(c->workloads[i].id, id) == 0) return &c->workloads[i];
    return NULL;
}

const GroupSpec *config_group(const Config *c, const char *group) {
    for (size_t i = 0; i < c->group_count; i++)
        if (glob_match(c->groups_spec[i].match, group)) return &c->groups_spec[i];
    return NULL;
}

/* ---- printing -------------------------------------------------------------- */

static void print_string(FILE *out, const char *key, const char *value, const char *comment) {
    fprintf(out, "%s = \"", key);
    for (const char *p = value; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        if (*p == '\n') fputs("\\n", out);
        else fputc(*p, out);
    }
    fprintf(out, "\"%s%s\n", comment ? "  # " : "", comment ? comment : "");
}

static void print_strings(FILE *out, const char *key, const Vec *v, const char *comment) {
    fprintf(out, "%s = [", key);
    for (size_t i = 0; i < v->count; i++) fprintf(out, "%s\"%s\"", i ? ", " : "", (char *)v->items[i]);
    fprintf(out, "]  # %s\n", comment);
}

static void print_flags(FILE *out, const char *key, unsigned flags, const char *const *names, const char *comment) {
    int first = 1;
    fprintf(out, "%s = [", key);
    for (int i = 0; names[i]; i++)
        if (flags & (1u << i)) {
            fprintf(out, "%s\"%s\"", first ? "" : ", ", names[i]);
            first = 0;
        }
    fprintf(out, "]  # %s\n", comment);
}

static void print_numbers(FILE *out, const char *key, const Numbers *n, const char *comment) {
    fprintf(out, "%s = [", key);
    for (size_t i = 0; i < n->count; i++) fprintf(out, "%s%g", i ? ", " : "", n->values[i]);
    fprintf(out, "]  # %s\n", comment);
}

static void print_color(FILE *out, const char *key, Rgb c, const char *comment) {
    fprintf(out, "%s = \"#%02x%02x%02x\"  # %s\n", key, c.r, c.g, c.b, comment);
}

void config_print(const Config *c, FILE *out) {
    static const char *const views[] = {"overview", "workloads", NULL};
    static const char *const formats[] = {"png", "svg", "pdf", NULL};
    static const char *const metrics[] = {"time", "memory", NULL};
    fprintf(out, "# isocost.toml\n# Every key is optional. Values shown are %s.\n# Command-line options override this file.\n\nschema = 1\n\n",
            c->path ? "the resolved configuration" : "the built-in defaults");

    fputs("[report]\n", out);
    print_string(out, "title", c->report_title, "first heading of report.md");
    print_string(out, "baseline", c->baseline, "reference tool id or family; empty = automatic");
    fprintf(out, "statistic = \"%s\"  # Zebrac estimate per result: median or mean\n", STATISTICS[c->statistic]);
    fprintf(out, "cost = \"%s\"  # equal-cost curves: geometric (time^w * memory^w) or linear\n", COSTS[c->cost_model]);
    fprintf(out, "time_weight = %g  # weights are normalized to sum to 1\n", c->time_weight);
    fprintf(out, "memory_weight = %g\n", c->memory_weight);
    fprintf(out, "cases = \"%s\"  # all, or common: only workloads every tool ran\n", CASES[c->common_cases]);
    fprintf(out, "runs = \"%s\"  # latest run per group, or all runs\n", RUNS[c->all_runs]);
    print_strings(out, "groups", &c->groups, "draw figures whose group matches one of these globs");
    print_strings(out, "suites", &c->suites, "and whose suite matches one of these");
    print_flags(out, "views", c->views, views, "overview, workloads");
    print_flags(out, "formats", c->formats, formats, "png, svg, pdf");
    fprintf(out, "data = %s  # also write points.tsv\n\n", c->data ? "true" : "false");

    fputs("[figure]\n", out);
    print_string(out, "size", c->size, "screen, paper, paper-wide, slide, or WxH with px, mm, or in");
    fprintf(out, "dpi = %g  # 0 = size default (screen 96, paper 300, slide 144)\n", c->dpi);
    fprintf(out, "font_scale = %g\n", c->font_scale);
    fprintf(out, "header = \"%s\"  # auto: title and subtitle on screen and slide sizes, none on print sizes (use a caption); show; hide\n", HEADERS[c->header]);
    print_string(out, "title", c->title, "placeholders: {project} {suite} {group} {run} {reference} {workloads}");
    print_string(out, "subtitle", c->subtitle, "\"auto\", \"\" for none, or a template");
    fprintf(out, "legend = \"%s\"  # top, bottom, right, none\n", LEGENDS[c->legend]);
    print_string(out, "note", c->note, "\"auto\" explains the marks on screen and slide sizes, \"\" for none, or your own text");
    fputc('\n', out);

    fputs("[overview]\n", out);
    print_string(out, "x_label", c->x_label, NULL);
    print_string(out, "y_label", c->y_label, NULL);
    fprintf(out, "x_scale = \"%s\"  # auto (log when the data spans more than 6x), log, linear\n", SCALES[c->x_scale]);
    fprintf(out, "y_scale = \"%s\"\n", SCALES[c->y_scale]);
    fprintf(out, "x_min = %g  # 0 = automatic, for all four limits\nx_max = %g\ny_min = %g\ny_max = %g\n", c->x_min, c->x_max, c->y_min, c->y_max);
    print_numbers(out, "x_ticks", &c->x_ticks, "empty = automatic");
    print_numbers(out, "y_ticks", &c->y_ticks, "empty = automatic");
    fprintf(out, "contours = %s  # equal-cost curves\n", c->contours ? "true" : "false");
    print_numbers(out, "contour_levels", &c->contour_levels, "empty = automatic");
    fprintf(out, "case_points = %s  # one faint mark per workload\n", c->case_points ? "true" : "false");
    fprintf(out, "spread = \"%s\"  # iqr, minmax, none\n", SPREADS[c->spread]);
    fprintf(out, "ellipse = %s  # drawn for tools with 5 or more workloads\n", c->ellipse ? "true" : "false");
    fprintf(out, "pareto = %s  # line through tools no other tool beats on both axes\n", c->pareto ? "true" : "false");
    fprintf(out, "highlight = %s  # tint the faster-and-smaller region\n", c->highlight ? "true" : "false");
    fprintf(out, "reference_lines = %s\n", c->reference_lines ? "true" : "false");
    fprintf(out, "labels = \"%s\"  # auto or none\n\n", LABEL_MODES[c->labels]);

    fputs("[workloads]\n", out);
    print_flags(out, "metrics", (unsigned)(c->workloads_time | (c->workloads_memory << 1)), metrics, "one panel per metric: time, memory");
    fprintf(out, "sort = \"%s\"  # name, time, memory, config\n", SORTS[c->workloads_sort]);
    fprintf(out, "scale = \"%s\"  # log or linear\n", c->workloads_scale == SCALE_LINEAR ? "linear" : "log");
    fprintf(out, "labels = %s\n\n", c->workloads_labels ? "true" : "false");

    fputs("[theme]\n", out);
    print_color(out, "background", c->background, "figure background");
    print_color(out, "ink", c->ink, "titles and labels");
    print_color(out, "ink_secondary", c->ink_secondary, "axis text");
    print_color(out, "muted", c->muted, "notes and contour labels");
    print_color(out, "grid", c->grid, "gridlines");
    print_color(out, "reference", c->reference, "lines through 1x");
    print_color(out, "contour", c->contour, "equal-cost curves");
    print_color(out, "region", c->region, "faster-and-smaller tint");
    fputs("palette = [", out);
    for (int i = 0; i < c->palette_count; i++) fprintf(out, "%s\"#%02x%02x%02x\"", i ? ", " : "", c->palette[i].r, c->palette[i].g, c->palette[i].b);
    fputs("]\n\n", out);

    if (!c->tool_count)
        fputs("# [[tool]]\n# id = \"zstd -3\"          # required\n# match = \"zstd -3 *\"     # optional glob on the command; names matching results\n"
              "# label = \"zstd level 3\"\n# color = \"#2a78d6\"\n# shape = \"circle\"         # circle, square, triangle, diamond, triangle-down, plus, star, cross\n"
              "# fill = \"solid\"           # solid or hollow\n# line = \"solid\"           # solid, dashed, dotted\n# order = 1\n# hide = false\n"
              "# label_position = \"auto\"  # auto, left, right, above, below, none\n\n", out);
    for (size_t i = 0; i < c->tool_count; i++) {
        const ToolSpec *s = &c->tools[i];
        fputs("[[tool]]\n", out);
        print_string(out, "id", s->id, NULL);
        if (s->match) print_string(out, "match", s->match, NULL);
        if (s->label) print_string(out, "label", s->label, NULL);
        if (s->has_color) fprintf(out, "color = \"#%02x%02x%02x\"\n", s->color.r, s->color.g, s->color.b);
        if (s->shape != SHAPE_AUTO) fprintf(out, "shape = \"%s\"\n", SHAPES[s->shape]);
        if (s->fill != FILL_AUTO) fprintf(out, "fill = \"%s\"\n", FILLS[s->fill]);
        if (s->line) fprintf(out, "line = \"%s\"\n", LINES[s->line]);
        if (s->order) fprintf(out, "order = %d\n", s->order);
        if (s->hide) fputs("hide = true\n", out);
        if (s->label_position) fprintf(out, "label_position = \"%s\"\n", LABEL_POSITIONS[s->label_position]);
        fputc('\n', out);
    }
    if (!c->workload_count) fputs("# [[workload]]\n# id = \"source\"\n# label = \"C headers\"\n# order = 1\n# hide = false\n\n", out);
    for (size_t i = 0; i < c->workload_count; i++) {
        const WorkloadSpec *s = &c->workloads[i];
        fputs("[[workload]]\n", out);
        print_string(out, "id", s->id, NULL);
        if (s->label) print_string(out, "label", s->label, NULL);
        if (s->order) fprintf(out, "order = %d\n", s->order);
        if (s->hide) fputs("hide = true\n", out);
        fputc('\n', out);
    }
    if (!c->group_count) fputs("# [[group]]\n# match = \"decompress\"     # glob on the figure group\n# title = \"Decompression\"\n# subtitle = \"\"\n# baseline = \"gzip -6\"\n# hide = false\n", out);
    for (size_t i = 0; i < c->group_count; i++) {
        const GroupSpec *s = &c->groups_spec[i];
        fputs("[[group]]\n", out);
        print_string(out, "match", s->match, NULL);
        if (s->title) print_string(out, "title", s->title, NULL);
        if (s->subtitle) print_string(out, "subtitle", s->subtitle, NULL);
        if (s->baseline) print_string(out, "baseline", s->baseline, NULL);
        if (s->hide) fputs("hide = true\n", out);
        fputc('\n', out);
    }
}
